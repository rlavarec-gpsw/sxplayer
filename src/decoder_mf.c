// This file is part of sxplayer.
//
// Copyright (c) 2023 GoPro, Inc. All rights reserved.
// 
// THIS SOURCE CODE IS THE PROPRIETARY INTELLECTUAL PROPERTY AND CONFIDENTIAL
// INFORMATION OF GOPRO, INC. AND IS PROTECTED UNDER U.S. AND INTERNATIONAL
// LAW. ANY USE OF THIS SOURCE CODE WITHOUT THE PRIOR WRITTEN AUTHORIZATION OF
// GOPRO IS STRICTLY PROHIBITED.


// temporary lib references for build, 
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

#define COBJMACROS

#include <string.h>
#include <Windows.h>
#include <initguid.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <libavcodec/mf_utils.h>
#include <libavcodec/hwconfig.h>
#include <libavutil/hwcontext.h>
#include <libavcodec/mpeg4audio.h>
#include <libavcodec/bytestream.h>
#include <libavcodec/decode.h>
#include <libavcodec/bsf.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libavcodec/internal.h>
#include <libavutil/hwcontext_internal.h>
#include <libavutil/hwcontext_dxva2.h>
#include <libavutil/thread.h>

#include "hwcontext_mf.h"
#include "mod_decoding.h"
#include "decoders.h"
#include "log.h"

extern IMFSample* av_ff_create_memory_sample(void* fill_data, size_t size, size_t align);
extern const CLSID* av_ff_codec_to_mf_subtype(enum AVCodecID codec);
#define av_ff_hr_str(hr) av_ff_hr_str_buf((char[80]){0}, 80, hr)

typedef struct MFDecoderContext {
    AVClass *av_class;
    int is_video, is_audio;
    GUID main_subtype;
    IMFTransform *mft;
    IMFMediaEventGenerator *async_events;
    DWORD in_stream_id, out_stream_id;
    MFT_INPUT_STREAM_INFO in_info;
    MFT_OUTPUT_STREAM_INFO out_info;
    int out_stream_provides_samples;
    int draining, draining_done;
    int sample_sent;
    int async_need_input, async_have_output, async_marker;
    uint8_t *send_extradata;
    int send_extradata_size;
    ICodecAPI *codec_api;
    AVBSFContext *bsfc;
    int sw_format;
    AVBufferRef *frames_ref; // really AVHWFramesContext*
    AVBufferRef *decoder_ref; // really MFDecoder*
    AVFrame *tmp_frame;
    // Important parameters which might be overwritten by decoding.
    int original_channels;
    // set by AVOption
    int opt_use_d3d;
    int opt_require_d3d;
    int opt_out_samples;
    int opt_d3d_bind_flags;
} MFDecoderContext;

// MP42 FourCC. Pawel Wegner (with WM4) created the symbol name.
DEFINE_GUID(ff_MFVideoFormat_MP42, 0x3234504D, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

// RFJ (from Pawel Wegner path)
// Extend ffmpeg codec mapping for 
//  case AV_CODEC_ID_MSMPEG4V2:  return &ff_MFVideoFormat_MP42;

#pragma region "ADAPTED_FROM_PAWELWEGNER"
static const CLSID* sx_extended_codec_to_mf_subtype(enum AVCodecID id)
{
    const CLSID* subtype = av_ff_codec_to_mf_subtype(id);

    if (!subtype && id == AV_CODEC_ID_MSMPEG4V2)
        subtype = &ff_MFVideoFormat_MP42;

    return subtype;
}

#define MF_TIMEBASE (AVRational){1, 10000000}
// Sentinel value only used by us.
#define MF_INVALID_TIME AV_NOPTS_VALUE

static int mf_wait_events(AVCodecContext *avctx)
{
    MFDecoderContext *c = avctx->priv_data;

    if (!c->async_events)
        return 0;

    while (!(c->async_need_input || c->async_have_output || c->draining_done || c->async_marker)) {
        IMFMediaEvent *ev = NULL;
        MediaEventType ev_id = 0;
        HRESULT hr = IMFMediaEventGenerator_GetEvent(c->async_events, 0, &ev);
        
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "IMFMediaEventGenerator_GetEvent() failed: %s\n",
                   av_ff_hr_str(hr));
            return AVERROR_EXTERNAL;
        }
        
        IMFMediaEvent_GetType(ev, &ev_id);
        switch (ev_id) {
        case ff_METransformNeedInput:
            if (!c->draining)
                c->async_need_input = 1;
            break;
        case ff_METransformHaveOutput:
            c->async_have_output = 1;
            break;
        case ff_METransformDrainComplete:
            c->draining_done = 1;
            break;
        case ff_METransformMarker:
            c->async_marker = 1;
            break;
        default: ;
        }
        IMFMediaEvent_Release(ev);
    }

    return 0;
}

static AVRational mf_get_timebase(AVCodecContext *avctx)
{
    if (avctx->pkt_timebase.num > 0 && avctx->pkt_timebase.den > 0)
        return avctx->pkt_timebase;
    if (avctx->time_base.num > 0 && avctx->time_base.den > 0)
        return avctx->time_base;

    return MF_TIMEBASE;
}

static LONGLONG mf_to_mf_time(AVCodecContext *avctx, int64_t av_pts)
{
    if (av_pts == AV_NOPTS_VALUE)
        return MF_INVALID_TIME;

    return av_rescale_q(av_pts, mf_get_timebase(avctx), MF_TIMEBASE);
}

static void mf_sample_set_pts(AVCodecContext *avctx, IMFSample *sample, int64_t av_pts)
{
    LONGLONG stime = mf_to_mf_time(avctx, av_pts);

    if (stime != MF_INVALID_TIME)
        IMFSample_SetSampleTime(sample, stime);
}

static int64_t mf_from_mf_time(AVCodecContext *avctx, LONGLONG stime)
{
    return av_rescale_q(stime, MF_TIMEBASE, mf_get_timebase(avctx));
}

static int64_t mf_sample_get_pts(AVCodecContext *avctx, IMFSample *sample)
{
    LONGLONG pts;
    HRESULT hr = IMFSample_GetSampleTime(sample, &pts);

    if (FAILED(hr))
        return AV_NOPTS_VALUE;

    return mf_from_mf_time(avctx, pts);
}

static IMFSample *mf_avpacket_to_sample(AVCodecContext *avctx, const AVPacket *avpkt)
{
    MFDecoderContext *c = avctx->priv_data;
    IMFSample *sample = NULL;
    AVPacket tmp = {0};
    int ret;

    if ((ret = av_packet_ref(&tmp, avpkt)) >= 0) {
        if (c->bsfc) {
            AVPacket tmp2 = {0};
            
            if ((ret = av_bsf_send_packet(c->bsfc, &tmp)) < 0)
                goto done;
            if ((ret = av_bsf_receive_packet(c->bsfc, &tmp)) < 0)
                goto done;
            
            // We don't support any 1:m BSF filtering - but at least don't get stuck.
            while ((ret = av_bsf_receive_packet(c->bsfc, &tmp2)) >= 0)
                av_log(avctx, AV_LOG_ERROR, "Discarding unsupported sub-packet.\n");

            av_packet_unref(&tmp2);
        }

        if (tmp.data) {
            sample = av_ff_create_memory_sample(tmp.data, tmp.size, c->in_info.cbAlignment);
            if (sample) {
                int64_t pts = (avpkt->pts != AV_NOPTS_VALUE) ? avpkt->pts : avpkt->dts;

                if (pts != AV_NOPTS_VALUE) {
                    LONGLONG stime = av_rescale_q(pts, mf_get_timebase(avctx), MF_TIMEBASE);
                    IMFSample_SetSampleTime(sample, stime);
                }
                if (avpkt->flags & AV_PKT_FLAG_KEY)
                    IMFSample_SetUINT32(sample, &MFSampleExtension_CleanPoint, TRUE);
            }
        }
    }

done:
    av_packet_unref(&tmp);
    return sample;
}

static int mf_deca_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    UINT32 t;
    HRESULT hr;

    hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    avctx->channels = t;
    avctx->channel_layout = av_get_default_channel_layout(t);

    hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_CHANNEL_MASK, &t);
    if (!FAILED(hr))
        avctx->channel_layout = t;

    hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    avctx->sample_rate = t;

    avctx->sample_fmt = av_ff_media_type_to_sample_fmt((IMFAttributes *)type);

    if (avctx->sample_fmt == AV_SAMPLE_FMT_NONE || !avctx->channels)
        return AVERROR_EXTERNAL;

    return 0;
}

static int mf_decv_output_type_get(AVCodecContext *avctx, IMFMediaType *type)
{
    MFDecoderContext *c = avctx->priv_data;
    AVHWFramesContext *frames_context;
    HRESULT hr;
    UINT32 w, h, cw, ch, t, t2;
    MFVideoArea area = {0};
    int ret;

    c->sw_format = av_ff_media_type_to_pix_fmt((IMFAttributes *)type);
    avctx->pix_fmt = c->sw_format;

    int res = av_ff_get_mf_attributes((IMFAttributes *)type, &MF_MT_FRAME_SIZE, &cw, &ch);
    if (res != 0)
        return res;

    // Cropping rectangle. Ignore the fractional offset, because nobody uses that anyway.
    // (libavcodec native decoders still try to crop away mod-2 offset pixels by
    // adjusting the pixel plane pointers.)
    hr = IMFMediaType_GetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (void *)&area, sizeof(area), NULL);
    if (FAILED(hr)) {
        w = cw;
        h = ch;
    } else {
        w = area.OffsetX.value + area.Area.cx;
        h = area.OffsetY.value + area.Area.cy;
    }

    if (w > cw || h > ch)
        return AVERROR_EXTERNAL;

    res = av_ff_get_mf_attributes((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO, &t, &t2);
    if (res != 0)
        return res;

    avctx->sample_aspect_ratio.num = t;
    avctx->sample_aspect_ratio.den = t2;

    hr = IMFMediaType_GetUINT32(type, &MF_MT_YUV_MATRIX, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoTransferMatrix_BT709:       avctx->colorspace = AVCOL_SPC_BT709; break;
        case MFVideoTransferMatrix_BT601:       avctx->colorspace = AVCOL_SPC_BT470BG; break;
        case MFVideoTransferMatrix_SMPTE240M:   avctx->colorspace = AVCOL_SPC_SMPTE240M; break;
        }
    }

    hr = IMFMediaType_GetUINT32(type, &MF_MT_VIDEO_PRIMARIES, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoPrimaries_BT709:            avctx->color_primaries = AVCOL_PRI_BT709; break;
        case MFVideoPrimaries_BT470_2_SysM:     avctx->color_primaries = AVCOL_PRI_BT470M; break;
        case MFVideoPrimaries_BT470_2_SysBG:    avctx->color_primaries = AVCOL_PRI_BT470BG; break;
        case MFVideoPrimaries_SMPTE170M:        avctx->color_primaries = AVCOL_PRI_SMPTE170M; break;
        case MFVideoPrimaries_SMPTE240M:        avctx->color_primaries = AVCOL_PRI_SMPTE240M; break;
        }
    }

    hr = IMFMediaType_GetUINT32(type, &MF_MT_TRANSFER_FUNCTION, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoTransFunc_10:               avctx->color_trc = AVCOL_TRC_LINEAR; break;
        case MFVideoTransFunc_22:               avctx->color_trc = AVCOL_TRC_GAMMA22; break;
        case MFVideoTransFunc_709:              avctx->color_trc = AVCOL_TRC_BT709; break;
        case MFVideoTransFunc_240M:             avctx->color_trc = AVCOL_TRC_SMPTE240M; break;
        case MFVideoTransFunc_sRGB:             avctx->color_trc = AVCOL_TRC_IEC61966_2_1; break;
        case MFVideoTransFunc_28:               avctx->color_trc = AVCOL_TRC_GAMMA28; break;
        // mingw doesn't define these yet
        //case MFVideoTransFunc_Log_100:          avctx->color_trc = AVCOL_TRC_LOG; break;
        //case MFVideoTransFunc_Log_316:          avctx->color_trc = AVCOL_TRC_LOG_SQRT; break;
        }
    }

    hr = IMFMediaType_GetUINT32(type, &MF_MT_VIDEO_CHROMA_SITING, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFVideoChromaSubsampling_MPEG2:    avctx->chroma_sample_location = AVCHROMA_LOC_LEFT; break;
        case MFVideoChromaSubsampling_MPEG1:    avctx->chroma_sample_location = AVCHROMA_LOC_CENTER; break;
        }
    }

    hr = IMFMediaType_GetUINT32(type, &MF_MT_VIDEO_NOMINAL_RANGE, &t);
    if (!FAILED(hr)) {
        switch (t) {
        case MFNominalRange_0_255:              avctx->color_range = AVCOL_RANGE_JPEG; break;
        case MFNominalRange_16_235:             avctx->color_range = AVCOL_RANGE_MPEG; break;
        }
    }

    if ((ret = av_ff_set_dimensions(avctx, cw, ch)) < 0)
        return ret;

    avctx->width = w;
    avctx->height = h;

    return ret;
}

static int mf_output_type_get(AVCodecContext *avctx)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    IMFMediaType *type;
    int ret;

    hr = IMFTransform_GetOutputCurrentType(c->mft, c->out_stream_id, &type);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not get output type\n");
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_VERBOSE, "final output type:\n");
    av_ff_attributes_dump(avctx, (IMFAttributes *)type);

    ret = 0;
    if (c->is_video) {
        ret = mf_decv_output_type_get(avctx, type);
    } else if (c->is_audio) {
        ret = mf_deca_output_type_get(avctx, type);
    }

    if (ret < 0)
        av_log(avctx, AV_LOG_ERROR, "output type not supported\n");

    IMFMediaType_Release(type);
    return ret;
}

static int mf_deca_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFDecoderContext *c = avctx->priv_data;

    int sample_rate = avctx->sample_rate;
    int channels = avctx->channels;

    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &c->main_subtype);

    if (avctx->codec_id == AV_CODEC_ID_AAC) {
        int assume_adts = avctx->extradata_size == 0;
        // The first 12 bytes are the remainder of HEAACWAVEINFO.
        // Fortunately all fields can be left 0.
        size_t ed_size = 12 + (size_t)avctx->extradata_size;
        uint8_t *ed = av_mallocz(ed_size);
        if (!ed)
            return AVERROR(ENOMEM);
        if (assume_adts)
            ed[0] = 1; // wPayloadType=1 (ADTS)
        if (avctx->extradata_size) {
            MPEG4AudioConfig c = {0};
            memcpy(ed + 12, avctx->extradata, avctx->extradata_size);
            if (avpriv_mpeg4audio_get_config2(&c, avctx->extradata, avctx->extradata_size * 8, 0, avctx) >= 0) {
                if (c.channels > 0)
                    channels = c.channels;
                sample_rate = c.sample_rate;
            }
        }
        IMFMediaType_SetBlob(type, &MF_MT_USER_DATA, ed, ed_size);
        av_free(ed);
        IMFMediaType_SetUINT32(type, &MF_MT_AAC_PAYLOAD_TYPE, assume_adts ? 1 : 0);
    } else if (avctx->extradata_size) {
        IMFMediaType_SetBlob(type, &MF_MT_USER_DATA, avctx->extradata, avctx->extradata_size);
    }

    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, channels);

    // WAVEFORMATEX stuff; might be required by some codecs.
    if (avctx->block_align)
        IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, avctx->block_align);
    if (avctx->bit_rate)
        IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avctx->bit_rate / 8);
    if (avctx->bits_per_coded_sample)
        IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, avctx->bits_per_coded_sample);

    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_PREFER_WAVEFORMATEX, 1);

    return 0;
}

static int64_t mf_decv_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFDecoderContext *c = avctx->priv_data;
    uint32_t fourcc;
    GUID tg;
    HRESULT hr;
    int score = -1;

    hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;

        // For the MPEG-4 decoder (selects MPEG-4 variant via FourCC).
        if (av_ff_fourcc_from_guid(&tg, &fourcc) >= 0 && fourcc == avctx->codec_tag)
            score = 2;
    }

    enum AVPixelFormat pix_fmt = av_ff_media_type_to_pix_fmt((IMFAttributes*)type);

    if (pix_fmt == AV_PIX_FMT_NONE)
        return -1;
    if (pix_fmt == AV_PIX_FMT_YUYV422)
        return 5;
    if (pix_fmt == AV_PIX_FMT_YUVJ420P)
        return 4;
    if (pix_fmt == AV_PIX_FMT_YUV420P)
        return 3;
    if (pix_fmt == AV_PIX_FMT_P010)
        return 2;
    if (pix_fmt == AV_PIX_FMT_NV12)
        return 1;

    return score;
}

static int mf_decv_input_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    int use_extradata = avctx->extradata_size && !c->bsfc;

    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);

    hr = IMFMediaType_GetItem(type, &MF_MT_SUBTYPE, NULL);
    if (FAILED(hr))
        IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &c->main_subtype);

    av_ff_set_mf_attributes((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);

    IMFMediaType_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);

    if (avctx->sample_aspect_ratio.num)
        av_ff_set_mf_attributes((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO,
                               avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);

    if (avctx->bit_rate)
        IMFMediaType_SetUINT32(type, &MF_MT_AVG_BITRATE, avctx->bit_rate);

    if (IsEqualGUID(&c->main_subtype, &MFVideoFormat_MP4V) ||
        IsEqualGUID(&c->main_subtype, &MFVideoFormat_MP43) ||
        IsEqualGUID(&c->main_subtype, &ff_MFVideoFormat_MP42)) {
        if (avctx->extradata_size < 3 ||
            avctx->extradata[0] || avctx->extradata[1] ||
            avctx->extradata[2] != 1)
            use_extradata = 0;
    }

    if (use_extradata)
        IMFMediaType_SetBlob(type, &MF_MT_USER_DATA, avctx->extradata, avctx->extradata_size);

    return 0;
}

static int64_t mf_deca_input_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    GUID tg;
    int score = -1;

    hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &tg);
    if (!FAILED(hr)) {
        if (IsEqualGUID(&c->main_subtype, &tg))
            score = 1;
    }

    return score;
}

// Sort the types by preference:
// - float sample format (highest)
// - sample depth
// - channel count
// - sample rate (lowest)
// Assume missing information means any is allowed.
static int64_t mf_deca_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    UINT32 t;
    int sample_fmt;
    int64_t score = 0;

    hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &t);
    if (!FAILED(hr))
        score |= t;

    // MF doesn't seem to tell us the native channel count. Try to get the
    // same number of channels by looking at the input codec parameters.
    // (With some luck they are correct, or even come from a parser.)
    // Prefer equal or larger channel count.
    hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &t);
    if (!FAILED(hr)) {
        int channels = av_get_channel_layout_nb_channels(avctx->request_channel_layout);
        int64_t ch_score = 0;
        int diff;
        if (channels < 1)
            channels = c->original_channels;
        diff = (int)t - channels;
        if (diff >= 0) {
            ch_score |= (1LL << 7) - diff;
        } else {
            ch_score |= (1LL << 6) + diff;
        }
        score |= ch_score << 20;
    }

    sample_fmt = av_ff_media_type_to_sample_fmt((IMFAttributes *)type);
    if (sample_fmt == AV_SAMPLE_FMT_NONE) {
        score = -1;
    } else {
        score |= av_get_bytes_per_sample(sample_fmt) << 28;
        if (sample_fmt == AV_SAMPLE_FMT_FLT)
            score |= 1LL << 32;
    }

    return score;
}

static int mf_deca_output_adjust(AVCodecContext *avctx, IMFMediaType *type)
{
    int block_align;
    HRESULT hr;

    // Some decoders (wmapro) do not list any output types. I have no clue
    // what we're supposed to do, and this is surely a MFT bug. Setting an
    // arbitrary output type helps.
    hr = IMFMediaType_GetItem(type, &MF_MT_MAJOR_TYPE, NULL);
    if (!FAILED(hr))
        return 0;

    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);

    block_align = 4;
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 32);

    block_align *= avctx->channels;
    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, avctx->channels);

    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);

    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, avctx->sample_rate);

    IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_align * avctx->sample_rate);

    return 0;
}

// Temporary format scoring code.  Should be replaced with a more reliable
// "best format" solution.
static int64_t mf_decv_output_score(AVCodecContext *avctx, IMFMediaType *type)
{
    enum AVPixelFormat pix_fmt = av_ff_media_type_to_pix_fmt((IMFAttributes *)type);
    if (pix_fmt == AV_PIX_FMT_NONE)
        return -1;
    if (avctx->codec_id != AV_CODEC_ID_HEVC)
    {
        if (pix_fmt == AV_PIX_FMT_YUYV422)
            return 5;
        if (pix_fmt == AV_PIX_FMT_YUVJ420P)
            return 4;
        if (pix_fmt == AV_PIX_FMT_YUV420P)
            return 3;
        if (pix_fmt == AV_PIX_FMT_P010)
            return 2;
        if (pix_fmt == AV_PIX_FMT_NV12)
            return 1;
    }
    else {
        if (pix_fmt == AV_PIX_FMT_YUYV422)
            return 5;
        if (pix_fmt == AV_PIX_FMT_YUVJ420P)
            return 6;
        if (pix_fmt == AV_PIX_FMT_YUV420P)
            return 3;
        if (pix_fmt == AV_PIX_FMT_P010)
            return 2;
        if (pix_fmt == AV_PIX_FMT_NV12)
            return 10;
    }
    return 0;
}

static int mf_choose_output_type(AVCodecContext *avctx)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;
    IMFMediaType *out_type = NULL;
    int64_t out_type_score = -1;
    int out_type_index = -1;
    int n;

    av_log(avctx, AV_LOG_VERBOSE, "output types:\n");

    for (n = 0; ; n++) {
        IMFMediaType *type;
        int64_t score = -1;

        hr = IMFTransform_GetOutputAvailableType(c->mft, c->out_stream_id, n, &type);
        if (hr == MF_E_NO_MORE_TYPES || hr == E_NOTIMPL)
            break;

        if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "(need to set input type)\n");
            ret = 0;
            goto done;
        }

        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "error getting output type: %s\n", av_ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
            goto done;
        }

        av_log(avctx, AV_LOG_VERBOSE, "output type %d:\n", n);
        av_ff_attributes_dump(avctx, (IMFAttributes *)type);

        if (c->is_video) {
            score = mf_decv_output_score(avctx, type);
        } else if (c->is_audio) {
            score = mf_deca_output_score(avctx, type);
        }

        if (score > out_type_score) {
            if (out_type)
                IMFMediaType_Release(out_type);
            out_type = type;
            out_type_score = score;
            out_type_index = n;
            IMFMediaType_AddRef(out_type);
        }

        IMFMediaType_Release(type);
    }

    if (out_type) {
        av_log(avctx, AV_LOG_VERBOSE, "picking output type %d.\n", out_type_index);
    } else {
        hr = MFCreateMediaType(&out_type);
        if (FAILED(hr)) {
            ret = AVERROR(ENOMEM);
            goto done;
        }
    }

    ret = 0;
    if (c->is_audio) {
        ret = mf_deca_output_adjust(avctx, out_type);
    }

    if (ret >= 0) {
        av_log(avctx, AV_LOG_VERBOSE, "setting output type:\n");
        av_ff_attributes_dump(avctx, (IMFAttributes *)out_type);

        hr = IMFTransform_SetOutputType(c->mft, c->out_stream_id, out_type, 0);
        if (!FAILED(hr)) {
            ret = 1;
        } else if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "rejected - need to set input type\n");
            ret = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "could not set output type (%s)\n", av_ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
        }
    }

done:
    if (out_type)
        IMFMediaType_Release(out_type);
    return ret;
}

static int mf_choose_input_type(AVCodecContext *avctx)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    int ret;
    IMFMediaType *in_type = NULL;
    int64_t in_type_score = -1;
    int in_type_index = -1;
    int n;

    av_log(avctx, AV_LOG_VERBOSE, "input types:\n");
    for (n = 0; ; n++) {
        IMFMediaType *type = NULL;
        int64_t score = -1;

        hr = IMFTransform_GetInputAvailableType(c->mft, c->in_stream_id, n, &type);
        if (hr == MF_E_NO_MORE_TYPES || hr == E_NOTIMPL)
            break;

        if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "(need to set output type 1)\n");
            ret = 0;
            goto done;
        }
        if (FAILED(hr)) {
            av_log(avctx, AV_LOG_ERROR, "error getting input type: %s\n", av_ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
            goto done;
        }

        av_log(avctx, AV_LOG_VERBOSE, "input type %d:\n", n);
        av_ff_attributes_dump(avctx, (IMFAttributes *)type);

        if (c->is_video) {
            score = mf_decv_input_score(avctx, type);
        } else if (c->is_audio) {
            score = mf_deca_input_score(avctx, type);
        }

        if (score > in_type_score) {
            if (in_type)
                IMFMediaType_Release(in_type);
            in_type = type;
            in_type_score = score;
            in_type_index = n;
            IMFMediaType_AddRef(in_type);
        }

        IMFMediaType_Release(type);
    }

    if (in_type) {
        av_log(avctx, AV_LOG_VERBOSE, "picking input type %d.\n", in_type_index);
    } else {
        hr = MFCreateMediaType(&in_type);
        if (FAILED(hr)) {
            ret = AVERROR(ENOMEM);
            goto done;
        }
    }

    ret = 0;
    if (c->is_video) {
        ret = mf_decv_input_adjust(avctx, in_type);
    } else if (c->is_audio) {
        ret = mf_deca_input_adjust(avctx, in_type);
    }

    if (ret >= 0) {
        av_log(avctx, AV_LOG_VERBOSE, "setting input type:\n");
        av_ff_attributes_dump(avctx, (IMFAttributes *)in_type);

        hr = IMFTransform_SetInputType(c->mft, c->in_stream_id, in_type, 0);
        if (!FAILED(hr)) {
            ret = 1;
        } else if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "rejected - need to set output type\n");
            ret = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "could not set input type (%s)\n", av_ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
        }
    }

done:
    if (in_type)
        IMFMediaType_Release(in_type);
    return ret;
}

static int mf_negotiate_types(AVCodecContext *avctx)
{
    // This follows steps 1-5 on:
    //  https://msdn.microsoft.com/en-us/library/windows/desktop/aa965264(v=vs.85).aspx
    // If every MFT implementer does this correctly, this loop should at worst
    // be repeated once.
    int need_input = 1, need_output = 1;
    int n;

    for (n = 0; n < 2 && (need_input || need_output); n++) {
        int ret;
        ret = mf_choose_input_type(avctx);
        if (ret < 0)
            return ret;
        need_input = ret < 1;
        ret = mf_choose_output_type(avctx);
        if (ret < 0)
            return ret;
        need_output = ret < 1;
    }

    if (need_input || need_output) {
        av_log(avctx, AV_LOG_ERROR, "format negotiation failed (%d/%d)\n",
               need_input, need_output);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static LONG mf_codecapi_get_int(ICodecAPI *capi, const GUID *guid, LONG def)
{
    LONG ret = def;
    VARIANT v;
    HRESULT hr = ICodecAPI_GetValue(capi, &ff_CODECAPI_AVDecVideoMaxCodedWidth, &v);
    if (FAILED(hr))
        return ret;
    if (v.vt == VT_I4)
        ret = v.lVal;
    if (v.vt == VT_UI4)
        ret = v.ulVal;
    VariantClear(&v);
    return ret;
}

static int mf_check_codec_requirements(AVCodecContext *avctx)
{
    MFDecoderContext *c = avctx->priv_data;

    if (c->is_video && c->codec_api) {
        LONG w = mf_codecapi_get_int(c->codec_api, &ff_CODECAPI_AVDecVideoMaxCodedWidth, 0);
        LONG h = mf_codecapi_get_int(c->codec_api, &ff_CODECAPI_AVDecVideoMaxCodedHeight, 0);

        if (w <= 0 || h <= 0)
            return 0;

        av_log(avctx, AV_LOG_VERBOSE, "Max. supported video size: %dx%d\n", (int)w, (int)h);

        // avctx generally has only the cropped size. Assume the coded size is
        // the same size, rounded up to the next macroblock boundary.
        if (avctx->width > w || avctx->height > h) {
            av_log(avctx, AV_LOG_ERROR, "Video size %dx%d larger than supported size.\n",
                   avctx->width, avctx->height);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int mf_unlock_async(AVCodecContext *avctx)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    IMFAttributes *attrs;
    UINT32 v;
    int res = AVERROR_EXTERNAL;

    hr = IMFTransform_GetAttributes(c->mft, &attrs);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "error retrieving MFT attributes: %s\n", av_ff_hr_str(hr));
        goto err;
    }

    hr = IMFAttributes_GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &v);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "error querying async: %s\n", av_ff_hr_str(hr));
        goto err;
    }

    if (avctx->hw_device_ctx != NULL &&  !v) {
        av_log(avctx, AV_LOG_ERROR, "hardware MFT is not async\n");
        goto err;
    }

    hr = IMFAttributes_SetUINT32(attrs, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not set async unlock: %s\n", av_ff_hr_str(hr));
        goto err;
    }

    hr = IMFTransform_QueryInterface(c->mft, &IID_IMFMediaEventGenerator, (void **)&c->async_events);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not get async interface\n");
        goto err;
    }

    res = 0;

err:
    IMFAttributes_Release(attrs);
    return res;
}

static int mf_setup_context(AVCodecContext* avctx)
{
    MFDecoderContext* c = avctx->priv_data;
    HRESULT hr;
    int ret;

    hr = IMFTransform_GetInputStreamInfo(c->mft, c->in_stream_id, &c->in_info);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    av_log(avctx, AV_LOG_VERBOSE, "in_info: size=%d, align=%d\n",
        (int)c->in_info.cbSize, (int)c->in_info.cbAlignment);

    hr = IMFTransform_GetOutputStreamInfo(c->mft, c->out_stream_id, &c->out_info);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    c->out_stream_provides_samples =
        (c->out_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) ||
        (c->out_info.dwFlags & MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);

    av_log(avctx, AV_LOG_VERBOSE, "out_info: size=%d, align=%d%s\n",
        (int)c->out_info.cbSize, (int)c->out_info.cbAlignment,
        c->out_stream_provides_samples ? " (provides samples)" : "");

    if ((ret = mf_output_type_get(avctx)) < 0)
        return ret;

    return 0;
}

static int mf_create(void *log, IMFTransform **mft, const AVCodec *codec, int use_hw)
{
    int is_audio = codec->type == AVMEDIA_TYPE_AUDIO;
    const CLSID *subtype = sx_extended_codec_to_mf_subtype(codec->id);
    MFT_REGISTER_TYPE_INFO reg = {0};
    GUID category;
    int ret;

    *mft = NULL;

    if (!subtype)
        return AVERROR(ENOSYS);

    reg.guidSubtype = *subtype;

    if (is_audio) {
        reg.guidMajorType = MFMediaType_Audio;
        category = MFT_CATEGORY_AUDIO_DECODER;
    } else {
        reg.guidMajorType = MFMediaType_Video;
        category = MFT_CATEGORY_VIDEO_DECODER;
    }

    if ((ret = av_ff_instantiate_mf(log, category, &reg, NULL, use_hw, mft)) < 0)
        return ret;

    return 0;
}

#pragma endregion


static int mfdec_init_sw(struct decoder_ctx *ctx, const struct sxplayer_opts *opts)
{
    int ret;
    AVCodecContext *avctx = ctx->avctx;
    HRESULT hr;

    // disabling hardware acceleration and asynchronous MFT support for future implementation
    int use_hw = 0;
    int use_async_mft = 0;

#if (defined(_WIN32) && defined(_DEBUG))
    // while (!IsDebuggerPresent()) Sleep(100);
#endif

    avctx->thread_count = 0;
    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);

    // Set up just enough of the codec to pass into mf_create and init for us
    AVCodec *mfcodec = av_mallocz(sizeof(AVCodec));
    mfcodec->id = avctx->codec_id;
    mfcodec->name = "MF AVCodec";
    avctx->codec = mfcodec;

    // Create the Media Foundation transform decoder
    IMFTransform* mft;
    if ((ret = mf_create(avctx, &mft, mfcodec, 0)) < 0)
        return ret;

    // Took a similar approach as in decoder_ffmpeg.c but we can't simply return the result here.
    if ((ret = avcodec_open2(avctx, mfcodec, NULL)) < 0)
        return ret;

    // create a decoder context for passing around
    if (ctx->priv_data != NULL) {
        if (!avctx->priv_data) {
            avctx->priv_data = av_memdup(ctx->priv_data, sizeof(MFDecoderContext));
            if (!avctx->priv_data) {
                return AVERROR(ENOMEM);
            }
        }
    }
    else {
        avctx->priv_data = NULL;
    }

    MFDecoderContext* c = avctx->priv_data;
    
    const CLSID* subtype = sx_extended_codec_to_mf_subtype(avctx->codec_id);
    if (!subtype)
        return AVERROR(ENOSYS);

    c->tmp_frame = av_frame_alloc();
    if (!c->tmp_frame)
        return AVERROR(ENOMEM);

    c->original_channels = avctx->channels;
    c->is_audio = avctx->codec_type == AVMEDIA_TYPE_AUDIO;
    c->is_video = !c->is_audio;
    c->main_subtype = *subtype;
    c->mft = mft;

    // asynchronous MFT support is not available and not fully tested.
    if (use_async_mft && (ret = mf_unlock_async(avctx)) < 0)
        return ret;

    // get ICodecAPI interface for maximum buffer resolution check later
    hr = IMFTransform_QueryInterface(c->mft, &IID_ICodecAPI, (void **)&c->codec_api);
    if (!FAILED(hr))
        av_log(avctx, AV_LOG_VERBOSE, "MFT supports ICodecAPI.\n");

    const char *bsf = NULL;

    if (avctx->codec->id == AV_CODEC_ID_H264 && avctx->extradata && avctx->extradata[0] == 1)
        bsf = "h264_mp4toannexb";

    if (avctx->codec->id == AV_CODEC_ID_HEVC && avctx->extradata && avctx->extradata[0] == 1)
        bsf = "hevc_mp4toannexb";

    if (bsf) {
        const AVBitStreamFilter *bsfc = av_bsf_get_by_name(bsf);
        if (!bsfc) {
            ret = AVERROR(ENOSYS);
            goto bsf_done;
        }
        if ((ret = av_bsf_alloc(bsfc, &c->bsfc)) < 0)
            goto bsf_done;
        if ((ret = avcodec_parameters_from_context(c->bsfc->par_in, avctx)) < 0)
            goto bsf_done;
        if ((ret = av_bsf_init(c->bsfc)) < 0)
            goto bsf_done;

         ret = 0;
    bsf_done:
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot open the %s BSF!\n", bsf);
            return ret;
        }
    }

    // Check for maximum MFT transform resolution requirement.
    if ((ret = mf_check_codec_requirements(avctx)) < 0)
        return ret;

    // get stream ids
    hr = IMFTransform_GetStreamIDs(c->mft, 1, &c->in_stream_id, 1, &c->out_stream_id);
    if (hr == E_NOTIMPL) {
        c->in_stream_id = c->out_stream_id = 0;
    } else if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not get stream IDs (%s)\n", av_ff_hr_str(hr));
        return AVERROR_EXTERNAL;
    }

    // Set the media subtypes and check the MF buffer requirements
    if ((ret = mf_negotiate_types(avctx)) < 0)
        return ret;
    if ((ret = mf_setup_context(avctx)) < 0)
        return ret;

    // start up streaming and notify start of stream
    hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not start streaming (%s)\n", av_ff_hr_str(hr));
        return AVERROR_EXTERNAL;
    }
    hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not start stream (%s)\n", av_ff_hr_str(hr));
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int mfdec_uninit_sw(struct decoder_ctx *ctx)
{
    AVCodecContext *avctx = ctx->avctx;
    MFDecoderContext *c = avctx->priv_data;

    int uninit_com = c->mft != NULL;

    if (c->codec_api)
        ICodecAPI_Release(c->codec_api);

    if (c->async_events)
        IMFMediaEventGenerator_Release(c->async_events);

    av_bsf_free(&c->bsfc);

    av_frame_free(&c->tmp_frame);

    if (uninit_com) {
        IMFTransform_Release(c->mft);
        MFShutdown();
        CoUninitialize();
    }

    return 0;
}

static int mfdec_init_hw(struct decoder_ctx *ctx, const struct sxplayer_opts *opts)
{
    return AVERROR_DECODER_NOT_FOUND;
}

static int mf_sample_to_a_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    HRESULT hr;
    int ret;
    DWORD len;
    IMFMediaBuffer *buffer;
    BYTE *data;
    size_t bps;

    hr = IMFSample_GetTotalLength(sample, &len);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    bps = av_get_bytes_per_sample(avctx->sample_fmt) * avctx->channels;

    frame->nb_samples = len/bps;
    if (frame->nb_samples * bps != len)
        return AVERROR_EXTERNAL; // unaligned crap -> assume not possible

    if ((ret = av_ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
    if (FAILED(hr)) {
        IMFMediaBuffer_Release(buffer);
        return AVERROR_EXTERNAL;
    }

    memcpy(frame->data[0], data, len);

    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);

    return 0;
}

typedef struct frame_ref {
    IMFMediaBuffer* mf_buffer;
    AVCodecContext* avcodec_ref;
} MFFrameRef;

static int mf_free_media_buffer_ref(void* opaque) {

    if (opaque) {

        MFFrameRef* ref = opaque;
        IMFMediaBuffer* media_buffer = ref->mf_buffer;

        IMFMediaBuffer_Unlock(media_buffer);
        IMFMediaBuffer_Release(media_buffer);

        av_log(ref->avcodec_ref, AV_LOG_VERBOSE, "mf_free_media_buffer_ref - freed media buffer\n");

        av_free(opaque);
    }

    return 0;
}

static int mf_set_avframe_data_from_sample(AVCodecContext* avctx, IMFSample* sample, AVFrame* frame)
{
    IMFMediaBuffer* media_buffer;
    int ret;

    if (!frame || !sample) {
        ret = AVERROR_INVALIDDATA;
    } 
    else {
        HRESULT hr = IMFSample_ConvertToContiguousBuffer(sample, &media_buffer);
        DWORD buff_len;

        if (FAILED(hr)) {
            ret = AVERROR_EXTERNAL;
        } 
        else {
            BYTE* data;
            hr = IMFMediaBuffer_Lock(media_buffer, &data, NULL, &buff_len);

            if (FAILED(hr)) {
                ret = AVERROR_EXTERNAL;
            } 
            else {
                int plane_linesz[4] = { 0 };
                ptrdiff_t linesizes[4];
                size_t ptrofs_sz[4];

                if ((ret = av_image_fill_linesizes(plane_linesz, frame->format, frame->width)) < 0) {
                    ret = AVERROR_INVALIDDATA;
                }
                else {
                    for (int i = 0; i < 4; i++) {
                        linesizes[i] = plane_linesz[i];
                    }

                    if ((ret = av_image_fill_plane_sizes(ptrofs_sz, avctx->pix_fmt, avctx->height, linesizes)) < 0) {
                        ret = AVERROR_INVALIDDATA;
                    }
                    else {
                        frame->data[0] = data;

                        int u_offset = ptrofs_sz[0] + 4 * plane_linesz[0];
                        int v_offset = u_offset + ptrofs_sz[1];

                        frame->data[1] = frame->data[0] + u_offset;
                        frame->data[2] = frame->data[0] + v_offset;

                        FrameDecodeData* fdd = (FrameDecodeData*)frame->private_ref->data;
                        MFFrameRef *ref = av_malloc(sizeof(*ref));
                        ref->mf_buffer = media_buffer;
                        ref->avcodec_ref = avctx;

                        fdd->post_process_opaque = ref;
                        fdd->post_process_opaque_free = mf_free_media_buffer_ref;
                    }
                }
            }
        }
    }

    if (ret < 0 && media_buffer)
        IMFMediaBuffer_Release(media_buffer);

    return ret;
}

static int mf_sample_to_v_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    int ret = 0;
    av_frame_unref(frame);
    
    if ((ret = av_ff_get_buffer(avctx, frame, 0)) >= 0) {
        ret = mf_set_avframe_data_from_sample(avctx, sample, frame);
    }

    // logging result 
    av_log(avctx, AV_LOG_VERBOSE, "mf_sample_to_v_avframe - set AVFrame from media buffer data, result: %d\n", ret);
    return ret;
}

// Allocate the given frame and copy the sample to it.
// Format must have been set on the avctx.
static int mfdec_sample_to_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    MFDecoderContext *c = avctx->priv_data;
    int ret;

    if (c->is_audio) {
        ret = mf_sample_to_a_avframe(avctx, sample, frame);
    } else {
        ret = mf_sample_to_v_avframe(avctx, sample, frame);
    }

    frame->pts = mf_sample_get_pts(avctx, sample);
    frame->best_effort_timestamp = frame->pts;
    frame->pkt_dts = AV_NOPTS_VALUE;

    return ret;
}

static int mfdec_receive_sample(AVCodecContext *avctx, IMFSample **out_sample)
{
    MFDecoderContext *c = avctx->priv_data;
    HRESULT hr;
    DWORD st;
    MFT_OUTPUT_DATA_BUFFER out_buffers;
    IMFSample *sample;
    int ret = 0;

    for (;;) {
        *out_sample = NULL;
        sample = NULL;

        if (c->async_events) {
            if ((ret = mf_wait_events(avctx)) < 0)
                return ret;
            if (!c->async_have_output || c->draining_done) {
                ret = 0;
                break;
            }
        }

        if (!c->out_stream_provides_samples) {
            sample = av_ff_create_memory_sample(NULL, c->out_info.cbSize, c->out_info.cbAlignment);
            if (!sample)
                return AVERROR(ENOMEM);
        }

        out_buffers = (MFT_OUTPUT_DATA_BUFFER) {
            .dwStreamID = c->out_stream_id,
            .pSample = sample,
        };

        st = 0;
        hr = IMFTransform_ProcessOutput(c->mft, 0, 1, &out_buffers, &st);

        if (out_buffers.pEvents)
            IMFCollection_Release(out_buffers.pEvents);

        if (!FAILED(hr)) {
            // We have a MF sample so exit
            *out_sample = out_buffers.pSample;
            ret = 0;
            break;
        }

        if (out_buffers.pSample)
            IMFSample_Release(out_buffers.pSample);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (c->draining)
                c->draining_done = 1;
            ret = 0;
        } else if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            av_log(avctx,  AV_LOG_WARNING, "stream format change\n");
            ret = mf_choose_output_type(avctx);
            if (ret == 0) // we don't expect renegotiating the input type
                ret = AVERROR_EXTERNAL;
            if (ret > 0) {
                ret = mf_setup_context(avctx);
                if (ret >= 0) {
                    c->async_have_output = 0;
                    continue;
                }
            }
        } else {
            av_log(avctx, AV_LOG_ERROR, "failed processing output: %s\n", av_ff_hr_str(hr));
            ret = AVERROR_EXTERNAL;
        }

        break;
    }

    c->async_have_output = 0;

    if (ret >= 0 && !*out_sample)
        ret = c->draining_done ? AVERROR_EOF : AVERROR(EAGAIN);

    return ret;
}

static int mfdec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    IMFSample *sample;
    int ret;
    AVPacket packet;

    for (;;) {
        ret = mfdec_receive_sample(avctx, &sample);
        av_log(avctx, AV_LOG_VERBOSE, "mf_receive_sample result: %d\n", ret);

        if (ret == 0) {
            // we have an IMFSample from Media Foundation,
            // convert it to an AVFrame for consumption down stream
            ret = mfdec_sample_to_avframe(avctx, sample, frame);
            IMFSample_Release(sample);

            av_log(avctx, AV_LOG_VERBOSE, "mf_sample_to_avframe result: %d\n", ret);
            return ret;

        } else if (ret == AVERROR(EAGAIN)) {
            ret = av_ff_decode_get_packet(avctx, &packet);
            if (ret < 0) {
                return ret;
            }
            ret = mfdec_send_packet(avctx, &packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                return ret;
            }
        } else {
            return ret;
        }
    }
}

static int mfdec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    int ret;
    HRESULT hr;
    MFDecoderContext *c = avctx->priv_data;
    IMFSample *sample = NULL;

    if (avpkt) {
        sample = mf_avpacket_to_sample(avctx, avpkt);
        if (!sample)
            return AVERROR(ENOMEM);
    }

    if (sample) {
        ret = 0;
        if (c->async_events) {
            ret = mf_wait_events(avctx);
            if (ret >= 0 && !c->async_need_input)
                ret = AVERROR(EAGAIN);
        }
        if (ret >= 0) {
            if (!c->sample_sent)
                IMFSample_SetUINT32(sample, &MFSampleExtension_Discontinuity, TRUE);
            c->sample_sent = 1;

            hr = IMFTransform_ProcessInput(c->mft, c->in_stream_id, sample, 0);

            if (hr == MF_E_NOTACCEPTING) {
                ret = AVERROR(EAGAIN);
            } else if (FAILED(hr)) {
                av_log(avctx, AV_LOG_ERROR, "failed processing input: %s\n", av_ff_hr_str(hr));
                ret = AVERROR_EXTERNAL;
            }

            DWORD dwFlags = 0;
            hr = IMFTransform_GetOutputStatus(c->mft, &dwFlags);
            if (SUCCEEDED(hr)) {
                BOOL needsData = ((dwFlags & MF_E_TRANSFORM_NEED_MORE_INPUT) == MF_E_TRANSFORM_NEED_MORE_INPUT);
            }

            c->async_need_input = 0;
        }
    } else if (!c->draining) {
        hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_COMMAND_DRAIN, 0);

        if (FAILED(hr))
            av_log(avctx, AV_LOG_ERROR, "failed draining: %s\n", av_ff_hr_str(hr));

        // Some MFTs (AC3) will send a frame after each drain command (???), so
        // this is required to make draining actually terminate.
        c->draining = 1;
        c->async_need_input = 0;
        ret = 0;
    } else {
        ret = AVERROR_EOF;
    }

    if (sample)
        IMFSample_Release(sample);

    return ret;
}

static int mfdec_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    int pkt_consumed = 0;
    AVCodecContext *avctx = ctx->avctx;
    const int pkt_size = pkt ? pkt->size : 0;
    const int flush = !pkt_size;
    int ret;

    av_assert0(avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
               avctx->codec_type == AVMEDIA_TYPE_AUDIO);

    TRACE(ctx, "Received packet of size %d", pkt_size);

    while (!pkt_consumed) {
        // send the packet to the MFT transform decoder
        ret = mfdec_send_packet(avctx, pkt);
        av_log(avctx, AV_LOG_VERBOSE, "mfdec_push_packet packet-size: %d, result: %d\n", pkt ? pkt->size : 0, ret);

        if (ret == AVERROR(EAGAIN)) {
            ret = 0;
        } else if (ret < 0) {
            LOG(ctx, ERROR, "Error sending packet to %s decoder: %s",
                av_get_media_type_string(avctx->codec_type),
                av_err2str(ret));
            return ret;
        } else {
            pkt_consumed = 1;
        }

        const int draining = flush && pkt_consumed;
        int64_t next_pts = AV_NOPTS_VALUE;
        while (ret >= 0 || (draining && ret == AVERROR(EAGAIN))) {
            AVFrame *dec_frame = av_frame_alloc();

            if (!dec_frame)
                return AVERROR(ENOMEM);

            // receive frame from MFT
            ret = mfdec_receive_frame(avctx, dec_frame);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                LOG(ctx, ERROR, "Error receiving frame from %s decoder: %s",
                    av_get_media_type_string(avctx->codec_type),
                    av_err2str(ret));
                    av_frame_free(&dec_frame);
                return ret;
            }

            if (ret >= 0) {
                /*
                 * If there are multiple frames in the packet, some frames may
                 * not have any PTS but we don't want to drop them.
                 */
                if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                    if (dec_frame->pts == AV_NOPTS_VALUE && next_pts != AV_NOPTS_VALUE)
                        dec_frame->pts = next_pts;
                    else if (next_pts == AV_NOPTS_VALUE)
                        next_pts = dec_frame->pts;
                    if (next_pts != AV_NOPTS_VALUE)
                        next_pts += dec_frame->nb_samples;
                }

                ret = sxpi_decoding_queue_frame(ctx->decoding_ctx, dec_frame);
                if (ret < 0) {
                    TRACE(ctx, "Could not queue frame: %s", av_err2str(ret));
                    av_frame_free(&dec_frame);
                    return ret;
                }
            } else {
                av_frame_free(&dec_frame);
            }
        }
    }

    if (ret == AVERROR(EAGAIN))
        ret = 0;

    if (flush)
        ret = sxpi_decoding_queue_frame(ctx->decoding_ctx, NULL);

    return ret;
}

static void mfdec_flush(struct decoder_ctx *ctx)
{
    AVCodecContext *avctx = ctx->avctx;
    avcodec_flush_buffers(avctx);
}

const struct decoder sxpi_decoder_mediaf_sw = {
    .name        = "mediaf_sw",
    .init        = mfdec_init_sw,
    .push_packet = mfdec_push_packet,
    .flush       = mfdec_flush,
    .uninit      = mfdec_uninit_sw,
    .priv_data_size = sizeof(MFDecoderContext),
};

const struct decoder sxpi_decoder_mediaf_hw = {
    .name        = "mediaf_hw",
    .init        = mfdec_init_hw,
    .push_packet = mfdec_push_packet,
    .flush       = mfdec_flush,
};

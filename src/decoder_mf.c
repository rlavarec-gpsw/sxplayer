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

static const enum AVPixelFormat mf_pix_fmts[] = {
    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUYV422,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_NONE
};

// MP42 FourCC. Pawel Wegner (with WM4) created the symbol name.
DEFINE_GUID(ff_MFVideoFormat_MP42, 0x3234504D, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

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

// RFJ remove these cloned 'internal_' methods once we have the proper integration decided
// These should be added to EXPORTS from an internal GoPro FFmpeg library build 
// (using the current https://github.com/gopro/FFmpeg repo)
//
// NOTE: internal ffmpeg functions were copied here and renamed from ff_* to internal_*.
//       
#pragma region "INTERNAL_AVFCNS"

static int internal_set_mf_attributes(IMFAttributes *pattrs, const GUID *attrid, UINT32 inhi, UINT32 inlo)
{
    UINT64 t = (((UINT64)inhi) << 32) | inlo;
    return IMFAttributes_SetUINT64(pattrs, attrid, t);
}

static int internal_get_mf_attributes(IMFAttributes *pattrs, const GUID *attrid, UINT32 *outhi, UINT32 *outlo)
{
    UINT64 ut;
    HRESULT hr = IMFAttributes_GetUINT64(pattrs, attrid, &ut);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;
    else {
        *outhi = ut >> 32;
        *outlo = (UINT32)ut;
    }
    return 0;
}

int internal_set_dimensions(AVCodecContext *s, int width, int height)
{
    int ret = av_image_check_size2(width, height, s->max_pixels, AV_PIX_FMT_NONE, 0, s);

    if (ret < 0)
        width = height = 0;

    s->coded_width  = width;
    s->coded_height = height;
    s->width        = AV_CEIL_RSHIFT(width,  s->lowres);
    s->height       = AV_CEIL_RSHIFT(height, s->lowres);

    return ret;
}

#define internal_hr_str(hr) internal_hr_str_buf((char[80]){0}, 80, hr)

char *internal_hr_str_buf(char *buf, size_t size, HRESULT hr)
{
#define HR(x) case x: return (char *) # x;
    switch (hr) {
    HR(S_OK)
    HR(E_UNEXPECTED)
    HR(MF_E_INVALIDMEDIATYPE)
    HR(MF_E_INVALIDSTREAMNUMBER)
    HR(MF_E_INVALIDTYPE)
    HR(MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING)
    HR(MF_E_TRANSFORM_TYPE_NOT_SET)
    HR(MF_E_UNSUPPORTED_D3D_TYPE)
    HR(MF_E_TRANSFORM_NEED_MORE_INPUT)
    HR(MF_E_TRANSFORM_STREAM_CHANGE)
    HR(MF_E_NOTACCEPTING)
    HR(MF_E_NO_SAMPLE_TIMESTAMP)
    HR(MF_E_NO_SAMPLE_DURATION)
#undef HR
    }
    snprintf(buf, size, "%x", (unsigned)hr);
    return buf;
}

struct GUID_Entry {
    const GUID *guid;
    const char *name;
};

#define GUID_ENTRY(var) {&(var), # var}

static struct GUID_Entry guid_names[] = {
    GUID_ENTRY(MFT_FRIENDLY_NAME_Attribute),
    GUID_ENTRY(MFT_TRANSFORM_CLSID_Attribute),
    GUID_ENTRY(MFT_ENUM_HARDWARE_URL_Attribute),
    GUID_ENTRY(MFT_CONNECTED_STREAM_ATTRIBUTE),
    GUID_ENTRY(MFT_CONNECTED_TO_HW_STREAM),
    GUID_ENTRY(MF_SA_D3D_AWARE),
    GUID_ENTRY(ff_MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT),
    GUID_ENTRY(ff_MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE),
    GUID_ENTRY(ff_MF_SA_D3D11_BINDFLAGS),
    GUID_ENTRY(ff_MF_SA_D3D11_USAGE),
    GUID_ENTRY(ff_MF_SA_D3D11_AWARE),
    GUID_ENTRY(ff_MF_SA_D3D11_SHARED),
    GUID_ENTRY(ff_MF_SA_D3D11_SHARED_WITHOUT_MUTEX),
    GUID_ENTRY(MF_MT_SUBTYPE),
    GUID_ENTRY(MF_MT_MAJOR_TYPE),
    GUID_ENTRY(MF_MT_AUDIO_SAMPLES_PER_SECOND),
    GUID_ENTRY(MF_MT_AUDIO_NUM_CHANNELS),
    GUID_ENTRY(MF_MT_AUDIO_CHANNEL_MASK),
    GUID_ENTRY(MF_MT_FRAME_SIZE),
    GUID_ENTRY(MF_MT_INTERLACE_MODE),
    GUID_ENTRY(MF_MT_USER_DATA),
    GUID_ENTRY(MF_MT_PIXEL_ASPECT_RATIO),
    GUID_ENTRY(MFMediaType_Audio),
    GUID_ENTRY(MFMediaType_Video),
    GUID_ENTRY(MFAudioFormat_PCM),
    GUID_ENTRY(MFAudioFormat_Float),
    GUID_ENTRY(MFVideoFormat_H264),
    GUID_ENTRY(MFVideoFormat_H264_ES),
    GUID_ENTRY(ff_MFVideoFormat_HEVC),
    GUID_ENTRY(ff_MFVideoFormat_HEVC_ES),
    GUID_ENTRY(MFVideoFormat_MPEG2),
    GUID_ENTRY(MFVideoFormat_MP43),
    GUID_ENTRY(MFVideoFormat_MP4V),
    GUID_ENTRY(MFVideoFormat_WMV1),
    GUID_ENTRY(MFVideoFormat_WMV2),
    GUID_ENTRY(MFVideoFormat_WMV3),
    GUID_ENTRY(MFVideoFormat_WVC1),
    GUID_ENTRY(MFAudioFormat_Dolby_AC3),
    GUID_ENTRY(MFAudioFormat_Dolby_DDPlus),
    GUID_ENTRY(MFAudioFormat_AAC),
    GUID_ENTRY(MFAudioFormat_MP3),
    GUID_ENTRY(MFAudioFormat_MSP1),
    GUID_ENTRY(MFAudioFormat_WMAudioV8),
    GUID_ENTRY(MFAudioFormat_WMAudioV9),
    GUID_ENTRY(MFAudioFormat_WMAudio_Lossless),
    GUID_ENTRY(MF_MT_ALL_SAMPLES_INDEPENDENT),
    GUID_ENTRY(MF_MT_COMPRESSED),
    GUID_ENTRY(MF_MT_FIXED_SIZE_SAMPLES),
    GUID_ENTRY(MF_MT_SAMPLE_SIZE),
    GUID_ENTRY(MF_MT_WRAPPED_TYPE),
    GUID_ENTRY(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION),
    GUID_ENTRY(MF_MT_AAC_PAYLOAD_TYPE),
    GUID_ENTRY(MF_MT_AUDIO_AVG_BYTES_PER_SECOND),
    GUID_ENTRY(MF_MT_AUDIO_BITS_PER_SAMPLE),
    GUID_ENTRY(MF_MT_AUDIO_BLOCK_ALIGNMENT),
    GUID_ENTRY(MF_MT_AUDIO_CHANNEL_MASK),
    GUID_ENTRY(MF_MT_AUDIO_FLOAT_SAMPLES_PER_SECOND),
    GUID_ENTRY(MF_MT_AUDIO_FOLDDOWN_MATRIX),
    GUID_ENTRY(MF_MT_AUDIO_NUM_CHANNELS),
    GUID_ENTRY(MF_MT_AUDIO_PREFER_WAVEFORMATEX),
    GUID_ENTRY(MF_MT_AUDIO_SAMPLES_PER_BLOCK),
    GUID_ENTRY(MF_MT_AUDIO_SAMPLES_PER_SECOND),
    GUID_ENTRY(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE),
    GUID_ENTRY(MF_MT_AUDIO_WMADRC_AVGREF),
    GUID_ENTRY(MF_MT_AUDIO_WMADRC_AVGTARGET),
    GUID_ENTRY(MF_MT_AUDIO_WMADRC_PEAKREF),
    GUID_ENTRY(MF_MT_AUDIO_WMADRC_PEAKTARGET),
    GUID_ENTRY(MF_MT_AVG_BIT_ERROR_RATE),
    GUID_ENTRY(MF_MT_AVG_BITRATE),
    GUID_ENTRY(MF_MT_DEFAULT_STRIDE),
    GUID_ENTRY(MF_MT_DRM_FLAGS),
    GUID_ENTRY(MF_MT_FRAME_RATE),
    GUID_ENTRY(MF_MT_FRAME_RATE_RANGE_MAX),
    GUID_ENTRY(MF_MT_FRAME_RATE_RANGE_MIN),
    GUID_ENTRY(MF_MT_FRAME_SIZE),
    GUID_ENTRY(MF_MT_GEOMETRIC_APERTURE),
    GUID_ENTRY(MF_MT_INTERLACE_MODE),
    GUID_ENTRY(MF_MT_MAX_KEYFRAME_SPACING),
    GUID_ENTRY(MF_MT_MINIMUM_DISPLAY_APERTURE),
    GUID_ENTRY(MF_MT_MPEG_SEQUENCE_HEADER),
    GUID_ENTRY(MF_MT_MPEG_START_TIME_CODE),
    GUID_ENTRY(MF_MT_MPEG2_FLAGS),
    GUID_ENTRY(MF_MT_MPEG2_LEVEL),
    GUID_ENTRY(MF_MT_MPEG2_PROFILE),
    GUID_ENTRY(MF_MT_PAD_CONTROL_FLAGS),
    GUID_ENTRY(MF_MT_PALETTE),
    GUID_ENTRY(MF_MT_PAN_SCAN_APERTURE),
    GUID_ENTRY(MF_MT_PAN_SCAN_ENABLED),
    GUID_ENTRY(MF_MT_PIXEL_ASPECT_RATIO),
    GUID_ENTRY(MF_MT_SOURCE_CONTENT_HINT),
    GUID_ENTRY(MF_MT_TRANSFER_FUNCTION),
    GUID_ENTRY(MF_MT_VIDEO_CHROMA_SITING),
    GUID_ENTRY(MF_MT_VIDEO_LIGHTING),
    GUID_ENTRY(MF_MT_VIDEO_NOMINAL_RANGE),
    GUID_ENTRY(MF_MT_VIDEO_PRIMARIES),
    GUID_ENTRY(MF_MT_VIDEO_ROTATION),
    GUID_ENTRY(MF_MT_YUV_MATRIX),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoThumbnailGenerationMode),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoDropPicWithMissingRef),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoSoftwareDeinterlaceMode),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoFastDecodeMode),
    GUID_ENTRY(ff_CODECAPI_AVLowLatencyMode),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoH264ErrorConcealment),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoMPEG2ErrorConcealment),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoCodecType),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoDXVAMode),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoDXVABusEncryption),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoSWPowerLevel),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoMaxCodedWidth),
    GUID_ENTRY(ff_CODECAPI_AVDecVideoMaxCodedHeight),
    GUID_ENTRY(ff_CODECAPI_AVDecNumWorkerThreads),
    GUID_ENTRY(ff_CODECAPI_AVDecSoftwareDynamicFormatChange),
    GUID_ENTRY(ff_CODECAPI_AVDecDisableVideoPostProcessing),
};

#define internal_guid_str(guid) internal_guid_str_buf((char[80]){0}, 80, guid)

char *internal_guid_str_buf(char *buf, size_t buf_size, const GUID *guid)
{
    uint32_t fourcc;
    int n;
    for (n = 0; n < FF_ARRAY_ELEMS(guid_names); n++) {
        if (IsEqualGUID(guid, guid_names[n].guid)) {
            snprintf(buf, buf_size, "%s", guid_names[n].name);
            return buf;
        }
    }

    if (internal_fourcc_from_guid(guid, &fourcc) >= 0) {
        snprintf(buf, buf_size, "<FourCC %s>", av_fourcc2str(fourcc));
        return buf;
    }

    snprintf(buf, buf_size,
             "{%8.8x-%4.4x-%4.4x-%2.2x%2.2x-%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x}",
             (unsigned) guid->Data1, guid->Data2, guid->Data3,
             guid->Data4[0], guid->Data4[1],
             guid->Data4[2], guid->Data4[3],
             guid->Data4[4], guid->Data4[5],
             guid->Data4[6], guid->Data4[7]);
    return buf;
}

// If fill_data!=NULL, initialize the buffer and set the length. (This is a
// subtle but important difference: some decoders want CurrentLength==0 on
// provided output buffers.)
IMFSample *internal_create_memory_sample(void *fill_data, size_t size, size_t align)
{
    HRESULT hr;
    IMFSample *sample;
    IMFMediaBuffer *buffer;

    hr = MFCreateSample(&sample);
    if (FAILED(hr))
        return NULL;

    align = FFMAX(align, 16); // 16 is "recommended", even if not required

    hr = MFCreateAlignedMemoryBuffer(size, align - 1, &buffer);
    if (FAILED(hr))
        return NULL;

    if (fill_data) {
        BYTE *tmp;

        hr = IMFMediaBuffer_Lock(buffer, &tmp, NULL, NULL);
        if (FAILED(hr)) {
            IMFMediaBuffer_Release(buffer);
            IMFSample_Release(sample);
            return NULL;
        }
        memcpy(tmp, fill_data, size);

        IMFMediaBuffer_SetCurrentLength(buffer, size);
        IMFMediaBuffer_Unlock(buffer);
    }

    IMFSample_AddBuffer(sample, buffer);
    IMFMediaBuffer_Release(buffer);

    return sample;
}

static int internal_apply_param_change(AVCodecContext *avctx, const AVPacket *avpkt)
{
    int ret;
    size_t size;
    const uint8_t *data;
    uint32_t flags;
    int64_t val;

    data = av_packet_get_side_data(avpkt, AV_PKT_DATA_PARAM_CHANGE, &size);
    if (!data)
        return 0;

    if (!(avctx->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE)) {
        av_log(avctx, AV_LOG_ERROR, "This decoder does not support parameter "
               "changes, but PARAM_CHANGE side data was sent to it.\n");
        ret = AVERROR(EINVAL);
        goto fail2;
    }

    if (size < 4)
        goto fail;

    flags = bytestream_get_le32(&data);
    size -= 4;

    if (flags & AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_COUNT) {
        if (size < 4)
            goto fail;
        val = bytestream_get_le32(&data);
        if (val <= 0 || val > INT_MAX) {
            av_log(avctx, AV_LOG_ERROR, "Invalid channel count");
            ret = AVERROR_INVALIDDATA;
            goto fail2;
        }
        avctx->channels = val;
        size -= 4;
    }
    if (flags & AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_LAYOUT) {
        if (size < 8)
            goto fail;
        avctx->channel_layout = bytestream_get_le64(&data);
        size -= 8;
    }
    if (flags & AV_SIDE_DATA_PARAM_CHANGE_SAMPLE_RATE) {
        if (size < 4)
            goto fail;
        val = bytestream_get_le32(&data);
        if (val <= 0 || val > INT_MAX) {
            av_log(avctx, AV_LOG_ERROR, "Invalid sample rate");
            ret = AVERROR_INVALIDDATA;
            goto fail2;
        }
        avctx->sample_rate = val;
        size -= 4;
    }
    if (flags & AV_SIDE_DATA_PARAM_CHANGE_DIMENSIONS) {
        if (size < 8)
            goto fail;
        avctx->width  = bytestream_get_le32(&data);
        avctx->height = bytestream_get_le32(&data);
        size -= 8;
        ret = internal_set_dimensions(avctx, avctx->width, avctx->height);
        if (ret < 0)
            goto fail2;
    }

    return 0;
fail:
    av_log(avctx, AV_LOG_ERROR, "PARAM_CHANGE side data too small.\n");
    ret = AVERROR_INVALIDDATA;
fail2:
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error applying parameter changes.\n");
        if (avctx->err_recognition & AV_EF_EXPLODE)
            return ret;
    }
    return 0;
}

#define IS_EMPTY(pkt) (!(pkt)->data)

static int internal_copy_packet_props(AVPacket *dst, const AVPacket *src)
{
    int ret = av_packet_copy_props(dst, src);
    if (ret < 0)
        return ret;

    dst->size = src->size; // HACK: Needed for ff_decode_frame_props().
    dst->data = (void*)1;  // HACK: Needed for IS_EMPTY().

    return 0;
}

static int internal_extract_packet_props(AVCodecInternal *avci, const AVPacket *pkt)
{
    AVPacket tmp = { 0 };
    int ret = 0;

    if (IS_EMPTY(avci->last_pkt_props)) {
        if (av_fifo_size(avci->pkt_props) >= sizeof(*pkt)) {
            av_fifo_generic_read(avci->pkt_props, avci->last_pkt_props,
                                 sizeof(*avci->last_pkt_props), NULL);
        } else
            return internal_copy_packet_props(avci->last_pkt_props, pkt);
    }

    if (av_fifo_space(avci->pkt_props) < sizeof(*pkt)) {
        ret = av_fifo_grow(avci->pkt_props, sizeof(*pkt));
        if (ret < 0)
            return ret;
    }

    ret = internal_copy_packet_props(&tmp, pkt);
    if (ret < 0)
        return ret;

    av_fifo_generic_write(avci->pkt_props, &tmp, sizeof(tmp), NULL);

    return 0;
}

int internal_decode_get_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    if (avci->draining)
        return AVERROR_EOF;

    ret = av_bsf_receive_packet(avci->bsf, pkt);
    if (ret == AVERROR_EOF)
        avci->draining = 1;
    if (ret < 0)
        return ret;

    if (!(avctx->codec->caps_internal & FF_CODEC_CAP_SETS_FRAME_PROPS)) {
        ret = internal_extract_packet_props(avctx->internal, pkt);
        if (ret < 0)
            goto finish;
    }

    ret = internal_apply_param_change(avctx, pkt);
    if (ret < 0)
        goto finish;

    return 0;
finish:
    av_packet_unref(pkt);
    return ret;
}

const CLSID *internal_codec_to_mf_subtype(enum AVCodecID codec)
{
    switch (codec) {
    case AV_CODEC_ID_H264:              return &MFVideoFormat_H264;
    case AV_CODEC_ID_HEVC:              return &ff_MFVideoFormat_HEVC;
    case AV_CODEC_ID_AC3:               return &MFAudioFormat_Dolby_AC3;
    case AV_CODEC_ID_AAC:               return &MFAudioFormat_AAC;
    case AV_CODEC_ID_MP3:               return &MFAudioFormat_MP3;
    default:                            return NULL;
    }
}

enum AVSampleFormat internal_media_type_to_sample_fmt(IMFAttributes *type)
{
    HRESULT hr;
    UINT32 bits;
    GUID subtype;

    hr = IMFAttributes_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (FAILED(hr))
        return AV_SAMPLE_FMT_NONE;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr))
        return AV_SAMPLE_FMT_NONE;

    if (IsEqualGUID(&subtype, &MFAudioFormat_PCM)) {
        switch (bits) {
        case 8:  return AV_SAMPLE_FMT_U8;
        case 16: return AV_SAMPLE_FMT_S16;
        case 32: return AV_SAMPLE_FMT_S32;
        }
    } else if (IsEqualGUID(&subtype, &MFAudioFormat_Float)) {
        switch (bits) {
        case 32: return AV_SAMPLE_FMT_FLT;
        case 64: return AV_SAMPLE_FMT_DBL;
        }
    }

    return AV_SAMPLE_FMT_NONE;
}

struct mf_pix_fmt_entry {
    const GUID *guid;
    enum AVPixelFormat pix_fmt;
};

static const struct mf_pix_fmt_entry mf_pix_fmts_map[] = {
    {&MFVideoFormat_IYUV, AV_PIX_FMT_YUV420P},
    {&MFVideoFormat_I420, AV_PIX_FMT_YUV420P},
    {&MFVideoFormat_NV12, AV_PIX_FMT_NV12},
    {&MFVideoFormat_P010, AV_PIX_FMT_P010},
    {&MFVideoFormat_P016, AV_PIX_FMT_P010}, // not equal, but compatible
    {&MFVideoFormat_YUY2, AV_PIX_FMT_YUYV422},
};

enum AVPixelFormat internal_media_type_to_pix_fmt(IMFAttributes *type)
{
    HRESULT hr;
    GUID subtype;
    int i;

    hr = IMFAttributes_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr))
        return AV_PIX_FMT_NONE;

    for (i = 0; i < FF_ARRAY_ELEMS(mf_pix_fmts_map); i++) {
        if (IsEqualGUID(&subtype, mf_pix_fmts_map[i].guid))
            return mf_pix_fmts_map[i].pix_fmt;
    }

    return AV_PIX_FMT_NONE;
}

// If this GUID is of the form XXXXXXXX-0000-0010-8000-00AA00389B71, then
// extract the XXXXXXXX prefix as FourCC (oh the pain).
int internal_fourcc_from_guid(const GUID *guid, uint32_t *out_fourcc)
{
    if (guid->Data2 == 0 && guid->Data3 == 0x0010 &&
        guid->Data4[0] == 0x80 &&
        guid->Data4[1] == 0x00 &&
        guid->Data4[2] == 0x00 &&
        guid->Data4[3] == 0xAA &&
        guid->Data4[4] == 0x00 &&
        guid->Data4[5] == 0x38 &&
        guid->Data4[6] == 0x9B &&
        guid->Data4[7] == 0x71) {
        *out_fourcc = guid->Data1;
        return 0;
    }

    *out_fourcc = 0;
    return AVERROR_UNKNOWN;
}

void internal_attributes_dump(void *log, IMFAttributes *attrs)
{
    HRESULT hr;
    UINT32 count;
    int n;

    hr = IMFAttributes_GetCount(attrs, &count);
    if (FAILED(hr))
        return;

    for (n = 0; n < count; n++) {
        GUID key;
        MF_ATTRIBUTE_TYPE type;
        char extra[80] = {0};
        const char *name = NULL;

        hr = IMFAttributes_GetItemByIndex(attrs, n, &key, NULL);
        if (FAILED(hr))
            goto err;

        name = internal_guid_str(&key);

        if (IsEqualGUID(&key, &MF_MT_AUDIO_CHANNEL_MASK)) {
            UINT32 v;
            hr = IMFAttributes_GetUINT32(attrs, &key, &v);
            if (FAILED(hr))
                goto err;
            snprintf(extra, sizeof(extra), " (0x%x)", (unsigned)v);
        } else if (IsEqualGUID(&key, &MF_MT_FRAME_SIZE)) {
            UINT32 w, h;

            hr = internal_get_mf_attributes(attrs, &MF_MT_FRAME_SIZE, &w, &h);
            if (FAILED(hr))
                goto err;
            snprintf(extra, sizeof(extra), " (%dx%d)", (int)w, (int)h);
        } else if (IsEqualGUID(&key, &MF_MT_PIXEL_ASPECT_RATIO) ||
                   IsEqualGUID(&key, &MF_MT_FRAME_RATE)) {
            UINT32 num, den;

            hr = internal_get_mf_attributes(attrs, &key, &num, &den);
            if (FAILED(hr))
                goto err;
            snprintf(extra, sizeof(extra), " (%d:%d)", (int)num, (int)den);
        }

        hr = IMFAttributes_GetItemType(attrs, &key, &type);
        if (FAILED(hr))
            goto err;

        switch (type) {
            case MF_ATTRIBUTE_UINT32: {
                UINT32 v;
                hr = IMFAttributes_GetUINT32(attrs, &key, &v);
                if (FAILED(hr))
                    goto err;
                av_log(log, AV_LOG_VERBOSE, "   %s=%d%s\n", name, (int)v, extra);
                break;
            case MF_ATTRIBUTE_UINT64: {
                UINT64 v;
                hr = IMFAttributes_GetUINT64(attrs, &key, &v);
                if (FAILED(hr))
                    goto err;
                av_log(log, AV_LOG_VERBOSE, "   %s=%lld%s\n", name, (long long)v, extra);
                break;
            }
            case MF_ATTRIBUTE_DOUBLE: {
                DOUBLE v;
                hr = IMFAttributes_GetDouble(attrs, &key, &v);
                if (FAILED(hr))
                    goto err;
                av_log(log, AV_LOG_VERBOSE, "   %s=%f%s\n", name, (double)v, extra);
                break;
            }
            case MF_ATTRIBUTE_STRING: {
                wchar_t s[512]; // being lazy here
                hr = IMFAttributes_GetString(attrs, &key, s, sizeof(s), NULL);
                if (FAILED(hr))
                    goto err;
                av_log(log, AV_LOG_VERBOSE, "   %s='%ls'%s\n", name, s, extra);
                break;
            }
            case MF_ATTRIBUTE_GUID: {
                GUID v;
                hr = IMFAttributes_GetGUID(attrs, &key, &v);
                if (FAILED(hr))
                    goto err;
                av_log(log, AV_LOG_VERBOSE, "   %s=%s%s\n", name, internal_guid_str(&v), extra);
                break;
            }
            case MF_ATTRIBUTE_BLOB: {
                UINT32 sz;
                UINT8 buffer[100];
                hr = IMFAttributes_GetBlobSize(attrs, &key, &sz);
                if (FAILED(hr))
                    goto err;
                if (sz <= sizeof(buffer)) {
                    // hex-dump it
                    char str[512] = {0};
                    size_t pos = 0;
                    hr = IMFAttributes_GetBlob(attrs, &key, buffer, sizeof(buffer), &sz);
                    if (FAILED(hr))
                        goto err;
                    for (pos = 0; pos < sz; pos++) {
                        const char *hex = "0123456789ABCDEF";
                        if (pos * 3 + 3 > sizeof(str))
                            break;
                        str[pos * 3 + 0] = hex[buffer[pos] >> 4];
                        str[pos * 3 + 1] = hex[buffer[pos] & 15];
                        str[pos * 3 + 2] = ' ';
                    }
                    str[pos * 3 + 0] = 0;
                    av_log(log, AV_LOG_VERBOSE, "   %s=<blob size %d: %s>%s\n", name, (int)sz, str, extra);
                } else {
                    av_log(log, AV_LOG_VERBOSE, "   %s=<blob size %d>%s\n", name, (int)sz, extra);
                }
                break;
            }
            case MF_ATTRIBUTE_IUNKNOWN: {
                av_log(log, AV_LOG_VERBOSE, "   %s=<IUnknown>%s\n", name, extra);
                break;
            }
            default:
                av_log(log, AV_LOG_VERBOSE, "   %s=<unknown type>%s\n", name, extra);
                break;
            }
        }

        if (IsEqualGUID(&key, &MF_MT_SUBTYPE)) {
            const char *fmt;
            fmt = av_get_sample_fmt_name(internal_media_type_to_sample_fmt(attrs));
            if (fmt)
                av_log(log, AV_LOG_VERBOSE, "   FF-sample-format=%s\n", fmt);

            fmt = av_get_pix_fmt_name(internal_media_type_to_pix_fmt(attrs));
            if (fmt)
                av_log(log, AV_LOG_VERBOSE, "   FF-pixel-format=%s\n", fmt);
        }

        continue;
    err:
        av_log(log, AV_LOG_VERBOSE, "   %s=<failed to get value>\n", name ? name : "?");
    }
}

static int internal_add_metadata_from_side_data(const AVPacket *avpkt, AVFrame *frame)
{
    size_t size;
    const uint8_t *side_metadata;

    AVDictionary **frame_md = &frame->metadata;

    side_metadata = av_packet_get_side_data(avpkt,
                                            AV_PKT_DATA_STRINGS_METADATA, &size);
    return av_packet_unpack_dictionary(side_metadata, size, frame_md);
}

int internal_decode_frame_props(AVCodecContext *avctx, AVFrame *frame)
{
    AVPacket *pkt = avctx->internal->last_pkt_props;

    static const struct {
        enum AVPacketSideDataType packet;
        enum AVFrameSideDataType frame;
    } 
    pkt_to_frame_sd[] = {
        { AV_PKT_DATA_REPLAYGAIN ,                AV_FRAME_DATA_REPLAYGAIN },
        { AV_PKT_DATA_DISPLAYMATRIX,              AV_FRAME_DATA_DISPLAYMATRIX },
        { AV_PKT_DATA_SPHERICAL,                  AV_FRAME_DATA_SPHERICAL },
        { AV_PKT_DATA_STEREO3D,                   AV_FRAME_DATA_STEREO3D },
        { AV_PKT_DATA_AUDIO_SERVICE_TYPE,         AV_FRAME_DATA_AUDIO_SERVICE_TYPE },
        { AV_PKT_DATA_MASTERING_DISPLAY_METADATA, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA },
        { AV_PKT_DATA_CONTENT_LIGHT_LEVEL,        AV_FRAME_DATA_CONTENT_LIGHT_LEVEL },
        { AV_PKT_DATA_A53_CC,                     AV_FRAME_DATA_A53_CC },
        { AV_PKT_DATA_ICC_PROFILE,                AV_FRAME_DATA_ICC_PROFILE },
        { AV_PKT_DATA_S12M_TIMECODE,              AV_FRAME_DATA_S12M_TIMECODE },
        { AV_PKT_DATA_DYNAMIC_HDR10_PLUS,         AV_FRAME_DATA_DYNAMIC_HDR_PLUS },
    };

    if (!(avctx->codec->caps_internal & FF_CODEC_CAP_SETS_FRAME_PROPS)) {
        frame->pts          = pkt->pts;
        frame->pkt_pos      = pkt->pos;
        frame->pkt_duration = pkt->duration;
        frame->pkt_size     = pkt->size;

        for (int i = 0; i < FF_ARRAY_ELEMS(pkt_to_frame_sd); i++) {
            size_t size;
            uint8_t *packet_sd = av_packet_get_side_data(pkt, pkt_to_frame_sd[i].packet, &size);
            if (packet_sd) {
                AVFrameSideData *frame_sd = av_frame_new_side_data(frame, pkt_to_frame_sd[i].frame, size);
                if (!frame_sd)
                    return AVERROR(ENOMEM);

                memcpy(frame_sd->data, packet_sd, size);
            }
        }
        internal_add_metadata_from_side_data(pkt, frame);

        if (pkt->flags & AV_PKT_FLAG_DISCARD) {
            frame->flags |= AV_FRAME_FLAG_DISCARD;
        } else {
            frame->flags = (frame->flags & ~AV_FRAME_FLAG_DISCARD);
        }
    }

    frame->reordered_opaque = avctx->reordered_opaque;

    if (frame->color_primaries == AVCOL_PRI_UNSPECIFIED)
        frame->color_primaries = avctx->color_primaries;
    if (frame->color_trc == AVCOL_TRC_UNSPECIFIED)
        frame->color_trc = avctx->color_trc;
    if (frame->colorspace == AVCOL_SPC_UNSPECIFIED)
        frame->colorspace = avctx->colorspace;
    if (frame->color_range == AVCOL_RANGE_UNSPECIFIED)
        frame->color_range = avctx->color_range;
    if (frame->chroma_location == AVCHROMA_LOC_UNSPECIFIED)
        frame->chroma_location = avctx->chroma_sample_location;

    switch (avctx->codec->type) {
    case AVMEDIA_TYPE_VIDEO:
        frame->format = avctx->pix_fmt;

        if (!frame->sample_aspect_ratio.num)
            frame->sample_aspect_ratio = avctx->sample_aspect_ratio;

        if (frame->width && frame->height &&
            av_image_check_sar(frame->width, frame->height,
                               frame->sample_aspect_ratio) < 0) {
            av_log(avctx, AV_LOG_WARNING, "ignoring invalid sample aspect ration (SAR): %u/%u\n",
                   frame->sample_aspect_ratio.num,
                   frame->sample_aspect_ratio.den);
            frame->sample_aspect_ratio = (AVRational){ 0, 1 };
        }

        break;

    case AVMEDIA_TYPE_AUDIO:
        if (!frame->sample_rate)
            frame->sample_rate = avctx->sample_rate;
        
        if (frame->format < 0)
            frame->format = avctx->sample_fmt;

        if (!frame->channel_layout) {
            if (avctx->channel_layout) {
                 if (av_get_channel_layout_nb_channels(avctx->channel_layout) !=
                     avctx->channels) {
                     av_log(avctx, AV_LOG_ERROR, "Inconsistent channel configuration.\n");
                     return AVERROR(EINVAL);
                 }

                frame->channel_layout = avctx->channel_layout;
            } else {
                if (avctx->channels > FF_SANE_NB_CHANNELS) {
                    av_log(avctx, AV_LOG_ERROR, "Too many channels: %d.\n",
                           avctx->channels);
                    return AVERROR(ENOSYS);
                }
            }
        }

        frame->channels = avctx->channels;
        break;
    }
    return 0;
}

int internal_get_format(AVCodecContext *avctx, const enum AVPixelFormat *fmt)
{
    const AVPixFmtDescriptor *desc;
    enum AVPixelFormat *choices;
    enum AVPixelFormat ret, user_choice;
    const AVCodecHWConfigInternal *hw_config;
    const AVCodecHWConfig *config;
    int i, n, err;

    // Find end of list.
    for (n = 0; fmt[n] != AV_PIX_FMT_NONE; n++);
    // Must contain at least one entry.
    av_assert0(n >= 1);
    // If a software format is available, it must be the last entry.
    desc = av_pix_fmt_desc_get(fmt[n - 1]);
    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        // No software format is available.
    } else {
        avctx->sw_pix_fmt = fmt[n - 1];
    }

    choices = av_memdup(fmt, (n + 1) * sizeof(*choices));
    if (!choices)
        return AV_PIX_FMT_NONE;

    for (;;) {
        user_choice = avctx->get_format(avctx, choices);
        if (user_choice == AV_PIX_FMT_NONE) {
            // Explicitly chose nothing, give up.
            ret = AV_PIX_FMT_NONE;
            break;
        }

        desc = av_pix_fmt_desc_get(user_choice);
        if (!desc) {
            av_log(avctx, AV_LOG_ERROR, "Invalid format returned by "
                   "get_format() callback.\n");
            ret = AV_PIX_FMT_NONE;
            break;
        }
        av_log(avctx, AV_LOG_DEBUG, "Format %s chosen by get_format().\n",
               desc->name);

        for (i = 0; i < n; i++) {
            if (choices[i] == user_choice)
                break;
        }
        if (i == n) {
            av_log(avctx, AV_LOG_ERROR, "Invalid return from get_format(): "
                   "%s not in possible list.\n", desc->name);
            ret = AV_PIX_FMT_NONE;
            break;
        }

        if (avctx->codec->hw_configs) {
            for (i = 0;; i++) {
                hw_config = avctx->codec->hw_configs[i];
                if (!hw_config)
                    break;
                if (hw_config->public.pix_fmt == user_choice)
                    break;
            }
        } else {
            hw_config = NULL;
        }

        if (!hw_config) {
            // No config available, so no extra setup required.
            ret = user_choice;
            break;
        }
        config = &hw_config->public;

        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX && avctx->hw_frames_ctx) {
            const AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
            
            if (frames_ctx->format != user_choice) {
                av_log(avctx, AV_LOG_ERROR, "Invalid setup for format %s: "
                       "does not match the format of the provided frames context.\n", desc->name);
                goto try_again;
            }
        } else if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && avctx->hw_device_ctx) {
            const AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;

            if (device_ctx->type != config->device_type) {
                av_log(avctx, AV_LOG_ERROR, "Invalid setup for format %s: "
                       "does not match the type of the provided device context.\n", desc->name);
                goto try_again;
            }
        } else if (config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) {
            // Internal-only setup, no additional configuration.
        } else if (config->methods & AV_CODEC_HW_CONFIG_METHOD_AD_HOC) {
            // Some ad-hoc configuration we can't see and can't check.
        } else {
            av_log(avctx, AV_LOG_ERROR, "Invalid setup for format %s: missing configuration.\n", desc->name);
            goto try_again;
        }

        ret = user_choice;
        break;

    try_again:
        av_log(avctx, AV_LOG_DEBUG, "Format %s not usable, retrying get_format() without it.\n", desc->name);
        for (i = 0; i < n; i++) {
            if (choices[i] == user_choice)
                break;
        }
        for (; i + 1 < n; i++)
            choices[i] = choices[i + 1];
        --n;
    }

    av_freep(&choices);
    return ret;
}

static void internal_decode_data_free(void *opaque, uint8_t *data)
{
    FrameDecodeData *fdd = (FrameDecodeData*)data;

    if (fdd->post_process_opaque_free)
        fdd->post_process_opaque_free(fdd->post_process_opaque);

    av_freep(&fdd);
}

int internal_attach_decode_data(AVFrame *frame)
{
    AVBufferRef *fdd_buf;
    FrameDecodeData *fdd;

    av_assert1(!frame->private_ref);
    av_buffer_unref(&frame->private_ref);

    fdd = av_mallocz(sizeof(*fdd));
    if (!fdd)
        return AVERROR(ENOMEM);

    fdd_buf = av_buffer_create((uint8_t*)fdd, sizeof(*fdd), internal_decode_data_free,
                               NULL, AV_BUFFER_FLAG_READONLY);
    if (!fdd_buf) {
        av_freep(&fdd);
        return AVERROR(ENOMEM);
    }

    frame->private_ref = fdd_buf;

    return 0;
}

static void internal_validate_avframe_allocation(AVCodecContext *avctx, AVFrame *frame)
{
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        int i;
        int num_planes = av_pix_fmt_count_planes(frame->format);
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
        int flags = desc ? desc->flags : 0;
        if (num_planes == 1 && (flags & AV_PIX_FMT_FLAG_PAL))
            num_planes = 2;
        for (i = 0; i < num_planes; i++) {
            av_assert0(frame->data[i]);
        }
        // For formats without data like hwaccel allow unused pointers to be non-NULL.
        for (i = num_planes; num_planes > 0 && i < FF_ARRAY_ELEMS(frame->data); i++) {
            if (frame->data[i])
                av_log(avctx, AV_LOG_ERROR, "Buffer returned by get_buffer2() did not zero unused plane pointers\n");
            frame->data[i] = NULL;
        }
    }
}

int internal_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    int override_dimensions = 1;
    int ret;

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        if ((unsigned)avctx->width > INT_MAX - STRIDE_ALIGN ||
            (ret = av_image_check_size2(FFALIGN(avctx->width, STRIDE_ALIGN), avctx->height, avctx->max_pixels, AV_PIX_FMT_NONE, 0, avctx)) < 0 || avctx->pix_fmt<0) {
            av_log(avctx, AV_LOG_ERROR, "video_get_buffer: image parameters invalid\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }

        if (frame->width <= 0 || frame->height <= 0) {
            frame->width  = FFMAX(avctx->width,  AV_CEIL_RSHIFT(avctx->coded_width,  avctx->lowres));
            frame->height = FFMAX(avctx->height, AV_CEIL_RSHIFT(avctx->coded_height, avctx->lowres));
            override_dimensions = 0;
        }

        if (frame->data[0] || frame->data[1] || frame->data[2] || frame->data[3]) {
            av_log(avctx, AV_LOG_ERROR, "pic->data[*]!=NULL in get_buffer_internal\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
    } else if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (frame->nb_samples * (int64_t)avctx->channels > avctx->max_samples) {
            av_log(avctx, AV_LOG_ERROR, "samples per frame %d, exceeds max_samples %"PRId64"\n", frame->nb_samples, avctx->max_samples);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }
    ret = internal_decode_frame_props(avctx, frame);
    if (ret < 0)
        goto fail;

    avctx->sw_pix_fmt = avctx->pix_fmt;

    ret = avctx->get_buffer2(avctx, frame, flags);
    if (ret < 0)
        goto fail;

    internal_validate_avframe_allocation(avctx, frame);

    ret = internal_attach_decode_data(frame);
    if (ret < 0)
        goto fail;

end:
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO && !override_dimensions &&
        !(avctx->codec->caps_internal & FF_CODEC_CAP_EXPORTS_CROPPING)) {
        frame->width  = avctx->width;
        frame->height = avctx->height;
    }

fail:
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        av_frame_unref(frame);
    }

    return ret;
}

static int init_com_mf(void* log)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        av_log(log, AV_LOG_ERROR, "COM must not be in STA mode\n");
        return AVERROR(EINVAL);
    }
    else if (FAILED(hr)) {
        av_log(log, AV_LOG_ERROR, "could not initialize COM\n");
        return AVERROR(ENOSYS);
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        av_log(log, AV_LOG_ERROR, "could not initialize MediaFoundation\n");
        CoUninitialize();
        return AVERROR(ENOSYS);
    }

    return 0;
}

static void uninit_com_mf(void)
{
    MFShutdown();
    CoUninitialize();
}

// Find and create a IMFTransform with the given input/output types. When done,
// you should use internal_free_mf() to destroy it, which will also uninit COM.
int internal_instantiate_mf(void* log,
    GUID category,
    MFT_REGISTER_TYPE_INFO* in_type,
    MFT_REGISTER_TYPE_INFO* out_type,
    int use_hw,
    IMFTransform** res)
{
    HRESULT hr;
    int n;
    int ret;
    IMFActivate** activate;
    UINT32 num_activate;
    IMFActivate* winner = 0;
    UINT32 flags;

    ret = init_com_mf(log);
    if (ret < 0)
        return ret;

    flags = MFT_ENUM_FLAG_SORTANDFILTER;

    if (use_hw) {
        flags |= MFT_ENUM_FLAG_HARDWARE;
    }
    else {
        flags |= MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT;
    }

    hr = MFTEnumEx(category, flags, in_type, out_type, &activate, &num_activate);
    if (FAILED(hr))
        goto error_uninit_mf;

    if (log) {
        if (!num_activate)
            av_log(log, AV_LOG_ERROR, "could not find any MFT for the given media type\n");

        for (n = 0; n < num_activate; n++) {
            av_log(log, AV_LOG_VERBOSE, "MF %d attributes:\n", n);
            internal_attributes_dump(log, (IMFAttributes*)activate[n]);
        }
    }

    *res = NULL;
    for (n = 0; n < num_activate; n++) {
        if (log)
            av_log(log, AV_LOG_VERBOSE, "activate MFT %d\n", n);
        hr = IMFActivate_ActivateObject(activate[n], &IID_IMFTransform,
            (void**)res);
        if (*res) {
            winner = activate[n];
            IMFActivate_AddRef(winner);
            break;
        }
    }

    for (n = 0; n < num_activate; n++)
        IMFActivate_Release(activate[n]);
    CoTaskMemFree(activate);

    if (!*res) {
        if (log)
            av_log(log, AV_LOG_ERROR, "could not create MFT\n");
        goto error_uninit_mf;
    }

    if (log) {
        wchar_t s[512]; // being lazy here
        IMFAttributes* attrs;
        hr = IMFTransform_GetAttributes(*res, &attrs);
        if (!FAILED(hr) && attrs) {

            av_log(log, AV_LOG_VERBOSE, "MFT attributes\n");
            internal_attributes_dump(log, attrs);
            IMFAttributes_Release(attrs);
        }

        hr = IMFActivate_GetString(winner, &MFT_FRIENDLY_NAME_Attribute, s,
            sizeof(s), NULL);
        if (!FAILED(hr))
            av_log(log, AV_LOG_INFO, "MFT name: '%ls'\n", s);

    }

    IMFActivate_Release(winner);

    return 0;

error_uninit_mf:
    uninit_com_mf();
    return AVERROR(ENOSYS);
}

void internal_free_mf(IMFTransform** mft)
{
    if (*mft)
        IMFTransform_Release(*mft);
    *mft = NULL;
    uninit_com_mf();
}

#pragma endregion

// RFJ (from Pawel Wegner path)
// Extend ffmpeg codec mapping for 
//  case AV_CODEC_ID_MSMPEG4V2:  return &ff_MFVideoFormat_MP42;

#pragma region ""ADAPTED_FROM_PAWELWEGNER""
static const CLSID* sx_ff_extended_codec_to_mf_subtype(enum AVCodecID id)
{
    const CLSID* subtype = internal_codec_to_mf_subtype(id);

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
                   internal_hr_str(hr));
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
        else {
            // What now???
            int a = -1;
        }

        sample = internal_create_memory_sample(tmp.data, tmp.size, c->in_info.cbAlignment);
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

    avctx->sample_fmt = internal_media_type_to_sample_fmt((IMFAttributes *)type);

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

    c->sw_format = internal_media_type_to_pix_fmt((IMFAttributes *)type);
    avctx->pix_fmt = c->sw_format;

    int res = internal_get_mf_attributes((IMFAttributes *)type, &MF_MT_FRAME_SIZE, &cw, &ch);
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

    res = internal_get_mf_attributes((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO, &t, &t2);
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

    if ((ret = internal_set_dimensions(avctx, cw, ch)) < 0)
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
    internal_attributes_dump(avctx, (IMFAttributes *)type);

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
        if (internal_fourcc_from_guid(&tg, &fourcc) >= 0 && fourcc == avctx->codec_tag)
            score = 2;
    }

    enum AVPixelFormat pix_fmt = internal_media_type_to_pix_fmt((IMFAttributes*)type);

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

    internal_set_mf_attributes((IMFAttributes *)type, &MF_MT_FRAME_SIZE, avctx->width, avctx->height);

    IMFMediaType_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);

    if (avctx->sample_aspect_ratio.num)
        internal_set_mf_attributes((IMFAttributes *)type, &MF_MT_PIXEL_ASPECT_RATIO,
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

    sample_fmt = internal_media_type_to_sample_fmt((IMFAttributes *)type);
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
    enum AVPixelFormat pix_fmt = internal_media_type_to_pix_fmt((IMFAttributes *)type);
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
            av_log(avctx, AV_LOG_ERROR, "error getting output type: %s\n", internal_hr_str(hr));
            ret = AVERROR_EXTERNAL;
            goto done;
        }

        av_log(avctx, AV_LOG_VERBOSE, "output type %d:\n", n);
        internal_attributes_dump(avctx, (IMFAttributes *)type);

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
        internal_attributes_dump(avctx, (IMFAttributes *)out_type);

        hr = IMFTransform_SetOutputType(c->mft, c->out_stream_id, out_type, 0);
        if (!FAILED(hr)) {
            ret = 1;
        } else if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "rejected - need to set input type\n");
            ret = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "could not set output type (%s)\n", internal_hr_str(hr));
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
            av_log(avctx, AV_LOG_ERROR, "error getting input type: %s\n", internal_hr_str(hr));
            ret = AVERROR_EXTERNAL;
            goto done;
        }

        av_log(avctx, AV_LOG_VERBOSE, "input type %d:\n", n);
        internal_attributes_dump(avctx, (IMFAttributes *)type);

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
        internal_attributes_dump(avctx, (IMFAttributes *)in_type);

        hr = IMFTransform_SetInputType(c->mft, c->in_stream_id, in_type, 0);
        if (!FAILED(hr)) {
            ret = 1;
        } else if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            av_log(avctx, AV_LOG_VERBOSE, "rejected - need to set output type\n");
            ret = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "could not set input type (%s)\n", internal_hr_str(hr));
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
        av_log(avctx, AV_LOG_ERROR, "error retrieving MFT attributes: %s\n", internal_hr_str(hr));
        goto err;
    }

    hr = IMFAttributes_GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &v);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "error querying async: %s\n", internal_hr_str(hr));
        goto err;
    }

    if (avctx->hw_device_ctx != NULL &&  !v) {
        av_log(avctx, AV_LOG_ERROR, "hardware MFT is not async\n");
        goto err;
    }

    hr = IMFAttributes_SetUINT32(attrs, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not set async unlock: %s\n", internal_hr_str(hr));
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
    const CLSID *subtype = sx_ff_extended_codec_to_mf_subtype(codec->id);
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

    if ((ret = internal_instantiate_mf(log, category, &reg, NULL, use_hw, mft)) < 0)
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
    while (!IsDebuggerPresent()) Sleep(100);
#endif

    avctx->thread_count = 0;
    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);

    // RFJ setting up just enough of the codec to pass in for mf_create to init for us
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
    
    // sx_ff_extended_codec_to_mf_subtype(avctx->codec_id);  // this method is also created as part of Pawel Wegner's work
    const CLSID* subtype = internal_codec_to_mf_subtype(avctx->codec_id);  
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
        av_log(avctx, AV_LOG_ERROR, "could not get stream IDs (%s)\n", internal_hr_str(hr));
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
        av_log(avctx, AV_LOG_ERROR, "could not start streaming (%s)\n", internal_hr_str(hr));
        return AVERROR_EXTERNAL;
    }
    hr = IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "could not start stream (%s)\n", internal_hr_str(hr));
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

    if ((ret = internal_get_buffer(avctx, frame, 0)) < 0)
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

static void fill_yuv_image(uint8_t* data[4], int linesize[4],
    int width, int height, int frame_index)
{
    int x, y;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            data[0][y * linesize[0] + x] = x + y + frame_index * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            data[1][y * linesize[1] + x] = 128 + y + frame_index * 2;
            data[2][y * linesize[2] + x] = 64 + x + frame_index * 5;
        }
    }
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

        // av_buffer_unref(frame);
        // av_free(ref);

        return 0;
    }

    return -1;
}

static int mf_copy_imfsample_to_avframe(AVCodecContext* avctx, IMFSample* sample, AVFrame* frame, BOOL ref_only)
{
    MFDecoderContext* c = avctx->priv_data;
    DWORD bufferCount;
    IMFMediaBuffer* media_buffer;
    DWORD buff_len;
    HRESULT hr = IMFSample_ConvertToContiguousBuffer(sample, &media_buffer);
    int ret;

    if (!FAILED(hr)) {
        BYTE* data;
        hr = IMFMediaBuffer_Lock(media_buffer, &data, NULL, &buff_len);

        if (FAILED(hr)) {
            IMFMediaBuffer_Release(media_buffer);
            return AVERROR_EXTERNAL;
        }

#if defined(DEBUG)
        int d0t1, d0t2;
        d0t1 = frame->data[1] - frame->data[0];
        d0t2 = frame->data[2] - frame->data[0];
#endif

        if (!ref_only) {
            // copy vs reference to sample buffer
            memcpy(frame->data[0], data, buff_len);

            IMFMediaBuffer_Unlock(media_buffer);
            IMFMediaBuffer_Release(media_buffer);
        }
        else {
            // Set the data pointers to the MFT locked sample buffer
            // released later see: mf_free_media_buffer_ref and post_process_opaque
            // NOTE: I am not completely sure this is the best approach to defer the buffer unlock and free,
            //       but it seemed like an appropriate convention to use.
            frame->data[0] = data;
        }

#if defined(DEBUG)
        int sz0 = d0t1;
        int sz1 = d0t2 - d0t1;
        int sz2 = buff_len - d0t2;

        int frame_planes = av_pix_fmt_count_planes(frame->format);
        int avctx_planes = av_pix_fmt_count_planes(avctx->sw_pix_fmt);
        int avctx_pix_planes = av_pix_fmt_count_planes(avctx->pix_fmt);

        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(frame->format);

        int hshift, vshift;
        ret = av_pix_fmt_get_chroma_sub_sample(avctx->pix_fmt, &hshift, &vshift);

        int plane_linesz[4] = { 0 };

        for (int plane = 0; plane < frame_planes; plane++) {
            plane_linesz[plane] = av_image_get_linesize(frame->format, frame->width, plane);
        }

        av_log(avctx, AV_LOG_VERBOSE, "mf_copy_imfsample_to_avframe - plane sizes: Y (%d), U (%d), V (%d)\n"
            "                               image sizes: WxH (%d), W/2 (%d), H/2 (%d), P (%d)\n"
            "                               buffer difference: Len-Parts: (%d)\n"
            "                               format: %s / %s\n"
            "                               chroma subsample shifts: Horz (%d), Vert (%d)\n"
            "                               line sizes: Y (%d), U (%d), V (%d)\n",
            sz0, sz1, sz2,
            frame->width, frame->width / 2, frame->height / 2, frame_planes,
            buff_len - sz0 - sz1 - sz2,
            desc->name, desc->alias,
            hshift, vshift,
            plane_linesz[0], plane_linesz[1], plane_linesz[2]);
#endif

        if (ref_only) {
            FrameDecodeData* fdd = (FrameDecodeData*)frame->private_ref->data;
            MFFrameRef ref = {
                .mf_buffer = media_buffer,
                .avcodec_ref = avctx,
            };

            fdd->post_process_opaque = av_buffer_ref(&ref);
            fdd->post_process_opaque_free = mf_free_media_buffer_ref;
        }
    }

    return ret;
}

static int mf_sample_to_v_avframe(AVCodecContext *avctx, IMFSample *sample, AVFrame *frame)
{
    int ret = 0;
    av_frame_unref(frame);

    if ((ret = internal_get_buffer(avctx, frame, 0)) >= 0) {
        frame->width = avctx->width;
        frame->height = avctx->height;
        frame->format = avctx->pix_fmt;
        frame->channels = avctx->channels;
        frame->channel_layout = avctx->channel_layout;

        // Get the AVFrame buffer and copy/assign the MF sample buffer content
        // NOTE: Setting the 'ref_only' parameter uses the locked IMFMediaSample buffer directly without a memcpy
        //       !!! It is the ref_only mode where I am seeing bad chroma handing in HEVC decoded frames, aka "green tinted" rendering
        if ((ret = av_frame_get_buffer(frame, 0)) >= 0) {
            ret = mf_copy_imfsample_to_avframe(avctx, sample, frame, TRUE); 

            // logging result and chroma location from earlier debugging
            av_log(avctx, AV_LOG_VERBOSE, "mf_sample_to_v_avframe - chroma-loc: %s (%d), result: %d\n", 
                av_chroma_location_name(frame->chroma_location), frame->chroma_location, ret);
        }
    }

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
            sample = internal_create_memory_sample(NULL, c->out_info.cbSize, c->out_info.cbAlignment);
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
            av_log(avctx, AV_LOG_ERROR, "failed processing output: %s\n", internal_hr_str(hr));
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
            ret = internal_decode_get_packet(avctx, &packet);
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
                av_log(avctx, AV_LOG_ERROR, "failed processing input: %s\n", internal_hr_str(hr));
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
            av_log(avctx, AV_LOG_ERROR, "failed draining: %s\n", internal_hr_str(hr));

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

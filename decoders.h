/*
 * This file is part of sxplayer.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * sxplayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * sxplayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with sxplayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef SXPLAYER_DECODERS_H
#define SXPLAYER_DECODERS_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

struct decoding_ctx;

struct decoder_ctx {
    void *log_ctx;
    AVCodecContext *avctx;
    const struct decoder *dec;
    void *priv_data;
    struct decoding_ctx *decoding_ctx;
    void *opaque;
    int use_hwaccel;
    int max_pixels;
    const char *vt_pix_fmt;
};

struct decoder {
    const char *name;
    int (*init)(struct decoder_ctx *ctx);
    void (*uninit)(struct decoder_ctx *ctx);
    int (*push_packet)(struct decoder_ctx *ctx, const AVPacket *pkt);
    void (*flush)(struct decoder_ctx *ctx);
    int priv_data_size;
};

struct decoder_ctx *decoder_alloc(void);
int decoder_init(void *log_ctx,
                 struct decoder_ctx *ctx,
                 const struct decoder *dec,
                 const AVStream *stream,
                 struct decoding_ctx *decoding_ctx,
                 void *opaque,
                 int max_pixels,
                 const char *vt_pix_fmt);
int decoder_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt);
void decoder_flush(struct decoder_ctx *ctx);
void decoder_free(struct decoder_ctx **ctxp);

#endif

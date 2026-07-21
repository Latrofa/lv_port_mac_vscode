/**
 * @file frame_store.c
 * @brief Random-access decode from MJPEG proxy into LVGL draw buffers.
 */

#include "frame_store.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct frame_store_t {
    video_asset_t asset;
    AVFormatContext * fmt_ctx;
    AVCodecContext * dec_ctx;
    AVStream * stream;
    int stream_idx;
    AVFrame * frame;
    AVPacket * pkt;
    struct SwsContext * sws;
    int sws_src_w;
    int sws_src_h;
    int sws_dst_w;
    int sws_dst_h;
    enum AVPixelFormat sws_src_fmt;
    int32_t last_frame_id;
};

static int open_decoder(frame_store_t * s)
{
    int ret = avformat_open_input(&s->fmt_ctx, s->asset.proxy_path, NULL, NULL);
    if(ret < 0) return ret;
    ret = avformat_find_stream_info(s->fmt_ctx, NULL);
    if(ret < 0) return ret;

    s->stream_idx = av_find_best_stream(s->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if(s->stream_idx < 0) return s->stream_idx;

    s->stream = s->fmt_ctx->streams[s->stream_idx];
    const AVCodec * dec = avcodec_find_decoder(s->stream->codecpar->codec_id);
    if(!dec) return AVERROR_DECODER_NOT_FOUND;

    s->dec_ctx = avcodec_alloc_context3(dec);
    if(!s->dec_ctx) return AVERROR(ENOMEM);
    avcodec_parameters_to_context(s->dec_ctx, s->stream->codecpar);
    ret = avcodec_open2(s->dec_ctx, dec, NULL);
    if(ret < 0) return ret;

    s->frame = av_frame_alloc();
    s->pkt = av_packet_alloc();
    s->last_frame_id = -1;
    return 0;
}

frame_store_t * frame_store_open(const video_asset_t * asset)
{
    if(!asset || !asset->valid) return NULL;

    frame_store_t * s = calloc(1, sizeof(*s));
    if(!s) return NULL;
    s->asset = *asset;

    if(open_decoder(s) < 0) {
        fprintf(stderr, "frame_store: failed to open %s\n", asset->proxy_path);
        frame_store_close(s);
        return NULL;
    }
    return s;
}

void frame_store_close(frame_store_t * store)
{
    if(!store) return;
    if(store->sws) sws_freeContext(store->sws);
    if(store->frame) av_frame_free(&store->frame);
    if(store->pkt) av_packet_free(&store->pkt);
    if(store->dec_ctx) avcodec_free_context(&store->dec_ctx);
    if(store->fmt_ctx) avformat_close_input(&store->fmt_ctx);
    free(store);
}

int32_t frame_store_frame_count(const frame_store_t * store)
{
    return store ? store->asset.frame_count : 0;
}

static int seek_to_frame(frame_store_t * s, int32_t frame_id)
{
    if(frame_id < 0) frame_id = 0;
    if(s->asset.frame_count > 0 && frame_id >= s->asset.frame_count) {
        frame_id = s->asset.frame_count - 1;
    }

    /* Sequential fast path: decode forward if close ahead */
    if(s->last_frame_id >= 0 && frame_id >= s->last_frame_id &&
       frame_id - s->last_frame_id <= 8) {
        return 0; /* caller will decode forward */
    }

    double fps = s->asset.fps > 0.1 ? s->asset.fps : 30.0;
    double sec = (double)frame_id / fps;
    int64_t ts = (int64_t)(sec / av_q2d(s->stream->time_base));

    int ret = av_seek_frame(s->fmt_ctx, s->stream_idx, ts, AVSEEK_FLAG_BACKWARD);
    if(ret < 0) {
        /* Try byte seek from start and walk */
        ret = av_seek_frame(s->fmt_ctx, s->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    }
    avcodec_flush_buffers(s->dec_ctx);
    s->last_frame_id = -1;
    return ret;
}

static int decode_next_video_frame(frame_store_t * s)
{
    while(1) {
        int ret = av_read_frame(s->fmt_ctx, s->pkt);
        if(ret < 0) return ret;

        if(s->pkt->stream_index != s->stream_idx) {
            av_packet_unref(s->pkt);
            continue;
        }

        ret = avcodec_send_packet(s->dec_ctx, s->pkt);
        av_packet_unref(s->pkt);
        if(ret < 0) continue;

        ret = avcodec_receive_frame(s->dec_ctx, s->frame);
        if(ret == AVERROR(EAGAIN)) continue;
        if(ret < 0) return ret;

        if(s->last_frame_id < 0) s->last_frame_id = 0;
        else s->last_frame_id++;
        return 0;
    }
}

static int decode_until_frame(frame_store_t * s, int32_t frame_id)
{
    if(s->last_frame_id == frame_id && s->frame->data[0]) {
        return 0;
    }

    if(s->last_frame_id < 0 || frame_id < s->last_frame_id ||
       frame_id - s->last_frame_id > 8) {
        if(seek_to_frame(s, frame_id) < 0) {
            fprintf(stderr, "frame_store: seek failed for frame %d\n", frame_id);
        }
        /* After seek we don't know exact frame; walk until we have enough frames.
         * For MJPEG AVI, packets ≈ frames and seek lands on/near target. */
        int32_t target = frame_id;
        /* Re-seek and count from an estimated start */
        double fps = s->asset.fps > 0.1 ? s->asset.fps : 30.0;
        double sec = (double)frame_id / fps;
        int64_t ts = (int64_t)(sec / av_q2d(s->stream->time_base));
        av_seek_frame(s->fmt_ctx, s->stream_idx, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(s->dec_ctx);
        s->last_frame_id = -1;

        /* Decode one frame first (nearest key/I frame — all MJPEG are I) */
        if(decode_next_video_frame(s) < 0) return -1;

        /* Estimate current frame from PTS if available */
        if(s->frame->pts != AV_NOPTS_VALUE) {
            double t = s->frame->pts * av_q2d(s->stream->time_base);
            s->last_frame_id = (int32_t)(t * fps + 0.5);
        }
        else {
            s->last_frame_id = frame_id; /* best effort after seek */
        }

        /* Walk forward if we landed early */
        int guard = 0;
        while(s->last_frame_id < target && guard < 10000) {
            if(decode_next_video_frame(s) < 0) break;
            guard++;
        }
        return s->frame->data[0] ? 0 : -1;
    }

    /* Forward decode */
    while(s->last_frame_id < frame_id) {
        if(decode_next_video_frame(s) < 0) return -1;
    }
    return 0;
}

lv_draw_buf_t * frame_store_get_frame(frame_store_t * store, int32_t frame_id,
                                      int32_t out_w, int32_t out_h,
                                      lv_draw_buf_t * reuse)
{
    if(!store || out_w <= 0 || out_h <= 0) return reuse;

    if(decode_until_frame(store, frame_id) < 0) {
        return reuse;
    }

    AVFrame * f = store->frame;
    enum AVPixelFormat src_fmt = (enum AVPixelFormat)f->format;

    /* Rebuild sws if geometry/format changed */
    if(!store->sws || store->sws_src_w != f->width || store->sws_src_h != f->height ||
       store->sws_dst_w != out_w || store->sws_dst_h != out_h ||
       store->sws_src_fmt != src_fmt) {
        if(store->sws) sws_freeContext(store->sws);
        /* BGRA matches LV_COLOR_FORMAT_ARGB8888 on little-endian hosts */
        store->sws = sws_getContext(f->width, f->height, src_fmt,
                                    out_w, out_h, AV_PIX_FMT_BGRA,
                                    SWS_BILINEAR, NULL, NULL, NULL);
        store->sws_src_w = f->width;
        store->sws_src_h = f->height;
        store->sws_dst_w = out_w;
        store->sws_dst_h = out_h;
        store->sws_src_fmt = src_fmt;
    }
    if(!store->sws) return reuse;

    lv_draw_buf_t * buf = reuse;
    if(!buf || buf->header.w != (uint32_t)out_w || buf->header.h != (uint32_t)out_h) {
        if(buf) lv_draw_buf_destroy(buf);
        buf = lv_draw_buf_create(out_w, out_h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
        if(!buf) return NULL;
    }

    uint8_t * dst_data[4] = { buf->data, NULL, NULL, NULL };
    int dst_linesize[4] = { (int)buf->header.stride, 0, 0, 0 };

    /* Clear to black then scale (letterbox not applied — full stretch to out_w/h;
     * caller can pass aspect-correct sizes). */
    memset(buf->data, 0, buf->data_size);
    sws_scale(store->sws, (const uint8_t * const *)f->data, f->linesize,
              0, f->height, dst_data, dst_linesize);

    return buf;
}

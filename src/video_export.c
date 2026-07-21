/**
 * @file video_export.c
 * @brief Re-encode a frame range from the source (or proxy) into MP4.
 */

#include "video_export.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static void ensure_parent_dir(const char * path)
{
    char tmp[VIDEO_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char * slash = strrchr(tmp, '/');
    if(!slash) return;
    *slash = '\0';
    struct stat st;
    if(stat(tmp, &st) != 0) {
        mkdir(tmp, 0755);
    }
}

static const AVCodec * find_video_encoder(enum AVCodecID * out_id)
{
    const AVCodec * c = avcodec_find_encoder_by_name("libx264");
    if(c) {
        *out_id = AV_CODEC_ID_H264;
        return c;
    }
    c = avcodec_find_encoder(AV_CODEC_ID_H264);
    if(c) {
        *out_id = AV_CODEC_ID_H264;
        return c;
    }
    c = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if(c) {
        *out_id = AV_CODEC_ID_MPEG4;
        return c;
    }
    return NULL;
}

bool video_export_clip(const video_asset_t * asset,
                       int32_t start_frame, int32_t end_frame,
                       const char * out_path,
                       video_export_progress_cb progress_cb,
                       void * user_data)
{
    if(!asset || !asset->valid || !out_path) return false;
    if(end_frame < start_frame) return false;
    if(start_frame < 0) start_frame = 0;
    if(asset->frame_count > 0 && end_frame >= asset->frame_count) {
        end_frame = asset->frame_count - 1;
    }

    ensure_parent_dir(out_path);

    /* Prefer original source for quality; fall back to proxy. */
    const char * in_path = asset->src_path;
    struct stat st;
    if(stat(in_path, &st) != 0) {
        in_path = asset->proxy_path;
    }

    AVFormatContext * ifmt = NULL;
    if(avformat_open_input(&ifmt, in_path, NULL, NULL) < 0) {
        fprintf(stderr, "video_export: cannot open %s\n", in_path);
        return false;
    }
    avformat_find_stream_info(ifmt, NULL);

    int vstream = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if(vstream < 0) {
        avformat_close_input(&ifmt);
        return false;
    }

    AVStream * ist = ifmt->streams[vstream];
    const AVCodec * dec = avcodec_find_decoder(ist->codecpar->codec_id);
    AVCodecContext * dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, ist->codecpar);
    if(avcodec_open2(dec_ctx, dec, NULL) < 0) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    double fps = asset->fps > 0.1 ? asset->fps : 30.0;
    int src_w = dec_ctx->width;
    int src_h = dec_ctx->height;
    int dst_w = src_w & ~1;
    int dst_h = src_h & ~1;
    if(dst_w < 2) dst_w = 2;
    if(dst_h < 2) dst_h = 2;

    enum AVCodecID enc_id = AV_CODEC_ID_MPEG4;
    const AVCodec * enc = find_video_encoder(&enc_id);
    if(!enc) {
        fprintf(stderr, "video_export: no video encoder available\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    AVFormatContext * ofmt = NULL;
    if(avformat_alloc_output_context2(&ofmt, NULL, "mp4", out_path) < 0 || !ofmt) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    AVStream * ost = avformat_new_stream(ofmt, NULL);
    AVCodecContext * enc_ctx = avcodec_alloc_context3(enc);
    enc_ctx->width = dst_w;
    enc_ctx->height = dst_h;
    enc_ctx->time_base = av_inv_q(av_d2q(fps, 100000));
    enc_ctx->framerate = av_d2q(fps, 100000);
    enc_ctx->bit_rate = 2 * 1000 * 1000;
    enc_ctx->gop_size = 12;
    enc_ctx->max_b_frames = 0;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if(ofmt->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if(enc_id == AV_CODEC_ID_H264) {
        av_opt_set(enc_ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(enc_ctx->priv_data, "crf", "23", 0);
    }

    if(avcodec_open2(enc_ctx, enc, NULL) < 0) {
        fprintf(stderr, "video_export: cannot open encoder\n");
        avcodec_free_context(&enc_ctx);
        avformat_free_context(ofmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }
    avcodec_parameters_from_context(ost->codecpar, enc_ctx);
    ost->time_base = enc_ctx->time_base;

    if(!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        if(avio_open(&ofmt->pb, out_path, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "video_export: cannot open %s for write\n", out_path);
            avcodec_free_context(&enc_ctx);
            avformat_free_context(ofmt);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&ifmt);
            return false;
        }
    }
    if(avformat_write_header(ofmt, NULL) < 0) {
        fprintf(stderr, "video_export: write_header failed\n");
        if(!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avcodec_free_context(&enc_ctx);
        avformat_free_context(ofmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    struct SwsContext * sws = sws_getContext(
        src_w, src_h, dec_ctx->pix_fmt,
        dst_w, dst_h, enc_ctx->pix_fmt,
        SWS_BILINEAR, NULL, NULL, NULL);

    AVFrame * frame = av_frame_alloc();
    AVFrame * yuv = av_frame_alloc();
    AVPacket * ipkt = av_packet_alloc();
    AVPacket * opkt = av_packet_alloc();
    yuv->format = enc_ctx->pix_fmt;
    yuv->width = dst_w;
    yuv->height = dst_h;
    av_frame_get_buffer(yuv, 32);

    /* Seek to start */
    double start_sec = (double)start_frame / fps;
    int64_t ts = (int64_t)(start_sec / av_q2d(ist->time_base));
    av_seek_frame(ifmt, vstream, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    int32_t total = end_frame - start_frame + 1;
    int32_t written = 0;
    int64_t pts = 0;
    int32_t decoded_idx = -1;
    bool ok = true;

    /* Walk from seek point; count frames until we pass end.
     * For accuracy we estimate index from PTS when available. */
    while(av_read_frame(ifmt, ipkt) >= 0 && ok) {
        if(ipkt->stream_index != vstream) {
            av_packet_unref(ipkt);
            continue;
        }
        if(avcodec_send_packet(dec_ctx, ipkt) < 0) {
            av_packet_unref(ipkt);
            continue;
        }
        av_packet_unref(ipkt);

        while(avcodec_receive_frame(dec_ctx, frame) >= 0) {
            int32_t cur;
            if(frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                double t = frame->best_effort_timestamp * av_q2d(ist->time_base);
                cur = (int32_t)(t * fps + 0.5);
            }
            else if(frame->pts != AV_NOPTS_VALUE) {
                double t = frame->pts * av_q2d(ist->time_base);
                cur = (int32_t)(t * fps + 0.5);
            }
            else {
                decoded_idx++;
                cur = decoded_idx;
            }

            if(cur < start_frame) {
                av_frame_unref(frame);
                continue;
            }
            if(cur > end_frame) {
                av_frame_unref(frame);
                ok = true;
                goto done_loop;
            }

            sws_scale(sws, (const uint8_t * const *)frame->data, frame->linesize,
                      0, src_h, yuv->data, yuv->linesize);
            yuv->pts = pts++;

            if(avcodec_send_frame(enc_ctx, yuv) >= 0) {
                while(avcodec_receive_packet(enc_ctx, opkt) >= 0) {
                    av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
                    opkt->stream_index = ost->index;
                    av_interleaved_write_frame(ofmt, opkt);
                    av_packet_unref(opkt);
                }
            }
            written++;
            if(progress_cb && total > 0) {
                int pct = (int)((written * 100) / total);
                if(pct > 100) pct = 100;
                progress_cb(pct, user_data);
            }
            av_frame_unref(frame);
        }
    }
done_loop:

    /* Flush encoder */
    avcodec_send_frame(enc_ctx, NULL);
    while(avcodec_receive_packet(enc_ctx, opkt) >= 0) {
        av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
        opkt->stream_index = ost->index;
        av_interleaved_write_frame(ofmt, opkt);
        av_packet_unref(opkt);
    }
    av_write_trailer(ofmt);

    if(progress_cb) progress_cb(100, user_data);

    printf("video_export: wrote %s (%d frames, range %d-%d)\n",
           out_path, written, start_frame, end_frame);

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_frame_free(&yuv);
    av_packet_free(&ipkt);
    av_packet_free(&opkt);
    if(!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avcodec_free_context(&enc_ctx);
    avformat_free_context(ofmt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&ifmt);

    return written > 0;
}

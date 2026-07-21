/**
 * @file video_proxy.c
 * @brief Decode MP4 and write an MJPEG AVI proxy for frame-accurate scrubbing.
 */

#include "video_proxy.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define CACHE_DIR "cache"
#define DEFAULT_FPS 30.0
#define MAX_PROXY_FPS 30.0

static void ensure_cache_dir(void)
{
    struct stat st;
    if(stat(CACHE_DIR, &st) != 0) {
        if(mkdir(CACHE_DIR, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "video_proxy: cannot create %s: %s\n", CACHE_DIR, strerror(errno));
        }
    }
}

static void basename_no_ext(const char * path, char * out, size_t out_sz)
{
    const char * base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(out, base, out_sz - 1);
    out[out_sz - 1] = '\0';
    char * dot = strrchr(out, '.');
    if(dot) *dot = '\0';
}

static bool file_exists_nonempty(const char * path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

void video_proxy_format_time(const video_asset_t * asset, int32_t frame, char * buf, size_t buf_sz)
{
    if(!asset || !buf || buf_sz == 0) return;
    double fps = asset->fps > 0.1 ? asset->fps : DEFAULT_FPS;
    double sec = (double)frame / fps;
    if(sec < 0) sec = 0;
    int total_ms = (int)(sec * 1000.0 + 0.5);
    int mm = total_ms / 60000;
    int ss = (total_ms / 1000) % 60;
    int d = (total_ms / 100) % 10;
    snprintf(buf, buf_sz, "%d:%02d.%d", mm, ss, d);
}

static int open_input(const char * path, AVFormatContext ** fmt_ctx,
                      AVCodecContext ** dec_ctx, int * stream_idx, double * fps_out)
{
    int ret = avformat_open_input(fmt_ctx, path, NULL, NULL);
    if(ret < 0) {
        fprintf(stderr, "video_proxy: open_input failed: %s\n", path);
        return ret;
    }
    ret = avformat_find_stream_info(*fmt_ctx, NULL);
    if(ret < 0) return ret;

    *stream_idx = av_find_best_stream(*fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if(*stream_idx < 0) return *stream_idx;

    AVStream * st = (*fmt_ctx)->streams[*stream_idx];
    const AVCodec * dec = avcodec_find_decoder(st->codecpar->codec_id);
    if(!dec) return AVERROR_DECODER_NOT_FOUND;

    *dec_ctx = avcodec_alloc_context3(dec);
    if(!*dec_ctx) return AVERROR(ENOMEM);
    avcodec_parameters_to_context(*dec_ctx, st->codecpar);
    ret = avcodec_open2(*dec_ctx, dec, NULL);
    if(ret < 0) return ret;

    double fps = 0;
    if(st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
        fps = av_q2d(st->avg_frame_rate);
    }
    else if(st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
        fps = av_q2d(st->r_frame_rate);
    }
    if(fps < 0.1 || fps > 120.0) fps = DEFAULT_FPS;
    if(fps > MAX_PROXY_FPS) fps = MAX_PROXY_FPS;
    *fps_out = fps;
    return 0;
}

bool video_proxy_build(const char * src_path, int max_w, video_asset_t * out)
{
    if(!src_path || !out) return false;
    memset(out, 0, sizeof(*out));
    strncpy(out->src_path, src_path, VIDEO_PATH_MAX - 1);

    ensure_cache_dir();

    char base[256];
    basename_no_ext(src_path, base, sizeof(base));
    snprintf(out->proxy_path, VIDEO_PATH_MAX, "%s/%s_proxy.avi", CACHE_DIR, base);

    AVFormatContext * ifmt = NULL;
    AVCodecContext * dec_ctx = NULL;
    int vstream = -1;
    double fps = DEFAULT_FPS;

    if(open_input(src_path, &ifmt, &dec_ctx, &vstream, &fps) < 0) {
        fprintf(stderr, "video_proxy: failed to open source %s\n", src_path);
        if(dec_ctx) avcodec_free_context(&dec_ctx);
        if(ifmt) avformat_close_input(&ifmt);
        return false;
    }

    int src_w = dec_ctx->width;
    int src_h = dec_ctx->height;
    int dst_w = src_w;
    int dst_h = src_h;
    if(max_w > 0 && src_w > max_w) {
        dst_w = max_w;
        dst_h = (int)((double)src_h * max_w / src_w);
        if(dst_h < 2) dst_h = 2;
        /* even dimensions for encoders */
        dst_w &= ~1;
        dst_h &= ~1;
    }

    out->fps = fps;
    out->width = dst_w;
    out->height = dst_h;

    if(ifmt->duration > 0) {
        out->duration_us = ifmt->duration; /* AV_TIME_BASE units ≈ µs */
    }

    /* Reuse existing proxy if present and non-empty (simple cache). */
    if(file_exists_nonempty(out->proxy_path)) {
        /* Probe frame count from existing proxy */
        AVFormatContext * pfmt = NULL;
        if(avformat_open_input(&pfmt, out->proxy_path, NULL, NULL) >= 0) {
            avformat_find_stream_info(pfmt, NULL);
            int pi = av_find_best_stream(pfmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
            if(pi >= 0) {
                AVStream * pst = pfmt->streams[pi];
                if(pst->nb_frames > 0) {
                    out->frame_count = (int32_t)pst->nb_frames;
                }
                else if(pst->duration > 0 && pst->time_base.den > 0) {
                    double sec = pst->duration * av_q2d(pst->time_base);
                    out->frame_count = (int32_t)(sec * fps + 0.5);
                }
            }
            avformat_close_input(&pfmt);
        }
        if(out->frame_count <= 0 && out->duration_us > 0) {
            out->frame_count = (int32_t)((out->duration_us / 1000000.0) * fps + 0.5);
        }
        if(out->frame_count <= 0) out->frame_count = 1;

        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        out->valid = true;
        printf("video_proxy: reusing cache %s (%d frames, %.2f fps, %dx%d)\n",
               out->proxy_path, out->frame_count, out->fps, out->width, out->height);
        return true;
    }

    /* --- Encoder setup --- */
    AVFormatContext * ofmt = NULL;
    int ret = avformat_alloc_output_context2(&ofmt, NULL, "avi", out->proxy_path);
    if(ret < 0 || !ofmt) {
        fprintf(stderr, "video_proxy: cannot alloc output\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    const AVCodec * enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if(!enc) {
        fprintf(stderr, "video_proxy: MJPEG encoder not found\n");
        avformat_free_context(ofmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    AVStream * ost = avformat_new_stream(ofmt, NULL);
    AVCodecContext * enc_ctx = avcodec_alloc_context3(enc);
    if(!ost || !enc_ctx) {
        avcodec_free_context(&enc_ctx);
        avformat_free_context(ofmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    enc_ctx->width = dst_w;
    enc_ctx->height = dst_h;
    enc_ctx->time_base = av_inv_q(av_d2q(fps, 100000));
    enc_ctx->framerate = av_d2q(fps, 100000);
    enc_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc_ctx->color_range = AVCOL_RANGE_JPEG;
    if(ofmt->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(enc_ctx, enc, NULL);
    if(ret < 0) {
        /* Fallback pix fmt */
        enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ret = avcodec_open2(enc_ctx, enc, NULL);
    }
    if(ret < 0) {
        fprintf(stderr, "video_proxy: cannot open MJPEG encoder\n");
        avcodec_free_context(&enc_ctx);
        avformat_free_context(ofmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&ifmt);
        return false;
    }

    avcodec_parameters_from_context(ost->codecpar, enc_ctx);
    ost->time_base = enc_ctx->time_base;

    if(!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt->pb, out->proxy_path, AVIO_FLAG_WRITE);
        if(ret < 0) {
            fprintf(stderr, "video_proxy: cannot open output file %s\n", out->proxy_path);
            avcodec_free_context(&enc_ctx);
            avformat_free_context(ofmt);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&ifmt);
            return false;
        }
    }

    ret = avformat_write_header(ofmt, NULL);
    if(ret < 0) {
        fprintf(stderr, "video_proxy: write_header failed\n");
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
    AVFrame * scaled = av_frame_alloc();
    AVPacket * ipkt = av_packet_alloc();
    AVPacket * opkt = av_packet_alloc();

    scaled->format = enc_ctx->pix_fmt;
    scaled->width = dst_w;
    scaled->height = dst_h;
    av_frame_get_buffer(scaled, 32);

    int64_t pts = 0;
    int32_t frame_count = 0;
    bool ok = true;

    while(av_read_frame(ifmt, ipkt) >= 0) {
        if(ipkt->stream_index != vstream) {
            av_packet_unref(ipkt);
            continue;
        }
        ret = avcodec_send_packet(dec_ctx, ipkt);
        av_packet_unref(ipkt);
        if(ret < 0) continue;

        while(ret >= 0) {
            ret = avcodec_receive_frame(dec_ctx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if(ret < 0) {
                ok = false;
                break;
            }

            sws_scale(sws, (const uint8_t * const *)frame->data, frame->linesize,
                      0, src_h, scaled->data, scaled->linesize);
            scaled->pts = pts++;

            ret = avcodec_send_frame(enc_ctx, scaled);
            if(ret < 0) {
                ok = false;
                break;
            }
            while(ret >= 0) {
                ret = avcodec_receive_packet(enc_ctx, opkt);
                if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if(ret < 0) {
                    ok = false;
                    break;
                }
                av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
                opkt->stream_index = ost->index;
                av_interleaved_write_frame(ofmt, opkt);
                av_packet_unref(opkt);
            }
            frame_count++;
            av_frame_unref(frame);
        }
        if(!ok) break;
    }

    /* Flush decoder */
    avcodec_send_packet(dec_ctx, NULL);
    while(avcodec_receive_frame(dec_ctx, frame) >= 0) {
        sws_scale(sws, (const uint8_t * const *)frame->data, frame->linesize,
                  0, src_h, scaled->data, scaled->linesize);
        scaled->pts = pts++;
        if(avcodec_send_frame(enc_ctx, scaled) >= 0) {
            while(avcodec_receive_packet(enc_ctx, opkt) >= 0) {
                av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
                opkt->stream_index = ost->index;
                av_interleaved_write_frame(ofmt, opkt);
                av_packet_unref(opkt);
            }
        }
        frame_count++;
        av_frame_unref(frame);
    }

    /* Flush encoder */
    avcodec_send_frame(enc_ctx, NULL);
    while(avcodec_receive_packet(enc_ctx, opkt) >= 0) {
        av_packet_rescale_ts(opkt, enc_ctx->time_base, ost->time_base);
        opkt->stream_index = ost->index;
        av_interleaved_write_frame(ofmt, opkt);
        av_packet_unref(opkt);
    }

    av_write_trailer(ofmt);

    out->frame_count = frame_count > 0 ? frame_count : 1;
    out->valid = ok && frame_count > 0;

    printf("video_proxy: wrote %s (%d frames, %.2f fps, %dx%d)\n",
           out->proxy_path, out->frame_count, out->fps, out->width, out->height);

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_frame_free(&scaled);
    av_packet_free(&ipkt);
    av_packet_free(&opkt);
    if(!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avcodec_free_context(&enc_ctx);
    avformat_free_context(ofmt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&ifmt);

    if(!out->valid) {
        remove(out->proxy_path);
        return false;
    }
    return true;
}

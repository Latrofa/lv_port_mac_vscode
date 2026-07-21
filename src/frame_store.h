/**
 * @file frame_store.h
 * @brief Random-access frame extraction from an MJPEG proxy.
 */

#ifndef FRAME_STORE_H
#define FRAME_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "lvgl/lvgl.h"
#include "video_proxy.h"

typedef struct frame_store_t frame_store_t;

/**
 * Open a proxy for random-access frame reads.
 */
frame_store_t * frame_store_open(const video_asset_t * asset);

void frame_store_close(frame_store_t * store);

/**
 * Decode frame @p frame_id into an RGB draw buffer sized to @p out_w x @p out_h
 * (aspect preserved, letterboxed). Caller owns the returned buffer and must
 * destroy it with lv_draw_buf_destroy(), or pass the previous buffer via
 * @p reuse to avoid realloc each scrub tick.
 *
 * @param store     Open store
 * @param frame_id  Frame index [0, frame_count)
 * @param out_w     Desired width
 * @param out_h     Desired height
 * @param reuse     Optional existing buffer to recycle (may be reallocated)
 * @return draw buffer or NULL on failure
 */
lv_draw_buf_t * frame_store_get_frame(frame_store_t * store, int32_t frame_id,
                                      int32_t out_w, int32_t out_h,
                                      lv_draw_buf_t * reuse);

int32_t frame_store_frame_count(const frame_store_t * store);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_STORE_H */

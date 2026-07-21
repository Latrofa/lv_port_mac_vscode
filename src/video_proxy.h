/**
 * @file video_proxy.h
 * @brief MP4 ingest → scrubbable intermediate (MJPEG) proxy.
 */

#ifndef VIDEO_PROXY_H
#define VIDEO_PROXY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef VIDEO_PATH_MAX
#define VIDEO_PATH_MAX 512
#endif

typedef struct {
    char src_path[VIDEO_PATH_MAX];
    char proxy_path[VIDEO_PATH_MAX];
    int32_t width;
    int32_t height;
    int32_t frame_count;
    double fps;
    int64_t duration_us;
    bool valid;
} video_asset_t;

/**
 * Build (or reuse) an MJPEG proxy for @p src_path.
 * Writes proxy under cache/ next to the working directory.
 *
 * @param src_path  Path to source MP4 (POSIX path, not LVGL A: prefix)
 * @param max_w     Max proxy width (0 = keep source width, typically 320)
 * @param out       Filled on success
 * @return true on success
 */
bool video_proxy_build(const char * src_path, int max_w, video_asset_t * out);

/**
 * Format frame index as mm:ss.d into @p buf.
 */
void video_proxy_format_time(const video_asset_t * asset, int32_t frame, char * buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_PROXY_H */

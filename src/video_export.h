/**
 * @file video_export.h
 * @brief Export a clipped frame range to MP4.
 */

#ifndef VIDEO_EXPORT_H
#define VIDEO_EXPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "video_proxy.h"

typedef void (*video_export_progress_cb)(int percent, void * user_data);

/**
 * Export frames [start_frame, end_frame] inclusive from the original source
 * (falls back to proxy if needed) into an H.264/MPEG-4 MP4.
 *
 * @return true on success
 */
bool video_export_clip(const video_asset_t * asset,
                       int32_t start_frame, int32_t end_frame,
                       const char * out_path,
                       video_export_progress_cb progress_cb,
                       void * user_data);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_EXPORT_H */

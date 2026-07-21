/**
 * @file video_browser.h
 * @brief Scrollable file browser for selecting videos to edit.
 */

#ifndef VIDEO_BROWSER_H
#define VIDEO_BROWSER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show the video file browser on the active screen.
 * @param start_dir  Optional starting directory (POSIX path). Defaults to "photos".
 */
void video_browser_create(const char * start_dir);

/**
 * Tear down the browser UI (if present).
 */
void video_browser_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_BROWSER_H */

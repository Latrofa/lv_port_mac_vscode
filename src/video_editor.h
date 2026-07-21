/**
 * @file video_editor.h
 * @brief Social-style dual-knob video trim UI.
 */

#ifndef VIDEO_EDITOR_H
#define VIDEO_EDITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the video editor UI on the active screen.
 * @param src_path POSIX path to an MP4 (e.g. "photos/recording_....mp4")
 */
void video_editor_create(const char * src_path);

/**
 * Tear down the editor UI and release frame buffers / timers.
 */
void video_editor_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_EDITOR_H */

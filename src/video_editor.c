/**
 * @file video_editor.c
 * @brief Dual-knob video trim UI with frame thumbnails and MP4 export.
 */

#include "video_editor.h"
#include "video_proxy.h"
#include "frame_store.h"
#include "video_export.h"
#include "video_browser.h"

#include "lvgl/lvgl.h"
#include "../lvgl/include/lvgl/drivers/ffmpeg/lv_ffmpeg.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* Seek helpers exist in lv_ffmpeg.c but are not always in the public header. */
extern void lv_ffmpeg_player_seek(lv_obj_t * obj, int32_t frame_id);
extern int32_t lv_ffmpeg_player_get_cur_frame(lv_obj_t * obj);

#define THUMB_W 72
#define THUMB_H 54
#define PREVIEW_MAX_W 280
#define PREVIEW_MAX_H 200
#define FILMSTRIP_TILES 8
#define DEFAULT_VIDEO "photos/recording_20260512_183037_285.mp4"

typedef enum {
    EDITOR_IDLE,
    EDITOR_PLAYING,
    EDITOR_PAUSED
} editor_state_t;

typedef enum {
    KNOB_NONE = 0,
    KNOB_START,
    KNOB_END
} active_knob_t;

typedef struct {
    video_asset_t asset;
    frame_store_t * store;

    lv_obj_t * root;
    lv_obj_t * preview_img;
    lv_obj_t * player;          /* ffmpeg player for range playback (proxy) */
    lv_obj_t * status_label;
    lv_obj_t * start_label;
    lv_obj_t * end_label;
    lv_obj_t * range_slider;
    lv_obj_t * start_thumb;
    lv_obj_t * end_thumb;
    lv_obj_t * play_btn;
    lv_obj_t * export_btn;
    lv_obj_t * filmstrip;

    lv_draw_buf_t * preview_buf;
    lv_draw_buf_t * start_thumb_buf;
    lv_draw_buf_t * end_thumb_buf;

    int32_t start_frame;
    int32_t end_frame;
    int32_t prev_left;
    int32_t prev_right;
    active_knob_t active_knob;
    editor_state_t state;

    lv_timer_t * play_timer;
    bool busy;
} editor_t;

static editor_t g_ed;

static void show_status(const char * fmt, ...)
{
    if(!g_ed.status_label) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(g_ed.status_label, buf);
}

static void update_time_labels(void)
{
    char a[32], b[32];
    video_proxy_format_time(&g_ed.asset, g_ed.start_frame, a, sizeof(a));
    video_proxy_format_time(&g_ed.asset, g_ed.end_frame, b, sizeof(b));
    if(g_ed.start_label) lv_label_set_text(g_ed.start_label, a);
    if(g_ed.end_label) lv_label_set_text(g_ed.end_label, b);
}

static void position_thumb_above_knob(lv_obj_t * thumb, bool is_start)
{
    if(!thumb || !g_ed.range_slider) return;

    lv_obj_update_layout(g_ed.range_slider);
    lv_coord_t sw = lv_obj_get_width(g_ed.range_slider);
    lv_coord_t sh = lv_obj_get_height(g_ed.range_slider);
    int32_t min = lv_slider_get_min_value(g_ed.range_slider);
    int32_t max = lv_slider_get_max_value(g_ed.range_slider);
    if(max <= min) return;

    int32_t val = is_start ? g_ed.start_frame : g_ed.end_frame;
    float t = (float)(val - min) / (float)(max - min);
    lv_coord_t x = (lv_coord_t)(t * (sw - 20)) + 10 - THUMB_W / 2;
    lv_coord_t y = - (THUMB_H + 8);

    lv_obj_align_to(thumb, g_ed.range_slider, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_HIDDEN);
    LV_UNUSED(sh);
}

static void refresh_preview_frame(int32_t frame_id)
{
    if(!g_ed.store || !g_ed.preview_img) return;

    g_ed.preview_buf = frame_store_get_frame(g_ed.store, frame_id,
                                             PREVIEW_MAX_W, PREVIEW_MAX_H,
                                             g_ed.preview_buf);
    if(g_ed.preview_buf) {
        lv_image_set_src(g_ed.preview_img, g_ed.preview_buf);
        /* Hide ffmpeg player while scrubbing still frame */
        if(g_ed.player) lv_obj_add_flag(g_ed.player, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_ed.preview_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_knob_thumb(bool is_start)
{
    if(!g_ed.store) return;
    int32_t frame = is_start ? g_ed.start_frame : g_ed.end_frame;
    lv_draw_buf_t ** slot = is_start ? &g_ed.start_thumb_buf : &g_ed.end_thumb_buf;
    lv_obj_t * img = is_start ? g_ed.start_thumb : g_ed.end_thumb;
    if(!img) return;

    *slot = frame_store_get_frame(g_ed.store, frame, THUMB_W, THUMB_H, *slot);
    if(*slot) {
        lv_image_set_src(img, *slot);
        position_thumb_above_knob(img, is_start);
    }
}

static void hide_knob_thumbs(void)
{
    if(g_ed.start_thumb) lv_obj_add_flag(g_ed.start_thumb, LV_OBJ_FLAG_HIDDEN);
    if(g_ed.end_thumb) lv_obj_add_flag(g_ed.end_thumb, LV_OBJ_FLAG_HIDDEN);
}

static void set_play_icon(bool playing)
{
    if(!g_ed.play_btn) return;
    lv_obj_t * label = lv_obj_get_child(g_ed.play_btn, 0);
    if(label) lv_label_set_text(label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void pause_playback(void)
{
    if(g_ed.player) {
        lv_ffmpeg_player_set_cmd(g_ed.player, LV_FFMPEG_PLAYER_CMD_PAUSE);
        lv_obj_add_flag(g_ed.player, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_ed.preview_img) lv_obj_clear_flag(g_ed.preview_img, LV_OBJ_FLAG_HIDDEN);
    g_ed.state = EDITOR_PAUSED;
    set_play_icon(false);
}

static void play_selection(void)
{
    if(!g_ed.player || g_ed.busy) return;

    lv_ffmpeg_player_seek(g_ed.player, g_ed.start_frame);
    lv_ffmpeg_player_set_cmd(g_ed.player, LV_FFMPEG_PLAYER_CMD_RESUME);
    lv_obj_clear_flag(g_ed.player, LV_OBJ_FLAG_HIDDEN);
    if(g_ed.preview_img) lv_obj_add_flag(g_ed.preview_img, LV_OBJ_FLAG_HIDDEN);
    g_ed.state = EDITOR_PLAYING;
    set_play_icon(true);
    hide_knob_thumbs();
}

static void play_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if(g_ed.state != EDITOR_PLAYING || !g_ed.player) return;

    int32_t cur = lv_ffmpeg_player_get_cur_frame(g_ed.player);
    if(cur < 0) return;

    if(cur >= g_ed.end_frame) {
        lv_ffmpeg_player_set_cmd(g_ed.player, LV_FFMPEG_PLAYER_CMD_PAUSE);
        lv_ffmpeg_player_seek(g_ed.player, g_ed.start_frame);
        g_ed.state = EDITOR_PAUSED;
        set_play_icon(false);
        refresh_preview_frame(g_ed.start_frame);
    }
}

static void range_slider_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(g_ed.busy) return;

    if(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
       code == LV_EVENT_VALUE_CHANGED) {
        if(g_ed.state == EDITOR_PLAYING) pause_playback();

        int32_t left = lv_slider_get_left_value(g_ed.range_slider);
        int32_t right = lv_slider_get_value(g_ed.range_slider);

        /* Enforce min gap of 1 frame */
        if(right <= left) {
            if(g_ed.active_knob == KNOB_START || left != g_ed.prev_left) {
                right = left + 1;
                if(right > lv_slider_get_max_value(g_ed.range_slider)) {
                    right = lv_slider_get_max_value(g_ed.range_slider);
                    left = right - 1;
                    if(left < 0) left = 0;
                    lv_slider_set_start_value(g_ed.range_slider, left, LV_ANIM_OFF);
                }
                lv_slider_set_value(g_ed.range_slider, right, LV_ANIM_OFF);
            }
            else {
                left = right - 1;
                if(left < 0) left = 0;
                lv_slider_set_start_value(g_ed.range_slider, left, LV_ANIM_OFF);
            }
        }

        /* Detect which knob moved */
        if(left != g_ed.prev_left && right == g_ed.prev_right) {
            g_ed.active_knob = KNOB_START;
        }
        else if(right != g_ed.prev_right && left == g_ed.prev_left) {
            g_ed.active_knob = KNOB_END;
        }
        else if(left != g_ed.prev_left) {
            g_ed.active_knob = KNOB_START;
        }
        else if(right != g_ed.prev_right) {
            g_ed.active_knob = KNOB_END;
        }

        g_ed.start_frame = left;
        g_ed.end_frame = right;
        g_ed.prev_left = left;
        g_ed.prev_right = right;

        update_time_labels();

        if(g_ed.active_knob == KNOB_START) {
            refresh_preview_frame(g_ed.start_frame);
            refresh_knob_thumb(true);
            if(g_ed.end_thumb) lv_obj_add_flag(g_ed.end_thumb, LV_OBJ_FLAG_HIDDEN);
        }
        else if(g_ed.active_knob == KNOB_END) {
            refresh_preview_frame(g_ed.end_frame);
            refresh_knob_thumb(false);
            if(g_ed.start_thumb) lv_obj_add_flag(g_ed.start_thumb, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /* Keep thumbs briefly visible then hide — leave visible while not dragging next */
        hide_knob_thumbs();
        g_ed.active_knob = KNOB_NONE;
    }
}

static void play_btn_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    if(g_ed.busy) return;

    if(g_ed.state == EDITOR_PLAYING) {
        pause_playback();
        refresh_preview_frame(g_ed.start_frame);
    }
    else {
        play_selection();
    }
}

static void export_progress_cb(int percent, void * user_data)
{
    LV_UNUSED(user_data);
    show_status("Exporting… %d%%", percent);
    /* Pump LVGL so status updates while export runs on UI thread */
    lv_timer_handler();
}

static void export_btn_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    if(g_ed.busy || !g_ed.asset.valid) return;

    if(g_ed.state == EDITOR_PLAYING) pause_playback();
    g_ed.busy = true;
    lv_obj_add_state(g_ed.export_btn, LV_STATE_DISABLED);
    show_status("Exporting…");

    char out_path[VIDEO_PATH_MAX];
    time_t now = time(NULL);
    struct tm * tm = localtime(&now);
    snprintf(out_path, sizeof(out_path),
             "exports/clip_%04d%02d%02d_%02d%02d%02d.mp4",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    bool ok = video_export_clip(&g_ed.asset, g_ed.start_frame, g_ed.end_frame,
                                out_path, export_progress_cb, NULL);

    g_ed.busy = false;
    lv_obj_clear_state(g_ed.export_btn, LV_STATE_DISABLED);

    if(ok) {
        show_status("Saved %s", out_path);
    }
    else {
        show_status("Export failed");
    }
}

static void back_btn_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    if(g_ed.busy) return;
    if(g_ed.state == EDITOR_PLAYING) pause_playback();

    /* Remember last folder so browser opens nearby */
    char start_dir[VIDEO_PATH_MAX];
    start_dir[0] = '\0';
    if(g_ed.asset.src_path[0]) {
        strncpy(start_dir, g_ed.asset.src_path, sizeof(start_dir) - 1);
        start_dir[sizeof(start_dir) - 1] = '\0';
        char * slash = strrchr(start_dir, '/');
        if(slash) *slash = '\0';
        else start_dir[0] = '\0';
    }

    video_editor_destroy();
    video_browser_create(start_dir[0] ? start_dir : "photos");
}

void video_editor_destroy(void)
{
    if(g_ed.play_timer) {
        lv_timer_delete(g_ed.play_timer);
        g_ed.play_timer = NULL;
    }
    if(g_ed.player) {
        lv_ffmpeg_player_set_cmd(g_ed.player, LV_FFMPEG_PLAYER_CMD_STOP);
        g_ed.player = NULL; /* deleted with root */
    }
    if(g_ed.store) {
        frame_store_close(g_ed.store);
        g_ed.store = NULL;
    }
    if(g_ed.preview_buf) {
        lv_draw_buf_destroy(g_ed.preview_buf);
        g_ed.preview_buf = NULL;
    }
    if(g_ed.start_thumb_buf) {
        lv_draw_buf_destroy(g_ed.start_thumb_buf);
        g_ed.start_thumb_buf = NULL;
    }
    if(g_ed.end_thumb_buf) {
        lv_draw_buf_destroy(g_ed.end_thumb_buf);
        g_ed.end_thumb_buf = NULL;
    }
    if(g_ed.root) {
        lv_obj_delete(g_ed.root);
        g_ed.root = NULL;
    }
    memset(&g_ed, 0, sizeof(g_ed));
}

static lv_obj_t * make_thumb_img(lv_obj_t * parent)
{
    lv_obj_t * img = lv_image_create(parent);
    lv_obj_set_size(img, THUMB_W, THUMB_H);
    lv_obj_set_style_border_width(img, 2, 0);
    lv_obj_set_style_border_color(img, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(img, 4, 0);
    lv_obj_set_style_bg_color(img, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(img, LV_OBJ_FLAG_FLOATING);
    return img;
}

static void filmstrip_tile_delete_cb(lv_event_t * e)
{
    lv_draw_buf_t * buf = (lv_draw_buf_t *)lv_event_get_user_data(e);
    if(buf) lv_draw_buf_destroy(buf);
}

static void build_filmstrip(void)
{
    if(!g_ed.filmstrip || !g_ed.store || g_ed.asset.frame_count <= 0) return;

    lv_obj_clean(g_ed.filmstrip);

    int32_t n = FILMSTRIP_TILES;
    if(n > g_ed.asset.frame_count) n = g_ed.asset.frame_count;
    const int tile_w = 36;
    const int tile_h = 28;

    for(int i = 0; i < n; i++) {
        int32_t frame = 0;
        if(n > 1) {
            frame = (int32_t)((int64_t)i * (g_ed.asset.frame_count - 1) / (n - 1));
        }
        lv_draw_buf_t * buf = frame_store_get_frame(g_ed.store, frame, tile_w, tile_h, NULL);
        if(!buf) continue;

        lv_obj_t * img = lv_image_create(g_ed.filmstrip);
        lv_image_set_src(img, buf);
        lv_obj_add_event_cb(img, filmstrip_tile_delete_cb, LV_EVENT_DELETE, buf);
    }
}

void video_editor_create(const char * src_path)
{
    video_editor_destroy();
    video_browser_destroy();

    memset(&g_ed, 0, sizeof(g_ed));
    g_ed.state = EDITOR_PAUSED;

    if(!src_path || !src_path[0]) src_path = DEFAULT_VIDEO;

    /* Root */
    g_ed.root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_ed.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(g_ed.root, 6, 0);
    lv_obj_set_style_pad_row(g_ed.root, 4, 0);
    lv_obj_set_flex_flow(g_ed.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_ed.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(g_ed.root, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(g_ed.root, 0, 0);

    /* Title row with back button */
    lv_obj_t * title_row = lv_obj_create(g_ed.root);
    lv_obj_set_size(title_row, lv_pct(95), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_pad_column(title_row, 8, 0);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * back_btn = lv_button_create(title_row);
    lv_obj_set_style_pad_hor(back_btn, 10, 0);
    lv_obj_set_style_pad_ver(back_btn, 6, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Files");
    lv_obj_center(back_lbl);

    lv_obj_t * title = lv_label_create(title_row);
    lv_label_set_text(title, "Clip Editor");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    g_ed.status_label = lv_label_create(g_ed.root);
    lv_label_set_long_mode(g_ed.status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(g_ed.status_label, lv_pct(95));
    lv_obj_set_style_text_color(g_ed.status_label, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(g_ed.status_label, &lv_font_montserrat_12, 0);
    show_status("Preparing proxy…");
    lv_timer_handler();

    /* Preview container */
    lv_obj_t * preview_box = lv_obj_create(g_ed.root);
    lv_obj_set_size(preview_box, PREVIEW_MAX_W + 8, PREVIEW_MAX_H + 8);
    lv_obj_set_style_pad_all(preview_box, 2, 0);
    lv_obj_set_style_bg_color(preview_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(preview_box, 0, 0);
    lv_obj_clear_flag(preview_box, LV_OBJ_FLAG_SCROLLABLE);

    g_ed.preview_img = lv_image_create(preview_box);
    lv_obj_center(g_ed.preview_img);

    /* Build proxy (may take a few seconds on first open) */
    if(!video_proxy_build(src_path, 320, &g_ed.asset)) {
        show_status("Failed to open: %s", src_path);
        return;
    }

    g_ed.store = frame_store_open(&g_ed.asset);
    if(!g_ed.store) {
        show_status("Frame store open failed");
        return;
    }

    /* FFmpeg player on proxy for range playback */
    g_ed.player = lv_ffmpeg_player_create(preview_box);
    if(lv_ffmpeg_player_set_src(g_ed.player, g_ed.asset.proxy_path) != LV_RESULT_OK) {
        show_status("Player open failed (proxy)");
        /* Still allow scrub via frame_store */
        lv_obj_delete(g_ed.player);
        g_ed.player = NULL;
    }
    else {
        lv_ffmpeg_player_set_auto_restart(g_ed.player, false);
        lv_obj_set_size(g_ed.player, PREVIEW_MAX_W, PREVIEW_MAX_H);
        lv_obj_center(g_ed.player);
        lv_ffmpeg_player_set_cmd(g_ed.player, LV_FFMPEG_PLAYER_CMD_PAUSE);
        lv_obj_add_flag(g_ed.player, LV_OBJ_FLAG_HIDDEN);
    }

    g_ed.start_frame = 0;
    g_ed.end_frame = g_ed.asset.frame_count > 0 ? g_ed.asset.frame_count - 1 : 0;
    g_ed.prev_left = g_ed.start_frame;
    g_ed.prev_right = g_ed.end_frame;

    refresh_preview_frame(0);

    /* Filmstrip */
    g_ed.filmstrip = lv_obj_create(g_ed.root);
    lv_obj_set_size(g_ed.filmstrip, lv_pct(95), 40);
    lv_obj_set_style_pad_all(g_ed.filmstrip, 2, 0);
    lv_obj_set_style_pad_column(g_ed.filmstrip, 2, 0);
    lv_obj_set_style_bg_color(g_ed.filmstrip, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(g_ed.filmstrip, 0, 0);
    lv_obj_set_flex_flow(g_ed.filmstrip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_ed.filmstrip, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_ed.filmstrip, LV_OBJ_FLAG_SCROLLABLE);
    build_filmstrip();

    /* Time labels row */
    lv_obj_t * time_row = lv_obj_create(g_ed.root);
    lv_obj_set_size(time_row, lv_pct(95), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_ed.start_label = lv_label_create(time_row);
    g_ed.end_label = lv_label_create(time_row);
    lv_obj_set_style_text_color(g_ed.start_label, lv_color_hex(0x66ccff), 0);
    lv_obj_set_style_text_color(g_ed.end_label, lv_color_hex(0xff8866), 0);
    update_time_labels();

    /* Slider container (room for floating thumbs) */
    lv_obj_t * slider_box = lv_obj_create(g_ed.root);
    lv_obj_set_size(slider_box, lv_pct(95), THUMB_H + 40);
    lv_obj_set_style_pad_top(slider_box, THUMB_H + 4, 0);
    lv_obj_set_style_pad_bottom(slider_box, 4, 0);
    lv_obj_set_style_pad_hor(slider_box, 4, 0);
    lv_obj_set_style_bg_opa(slider_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(slider_box, 0, 0);
    lv_obj_clear_flag(slider_box, LV_OBJ_FLAG_SCROLLABLE);

    g_ed.start_thumb = make_thumb_img(slider_box);
    g_ed.end_thumb = make_thumb_img(slider_box);

    g_ed.range_slider = lv_slider_create(slider_box);
    lv_obj_set_width(g_ed.range_slider, lv_pct(100));
    lv_obj_align(g_ed.range_slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_slider_set_mode(g_ed.range_slider, LV_SLIDER_MODE_RANGE);
    lv_slider_set_range(g_ed.range_slider, 0,
                        g_ed.asset.frame_count > 1 ? g_ed.asset.frame_count - 1 : 1);
    lv_slider_set_start_value(g_ed.range_slider, g_ed.start_frame, LV_ANIM_OFF);
    lv_slider_set_value(g_ed.range_slider, g_ed.end_frame, LV_ANIM_OFF);

    /* Larger knobs for touch/phone UI */
    lv_obj_set_style_pad_all(g_ed.range_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_ed.range_slider, lv_color_hex(0x66ccff), LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_ed.range_slider, lv_color_hex(0x3a8fd6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_ed.range_slider, lv_color_hex(0x444444), LV_PART_MAIN);

    lv_obj_add_event_cb(g_ed.range_slider, range_slider_event_cb, LV_EVENT_ALL, NULL);

    /* Controls row */
    lv_obj_t * controls = lv_obj_create(g_ed.root);
    lv_obj_set_size(controls, lv_pct(95), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(controls, 4, 0);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_ed.play_btn = lv_button_create(controls);
    lv_obj_add_event_cb(g_ed.play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * play_lbl = lv_label_create(g_ed.play_btn);
    lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
    lv_obj_center(play_lbl);

    g_ed.export_btn = lv_button_create(controls);
    lv_obj_add_event_cb(g_ed.export_btn, export_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * exp_lbl = lv_label_create(g_ed.export_btn);
    lv_label_set_text(exp_lbl, LV_SYMBOL_SAVE " Export");
    lv_obj_center(exp_lbl);

    g_ed.play_timer = lv_timer_create(play_timer_cb, 40, NULL);

    show_status("%s  ·  %d frames  ·  %.1f fps",
                g_ed.asset.proxy_path, g_ed.asset.frame_count, g_ed.asset.fps);
}

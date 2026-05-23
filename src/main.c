/**
 * @file main.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE /* needed for usleep() */
#endif

#include <stdlib.h>
#include <stdio.h>
#ifdef _MSC_VER
  #include <Windows.h>
#else
  #include <unistd.h>
  #include <pthread.h>
#endif
#include "lvgl/lvgl.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"
#include "lvgl/examples/libs/ffmpeg/lv_example_ffmpeg.h"
#include "../lvgl/include/lvgl/drivers/ffmpeg/lv_ffmpeg.h"
#include <SDL.h>

#include "hal/hal.h"

/*********************
 *      DEFINES
 *********************/
#define THUMB_MAX_W 100
#define THUMB_MAX_H 100

/**********************
 *      TYPEDEFS
 **********************/

typedef enum {
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED
} player_state_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
/* For thumbnail gallery */
static void * create_thumbnail_dsc_alloc(const char * image_path, lv_coord_t thumb_w, lv_coord_t thumb_h);
static void thumbnail_delete_event_cb(lv_event_t * e);
void display_thumbnail_gallery(void);

/* For ffmpeg player */
static void play_pause_event_cb(lv_event_t * e);
static void stop_event_cb(lv_event_t * e);
static void slider_event_cb(lv_event_t * e);
static void progress_update_timer_cb(lv_timer_t * t);
static void player_event_cb(lv_event_t * e);
void lv_ffmpeg_player_with_controls(void);

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_obj_t * player;
static lv_obj_t * slider;
static lv_obj_t * play_pause_btn;
static player_state_t g_player_state = PLAYER_STATE_STOPPED;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

#if LV_USE_OS != LV_OS_FREERTOS

int main(int argc, char **argv)
{
  (void)argc; /*Unused*/
  (void)argv; /*Unused*/

  /*Initialize LVGL*/
  lv_init();

  /*Initialize the HAL (display, input devices, tick) for LVGL*/
  sdl_hal_init(320, 480);

  /* Run the thumbnail creation example */
  display_thumbnail_gallery();
  /* Or run the ffmpeg player example */
  /* lv_ffmpeg_player_with_controls(); */

  while(1) {
    /* Periodically call the lv_task handler.
     * It could be done in a timer interrupt or an OS task too.*/
    uint32_t sleep_time_ms = lv_timer_handler();
    if(sleep_time_ms == LV_NO_TIMER_READY){
	sleep_time_ms =  LV_DEF_REFR_PERIOD;
    }
#ifdef _MSC_VER
    Sleep(sleep_time_ms);
#else
    usleep(sleep_time_ms * 1000);
#endif
  }

  return 0;
}


#endif
extern int32_t lv_ffmpeg_player_get_cur_frame(lv_obj_t * obj);
extern void lv_ffmpeg_player_seek(lv_obj_t * obj, int32_t frame_id);
/**********************
 *   STATIC FUNCTIONS
 **********************/

static void thumbnail_delete_event_cb(lv_event_t * e)
{
    lv_obj_t * img = lv_event_get_target(e);
    lv_draw_buf_t * draw_buf = (lv_draw_buf_t *)lv_image_get_src(img);

    if(draw_buf) {
        lv_draw_buf_destroy(draw_buf);
    }
}

static void * create_thumbnail_dsc_alloc(const char * image_path, lv_coord_t thumb_w, lv_coord_t thumb_h)
{
    if (thumb_w > THUMB_MAX_W || thumb_h > THUMB_MAX_H) {
        return NULL;
    }

    /* Get image info */
    lv_image_header_t header;
    if(lv_image_decoder_get_info(image_path, &header) != LV_RESULT_OK) {
        return NULL;
    }

    LV_LOG_INFO("Creating thumbnail for %s (%d×%d) -> %d×%d", image_path, header.w, header.h, thumb_w, thumb_h);

    lv_obj_t * canvas = NULL;
    lv_draw_buf_t * draw_buf = NULL;

    /* Allocate draw buffer and create a temporary canvas to perform the scaling. */
    draw_buf = lv_draw_buf_create(thumb_w, thumb_h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if(draw_buf == NULL) {
        LV_LOG_ERROR("Could not allocate draw buffer for thumbnail");
        goto error;
    }

    canvas = lv_canvas_create(lv_scr_act());
    if (!canvas) goto error;

    lv_canvas_set_draw_buf(canvas, draw_buf);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_fill_bg(canvas, lv_color_hex3(0x000), LV_OPA_TRANSP);

    /* Calculate scaled size maintaining aspect ratio */
    int32_t zoom_x = ((int32_t)thumb_w * 256) / header.w;
    int32_t zoom_y = ((int32_t)thumb_h * 256) / header.h;
    int32_t zoom = LV_MIN(zoom_x, zoom_y);

    lv_coord_t scaled_w = (header.w * zoom) / 256;
    lv_coord_t scaled_h = (header.h * zoom) / 256;
    lv_coord_t dest_x = (thumb_w - scaled_w) / 2;
    lv_coord_t dest_y = (thumb_h - scaled_h) / 2;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_image_dsc_t draw_dsc;
    lv_draw_image_dsc_init(&draw_dsc);
    draw_dsc.src = image_path;
    draw_dsc.scale_x = zoom;
    draw_dsc.scale_y = zoom;

    /* Set pivot to top-left so the image scales from the origin */
    draw_dsc.pivot.x = 0;
    draw_dsc.pivot.y = 0;

    /* coords must represent the unscaled bounding box of the image */
    lv_area_t coords = { .x1 = dest_x, .y1 = dest_y, .x2 = dest_x + header.w - 1, .y2 = dest_y + header.h - 1};
    lv_draw_image(&layer, &draw_dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);

    /* Clean up temporary canvas. */
    lv_obj_del(canvas);

    LV_LOG_INFO("Thumbnail created: %d×%d", thumb_w, thumb_h);

    /* Return the draw buffer directly. LVGL image widget accepts draw buffer natively. */
    return draw_buf;

error:
    if(canvas) lv_obj_del(canvas);
    if(draw_buf) lv_draw_buf_destroy(draw_buf);
    return NULL;
}

/**
 * @brief Creates a gallery of thumbnails from a directory of images.
 */
void display_thumbnail_gallery(void)
{
    const char * image_dir = "A:photos";

    lv_obj_t * cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_pad_gap(cont, 10, 0);

    lv_fs_dir_t d;
    if (lv_fs_dir_open(&d, image_dir) != LV_FS_RES_OK) {
        LV_LOG_ERROR("Failed to open image directory: %s", image_dir);
        lv_obj_t * label = lv_label_create(cont);
        lv_label_set_text_fmt(label, "Failed to open directory:\n%s", image_dir);
        return;
    }

    char fn[256];
    while (lv_fs_dir_read(&d, fn, sizeof(fn)) == LV_FS_RES_OK) {
        if (strlen(fn) == 0) break;
        if (fn[0] == '/' || fn[0] == '.') continue;

        const char * ext = lv_fs_get_ext(fn);
        bool is_image = (ext && (strcmp(ext, "png") == 0 || strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0));
        bool is_video = (ext && (strcmp(ext, "mp4") == 0));

        if(is_image || is_video) {
            char full_path[512];
            lv_snprintf(full_path, sizeof(full_path), "%s/%s", image_dir, fn);

            void * thumb_dsc = create_thumbnail_dsc_alloc(full_path, THUMB_MAX_W, THUMB_MAX_H);

            if (thumb_dsc) {
                lv_obj_t * thumb_cont = lv_obj_create(cont);
                lv_obj_remove_style_all(thumb_cont);
                lv_obj_set_size(thumb_cont, THUMB_MAX_W, THUMB_MAX_H);

                lv_obj_t * thumb_img = lv_image_create(thumb_cont);
                lv_image_set_src(thumb_img, thumb_dsc);
                lv_obj_add_event_cb(thumb_img, thumbnail_delete_event_cb, LV_EVENT_DELETE, NULL);
                lv_obj_center(thumb_img);

                if(is_video) {
                    lv_obj_t * play_icon = lv_label_create(thumb_cont);
                    lv_label_set_text(play_icon, LV_SYMBOL_PLAY);
                    lv_obj_set_style_text_color(play_icon, lv_color_white(), 0);
                    lv_obj_set_style_text_opa(play_icon, LV_OPA_90, 0);
                    lv_obj_set_style_text_font(play_icon, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_bg_color(play_icon, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(play_icon, LV_OPA_50, 0);
                    lv_obj_set_style_border_width(play_icon, 0, 0);
                    lv_obj_set_style_pad_all(play_icon, 5, 0);
                    lv_obj_set_style_radius(play_icon, LV_RADIUS_CIRCLE, 0);
                    lv_obj_center(play_icon);
                }
            }
        }
    }

    lv_fs_dir_close(&d);
}

static void play_pause_event_cb(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);

    if(g_player_state == PLAYER_STATE_PLAYING) {
        lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_PAUSE);
        lv_label_set_text(label, LV_SYMBOL_PLAY);
        g_player_state = PLAYER_STATE_PAUSED;
    }
    else {
        if(g_player_state == PLAYER_STATE_PAUSED) {
            lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_RESUME);
        }
        else { /* STOPPED */
            lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_START);
        }
        lv_label_set_text(label, LV_SYMBOL_PAUSE);
        g_player_state = PLAYER_STATE_PLAYING;
    }
}

static void stop_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_STOP);
    g_player_state = PLAYER_STATE_STOPPED;

    lv_obj_t * label = lv_obj_get_child(play_pause_btn, 0);
    lv_label_set_text(label, LV_SYMBOL_PLAY);

    if(slider) {
        lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    }
}

static void slider_event_cb(lv_event_t * e)
{
    lv_obj_t * s = lv_event_get_target(e);
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        int32_t v = lv_slider_get_value(s);
        lv_ffmpeg_player_seek(player, v);
    }
}


static void progress_update_timer_cb(lv_timer_t * t)
{
  LV_UNUSED(t);
  if(player && slider)
  {
    if(lv_obj_get_state(slider) & LV_STATE_PRESSED)
    {
      lv_ffmpeg_player_seek(player, lv_slider_get_value(slider));
    }
    else
    {
      int32_t current_frame = lv_ffmpeg_player_get_cur_frame(player);
      if(current_frame >= 0)
      {
          /*Don't update while the slider is being pressed*/
          lv_slider_set_value(slider, current_frame, LV_ANIM_OFF);
      }
    }
  }
}

static void player_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_READY) { /* Assuming LV_EVENT_READY is sent when playback finishes */
        g_player_state = PLAYER_STATE_STOPPED;
        lv_obj_t * label = lv_obj_get_child(play_pause_btn, 0);
        lv_label_set_text(label, LV_SYMBOL_PLAY);
        if(slider) {
            lv_slider_set_value(slider, 0, LV_ANIM_OFF);
        }
    }
}

void lv_ffmpeg_player_with_controls(void)
{
    const char * video_path = "lvgl/examples/libs/ffmpeg/birds.mp4";
     lv_obj_t * root = lv_obj_create(lv_scr_act());
     lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
     player = lv_ffmpeg_player_create(root);
    if(lv_ffmpeg_player_set_src(player, video_path) != LV_RESULT_OK) {
        LV_LOG_ERROR("Could not open video file: %s", video_path);
        lv_obj_delete(player);
        player = NULL;
        return;
    }
    lv_ffmpeg_player_set_auto_restart(player, false);
    lv_obj_set_size(player, lv_pct(60), lv_pct(60));
//    lv_obj_center(player);
    lv_obj_add_event_cb(player, player_event_cb, LV_EVENT_ALL, NULL);

    /* Create a container for controls */
    lv_obj_t * cont = lv_obj_create(root);
    lv_obj_set_size(cont, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_align_to(cont, player, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cont, 20, 0);

    /* Play/Pause button */
    play_pause_btn = lv_btn_create(cont);
    lv_obj_add_event_cb(play_pause_btn, play_pause_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * label_play = lv_label_create(play_pause_btn);
    lv_label_set_text(label_play, LV_SYMBOL_PLAY);
    lv_obj_center(label_play);

    /* Stop button */
    lv_obj_t * btn_stop = lv_btn_create(cont);
    lv_obj_add_event_cb(btn_stop, stop_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * label_stop = lv_label_create(btn_stop);
    lv_label_set_text(label_stop, LV_SYMBOL_STOP);
    lv_obj_center(label_stop);

    /* Slider */
    slider = lv_slider_create(lv_scr_act());
    lv_obj_set_width(slider, lv_pct(80));
    lv_obj_align_to(slider, cont, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    int32_t frame_cnt = lv_ffmpeg_get_frame_num(video_path);
    if(frame_cnt > 0) {
        lv_slider_set_range(slider, 0, frame_cnt);
    }
    else {
        lv_slider_set_range(slider, 0, 100); /* Fallback */
    }

    //lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Timer to update progress */
    lv_timer_create(progress_update_timer_cb, 50, NULL);

    g_player_state = PLAYER_STATE_PAUSED;
    lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_PAUSE);
}

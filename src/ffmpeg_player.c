#include "ffmpeg_player.h"
#include "lvgl/lvgl.h"
#include "../lvgl/include/lvgl/drivers/ffmpeg/lv_ffmpeg.h"
#include <stdio.h>

typedef enum {
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED
} player_state_t;

static lv_obj_t * player;
static lv_obj_t * slider;
static lv_obj_t * play_pause_btn;
static player_state_t g_player_state = PLAYER_STATE_STOPPED;

extern int32_t lv_ffmpeg_player_get_cur_frame(lv_obj_t * obj);
extern void lv_ffmpeg_player_seek(lv_obj_t * obj, int32_t frame_id);

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

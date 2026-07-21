/**
 * @file main.c
 * @brief LVGL simulator entry — launches the video clip editor.
 */

#ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE /* needed for usleep() */
#endif

#include <stdlib.h>
#include <stdio.h>
#ifdef _MSC_VER
  #include <Windows.h>
#else
  #include <unistd.h>
#endif

#include "lvgl/lvgl.h"
#include "hal/hal.h"
#include "video_editor.h"
#include "video_browser.h"

#if LV_USE_OS != LV_OS_FREERTOS

int main(int argc, char ** argv)
{
    lv_init();
    sdl_hal_init(320, 480);

    /* Optional CLI: open a video directly; otherwise show the file browser */
    if(argc > 1 && argv[1] && argv[1][0]) {
        video_editor_create(argv[1]);
    }
    else {
        video_browser_create("photos");
    }

    while(1) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if(sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
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

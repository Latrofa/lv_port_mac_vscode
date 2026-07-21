#include "thumbnail_gallery.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>

#define THUMB_MAX_W 100
#define THUMB_MAX_H 100

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
    while (lv_fs_dir_read(&d, fn, sizeof(fn)) == LV_FS_RES_OK)
    {
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

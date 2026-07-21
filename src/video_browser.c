/**
 * @file video_browser.c
 * @brief Directory browser for picking video files.
 */

#include "video_browser.h"
#include "video_editor.h"
#include "video_proxy.h"

#include "lvgl/lvgl.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define BROWSER_DEFAULT_DIR "photos"
#define MAX_ENTRIES 256
#define NAME_MAX_LEN 256
#define PATH_MAX_LEN VIDEO_PATH_MAX

typedef enum {
    ENTRY_DIR,
    ENTRY_VIDEO
} entry_kind_t;

typedef struct {
    entry_kind_t kind;
    char name[NAME_MAX_LEN];
} dir_entry_t;

typedef struct {
    lv_obj_t * root;
    lv_obj_t * path_label;
    lv_obj_t * list;
    lv_obj_t * status_label;
    char cwd[PATH_MAX_LEN];
    dir_entry_t entries[MAX_ENTRIES];
    int entry_count;
} browser_t;

static browser_t g_br;

static bool ends_with_ci(const char * name, const char * ext)
{
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if(nlen < elen) return false;
    const char * p = name + (nlen - elen);
    for(size_t i = 0; i < elen; i++) {
        char a = p[i];
        char b = ext[i];
        if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if(a != b) return false;
    }
    return true;
}

static bool is_video_name(const char * name)
{
    return ends_with_ci(name, ".mp4") ||
           ends_with_ci(name, ".mov") ||
           ends_with_ci(name, ".avi") ||
           ends_with_ci(name, ".mkv") ||
           ends_with_ci(name, ".m4v") ||
           ends_with_ci(name, ".webm");
}

static int entry_cmp(const void * a, const void * b)
{
    const dir_entry_t * ea = a;
    const dir_entry_t * eb = b;
    /* Directories first */
    if(ea->kind != eb->kind) {
        return ea->kind == ENTRY_DIR ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

static void join_path(char * out, size_t out_sz, const char * dir, const char * name)
{
    if(!dir || !dir[0] || strcmp(dir, ".") == 0) {
        snprintf(out, out_sz, "%s", name);
        return;
    }
    size_t len = strlen(dir);
    if(len > 0 && dir[len - 1] == '/') {
        snprintf(out, out_sz, "%s%s", dir, name);
    }
    else {
        snprintf(out, out_sz, "%s/%s", dir, name);
    }
}

static bool parent_dir(const char * path, char * out, size_t out_sz)
{
    if(!path || !path[0] || strcmp(path, ".") == 0 || strcmp(path, "/") == 0) {
        return false;
    }
    strncpy(out, path, out_sz - 1);
    out[out_sz - 1] = '\0';

    size_t len = strlen(out);
    while(len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    char * slash = strrchr(out, '/');
    if(!slash) {
        snprintf(out, out_sz, ".");
        return true;
    }
    if(slash == out) {
        snprintf(out, out_sz, "/");
        return true;
    }
    *slash = '\0';
    return true;
}

static void set_status(const char * fmt, ...)
{
    if(!g_br.status_label) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(g_br.status_label, buf);
}

static void scan_directory(void)
{
    g_br.entry_count = 0;

    DIR * dir = opendir(g_br.cwd);
    if(!dir) {
        set_status("Cannot open: %s", g_br.cwd);
        return;
    }

    struct dirent * de;
    while((de = readdir(dir)) != NULL && g_br.entry_count < MAX_ENTRIES) {
        const char * name = de->d_name;
        if(!name || name[0] == '\0') continue;
        if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if(name[0] == '.') continue; /* skip hidden */

        char full[PATH_MAX_LEN];
        join_path(full, sizeof(full), g_br.cwd, name);

        struct stat st;
        if(stat(full, &st) != 0) continue;

        if(S_ISDIR(st.st_mode)) {
            dir_entry_t * e = &g_br.entries[g_br.entry_count++];
            e->kind = ENTRY_DIR;
            strncpy(e->name, name, NAME_MAX_LEN - 1);
            e->name[NAME_MAX_LEN - 1] = '\0';
        }
        else if(S_ISREG(st.st_mode) && is_video_name(name)) {
            dir_entry_t * e = &g_br.entries[g_br.entry_count++];
            e->kind = ENTRY_VIDEO;
            strncpy(e->name, name, NAME_MAX_LEN - 1);
            e->name[NAME_MAX_LEN - 1] = '\0';
        }
    }
    closedir(dir);

    qsort(g_br.entries, (size_t)g_br.entry_count, sizeof(g_br.entries[0]), entry_cmp);

    int videos = 0, folders = 0;
    for(int i = 0; i < g_br.entry_count; i++) {
        if(g_br.entries[i].kind == ENTRY_VIDEO) videos++;
        else folders++;
    }
    set_status("%d video%s · %d folder%s",
               videos, videos == 1 ? "" : "s",
               folders, folders == 1 ? "" : "s");
}

static void rebuild_list(void);

static void open_video_path(const char * path)
{
    /* Copy path — browser destroy clears UI state */
    char path_copy[PATH_MAX_LEN];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    video_browser_destroy();
    video_editor_create(path_copy);
}

static void entry_click_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= g_br.entry_count) return;

    const dir_entry_t * ent = &g_br.entries[idx];
    char full[PATH_MAX_LEN];
    join_path(full, sizeof(full), g_br.cwd, ent->name);

    if(ent->kind == ENTRY_DIR) {
        strncpy(g_br.cwd, full, sizeof(g_br.cwd) - 1);
        g_br.cwd[sizeof(g_br.cwd) - 1] = '\0';
        rebuild_list();
    }
    else {
        open_video_path(full);
    }
}

static void up_btn_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    char parent[PATH_MAX_LEN];
    if(!parent_dir(g_br.cwd, parent, sizeof(parent))) {
        set_status("Already at top");
        return;
    }
    strncpy(g_br.cwd, parent, sizeof(g_br.cwd) - 1);
    g_br.cwd[sizeof(g_br.cwd) - 1] = '\0';
    rebuild_list();
}

static void refresh_btn_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    rebuild_list();
}

static void shortcut_btn_cb(lv_event_t * e)
{
    const char * dir = (const char *)lv_event_get_user_data(e);
    if(!dir) return;
    strncpy(g_br.cwd, dir, sizeof(g_br.cwd) - 1);
    g_br.cwd[sizeof(g_br.cwd) - 1] = '\0';
    rebuild_list();
}

static void rebuild_list(void)
{
    if(!g_br.list) return;

    lv_obj_clean(g_br.list);
    scan_directory();

    if(g_br.path_label) {
        lv_label_set_text_fmt(g_br.path_label, LV_SYMBOL_DIRECTORY " %s", g_br.cwd);
    }

    if(g_br.entry_count == 0) {
        lv_obj_t * empty = lv_label_create(g_br.list);
        lv_label_set_text(empty, "No videos or folders here");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        return;
    }

    for(int i = 0; i < g_br.entry_count; i++) {
        const dir_entry_t * ent = &g_br.entries[i];

        lv_obj_t * row = lv_button_create(g_br.list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3a5a7a), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, entry_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * lbl = lv_label_create(row);
        if(ent->kind == ENTRY_DIR) {
            lv_label_set_text_fmt(lbl, LV_SYMBOL_DIRECTORY "  %s", ent->name);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xffcc66), 0);
        }
        else {
            lv_label_set_text_fmt(lbl, LV_SYMBOL_VIDEO "  %s", ent->name);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        }
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, lv_pct(95));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

void video_browser_destroy(void)
{
    if(g_br.root) {
        lv_obj_delete(g_br.root);
        g_br.root = NULL;
    }
    memset(&g_br, 0, sizeof(g_br));
}

void video_browser_create(const char * start_dir)
{
    video_browser_destroy();
    /* Ensure any leftover editor is gone when returning */
    video_editor_destroy();

    memset(&g_br, 0, sizeof(g_br));
    if(start_dir && start_dir[0]) {
        strncpy(g_br.cwd, start_dir, sizeof(g_br.cwd) - 1);
    }
    else {
        strncpy(g_br.cwd, BROWSER_DEFAULT_DIR, sizeof(g_br.cwd) - 1);
    }

    g_br.root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_br.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(g_br.root, 8, 0);
    lv_obj_set_style_pad_row(g_br.root, 6, 0);
    lv_obj_set_flex_flow(g_br.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(g_br.root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(g_br.root, 0, 0);
    lv_obj_clear_flag(g_br.root, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t * title = lv_label_create(g_br.root);
    lv_label_set_text(title, "Select Video");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    /* Shortcut chips */
    lv_obj_t * chips = lv_obj_create(g_br.root);
    lv_obj_set_size(chips, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(chips, 0, 0);
    lv_obj_set_style_pad_column(chips, 6, 0);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chips, 0, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);

    static const char * shortcuts[] = { "photos", "exports", "cache", "." };
    static const char * shortcut_labels[] = { "Photos", "Exports", "Cache", "Root" };
    for(int i = 0; i < 4; i++) {
        lv_obj_t * btn = lv_button_create(chips);
        lv_obj_set_style_pad_hor(btn, 10, 0);
        lv_obj_set_style_pad_ver(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2d4a6f), 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, shortcut_btn_cb, LV_EVENT_CLICKED, (void *)shortcuts[i]);
        lv_obj_t * l = lv_label_create(btn);
        lv_label_set_text(l, shortcut_labels[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
    }

    /* Nav row: Up + path + refresh */
    lv_obj_t * nav = lv_obj_create(g_br.root);
    lv_obj_set_size(nav, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(nav, 0, 0);
    lv_obj_set_style_pad_column(nav, 6, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * up = lv_button_create(nav);
    lv_obj_set_style_pad_all(up, 8, 0);
    lv_obj_set_style_shadow_width(up, 0, 0);
    lv_obj_add_event_cb(up, up_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * up_l = lv_label_create(up);
    lv_label_set_text(up_l, LV_SYMBOL_UP);
    lv_obj_center(up_l);

    g_br.path_label = lv_label_create(nav);
    lv_obj_set_flex_grow(g_br.path_label, 1);
    lv_label_set_long_mode(g_br.path_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(g_br.path_label, 180);
    lv_obj_set_style_text_color(g_br.path_label, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(g_br.path_label, &lv_font_montserrat_12, 0);

    lv_obj_t * refresh = lv_button_create(nav);
    lv_obj_set_style_pad_all(refresh, 8, 0);
    lv_obj_set_style_shadow_width(refresh, 0, 0);
    lv_obj_add_event_cb(refresh, refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ref_l = lv_label_create(refresh);
    lv_label_set_text(ref_l, LV_SYMBOL_REFRESH);
    lv_obj_center(ref_l);

    /* Scrollable file list */
    g_br.list = lv_obj_create(g_br.root);
    lv_obj_set_width(g_br.list, lv_pct(100));
    lv_obj_set_flex_grow(g_br.list, 1);
    lv_obj_set_style_pad_all(g_br.list, 4, 0);
    lv_obj_set_style_pad_row(g_br.list, 4, 0);
    lv_obj_set_style_bg_color(g_br.list, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(g_br.list, 0, 0);
    lv_obj_set_style_radius(g_br.list, 8, 0);
    lv_obj_set_flex_flow(g_br.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_br.list, LV_DIR_VER);

    g_br.status_label = lv_label_create(g_br.root);
    lv_obj_set_width(g_br.status_label, lv_pct(100));
    lv_obj_set_style_text_color(g_br.status_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(g_br.status_label, &lv_font_montserrat_12, 0);

    rebuild_list();
}

/**
 * @file lv_demo_widgets_baidu_pan.c
 * Baidu Netdisk tab — LVGL v9.
 * States: INIT → LOADING → QR_SHOW → FILE_LIST / FAILED
 */
#include "lv_demo_widgets_baidu_pan.h"
#if LV_USE_DEMO_WIDGETS

#include "src/baidu_pan/baidu_oauth.h"
#include "lvgl/lvgl.h"
#include "lv_demo_widgets_components.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define QR_FILE_PATH "/tmp/baidu_qr.png"

LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk)

typedef enum {
    UI_STATE_INIT,
    UI_STATE_LOADING,
    UI_STATE_QR_SHOW,
    UI_STATE_FILE_LIST,
    UI_STATE_FAILED
} ui_state_t;

typedef struct {
    lv_obj_t *parent;
    lv_obj_t *container;
    lv_obj_t *login_btn;
    lv_obj_t *spinner;
    lv_obj_t *info_label;
    lv_obj_t *qr_image;
    lv_obj_t *action_btn;

    ui_state_t state;

    /* cross-thread flags */
    volatile int qr_ready;
    volatile int status_changed;
    volatile int list_ready;
    volatile int dl_event;
    baidu_oauth_status_t last_status;

    char  access_token[256];
    char  refresh_token[256];
    char  dl_msg[256];
    baidu_dl_status_t dl_status;

    uint8_t *qr_png_data;
    size_t   qr_png_len;
    char     user_code[64];
    char     current_dir[256];  /* last browsed directory */
} baidu_pan_ctx_t;

static baidu_pan_ctx_t g_ctx;

/* File cache shared between curl thread (list_cb) and main thread (on_timer) */
static baidu_oauth_file_t list_cached[200];
static volatile int list_cached_count = 0;

static void ui_set_state(ui_state_t s);
static void ui_clear_widgets(void);
static void on_login_click(lv_event_t *e);
static void on_logout_click(lv_event_t *e);
static void on_download_click(lv_event_t *e);
static void on_dir_click(lv_event_t *e);
static void on_download_click(lv_event_t *e);
static void on_dir_click(lv_event_t *e);
static void on_view_click(lv_event_t *e);
static void on_timer(lv_timer_t *t);

static void qr_cb(const uint8_t *png_data, size_t png_len,
                  const char *user_code, void *user_data);
static void status_cb(baidu_oauth_status_t status,
                      const char *access, const char *refresh, void *user_data);
static void list_cb(const baidu_oauth_file_t *files, int count, void *user_data);
static void dl_cb(baidu_dl_status_t st, const char *msg, void *user_data);

static void fetch_file_list(void);

/* ============  public entry  ============ */

void lv_demo_widgets_baidu_pan_create(lv_obj_t *parent)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.parent = parent;
    strcpy(g_ctx.current_dir, "/");

    g_ctx.container = lv_obj_create(parent);
    lv_obj_set_size(g_ctx.container, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(g_ctx.container, 8, 0);
    lv_obj_set_style_border_width(g_ctx.container, 0, 0);

    lv_timer_create(on_timer, 200, NULL);

    /* Check for saved token */
    if (baidu_oauth_has_token()) {
        baidu_oauth_load_token(g_ctx.access_token, sizeof(g_ctx.access_token),
                                g_ctx.refresh_token, sizeof(g_ctx.refresh_token));
        ui_set_state(UI_STATE_FILE_LIST);
        fetch_file_list();
    } else {
        ui_set_state(UI_STATE_INIT);
    }
}

/* ============  UI builder  ============ */

static void ui_clear_widgets(void)
{
    lv_obj_clean(g_ctx.container);
    g_ctx.login_btn  = NULL;
    g_ctx.spinner    = NULL;
    g_ctx.info_label = NULL;
    g_ctx.qr_image   = NULL;
    g_ctx.action_btn = NULL;
}

static void ui_set_state(ui_state_t s)
{
    g_ctx.state = s;
    ui_clear_widgets();

    switch (s) {
    case UI_STATE_INIT: {
        lv_obj_t *btn = lv_btn_create(g_ctx.container);
        lv_obj_set_size(btn, 200, 60);
        lv_obj_center(btn);
        lv_obj_add_event_cb(btn, on_login_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, "Login Baidu Netdisk");
        lv_obj_center(label);
        g_ctx.login_btn = btn;
        break;
    }
    case UI_STATE_LOADING: {
        lv_obj_t *spin = lv_spinner_create(g_ctx.container);
        lv_obj_set_size(spin, 60, 60);
        lv_obj_align(spin, LV_ALIGN_CENTER, 0, -40);
        lv_obj_t *label = lv_label_create(g_ctx.container);
        lv_label_set_text(label, "Fetching QR code...");
        lv_obj_align_to(label, spin, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
        g_ctx.spinner    = spin;
        g_ctx.info_label = label;
        break;
    }
    case UI_STATE_QR_SHOW: {
#if LV_USE_LODEPNG
        FILE *fp = fopen(QR_FILE_PATH, "wb");
        if (fp) {
            fwrite(g_ctx.qr_png_data, 1, g_ctx.qr_png_len, fp);
            fclose(fp);
            lv_obj_t *img = lv_image_create(g_ctx.container);
            lv_image_set_src(img, "A:" QR_FILE_PATH);
            /* Auto-size, then scale to max 300px */
            lv_image_header_t qr_hdr = {0};
            lv_image_decoder_get_info(QR_FILE_PATH, &qr_hdr);
            uint32_t zoom = 256;
            if (qr_hdr.w > 300)
                zoom = (256U * 300) / qr_hdr.w;
            if (qr_hdr.h > 300 && (256U * 300) / qr_hdr.h < zoom)
                zoom = (256U * 300) / qr_hdr.h;
            lv_image_set_scale(img, zoom);
            lv_obj_set_style_bg_color(img, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(img, 12, 0);
            lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 30);
            g_ctx.qr_image = img;
        }
#endif
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Scan with Baidu APP\nUser code: %s", g_ctx.user_code);
        lv_obj_t *label = lv_label_create(g_ctx.container);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        if (g_ctx.qr_image)
            lv_obj_align_to(label, g_ctx.qr_image, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
        else
            lv_obj_center(label);
        g_ctx.info_label = label;
        break;
    }
    case UI_STATE_FILE_LIST: {
        lv_obj_t *title = lv_label_create(g_ctx.container);
        lv_label_set_text(title, "Baidu Netdisk Files");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        g_ctx.info_label = title;

        /* Logout button top-right */
        lv_obj_t *logout = lv_btn_create(g_ctx.container);
        lv_obj_set_size(logout, 80, 32);
        lv_obj_align(logout, LV_ALIGN_TOP_RIGHT, -8, 8);
        lv_obj_add_event_cb(logout, on_logout_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lt = lv_label_create(logout);
        lv_label_set_text(lt, "Logout");
        lv_obj_center(lt);
        g_ctx.action_btn = logout;

        /* File list */
        lv_obj_t *plist = lv_list_create(g_ctx.container);
        lv_obj_set_size(plist, lv_pct(100), lv_pct(85));
        lv_obj_align(plist, LV_ALIGN_TOP_MID, 0, 50);
        g_ctx.spinner = plist; /* reuse spinner slot for list */
        break;
    }
    case UI_STATE_FAILED: {
        const char *msg = "Unknown error";
        switch (g_ctx.last_status) {
        case BAIDU_OAUTH_DECLINED: msg = "Authorization declined"; break;
        case BAIDU_OAUTH_EXPIRED:  msg = "QR code expired";        break;
        case BAIDU_OAUTH_ERROR:    msg = "Network error, retry";   break;
        default: break;
        }
        lv_obj_t *label = lv_label_create(g_ctx.container);
        lv_label_set_text(label, msg);
        lv_obj_set_style_text_color(label, lv_color_hex(0xCC0000), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, -40);
        lv_obj_t *btn = lv_btn_create(g_ctx.container);
        lv_obj_set_size(btn, 140, 44);
        lv_obj_align_to(btn, label, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
        lv_obj_add_event_cb(btn, on_login_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, "Retry");
        lv_obj_center(bl);
        g_ctx.info_label = label;
        g_ctx.action_btn = btn;
        break;
    }
    }
}

/* ============  event handlers  ============ */

static void on_login_click(lv_event_t *e)
{
    (void)e;
    free(g_ctx.qr_png_data); g_ctx.qr_png_data = NULL; g_ctx.qr_png_len = 0;
    memset(g_ctx.user_code, 0, sizeof(g_ctx.user_code));
    memset(g_ctx.access_token, 0, sizeof(g_ctx.access_token));
    memset(g_ctx.refresh_token, 0, sizeof(g_ctx.refresh_token));
    g_ctx.qr_ready = g_ctx.status_changed = g_ctx.list_ready = 0;
    ui_set_state(UI_STATE_LOADING);
    baidu_oauth_login_start(qr_cb, status_cb, NULL);
}

static void on_logout_click(lv_event_t *e)
{
    (void)e;
    baidu_oauth_clear_token();
    memset(g_ctx.access_token, 0, sizeof(g_ctx.access_token));
    memset(g_ctx.refresh_token, 0, sizeof(g_ctx.refresh_token));
    g_ctx.list_ready = 0;
    ui_set_state(UI_STATE_INIT);
}

static void on_download_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *fs_id = lv_obj_get_user_data(btn);
    if (!fs_id) return;
    /* Find file name from cached list by matching fs_id */
    const char *fname = "download";
    int n = list_cached_count;
    for (int i = 0; i < n; i++) {
        if (list_cached[i].fs_id && strcmp(list_cached[i].fs_id, fs_id) == 0) {
            fname = list_cached[i].name ? list_cached[i].name : list_cached[i].path;
            break;
        }
    }
    baidu_oauth_download_file(g_ctx.access_token, fs_id, fname, dl_cb, NULL);
}

static void on_view_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *fs_id = lv_obj_get_user_data(btn);
    if (!fs_id) return;
    int n = list_cached_count;
    for (int i = 0; i < n; i++) {
        if (list_cached[i].fs_id && strcmp(list_cached[i].fs_id, fs_id) == 0) {
            const char *nm = list_cached[i].name ? list_cached[i].name : "img";
            char path[512];
            snprintf(path, sizeof(path), "A:/mnt/sdcard/baidu-xkphoto/img/%s", nm);

            /* Check image size before decoding */
            lv_image_header_t hdr = {0};
            lv_image_decoder_get_info(path, &hdr);
            int32_t iw = hdr.w, ih = hdr.h;

            if (iw > 1920 || ih > 1920) {
                /* Too large — don't decode, show error */
                ui_clear_widgets();
                lv_obj_t *lbl = lv_label_create(g_ctx.container);
                lv_label_set_text_fmt(lbl, "%s\n%dx%d\nToo large for display!",
                                      nm, iw, ih);
                lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);
            } else {
                ui_clear_widgets();
                lv_obj_t *img = lv_image_create(g_ctx.container);
                lv_image_set_src(img, path);
                lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

                /* Auto-scale */
                if (iw > 0 && ih > 0) {
                    int32_t sw = lv_display_get_horizontal_resolution(NULL) - 16;
                    int32_t sh = lv_display_get_vertical_resolution(NULL) - 16;
                    uint32_t zoom = (iw * 256) / sw > (ih * 256) / sh
                        ? (uint32_t)((sw * 256) / iw)
                        : (uint32_t)((sh * 256) / ih);
                    if (zoom < 256)
                        lv_image_set_scale(img, zoom);
                }
            }

            /* Back: restore file list directly */
            lv_obj_t *bk = lv_btn_create(g_ctx.container);
            lv_obj_set_size(bk, 80, 40);
            lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -8, 8);
            lv_obj_add_event_cb(bk, on_dir_click, LV_EVENT_CLICKED, NULL);
            lv_obj_set_user_data(bk, strdup(g_ctx.current_dir));
            lv_obj_t *bt = lv_label_create(bk);
            lv_label_set_text(bt, LV_SYMBOL_LEFT " Back");
            lv_obj_center(bt);
            return;
        }
    }
}

static void on_dir_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *path = lv_obj_get_user_data(btn);
    if (!path) return;
    g_ctx.list_ready = 0;
    /* Save current directory before navigating */
    strncpy(g_ctx.current_dir, path, sizeof(g_ctx.current_dir) - 1);
    if (!g_ctx.spinner) {
        ui_set_state(UI_STATE_FILE_LIST);
    } else {
        lv_obj_clean(g_ctx.spinner);
    }
    baidu_oauth_list_files(g_ctx.access_token, path, list_cb, NULL);
}

static void fetch_file_list(void)
{
    if (!g_ctx.access_token[0]) return;
    baidu_oauth_list_files(g_ctx.access_token, "/", list_cb, NULL);
}

/* ============  curl-thread callbacks  ============ */

static void qr_cb(const uint8_t *png_data, size_t png_len,
                  const char *user_code, void *user_data)
{
    (void)user_data;
    g_ctx.qr_png_data = malloc(png_len);
    if (g_ctx.qr_png_data) {
        memcpy(g_ctx.qr_png_data, png_data, png_len);
        g_ctx.qr_png_len = png_len;
    }
    strncpy(g_ctx.user_code, user_code, sizeof(g_ctx.user_code) - 1);
    g_ctx.qr_ready = 1;
}

static void status_cb(baidu_oauth_status_t status,
                      const char *access, const char *refresh,
                      void *user_data)
{
    (void)user_data;
    g_ctx.last_status = status;
    if (status == BAIDU_OAUTH_SUCCESS) {
        if (access) {
            strncpy(g_ctx.access_token, access, sizeof(g_ctx.access_token) - 1);
            baidu_oauth_save_token(access, refresh ? refresh : "");
        }
        if (refresh)
            strncpy(g_ctx.refresh_token, refresh, sizeof(g_ctx.refresh_token) - 1);
    }
    g_ctx.status_changed = 1;
}

static void list_cb(const baidu_oauth_file_t *files, int count, void *user_data)
{
    (void)user_data;
    list_cached_count = count;
    for (int i = 0; i < count && i < 200; i++) {
        free(list_cached[i].path);
        free(list_cached[i].name);
        free(list_cached[i].fs_id);
        list_cached[i].path   = files[i].path ? strdup(files[i].path) : NULL;
        list_cached[i].name   = files[i].name ? strdup(files[i].name) : NULL;
        list_cached[i].fs_id  = files[i].fs_id ? strdup(files[i].fs_id) : NULL;
        list_cached[i].is_dir = files[i].is_dir;
        list_cached[i].size   = files[i].size;
    }
    g_ctx.list_ready = count;
}

static void dl_cb(baidu_dl_status_t st, const char *msg, void *user_data)
{
    (void)user_data;
    g_ctx.dl_status = st;
    if (msg)
        strncpy(g_ctx.dl_msg, msg, sizeof(g_ctx.dl_msg) - 1);
    g_ctx.dl_event = 1;
}

/* ============  LVGL timer (main thread)  ============ */

static void on_timer(lv_timer_t *t)
{
    (void)t;
    if (g_ctx.qr_ready) { g_ctx.qr_ready = 0; ui_set_state(UI_STATE_QR_SHOW); }

    if (g_ctx.status_changed) {
        g_ctx.status_changed = 0;
        switch (g_ctx.last_status) {
        case BAIDU_OAUTH_PENDING:
            if (g_ctx.info_label)
                lv_label_set_text(g_ctx.info_label, "Waiting for scan...");
            break;
        case BAIDU_OAUTH_SUCCESS:
            ui_set_state(UI_STATE_FILE_LIST);
            fetch_file_list();
            break;
        case BAIDU_OAUTH_DECLINED:
        case BAIDU_OAUTH_EXPIRED:
        case BAIDU_OAUTH_ERROR:
            ui_set_state(UI_STATE_FAILED);
            break;
        default: break;
        }
    }

    /* File list ready (from list_cb on curl thread) */
    if (g_ctx.list_ready > 0 && g_ctx.state == UI_STATE_FILE_LIST) {
        int n = g_ctx.list_ready;
        g_ctx.list_ready = 0;

        lv_obj_t *plist = g_ctx.spinner;
        if (!plist) {
            /* List was destroyed (e.g. by image view) — recreate */
            plist = lv_list_create(g_ctx.container);
            lv_obj_set_size(plist, lv_pct(100), lv_pct(85));
            lv_obj_align(plist, LV_ALIGN_TOP_MID, 0, 50);
            g_ctx.spinner = plist;
        }
        lv_obj_clean(plist);
        /* Back button */
            {
                lv_obj_t *back = lv_list_add_button(plist, NULL, LV_SYMBOL_LEFT " ..");
                lv_obj_add_event_cb(back, on_dir_click, LV_EVENT_CLICKED, NULL);
                lv_obj_set_user_data(back, (void *)"/");
            }

            for (int i = 0; i < n; i++) {
                if (!list_cached[i].path) continue;
                char buf[384];
                const char *nm = list_cached[i].name ?
                    list_cached[i].name : list_cached[i].path;
                if (list_cached[i].is_dir) {
                    snprintf(buf, sizeof(buf), LV_SYMBOL_DIRECTORY " %s/", nm);
                } else {
                    snprintf(buf, sizeof(buf), LV_SYMBOL_FILE " %s  %zu B",
                             nm, list_cached[i].size);
                }
                lv_obj_t *btn = lv_list_add_button(plist, NULL, buf);
                lv_obj_set_style_text_font(btn,
                    &lv_font_source_han_sans_sc_16_cjk, 0);
                lv_obj_add_event_cb(btn,
                    list_cached[i].is_dir ? on_dir_click : on_download_click,
                    LV_EVENT_CLICKED, NULL);
                lv_obj_set_user_data(btn,
                    strdup(list_cached[i].is_dir ?
                           list_cached[i].path : list_cached[i].fs_id));

                /* Add [View] only for already-downloaded images */
                if (!list_cached[i].is_dir) {
                    const char *nm2 = list_cached[i].name ?
                        list_cached[i].name : "";
                    int ilen = strlen(nm2);
                    int is_img = (ilen > 4 && (strcasecmp(nm2 + ilen - 4, ".jpg") == 0
                        || strcasecmp(nm2 + ilen - 4, ".png") == 0))
                        || (ilen > 5 && strcasecmp(nm2 + ilen - 5, ".jpeg") == 0);
                    if (is_img) {
                        char lpath[512];
                        snprintf(lpath, sizeof(lpath),
                                 "/mnt/sdcard/baidu-xkphoto/img/%s", nm2);
                        if (access(lpath, R_OK) == 0) {
                            lv_obj_t *vbtn = lv_list_add_button(plist, NULL,
                                "   -> [View]");
                            lv_obj_add_event_cb(vbtn, on_view_click,
                                LV_EVENT_CLICKED, NULL);
                            lv_obj_set_user_data(vbtn, strdup(list_cached[i].fs_id));
                        }
                    }
                }
            }
    }

    /* Download event */
    if (g_ctx.dl_event) {
        g_ctx.dl_event = 0;
        if (g_ctx.info_label) {
            char buf[320];
            const char *st = "";
            switch (g_ctx.dl_status) {
            case BAIDU_DL_PROGRESS: st = "Downloading"; break;
            case BAIDU_DL_SUCCESS:  st = "OK"; break;
            case BAIDU_DL_ERROR:    st = "ERR"; break;
            case BAIDU_DL_COMPLETE: st = "Done"; break;
            }
            snprintf(buf, sizeof(buf), "%s: %s", st, g_ctx.dl_msg);
            lv_label_set_text(g_ctx.info_label, buf);
        }
    }
}

#endif /* LV_USE_DEMO_WIDGETS */

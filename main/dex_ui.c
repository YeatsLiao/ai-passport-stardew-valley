// main/dex_ui.c —— 星露谷图鉴界面:分类页 → 列表页 → 详情页。
//
// 线程模型(对齐 ai-passport 官方纪律):
//   - 按键回调(main.c 持 LVGL 锁)只改 RAM 状态与页面。
//   - 图片 inflate/缩放与 NVS 落盘在 worker 任务;解码结果经双缓冲 + 就绪
//     标志交给 LVGL 任务内的轮询定时器上屏,worker 不触碰任何 lv_* 对象。
//   - NVS 写入按 2s 空闲去抖:连续翻页只落盘最后一次位置。
//
// 按键:
//   分类页  UP/DN 移动  OK 进入            LONG:无
//   列表页  UP/DN 滚动  LONG ±10  OK 查看   OK LONG 返回分类
//   详情页  UP/DN ±1    LONG ±10           DOUBLE 跳首/末
//           OK LONG 返回列表
#include "dex_ui.h"

#include "dex_battery.h"
#include "dex_core.h"
#include "dex_layout.h"
#include "dex_sprite.h"
#include "dex_audio.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "lvgl.h"

#include <string.h>

static const char *TAG = "dex_ui";

#define DEX_NVS_NS   "dex"
#define DEX_NVS_CAT  "cat"
#define DEX_NVS_IDX  "idx"

// ---- 星露谷羊皮纸配色(与生成管线的预混背景 #F4F0E0 一致) ----
#define CS_BG     0xF4F0E0u
#define CS_PANEL  0xEDE5CEu
#define CS_WOOD   0x8B5A2Bu
#define CS_WOOD_D 0x5E3C1Cu
#define CS_INK    0x3B2F1Eu
#define CS_DIM    0x9A8C74u
#define CS_GREEN  0x5C8A3Au
#define CS_HINT   0xF7F2E2u

#define WORKER_STACK   6144
#define WORKER_PRIO    5
#define ROLLER_BUF     8192
#define ATTR_ROW_MAX   DEX_ATTR_ROWS

typedef enum { PAGE_CATEGORY = 0, PAGE_LIST, PAGE_DETAIL } page_t;
typedef enum { MSG_SPRITE = 0, MSG_SAVE } msg_type_t;
typedef struct {
    msg_type_t type;
    uint16_t a; /* MSG_SPRITE: sprite_idx; MSG_SAVE: cat */
    uint16_t b; /* MSG_SAVE: idx */
} dex_msg_t;

static page_t s_page = PAGE_CATEGORY;

/* 位置状态 */
static uint16_t s_cat;         /* 当前类别 */
static uint16_t s_idx;         /* 类别内当前条目 */
static int s_sel;              /* 分类页当前选中 */

/* 分类页 */
static lv_obj_t *s_cat_scr;
static lv_obj_t *s_cat_grid;
static lv_obj_t *s_card[32];   /* DEX_CATEGORY_COUNT == 30 */
static lv_obj_t *s_cat_bat;

/* 列表页 */
static lv_obj_t *s_list_scr;
static lv_obj_t *s_roller;
static lv_obj_t *s_list_bat;
static char s_roller_buf[ROLLER_BUF];

/* 详情页 */
static lv_obj_t *s_det_scr;
static lv_obj_t *s_det_bat;
static lv_obj_t *s_pos_l;
static lv_obj_t *s_img;
static lv_obj_t *s_img_hint;
static lv_obj_t *s_name_l;
/* 信息面板:视口 + 内部完整文本(不省略),超高时自动垂直循环滚动 */
static lv_obj_t *s_info_view;
#define INFO_GROUP_GAP 24         /* 循环滚动中两组内容间隔(px) */
static lv_obj_t *s_desc_l[2];      /* 描述,内容渲染两份供无缝循环滚动 */
static lv_obj_t *s_attr_l[2][ATTR_ROW_MAX];   /* 属性名,CS_DIM,宽自适应 */
static lv_obj_t *s_attr_v[2][ATTR_ROW_MAX];   /* 属性值,CS_INK,同行紧随其右 */
static lv_timer_t *s_scroll_timer;
static int32_t s_scroll_max;          /* 面板最大滚动量(px),0=无需滚动 */
static dex_layout_t s_lay;

/* worker:解码双缓冲 + 消息队列 */
static QueueHandle_t s_q;
static TaskHandle_t s_worker;
static uint16_t s_pixbuf[2][DEX_SPRITE_MAX_W * DEX_SPRITE_MAX_H];
/* 每块缓冲配一个独立图像描述符:worker 只写"后备"描述符,LVGL 只读
 * "就绪"描述符,消除单描述符被跨线程改写导致的撕裂。图片缓存已关闭
 * (CONFIG_LV_CACHE_DEF_SIZE=0),set_src 每次都重读 header,无短路。
 * 就绪用递增序列号 s_seq 提交:worker 每解码一帧 ++,poll 比较序列号
 * 消费,避免布尔标志在快速连点下丢帧(表现为切换后图片不显示)。 */
static lv_image_dsc_t s_dsc[2];
static volatile uint8_t s_ready_idx;  /* 最后提交帧对应的描述符 */
static volatile uint32_t s_seq;       /* 每提交一帧 +1 */
static volatile bool s_gen_odd;       /* 交替使用两块缓冲 */

static lv_timer_t *s_poll_timer;

/* NVS 去抖暂存(worker 内访问) */
static uint16_t s_save_cat, s_save_idx;
static bool s_save_dirty;

// ---------------------------------------------------------------------------
// worker:图片解码 + NVS 持久化(不触碰 LVGL)
// ---------------------------------------------------------------------------
static void nvs_save_pos(uint16_t cat, uint16_t idx)
{
    nvs_handle_t h;
    if (nvs_open(DEX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, DEX_NVS_CAT, cat);
    nvs_set_u16(h, DEX_NVS_IDX, idx);
    nvs_commit(h);
    nvs_close(h);
}

static void worker_main(void *arg)
{
    (void)arg;
    dex_msg_t m;
    for (;;) {
        bool have = xQueueReceive(s_q, &m, pdMS_TO_TICKS(2000)) == pdTRUE;

        if (have && m.type == MSG_SAVE) {
            s_save_cat = m.a;
            s_save_idx = m.b;
            s_save_dirty = true;
        }

        if (have && m.type == MSG_SPRITE) {
            uint16_t idx = m.a;             /* 索引随消息传入,无全局判重 */
            uint16_t k = s_gen_odd ? 1 : 0; /* 后备缓冲/描述符 */
            uint16_t *buf = s_pixbuf[k];
            uint32_t w = 0, h = 0;
            /* 解码最多重试 2 次(共 3 次):同一张图同一索引偶发 TINFL_STATUS_FAILED
             * 多为 SPI flash 映射瞬间缓存未命中,小延迟(vTaskDelay 1 tick)
             * 重试通常可恢复;dex_sprite.c 已把 out_bytes 改为 out_cap_u16*2
             * 给足回溯字典空间,此处为额外兑底。 */
            int retries = 0;
            bool ok = false;
            if (idx != DEX_SPRITE_NONE) {
                for (; retries < 3; retries++) {
                    ok = dex_sprite_render(idx, buf,
                                           DEX_SPRITE_MAX_W * DEX_SPRITE_MAX_H,
                                           &w, &h);
                    if (ok) break;
                    vTaskDelay(1);
                }
            }
            if (ok) {
                /* 小图 2x 最近邻放大到 ≤96,再居中交给 UI */
                if (w <= 48 && h <= 48 &&
                    (w * 2 <= DEX_SPRITE_MAX_W && h * 2 <= DEX_SPRITE_MAX_H)) {
                    uint32_t ow, oh;
                    if (dex_sprite_scale2x(buf, w, h, buf,
                                           DEX_SPRITE_MAX_W * DEX_SPRITE_MAX_H,
                                           &ow, &oh)) {
                        w = ow;
                        h = oh;
                    }
                }
                lv_image_dsc_t *d = &s_dsc[k];
                memset(d, 0, sizeof(*d));
                d->header.magic = LV_IMAGE_HEADER_MAGIC;
                d->header.cf = LV_COLOR_FORMAT_RGB565;
                d->header.w = w;
                d->header.h = h;
                /* LVGL 9 必须给 stride,为 0 时图片根本不渲染。
                 * lv_draw_buf_width_to_stride 是纯计算函数,不触 LVGL
                 * 全局状态,worker 线程调用安全。 */
                d->header.stride =
                    lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
                d->data = (const uint8_t *)buf;
                d->data_size = (size_t)w * h * 2;
            } else {
                /* 无图/坏图:cf=0 → UI 显示占位提示 */
                memset(&s_dsc[k], 0, sizeof(s_dsc[k]));
            }
            s_ready_idx = (uint8_t)k;
            s_gen_odd = !s_gen_odd;
            s_seq++;                        /* 提交:poll 凭序列号感知新帧 */
            ESP_LOGI(TAG, "sprite %u -> %s %lux%lu buf%u retries=%d", (unsigned)idx,
                     ok ? "ok" : "none", (unsigned long)w, (unsigned long)h,
                     (unsigned)k, retries);
        }

        if (!have && s_save_dirty) { /* 2s 无消息:去抖落盘 */
            nvs_save_pos(s_save_cat, s_save_idx);
            s_save_dirty = false;
        }
    }
}

static void request_save_pos(uint16_t cat, uint16_t idx)
{
    dex_msg_t m = {.type = MSG_SAVE, .a = cat, .b = idx};
    if (s_q) xQueueSend(s_q, &m, 0);
}

static void request_sprite(uint16_t sprite_idx)
{
    /* 索引随消息传入,worker 逐条解码,不再用全局 pending 判重,
     * 彻底消除"快速切换时 request 被丢弃导致图片不显示"。 */
    dex_msg_t m = {.type = MSG_SPRITE, .a = sprite_idx};
    if (s_q) xQueueSend(s_q, &m, 0);
}

/* LVGL 任务内轮询:把解码完成的缓冲挂上屏 */
static void poll_tick(lv_timer_t *t)
{
    (void)t;
    static uint32_t last_seq;
    if (!s_img || s_seq == last_seq) return;
    last_seq = s_seq;

    lv_image_dsc_t *d = &s_dsc[s_ready_idx];
    if (d->header.cf != LV_COLOR_FORMAT_RGB565) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_img_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_img_hint, "NO IMAGE");
        return;
    }
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_img_hint, LV_OBJ_FLAG_HIDDEN);
    /* 图片缓存已关闭,且每块缓冲用独立描述符,set_src 会重新读取
     * header,无需 cache_drop。 */
    lv_image_set_src(s_img, d);
    /* 在图井内区(sprite.w x sprite.h = 96x96)居中 */
    int16_t ox = (int16_t)(s_lay.sprite.x +
                           (s_lay.sprite.w - (int16_t)d->header.w) / 2);
    int16_t oy = (int16_t)(s_lay.sprite.y +
                           (s_lay.sprite.h - (int16_t)d->header.h) / 2);
    lv_obj_set_pos(s_img, ox, oy);
}

// ---------------------------------------------------------------------------
// 页面公共部件
// ---------------------------------------------------------------------------
static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(CS_BG), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

static void make_header(lv_obj_t *scr, const char *title,
                        lv_obj_t **out_title, lv_obj_t **out_bat)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 24);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CS_WOOD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_pos(bar, 0, 0);

    lv_obj_t *t = lv_label_create(bar);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(CS_HINT), 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 8, 5);
    if (out_title) *out_title = t;

    lv_obj_t *b = lv_label_create(bar);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(CS_HINT), 0);
    lv_label_set_text(b, "--");
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -8, 0);
    if (out_bat) *out_bat = b;
}

static void make_hint(lv_obj_t *scr, const char *text)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 18);
    lv_obj_set_pos(bar, 0, 302);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CS_WOOD_D), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(bar);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(CS_HINT), 0);
    lv_obj_set_size(l, 232, 16);          /* 定宽定高,超出部分省略号 */
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

// ---------------------------------------------------------------------------
// 分类页
// ---------------------------------------------------------------------------
static void cat_refresh(void)
{
    for (int i = 0; i < (int)DEX_CATEGORY_COUNT; i++) {
        bool sel = (i == s_sel);
        lv_obj_set_style_bg_color(s_card[i],
            lv_color_hex(sel ? CS_GREEN : CS_PANEL), 0);
        lv_obj_set_style_border_color(s_card[i],
            lv_color_hex(sel ? CS_GREEN : CS_WOOD), 0);
        lv_obj_t *l = lv_obj_get_child(s_card[i], 0);
        lv_obj_set_style_text_color(l,
            lv_color_hex(sel ? CS_HINT : CS_INK), 0);
    }
}

static void build_category_page(void)
{
    s_page = PAGE_CATEGORY;
    s_cat_scr = make_screen();
    make_header(s_cat_scr, "Stardew Valley", NULL, &s_cat_bat);
    dex_battery_attach(s_cat_bat);

    s_cat_grid = lv_obj_create(s_cat_scr);
    lv_obj_remove_style_all(s_cat_grid);
    lv_obj_set_size(s_cat_grid, 236, 276);
    lv_obj_set_pos(s_cat_grid, 2, 26);
    lv_obj_add_flag(s_cat_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_cat_grid, LV_DIR_VER);
    lv_obj_set_style_pad_all(s_cat_grid, 2, 0);

    /* BGM: Country Shop (track 0) for browsing */
    dex_audio_play(0);

    for (int i = 0; i < (int)DEX_CATEGORY_COUNT; i++) {
        int x = (i % 2) * 116;
        int y = (i / 2) * 40;
        lv_obj_t *card = lv_obj_create(s_cat_grid);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 112, 36);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_style_radius(card, 4, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_t *l = lv_label_create(card);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_width(l, 104);         /* 卡宽 112,留边框余量 */
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_label_set_text_fmt(l, "%s %d", dex_category_name(i),
                              (int)dex_category_len(i));
        lv_obj_center(l);
        s_card[i] = card;
    }

    make_hint(s_cat_scr, "UP/DN MOVE    OK OPEN");
    cat_refresh();
    lv_obj_scroll_to_view(s_card[s_sel], LV_ANIM_OFF);
    lv_screen_load(s_cat_scr);
}

// ---------------------------------------------------------------------------
// 列表页
// ---------------------------------------------------------------------------
static void build_list_page(void)
{
    s_page = PAGE_LIST;
    uint16_t n = dex_category_len(s_cat);

    size_t used = 0;
    s_roller_buf[0] = '\0';
    for (uint16_t i = 0; i < n; i++) {
        const dex_entry_t *e = dex_entry_at(s_cat, i);
        if (!e) continue;
        size_t len = strlen(e->name);
        if (used + len + 2 >= sizeof(s_roller_buf)) break;
        memcpy(&s_roller_buf[used], e->name, len);
        used += len;
        s_roller_buf[used++] = '\n';
    }
    if (used) s_roller_buf[used - 1] = '\0'; /* 去掉末尾换行 */

    s_list_scr = make_screen();
    char title[32];
    snprintf(title, sizeof(title), "%s (%u)", dex_category_name(s_cat),
             (unsigned)n);
    make_header(s_list_scr, title, NULL, &s_list_bat);
    dex_battery_attach(s_list_bat);

    s_roller = lv_roller_create(s_list_scr);
    lv_roller_set_options(s_roller, s_roller_buf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller, 10);
    lv_obj_set_width(s_roller, 224);
    lv_obj_set_pos(s_roller, 8, 30);
    lv_obj_set_style_bg_color(s_roller, lv_color_hex(CS_PANEL), 0);
    lv_obj_set_style_border_color(s_roller, lv_color_hex(CS_WOOD), 0);
    lv_obj_set_style_text_color(s_roller, lv_color_hex(CS_INK), 0);
    lv_obj_set_style_text_color(s_roller, lv_color_hex(CS_HINT),
                                LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_roller, lv_color_hex(CS_GREEN),
                              LV_PART_SELECTED);

    make_hint(s_list_scr, "OK VIEW  HOLD +-10  OK BACK");
    lv_screen_load(s_list_scr);
    if (n) lv_roller_set_selected(s_roller, s_idx, LV_ANIM_OFF);

    /* BGM: Country Shop (track 0) for browsing */
    dex_audio_play(0);
}

// ---------------------------------------------------------------------------
// 详情页
// ---------------------------------------------------------------------------
/* 信息面板循环滚动:内容渲染两份(组A+组B,间隔 INFO_GROUP_GAP),
 * 以约 33px/s 匀速下移;滚过一个周期(组A高+间隔)时瞬时回卷到顶部,
 * 因组B开头与组A完全相同,画面无跳变,观感为连续循环。内容不足
 * 一屏则不滚。 */
static void scroll_tick(lv_timer_t *t)
{
    (void)t;
    if (s_page != PAGE_DETAIL || !s_info_view || s_scroll_max <= 0) return;
    if (lv_obj_get_scroll_y(s_info_view) >= s_scroll_max)
        lv_obj_scroll_to_y(s_info_view, 0, LV_ANIM_OFF); /* 周期点无缝回卷 */
    else
        lv_obj_scroll_by(s_info_view, 0, -1, LV_ANIM_OFF);
}

static void apply_entry(void)
{
    const dex_entry_t *e = dex_entry_at(s_cat, s_idx);
    if (!e) return;

    lv_label_set_text_fmt(s_pos_l, "%u/%u", (unsigned)(s_idx + 1),
                          (unsigned)dex_category_len(s_cat));
    lv_label_set_text(s_name_l, e->name);

    dex_attr_text_t attrs[ATTR_ROW_MAX];
    size_t na = dex_entry_attrs_text(e, attrs, ATTR_ROW_MAX);

    /* 内容渲染两份(组A+组B):先统一设文本,再一次性更新布局,
     * 最后按实测高度流式排布;属性名/值均完整显示,绝不省略。
     * 组A排完后得到周期 cycle = 组A高 + INFO_GROUP_GAP,
     * 滚到 cycle 时瞬时回零即无缝(组B与组A内容完全相同)。 */
    int cycle = 0;
    for (int g = 0; g < 2; g++) {
        int y = cycle;
        lv_label_set_text(s_desc_l[g], e->desc[0] ? e->desc : " ");
        for (int i = 0; i < ATTR_ROW_MAX; i++) {
            if (i < (int)na) {
                lv_obj_remove_flag(s_attr_l[g][i], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text_fmt(s_attr_l[g][i], "%s:", attrs[i].label);
                if (attrs[i].value[0]) {
                    lv_obj_remove_flag(s_attr_v[g][i], LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(s_attr_v[g][i], attrs[i].value);
                } else {
                    lv_obj_add_flag(s_attr_v[g][i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_obj_add_flag(s_attr_l[g][i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_attr_v[g][i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_update_layout(s_info_view);

        lv_obj_set_pos(s_desc_l[g], 12, (int16_t)y);
        y += lv_obj_get_height(s_desc_l[g]) + 8;
        for (int i = 0; i < ATTR_ROW_MAX; i++) {
            if (lv_obj_has_flag(s_attr_l[g][i], LV_OBJ_FLAG_HIDDEN)) continue;
            /* 名称宽度自适应(完整不截断),值紧随其右同行接排;
             * 值折行后从名称右缘起悬挂缩进,行高取两者较大者 */
            int lw = lv_obj_get_width(s_attr_l[g][i]);
            int vw = 216 - lw - 4;
            if (vw < 60) vw = 60;
            lv_obj_set_width(s_attr_v[g][i], (int32_t)vw);
            lv_obj_update_layout(s_info_view);
            int lh = lv_obj_get_height(s_attr_l[g][i]);
            int vh = lv_obj_has_flag(s_attr_v[g][i], LV_OBJ_FLAG_HIDDEN)
                         ? 0 : lv_obj_get_height(s_attr_v[g][i]);
            lv_obj_set_pos(s_attr_l[g][i], 12, (int16_t)y);
            if (vh) lv_obj_set_pos(s_attr_v[g][i], 12 + lw + 4, (int16_t)y);
            y += (vh > lh ? vh : lh) + 6;
        }
        if (g == 0) cycle = y + INFO_GROUP_GAP - 6; /* 组内末距已加6,补足间隔 */
    }

    lv_obj_update_layout(s_info_view);
    lv_obj_scroll_to_y(s_info_view, 0, LV_ANIM_OFF);
    int32_t view_h = lv_obj_get_height(s_info_view);
    s_scroll_max = cycle > view_h ? cycle : 0; /* 不足一屏则不滚 */

    /* 不再预隐藏图片:旧图保留到新图解码完成,由 poll 无缝替换,
     * 消除"切换瞬间空白/卡在 ..."的不显示观感。仅真正无图时 poll 显示提示。 */
    request_save_pos(s_cat, s_idx);
    request_sprite(e->sprite_idx);
}

static void build_detail_page(void)
{
    s_page = PAGE_DETAIL;
    dex_layout_build(&s_lay);

    s_det_scr = make_screen();

    char title[32];
    snprintf(title, sizeof(title), "%s", dex_category_name(s_cat));
    lv_obj_t *title_l;
    make_header(s_det_scr, title, &title_l, &s_det_bat);
    dex_battery_attach(s_det_bat);

    /* 图片井 */
    lv_obj_t *frame = lv_obj_create(s_det_scr);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, s_lay.sprite_frame.x, s_lay.sprite_frame.y);
    lv_obj_set_size(frame, s_lay.sprite_frame.w, s_lay.sprite_frame.h);
    lv_obj_set_style_bg_color(frame, lv_color_hex(CS_PANEL), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(CS_WOOD), 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_radius(frame, 4, 0);

    s_img = lv_image_create(s_det_scr);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    s_img_hint = lv_label_create(s_det_scr);
    lv_obj_set_style_text_font(s_img_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_img_hint, lv_color_hex(CS_DIM), 0);
    lv_label_set_text(s_img_hint, "...");
    /* 占位提示在图井内居中 */
    lv_obj_align(s_img_hint, LV_ALIGN_TOP_MID, 0,
                 s_lay.sprite_frame.y + (s_lay.sprite_frame.h - 16) / 2);

    /* 名称/描述 */
    s_name_l = lv_label_create(s_det_scr);
    lv_obj_set_style_text_font(s_name_l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_name_l, lv_color_hex(CS_INK), 0);
    lv_obj_set_width(s_name_l, s_lay.name.w);
    lv_label_set_long_mode(s_name_l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_name_l, s_lay.name.x, s_lay.name.y);
    lv_obj_set_style_text_align(s_name_l, LV_TEXT_ALIGN_CENTER, 0);

    /* 信息面板:描述+属性的完整内容(不省略、不打点),文本自适应高度,
     * 总高超出视口时由 scroll_tick 自动垂直循环滚动 */
    s_info_view = lv_obj_create(s_det_scr);
    lv_obj_remove_style_all(s_info_view);
    lv_obj_set_pos(s_info_view, s_lay.info.x, s_lay.info.y);
    lv_obj_set_size(s_info_view, s_lay.info.w, s_lay.info.h);
    lv_obj_add_flag(s_info_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_info_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_info_view, LV_SCROLLBAR_MODE_OFF);

    for (int g = 0; g < 2; g++) {
        s_desc_l[g] = lv_label_create(s_info_view);
        lv_obj_set_style_text_font(s_desc_l[g], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_desc_l[g], lv_color_hex(CS_DIM), 0);
        lv_obj_set_width(s_desc_l[g], 216);  /* 定宽,高度随内容自适应 */
        lv_label_set_long_mode(s_desc_l[g], LV_LABEL_LONG_WRAP);

        for (int i = 0; i < ATTR_ROW_MAX; i++) {
            /* 属性名:CS_DIM 弱色,宽度自适应完整显示,不截断 */
            s_attr_l[g][i] = lv_label_create(s_info_view);
            lv_obj_set_style_text_font(s_attr_l[g][i],
                                       &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(s_attr_l[g][i],
                                        lv_color_hex(CS_DIM), 0);
            lv_label_set_long_mode(s_attr_l[g][i], LV_LABEL_LONG_WRAP);

            /* 属性值:CS_INK 强色,宽度在 apply_entry 按名称实际宽动态设定 */
            s_attr_v[g][i] = lv_label_create(s_info_view);
            lv_obj_set_style_text_font(s_attr_v[g][i],
                                       &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(s_attr_v[g][i],
                                        lv_color_hex(CS_INK), 0);
            lv_obj_set_width(s_attr_v[g][i], 216);
            lv_label_set_long_mode(s_attr_v[g][i], LV_LABEL_LONG_WRAP);
        }
    }

    /* 位置 chip 放在标题条内,紧贴类别名右侧(间距 8px)。
     * 最长类别名 "Artisan Goods" + chip 最宽 "999/999" 时右缘约 161px,
     * 不会碰到电池(左缘约 166px)。 */
    s_pos_l = lv_label_create(lv_obj_get_parent(title_l));
    lv_obj_set_style_text_font(s_pos_l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pos_l, lv_color_hex(CS_PANEL), 0);
    lv_obj_update_layout(title_l);
    lv_obj_align_to(s_pos_l, title_l, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    make_hint(s_det_scr, "UP/DN +-1  HOLD +-10  OK BACK");
    lv_screen_load(s_det_scr);

    /* BGM: Library & Museum (track 1) for detail view */
    dex_audio_play(1);

    apply_entry();
}

static void move_sel(int delta)
{
    s_sel = (s_sel + delta + (int)DEX_CATEGORY_COUNT) % (int)DEX_CATEGORY_COUNT;
    cat_refresh();
    lv_obj_scroll_to_view(s_card[s_sel], LV_ANIM_ON);
}

static void leave_detail(void)
{
    lv_obj_delete(s_det_scr);
    s_det_scr = NULL;
    s_img = NULL;
    s_info_view = NULL;
    s_scroll_max = 0;
    build_list_page();
}

static void list_move(int delta)
{
    uint16_t n = dex_category_len(s_cat);
    if (!n) return;
    int idx = (int)s_idx + delta;
    if (idx < 0) idx = 0;
    if (idx > (int)n - 1) idx = n - 1;
    s_idx = (uint16_t)idx;
    lv_roller_set_selected(s_roller, s_idx, LV_ANIM_ON);
}

static void detail_move(int delta)
{
    uint16_t n = dex_category_len(s_cat);
    if (!n) return;
    int idx = (int)s_idx + delta;
    if (idx < 0) idx = 0;
    if (idx > (int)n - 1) idx = n - 1;
    if (idx == (int)s_idx) return;
    s_idx = (uint16_t)idx;
    apply_entry();
}

void dex_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    dex_battery_activity();

    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_DOUBLE && ev != BSP_BTN_LONG)
        return;

    if (s_page == PAGE_CATEGORY) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) move_sel(-1);
        if (btn == BSP_BTN_DOWN) move_sel(+1);
        if (btn == BSP_BTN_OK) {
            s_cat = (uint16_t)s_sel;
            s_idx = 0;
            lv_obj_delete(s_cat_scr);
            s_cat_scr = NULL;
            build_list_page();
        }
    } else if (s_page == PAGE_LIST) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            lv_obj_delete(s_list_scr);
            s_list_scr = NULL;
            build_category_page();
            return;
        }
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) list_move(-1);
            if (btn == BSP_BTN_DOWN) list_move(+1);
            if (btn == BSP_BTN_OK) {
                s_idx = lv_roller_get_selected(s_roller);
                lv_obj_delete(s_list_scr);
                s_list_scr = NULL;
                build_detail_page();
            }
        } else if (ev == BSP_BTN_LONG) {   /* 长按 = ±10,快速翻页 */
            if (btn == BSP_BTN_UP) list_move(-10);
            if (btn == BSP_BTN_DOWN) list_move(+10);
        }
    } else { /* PAGE_DETAIL */
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            leave_detail();
            return;
        }
        if (ev == BSP_BTN_CLICK) {          /* 单击 = ±1 */
            if (btn == BSP_BTN_UP) detail_move(-1);
            if (btn == BSP_BTN_DOWN) detail_move(+1);
        } else if (ev == BSP_BTN_LONG) {    /* 长按 = ±10(不依赖双击) */
            if (btn == BSP_BTN_UP) detail_move(-10);
            if (btn == BSP_BTN_DOWN) detail_move(+10);
        } else if (ev == BSP_BTN_DOUBLE) {  /* 双击 = 跳首/末(锦上添花) */
            uint16_t n = dex_category_len(s_cat);
            if (btn == BSP_BTN_UP && n) { s_idx = 0; apply_entry(); }
            if (btn == BSP_BTN_DOWN && n) { s_idx = n - 1; apply_entry(); }
            if (btn == BSP_BTN_OK) { dex_audio_toggle_mute(); }  /* OK 双击静音切换 */
        }
    }
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
static void load_last_pos(uint16_t *cat, uint16_t *idx)
{
    nvs_handle_t h;
    if (nvs_open(DEX_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u16(h, DEX_NVS_CAT, cat);
    nvs_get_u16(h, DEX_NVS_IDX, idx);
    nvs_close(h);
}

void dex_ui_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    s_q = xQueueCreate(8, sizeof(dex_msg_t));
    if (s_q &&
        xTaskCreate(worker_main, "dex_work", WORKER_STACK, NULL,
                    WORKER_PRIO, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "worker 创建失败");
    }

    uint16_t cat = 0, idx = 0;
    load_last_pos(&cat, &idx);
    if (cat >= DEX_CATEGORY_COUNT) cat = 0;
    if (idx >= dex_category_len(cat)) idx = 0;
    s_sel = cat;
    s_cat = cat;
    s_idx = idx;

    dex_battery_timers_start();
    s_poll_timer = lv_timer_create(poll_tick, 30, NULL);
    s_scroll_timer = lv_timer_create(scroll_tick, 30, NULL);

    build_category_page();
}

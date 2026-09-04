// main/dex_layout.h —— 图鉴 240x320 布局几何(纯 C,不依赖 ESP-IDF/LVGL)。
// 详情页:顶部标题条、右上/居中图片井、名称、描述区、属性行、底部按键提示。
// 所有矩形必须落在屏内且兄弟区域不相交(几何约束便于宿主校验)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEX_SCREEN_W 240
#define DEX_SCREEN_H 320
#define DEX_ATTR_ROWS 5

typedef struct {
    int16_t x, y, w, h;
} dex_rect_t;

typedef struct {
    dex_rect_t header;       /* 顶部标题条 */
    dex_rect_t header_rule;  /* 标题条下分隔线 */
    dex_rect_t title;        /* 页面标题(分类名) */
    dex_rect_t battery;      /* 电量,右对齐 */
    dex_rect_t sprite_frame; /* 图片井外框 */
    dex_rect_t sprite;       /* 图片实际区域(井内) */
    dex_rect_t name;         /* 条目名 */
    dex_rect_t desc;         /* 描述,≤3 行 */
    dex_rect_t attr_label[DEX_ATTR_ROWS]; /* 属性行左列 */
    dex_rect_t attr_value[DEX_ATTR_ROWS]; /* 属性行右列 */
    dex_rect_t hint;         /* 底部按键提示条 */
} dex_layout_t;

// 填入固定骨架。调用方直接拿矩形去放 LVGL 对象。
void dex_layout_build(dex_layout_t *out);

bool dex_rect_in_bounds(dex_rect_t r, int w, int h);
bool dex_rect_overlaps(dex_rect_t a, dex_rect_t b);
bool dex_rect_contains(dex_rect_t outer, dex_rect_t inner);

// 超长文本截断加 "..."。返回写入长度(不含 NUL)。
size_t dex_clip(char *dst, size_t dst_cap, const char *src, size_t max_chars);

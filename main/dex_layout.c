// main/dex_layout.c —— 图鉴 240x320 布局几何(纯 C)。
#include "dex_layout.h"

#include <string.h>

void dex_layout_build(dex_layout_t *o)
{
    *o = (dex_layout_t){0};

    o->header = (dex_rect_t){0, 0, 240, 24};
    o->header_rule = (dex_rect_t){0, 24, 240, 2};
    o->title = (dex_rect_t){8, 5, 120, 16};
    o->battery = (dex_rect_t){186, 5, 46, 16};

    /* 垂直预算(总高 320):
     *   header 0..24  图井 26..126  名称 128..150
     *   信息面板 152..298(描述+属性完整内容,超高自动循环滚动)
     *   提示条 302..320
     * 图井外框 100x100(含 2px 边框),内区恰好 96x96,
     * 与 DEX_SPRITE_MAX_W/H 一致,96x96 大图正好填满不溢出。 */
    o->sprite_frame = (dex_rect_t){70, 26, 100, 100};
    o->sprite = (dex_rect_t){72, 28, 96, 96};

    o->name = (dex_rect_t){0, 128, 240, 22};
    o->info = (dex_rect_t){0, 152, 240, 146};

    o->hint = (dex_rect_t){0, 302, 240, 18};
}

bool dex_rect_in_bounds(dex_rect_t r, int w, int h)
{
    return r.x >= 0 && r.y >= 0 && r.w >= 0 && r.h >= 0 &&
           r.x + r.w <= w && r.y + r.h <= h;
}

bool dex_rect_overlaps(dex_rect_t a, dex_rect_t b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

bool dex_rect_contains(dex_rect_t outer, dex_rect_t inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

size_t dex_clip(char *dst, size_t dst_cap, const char *src, size_t max_chars)
{
    if (!dst || dst_cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    size_t n = 0;
    while (src[n] && n < max_chars) n++;
    if (n < max_chars) {
        if (n >= dst_cap) n = dst_cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
        return n;
    }
    size_t keep = max_chars > 3 ? max_chars - 3 : 0;
    if (keep + 3 >= dst_cap) keep = dst_cap > 4 ? dst_cap - 4 : 0;
    memcpy(dst, src, keep);
    dst[keep] = '.'; dst[keep + 1] = '.'; dst[keep + 2] = '.';
    dst[keep + 3] = '\0';
    return keep + 3;
}

// main/dex_core.c —— 星露谷图鉴数据访问(纯 C)。
#include "dex_core.h"

#include <string.h>

// dex_attrs.bin 由 CMake 嵌入;条目 attrs_off 即池内字节偏移。
extern const uint8_t _binary_dex_attrs_bin_start[];
extern const uint8_t _binary_dex_attrs_bin_end[];

uint16_t dex_num_categories(void) { return (uint16_t)DEX_CATEGORY_COUNT; }

const char *dex_category_name(uint16_t cat)
{
    if (cat >= DEX_CATEGORY_COUNT) return "";
    return dex_category_names[cat];
}

uint16_t dex_category_len(uint16_t cat)
{
    if (cat >= DEX_CATEGORY_COUNT) return 0;
    return dex_category_count[cat];
}

const dex_entry_t *dex_entry_at(uint16_t cat, uint16_t idx)
{
    if (cat >= DEX_CATEGORY_COUNT || idx >= dex_category_count[cat]) return NULL;
    return &dex_entries[dex_category_first[cat] + idx];
}

size_t dex_entry_attrs(const dex_entry_t *e, dex_attr_t *out, size_t cap)
{
    if (!e || !out || cap == 0 || e->attrs_count == 0) return 0;
    const uint8_t *p = _binary_dex_attrs_bin_start + e->attrs_off;
    const uint8_t *end = _binary_dex_attrs_bin_end;
    size_t n = 0;
    for (uint8_t consumed = 0; consumed < e->attrs_count && n < cap; consumed++) {
        if (p >= end) break;
        const char *s = (const char *)p;
        size_t len = strnlen(s, (size_t)(end - p));
        if (len == 0 || (size_t)(end - p) <= len) break; /* 未按 NUL 结尾:损坏 */
        const char *bar = memchr(s, '|', len);
        if (!bar) { p += len + 1; continue; /* 跳过损坏串,不占输出槽 */ }
        out[n].label = s;
        out[n].value = bar + 1;
        n++;
        p += len + 1;
    }
    return n;
}

size_t dex_entry_attrs_text(const dex_entry_t *e, dex_attr_text_t *out,
                            size_t cap)
{
    dex_attr_t raw[DEX_ATTRS_MAX];
    size_t n = dex_entry_attrs(e, raw, sizeof(raw) / sizeof(raw[0]));
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) {
        const char *s = raw[i].label;
        const char *bar = raw[i].value;
        size_t llen = bar ? (size_t)(bar - s) - 1 : strnlen(s, 15);
        if (llen > 15) llen = 15;
        memcpy(out[i].label, s, llen);
        out[i].label[llen] = '\0';
        const char *v = bar ? bar : "";
        size_t vlen = strnlen(v, 79);
        if (vlen > 79) vlen = 79;
        memcpy(out[i].value, v, vlen);
        out[i].value[vlen] = '\0';
    }
    return n;
}

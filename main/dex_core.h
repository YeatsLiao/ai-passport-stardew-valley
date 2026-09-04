// main/dex_core.h —— 星露谷图鉴数据访问(纯 C,不依赖 ESP-IDF/LVGL)。
// 数据来源: dex_static.h(生成文件)与 dex_attrs.bin 属性字符串池。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dex_static.h"

typedef struct {
    const char *label; /* 池内指针,含完整 "Label|Value" 串(只读) */
    const char *value; /* 指向首个 '|' 之后;无 '|' 时为 NULL */
} dex_attr_t;

typedef struct {
    char label[16];
    char value[80];
} dex_attr_text_t;

// 类别数量与访问(类别内条目按生成顺序稳定排列)。
uint16_t dex_num_categories(void);
const char *dex_category_name(uint16_t cat);
uint16_t dex_category_len(uint16_t cat);

// 类别内第 idx 条(0..len-1)。越界返回 NULL。
const dex_entry_t *dex_entry_at(uint16_t cat, uint16_t idx);

// 解析条目属性("Label|Value\0" 连续串)。最多填 cap 个,返回实际个数。
// 注意:label/value 均指向池内原始串,label 含完整内容,仅供 dex_entry_attrs_text 使用。
size_t dex_entry_attrs(const dex_entry_t *e, dex_attr_t *out, size_t cap);

// 解析并把 "Label|Value" 拆分拷贝到 RAM(截断到缓冲),供 LVGL 直接显示。
// 返回实际个数。
size_t dex_entry_attrs_text(const dex_entry_t *e, dex_attr_text_t *out,
                            size_t cap);

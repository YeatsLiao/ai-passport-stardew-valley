// main/dex_static.h —— 星露谷图鉴离线静态数据(生成文件,勿手改)。
// 生成: tools/gen_dex_data.py; 数据来源 stardew-valley-data。
// 素材版权归 ConcernedApe, 非官方项目, 仅供个人学习, 禁止商用。
#pragma once
#include <stdint.h>
#include <stddef.h>

#define DEX_CATEGORY_COUNT 30u
#define DEX_ENTRY_COUNT 1161u
#define DEX_ATTRS_MAX 5u
#define DEX_SPRITE_NONE 0xFFFFu
#define DEX_SPRITE_COUNT 1092u

typedef struct {
    uint8_t  category;
    uint8_t  attrs_count;
    uint16_t sprite_idx;   /* DEX_SPRITE_NONE = 无图 */
    uint16_t attrs_off;
    char     name[24];
    char     desc[160];
} dex_entry_t;

extern const char *dex_category_names[DEX_CATEGORY_COUNT];
extern const uint16_t dex_category_first[DEX_CATEGORY_COUNT];
extern const uint16_t dex_category_count[DEX_CATEGORY_COUNT];
extern const dex_entry_t dex_entries[DEX_ENTRY_COUNT];

// dex_sprites.bin: TOC(12B/图 {off u32le,len u32le,w u16le,h u16le}) + raw-deflate RGB565
extern const uint8_t _binary_dex_sprites_bin_start[];
extern const uint8_t _binary_dex_sprites_bin_end[];
// dex_attrs.bin: "Label|Value\0" 串联字符串池(attrs_off 为字节偏移)
extern const uint8_t _binary_dex_attrs_bin_start[];
extern const uint8_t _binary_dex_attrs_bin_end[];

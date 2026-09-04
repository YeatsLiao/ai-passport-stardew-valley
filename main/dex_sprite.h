// main/dex_sprite.h —— 图鉴图片渲染管线(纯 C + 内置 miniz,无 ESP-IDF/LVGL 依赖)。
//
// dex_sprites.bin 布局(见 tools/gen_dex_data.py):
//   [0 .. DEX_SPRITE_COUNT*12)  TOC,每条 {off(u32le), len(u32le), w(u16le), h(u16le)}
//   [DEX_SPRITE_COUNT*12 ..)    raw-deflate 压缩的 RGB565 像素(解压后 w*h*2 字节)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEX_SPRITE_MAX_W 96u
#define DEX_SPRITE_MAX_H 96u
#define DEX_SPRITE_MAX_BYTES (DEX_SPRITE_MAX_W * DEX_SPRITE_MAX_H * 2u)

// 解出第 sprite_idx 张图为 RGB565(小端)像素。
// out_cap_u16 为输出缓冲的 u16 单元数,至少 DEX_SPRITE_MAX_BYTES/2。
// 成功返回 true 并写 *out_w/*out_h(1..96)。
bool dex_sprite_render(uint16_t sprite_idx, uint16_t *rgb565_out,
                       size_t out_cap_u16, uint32_t *out_w, uint32_t *out_h);

// 最近邻 2x 放大 RGB565(允许 src==dst,从后往前写)。
// 输出尺寸 (w*2,h*2);缓冲不足或 w/h 为 0 返回 false。
bool dex_sprite_scale2x(const uint16_t *src, uint32_t w, uint32_t h,
                        uint16_t *dst, size_t dst_cap_u16,
                        uint32_t *out_w, uint32_t *out_h);

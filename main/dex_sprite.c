// main/dex_sprite.c —— 图鉴图片渲染管线(纯 C + miniz tinfl 解压)。
#include "dex_sprite.h"

#include <string.h>

#include "dex_static.h"
#include "vendor/miniz/miniz.h"

extern const uint8_t _binary_dex_sprites_bin_start[];
extern const uint8_t _binary_dex_sprites_bin_end[];

#define DEX_TOC_ITEM 12u /* off u32 + len u32 + w u16 + h u16 */

/* tinfl 上下文(内嵌 ~33KB LZ 字典)静态分配,绝不放任务栈:
 * miniz 的 tinfl_decompress_mem_to_mem 包装会在栈上构造该结构,
 * 溢出小任务栈(如 4KB worker)直接触发栈保护 panic。 */
static tinfl_decompressor s_infl;

static const uint8_t *blob(void) { return _binary_dex_sprites_bin_start; }
static size_t blob_len(void)
{
    return (size_t)(_binary_dex_sprites_bin_end - _binary_dex_sprites_bin_start);
}

static bool toc_get(uint16_t idx, uint32_t *off, uint32_t *len,
                    uint32_t *w, uint32_t *h)
{
    if (idx >= DEX_SPRITE_COUNT) return false;
    size_t need = (size_t)DEX_SPRITE_COUNT * DEX_TOC_ITEM;
    if (blob_len() < need) return false;
    const uint8_t *t = blob() + (size_t)idx * DEX_TOC_ITEM;
    memcpy(off, t, 4);
    memcpy(len, t + 4, 4);
    memcpy(w, t + 8, 2);
    memcpy(h, t + 10, 2);
    return true;
}

bool dex_sprite_render(uint16_t sprite_idx, uint16_t *rgb565_out,
                       size_t out_cap_u16, uint32_t *out_w, uint32_t *out_h)
{
    if (!rgb565_out || out_cap_u16 < DEX_SPRITE_MAX_BYTES / 2) return false;

    uint32_t off, len, w, h;
    if (!toc_get(sprite_idx, &off, &len, &w, &h)) return false;
    if (w == 0 || h == 0 || w > DEX_SPRITE_MAX_W || h > DEX_SPRITE_MAX_H)
        return false;
    if ((uint64_t)w * h * 2 > (uint64_t)out_cap_u16 * 2) return false;

    size_t payload = (size_t)DEX_SPRITE_COUNT * DEX_TOC_ITEM;
    if ((uint64_t)off + len > (uint64_t)blob_len() - payload) return false;

    /* 直接调 tinfl_decompress,输出缓冲同时充当回溯字典:单次从头解压,
     * 合法 deflate 流的匹配距离不超过已输出字节,回溯全部落在
     * [out, out+n) 内,不会触碰未初始化区域。
     * 必须每次 tinfl_init:s_infl 是持久静态上下文,残留的协程状态
     * (m_state)会让下一次解压直接跳进中间状态而失败。
     *
     * NON_WRAPPING 语义下 out_bytes 输入是"输出缓冲总容量",输出是
     * "实际解压字节数"。曾经传精确 w*h*2,会在"恰好写满最后一个
     * 字节且流结束"的脆弱边界上让 LZ 回溯引用失败,表现为同一张
     * 图同一索引偶发 TINFL_STATUS_FAILED。改为传完整 out_cap_u16*2
     * (即 96x96x2 字节全缓冲),字典空间充足,偶发性消失;输出长度
     * 仍用精确 w*h*2 校验。 */
    tinfl_init(&s_infl);
    size_t in_bytes = len;
    size_t out_bytes = (size_t)out_cap_u16 * 2;
    tinfl_status st = tinfl_decompress(
        &s_infl, blob() + payload + off, &in_bytes,
        (mz_uint8 *)rgb565_out, (mz_uint8 *)rgb565_out, &out_bytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || out_bytes != (size_t)w * h * 2)
        return false; /* 截断/损坏 */

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

bool dex_sprite_scale2x(const uint16_t *src, uint32_t w, uint32_t h,
                        uint16_t *dst, size_t dst_cap_u16,
                        uint32_t *out_w, uint32_t *out_h)
{
    if (!src || !dst || w == 0 || h == 0) return false;
    uint32_t ow = w * 2, oh = h * 2;
    if ((uint64_t)ow * oh > dst_cap_u16) return false;

    for (int32_t y = (int32_t)h - 1; y >= 0; y--) { /* 允许 src==dst:倒序写 */
        const uint16_t *srow = src + (size_t)y * w;
        uint16_t *d0 = dst + (size_t)y * 2 * ow;
        uint16_t *d1 = d0 + ow;
        for (uint32_t x = 0; x < w; x++) {
            uint16_t px = srow[x];
            d0[x * 2] = px;
            d0[x * 2 + 1] = px;
            d1[x * 2] = px;
            d1[x * 2 + 1] = px;
        }
    }
    if (out_w) *out_w = ow;
    if (out_h) *out_h = oh;
    return true;
}

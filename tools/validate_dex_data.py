#!/usr/bin/env python3
"""校验 gen_dex_data.py 产物与数据源的一致性(无需 Pillow)。

用法: python tools/validate_dex_data.py [--data <stardew-valley-data 路径>]
检查项:
  1. dex_entries 总数/各类条数与 JSON 数据源一致
  2. name/desc 长度上限、非空
  3. 属性串带 '|'、长度与个数上限、池内偏移不越界
  4. dex_sprites.bin 的 TOC 与数据区自洽(off+len 不越界, w/h 在 1..96)
  5. 每条目的 sprite_idx 合法
全部通过退出码 0,否则打印问题清单并以 1 退出。
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN = REPO / "main"

errors: list[str] = []


def err(msg: str) -> None:
    errors.append(msg)


def c_unescape(s: str) -> str:
    """把 C 字符串字面量转义还原为实际字节长度口径。"""
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append(s[i + 1])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def source_categories(data_dir: Path) -> list[tuple[str, int]]:
    """从 gen 脚本里解析类别文件列表(与生成端保持单一来源)。"""
    gen = (REPO / "tools" / "gen_dex_data.py").read_text(encoding="utf-8")
    files = re.findall(r'C\("([a-z0-9-]+)",\s*"[^"]+"', gen)
    out = []
    for f in files:
        j = json.loads((data_dir / "data" / f"{f}.json").read_text(encoding="utf-8"))
        if isinstance(j, dict):
            j = list(j.values())
        n = sum(1 for e in j if isinstance(e, dict) and e.get("name"))
        out.append((f, n))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=r"D:\Project\stardew-valley-data")
    args = ap.parse_args()
    data_dir = Path(args.data)

    header = (MAIN / "dex_static.h").read_text(encoding="utf-8")
    src = (MAIN / "dex_static.c").read_text(encoding="utf-8")
    sprites = (MAIN / "dex_sprites.bin").read_bytes()
    attrs = (MAIN / "dex_attrs.bin").read_bytes()

    n_cat = int(re.search(r"#define DEX_CATEGORY_COUNT (\d+)u", header).group(1))
    n_entry = int(re.search(r"#define DEX_ENTRY_COUNT (\d+)u", header).group(1))
    n_sprite = int(re.search(r"#define DEX_SPRITE_COUNT (\d+)u", header).group(1))
    print(f"类别 {n_cat} / 条目 {n_entry} / 图片 {n_sprite}")

    # 1. 与数据源对账
    srcs = source_categories(data_dir)
    if len(srcs) != n_cat:
        err(f"类别数不一致: header={n_cat}, gen 脚本={len(srcs)}")
    total = sum(n for _, n in srcs)
    if total != n_entry:
        err(f"条目总数不一致: header={n_entry}, 数据源合计={total}")
    m = re.search(r"dex_category_count\[DEX_CATEGORY_COUNT\] = \{([\d, ]+)\}", src)
    if not m:
        err("dex_static.c 中找不到 dex_category_count")
        counts = []
    else:
        counts = [int(x) for x in m.group(1).split(",")]
    if sum(counts) != n_entry:
        err(f"dex_category_count 合计 {sum(counts)} != {n_entry}")
    for (f, n), c in zip(srcs, counts):
        if n != c:
            err(f"类别 {f}: 数据源 {n} 条, 产物 {c} 条")

    # 2. 解析 dex_entries 行
    rows = re.findall(
        r"\{\s*(\d+),\s*(\d+),\s*(\w+),\s*(\d+),\s*\"((?:[^\"\\]|\\.)*)\",\s*"
        r"\"((?:[^\"\\]|\\.)*)\"\s*\}", src)
    if len(rows) != n_entry:
        err(f"dex_entries 行数 {len(rows)} != {n_entry}")

    sprite_blob_len = n_sprite * 12
    if len(sprites) < sprite_blob_len:
        err(f"dex_sprites.bin 过小: {len(sprites)} < TOC {sprite_blob_len}")
    toc = []
    for i in range(n_sprite):
        off, ln, w, h = struct.unpack_from("<IIHH", sprites, i * 12)
        toc.append((off, ln, w, h))
        if not (1 <= w <= 96 and 1 <= h <= 96):
            err(f"sprite {i}: 尺寸非法 {w}x{h}")
        if off + ln > len(sprites) - sprite_blob_len:
            err(f"sprite {i}: 数据越界 off={off} len={ln}")

    none_cnt = 0
    for i, (cat, nattr, sp, aoff, name, desc) in enumerate(rows):
        name = c_unescape(name)
        desc = c_unescape(desc)
        if not name:
            err(f"entry {i}: 名称为空")
        if len(name) > 23:
            err(f"entry {i}: name 过长({len(name)}): {name}")
        if len(desc) > 159:
            err(f"entry {i}: desc 过长({len(desc)})")
        if int(nattr) > 5:
            err(f"entry {i}: 属性数 {nattr} > 5")
        if sp == "DEX_SPRITE_NONE":
            none_cnt += 1
        elif int(sp) >= n_sprite:
            err(f"entry {i}: sprite_idx {sp} 越界")
        # 3. 属性串抽查
        p = int(aoff)
        for _ in range(int(nattr)):
            e = attrs.find(b"\x00", p)
            if e < 0 or e > len(attrs):
                err(f"entry {i}: 属性串无 NUL 结尾(池越界)")
                break
            s = attrs[p:e]
            if b"|" not in s:
                err(f"entry {i}: 属性串缺 '|': {s[:20]!r}")
            if len(s) > 127:  # UTF-8 多字节字符会使字节数超过字符数上限 95,取宽松字节口径
                err(f"entry {i}: 属性串过长 {len(s)}")
            p = e + 1
        if int(aoff) + int(nattr) > 0 and p > len(attrs):
            err(f"entry {i}: 属性池越界")

    total_comp = sum(ln for _, ln, _, _ in toc)
    print(f"TOC {sprite_blob_len}B + 压缩数据 {total_comp}B = {len(sprites)}B"
          f" (实测 {len(sprites)})")
    if sprite_blob_len + total_comp != len(sprites):
        err("dex_sprites.bin 长度与 TOC 汇总不符")
    print(f"属性池 {len(attrs)}B; 无图条目 {none_cnt}")

    if errors:
        print(f"\n校验失败 {len(errors)} 项:")
        for e in errors[:40]:
            print(f"  - {e}")
        return 1
    print("\n校验通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

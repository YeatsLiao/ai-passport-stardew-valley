#!/usr/bin/env python3
"""生成星露谷图鉴离线静态资源(ai-passport-stardew-valley)。

输入: stardew-valley-data 仓库的 data/*.json 与 images/。
输出(写入 main/, 随固件编译并入库):
  - dex_static.h / dex_static.c : 条目数据(类别/名称/描述/属性偏移/图片索引)
  - dex_sprites.bin             : TOC(12B/图: off,len,w,h) + raw-deflate RGB565 像素
  - dex_attrs.bin               : "Label|Value\\0" 串联的属性字符串池

用法(需 Pillow):
  python tools/gen_dex_data.py [--data <stardew-valley-data 路径>]

图片处理: 统一 RGB565 小端、透明像素按纸色 #F4F0E0 预混合、
超过 96x96 的图最近邻等比缩到 96 盒内, raw-deflate 压缩。
条目与图片与 stardew-valley-data 内容对应; 星露谷素材版权归 ConcernedApe,
本项目非官方、仅供个人学习,禁止商用。
"""
from __future__ import annotations

import argparse
import io
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
OUT_DIR = REPO / "main"
DEFAULT_DATA = Path(r"D:\Project\stardew-valley-data")

PAPER = (244, 240, 224)      # 图鉴纸色 #F4F0E0(与设备端 UI 背景一致)
MAX_SPRITE = 96              # 图片盒上限(px)
NAME_MAX = 23                # name[24] 含 NUL
DESC_MAX = 159               # desc[160] 含 NUL
ATTR_STR_MAX = 95            # 单条 "Label|Value" 截断(含 NUL)
ATTRS_MAX = 5                # 每条目最多属性数

# ---------------------------------------------------------------------------
# 类别配置: (json 文件, 显示名, 属性提取函数(entry)->[(label, value)])
# 值格式化约定: None=跳过该属性; 标量直出; list 按元素递归后逗号拼接;
# dict 若元素含 name 取 name, 否则 "k x v" / "k: v"。
# ---------------------------------------------------------------------------

def _fmt(v, depth=0):
    if v is None:
        return None
    if isinstance(v, bool):
        return "Yes" if v else "No"
    if isinstance(v, (int, float)):
        return f"{v:,}" if isinstance(v, int) else f"{v:g}"
    if isinstance(v, str):
        return v
    if isinstance(v, dict):
        # 配料/材料类结构:{name, quantity} 优先输出 "名称 xN"。
        if "name" in v and "quantity" in v:
            return f"{_fmt(v['name'], depth)} x{_fmt(v['quantity'], depth)}"
        if "name" in v:
            return _fmt(v["name"], depth)
        return ", ".join(
            f"{k} x {_fmt(x)}" if isinstance(x, (int, float)) else f"{k}: {_fmt(x)}"
            for k, x in list(v.items())[:4]) or None
    if isinstance(v, list):
        if not v:
            return None
        parts = [p for p in (_fmt(x, depth + 1) for x in v[:3]) if p]
        if len(v) > 3:
            parts.append(f"+{len(v) - 3}")
        return ", ".join(parts) or None
    return str(v)


def _g(entry, *path):
    """按点路径取值, 缺失返回 None。"""
    cur = entry
    for p in path:
        if not isinstance(cur, dict) or p not in cur:
            return None
        cur = cur[p]
    return cur


def C(file, title, attrs):
    return {"file": file, "title": title, "attrs": attrs}


CATEGORIES = [
    C("fish", "Fish", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Seasons", _g(e, "seasons")),
        ("Location", _g(e, "location")), ("Time", _g(e, "time")),
        ("Weather", _g(e, "weather"))]),
    C("crops", "Crops", lambda e: [
        ("Price", _g(e, "cropSellPrice")), ("Seasons", _g(e, "seasons")),
        ("Grow", f"{_g(e,'growDays')} days" if _g(e, "growDays") else None),
        ("Regrow", f"{_g(e,'regrowDays')} days" if _g(e, "regrowDays") else None),
        ("Seed", _g(e, "seedName"))]),
    C("forageables", "Forage", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Seasons", _g(e, "seasons")),
        ("Location", _g(e, "locations"))]),
    C("minerals", "Minerals", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Location", _g(e, "locations"))]),
    C("artifacts", "Artifacts", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Location", _g(e, "locations"))]),
    C("cooking", "Cooking", lambda e: [
        ("Price", _g(e, "sellPrice")),
        ("Energy", _g(e, "energyHealth", "energy")),
        ("Health", _g(e, "energyHealth", "health")),
        ("Recipe", _g(e, "ingredients"))]),
    C("artisan-goods", "Artisan Goods", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Equipment", _g(e, "equipment"))]),
    C("villagers", "Villagers", lambda e: [
        ("Birthday", _g(e, "birthday")), ("Home", _g(e, "address")),
        ("Job", _g(e, "occupation")),
        ("Loves", _g(e, "loves")),
        ("Marriage", _g(e, "marriageable"))]),
    C("monsters", "Monsters", lambda e: [
        ("HP", _g(e, "hp")), ("Damage", _g(e, "damage")), ("XP", _g(e, "xp")),
        ("Location", _g(e, "locations")), ("Loot", _g(e, "lootIds"))]),
    C("weapons", "Weapons", lambda e: [
        ("Damage", f"{_g(e,'damageMin')}-{_g(e,'damageMax')}"
         if _g(e, "damageMin") is not None else None),
        ("Speed", _g(e, "speed")), ("Defense", _g(e, "defense")),
        ("Level", _g(e, "level")), ("Price", _g(e, "sellPrice"))]),
    C("rings", "Rings", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Craft",
         f"Lv{_g(e,'craftingLevel')} {_g(e,'craftingSkill')}"
         if _g(e, "craftingLevel") else None),
        ("Ingredients", _g(e, "ingredients"))]),
    C("footwear", "Footwear", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Defense", _g(e, "defense")),
        ("Immunity", _g(e, "immunity")), ("Obtain", _g(e, "obtain"))]),
    C("hats", "Hats", lambda e: [("Obtain", _g(e, "obtain"))]),
    C("crafting", "Crafting", lambda e: [
        ("Ingredients", _g(e, "ingredients")), ("Source", _g(e, "source"))]),
    C("animals", "Animals", lambda e: [("Price", _g(e, "purchasePrice"))]),
    C("bait", "Bait", lambda e: [("Price", _g(e, "sellPrice"))]),
    C("tackle", "Tackle", lambda e: [("Price", _g(e, "sellPrice"))]),
    C("monster-loot", "Monster Loot", lambda e: [
        ("Price", _g(e, "sellPrice")), ("Dropped", _g(e, "droppedBy"))]),
    C("trees", "Trees", lambda e: [
        ("Seasons", _g(e, "seasons")),
        ("Mature", f"{_g(e,'daysToMature')} days" if _g(e, "daysToMature") else None),
        ("Produce", _g(e, "produce"))]),
    C("tools", "Tools", lambda e: [
        ("Type", _g(e, "type")),
        ("Enchant", _g(e, "canEnchant"))]),
    C("trinkets", "Trinkets", lambda e: [
        ("Effect", _g(e, "effect")), ("Source", _g(e, "source")),
        ("Price", _g(e, "sellPrice"))]),
    C("special-items", "Special Items", lambda e: [
        ("Effect", _g(e, "effect")), ("From", _g(e, "obtainedFrom"))]),
    C("stardrops", "Stardrops", lambda e: [("Source", _g(e, "source"))]),
    C("rarecrows", "Rarecrows", lambda e: [("Obtain", _g(e, "obtain"))]),
    C("concessions", "Concessions", lambda e: [("Price", _g(e, "price"))]),
    C("achievements", "Achievements", lambda e: [("Reward", _g(e, "reward"))]),
    C("mixed-seeds", "Mixed Seeds", lambda e: [("Price", _g(e, "sellPrice"))]),
    C("lost-books", "Lost Books", lambda e: []),
    C("skills", "Skills", lambda e: [("Tool", _g(e, "toolBonus"))]),
    C("buildings", "Buildings", lambda e: [
        ("Cost", _g(e, "buildCost")), ("Days", _g(e, "buildDays")),
        ("Capacity", _g(e, "animalCapacity")), ("Builder", _g(e, "builder"))]),
]

# ---------------------------------------------------------------------------

# LVGL 内置 montserrat 字体仅覆盖 ASCII。常见非 ASCII 符号做等价
# 映射,其余非 ASCII 字符直接丢弃,避免设备上渲染成空白方块。
_ASCII_MAP = {
    "\u2013": "-", "\u2014": "-", "\u2015": "-", "\u2212": "-",  # dash
    "\u2018": "'", "\u2019": "'", "\u201a": "'",                # single quote
    "\u201c": '"', "\u201d": '"', "\u201e": '"',                # double quote
    "\u00b0": " deg", "\u00d7": "x", "\u2026": "...",           # degree/mult/ellipsis
    "\u2022": "-", "\u00b7": "-", "\u2043": "-",               # bullet
    "\u2192": "->", "\u2190": "<-", "\u2264": "<=", "\u2265": ">=",
    "\u00a0": " ", "\u2007": " ", "\u202f": " ",               # nbsp variants
}


def to_ascii(s: str) -> str:
    s = "".join(_ASCII_MAP.get(ch, ch) for ch in s)
    return s.encode("ascii", "ignore").decode("ascii")


def clamp(s: str, n: int) -> str:
    s = to_ascii(s)
    s = " ".join(s.split())
    return s if len(s) <= n else s[: n - 3].rstrip() + "..."


def load_entries(data_dir: Path):
    entries = []  # list of dict(cat, name, desc, attrs[list[str]], image)
    for ci, cat in enumerate(CATEGORIES):
        path = data_dir / "data" / f"{cat['file']}.json"
        raw = path.read_text(encoding="utf-8")
        data = __import__("json").loads(raw)
        if isinstance(data, dict):
            data = list(data.values())
        count = 0
        for e in data:
            if not isinstance(e, dict):
                continue
            name = _fmt(e.get("name"))
            if not name:
                continue
            attrs = []
            for label, value in cat["attrs"](e):
                # 属性值可能是原始 JSON 结构(list/dict/bool),必须经 _fmt
                # 转为可读文本,否则 f-string 会写出 Python repr。
                value = _fmt(value)
                if value is None or value == "":
                    continue
                s = clamp(f"{label}|{value}", ATTR_STR_MAX)
                if s and len(attrs) < ATTRS_MAX:
                    attrs.append(s)
            desc = clamp(_fmt(e.get("description")) or "", DESC_MAX)
            entries.append({
                "cat": ci,
                "name": clamp(name, NAME_MAX),
                "desc": desc,
                "attrs": attrs,
                "image": e.get("image"),
            })
            count += 1
        if count == 0:
            print(f"  warn: {cat['file']}.json 无有效条目", file=sys.stderr)
    return entries


def sprite_to_rgb565(png_path: Path) -> bytes | None:
    try:
        im = Image.open(png_path).convert("RGBA")
    except Exception as exc:  # noqa: BLE001
        print(f"  warn: 图片打开失败 {png_path.name}: {exc}", file=sys.stderr)
        return None
    w, h = im.size
    if w > MAX_SPRITE or h > MAX_SPRITE:
        k = min(MAX_SPRITE / w, MAX_SPRITE / h)
        w, h = max(1, int(w * k)), max(1, int(h * k))
        im = im.resize((w, h), Image.NEAREST)
    out = bytearray()
    r5 = lambda v: (v >> 3) & 0x1F
    g6 = lambda v: (v >> 2) & 0x3F
    b5 = lambda v: (v >> 3) & 0x1F
    for (r, g, b, a) in im.getdata():
        if a < 255:
            k = a / 255.0
            r = int(r * k + PAPER[0] * (1 - k))
            g = int(g * k + PAPER[1] * (1 - k))
            b = int(b * k + PAPER[2] * (1 - k))
        out += struct.pack("<H", (r5(r) << 11) | (g6(g) << 5) | b5(b))
    return bytes(out)


def raw_deflate(data: bytes, level: int = 9) -> bytes:
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def _im_to_rgb565(im: Image.Image) -> bytes:
    r5 = lambda v: (v >> 3) & 0x1F
    g6 = lambda v: (v >> 2) & 0x3F
    b5 = lambda v: (v >> 3) & 0x1F
    out = bytearray()
    for (r, g, b, a) in im.convert("RGBA").getdata():
        if a < 255:
            k = a / 255.0
            r = int(r * k + PAPER[0] * (1 - k))
            g = int(g * k + PAPER[1] * (1 - k))
            b = int(b * k + PAPER[2] * (1 - k))
        out += struct.pack("<H", (r5(r) << 11) | (g6(g) << 5) | b5(b))
    return bytes(out)


def cstr(s: str) -> str:
    return (s.replace("\\", "\\\\").replace('"', '\\"')
             .replace("\n", " ").replace("\r", " "))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=str(DEFAULT_DATA))
    args = ap.parse_args()
    data_dir = Path(args.data)
    if not (data_dir / "data").is_dir():
        raise SystemExit(f"数据目录不存在: {data_dir}")

    print("1/4 读取并裁剪条目数据 ...")
    entries = load_entries(data_dir)
    print(f"  条目总数 {len(entries)}, 类别 {len(CATEGORIES)}")

    print("2/4 打包图片 ...")
    toc: list[bytes] = []
    blob = bytearray()
    index_of: dict[str, int] = {}
    no_image = 0
    for e in entries:
        p = e["image"]
        if not p:
            e["sprite"] = None
            continue
        if p in index_of:
            e["sprite"] = index_of[p]
            continue
        png = data_dir / p.replace("/", "\\")
        if not png.is_file():
            png = data_dir / "images" / Path(p).name
        im = None
        if png.is_file():
            try:
                im = Image.open(png).convert("RGBA")
            except Exception as exc:  # noqa: BLE001
                print(f"  warn: 图片打开失败 {p}: {exc}", file=sys.stderr)
        if im is None:
            e["sprite"] = None
            continue
        w, h = im.size
        if w > MAX_SPRITE or h > MAX_SPRITE:
            k = min(MAX_SPRITE / w, MAX_SPRITE / h)
            w, h = max(1, int(w * k)), max(1, int(h * k))
            im = im.resize((w, h), Image.NEAREST)
        comp = raw_deflate(_im_to_rgb565(im))
        index_of[p] = len(toc)
        e["sprite"] = len(toc)
        toc.append(struct.pack("<IIHH", len(blob), len(comp), w, h))
        blob += comp
    print(f"  唯一图片 {len(toc)} 张, 压缩后 {len(blob)} bytes, TOC {len(toc)*12} bytes")

    print("3/4 打包属性字符串池 ...")
    attrs_blob = bytearray()
    for e in entries:
        e["attrs_off"] = len(attrs_blob) if e["attrs"] else 0
        for s in e["attrs"]:
            attrs_blob += s.encode("utf-8") + b"\x00"
    print(f"  属性池 {len(attrs_blob)} bytes")

    print("4/4 生成 C 源与二进制 ...")
    n_entries = len(entries)
    cat_first, cat_count = [], []
    for ci in range(len(CATEGORIES)):
        idxs = [i for i, e in enumerate(entries) if e["cat"] == ci]
        cat_first.append(idxs[0] if idxs else 0)
        cat_count.append(len(idxs))

    header = io.StringIO()
    header.write("// main/dex_static.h —— 星露谷图鉴离线静态数据(生成文件,勿手改)。\n")
    header.write("// 生成: tools/gen_dex_data.py; 数据来源 stardew-valley-data。\n")
    header.write("// 素材版权归 ConcernedApe, 非官方项目, 仅供个人学习, 禁止商用。\n")
    header.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
    header.write(f"#define DEX_CATEGORY_COUNT {len(CATEGORIES)}u\n")
    header.write(f"#define DEX_ENTRY_COUNT {n_entries}u\n")
    header.write(f"#define DEX_ATTRS_MAX {ATTRS_MAX}u\n")
    header.write("#define DEX_SPRITE_NONE 0xFFFFu\n")
    header.write(f"#define DEX_SPRITE_COUNT {len(toc)}u\n\n")
    header.write("typedef struct {\n"
                 "    uint8_t  category;\n"
                 "    uint8_t  attrs_count;\n"
                 "    uint16_t sprite_idx;   /* DEX_SPRITE_NONE = 无图 */\n"
                 "    uint16_t attrs_off;\n"
                 "    char     name[24];\n"
                 "    char     desc[160];\n"
                 "} dex_entry_t;\n\n")
    header.write(f"extern const char *dex_category_names[DEX_CATEGORY_COUNT];\n")
    header.write(f"extern const uint16_t dex_category_first[DEX_CATEGORY_COUNT];\n")
    header.write(f"extern const uint16_t dex_category_count[DEX_CATEGORY_COUNT];\n")
    header.write(f"extern const dex_entry_t dex_entries[DEX_ENTRY_COUNT];\n\n")
    header.write("// dex_sprites.bin: TOC(12B/图 {off u32le,len u32le,w u16le,h u16le}) + raw-deflate RGB565\n")
    header.write("extern const uint8_t _binary_dex_sprites_bin_start[];\n")
    header.write("extern const uint8_t _binary_dex_sprites_bin_end[];\n")
    header.write("// dex_attrs.bin: \"Label|Value\\0\" 串联字符串池(attrs_off 为字节偏移)\n")
    header.write("extern const uint8_t _binary_dex_attrs_bin_start[];\n")
    header.write("extern const uint8_t _binary_dex_attrs_bin_end[];\n")
    (OUT_DIR / "dex_static.h").write_text(header.getvalue(), encoding="utf-8")

    src = io.StringIO()
    src.write('// main/dex_static.c —— 星露谷图鉴离线静态数据(生成文件,勿手改)。\n')
    src.write('// 生成: tools/gen_dex_data.py; 数据来源 stardew-valley-data。\n')
    src.write('#include "dex_static.h"\n\n')
    src.write("const char *dex_category_names[DEX_CATEGORY_COUNT] = {\n")
    for cat in CATEGORIES:
        src.write(f'    "{cat["title"]}",\n')
    src.write("};\n\n")
    src.write("const uint16_t dex_category_first[DEX_CATEGORY_COUNT] = {")
    src.write(", ".join(str(x) for x in cat_first) + "};\n\n")
    src.write("const uint16_t dex_category_count[DEX_CATEGORY_COUNT] = {")
    src.write(", ".join(str(x) for x in cat_count) + "};\n\n")
    src.write(f"const dex_entry_t dex_entries[DEX_ENTRY_COUNT] = {{\n")
    for e in entries:
        src.write("    { %d, %d, %s, %d, \"%s\", \"%s\" },\n" % (
            e["cat"], len(e["attrs"]),
            "DEX_SPRITE_NONE" if e.get("sprite") is None else e["sprite"],
            e["attrs_off"], cstr(e["name"]), cstr(e["desc"])))
    src.write("};\n")
    (OUT_DIR / "dex_static.c").write_text(src.getvalue(), encoding="utf-8")

    sprites = bytearray()
    for t in toc:
        sprites += t
    sprites += blob
    (OUT_DIR / "dex_sprites.bin").write_bytes(bytes(sprites))
    (OUT_DIR / "dex_attrs.bin").write_bytes(bytes(attrs_blob))

    print("\n==== 报告 ====")
    print(f"条目 {n_entries} / 类别 {len(CATEGORIES)} / 无图条目 {no_image}")
    print(f"dex_sprites.bin: {len(sprites)} bytes ({len(sprites)/1024:.0f} KB)")
    print(f"dex_attrs.bin:   {len(attrs_blob)} bytes ({len(attrs_blob)/1024:.0f} KB)")
    sc = (OUT_DIR / "dex_static.c").stat().st_size
    print(f"dex_static.c:    {sc} bytes ({sc/1024:.0f} KB)")
    print(f"静态资源合计:    {(len(sprites)+len(attrs_blob)+sc)/1024:.0f} KB")


if __name__ == "__main__":
    main()

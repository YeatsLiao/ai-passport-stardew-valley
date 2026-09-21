<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport — Stardew Valley Dex

An offline Stardew Valley encyclopedia ("dex") firmware for the
[FoloToy AI Passport](https://github.com/FoloToy/ai-passport) hardware
(ESP32-C3, 8 MB flash, 240x320 ST7789 display, three-button ADC keypad).

Everything runs fully offline: no WiFi, no Bluetooth, no network. The entire
dataset — **1161 entries across 30 categories with 1092 pixel sprites** — is
embedded inside the 3 MB factory app image.

## Features

- **30 categories**: Fish, Crops, Forageables, Minerals, Artifacts, Cooking,
  Artisan Goods, Villagers, Monsters, Weapons, Rings, Footwear, Hats,
  Crafting, Animals, Bait, Tackle, Monster Loot, Trees, Tools, Trinkets,
  Special Items, Stardrops, Rarecrows, Concessions, Achievements, Mixed Seeds,
  Lost Books, Skills, Buildings.
- **Detail view**: pixel sprite, name, description and up to 5 attribute rows
  per entry.
- **Position memory**: the last viewed entry is restored from NVS on boot.
- **Pixel-perfect rendering**: sprites are stored as RGB565 with the
  transparent color pre-blended onto the Stardew paper background, raw-DEFLATE
  compressed, inflated at runtime with miniz and 2x nearest-neighbor upscaled
  for small art.
- **Battery & idle dimming**: battery indicator on screen; backlight dims
  after 60 s of inactivity.
- **Full-text info panel**: description and attribute rows are never
  ellipsized; content taller than the panel auto-scrolls in a seamless
  marquee loop.
- **Background music**: loops Stardew Valley Overture (full 2:26) on boot;
  IMA ADPCM 8 kHz mono embedded in firmware, zero external dependencies.

## Controls

| Page | Key | Action |
| --- | --- | --- |
| Category grid | UP / DOWN (click) | Move selection |
| Category grid | OK (click) | Enter category |
| Entry list | UP / DOWN (click) | Move selection |
| Entry list | UP / DOWN (long) | Jump +-10 entries |
| Entry list | OK (click) | Open entry |
| Entry list | OK (long) | Back to categories |
| Detail | UP / DOWN (press) | Previous / next entry (wrap) |
| Detail | UP / DOWN (long) | Jump +-10 entries |
| Detail | UP / DOWN (double) | Jump to first / last entry |
| Detail | OK (long) | Back to list |
| Detail | OK (double) | Toggle mute |

## Repository layout

```
main/                 Firmware application (pure C core + LVGL UI)
  dex_core.c/h        Static data access + attribute string parser
  dex_sprite.c/h      Runtime sprite inflate + 2x scaler
  dex_layout.c/h      Layout geometry for the 240x320 screen
  dex_ui.c            Three-page UI state machine (category/list/detail)
  dex_audio.c/h       BGM playback (IMA ADPCM stream decode + I2S output)
  dex_adpcm.c/h       IMA ADPCM 4-bit decoder (~100 lines of C)
  dex_bgm_data.c/h    Generated: BGM track C arrays (from convert_bgm.py)
  dex_battery.c/h     Battery indicator + idle backlight dimming
  dex_static.c/h      Generated: entry table + attribute pool (from data JSON)
  dex_sprites.bin     Generated: sprite atlas (TOC + raw-DEFLATE RGB565)
  dex_attrs.bin       Generated: attribute string pool
  vendor/miniz/       miniz (tinfl only) for raw-DEFLATE decompression
components/bsp/       Board support package (from upstream ai-passport)
bootloader_components/recovery_boot_hook/
                      Permanent 5-second UP-key recovery hook (template contract)
tools/gen_dex_data.py Data pipeline: data JSON -> RGB565 atlas + C tables
tools/convert_bgm.py  BGM pipeline: OST MP3 -> IMA ADPCM WAV (ffmpeg)
tools/gen_bgm_data.py BGM pipeline: ADPCM WAV -> C arrays for firmware
tools/validate_dex_data.py  Generated-artifact consistency validator
data pipeline source  sibling repo `stardew-valley-data` (data/*.json + images)
docs/                 Development docs
  README.md           Feature/architecture/build/troubleshooting guide
  development-log.md  Full development process, pitfalls and decisions
```

## Building

Requires ESP-IDF 5.5.x (developed against 5.5.5) with the esp32c3 target.
Managed components (LVGL 9.5, esp_lvgl_port, espressif/button) are fetched on
first configure.

```bash
idf.py set-target esp32c3
idf.py build
idf.py merge-bin   # complete image at build/merged-binary.bin (about 1.5 MB)
idf.py -p COMx flash monitor
```

Python 3.11 + Pillow are required only when regenerating data assets:

```bash
# from the sibling stardew-valley-data checkout
python tools/gen_dex_data.py --data D:\path\to\stardew-valley-data
python tools/validate_dex_data.py --data D:\path\to\stardew-valley-data
```

## Partition contract

The template partition layout is preserved unchanged — the mini-program BLE
installer depends on it:

| Name | Offset | Size | Purpose |
| --- | --- | --- | --- |
| nvs | 0x9000 | 24 KB | Key-value store (last-viewed position) |
| factory | 0x10000 | 3 MB | App (dex + embedded assets) |
| cardid | 0x356000 | 16 KB | BLE pairing data (protected) |
| recovery | 0x700000 | 1 MB | Permanent recovery app (protected) |

The 5-second UP-key bootloader hook into Recovery is retained.

## Credits

- Data and sprites: [stardew-valley-data](https://github.com/chiefpansancolt/stardew-valley-data)
  community project. Stardew Valley and all associated assets are
  Copyright (c) ConcernedApe. This is a non-commercial fan project; game
  assets remain the property of their owner.
- Hardware board support, recovery hook: [ai-passport](https://github.com/FoloToy/ai-passport)
  (see `LICENSE.ai-passport`).
- Pokédex approach reference: [sunny0826/ai-passport](https://github.com/sunny0826/ai-passport) `feature/pokedex` branch.
- miniz: richgel999/miniz (MIT).

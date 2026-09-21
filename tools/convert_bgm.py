"""
Convert BGM tracks to IMA ADPCM WAV (8kHz mono 4-bit) using ffmpeg.
No external decoder library needed on ESP32 — ~50 lines of C.
Output: tools/bgm/*.wav (IMA ADPCM)
"""
import subprocess, os, glob

FFMPEG = r"D:\Project\ai-passport-stardew-valley\tools\ffmpeg_bin\ffmpeg.exe"
OST_DIR = r"C:\Users\yeats\Desktop\ConcernedApe - Stardew Valley OST"
OUT_DIR = r"D:\Project\ai-passport-stardew-valley\tools\_bgm"

# 1 track: full Stardew Valley Overture (2:26, no trim)
TRACKS = [
    ("01 Stardew Valley Overture", 0, "stardew_overture"),
]

os.makedirs(OUT_DIR, exist_ok=True)

results = []
for pattern, seconds, out_name in TRACKS:
    matches = glob.glob(os.path.join(OST_DIR, f"*{pattern}*"))
    if not matches:
        print(f"NOT FOUND: {pattern}")
        continue
    src = matches[0]
    dst = os.path.join(OUT_DIR, f"{out_name}.wav")
    cmd = [FFMPEG, "-y", "-i", src]
    if seconds > 0:
        cmd += ["-t", str(seconds)]
    cmd += [
        "-ac", "1",              # mono
        "-ar", "8000",           # 8kHz
        "-f", "wav",
        "-codec:a", "adpcm_ima_wav",  # IMA ADPCM 4-bit
        dst
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    size = os.path.getsize(dst)
    results.append((out_name, dst, size))
    print(f"  {out_name:20s} -> {size:>8,} bytes ({size/1024:.0f} KB)")

total = sum(s for _, _, s in results)
print(f"\n  Total audio: {total:,} bytes ({total/1024:.0f} KB)")
print(f"  + ADPCM decoder ~2 KB code = ~{total/1024 + 2:.0f} KB")
print(f"  Remaining: ~1530 KB -> {'OK' if total/1024 + 2 < 1530 else 'OVER!'}")
print(f"  Could fit {1530 // (total // len(results) / 1024):.0f} tracks total")

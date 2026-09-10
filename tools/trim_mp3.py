"""
Pure-Python MP3 frame parser / trimmer.
No ffmpeg dependency — parses MPEG frame headers directly.

Usage:
    python trim_mp3.py input.mp3 output.mp3 --seconds 60
"""
import struct, sys, os

# MPEG1/2/2.5 Layer III bitrate tables (kbps)
BITRATE_TABLE = {
    # (version, layer): [index0..15]
    (3, 1): [0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0],  # MPEG1 L1
    (3, 2): [0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,0],  # MPEG1 L2
    (3, 3): [0,32,40,48, 56, 64, 80, 88, 96,112,128,160,192,224,256,320],# MPEG1 L3
    (2, 1): [0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0],     # MPEG2 L1
    (2, 2): [0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0],     # MPEG2 L2
    (2, 3): [0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0],     # MPEG2 L3
    (1, 1): [0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0],     # MPEG2.5 L1
    (1, 2): [0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0],     # MPEG2.5 L2
    (1, 3): [0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0],     # MPEG2.5 L3
}

SAMPLE_RATE_TABLE = {
    3: [44100, 48000, 32000],  # MPEG1
    2: [22050, 24000, 16000],  # MPEG2
    1: [11025, 12000,  8000],  # MPEG2.5
}

VERSION_NAMES = {3: "MPEG1", 2: "MPEG2", 1: "MPEG2.5"}

def parse_frame_header(h):
    """Parse 4-byte MP3 frame header. Returns (version, layer, bitrate_kbps, sample_rate, padding, frame_size) or None."""
    if len(h) < 4:
        return None
    b0, b1, b2, b3 = h[0], h[1], h[2], h[3]
    # Sync: 11 bits all 1
    sync = (b0 << 3) | (b1 >> 5)
    if sync != 0x7FF:
        return None
    version = (b1 >> 3) & 0x3  # 3=MPEG1, 2=MPEG2, 0=MPEG2.5, 1=reserved
    layer = (b1 >> 1) & 0x3    # 3=L1, 2=L2, 1=L3
    if version == 0 or layer == 0:
        return None
    br_idx = (b2 >> 4) & 0xF
    sr_idx = (b2 >> 2) & 0x3
    padding = (b2 >> 1) & 0x1
    if br_idx == 0 or br_idx == 15 or sr_idx == 3:
        return None
    key = (version, layer)
    if key not in BITRATE_TABLE:
        return None
    bitrate = BITRATE_TABLE[key][br_idx] * 1000  # bps
    sample_rate = SAMPLE_RATE_TABLE[version][sr_idx]
    if layer == 3:  # L1
        frame_size = (12 * bitrate // sample_rate + padding) * 4
    else:  # L2, L3
        frame_size = 144 * bitrate // sample_rate + padding
    return version, layer, bitrate, sample_rate, padding, frame_size

def find_sync(data, start):
    """Find next MP3 sync word (11 bits = 0x7FF) starting from `start`."""
    i = start
    n = len(data) - 1
    while i < n:
        if data[i] == 0xFF and (data[i+1] & 0xE0) == 0xE0:
            return i
        i += 1
    return -1

def analyze_mp3(path):
    """Analyze MP3 file: count frames, total duration, bitrate range."""
    data = open(path, "rb").read()
    # Skip ID3v2 tag
    offset = 0
    if data[:3] == b"ID3":
        id3_size = ((data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 |
                    (data[8] & 0x7F) << 7  | (data[9] & 0x7F)) + 10
        offset = id3_size
    frames = 0
    duration = 0.0
    pos = find_sync(data, offset)
    versions, layers, rates = set(), set(), set()
    while pos >= 0 and pos + 4 <= len(data):
        info = parse_frame_header(data[pos:pos+4])
        if info is None:
            pos = find_sync(data, pos + 1)
            continue
        ver, lay, br, sr, pad, fsz = info
        versions.add(ver)
        layers.add(lay)
        rates.add(br)
        samples_per_frame = 1152 if ver == 3 else 576
        duration += samples_per_frame / sr
        frames += 1
        pos += fsz
        pos = find_sync(data, pos)
    return frames, duration, versions, layers, rates, offset

def trim_mp3(in_path, out_path, max_seconds):
    """Trim MP3 to at most max_seconds, preserving frame alignment."""
    data = open(in_path, "rb").read()
    # Preserve ID3v2 tag
    id3_tag = b""
    offset = 0
    if data[:3] == b"ID3":
        id3_size = ((data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 |
                    (data[8] & 0x7F) << 7  | (data[9] & 0x7F)) + 10
        id3_tag = data[:id3_size]
        offset = id3_size

    out_frames = bytearray()
    accumulated = 0.0
    pos = find_sync(data, offset)
    count = 0
    while pos >= 0 and pos + 4 <= len(data):
        info = parse_frame_header(data[pos:pos+4])
        if info is None:
            pos = find_sync(data, pos + 1)
            continue
        ver, lay, br, sr, pad, fsz = info
        samples_per_frame = 1152 if ver == 3 else 576
        frame_dur = samples_per_frame / sr
        if accumulated + frame_dur > max_seconds:
            break
        out_frames.extend(data[pos:pos+fsz])
        accumulated += frame_dur
        count += 1
        pos += fsz
        pos = find_sync(data, pos)

    with open(out_path, "wb") as f:
        f.write(id3_tag)
        f.write(out_frames)
    return count, accumulated

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description="Trim MP3 to N seconds (pure Python)")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--analyze", action="store_true")
    args = p.parse_args()

    if args.analyze:
        frames, dur, vers, lays, rates = analyze_mp3(args.input)[:5]
        vnames = ", ".join(VERSION_NAMES.get(v, str(v)) for v in vers)
        lnames = ", ".join(f"L{l}" for l in lays)
        print(f"File:    {args.input}")
        print(f"Size:    {os.path.getsize(args.input):,} bytes")
        print(f"Version: {vnames}")
        print(f"Layer:   {lnames}")
        print(f"Rates:   {sorted(set(r//1000 for r in rates))} kbps")
        print(f"Frames:  {frames}")
        print(f"Duration:{dur:.1f}s ({dur/60:.1f}min)")
    else:
        n, dur = trim_mp3(args.input, args.output, args.seconds)
        out_size = os.path.getsize(args.output)
        print(f"Trimmed: {n} frames, {dur:.1f}s -> {args.output} ({out_size:,} bytes)")

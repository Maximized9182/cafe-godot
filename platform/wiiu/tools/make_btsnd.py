#!/usr/bin/env python3
"""Converts any audio file to a Wii U boot sound (.btsnd) with ffmpeg.

btsnd: 8-byte big-endian header (u32 type, s32 loop start; 0/0 = play once,
type 2 = loop from loop start), then 16-bit big-endian stereo PCM at 48 kHz,
at most 960000 frames (20 s). Format from vgmstream's src/meta/btsnd.c.

By default the sound plays once: header type 2 (looping) with the loop point
in 0.5 s of silence added at the end (a type 0 file stayed silent on the
console, 2026-11-15). --loop loops the whole sound; --type0 writes type 0.

Usage: make_btsnd.py input.ogg output.btsnd [--loop | --type0]
"""
import struct
import subprocess
import sys

MAX_FRAMES = 960000


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    src, dst = sys.argv[1], sys.argv[2]
    loop = "--loop" in sys.argv[3:]
    type0 = "--type0" in sys.argv[3:]
    pcm = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", src, "-ac", "2", "-ar", "48000", "-f", "s16be", "-"],
        check=True, stdout=subprocess.PIPE).stdout
    frames = len(pcm) // 4
    if frames > MAX_FRAMES:
        print(f"make_btsnd: {frames / 48000:.1f} s is longer than 20 s; cutting it", file=sys.stderr)
        pcm = pcm[:MAX_FRAMES * 4]
        frames = MAX_FRAMES
    if loop:
        header = struct.pack(">Ii", 2, 0)
    elif type0:
        header = struct.pack(">Ii", 0, 0)
    else:
        tail = min(48000 // 2, MAX_FRAMES - frames)
        if tail <= 0:
            pcm = pcm[:(MAX_FRAMES - 4800) * 4]
            frames = MAX_FRAMES - 4800
            tail = 4800
        header = struct.pack(">Ii", 2, frames)
        pcm += bytes(tail * 4)
        frames += tail
    with open(dst, "wb") as f:
        f.write(header)
        f.write(pcm)
    print(f"make_btsnd: wrote {dst} ({frames / 48000:.2f} s)")


if __name__ == "__main__":
    main()

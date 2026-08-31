#!/usr/bin/env python3
"""Turn any video into a frame blob the panel can play.

    ./tools/encode_video.py clip.mp4 video.bin --fps 15

Scales to the panel's geometry, converts to RGB565 and prepends a 16 byte
header so the firmware knows what it is holding. Requires ffmpeg on PATH.

The output is raw frames, so size grows linearly: at 128x64 one frame is 16 KiB
and one second at 15 fps is 240 KiB. Check it against the video partition
before flashing -- that is the real limit on clip length.
"""
import argparse, struct, subprocess, sys

MAGIC = b"HB75"
WIDTH, HEIGHT = 128, 64

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--width", type=int, default=WIDTH)
    ap.add_argument("--height", type=int, default=HEIGHT)
    args = ap.parse_args()

    # Letterbox rather than distort: scale to fit, then pad to the exact size.
    vf = (f"fps={args.fps},"
          f"scale={args.width}:{args.height}:force_original_aspect_ratio=decrease:flags=lanczos,"
          f"pad={args.width}:{args.height}:(ow-iw)/2:(oh-ih)/2")

    cmd = ["ffmpeg", "-v", "error", "-i", args.input,
           "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb565le", "-"]
    frames = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout

    frame_bytes = args.width * args.height * 2
    if len(frames) % frame_bytes:
        sys.exit("ffmpeg returned a partial frame -- check the input file")
    count = len(frames) // frame_bytes

    with open(args.output, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<HHHI", args.width, args.height, args.fps, count))
        f.write(b"\0" * 2)                      # pad the header to 16 bytes
        f.write(frames)

    total = 16 + len(frames)
    print(f"{count} frames, {args.width}x{args.height} @ {args.fps} fps")
    print(f"{total/1024/1024:.2f} MiB, {count/args.fps:.1f} seconds")
    print(f"\nflash it with:\n"
          f"  parttool.py --port /dev/cu.usbmodem* write_partition "
          f"--partition-name video --input {args.output}")

if __name__ == "__main__":
    main()

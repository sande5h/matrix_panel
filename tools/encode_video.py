#!/usr/bin/env python3
"""Turn any video into a frame blob the panel can play.

    ./tools/encode_video.py clip.mp4 video.bin --fps 15

Scales to the panel's geometry, converts to RGB565 and prepends a 16 byte
header so the firmware knows what it is holding. Requires ffmpeg on PATH.

The output is raw frames, so size grows linearly: at 128x64 one frame is 16 KiB
and one second at 15 fps is 240 KiB. Check it against the video partition
before flashing -- that is the real limit on clip length.
"""
import argparse, os, pathlib, struct, subprocess, sys, tempfile

MAGIC_RAW   = b"HB75"     # uncompressed RGB565 frames
MAGIC_MJPEG = b"HB7J"     # per-frame JPEG, decoded on the device
WIDTH, HEIGHT = 128, 64

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--start", default=None,
                    help="seek to this timestamp first, e.g. 0:45 or 45")
    ap.add_argument("--duration", type=float, default=None,
                    help="seconds to take; raw frames are big, so this matters")
    ap.add_argument("--limit-mib", type=float, default=5.0,
                    help="warn if the output exceeds the video partition")
    ap.add_argument("--mjpeg", action="store_true",
                    help="store JPEG frames instead of raw RGB565; roughly 10x "
                         "smaller, decoded on the ESP32 with the ROM tjpgd")
    ap.add_argument("--quality", type=int, default=7,
                    help="ffmpeg -q:v for --mjpeg, 2 best .. 31 worst")
    ap.add_argument("--fit", choices=("pad", "crop"), default="pad",
                    help="pad letterboxes the whole frame; crop fills the panel "
                         "and throws away the overflow. A portrait source needs "
                         "crop, or it lands as a narrow strip")
    ap.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0,
                    help="rotate before fitting, for a panel mounted on its side")
    ap.add_argument("--width", type=int, default=WIDTH)
    ap.add_argument("--height", type=int, default=HEIGHT)
    args = ap.parse_args()

    steps = [f"fps={args.fps}"]
    if args.rotate == 90:
        steps.append("transpose=1")          # clockwise
    elif args.rotate == 270:
        steps.append("transpose=2")          # counter-clockwise
    elif args.rotate == 180:
        steps.append("hflip,vflip")

    if args.fit == "crop":
        # Fill the panel, then cut the overflow off the centre.
        steps.append(f"scale={args.width}:{args.height}"
                     f":force_original_aspect_ratio=increase:flags=lanczos")
        steps.append(f"crop={args.width}:{args.height}")
    else:
        # Keep the whole frame and letterbox it. Never distorts.
        steps.append(f"scale={args.width}:{args.height}"
                     f":force_original_aspect_ratio=decrease:flags=lanczos")
        steps.append(f"pad={args.width}:{args.height}:(ow-iw)/2:(oh-ih)/2")
    vf = ",".join(steps)

    base = ["ffmpeg", "-v", "error"]
    if args.start:                      # before -i, so ffmpeg seeks rather than decodes
        base += ["-ss", str(args.start)]
    base += ["-i", args.input]
    if args.duration:
        base += ["-t", str(args.duration)]
    base += ["-vf", vf]

    if args.mjpeg:
        # One file per frame: splitting a concatenated stream on markers is
        # doable but fragile, and this is not the slow part.
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(base + ["-c:v", "mjpeg", "-q:v", str(args.quality),
                                   "-f", "image2", f"{tmp}/%06d.jpg"], check=True)
            jpegs = [pathlib.Path(tmp, n).read_bytes()
                     for n in sorted(os.listdir(tmp))]
        count = len(jpegs)
        if not count:
            sys.exit("ffmpeg produced no frames")

        with open(args.output, "wb") as f:
            f.write(MAGIC_MJPEG)
            f.write(struct.pack("<HHHI", args.width, args.height, args.fps, count))
            f.write(b"\0" * 2)                  # header is 16 bytes
            for j in jpegs:                     # then one size per frame
                f.write(struct.pack("<I", len(j)))
            for j in jpegs:
                f.write(j)

        payload = sum(len(j) for j in jpegs)
        total = 16 + 4 * count + payload
        print(f"largest frame {max(len(j) for j in jpegs)} bytes, "
              f"mean {payload // count} bytes")
    else:
        out = subprocess.run(base + ["-f", "rawvideo", "-pix_fmt", "rgb565le", "-"],
                             stdout=subprocess.PIPE, check=True).stdout
        frame_bytes = args.width * args.height * 2
        if len(out) % frame_bytes:
            sys.exit("ffmpeg returned a partial frame -- check the input file")
        count = len(out) // frame_bytes

        with open(args.output, "wb") as f:
            f.write(MAGIC_RAW)
            f.write(struct.pack("<HHHI", args.width, args.height, args.fps, count))
            f.write(b"\0" * 2)
            f.write(out)

        total = 16 + len(out)
    mib = total / 1024 / 1024
    print(f"{count} frames, {args.width}x{args.height} @ {args.fps} fps")
    print(f"{mib:.2f} MiB, {count/args.fps:.1f} seconds")
    if mib > args.limit_mib:
        fits = count / args.fps * args.limit_mib / mib
        print(f"\n  WARNING: this will not fit the {args.limit_mib:g}M video "
              f"partition.\n  Trim to about {fits:.0f}s with --duration, drop "
              f"--fps, or enlarge the partition in partitions.csv.")
    print(f"\nflash it with:\n"
          f"  parttool.py --port /dev/cu.usbmodem* write_partition "
          f"--partition-name video --input {args.output}")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Turn any video into a frame blob the panel can play.

    ./tools/encode_video.py clip.mp4 video.bin --fps 15

Scales to the panel's geometry, converts to RGB565 and prepends a 16 byte
header so the firmware knows what it is holding. Requires ffmpeg on PATH.

The output is raw frames, so size grows linearly: at 128x64 one frame is 16 KiB
and one second at 15 fps is 240 KiB. Check it against the video partition
before flashing -- that is the real limit on clip length.
"""
import argparse, concurrent.futures, os, pathlib, shutil, struct, subprocess, sys, tempfile

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
    ap.add_argument("--limit-mib", type=float, default=12.9,
                    help="warn if the output exceeds the video partition")
    ap.add_argument("--mjpeg", action="store_true",
                    help="store JPEG frames instead of raw RGB565; roughly 10x "
                         "smaller, decoded on the ESP32 with the ROM tjpgd")
    ap.add_argument("--quality", type=int, default=85,
                    help="libjpeg quality for --mjpeg, 0 worst .. 100 best. "
                         "NOTE this is cjpeg's scale; it used to be ffmpeg's "
                         "-q:v, where the numbers ran 2 (best) to 31 (worst)")
    ap.add_argument("--chroma", choices=("420", "422", "444"), default="444",
                    help="chroma sampling for --mjpeg. Colour resolution is "
                         "worth more than quantisation steps at 128x64, so 444 "
                         "is the default and 420 is the one to pick only when "
                         "the decode budget is tight")
    ap.add_argument("--fit", choices=("pad", "crop"), default="pad",
                    help="pad letterboxes the whole frame; crop fills the panel "
                         "and throws away the overflow. A portrait source needs "
                         "crop, or it lands as a narrow strip")
    ap.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0,
                    help="rotate before fitting. Also fixes footage whose "
                         "content sits sideways inside an upright frame")
    ap.add_argument("--focus", type=float, default=0.5,
                    help="where --fit crop takes its band from: 0 is the top "
                         "edge, 1 the bottom, 0.5 the middle. Faces are rarely "
                         "in the middle of a portrait shot")
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
        # Fill the panel, then cut a band out of the overflow. ffmpeg evaluates
        # the offset against the scaled input, so --focus works whatever the
        # source aspect ratio is.
        focus = min(max(args.focus, 0.0), 1.0)
        steps.append(f"scale={args.width}:{args.height}"
                     f":force_original_aspect_ratio=increase:flags=lanczos")
        steps.append(f"crop={args.width}:{args.height}"
                     f":(iw-ow)*{focus}:(ih-oh)*{focus}")
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
        # Frames are encoded by cjpeg, not by ffmpeg. ffmpeg writes 4:2:2 and
        # 4:4:4 with chroma sampling factors of 0x12, and the ESP32 ROM tjpgd
        # accepts a luma factor of 0x11/0x21/0x22 but requires both chroma
        # factors to be exactly 0x11 -- so those clips decode to nothing but
        # JDR_FMT3. libjpeg writes the canonical factors for all three modes.
        cjpeg = shutil.which("cjpeg")
        if not cjpeg:
            sys.exit("cjpeg not found -- brew install jpeg-turbo. ffmpeg's own "
                     "mjpeg encoder cannot produce 422 or 444 the ESP32 will "
                     "decode; see the comment above this message in the source.")
        if args.quality <= 31:
            print("warning: --quality is now libjpeg's 0..100 scale (higher is "
                  "better), not ffmpeg's 2..31. %d is very low." % args.quality,
                  file=sys.stderr)

        sample = {"420": "2x2", "422": "2x1", "444": "1x1"}[args.chroma]

        # One file per frame: splitting a concatenated stream on markers is
        # doable but fragile, and this is not the slow part.
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(base + ["-c:v", "ppm", "-f", "image2",
                                   f"{tmp}/%06d.ppm"], check=True)
            names = sorted(os.listdir(tmp))

            def encode(n):
                src = os.path.join(tmp, n)
                dst = src[:-4] + ".jpg"
                subprocess.run([cjpeg, "-quality", str(args.quality),
                                "-sample", sample, "-optimize",
                                "-outfile", dst, src], check=True)
                return pathlib.Path(dst).read_bytes()

            # cjpeg is a subprocess, so threads are enough to keep the cores fed.
            with concurrent.futures.ThreadPoolExecutor() as pool:
                jpegs = list(pool.map(encode, names))

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

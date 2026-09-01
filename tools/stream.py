#!/usr/bin/env python3
"""Stream frames to the panel over TCP -- no encoder, no partition, no flashing.

    ./tools/stream.py clip.mp4 --loop
    ./tools/stream.py screen
    ./tools/stream.py camera
    ./tools/stream.py --list          which avfoundation devices exist

Frames go over the wire uncompressed, so unlike a flashed clip there is no
compression error at all: what the panel shows is as close to the source as
its six bit planes can get. That costs about 5.9 Mbps at 30 fps, which is
nothing to the ESP32's wifi, and it moves the per-frame cost off the CPU --
decoding a JPEG is the expensive part of playback, and there is no JPEG here.

The panel listens on 8089 and shows whatever arrives for as long as it keeps
arriving; close the sender and it goes back to what it was showing before.
"""
import argparse, socket, struct, subprocess, sys

WIDTH, HEIGHT = 128, 64
FRAME_BYTES = WIDTH * HEIGHT * 3
MAGIC = b"HB7S"
PORT = 8089


def list_devices():
    """avfoundation prints its device table to stderr and then exits non-zero,
    which is normal -- there is no input to open."""
    subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation",
                    "-list_devices", "true", "-i", ""], check=False)


def build_ffmpeg(args):
    cmd = ["ffmpeg", "-hide_banner", "-v", "error"]

    if args.source == "screen":
        cmd += ["-f", "avfoundation", "-capture_cursor", "1",
                "-framerate", str(args.fps), "-i", args.device or "1:none"]
    elif args.source == "camera":
        cmd += ["-f", "avfoundation",
                "-framerate", str(args.fps), "-i", args.device or "0:none"]
    else:
        # -re paces a file at its own speed. Without it ffmpeg decodes as fast
        # as it can and the whole clip arrives in a couple of seconds.
        cmd += ["-re"]
        if args.loop:
            cmd += ["-stream_loop", "-1"]
        cmd += ["-i", args.source]

    steps = [f"fps={args.fps}"]
    if args.rotate in (90, 270):
        steps.append("transpose=1" if args.rotate == 90 else "transpose=2")
    elif args.rotate == 180:
        steps.append("hflip,vflip")

    if args.fit == "crop":
        focus = min(max(args.focus, 0.0), 1.0)
        steps.append(f"scale={WIDTH}:{HEIGHT}:"
                     f"force_original_aspect_ratio=increase:flags=lanczos")
        steps.append(f"crop={WIDTH}:{HEIGHT}:(iw-ow)*{focus}:(ih-oh)*{focus}")
    else:
        steps.append(f"scale={WIDTH}:{HEIGHT}:"
                     f"force_original_aspect_ratio=decrease:flags=lanczos")
        steps.append(f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:black")

    return cmd + ["-vf", ",".join(steps),
                  "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?",
                    help="a video file, or the words 'screen' or 'camera'")
    ap.add_argument("--host", default="matrix-panel.local",
                    help="mDNS is flaky on some machines; pass the IP instead")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--loop", action="store_true", help="repeat a file forever")
    ap.add_argument("--fit", choices=("crop", "pad"), default="crop")
    ap.add_argument("--focus", type=float, default=0.5,
                    help="where --fit crop takes its band: 0 top, 1 bottom")
    ap.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0)
    ap.add_argument("--device", default=None,
                    help="avfoundation device for screen/camera, e.g. '2:none'")
    ap.add_argument("--list", action="store_true",
                    help="list capture devices and exit")
    args = ap.parse_args()

    if args.list:
        list_devices()
        return
    if not args.source:
        ap.error("give a video file, 'screen', or 'camera' (or --list)")

    sock = socket.create_connection((args.host, PORT), timeout=10)
    sock.settimeout(None)
    # The panel consumes frames steadily; Nagle would only add latency.
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"connected to {args.host}:{PORT}", file=sys.stderr)

    ff = subprocess.Popen(build_ffmpeg(args), stdout=subprocess.PIPE)
    sent = 0
    try:
        while True:
            frame = ff.stdout.read(FRAME_BYTES)   # short read only at the end
            if len(frame) < FRAME_BYTES:
                break
            # sendall blocks when the panel falls behind, which is the flow
            # control: no queue to grow, no frames to drop, no drift.
            sock.sendall(MAGIC + frame)
            sent += 1
    except (BrokenPipeError, ConnectionResetError):
        print("panel closed the connection", file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        ff.terminate()
        sock.close()
        print(f"sent {sent} frames", file=sys.stderr)


if __name__ == "__main__":
    main()

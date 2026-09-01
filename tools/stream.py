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
import argparse, re, socket, struct, subprocess, sys

WIDTH, HEIGHT = 128, 64
FRAME_BYTES = WIDTH * HEIGHT * 3
MAGIC_RAW = b"HB7S"
MAGIC_JPEG = b"HB7Z"
PORT = 8089


def probe_devices():
    """[(index, name)] of avfoundation video devices. It prints the table to
    stderr and exits non-zero, which is normal -- there is no input to open."""
    out = subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation",
                          "-list_devices", "true", "-i", ""],
                         capture_output=True, text=True).stderr
    devices, video = [], False
    for line in out.splitlines():
        if "video devices" in line:
            video = True
            continue
        if "audio devices" in line:
            video = False
            continue
        m = re.search(r"\[(\d+)\]\s+(.*)$", line)
        if video and m:
            devices.append((int(m.group(1)), m.group(2).strip()))
    return devices


def list_devices():
    for i, name in probe_devices():
        kind = "screen" if "capture screen" in name.lower() else "camera"
        print("  [%d] %-28s (%s)" % (i, name, kind))


def pick_device(kind):
    """Indices are not stable across machines -- a Mac mini with no camera has
    the screens at 0 and 1, a laptop has the camera at 0 and screens after it.
    So find the device by name rather than assuming a number."""
    devices = probe_devices()
    if not devices:
        sys.exit("ffmpeg listed no avfoundation video devices")
    want_screen = (kind == "screen")
    for i, name in devices:
        if ("capture screen" in name.lower()) == want_screen:
            return "%d:none" % i
    sys.exit("no %s found. Devices are:\n%s" %
             (kind, "\n".join("  [%d] %s" % d for d in devices)))


def build_ffmpeg(args):
    cmd = ["ffmpeg", "-hide_banner", "-v", "error"]

    if args.source in ("screen", "camera"):
        # avfoundation refuses ffmpeg's default yuv420p and lists what it does
        # support, which differs per device. Screens hand out packed RGB, so
        # asking for that keeps the whole path free of colour conversion;
        # cameras usually offer uyvy422 and rarely anything RGB.
        pix = args.pixel_format or ("bgr0" if args.source == "screen" else "uyvy422")
        cmd += ["-f", "avfoundation", "-pixel_format", pix]
        if args.source == "screen":
            # No -framerate here. A screen device refuses it ("Configuration of
            # video device failed, falling back to default") and then never
            # delivers a frame -- the capture hangs rather than erroring, so it
            # looks like a slow network. The fps= filter below sets the output
            # rate anyway, so asking the device for one buys nothing.
            cmd += ["-capture_cursor", "1"]
        else:
            # A camera does want it: it is how a capture mode gets selected.
            cmd += ["-framerate", str(args.fps)]
        cmd += ["-i", args.device or pick_device(args.source)]
    else:
        # -re paces a file at its own speed. Without it ffmpeg decodes as fast
        # as it can and the whole clip arrives in a couple of seconds.
        cmd += ["-re"]
        if args.loop:
            cmd += ["-stream_loop", "-1"]
        cmd += ["-i", args.source]

    steps = [f"fps={args.fps}"]

    # Crop first, so everything after it works on the region rather than the
    # whole screen. A 1920x1080 display squeezed into 128x64 is a 15x
    # reduction and no font survives it; taking a 2:1 region instead means a
    # 2x or 4x reduction, which text does survive.
    if args.crop:
        m = re.fullmatch(r"(\d+)x(\d+)(?:\+(\d+)\+(\d+))?", args.crop)
        if not m:
            sys.exit("--crop wants WxH or WxH+X+Y, e.g. 256x128+0+0")
        cw, ch = m.group(1), m.group(2)
        cx, cy = m.group(3) or "0", m.group(4) or "0"
        steps.append(f"crop={cw}:{ch}:{cx}:{cy}")

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

    cmd += ["-vf", ",".join(steps)]

    if args.format == "raw":
        return cmd + ["-f", "rawvideo", "-pix_fmt", "rgb24", "-"]

    # 4:2:0 only. ffmpeg writes 4:2:2 and 4:4:4 with chroma sampling factors of
    # 0x12, which the ESP32 ROM decoder rejects outright -- see the note in
    # encode_video.py. 4:2:0 comes out as 0x22 0x11 0x11, which it accepts.
    return cmd + ["-c:v", "mjpeg", "-q:v", str(args.quality),
                  "-pix_fmt", "yuvj420p", "-f", "mjpeg", "-"]


def jpeg_frames(stream):
    """Split ffmpeg's concatenated MJPEG output into frames. Splitting on the
    end-of-image marker is safe: inside entropy-coded data an FF byte is
    stuffed as FF 00, so FF D9 only ever appears as the real marker."""
    buf = b""
    while True:
        chunk = stream.read(65536)
        if not chunk:
            return
        buf += chunk
        while True:
            end = buf.find(b"\xff\xd9")
            if end < 0:
                break
            yield buf[:end + 2]
            buf = buf[end + 2:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?",
                    help="a video file, or the words 'screen' or 'camera'")
    ap.add_argument("--host", default="matrix-panel.local",
                    help="mDNS is flaky on some machines; pass the IP instead")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--format", choices=("mjpeg", "raw"), default="mjpeg",
                    help="mjpeg is about an eighth the size and is what makes "
                         "30 fps fit; raw has no compression error at all and "
                         "looks better, but needs 5.9 Mbps at 30 fps")
    ap.add_argument("--quality", type=int, default=7,
                    help="ffmpeg -q:v for --format mjpeg, 2 best .. 31 worst")
    ap.add_argument("--loop", action="store_true", help="repeat a file forever")
    ap.add_argument("--crop", default=None,
                    help="capture only part of the source: WxH or WxH+X+Y in "
                         "source pixels. Use a 2:1 region to match the panel "
                         "-- 256x128 is a 2x reduction and keeps text legible, "
                         "128x64 is pixel perfect. Applies to any source")
    ap.add_argument("--fit", choices=("crop", "pad"), default="crop")
    ap.add_argument("--focus", type=float, default=0.5,
                    help="where --fit crop takes its band: 0 top, 1 bottom")
    ap.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0)
    ap.add_argument("--pixel-format", default=None,
                    help="avfoundation input format. Defaults to bgr0 for the "
                         "screen and uyvy422 for a camera; if ffmpeg says the "
                         "device does not support it, it prints the list that "
                         "device does accept -- pass one of those")
    ap.add_argument("--device", default=None,
                    help="avfoundation device for screen/camera, e.g. '2:none'. "
                         "Found by name if not given, since the indices differ "
                         "between machines")
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
        if args.format == "raw":
            while True:
                frame = ff.stdout.read(FRAME_BYTES)   # short read at the end
                if len(frame) < FRAME_BYTES:
                    break
                # sendall blocks when the panel falls behind, which is the flow
                # control: no queue to grow, no frames to drop, no drift.
                sock.sendall(MAGIC_RAW + frame)
                sent += 1
        else:
            for jpg in jpeg_frames(ff.stdout):
                sock.sendall(MAGIC_JPEG + struct.pack("<I", len(jpg)) + jpg)
                sent += 1
        if sent == 0:
            print("ffmpeg produced no frames -- its error is above.",
                  file=sys.stderr)
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

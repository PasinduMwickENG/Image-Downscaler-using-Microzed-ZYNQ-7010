import argparse
import socket
import struct
import numpy as np
from PIL import Image

BOARD_IP = '192.168.1.10'   # must match BOARD_IP in helloworld.c
BOARD_PORT = 5001           # must match SERVER_PORT in helloworld.c

# Must match MAX_IMG_BYTES in helloworld.c - the max bytes a single image
# buffer may occupy in DDR. The image resolution itself is no longer fixed
# in the image_downscale IP; it's sent dynamically per-transfer and latched
# into the IP's AXI4-Lite WIDTH/HEIGHT registers by the firmware.
MAX_IMG_BYTES = 256 * 1024 * 1024


def _recv_exact(sock, n):
    """Read exactly n bytes from the socket or return None on timeout/close."""
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(min(1 << 20, n - len(buf)))
        except socket.timeout:
            return None
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def send_image(sock, path, width=None, height=None):
    img = Image.open(path).convert('RGB')
    if width and height:
        img = img.resize((width, height))

    # The IP downscales in 2x2 blocks, so it needs even dimensions - crop by
    # a pixel instead of silently misbehaving on hardware.
    w, h = img.size
    img = img.crop((0, 0, w - (w % 2), h - (h % 2)))

    arr = np.array(img)
    h, w, _ = arr.shape

    if w * h * 4 > MAX_IMG_BYTES:
        raise ValueError(f"{w}x{h} ({w*h*4} bytes) exceeds the firmware's max buffer of {MAX_IMG_BYTES} bytes")

    # Pad to 32-bit
    padded = np.zeros((h, w, 4), dtype=np.uint8)
    padded[:, :, :3] = arr
    data = padded.tobytes()

    print(f"Sending {w}x{h} ...")
    sock.sendall(struct.pack('<II', w, h))
    sock.sendall(data)


def receive_image(sock):
    header = _recv_exact(sock, 8)
    if header is None:
        print("Timeout/closed waiting for header")
        return None
    nw, nh = struct.unpack('<II', header)
    if not (0 < nw <= 65535 and 0 < nh <= 65535) or nw * nh * 4 > MAX_IMG_BYTES:
        print(f"Bogus header {nw}x{nh} - stream desynced")
        return None
    print(f"Receiving {nw}x{nh} ...")
    data = _recv_exact(sock, nw * nh * 4)
    if data is None:
        print(f"Timeout receiving image data (expected {nw * nh * 4} bytes)")
        return None
    arr = np.frombuffer(data, dtype=np.uint8).reshape((nh, nw, 4))
    return Image.fromarray(arr[:, :, :3])


def main():
    parser = argparse.ArgumentParser(description="Send an image to the FPGA for downscaling over Ethernet.")
    parser.add_argument("image", nargs="?", default="test.jpg", help="Path to the input image (default: test.png)")
    parser.add_argument("--width", type=int, default=None, help="Resize the image to this width before sending (default: send at its native size)")
    parser.add_argument("--height", type=int, default=None, help="Resize the image to this height before sending (default: send at its native size)")
    parser.add_argument("--ip", default=BOARD_IP, help=f"Board IP address (default: {BOARD_IP})")
    parser.add_argument("--port", type=int, default=BOARD_PORT, help=f"Board TCP port (default: {BOARD_PORT})")
    parser.add_argument("-o", "--output", default="downscaled.png", help="Path to save the downscaled result (default: downscaled.png)")
    args = parser.parse_args()

    sock = socket.create_connection((args.ip, args.port), timeout=30)
    try:
        send_image(sock, args.image, width=args.width, height=args.height)
        result = receive_image(sock)
    finally:
        sock.close()

    if result:
        result.save(args.output)
        print(f"Saved -> {args.output}")


if __name__ == "__main__":
    main()

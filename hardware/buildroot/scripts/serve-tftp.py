"""Read-only RAM-boot transfer server; only three named images, one device IP."""
import argparse
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time

FILES = {"uImage", "uInitrd", "am335x-raumfeld-connector2.dtb"}


def transfer(listener, root, device):
    request, peer = listener.recvfrom(2048)
    if peer[0] != device:
        return
    fields = request[2:].split(b"\0")
    name = fields[0].decode("ascii", errors="replace")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stream:
        stream.bind((listener.getsockname()[0], 0))
        stream.settimeout(1)
        if request[:2] != b"\0\1" or name not in FILES or len(fields) < 2 or fields[1].lower() != b"octet":
            stream.sendto(b"\0\5\0\2Read-only image access only\0", peer)
            return
        try:
            image = (root / name).open("rb")
        except OSError:
            stream.sendto(b"\0\5\0\1Image unavailable\0", peer)
            return
        print(f"Sending {name} to {peer[0]}", flush=True)
        total, block = 0, 1
        with image:
            while True:
                payload = image.read(512)
                packet = struct.pack(">HH", 3, block) + payload
                acknowledged = False
                for _ in range(10):
                    stream.sendto(packet, peer)
                    try:
                        ack, sender = stream.recvfrom(2048)
                    except socket.timeout:
                        continue
                    if sender == peer and ack == struct.pack(">HH", 4, block):
                        acknowledged = True
                        break
                if not acknowledged:
                    print(f"Timeout sending {name}, block {block}", flush=True)
                    return
                total += len(payload)
                if len(payload) < 512:
                    print(f"Completed {name}: {total} bytes", flush=True)
                    return
                block = (block + 1) & 0xffff


def self_test():
    with tempfile.TemporaryDirectory() as directory, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        root = Path(directory)
        data = bytes(range(256)) * 8
        (root / "uImage").write_bytes(data)
        server.bind(("127.0.0.1", 0))
        client.settimeout(3)
        worker = threading.Thread(target=transfer, args=(server, root, "127.0.0.1"))
        worker.start()
        # Unsupported option is ignored, as permitted by TFTP option negotiation.
        client.sendto(b"\0\1uImage\0octet\0blksize\0" + b"1468\0", server.getsockname())
        received = bytearray()
        block = 1
        while True:
            packet, peer = client.recvfrom(2048)
            assert packet[:4] == struct.pack(">HH", 3, block)
            received.extend(packet[4:])
            client.sendto(struct.pack(">HH", 4, block), peer)
            if len(packet) < 516:
                break
            block += 1
        worker.join(3)
        assert not worker.is_alive() and received == data
        worker = threading.Thread(target=transfer, args=(server, root, "127.0.0.1"))
        worker.start()
        client.sendto(b"\0\1../private/id_ed25519\0octet\0", server.getsockname())
        packet, _ = client.recvfrom(2048)
        assert packet[:4] == b"\0\5\0\2"
        worker.join(3)
        assert not worker.is_alive()
    print("PASS: image transfer, exact-block EOF, option fallback, path rejection.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host")
    parser.add_argument("--device")
    parser.add_argument("--port", type=int, default=69)
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--images", type=Path, default=Path(__file__).resolve().parent.parent / "output/connector2")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if not args.host or not args.device:
            parser.error("--host and --device are required")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind((args.host, args.port))
            server.settimeout(1)
            print(f"Read-only TFTP on {args.host}:{args.port}, device {args.device}", flush=True)
            deadline = time.monotonic() + args.seconds
            try:
                while time.monotonic() < deadline:
                    try:
                        transfer(server, args.images, args.device)
                    except socket.timeout:
                        continue
            except KeyboardInterrupt:
                pass

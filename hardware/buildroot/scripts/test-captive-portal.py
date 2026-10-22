"""Host-only HTTP probe, interface isolation and slow-client checks."""
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="galdrun-captive-portal-") as directory:
    binary = Path(directory) / "portal"
    if len(sys.argv) == 2:
        firmware = Path(sys.argv[1])
        command = ["qemu-arm", "-L", str(firmware), str(firmware / "usr/sbin/raumfeld-captive-portal")]
    else:
        subprocess.run(["c++", "-std=c++17", "-Os", "-Wall", "-Wextra", "-Werror",
                        str(project / "board/common/captive-portal.cpp"), "-o", str(binary)], check=True)
        command = [str(binary)]
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.2", 0))
        port = reserved.getsockname()[1]
    process = subprocess.Popen([*command, "127.0.0.2", str(port)])
    try:
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.2", port), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise AssertionError("Portal did not listen")

        def request(path, host="connectivitycheck.gstatic.com", method="GET", split=False):
            with socket.create_connection(("127.0.0.2", port), timeout=4) as connection:
                message = f"{method} {path} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode()
                if split:
                    connection.sendall(message[:10])
                    time.sleep(0.05)
                    connection.sendall(message[10:])
                else:
                    connection.sendall(message)
                data = b""
                while chunk := connection.recv(1024):
                    data += chunk
                return data.decode()

        for path, host in (("/generate_204", "connectivitycheck.gstatic.com"),
                           ("/hotspot-detect.html", "captive.apple.com"),
                           ("/connecttest.txt", "www.msftconnecttest.com"),
                           ("/ncsi.txt", "www.msftncsi.com"),
                           ("/", "127.0.0.2"), ("/anything?query=1", "example.com")):
            response = request(path, host)
            assert response.startswith("HTTP/1.1 302 Found\r\n"), response
            assert "Location: http://127.0.0.2:4720/\r\n" in response, response
            assert response.endswith("\r\n\r\n"), response
        assert request("/", method="HEAD").startswith("HTTP/1.1 302")
        assert request("/api", method="POST").startswith("HTTP/1.1 405")
        assert request("/generate_204", split=True).startswith("HTTP/1.1 302")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
        except OSError:
            pass
        else:
            raise AssertionError("Portal exposed on another interface")
        with socket.create_connection(("127.0.0.2", port), timeout=4) as slow:
            slow.sendall(b"GET /")
            assert slow.recv(1024) == b"", "Incomplete request did not time out"
        assert request("/").startswith("HTTP/1.1 302"), "Slow client stopped the portal"
        print("PASS: Android/Apple/Windows redirects, split headers, interface isolation, slow-client recovery")
    finally:
        process.terminate()
        process.wait(timeout=3)

    if len(sys.argv) == 2:
        network = (project / "board/common/overlay/usr/sbin/raumfeld-network").read_text()
        config = network.split('cat > "$state/dnsmasq.conf" <<EOF\n', 1)[1].split('\nEOF', 1)[0]
        for name, value in {"wifi": "wlan0", "AP_ADDRESS": "192.168.77.1",
                            "AP_POOL_START": "192.168.77.10", "AP_POOL_END": "192.168.77.100",
                            "state": directory}.items():
            config = config.replace("$" + name, value)
        config_path = Path(directory) / "dnsmasq.conf"
        config_path.write_text(config + "\n")
        dns_command = ["qemu-arm", "-L", str(firmware), str(firmware / "usr/sbin/dnsmasq")]
        subprocess.run([*dns_command, "--test", "--conf-file=" + str(config_path)], check=True)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserved:
            reserved.bind(("127.0.0.2", 0))
            dns_port = reserved.getsockname()[1]
        # Exercise the shipped DNS binary on a private loopback port, without DHCP.
        lines = [line for line in config.splitlines()
                 if not line.startswith(("dhcp-", "pid-file=", "except-interface="))]
        lines = [line.replace("port=53", f"port={dns_port}").replace("interface=wlan0", "interface=lo")
                 .replace("listen-address=192.168.77.1", "listen-address=127.0.0.2") for line in lines]
        config_path.write_text("\n".join(lines) + "\n")
        dns_process = subprocess.Popen([*dns_command, "--no-daemon", "--conf-file=" + str(config_path)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for kind in (1, 28):
                name = b"".join(bytes([len(part)]) + part for part in b"connectivitycheck.gstatic.com".split(b".")) + b"\x00"
                packet = struct.pack("!6H", 123, 0x100, 1, 0, 0, 0) + name + struct.pack("!2H", kind, 1)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as query:
                    query.settimeout(0.2)
                    for _ in range(30):
                        query.sendto(packet, ("127.0.0.2", dns_port))
                        try:
                            reply, _ = query.recvfrom(4096)
                            break
                        except socket.timeout:
                            assert dns_process.poll() is None, "Firmware DNS server failed"
                    else:
                        raise AssertionError("DNS probe timed out")
                _, flags, _, answers, _, _ = struct.unpack("!6H", reply[:12])
                assert flags & 0xF == 0, "DNS query did not succeed"
                if kind == 1:
                    assert answers == 1 and reply.endswith(socket.inet_aton("192.168.77.1")), reply
                else:
                    assert answers == 0, "AAAA query must return no data locally"
            print("PASS: shipped dnsmasq configuration, wildcard A answer, local empty AAAA answer")
        finally:
            dns_process.terminate()
            dns_process.wait(timeout=3)

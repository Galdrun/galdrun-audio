// Connector 3 MCU notifications -> evdev, without the vendor daemon.
// Serial protocol: 0x55, type, LE16 command, LE32 value, transaction,
// checksum, LE16 payload length, payload. Bytes after 0x55 sum to zero.
#undef NDEBUG
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Button { unsigned id; unsigned short key; };
constexpr std::array<Button, 9> buttons{{
    {0, KEY_1}, {1, KEY_2}, {2, KEY_3},
    {6, KEY_PREVIOUSSONG}, {5, KEY_PLAYPAUSE}, {7, KEY_NEXTSONG},
    {3, KEY_PROG1}, {10, KEY_VOLUMEDOWN}, {9, KEY_VOLUMEUP}
}};
using Header = std::array<uint8_t, 12>;
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
unsigned le16(const uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }
uint32_t le32(const uint8_t* p) { return le16(p) | (uint32_t(le16(p + 2)) << 16); }
int level(unsigned state) { return state == 0 ? 1 : state >= 1 && state <= 4 ? 0 : -1; }

bool nextPacket(std::vector<uint8_t>& bytes, Header& header)
{
    while (!bytes.empty()) {
        bytes.erase(bytes.begin(), std::find(bytes.begin(), bytes.end(), 0x55));
        if (bytes.size() < header.size()) return false;
        const size_t size = header.size() + le16(bytes.data() + 10);
        if (size > 4108) { bytes.erase(bytes.begin()); continue; }
        if (bytes.size() < size) return false;
        uint8_t sum = 0;
        for (size_t i = 1; i < size; ++i) sum += bytes[i];
        if (sum != 0) { bytes.erase(bytes.begin()); continue; }
        std::copy_n(bytes.begin(), header.size(), header.begin());
        bytes.erase(bytes.begin(), bytes.begin() + size);
        return true;
    }
    return false;
}
Header acknowledgement(uint8_t transaction)
{
    Header ack{0x55, 2}; // Notify command 0: transport acknowledgement, status OK.
    ack[8] = transaction;
    ack[9] = uint8_t(-(2 + transaction));
    return ack;
}
bool acknowledge(int fd, uint8_t transaction)
{
    const Header ack = acknowledgement(transaction);
    size_t sent = 0;
    while (sent < ack.size() && running) {
        const ssize_t count = write(fd, ack.data() + sent, ack.size() - sent);
        if (count > 0) sent += size_t(count);
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && errno == EAGAIN) {
            pollfd writable{fd, POLLOUT, 0};
            if (poll(&writable, 1, 500) <= 0) return false;
        } else return false;
    }
    return sent == ack.size();
}
bool emit(int fd, unsigned short code, int value)
{
    input_event events[2]{};
    events[0].type = EV_KEY; events[0].code = code; events[0].value = value;
    events[1].type = EV_SYN; events[1].code = SYN_REPORT;
    return write(fd, events, sizeof(events)) == static_cast<ssize_t>(sizeof(events));
}
bool handlePacket(int uart, int input, const Header& header, std::array<bool, buttons.size()>& pressed)
{
    const unsigned command = le16(header.data() + 2);
    if (command != 0 && !acknowledge(uart, header[8])) return false;
    if (header[1] != 2 || command != 0x40 || le16(header.data() + 10) != 0) return true;
    const uint32_t value = le32(header.data() + 4);
    const int state = level(value & 0xffff);
    for (size_t i = 0; state >= 0 && i < buttons.size(); ++i) {
        if (buttons[i].id != (value >> 16) || pressed[i] == (state != 0)) continue;
        if (!emit(input, buttons[i].key, state)) return false;
        pressed[i] = state != 0;
        std::fprintf(stderr, "button=%u key=%u pressed=%d\n", buttons[i].id, buttons[i].key, state);
    }
    return true;
}
Header request(uint8_t type, unsigned command, uint32_t value, uint8_t transaction)
{
    Header packet{0x55, type, uint8_t(command), uint8_t(command >> 8),
                  uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24), transaction};
    for (size_t i = 1; i < packet.size(); ++i) if (i != 9) packet[9] -= packet[i];
    return packet;
}
// GET replies are notifications with the MCU's own transaction number. Its ACK
// for our GET precedes that notification; acknowledge notifications as usual.
bool exchange(int uart, int input, std::array<bool, buttons.size()>& pressed,
              std::vector<uint8_t>& bytes, uint8_t type, unsigned command, uint32_t& value)
{
    static uint8_t transaction = 150;
    // Back-to-back GETs exhaust this MCU's packet pool (ACK status 2).
    // Let the preceding notification ACK drain before requesting more data.
    tcdrain(uart);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const Header packet = request(type, command, value, ++transaction);
    if (write(uart, packet.data(), packet.size()) != static_cast<ssize_t>(packet.size())) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool ack = false;
    while (running && std::chrono::steady_clock::now() < deadline) {
        pollfd event{uart, POLLIN, 0};
        if (poll(&event, 1, 100) < 0 && errno != EINTR) return false;
        std::array<uint8_t, 256> chunk{};
        const ssize_t count = read(uart, chunk.data(), chunk.size());
        if (count > 0) bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
        else if (count < 0 && errno != EAGAIN && errno != EINTR) return false;
        Header header{};
        while (nextPacket(bytes, header)) {
            if (!handlePacket(uart, input, header, pressed)) return false;
            const unsigned id = le16(header.data() + 2);
            if (header[1] == 2 && id == 0 && header[8] == transaction) {
                if (le32(header.data() + 4) != 0) {
                    std::fprintf(stderr, "MCU command 0x%04x type %u rejected: %u\n", command, type, le32(header.data() + 4));
                    return false;
                }
                ack = true;
                if (type == 0) return true;
            }
            if (ack && header[1] == 2 && id == command && le16(header.data() + 10) == 0) {
                value = le32(header.data() + 4);
                return true;
            }
        }
    }
    std::fprintf(stderr, "MCU command 0x%04x type %u timed out\n", command, type);
    return false;
}
constexpr const char* audioSocket = "/run/galdrun-connector3-mcu.sock";
bool setAudio(int uart, int input, std::array<bool, buttons.size()>& pressed,
              std::vector<uint8_t>& bytes, unsigned command, uint32_t requested)
{
    uint32_t value = requested;
    if (!exchange(uart, input, pressed, bytes, 0, command, value)) return false;
    // Mute ramps asynchronously: acceptance is not yet the applied state.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (!exchange(uart, input, pressed, bytes, 1, command, value)) return false;
        if (value == requested) return true;
    } while (running && std::chrono::steady_clock::now() < deadline);
    return false;
}
int createAudioSocket()
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, audioSocket);
    unlink(audioSocket);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        chmod(audioSocket, 0600) != 0 || listen(fd, 1) != 0) {
        close(fd); unlink(audioSocket); return -1;
    }
    return fd;
}
void handleAudio(int listener, int uart, int input, std::array<bool, buttons.size()>& pressed,
                 std::vector<uint8_t>& bytes)
{
    const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) return;
    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::string line;
    char c;
    while (line.size() < 64 && recv(fd, &c, 1, 0) == 1) {
        if (c == '\n') break;
        line += c;
    }
    std::istringstream requestLine(line);
    std::string operation, extra;
    unsigned requested = 0;
    requestLine >> operation;
    bool ok = operation == "get" && !(requestLine >> extra);
    if (operation == "mute" || operation == "volume") {
        ok = bool(requestLine >> requested) && !(requestLine >> extra) &&
             requested <= (operation == "mute" ? 1u : 100u);
        if (ok) ok = setAudio(uart, input, pressed, bytes, operation == "mute" ? 0x10 : 0x11, requested);
    }
    uint32_t mute = 0, volume = 0;
    if (ok) ok = exchange(uart, input, pressed, bytes, 1, 0x10, mute) &&
                 exchange(uart, input, pressed, bytes, 1, 0x11, volume) && mute <= 1 && volume <= 100;
    if (ok && operation != "get") ok = requested == (operation == "mute" ? mute : volume);
    if (operation != "get") std::fprintf(stderr, "MCU audio %s %u: ok=%d mute=%u volume=%u\n",
                                         operation.c_str(), requested, ok, mute, volume);
    const std::string reply = ok ? "ok " + std::to_string(mute) + " " + std::to_string(volume) + "\n" :
                                  "error MCU audio request failed\n";
    send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    close(fd);
}
int createInput()
{
    const int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0;
    for (const auto& button : buttons) ok = ok && ioctl(fd, UI_SET_KEYBIT, button.key) == 0;
    uinput_user_dev device{};
    std::strcpy(device.name, "Connector 3 touch buttons");
    device.id.bustype = BUS_HOST; device.id.product = 19; device.id.version = 2;
    if (!ok || write(fd, &device, sizeof(device)) != static_cast<ssize_t>(sizeof(device)) ||
        ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    return fd;
}
void selfTest()
{
    Header frame{0x55, 2, 0x40, 0, 0, 0, 3, 0, 100, 0x57, 0, 0}, decoded{};
    std::vector<uint8_t> buffer{0x99, 0x12};
    buffer.insert(buffer.end(), frame.begin(), frame.begin() + 7);
    assert(!nextPacket(buffer, decoded));
    buffer.insert(buffer.end(), frame.begin() + 7, frame.end());
    assert(nextPacket(buffer, decoded) && decoded == frame && buffer.empty());
    auto broken = frame; broken[9] ^= 1;
    buffer.insert(buffer.end(), broken.begin(), broken.end());
    buffer.insert(buffer.end(), frame.begin(), frame.end());
    assert(nextPacket(buffer, decoded) && decoded == frame && buffer.empty());
    Header oversized{0x55}; oversized[11] = 0xff;
    buffer.insert(buffer.end(), oversized.begin(), oversized.end());
    buffer.insert(buffer.end(), frame.begin(), frame.end());
    assert(nextPacket(buffer, decoded) && decoded == frame);
    for (unsigned transaction : {0U, 59U, 255U}) {
        auto ack = acknowledgement(uint8_t(transaction));
        buffer.assign(ack.begin(), ack.end());
        assert(nextPacket(buffer, decoded) && decoded == ack && le16(ack.data() + 2) == 0);
    }
    assert(le32(frame.data() + 4) == 0x00030000 && buttons[6].id == 3);
    assert(level(0) == 1 && level(1) == 0 && level(4) == 0 && level(0x10) == -1);
    for (auto packet : {request(0, 0x50, 0, 151), request(1, 0x10, 0, 152), request(0, 0x11, 67, 153)}) {
        buffer.assign(packet.begin(), packet.end());
        assert(nextPacket(buffer, decoded) && decoded == packet);
    }
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    std::thread emulator([&] {
        uint32_t power = 2, mute = 1, pendingMute = 1;
        bool mutePending = false;
        Header packet{};
        while (recv(pair[1], packet.data(), packet.size(), MSG_WAITALL) == static_cast<ssize_t>(packet.size())) {
            const unsigned command = le16(packet.data() + 2);
            if (command == 0) continue;
            const bool known = command == 0x50 || command == 0x10 || command == 0x11;
            const auto ack = request(2, 0, known ? 0 : 3, packet[8]);
            assert(write(pair[1], ack.data(), ack.size()) == static_cast<ssize_t>(ack.size()));
            if (!known) continue;
            if (packet[1] == 0) {
                if (command == 0x50) power = le32(packet.data() + 4);
                if (command == 0x10) { pendingMute = le32(packet.data() + 4); mutePending = true; }
            } else {
                const auto reply = request(2, command, command == 0x50 ? power : command == 0x10 ? mute : 67, 99);
                assert(write(pair[1], reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()));
                if (command == 0x10 && mutePending) { mute = pendingMute; mutePending = false; }
            }
        }
    });
    std::array<bool, buttons.size()> pressed{};
    uint32_t value = 0;
    assert(setAudio(pair[0], -1, pressed, buffer, 0x50, 0));
    assert(exchange(pair[0], -1, pressed, buffer, 1, 0x50, value) && value == 0);
    assert(setAudio(pair[0], -1, pressed, buffer, 0x10, 0));
    assert(exchange(pair[0], -1, pressed, buffer, 1, 0x10, value) && value == 0);
    assert(exchange(pair[0], -1, pressed, buffer, 1, 0x11, value) && value == 67);
    assert(!exchange(pair[0], -1, pressed, buffer, 1, 0x80, value));
    shutdown(pair[0], SHUT_RDWR);
    emulator.join();
    close(pair[0]); close(pair[1]);
    std::puts("PASS: MCU framing, checksum, resync, buttons, power/mute readback and rejected commands");
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) { selfTest(); return 0; }
    if (argc != 2) { std::fprintf(stderr, "Usage: %s UART\n", argv[0]); return 2; }
    const int uart = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (uart < 0) { std::perror("MCU UART"); return 1; }
    termios tty{};
    if (ioctl(uart, TIOCEXCL) < 0 || tcgetattr(uart, &tty) != 0) {
        std::perror("Claim MCU UART"); close(uart); return 1;
    }
    cfmakeraw(&tty); cfsetispeed(&tty, B115200); cfsetospeed(&tty, B115200);
    tty.c_cflag |= CLOCAL | CREAD; tty.c_cflag &= ~CRTSCTS;
    if (tcsetattr(uart, TCSANOW, &tty) != 0) { std::perror("Configure MCU UART"); close(uart); return 1; }
    const int input = createInput();
    if (input < 0) { std::perror("Create touch input"); close(uart); return 1; }
    std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
    std::array<bool, buttons.size()> pressed{};
    std::vector<uint8_t> bytes;
    bool ok = true;
    // A warm boot can inherit Standby and Mute from the vendor. ALSA running
    // alone does not power the carrier DAC: both commands and readback matter.
    const bool awake = setAudio(uart, input, pressed, bytes, 0x50, 0);
    const bool unmuted = awake && setAudio(uart, input, pressed, bytes, 0x10, 0);
    std::fprintf(stderr, "MCU audio power On: %d; unmuted: %d\n", awake, unmuted);
    const int listener = createAudioSocket();
    if (listener < 0) std::perror("MCU audio socket");
    std::fprintf(stderr, "Connector 3 MCU touch device ready: 9 buttons\n");
    while (running && ok) {
        pollfd events[2]{{uart, POLLIN, 0}, {listener, POLLIN, 0}};
        const int ready = poll(events, 2, 500);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (events[0].revents & (POLLERR | POLLHUP | POLLNVAL))) { ok = false; break; }
        if (ready == 0) continue;
        if (events[1].revents & POLLIN) handleAudio(listener, uart, input, pressed, bytes);
        if (events[0].revents & POLLIN) {
            std::array<uint8_t, 256> chunk{};
            const ssize_t count = read(uart, chunk.data(), chunk.size());
            if (count < 0 && errno != EAGAIN && errno != EINTR) { ok = false; break; }
            if (count > 0) bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
        }
        Header header{};
        while (ok && nextPacket(bytes, header)) {
            ok = handlePacket(uart, input, header, pressed);
        }
    }
    for (size_t i = 0; i < buttons.size(); ++i) if (pressed[i]) emit(input, buttons[i].key, 0);
    if (listener >= 0) { close(listener); unlink(audioSocket); }
    ioctl(input, UI_DEV_DESTROY); close(input); close(uart);
    return ok ? 0 : 1;
}

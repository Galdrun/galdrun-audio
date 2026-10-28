// Connector 3 MCU notifications -> evdev, without the vendor daemon.
// Serial protocol: 0x55, type, LE16 command, LE32 value, transaction,
// checksum, LE16 payload length, payload. Bytes after 0x55 sum to zero.
#undef NDEBUG
#include <linux/uinput.h>
#include <sys/ioctl.h>
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
    std::puts("PASS: MCU framing, checksum, resync, acknowledgement and button states");
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
    std::fprintf(stderr, "Connector 3 MCU touch device ready: 9 buttons\n");
    while (running && ok) {
        pollfd event{uart, POLLIN, 0};
        const int ready = poll(&event, 1, 500);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL))) { ok = false; break; }
        if (ready == 0) continue;
        std::array<uint8_t, 256> chunk{};
        const ssize_t count = read(uart, chunk.data(), chunk.size());
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count <= 0) { ok = false; break; }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
        Header header{};
        while (ok && nextPacket(bytes, header)) {
            const unsigned command = le16(header.data() + 2);
            if (command != 0 && !acknowledge(uart, header[8])) { ok = false; break; }
            if (header[1] != 2 || command != 0x40 || le16(header.data() + 10) != 0) continue;
            const uint32_t value = le32(header.data() + 4);
            const int state = level(value & 0xffff);
            for (size_t i = 0; state >= 0 && i < buttons.size(); ++i) {
                if (buttons[i].id != (value >> 16) || pressed[i] == (state != 0)) continue;
                if (!emit(input, buttons[i].key, state)) { ok = false; break; }
                pressed[i] = state != 0;
                std::fprintf(stderr, "button=%u key=%u pressed=%d\n", buttons[i].id, buttons[i].key, state);
            }
        }
    }
    for (size_t i = 0; i < buttons.size(); ++i) if (pressed[i]) emit(input, buttons[i].key, 0);
    ioctl(input, UI_DEV_DESTROY); close(input); close(uart);
    return ok ? 0 : 1;
}

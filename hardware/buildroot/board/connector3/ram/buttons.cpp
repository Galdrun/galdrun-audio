// Bridge the vendor MCU daemon's documented D-Bus signals to Linux evdev.
// Only used with the vendor kernel/hardwared; it is not a standalone MCU driver.
#undef NDEBUG
#include <dbus/dbus.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* service = "com.raumfeld.hardwared";
constexpr const char* path = "/com/raumfeld/hardwared";
struct Button { unsigned id; unsigned short key; };
constexpr std::array<Button, 9> buttons{{
    {0, KEY_1}, {1, KEY_2}, {2, KEY_3},
    {6, KEY_PREVIOUSSONG}, {5, KEY_PLAYPAUSE}, {7, KEY_NEXTSONG},
    {13, KEY_PROG1}, {11, KEY_VOLUMEDOWN}, {10, KEY_VOLUMEUP}
}};
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
int level(unsigned state)
{
    if (state == 0 || state == 7) return 1; // pressed / double-press
    if (state >= 1 && state <= 4) return 0; // short/medium/long/double release
    return -1; // hardware repeats and held notifications are not new edges
}
bool emit(int fd, unsigned short code, int value)
{
    input_event events[2]{};
    events[0].type = EV_KEY;
    events[0].code = code;
    events[0].value = value;
    events[1].type = EV_SYN;
    events[1].code = SYN_REPORT;
    return write(fd, events, sizeof(events)) == static_cast<ssize_t>(sizeof(events));
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
        assert(buttons.size() == 9 && buttons[6].id == 13);
        assert(level(0) == 1 && level(7) == 1);
        for (unsigned state = 1; state <= 4; ++state) assert(level(state) == 0);
        for (unsigned state : {5U, 6U, 8U, 999U}) assert(level(state) == -1);
        std::puts("PASS: MCU button press/release mapping; repeats ignored");
        return 0;
    }
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* bus = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (!bus || !dbus_bus_name_has_owner(bus, service, &error)) {
        std::fprintf(stderr, "Vendor hardwared is unavailable; no touch device created.\n");
        dbus_error_free(&error);
        return 1;
    }
    dbus_bus_add_match(bus, "type='signal',sender='com.raumfeld.hardwared',"
                      "path='/com/raumfeld/hardwared',interface='com.raumfeld.hardwared',"
                      "member='ButtonStateChanged'", &error);
    if (!dbus_error_is_set(&error))
        dbus_bus_add_match(bus, "type='signal',sender='org.freedesktop.DBus',"
                          "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                          "arg0='com.raumfeld.hardwared'", &error);
    if (dbus_error_is_set(&error)) {
        std::fprintf(stderr, "Cannot subscribe to MCU events: %s\n", error.message);
        dbus_error_free(&error);
        return 1;
    }
    const int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { std::perror("/dev/uinput"); return 1; }
    bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0;
    for (const auto& button : buttons) ok = ok && ioctl(fd, UI_SET_KEYBIT, button.key) == 0;
    uinput_user_dev device{};
    std::strcpy(device.name, "Connector 3 touch buttons");
    device.id.bustype = BUS_HOST;
    device.id.product = 19;
    device.id.version = 1;
    if (!ok || write(fd, &device, sizeof(device)) != static_cast<ssize_t>(sizeof(device)) ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        std::perror("Create touch input device"); close(fd); return 1;
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    std::array<bool, buttons.size()> pressed{};
    std::fprintf(stderr, "Connector 3 touch bridge ready: 9 buttons\n");
    while (running && dbus_connection_read_write(bus, 500)) {
        DBusMessage* message;
        while ((message = dbus_connection_pop_message(bus)) != nullptr) {
            if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
                running = 0; // destroy the device rather than leave held keys behind
            } else if (dbus_message_is_signal(message, service, "ButtonStateChanged") &&
                       dbus_message_has_path(message, path)) {
                dbus_uint32_t id, state;
                if (dbus_message_get_args(message, nullptr, DBUS_TYPE_UINT32, &id,
                                          DBUS_TYPE_UINT32, &state, DBUS_TYPE_INVALID)) {
                    const int value = level(state);
                    for (size_t i = 0; value >= 0 && i < buttons.size(); ++i) {
                        if (buttons[i].id != id || pressed[i] == (value != 0)) continue;
                        if (!emit(fd, buttons[i].key, value)) { running = 0; break; }
                        pressed[i] = value != 0;
                    }
                }
            }
            dbus_message_unref(message);
        }
    }
    for (size_t i = 0; i < buttons.size(); ++i)
        if (pressed[i]) emit(fd, buttons[i].key, 0);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    dbus_connection_unref(bus);
    return 0;
}

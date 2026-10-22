#include "input/linux/LinuxInputBackend.h"

#include "util/FileUtil.h"
#include "util/Logger.h"
#include "util/Time.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace multiroom {

namespace {

// How often the thread looks at the stop flag when nothing moves.
const int kPollTimeoutMs = 100;

// How often a missing input device is looked for again.
const uint64_t kDeviceRetryNs = 2000000000ULL;

// A device with more keys than this is a keyboard or a remote, which is only
// read when client.json names it.
const size_t kMaxDiscoveredKeys = 16;

const size_t kLongBits = sizeof(unsigned long) * CHAR_BIT;

std::string systemError(const std::string& what)
{
    return what + ": " + std::strerror(errno);
}

bool testBit(const unsigned long* bits, size_t bit)
{
    return ((bits[bit / kLongBits] >> (bit % kLongBits)) & 1UL) != 0;
}

// /dev/input/eventN in N's order, so discovery finds devices in the order
// the kernel registered them.
std::vector<std::string> eventDevicePaths()
{
    std::vector<std::pair<long, std::string>> found;
    if (DIR* dir = ::opendir("/dev/input"))
    {
        while (const dirent* entry = ::readdir(dir))
        {
            if (std::strncmp(entry->d_name, "event", 5) == 0)
                found.emplace_back(std::strtol(entry->d_name + 5, nullptr, 10),
                                   std::string("/dev/input/") + entry->d_name);
        }
        ::closedir(dir);
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> paths;
    for (const auto& entry : found)
        paths.push_back(entry.second);
    return paths;
}

int openEventDevice(const std::string& path)
{
    return ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

std::string deviceName(int fd)
{
    char name[256] = {};
    if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        return std::string();
    return name;
}

// `spec` is a path (/dev/input/by-path/... is stable across boots, eventN is
// not) or the name the device reports. -1 when there is none; `why` then says
// whether a device could not be opened, which is usually a permission.
int findDevice(const std::string& spec, std::string& why)
{
    if (spec[0] == '/')
    {
        const int fd = openEventDevice(spec);
        if (fd < 0)
            why = systemError(spec);
        return fd;
    }
    for (const std::string& path : eventDevicePaths())
    {
        const int fd = openEventDevice(path);
        if (fd < 0)
        {
            if (errno == EACCES)
                why = systemError(path) + " (is this user in the input group?)";
            continue;
        }
        if (deviceName(fd) == spec)
            return fd;
        ::close(fd);
    }
    return -1;
}

uint16_t findInput(const std::vector<std::pair<uint16_t, uint16_t>>& map, uint16_t code,
                   bool& found)
{
    for (const auto& entry : map)
    {
        if (entry.first == code)
        {
            found = true;
            return entry.second;
        }
    }
    found = false;
    return 0;
}

#if defined(GPIO_V2_GET_LINE_IOCTL)

// prev << 2 | cur, for the Gray sequence 00 01 11 10: a step one way is +1,
// the other way -1, and a jump over a state (a missed edge) counts nothing.
const int8_t kQuadratureStep[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};

uint64_t biasFlag(InputPull pull)
{
    switch (pull)
    {
    case InputPull::Up:
        return GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    case InputPull::Down:
        return GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    case InputPull::None:
        return GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    }
    return 0;
}

#endif

} // namespace

LinuxInputBackend::~LinuxInputBackend()
{
    stop();
}

bool LinuxInputBackend::start(const std::vector<InputDefinition>& inputs,
                              const std::string& gpioChip, InputEventQueue& queue,
                              std::string& error)
{
    stop();
    if (inputs.empty())
        return true;

    if (!claimLines(inputs, gpioChip, queue, error))
        return false;

    // One open device per `device` however many of its keys are inputs.
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        const InputDefinition& input = inputs[i];
        if (!input.isEvdev())
            continue;
        auto device = std::find_if(devices_.begin(), devices_.end(),
                                   [&](const Device& d) { return d.spec == input.device; });
        if (device == devices_.end())
        {
            devices_.emplace_back();
            device = devices_.end() - 1;
            device->spec = input.device;
        }
        auto& map = input.control.kind == matp::InputKind::Button ? device->keys : device->axes;
        map.emplace_back(input.code, static_cast<uint16_t>(i));
    }

    // A missing device is not an error: it is looked for again, like a USB
    // remote plugged in later.
    queue_ = &queue;
    const uint64_t nowNs = monotonicNowNs();
    for (Device& device : devices_)
        openDevice(device, nowNs);

    stopRequested_.store(false);
    thread_ = std::thread(&LinuxInputBackend::run, this);
    MULTIROOM_INFO("reading %zu GPIO input(s)%s%s and %zu input device(s)", lines_.size(),
             lines_.empty() ? "" : " on ", lines_.empty() ? "" : gpioChip.c_str(),
             devices_.size());
    return true;
}

bool LinuxInputBackend::claimLines(const std::vector<InputDefinition>& inputs,
                                   const std::string& gpioChip, InputEventQueue& queue,
                                   std::string& error)
{
    const bool any = std::any_of(inputs.begin(), inputs.end(),
                                 [](const InputDefinition& input) { return !input.isEvdev(); });
    if (!any)
        return true;

#if defined(GPIO_V2_GET_LINE_IOCTL)
    const int chip = ::open(gpioChip.c_str(), O_RDONLY | O_CLOEXEC);
    if (chip < 0)
    {
        error = systemError("cannot open " + gpioChip);
        return false;
    }

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        const InputDefinition& input = inputs[i];
        if (input.isEvdev())
            continue;
        Line line;
        line.index = static_cast<uint16_t>(i);
        line.encoder = input.control.kind == matp::InputKind::Encoder;
        line.activeLow = input.activeLow;
        line.stepsPerDetent = input.stepsPerDetent;

        gpio_v2_line_request request;
        std::memset(&request, 0, sizeof(request));
        std::strncpy(request.consumer, MULTIROOM_BRAND_SLUG, sizeof(request.consumer) - 1);
        request.offsets[0] = static_cast<uint32_t>(line.encoder ? input.pinA : input.pin);
        request.offsets[1] = static_cast<uint32_t>(input.pinB);
        request.num_lines = line.encoder ? 2 : 1;
        request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                               GPIO_V2_LINE_FLAG_EDGE_FALLING | biasFlag(input.pull);
        line.pinA = request.offsets[0];

        if (::ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request) < 0)
        {
            error = systemError("cannot claim the line(s) of input " + input.control.id + " on " +
                                gpioChip);
            ::close(chip);
            closeAll();
            return false;
        }
        line.fd = request.fd;

        // The level now, so a button held at start is known to be held and
        // the encoder's first edge has a state to step from.
        gpio_v2_line_values values;
        std::memset(&values, 0, sizeof(values));
        values.mask = line.encoder ? 3 : 1;
        if (::ioctl(line.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
        {
            error = systemError("cannot read input " + input.control.id);
            ::close(line.fd);
            ::close(chip);
            closeAll();
            return false;
        }
        if (line.encoder)
        {
            line.quadrature = static_cast<uint8_t>(((values.bits & 1) << 1) | ((values.bits >> 1) & 1));
        }
        else
        {
            RawInputEvent event;
            event.type = RawInputEvent::Type::Level;
            event.index = line.index;
            event.pressed = ((values.bits & 1) == 0) == line.activeLow;
            event.atNs = monotonicNowNs();
            if (event.pressed)
                queue.push(event);
        }
        lines_.push_back(line);
    }
    ::close(chip); // the line requests stay valid without it
    return true;
#else
    (void)gpioChip;
    (void)queue;
    error = "this client was built against kernel headers without the GPIO v2 interface "
            "(Linux 5.10); GPIO inputs cannot be read";
    return false;
#endif
}

void LinuxInputBackend::openDevice(Device& device, uint64_t nowNs)
{
    std::string why;
    const int fd = findDevice(device.spec, why);
    if (fd < 0)
    {
        device.retryAtNs = nowNs + kDeviceRetryNs;
        if (!device.missingLogged)
        {
            MULTIROOM_WARN("input device %s not found%s%s; looking again every 2 s",
                     device.spec.c_str(), why.empty() ? "" : ": ", why.c_str());
            device.missingLogged = true;
        }
        return;
    }

    // Without it the events carry wall-clock time, which is not what the
    // controller measures with; they are then stamped on arrival instead.
    int clock = CLOCK_MONOTONIC;
    device.monotonic = ::ioctl(fd, EVIOCSCLOCKID, &clock) == 0;
    device.fd = fd;
    device.missingLogged = false;
    device.dropped = false;

    // Mark startup levels so a held wake button cannot trigger sleep again.
    unsigned long state[KEY_MAX / (sizeof(unsigned long) * CHAR_BIT) + 1] = {};
    if (::ioctl(fd, EVIOCGKEY(sizeof(state)), state) >= 0)
    {
        for (const auto& key : device.keys)
        {
            if (!testBit(state, key.first))
                continue;
            RawInputEvent event;
            event.type = RawInputEvent::Type::Level;
            event.index = key.second;
            event.pressed = true;
            event.initialState = true;
            event.atNs = nowNs;
            queue_->push(event);
        }
    }
    MULTIROOM_INFO("reading input device %s", device.spec.c_str());
}

// Every key's level as the device has it now, marked as a resynchronization:
// the controller takes it as state, not as an edge after an unbroken hold.
void LinuxInputBackend::pushKeyLevels(Device& device, uint64_t nowNs)
{
    unsigned long state[KEY_MAX / (sizeof(unsigned long) * CHAR_BIT) + 1] = {};
    if (::ioctl(device.fd, EVIOCGKEY(sizeof(state)), state) < 0)
    {
        MULTIROOM_WARN("%s", systemError("reading the keys of " + device.spec).c_str());
        return;
    }
    for (const auto& key : device.keys)
    {
        RawInputEvent event;
        event.type = RawInputEvent::Type::Level;
        event.index = key.second;
        event.pressed = testBit(state, key.first);
        event.resync = true;
        event.atNs = nowNs;
        queue_->push(event);
    }
}

void LinuxInputBackend::closeDevice(Device& device, uint64_t nowNs)
{
    ::close(device.fd);
    device.fd = -1;
    device.missingLogged = true; // already said it is gone
    device.retryAtNs = nowNs + kDeviceRetryNs;

    // A key held while the device went away is not held forever. Marked as a
    // resynchronization, so the lost hold ends without completing a gesture.
    for (const auto& key : device.keys)
    {
        RawInputEvent event;
        event.type = RawInputEvent::Type::Level;
        event.index = key.second;
        event.pressed = false;
        event.resync = true;
        event.atNs = nowNs;
        queue_->push(event);
    }
}

void LinuxInputBackend::stop()
{
    stopRequested_.store(true);
    if (thread_.joinable())
        thread_.join();
    closeAll();
}

void LinuxInputBackend::closeAll()
{
    for (Line& line : lines_)
    {
        if (line.fd >= 0)
            ::close(line.fd);
    }
    lines_.clear();
    for (Device& device : devices_)
    {
        if (device.fd >= 0)
            ::close(device.fd);
    }
    devices_.clear();
}

void LinuxInputBackend::run()
{
    std::vector<pollfd> fds;
    std::vector<Device*> polledDevices;
    while (!stopRequested_.load())
    {
        const uint64_t nowNs = monotonicNowNs();
        fds.clear();
        polledDevices.clear();
        for (const Line& line : lines_)
            fds.push_back(pollfd{line.fd, POLLIN, 0});
        for (Device& device : devices_)
        {
            if (device.fd < 0 && nowNs >= device.retryAtNs)
                openDevice(device, nowNs);
            if (device.fd < 0)
                continue;
            fds.push_back(pollfd{device.fd, POLLIN, 0});
            polledDevices.push_back(&device);
        }

        const int ready = ::poll(fds.data(), fds.size(), kPollTimeoutMs);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            MULTIROOM_ERROR("%s; inputs are no longer read", systemError("polling the inputs").c_str());
            return;
        }
        for (size_t i = 0; i < lines_.size(); ++i)
        {
            if ((fds[i].revents & POLLIN) != 0)
                readLine(lines_[i]);
        }
        for (size_t i = 0; i < polledDevices.size(); ++i)
        {
            if ((fds[lines_.size() + i].revents & (POLLIN | POLLERR | POLLHUP)) != 0)
                readDevice(*polledDevices[i]);
        }
    }
}

void LinuxInputBackend::readDevice(Device& device)
{
    input_event events[64];
    const ssize_t bytes = ::read(device.fd, events, sizeof(events));
    if (bytes < 0)
    {
        if (errno == EAGAIN || errno == EINTR)
            return;
        MULTIROOM_WARN("%s; looking for it again", systemError("input device " + device.spec).c_str());
        closeDevice(device, monotonicNowNs());
        return;
    }

    const size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i)
    {
        const input_event& event = events[i];

        // The kernel's buffer overflowed: what follows up to the next report
        // is incomplete, and the keys' levels are read back instead
        // (Documentation/input/event-codes.rst). Detents lost are lost.
        if (event.type == EV_SYN && event.code == SYN_DROPPED)
        {
            device.dropped = true;
            continue;
        }
        if (device.dropped)
        {
            if (event.type == EV_SYN && event.code == SYN_REPORT)
            {
                device.dropped = false;
                pushKeyLevels(device, monotonicNowNs());
            }
            continue;
        }

        // Auto-repeat (2) is not a press: a hold is the controller's to time.
        const bool key = event.type == EV_KEY && event.value != 2;
        if (!key && event.type != EV_REL)
            continue;
        bool found = false;
        const uint16_t index = findInput(key ? device.keys : device.axes, event.code, found);
        if (!found)
            continue;

        RawInputEvent raw;
        raw.index = index;
        raw.atNs = device.monotonic
                       ? static_cast<uint64_t>(event.input_event_sec) * 1000000000ULL +
                             static_cast<uint64_t>(event.input_event_usec) * 1000ULL
                       : monotonicNowNs();
        if (key)
        {
            raw.type = RawInputEvent::Type::Level;
            raw.pressed = event.value != 0;
        }
        else
        {
            // The rotary-encoder driver sends one count per detent at its
            // default steps-per-period, positive turning clockwise.
            raw.type = RawInputEvent::Type::Detents;
            raw.detents = event.value;
        }
        queue_->push(raw);
    }
}

void LinuxInputBackend::readLine(Line& line)
{
#if defined(GPIO_V2_GET_LINE_IOCTL)
    gpio_v2_line_event events[16];
    const ssize_t bytes = ::read(line.fd, events, sizeof(events));
    if (bytes <= 0)
        return;

    // One post per run of one direction: the controller sums them anyway, but
    // keeps a reversal, which a sum over the whole read would cancel out.
    int32_t detents = 0;
    uint64_t lastNs = 0;
    const auto post = [&]() {
        if (detents == 0)
            return;
        RawInputEvent raw;
        raw.type = RawInputEvent::Type::Detents;
        raw.index = line.index;
        raw.detents = detents;
        raw.atNs = lastNs;
        queue_->push(raw);
        detents = 0;
    };
    const size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i)
    {
        const gpio_v2_line_event& event = events[i];
        const bool high = event.id == GPIO_V2_LINE_EVENT_RISING_EDGE;

        if (!line.encoder)
        {
            RawInputEvent raw;
            raw.type = RawInputEvent::Type::Level;
            raw.index = line.index;
            raw.pressed = high != line.activeLow;
            raw.atNs = event.timestamp_ns;
            // The kernel's buffer overflowed and edges between the last one
            // seen and this were lost: two short presses could otherwise read
            // as one long hold. The level after this edge is still known, and
            // as a resynchronization it ends any gesture in progress.
            raw.resync = event.line_seqno != line.seqno + 1;
            line.seqno = event.line_seqno;
            queue_->push(raw);
            continue;
        }

        const uint8_t previous = line.quadrature;
        const uint8_t bit = event.offset == line.pinA ? 2 : 1;
        line.quadrature = static_cast<uint8_t>(high ? (previous | bit) : (previous & ~bit));
        line.steps += kQuadratureStep[(previous << 2) | line.quadrature];
        const int32_t perDetent = static_cast<int32_t>(line.stepsPerDetent);
        if (line.steps >= perDetent || line.steps <= -perDetent)
        {
            const int32_t turned = line.steps / perDetent;
            if ((turned > 0) != (detents > 0))
                post();
            detents += turned;
            lastNs = event.timestamp_ns;
            line.steps %= perDetent;
        }
    }
    post();
#else
    (void)line; // never claimed without the interface
#endif
}

void discoverEvdevInputs(std::vector<InputDefinition>& inputs)
{
    std::string model, error;
    const bool connector2 = fileutil::readTextFile("/sys/firmware/devicetree/base/model", model, error) &&
                            model.find("Raumfeld Connector 2") != std::string::npos;
    // A device client.json already names, by name or by any path to it, is
    // left as it was declared, keys it leaves out included.
    std::vector<std::string> claimed;
    for (const InputDefinition& input : inputs)
    {
        if (!input.isEvdev())
            continue;
        claimed.push_back(input.device);
        char resolved[PATH_MAX];
        if (input.device[0] == '/' && ::realpath(input.device.c_str(), resolved) != nullptr)
            claimed.push_back(resolved);
    }
    auto isClaimed = [&](const std::string& what) {
        return std::find(claimed.begin(), claimed.end(), what) != claimed.end();
    };
    auto idTaken = [&](const std::string& id) {
        return std::any_of(inputs.begin(), inputs.end(),
                           [&](const InputDefinition& input) { return input.control.id == id; });
    };

    for (const std::string& path : eventDevicePaths())
    {
        const int fd = openEventDevice(path);
        if (fd < 0)
            continue;
        input_id id;
        std::memset(&id, 0, sizeof(id));
        const std::string name = deviceName(fd);
        unsigned long keyBits[KEY_MAX / (sizeof(unsigned long) * CHAR_BIT) + 1] = {};
        unsigned long relBits[REL_MAX / (sizeof(unsigned long) * CHAR_BIT) + 1] = {};
        const bool usable = ::ioctl(fd, EVIOCGID, &id) == 0 && id.bustype == BUS_HOST &&
                            !name.empty() && !isClaimed(name) && !isClaimed(path);
        if (usable)
        {
            ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
            ::ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits);
        }
        ::close(fd);
        if (!usable || testBit(keyBits, BTN_LEFT)) // a mouse turns, but is no knob
            continue;

        std::vector<InputDefinition> found;
        auto add = [&](matp::InputKind kind, uint16_t code, const std::string& codeName) {
            InputDefinition input;
            input.control.id = name + " " + codeName;
            input.control.kind = kind;
            input.control.bindings = matp::defaultInputBindings(kind);
            input.device = name;
            input.code = code;
            if (matp::isValidInputId(input.control.id) && !idTaken(input.control.id))
                found.push_back(input);
        };
        // Connector 2's top button is an audio input. Other boards' power
        // buttons (including the Pi 5's) stay under the system's control.
        for (uint16_t code = 0; code <= KEY_MAX; ++code)
        {
            if (testBit(keyBits, code) && (code != KEY_POWER || connector2))
                add(matp::InputKind::Button, code, inputKeyName(code));
        }
        if (found.size() > kMaxDiscoveredKeys)
            continue;
        for (uint16_t code = 0; code <= REL_MAX; ++code)
        {
            if (testBit(relBits, code))
                add(matp::InputKind::Encoder, code, inputAxisName(code));
        }
        if (found.empty() || inputs.size() + found.size() > matp::kMaxInputs)
            continue;

        MULTIROOM_INFO("found input device %s (%s): %zu input(s), reported with the default bindings",
                 name.c_str(), path.c_str(), found.size());
        inputs.insert(inputs.end(), found.begin(), found.end());
        claimed.push_back(name); // a second device of the same name is not merged in
    }
}

} // namespace multiroom

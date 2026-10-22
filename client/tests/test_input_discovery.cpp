// Exercise real discovery without depending on the host's input hardware.
#undef NDEBUG
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#include "input/linux/LinuxInputBackend.h"

namespace {
const int kDevice = 100000;
const char* model = nullptr;
bool returnedDevice = false;
const unsigned long kWordBits = sizeof(unsigned long) * 8;
}

extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode)
{
    if (std::strcmp(path, "/sys/firmware/devicetree/base/model") != 0)
        return __real_fopen(path, mode);
    if (!model)
        return nullptr;
    FILE* file = std::tmpfile();
    assert(file);
    std::fwrite(model, 1, std::strlen(model) + 1, file);
    std::rewind(file);
    return file;
}

extern "C" DIR* __real_opendir(const char*);
extern "C" DIR* __wrap_opendir(const char* path)
{
    assert(std::strcmp(path, "/dev/input") == 0);
    returnedDevice = false;
    return __real_opendir("/tmp");
}

extern "C" dirent* __wrap_readdir(DIR*)
{
    if (returnedDevice)
        return nullptr;
    returnedDevice = true;
    static dirent entry = {};
    std::strcpy(entry.d_name, "event0");
    return &entry;
}

extern "C" int __wrap_open(const char* path, int, ...)
{
    assert(std::strcmp(path, "/dev/input/event0") == 0);
    return kDevice;
}

extern "C" int __wrap_close(int fd)
{
    assert(fd == kDevice);
    return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == kDevice);
    va_list args;
    va_start(args, request);
    void* value = va_arg(args, void*);
    va_end(args);
    if (request == EVIOCGID)
        static_cast<input_id*>(value)->bustype = BUS_HOST;
    else if (_IOC_NR(request) == _IOC_NR(EVIOCGNAME(0)))
        std::strcpy(static_cast<char*>(value), "gpio-keys");
    else if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_KEY, 0)))
        for (unsigned code : {KEY_F3, KEY_SETUP, KEY_POWER})
            static_cast<unsigned long*>(value)[code / kWordBits] |= 1UL << (code % kWordBits);
    return 0;
}

int main()
{
    for (const char* name : {"Custom Raumfeld Connector 2", "Raspberry Pi 5 Model B", "Other board", static_cast<const char*>(nullptr)})
    {
        model = name;
        std::vector<multiroom::InputDefinition> inputs;
        multiroom::discoverEvdevInputs(inputs);
        const bool connector2 = name && std::strstr(name, "Raumfeld Connector 2");
        assert(inputs.size() == (connector2 ? 3U : 2U));
        assert(inputs[0].code == KEY_F3);
        if (connector2)
        {
            assert(inputs[1].code == KEY_POWER);
            assert(inputs[1].control.id == "gpio-keys KEY_POWER");
        }
        assert(inputs.back().code == KEY_SETUP);
        // Explicit settings, including deliberately omitted keys, stay intact.
        inputs.resize(1);
        multiroom::discoverEvdevInputs(inputs);
        assert(inputs.size() == 1);
    }
    std::puts("PASS: Connector 2 power discovery, system power exclusion and explicit inputs");
}

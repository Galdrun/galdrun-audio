#include "input/InputBackend.h"

#if defined(ESP_PLATFORM)
#include "input/esp32/Esp32InputBackend.h"
#elif defined(__linux__)
#include "input/linux/LinuxInputBackend.h"
#endif

namespace multiroom {

IInputBackend::~IInputBackend() = default;

std::unique_ptr<IInputBackend> createPlatformInputBackend()
{
#if defined(ESP_PLATFORM)
    return std::unique_ptr<IInputBackend>(new Esp32InputBackend());
#elif defined(__linux__)
    return std::unique_ptr<IInputBackend>(new LinuxInputBackend());
#else
    return nullptr;
#endif
}

void appendDiscoveredInputs(std::vector<InputDefinition>& inputs)
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    discoverEvdevInputs(inputs);
#else
    (void)inputs;
#endif
}

const char* platformInputBackendName()
{
#if defined(ESP_PLATFORM)
    return "esp32-gpio";
#elif defined(__linux__)
    return "linux";
#else
    return "none";
#endif
}

} // namespace multiroom

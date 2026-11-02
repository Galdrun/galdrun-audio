#include "bluetooth/BluetoothReceiver.h"

#if defined(__linux__) && !defined(ESP_PLATFORM)
#include "bluetooth/linux/BluealsaReceiver.h"
#endif

namespace multiroom {

IBluetoothReceiver::~IBluetoothReceiver() = default;

std::unique_ptr<IBluetoothReceiver> createPlatformBluetoothReceiver()
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    return std::unique_ptr<IBluetoothReceiver>(new BluealsaReceiver());
#else
    return nullptr;
#endif
}

} // namespace multiroom

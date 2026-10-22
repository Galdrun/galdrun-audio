#include "util/Clock.h"

#include "util/Time.h"

namespace multiroom {

uint64_t SystemClock::monotonicNowNs() const
{
    return multiroom::monotonicNowNs();
}

} // namespace multiroom

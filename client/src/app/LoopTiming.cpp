#include "app/LoopTiming.h"

namespace multiroom {

const char* LoopTiming::stepName(Step step)
{
    switch (step)
    {
    case Sample:
        return "sample";
    case Session:
        return "session";
    case Playback:
        return "playback";
    case Beacon:
        return "beacon";
    case Inputs:
        return "inputs";
    case Commands:
        return "commands";
    case Wait:
        return "wait";
    case kSteps:
        break;
    }
    return "unknown";
}

void LoopTiming::add(Step step, uint64_t ns)
{
    Figures& figures = current_.steps[step];
    figures.totalNs += ns;
    if (ns > figures.maxNs)
        figures.maxNs = ns;
    if (step != Wait)
        passBusyNs_ += ns;
}

void LoopTiming::endPass(uint64_t nowNs)
{
    if (current_.startNs == 0)
        current_.startNs = nowNs;
    ++current_.passes;
    if (passBusyNs_ > current_.busyMaxNs)
        current_.busyMaxNs = passBusyNs_;
    passBusyNs_ = 0;

    if (nowNs - current_.startNs >= kWindowNs)
    {
        current_.lengthNs = nowNs - current_.startNs;
        last_ = current_;
        hasLast_ = true;
        current_ = Window{};
        current_.startNs = nowNs;
    }
}

json::Value LoopTiming::toJson() const
{
    json::Value out = json::Value::makeObject();
    if (!hasLast_ || last_.passes == 0)
        return out;

    out["windowMs"] = json::Value(last_.lengthNs / 1000000);
    out["passes"] = json::Value(last_.passes);
    out["busyMaxUs"] = json::Value(last_.busyMaxNs / 1000);
    json::Value steps = json::Value::makeObject();
    for (size_t i = 0; i < kSteps; ++i)
    {
        const Figures& figures = last_.steps[i];
        json::Value step = json::Value::makeObject();
        step["avgUs"] = json::Value(figures.totalNs / last_.passes / 1000);
        step["maxUs"] = json::Value(figures.maxNs / 1000);
        step["totalMs"] = json::Value(figures.totalNs / 1000000);
        steps[stepName(static_cast<Step>(i))] = std::move(step);
    }
    out["steps"] = std::move(steps);
    return out;
}

} // namespace multiroom

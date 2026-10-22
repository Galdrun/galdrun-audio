#pragma once

// Printing a ratio, for the detail string of a failure.
//
// Six decimals, because a drift trim is parts per million and 1.000030 printed
// as 1.000000 would make two different failures read identically. It lives
// beside its source rather than in include/ because only the implementation
// layer formats ratios; every implementation's configure() and setRatio()
// needs it, as does the planner that refuses a ratio before one is built, and
// three copies of eight lines is three places to get the precision wrong.

#include <string>

namespace multiroom::resampler {

std::string ratioText(double ratio);

} // namespace multiroom::resampler

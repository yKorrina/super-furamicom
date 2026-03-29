#ifndef TIMING_HPP
#define TIMING_HPP

namespace EmuTiming {
constexpr double kNtscFrameRate = 21477272.0 / 357366.0;
constexpr int kCpuCyclesPerFrame = 59562;
constexpr int kCpuCyclesPerSecond = 3579545;
constexpr int kScanlinesPerFrame = 262;
constexpr int kVisibleScanlines = 224;
}

#endif

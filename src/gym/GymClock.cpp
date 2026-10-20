#include "gym/GymClock.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
// Ten seconds: any non-zero base works, and a round one reads well in logs.
constexpr uint64_t BASE_MICROSECONDS = 10000000ULL;

// Constant-initialised, so it is valid even for timers that other translation
// units build during static initialisation. Atomic because the log writer and
// the memory-card thread read the clock while the main thread moves it.
std::atomic<int64_t> g_iSamples{0};
}  // namespace

bool GymClock::IsEnabled() {
  static const bool bEnabled = [] {
    const char* s = std::getenv("ITGMANIA_GYM");
    return s != nullptr && std::strcmp(s, "1") == 0;
  }();
  return bEnabled;
}

int64_t GymClock::Samples() { return g_iSamples.load(); }

void GymClock::AdvanceTo(int64_t samples) {
  if (samples > g_iSamples.load()) {
    g_iSamples.store(samples);
  }
}

uint64_t GymClock::MicrosecondsAt(int64_t samples) {
  // samples * 1e6 / 48000 == samples * 125 / 6, kept integral.
  return BASE_MICROSECONDS + static_cast<uint64_t>(samples) * 125ULL / 6ULL;
}

uint64_t GymClock::NowMicroseconds() { return MicrosecondsAt(Samples()); }

struct tm GymClock::CalendarTime() {
  // Thursday 2026-01-15; a run lasts minutes, so the date never rolls over.
  const int64_t iSecond = 12 * 3600 + Samples() / SAMPLE_RATE;
  struct tm tm = {};
  tm.tm_year = 2026 - 1900;
  tm.tm_mon = 0;
  tm.tm_mday = 15;
  tm.tm_wday = 4;
  tm.tm_yday = 14;
  tm.tm_hour = static_cast<int>(iSecond / 3600 % 24);
  tm.tm_min = static_cast<int>(iSecond / 60 % 60);
  tm.tm_sec = static_cast<int>(iSecond % 60);
  return tm;
}

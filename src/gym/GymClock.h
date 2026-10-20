/* GymClock - the clock of the lock-step gym mode. */

#ifndef GYM_CLOCK_H
#define GYM_CLOCK_H

#include <cstdint>
#include <ctime>

/* With ITGMANIA_GYM=1 in the environment, time stands still until the gym loop
 * moves it. RageTimer reads this clock instead of the system's, and RageTimer
 * builds its first timers during static initialisation, which is why the
 * switch is an environment variable rather than a command-line flag: the clock
 * has to be virtual before main() runs.
 *
 * The clock counts audio frames at SAMPLE_RATE. A step of a run at F frames
 * per second ends at sample floor(k * SAMPLE_RATE / F), so the time of every
 * step, and the audio mixed between two steps, are exact integers that do not
 * drift whatever F is. */
namespace GymClock {

// 48 kHz divides the usual frame rates (60, 100, 120, 200, 240) exactly.
constexpr int SAMPLE_RATE = 48000;

// True when the environment has ITGMANIA_GYM=1. Read once.
bool IsEnabled();

// Samples since the process started.
int64_t Samples();

// Move the clock forward to `samples`; it never goes back.
void AdvanceTo(int64_t samples);

// The time RageTimer reads: a fixed non-zero base (a zero RageTimer means
// "unset") plus the samples so far, in microseconds.
uint64_t NowMicroseconds();

// The same, for an arbitrary sample count.
uint64_t MicrosecondsAt(int64_t samples);

// The calendar a gym run sees: a fixed day (themes change their look by date)
// whose clock starts at noon and follows the gym clock.
struct tm CalendarTime();

}  // namespace GymClock

#endif  // GYM_CLOCK_H

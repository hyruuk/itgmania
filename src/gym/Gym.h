/* Gym - the lock-step mode a Python gymnasium env drives. */

#ifndef GYM_H
#define GYM_H

#include <cstdint>
#include <string>
#include <vector>

/* A gym process (ITGMANIA_GYM=1, see GymClock.h) plays one song in lock step:
 * each step moves the clock by one frame, runs one update, draws one frame into
 * an offscreen target and mixes exactly the audio that sounds while that frame
 * is on screen. Nothing in it depends on wall time, so a run replays bit for
 * bit from its seed and its inputs. */
namespace Gym {

/* What one gym process was launched with (its --gym-* arguments). */
struct Options {
  int fps = 60;
  int width = 854;
  int height = 480;
  uint32_t seed = 1;
  // A fresh directory for this process's Save/, Cache/ and Logs/.
  std::string datadir;
  // --gym-selftest=N: play N steps with AutoPlay and write what they produce
  // to selftest_dir, instead of serving the protocol.
  int selftest_steps = 0;
  std::string selftest_dir;
  // Write every n-th frame of the self-test as an image; 0 writes none.
  int selftest_frame_every = 0;
};

/* Read and check the --gym-* arguments, and seed every random number
 * generator. Call first thing in sm_main, after SetCommandlineArguments. A gym
 * process with missing or inconsistent arguments prints why to stderr and
 * exits: it must never run on with a default it was not given. */
void Init();

const Options& GetOptions();

/* Mount this process's writable directories (Save, Cache, Logs, Screenshots)
 * inside Options::datadir, in place of the user's. */
void MountUserFilesystems();

/* Force the preferences gym mode depends on (window, drivers, frame size,
 * timing). Call after the preferences are read from disk. */
void ApplyPreferences();

/* Where the display writes the current frame (RGB, top row first,
 * width*height*3 bytes), or nullptr when this frame is not wanted. */
uint8_t* FrameDestination(int width, int height);

/* Replaces GameLoop::RunGameLoop in a gym process. */
void Run();

}  // namespace Gym

#endif  // GYM_H

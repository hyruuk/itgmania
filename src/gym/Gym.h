/* Gym - the lock-step mode a Python gymnasium env drives. */

#ifndef GYM_H
#define GYM_H

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/* A gym process (ITGMANIA_GYM=1, see GymClock.h) plays one song in lock step:
 * each step moves the clock by one frame, presses and releases the dance
 * buttons the step asks for, runs one update, draws one frame into an
 * offscreen target and mixes exactly the audio that sounds while that frame is
 * on screen. Nothing in it depends on wall time, so a run replays bit for bit
 * from its seed and its inputs.
 *
 * --gym serves that over stdin/stdout (GymServer.cpp); --gym-selftest=N plays
 * N steps with AutoPlay and writes what they produce. */
namespace Gym {

// The dance buttons a step holds, in the order of dance-single's columns.
constexpr int NUM_BUTTONS = 4;
using Buttons = std::array<bool, NUM_BUTTONS>;

/* What one gym process was launched with (its --gym-* arguments). */
struct Options {
  int fps = 60;
  int width = 854;
  int height = 480;
  uint32_t seed = 1;
  // A fresh directory for this process's Save/, Cache/ and Logs/.
  std::string datadir;
  // An extra folder of song groups, mounted read-only under /Songs.
  std::string songs_dir;
  // The stage: a song as "Group/Folder", its chart and the modifiers it is
  // played with (the engine's own mod names, comma-separated).
  std::string song;
  std::string steps_type;
  std::string difficulty;
  std::string mods;
  // Simply Love's game mode (ITG, FA+, Casual); empty for other themes.
  std::string game_mode;
  // --gym-pref=Name=Value, applied after the gym's own preferences.
  std::vector<std::pair<std::string, std::string>> prefs;
  // --gym-selftest=N: play N steps with AutoPlay and write what they produce
  // to selftest_dir, instead of serving the protocol.
  int selftest_steps = 0;
  std::string selftest_dir;
  // Write every n-th frame of the self-test as an image; 0 writes none.
  int selftest_frame_every = 0;
};

/* A press or release the gym gave the game, at its (virtual) time. */
struct Edge {
  int column;
  bool down;
  uint64_t ts_us;
};

/* One judgment the game made, as its "Judgment" message reports it. */
struct Judgment {
  int column;
  std::string tap_note_score;   // W1..W5, Miss, HitMine, AvoidMine, ...
  std::string hold_note_score;  // Held, LetGo, Missed; empty for a tap
  int row;                      // the note's row; -1 for holds and mines
  float offset;                 // seconds, negative when early; taps only
  bool early;
  bool held_miss;
};

/* Read and check the --gym-* arguments, and seed every random number
 * generator. Call first thing in sm_main, after SetCommandlineArguments. A gym
 * process with missing or inconsistent arguments prints why to stderr and
 * exits: it must never run on with a default it was not given. */
void Init();

const Options& GetOptions();

/* Mount this process's writable directories (Save, Cache, Logs, Screenshots)
 * inside Options::datadir, in place of the user's, and Options::songs_dir. */
void MountUserFilesystems();

/* Force the preferences gym mode depends on (window, drivers, frame size,
 * timing), then apply --gym-pref. Call after the preferences are read. */
void ApplyPreferences();

/* Where the display writes the current frame (RGB, top row first,
 * width*height*3 bytes), or nullptr when this frame is not wanted. */
uint8_t* FrameDestination(int width, int height);

/* The OpenGL renderer that draws the frames (its GL_RENDERER), which the
 * display reports once: frames are exact on one machine and renderer, not
 * across them. */
void SetRenderer(const std::string& sRenderer);
const std::string& Renderer();

/* ScreenGameplay calls this where it would leave for the next screen. */
void OnStageFinished();

/* Replaces GameLoop::RunGameLoop in a gym process. */
void Run();

// The lock-step machinery the server and the self-test drive. -------------

/* Set up the stage and draw its first frame; pcm receives the audio that
 * sounds while that frame is on screen. Once per process. */
void Reset(bool bWantFrame, std::vector<int16_t>& pcm);

/* Advance one frame holding `buttons`. */
void Step(const Buttons& buttons, bool bWantFrame, std::vector<int16_t>& pcm);

// Steps taken since the reset (-1 before it).
int64_t StepsTaken();
// True once the stage has ended (cleared or failed) and stepping stops.
bool IsFinished();
// The frame the last Reset or Step drew, when it asked for one.
const std::vector<uint8_t>& LastFrame();
// What the last Step pressed and released.
const std::vector<Edge>& LastEdges();
// Judgments made since the last call.
std::vector<Judgment> TakeJudgments();
// Lua errors reported since the last call.
std::vector<std::string> TakeScriptErrors();

}  // namespace Gym

#endif  // GYM_H

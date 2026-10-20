#include "gym/Gym.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "GameConstantsAndTypes.h"
#include "GameLoop.h"
#include "GamePreferences.h"
#include "GameSoundManager.h"
#include "GameState.h"
#include "PlayerNumber.h"
#include "Preference.h"
#include "RageFileManager.h"
#include "RageUtil.h"
#include "RageUtil/RandomNumbers.h"
#include "StatsManager.h"
#include "arch/Sound/RageSoundDriver_Gym.h"
#include "global.h"
#include "gym/GymClock.h"

namespace {
Gym::Options g_Options;

// The frame the step being run asked for, which the display fills.
bool g_bWantFrame = false;
std::vector<uint8_t> g_Frame;
int g_iFrameWidth = 0;
int g_iFrameHeight = 0;

// A gym process must not run on with anything it was not given: say why and
// stop, before the window or the protocol exist.
[[noreturn]] void Refuse(const std::string& sWhy) {
  std::fprintf(stderr, "itgmania gym: %s\n", sWhy.c_str());
  std::fflush(stderr);
  std::exit(2);
}

std::string RequiredArgument(const std::string& sName) {
  std::string sValue;
  if (!GetCommandlineArgument(sName, &sValue) || sValue.empty()) {
    Refuse("--" + sName + "=... is required in gym mode");
  }
  return sValue;
}

int PositiveInt(const std::string& sName, const std::string& sValue) {
  char* pEnd = nullptr;
  const long iValue = std::strtol(sValue.c_str(), &pEnd, 10);
  if (pEnd == sValue.c_str() || *pEnd != '\0' || iValue <= 0 ||
      iValue > 1000000000) {
    Refuse("--" + sName + "=" + sValue + ": expected a positive integer");
  }
  return static_cast<int>(iValue);
}

bool IsDirectory(const std::string& sPath) {
  struct stat st;
  return stat(sPath.c_str(), &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

void MakeDirectory(const std::string& sPath) {
  if (IsDirectory(sPath)) {
    return;
  }
  if (mkdir(sPath.c_str(), 0755) != 0) {
    Refuse("cannot create directory " + sPath);
  }
}

// The first audio frame of step k: steps end on exact sample counts, so the
// clock never drifts from k / fps.
int64_t SampleAtStep(int64_t k) {
  return k * GymClock::SAMPLE_RATE / g_Options.fps;
}

bool SameValue(const std::string& a, const std::string& b) {
  if (a == b) {
    return true;
  }
  char* pEndA = nullptr;
  char* pEndB = nullptr;
  const double fA = std::strtod(a.c_str(), &pEndA);
  const double fB = std::strtod(b.c_str(), &pEndB);
  return *pEndA == '\0' && *pEndB == '\0' && pEndA != a.c_str() &&
         pEndB != b.c_str() && std::abs(fA - fB) < 1e-4;
}

void SetPreference(const std::string& sName, const std::string& sValue) {
  IPreference* pPref = IPreference::GetPreferenceByName(sName);
  if (pPref == nullptr) {
    Refuse("no preference named " + sName);
  }
  pPref->FromString(sValue);
  // A value a preference cannot parse silently becomes its default.
  if (!SameValue(pPref->ToString(), sValue)) {
    Refuse(
        "preference " + sName + "=" + sValue + " was not accepted (it is " +
        pPref->ToString() + ")");
  }
}

// Writes interleaved stereo s16 PCM as a WAV file.
class WavFile {
 public:
  explicit WavFile(const std::string& sPath)
      : m_pFile(std::fopen(sPath.c_str(), "wb")) {
    if (m_pFile == nullptr) {
      Refuse("cannot write " + sPath);
    }
    WriteHeader();
  }
  ~WavFile() {
    std::fseek(m_pFile, 0, SEEK_SET);
    WriteHeader();
    std::fclose(m_pFile);
  }
  void Write(const std::vector<int16_t>& pcm) {
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), m_pFile);
    m_iDataBytes += static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
  }

 private:
  void WriteU32(uint32_t v) { std::fwrite(&v, 4, 1, m_pFile); }
  void WriteU16(uint16_t v) { std::fwrite(&v, 2, 1, m_pFile); }
  void WriteHeader() {
    const uint32_t iRate = GymClock::SAMPLE_RATE;
    std::fwrite("RIFF", 1, 4, m_pFile);
    WriteU32(36 + m_iDataBytes);
    std::fwrite("WAVEfmt ", 1, 8, m_pFile);
    WriteU32(16);
    WriteU16(1);  // PCM
    WriteU16(2);  // channels
    WriteU32(iRate);
    WriteU32(iRate * 4);
    WriteU16(4);
    WriteU16(16);
    std::fwrite("data", 1, 4, m_pFile);
    WriteU32(m_iDataBytes);
  }

  std::FILE* m_pFile;
  uint32_t m_iDataBytes = 0;
};

void WritePPM(const std::string& sPath) {
  std::FILE* f = std::fopen(sPath.c_str(), "wb");
  if (f == nullptr) {
    Refuse("cannot write " + sPath);
  }
  std::fprintf(f, "P6\n%d %d\n255\n", g_iFrameWidth, g_iFrameHeight);
  std::fwrite(g_Frame.data(), 1, g_Frame.size(), f);
  std::fclose(f);
}

// The audio that sounds while the frame just drawn is on screen: from the
// clock's current sample up to the start of the next step.
std::vector<int16_t> MixUntil(int64_t iNextSample) {
  // Sounds this update queued start now, so where they begin in the mix does
  // not depend on a thread.
  SOUND->Flush();
  const int iFrames = static_cast<int>(iNextSample - GymClock::Samples());
  std::vector<int16_t> pcm(static_cast<size_t>(iFrames) * 2);
  RageSoundDriver_Gym::Get()->MixStep(pcm.data(), iFrames);
  return pcm;
}

void WriteStateRow(std::FILE* f, int64_t iStep) {
  const PlayerStageStats& pss = STATSMAN->m_CurStageStats.m_player[PLAYER_1];
  std::fprintf(
      f, "%lld,%lld,%llu,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%u\n",
      static_cast<long long>(iStep),
      static_cast<long long>(GymClock::Samples()),
      static_cast<unsigned long long>(GymClock::NowMicroseconds()),
      GAMESTATE->m_Position.m_fMusicSeconds, GAMESTATE->m_Position.m_fSongBeat,
      pss.m_iTapNoteScores[TNS_W1], pss.m_iTapNoteScores[TNS_W2],
      pss.m_iTapNoteScores[TNS_W3], pss.m_iTapNoteScores[TNS_W4],
      pss.m_iTapNoteScores[TNS_W5], pss.m_iTapNoteScores[TNS_Miss],
      pss.m_iCurCombo);
}

/* --gym-selftest=N: reset, then N steps with AutoPlay, writing the audio
 * (audio.wav), one state row per step (steps.csv) and every n-th frame
 * (frames/NNNNNN.ppm) to --gym-selftest-dir. */
void RunSelfTest() {
  const Gym::Options& o = g_Options;
  MakeDirectory(o.selftest_dir + "/frames");
  WavFile wav(o.selftest_dir + "/audio.wav");
  std::FILE* csv = std::fopen((o.selftest_dir + "/steps.csv").c_str(), "w");
  if (csv == nullptr) {
    Refuse("cannot write " + o.selftest_dir + "/steps.csv");
  }
  std::fprintf(
      csv,
      "step,samples,t_us,music_seconds,song_beat,W1,W2,W3,W4,W5,Miss,combo\n");

  using Clock = std::chrono::steady_clock;
  std::vector<double> stepMs;
  stepMs.reserve(o.selftest_steps);

  // Reset: the first update only builds the screen (nothing is drawn on a
  // screen's first update), the second draws the first frame; the clock has
  // not moved. Then the audio that sounds while that frame is on screen.
  g_bWantFrame = o.selftest_frame_every > 0;
  GameLoop::UpdateAndDraw();
  GameLoop::UpdateAndDraw();
  wav.Write(MixUntil(SampleAtStep(1)));
  WriteStateRow(csv, -1);
  if (g_bWantFrame) {
    WritePPM(o.selftest_dir + "/frames/reset.ppm");
  }

  for (int64_t k = 0; k < o.selftest_steps; ++k) {
    const Clock::time_point start = Clock::now();
    g_bWantFrame =
        o.selftest_frame_every > 0 && (k + 1) % o.selftest_frame_every == 0;
    GymClock::AdvanceTo(SampleAtStep(k + 1));
    GameLoop::UpdateAndDraw();
    wav.Write(MixUntil(SampleAtStep(k + 2)));
    stepMs.push_back(
        std::chrono::duration<double, std::milli>(Clock::now() - start)
            .count());
    WriteStateRow(csv, k);
    if (g_bWantFrame) {
      WritePPM(ssprintf(
          "%s/frames/%06lld.ppm", o.selftest_dir.c_str(),
          static_cast<long long>(k)));
    }
  }
  std::fclose(csv);

  std::sort(stepMs.begin(), stepMs.end());
  double fMean = 0;
  for (double ms : stepMs) {
    fMean += ms / stepMs.size();
  }
  std::fprintf(
      stderr, "itgmania gym selftest: %d steps, %.2f ms mean, %.2f ms p95\n",
      o.selftest_steps, fMean,
      stepMs.empty() ? 0.0 : stepMs[stepMs.size() * 95 / 100]);
}
}  // namespace

void Gym::Init() {
  Options& o = g_Options;
  const bool bServe = GetCommandlineArgument("gym");
  std::string sSelfTest;
  const bool bSelfTest = GetCommandlineArgument("gym-selftest", &sSelfTest);
  if (bServe == bSelfTest) {
    Refuse("ITGMANIA_GYM=1 needs exactly one of --gym and --gym-selftest=N");
  }

  o.fps = PositiveInt("gym-fps", RequiredArgument("gym-fps"));
  if (o.fps > GymClock::SAMPLE_RATE) {
    Refuse("--gym-fps must not exceed the sample rate");
  }
  const std::string sSize = RequiredArgument("gym-screensize");
  const size_t x = sSize.find('x');
  if (x == std::string::npos) {
    Refuse("--gym-screensize=" + sSize + ": expected WIDTHxHEIGHT");
  }
  o.width = PositiveInt("gym-screensize", sSize.substr(0, x));
  o.height = PositiveInt("gym-screensize", sSize.substr(x + 1));
  o.seed = static_cast<uint32_t>(
      PositiveInt("gym-seed", RequiredArgument("gym-seed")));
  o.datadir = RequiredArgument("gym-datadir");
  if (!IsDirectory(o.datadir)) {
    Refuse("--gym-datadir=" + o.datadir + " is not a directory");
  }

  if (bSelfTest) {
    o.selftest_steps = PositiveInt("gym-selftest", sSelfTest);
    o.selftest_dir = RequiredArgument("gym-selftest-dir");
    MakeDirectory(o.selftest_dir);
    std::string sEvery;
    if (GetCommandlineArgument("gym-selftest-frame-every", &sEvery)) {
      o.selftest_frame_every = PositiveInt("gym-selftest-frame-every", sEvery);
    }
  }

  srand(o.seed);
  SeedRandomNumberGenerators(o.seed);
}

const Gym::Options& Gym::GetOptions() { return g_Options; }

void Gym::MountUserFilesystems() {
  for (const char* szDir : {"Cache", "Logs", "Save", "Screenshots"}) {
    const std::string sPath = g_Options.datadir + "/" + szDir;
    MakeDirectory(sPath);
    FILEMAN->Mount("dir", sPath, std::string("/") + szDir);
  }
}

void Gym::ApplyPreferences() {
  const Options& o = g_Options;
  const std::vector<std::pair<std::string, std::string>> prefs = {
      // A window that is never shown, exactly the frame size asked for.
      {"Windowed", "1"},
      {"DisplayWidth", std::to_string(o.width)},
      {"DisplayHeight", std::to_string(o.height)},
      {"DisplayAspectRatio",
       ssprintf("%.6f", static_cast<float>(o.width) / o.height)},
      {"Vsync", "0"},
      {"FrameLimitPercent", "0"},
      {"VideoRenderers", "opengl"},
      // Otherwise the first run on a machine resets the frame size.
      {"LastSeenVideoDriver", "OpenGL"},
      {"ShowLoadingWindow", "0"},
      {"ShowStats", "0"},
      // Drivers that read no clock, device or network of their own.
      {"SoundDrivers", "Gym"},
      {"InputDrivers", "Gym"},
      {"MovieDrivers", "Null"},
      {"HttpEnabled", "0"},
      {"MemoryCards", "0"},
      // Presses arrive stamped by the step; the mixer is the audio clock.
      {"InputDebounceTime", "0"},
      {"GlobalOffsetSeconds", "0"},
      {"DefaultSyncOffset", "NULL"},
      {"SoundVolume", "1"},
      // Nothing on screen that is drawn at random or by date.
      {"EasterEggs", "0"},
      {"ShowDancingCharacters", "Off"},
      {"RandomBackgroundMode", "Off"},
      // Straight into gameplay; several gym processes may run at once.
      {"EventMode", "1"},
      {"TestInitialScreen", "ScreenGameplay"},
      {"AllowMultipleInstances", "1"},
      {"AutogenGroupCourses", "0"},
  };
  for (const std::pair<std::string, std::string>& pref : prefs) {
    SetPreference(pref.first, pref.second);
  }
  if (o.selftest_steps > 0) {
    GamePreferences::m_AutoPlay.Set(PC_AUTOPLAY);
  }
}

uint8_t* Gym::FrameDestination(int width, int height) {
  if (!g_bWantFrame) {
    return nullptr;
  }
  g_iFrameWidth = width;
  g_iFrameHeight = height;
  g_Frame.resize(static_cast<size_t>(width) * height * 3);
  return g_Frame.data();
}

void Gym::Run() {
  if (RageSoundDriver_Gym::Get() == nullptr) {
    Refuse("the Gym sound driver did not start (SoundDrivers=Gym)");
  }
  if (g_Options.selftest_steps > 0) {
    RunSelfTest();
    return;
  }
  Refuse("--gym: the protocol server is not built yet; use --gym-selftest");
}

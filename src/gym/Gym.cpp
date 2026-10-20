#include "gym/Gym.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "GameConstantsAndTypes.h"
#include "GameInput.h"
#include "GameLoop.h"
#include "GameManager.h"
#include "GamePreferences.h"
#include "GameSoundManager.h"
#include "GameState.h"
#include "InputFilter.h"
#include "InputMapper.h"
#include "LuaManager.h"
#include "MessageManager.h"
#include "PlayerNumber.h"
#include "PlayerOptions.h"
#include "Preference.h"
#include "RageFileManager.h"
#include "RageInput.h"
#include "RageInputDevice.h"
#include "RageTimer.h"
#include "RageUtil.h"
#include "RageUtil/RandomNumbers.h"
#include "Song.h"
#include "SongManager.h"
#include "SongOptions.h"
#include "SongUtil.h"
#include "StatsManager.h"
#include "StdString.h"
#include "Steps.h"
#include "Style.h"
#include "ThemeManager.h"
#include "arch/Sound/RageSoundDriver_Gym.h"
#include "global.h"
#include "gym/GymClock.h"
#include "gym/GymServer.h"

namespace {
Gym::Options g_Options;

// The keys the gym presses for each button, and the dance buttons they must
// map to for player 1.
const DeviceButton BUTTON_KEYS[Gym::NUM_BUTTONS] = {
    KEY_LEFT, KEY_DOWN, KEY_UP, KEY_RIGHT};
const GameButton DANCE_BUTTONS[Gym::NUM_BUTTONS] = {
    DANCE_BUTTON_LEFT, DANCE_BUTTON_DOWN, DANCE_BUTTON_UP, DANCE_BUTTON_RIGHT};

// The frame the step being run asked for, which the display fills.
bool g_bWantFrame = false;
std::vector<uint8_t> g_Frame;

int64_t g_iStepsTaken = -1;
bool g_bFinished = false;
Gym::Buttons g_Held = {};
std::vector<Gym::Edge> g_Edges;
std::vector<Gym::Judgment> g_Judgments;
std::vector<std::string> g_ScriptErrors;

// A gym process must not run on with anything it was not given: say why and
// stop.
[[noreturn]] void Refuse(const std::string& sWhy) {
  std::fprintf(stderr, "itgmania gym: %s\n", sWhy.c_str());
  std::fflush(stderr);
  std::exit(2);
}

// The engine reads the first of two copies of an argument; refuse the second
// rather than ignore it.
void RefuseRepeated(const std::string& sName) {
  if (GetCommandlineArgument(sName, nullptr, 1)) {
    Refuse("--" + sName + " is given more than once");
  }
}

std::string RequiredArgument(const std::string& sName) {
  RefuseRepeated(sName);
  std::string sValue;
  if (!GetCommandlineArgument(sName, &sValue) || sValue.empty()) {
    Refuse("--" + sName + "=... is required in gym mode");
  }
  return sValue;
}

std::string OptionalArgument(const std::string& sName) {
  RefuseRepeated(sName);
  std::string sValue;
  GetCommandlineArgument(sName, &sValue);
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

using PreferenceList = std::vector<std::pair<std::string, std::string>>;

// The preferences gym mode depends on; --gym-pref may not change them.
const PreferenceList& OwnedPreferences() {
  static PreferenceList prefs;
  if (!prefs.empty()) {
    return prefs;
  }
  const Gym::Options& o = g_Options;
  prefs = {
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
      // Drivers that read no clock, device or network of their own.
      {"SoundDrivers", "Gym"},
      {"InputDrivers", "Gym"},
      {"MovieDrivers", "Null"},
      {"HttpEnabled", "0"},
      {"MemoryCards", "0"},
      // Nothing on screen that is drawn at random or by date.
      {"EasterEggs", "0"},
      {"ShowDancingCharacters", "Off"},
      {"RandomBackgroundMode", "Off"},
      // Straight into gameplay; several gym processes may run at once.
      {"EventMode", "1"},
      {"TestInitialScreen", "ScreenGameplay"},
      {"AllowMultipleInstances", "1"},
  };
  return prefs;
}

// What a gym run plays with unless --gym-pref says otherwise.
const PreferenceList& DefaultPreferences() {
  static const PreferenceList prefs = {
      // Presses arrive stamped by the step, and the mixer is the audio clock,
      // so no latency needs hiding. A setup whose sound reaches the ears late
      // sets GlobalOffsetSeconds to minus that delay.
      {"InputDebounceTime", "0"},
      {"GlobalOffsetSeconds", "0"},
      {"DefaultSyncOffset", "NULL"},
      {"SoundVolume", "1"},
      {"ShowStats", "0"},
      {"AutogenGroupCourses", "0"},
  };
  return prefs;
}

void ApplyUserPreferences() {
  std::set<std::string> owned = {"AutoPlay", "ConstantUpdateDeltaSeconds"};
  for (const std::pair<std::string, std::string>& pref : OwnedPreferences()) {
    owned.insert(pref.first);
  }
  for (const std::pair<std::string, std::string>& pref : g_Options.prefs) {
    if (owned.count(pref.first)) {
      Refuse("--gym-pref cannot change " + pref.first + ": gym mode sets it");
    }
    SetPreference(pref.first, pref.second);
  }
}

// Every modifier must be one the engine knows: an unknown one is otherwise
// dropped without a word.
void CheckModifiers(const std::string& sMods) {
  for (std::string sMod : split(sMods, ',', true)) {
    Trim(sMod);
    if (sMod.empty()) {
      continue;
    }
    PlayerOptions po;
    SongOptions so;
    std::string sError;
    if (!po.FromOneModString(sMod, sError) &&
        !so.FromOneModString(sMod, sError)) {
      Refuse("modifier \"" + sMod + "\" is not one the engine knows");
    }
  }
}

// Simply Love keeps its own copy of the speed mod and the music rate, which
// it shows and which its menus would have set; mirror the ones given.
std::string SimplyLoveModifiers(const std::string& sMods) {
  std::string sLua;
  for (std::string sMod : split(sMods, ',', true)) {
    Trim(sMod);
    MakeLower(sMod);
    char* pEnd = nullptr;
    if (EndsWith(sMod, "xmusic")) {
      const double fRate = std::strtod(sMod.c_str(), &pEnd);
      sLua += ssprintf("SL.Global.ActiveModifiers.MusicRate = %g\n", fRate);
    } else if (!sMod.empty() && (sMod[0] == 'c' || sMod[0] == 'm')) {
      const double fSpeed = std::strtod(sMod.c_str() + 1, &pEnd);
      if (pEnd != sMod.c_str() + 1 && *pEnd == '\0') {
        sLua += ssprintf(
            "SL.P1.ActiveModifiers.SpeedModType = '%c'\n"
            "SL.P1.ActiveModifiers.SpeedMod = %g\n",
            sMod[0] == 'c' ? 'C' : 'M', fSpeed);
      }
    } else if (EndsWith(sMod, "x")) {
      const double fSpeed = std::strtod(sMod.c_str(), &pEnd);
      if (pEnd == sMod.c_str() + sMod.size() - 1) {
        sLua += ssprintf(
            "SL.P1.ActiveModifiers.SpeedModType = 'X'\n"
            "SL.P1.ActiveModifiers.SpeedMod = %g\n",
            fSpeed);
      }
    }
  }
  return sLua;
}

// What Simply Love's menus do before gameplay: its game mode's preferences
// (timing windows, life) and its copy of the modifiers.
void ApplySimplyLoveGameMode() {
  const Gym::Options& o = g_Options;
  if (THEME->GetCurThemeName() != "Simply Love") {
    Refuse(
        "--gym-game-mode is Simply Love's; the theme is " +
        THEME->GetCurThemeName());
  }
  const std::string sScript = ssprintf(
                                  "SL.Global.GameMode = '%s'\n"
                                  "SetGameModePreferences()\n"
                                  "THEME:ReloadMetrics()\n",
                                  o.game_mode.c_str()) +
                              SimplyLoveModifiers(o.mods);
  Lua* L = LUA->Get();
  std::string sError;
  const bool bOK = LuaHelpers::RunScript(L, sScript, "@gym", sError, 0, 0);
  LUA->Release(L);
  if (!bOK) {
    Refuse("Simply Love game mode \"" + o.game_mode + "\": " + sError);
  }
}

// Put player 1 on the requested chart, as the menus would have.
void SetUpStage() {
  const Gym::Options& o = g_Options;
  const StepsType st = GAMEMAN->StringToStepsType(o.steps_type);
  if (st != StepsType_dance_single) {
    Refuse(
        "--gym-steps-type=" + o.steps_type + ": only dance-single is played");
  }
  const Difficulty dc = StringToDifficulty(o.difficulty);
  if (dc == Difficulty_Invalid) {
    Refuse("--gym-difficulty=" + o.difficulty + " is not a difficulty");
  }
  Song* pSong = SONGMAN->FindSong(o.song);
  if (pSong == nullptr) {
    Refuse("no song \"" + o.song + "\" (a song is Group/Folder under Songs/)");
  }
  Steps* pSteps = SongUtil::GetStepsByDifficulty(pSong, st, dc, false);
  if (pSteps == nullptr) {
    Refuse(
        "\"" + o.song + "\" has no " + o.steps_type + " " + o.difficulty +
        " chart");
  }
  const Style* pStyle =
      GAMEMAN->GetFirstCompatibleStyle(GAMESTATE->GetCurrentGame(), 1, st);
  if (pStyle == nullptr) {
    Refuse("no style plays " + o.steps_type);
  }
  CheckModifiers(o.mods);

  GAMESTATE->JoinPlayer(PLAYER_1);
  GAMESTATE->SetCurrentStyle(pStyle, PLAYER_1);
  GAMESTATE->m_PlayMode.Set(PLAY_MODE_REGULAR);
  GAMESTATE->m_pCurSong.Set(pSong);
  GAMESTATE->m_pPreferredSong = pSong;
  GAMESTATE->m_PreferredDifficulty[PLAYER_1].Set(dc);
  GAMESTATE->m_pCurSteps[PLAYER_1].Set(pSteps);

  // The theme's game mode sets preferences and a fail type of its own, so it
  // goes first and the modifiers and preferences asked for win.
  if (!o.game_mode.empty()) {
    ApplySimplyLoveGameMode();
  }
  ApplyUserPreferences();
  GAMESTATE->ApplyPreferredModifiers(PLAYER_1, o.mods);
  // Otherwise easy charts quietly soften the fail type.
  GAMESTATE->m_bFailTypeWasExplicitlySet = true;
}

// The input the game reads must be the gym's alone, and its arrows player 1's.
void CheckInput() {
  std::vector<InputDeviceInfo> devices;
  INPUTMAN->GetDevicesAndDescriptions(devices);
  for (const InputDeviceInfo& device : devices) {
    if (device.sDesc != "Gym" && device.sDesc != "MonkeyKeyboard") {
      Refuse(
          "input device \"" + device.sDesc +
          "\" is not the gym's (InputDrivers=Gym)");
    }
  }
  for (int c = 0; c < Gym::NUM_BUTTONS; ++c) {
    GameInput gi;
    if (!INPUTMAPPER->DeviceToGame(
            DeviceInput(DEVICE_KEYBOARD, BUTTON_KEYS[c]), gi) ||
        gi.controller != GameController_1 || gi.button != DANCE_BUTTONS[c]) {
      Refuse("the arrow keys do not map to player 1's dance buttons");
    }
  }
}

bool HasParam(const Message& msg, const char* szName) {
  Lua* L = LUA->Get();
  msg.GetParamFromStack(L, szName);
  const bool bHas = !lua_isnil(L, -1);
  lua_pop(L, 1);
  LUA->Release(L);
  return bHas;
}

// Collects player 1's judgments and every Lua error, as they are broadcast.
class Listener : public MessageSubscriber {
 public:
  Listener() {
    SubscribeToMessage("Judgment");
    SubscribeToMessage("ScriptError");
  }

  void HandleMessage(const Message& msg) {
    if (msg.GetName() == "ScriptError") {
      std::string sError;
      msg.GetParam("message", sError);
      g_ScriptErrors.push_back(sError);
      return;
    }
    PlayerNumber pn = PLAYER_INVALID;
    if (!msg.GetParam("Player", pn) || pn != PLAYER_1) {
      return;
    }
    Gym::Judgment j = {-1, "", "", -1, 0.0f, false, false};
    TapNoteScore tns = TNS_None;
    msg.GetParam("FirstTrack", j.column);
    msg.GetParam("TapNoteScore", tns);
    j.tap_note_score = TapNoteScoreToString(tns);
    if (HasParam(msg, "HoldNoteScore")) {
      HoldNoteScore hns = HNS_None;
      msg.GetParam("HoldNoteScore", hns);
      j.hold_note_score = HoldNoteScoreToString(hns);
    } else if (HasParam(msg, "TapNoteOffset")) {
      msg.GetParam("TapNoteOffset", j.offset);
      msg.GetParam("Early", j.early);
      msg.GetParam("Row", j.row);
      if (HasParam(msg, "HeldMiss")) {
        msg.GetParam("HeldMiss", j.held_miss);
      }
    }
    g_Judgments.push_back(j);
  }
};

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

// Press and release what changed since the last step, stamped in the middle
// of the step: a press could have come at any time during it.
void PressButtons(const Gym::Buttons& buttons, int64_t iStart, int64_t iEnd) {
  g_Edges.clear();
  const uint64_t iEndUs = GymClock::MicrosecondsAt(iEnd);
  const uint64_t iTs = iEndUs - (iEndUs - GymClock::MicrosecondsAt(iStart)) / 2;
  const RageTimer ts(iTs / 1000000, iTs % 1000000);
  for (int c = 0; c < Gym::NUM_BUTTONS; ++c) {
    if (buttons[c] == g_Held[c]) {
      continue;
    }
    g_Held[c] = buttons[c];
    INPUTFILTER->ButtonPressed(DeviceInput(
        DEVICE_KEYBOARD, BUTTON_KEYS[c], buttons[c] ? 1.0f : 0.0f, ts));
    g_Edges.push_back({c, buttons[c], iTs});
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
  std::fprintf(f, "P6\n%d %d\n255\n", g_Options.width, g_Options.height);
  std::fwrite(g_Frame.data(), 1, g_Frame.size(), f);
  std::fclose(f);
}

/* --gym-selftest=N: reset, then N steps with AutoPlay, writing the audio
 * (audio.wav), one row per step (steps.csv: the clock, the song position and
 * the judgments made so far) and every n-th frame (frames/NNNNNN.ppm) to
 * --gym-selftest-dir. */
void RunSelfTest() {
  const Gym::Options& o = g_Options;
  MakeDirectory(o.selftest_dir + "/frames");
  WavFile wav(o.selftest_dir + "/audio.wav");
  std::FILE* csv = std::fopen((o.selftest_dir + "/steps.csv").c_str(), "w");
  if (csv == nullptr) {
    Refuse("cannot write " + o.selftest_dir + "/steps.csv");
  }
  std::fprintf(
      csv, "step,samples,t_us,music_seconds,song_beat,W1,W2,W3,W4,W5,Miss\n");
  int counts[NUM_TapNoteScore] = {};
  auto writeRow = [&](int64_t iStep) {
    for (const Gym::Judgment& j : Gym::TakeJudgments()) {
      if (j.hold_note_score.empty()) {
        ++counts[StringToTapNoteScore(j.tap_note_score)];
      }
    }
    std::fprintf(
        csv, "%lld,%lld,%llu,%.6f,%.6f,%d,%d,%d,%d,%d,%d\n",
        static_cast<long long>(iStep),
        static_cast<long long>(GymClock::Samples()),
        static_cast<unsigned long long>(GymClock::NowMicroseconds()),
        GAMESTATE->m_Position.m_fMusicSeconds,
        GAMESTATE->m_Position.m_fSongBeat, counts[TNS_W1], counts[TNS_W2],
        counts[TNS_W3], counts[TNS_W4], counts[TNS_W5], counts[TNS_Miss]);
  };

  using Clock = std::chrono::steady_clock;
  std::vector<double> stepMs;
  stepMs.reserve(o.selftest_steps);
  std::vector<int16_t> pcm;

  Gym::Reset(o.selftest_frame_every > 0, pcm);
  wav.Write(pcm);
  writeRow(-1);
  if (o.selftest_frame_every > 0) {
    WritePPM(o.selftest_dir + "/frames/reset.ppm");
  }

  const Gym::Buttons none = {};
  for (int64_t k = 0; k < o.selftest_steps && !Gym::IsFinished(); ++k) {
    const bool bFrame =
        o.selftest_frame_every > 0 && (k + 1) % o.selftest_frame_every == 0;
    const Clock::time_point start = Clock::now();
    Gym::Step(none, bFrame, pcm);
    stepMs.push_back(
        std::chrono::duration<double, std::milli>(Clock::now() - start)
            .count());
    wav.Write(pcm);
    writeRow(k);
    if (bFrame) {
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
      stderr, "itgmania gym selftest: %zu steps%s, %.2f ms mean, %.2f ms p95\n",
      stepMs.size(), Gym::IsFinished() ? " (stage finished)" : "", fMean,
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
  if (GetCommandlineArgument("player") || GetCommandlineArgument("mode")) {
    Refuse("--player and --mode do not apply: the stage is --gym-song etc.");
  }
  if (bServe) {
    GymServer::ClaimStdout();
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
  o.songs_dir = OptionalArgument("gym-songs-dir");
  if (!o.songs_dir.empty() && !IsDirectory(o.songs_dir)) {
    Refuse("--gym-songs-dir=" + o.songs_dir + " is not a directory");
  }

  o.song = RequiredArgument("gym-song");
  o.steps_type = RequiredArgument("gym-steps-type");
  o.difficulty = RequiredArgument("gym-difficulty");
  o.mods = RequiredArgument("gym-mods");
  o.game_mode = OptionalArgument("gym-game-mode");
  std::string sPref;
  for (int i = 0; GetCommandlineArgument("gym-pref", &sPref, i); ++i) {
    const size_t eq = sPref.find('=');
    if (eq == std::string::npos || eq == 0) {
      Refuse("--gym-pref=" + sPref + ": expected Name=Value");
    }
    o.prefs.emplace_back(sPref.substr(0, eq), sPref.substr(eq + 1));
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
  if (!g_Options.songs_dir.empty()) {
    FILEMAN->Mount("dirro", g_Options.songs_dir, "/Songs");
  }
}

void Gym::ApplyPreferences() {
  for (const PreferenceList& prefs : {OwnedPreferences(), DefaultPreferences()}) {
    for (const std::pair<std::string, std::string>& pref : prefs) {
      SetPreference(pref.first, pref.second);
    }
  }
  ApplyUserPreferences();
  if (g_Options.selftest_steps > 0) {
    GamePreferences::m_AutoPlay.Set(PC_AUTOPLAY);
  }
}

uint8_t* Gym::FrameDestination(int width, int height) {
  if (!g_bWantFrame) {
    return nullptr;
  }
  if (width != g_Options.width || height != g_Options.height) {
    Refuse(ssprintf(
        "the frame is %dx%d, not the %dx%d asked for", width, height,
        g_Options.width, g_Options.height));
  }
  g_Frame.resize(static_cast<size_t>(width) * height * 3);
  return g_Frame.data();
}

void Gym::OnStageFinished() { g_bFinished = true; }

void Gym::Run() {
  if (RageSoundDriver_Gym::Get() == nullptr) {
    Refuse("the Gym sound driver did not start (SoundDrivers=Gym)");
  }
  CheckInput();
  SetUpStage();
  std::unique_ptr<Listener> pListener(new Listener);
  if (g_Options.selftest_steps > 0) {
    RunSelfTest();
  } else {
    GymServer::Serve();
  }
}

void Gym::Reset(bool bWantFrame, std::vector<int16_t>& pcm) {
  ASSERT_M(g_iStepsTaken < 0, "a gym process resets once");
  g_bWantFrame = bWantFrame;
  // The first update only builds the screen (nothing is drawn on a screen's
  // first update), the second draws the first frame; the clock has not moved.
  GameLoop::UpdateAndDraw();
  GameLoop::UpdateAndDraw();
  pcm = MixUntil(SampleAtStep(1));
  g_iStepsTaken = 0;
}

void Gym::Step(
    const Buttons& buttons, bool bWantFrame, std::vector<int16_t>& pcm) {
  ASSERT_M(g_iStepsTaken >= 0, "a gym process steps after its reset");
  const int64_t k = g_iStepsTaken;
  GymClock::AdvanceTo(SampleAtStep(k + 1));
  PressButtons(buttons, SampleAtStep(k), SampleAtStep(k + 1));
  g_bWantFrame = bWantFrame;
  GameLoop::UpdateAndDraw();
  pcm = MixUntil(SampleAtStep(k + 2));
  g_iStepsTaken = k + 1;
}

int64_t Gym::StepsTaken() { return g_iStepsTaken; }

bool Gym::IsFinished() { return g_bFinished; }

const std::vector<uint8_t>& Gym::LastFrame() { return g_Frame; }

const std::vector<Gym::Edge>& Gym::LastEdges() { return g_Edges; }

std::vector<Gym::Judgment> Gym::TakeJudgments() {
  std::vector<Judgment> judgments;
  judgments.swap(g_Judgments);
  return judgments;
}

std::vector<std::string> Gym::TakeScriptErrors() {
  std::vector<std::string> errors;
  errors.swap(g_ScriptErrors);
  return errors;
}

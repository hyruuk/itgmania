#include "gym/GymServer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "GameConstantsAndTypes.h"
#include "GameInput.h"
#include "GameManager.h"
#include "GameState.h"
#include "InputMapper.h"
#include "PlayerOptions.h"
#include "PlayerState.h"
#include "Preference.h"
#include "RadarValues.h"
#include "Song.h"
#include "SongOptions.h"
#include "StatsManager.h"
#include "Steps.h"
#include "ThemeManager.h"
#include "global.h"
#include "gym/Gym.h"
#include "gym/GymClock.h"
#include "json/json.h"
#include "ver.h"

namespace {
const int PROTOCOL_VERSION = 1;

std::FILE* g_pOut = nullptr;

const GameButton DANCE_BUTTONS[Gym::NUM_BUTTONS] = {
    DANCE_BUTTON_LEFT, DANCE_BUTTON_DOWN, DANCE_BUTTON_UP, DANCE_BUTTON_RIGHT};

std::string PreferenceValue(const std::string& sName) {
  IPreference* pPref = IPreference::GetPreferenceByName(sName);
  return pPref == nullptr ? std::string() : pPref->ToString();
}

Json::Value Meta() {
  const Gym::Options& o = Gym::GetOptions();
  const Song* pSong = GAMESTATE->m_pCurSong;
  const Steps* pSteps = GAMESTATE->m_pCurSteps[PLAYER_1];
  Json::Value meta;
  meta["protocol"] = PROTOCOL_VERSION;
  meta["engine"] = product_version;
  meta["engine_git"] = sm_version_git_hash;
  meta["renderer"] = Gym::Renderer();
  meta["fps"] = o.fps;
  meta["seed"] = o.seed;
  meta["frame"]["width"] = o.width;
  meta["frame"]["height"] = o.height;
  meta["frame"]["format"] = "rgb8";
  meta["audio"]["rate"] = GymClock::SAMPLE_RATE;
  meta["audio"]["channels"] = 2;
  meta["audio"]["format"] = "s16le";
  meta["theme"] = THEME->GetCurThemeName();
  meta["game_mode"] = o.game_mode;
  meta["movies"] = "off";

  meta["song"]["path"] = o.song;
  meta["song"]["title"] = pSong->GetDisplayFullTitle();
  meta["song"]["artist"] = pSong->m_sArtist;
  meta["song"]["length_seconds"] = pSong->m_fMusicLengthSeconds;

  const RadarValues& radar = pSteps->GetRadarValues(PLAYER_1);
  meta["chart"]["steps_type"] = o.steps_type;
  meta["chart"]["difficulty"] = DifficultyToString(pSteps->GetDifficulty());
  meta["chart"]["meter"] = pSteps->GetMeter();
  meta["chart"]["taps_and_holds"] = radar[RadarCategory_TapsAndHolds];
  meta["chart"]["jumps"] = radar[RadarCategory_Jumps];
  meta["chart"]["holds"] = radar[RadarCategory_Holds];
  meta["chart"]["mines"] = radar[RadarCategory_Mines];
  meta["chart"]["rolls"] = radar[RadarCategory_Rolls];

  // The options as the engine read them back, which is what gets played.
  meta["options"]["player"] = GAMESTATE->m_pPlayerState[PLAYER_1]
                                  ->m_PlayerOptions.GetPreferred()
                                  .GetString();
  meta["options"]["song"] = GAMESTATE->m_SongOptions.GetPreferred().GetString();

  for (const char* szWindow :
       {"W1", "W2", "W3", "W4", "W5", "Mine", "Hold", "Roll", "Attack",
        "Checkpoint"}) {
    meta["timing_windows"][szWindow] =
        PreferenceValue(std::string("TimingWindowSeconds") + szWindow);
  }
  meta["timing_windows"]["Add"] = PreferenceValue("TimingWindowAdd");
  meta["timing_windows"]["Scale"] = PreferenceValue("TimingWindowScale");

  for (const char* szPref :
       {"InputDebounceTime", "GlobalOffsetSeconds", "DefaultSyncOffset",
        "VisualDelaySeconds", "SoundVolume", "BGBrightness", "EasterEggs"}) {
    meta["preferences"][szPref] = PreferenceValue(szPref);
  }
  for (const std::pair<std::string, std::string>& pref : o.prefs) {
    meta["preferences"][pref.first] = PreferenceValue(pref.first);
  }
  return meta;
}

Json::Value State() {
  const PlayerStageStats& pss = STATSMAN->m_CurStageStats.m_player[PLAYER_1];
  Json::Value state;
  state["step"] = static_cast<Json::Int64>(Gym::StepsTaken());
  state["samples"] = static_cast<Json::Int64>(GymClock::Samples());
  state["t_us"] = static_cast<Json::UInt64>(GymClock::NowMicroseconds());
  state["music_seconds"] = GAMESTATE->m_Position.m_fMusicSeconds;
  state["song_beat"] = GAMESTATE->m_Position.m_fSongBeat;
  state["bps"] = GAMESTATE->m_Position.m_fCurBPS;

  state["edges"] = Json::Value(Json::arrayValue);
  for (const Gym::Edge& edge : Gym::LastEdges()) {
    Json::Value e;
    e["column"] = edge.column;
    e["down"] = edge.down;
    e["ts_us"] = static_cast<Json::UInt64>(edge.ts_us);
    state["edges"].append(e);
  }
  state["judgments"] = Json::Value(Json::arrayValue);
  for (const Gym::Judgment& judgment : Gym::TakeJudgments()) {
    Json::Value j;
    j["column"] = judgment.column;
    j["tap_note_score"] = judgment.tap_note_score;
    if (!judgment.hold_note_score.empty()) {
      j["hold_note_score"] = judgment.hold_note_score;
    } else if (judgment.row >= 0) {
      j["row"] = judgment.row;
      j["offset"] = judgment.offset;
      j["early"] = judgment.early;
      j["held_miss"] = judgment.held_miss;
    }
    state["judgments"].append(j);
  }
  state["held"] = Json::Value(Json::arrayValue);
  for (const GameButton button : DANCE_BUTTONS) {
    state["held"].append(
        INPUTMAPPER->IsBeingPressed(GameInput(GameController_1, button)));
  }

  for (int tns = TNS_HitMine; tns < NUM_TapNoteScore; ++tns) {
    state["tap_note_scores"]
         [TapNoteScoreToString(static_cast<TapNoteScore>(tns))] =
             pss.m_iTapNoteScores[tns];
  }
  for (int hns = HNS_LetGo; hns < NUM_HoldNoteScore; ++hns) {
    state["hold_note_scores"]
         [HoldNoteScoreToString(static_cast<HoldNoteScore>(hns))] =
             pss.m_iHoldNoteScores[hns];
  }
  state["combo"] = pss.m_iCurCombo;
  state["max_combo"] = pss.m_iMaxCombo;
  state["miss_combo"] = pss.m_iCurMissCombo;
  state["score"] = pss.m_iScore;
  state["actual_dance_points"] = pss.m_iActualDancePoints;
  state["current_possible_dance_points"] = pss.m_iCurPossibleDancePoints;
  state["possible_dance_points"] = pss.m_iPossibleDancePoints;
  state["percent_dance_points"] = pss.GetPercentDancePoints();
  state["life"] = pss.GetCurrentLife();
  state["failed"] = pss.m_bFailed;
  state["alive_seconds"] = pss.m_fAliveSeconds;
  state["finished"] = Gym::IsFinished();

  state["script_errors"] = Json::Value(Json::arrayValue);
  for (const std::string& sError : Gym::TakeScriptErrors()) {
    state["script_errors"].append(sError);
  }
  return state;
}

// One reply: its JSON line, then the frame and the audio it announces.
void Reply(
    Json::Value reply, const std::vector<uint8_t>* pFrame,
    const std::vector<int16_t>* pPcm) {
  const Gym::Options& o = Gym::GetOptions();
  reply["ok"] = true;
  if (pFrame != nullptr) {
    reply["frame"]["width"] = o.width;
    reply["frame"]["height"] = o.height;
    reply["frame"]["format"] = "rgb8";
  }
  if (pPcm != nullptr) {
    reply["audio"]["frames"] = static_cast<Json::UInt64>(pPcm->size() / 2);
    reply["audio"]["channels"] = 2;
    reply["audio"]["rate"] = GymClock::SAMPLE_RATE;
    reply["audio"]["format"] = "s16le";
  }
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string sLine = Json::writeString(writer, reply) + "\n";
  std::fwrite(sLine.data(), 1, sLine.size(), g_pOut);
  if (pFrame != nullptr) {
    std::fwrite(pFrame->data(), 1, pFrame->size(), g_pOut);
  }
  if (pPcm != nullptr) {
    std::fwrite(pPcm->data(), sizeof(int16_t), pPcm->size(), g_pOut);
  }
  std::fflush(g_pOut);
}

void ReplyError(
    const Json::Value& id, const std::string& sKind,
    const std::string& sError) {
  Json::Value reply;
  reply["id"] = id;
  reply["ok"] = false;
  reply["kind"] = sKind;
  reply["error"] = sError;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string sLine = Json::writeString(writer, reply) + "\n";
  std::fwrite(sLine.data(), 1, sLine.size(), g_pOut);
  std::fflush(g_pOut);
}

bool ParseButtons(const Json::Value& value, Gym::Buttons& buttons) {
  if (!value.isArray() || value.size() != Gym::NUM_BUTTONS) {
    return false;
  }
  for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
    if (value[i].isBool()) {
      buttons[i] = value[i].asBool();
    } else if (
        value[i].isIntegral() &&
        (value[i].asInt() == 0 || value[i].asInt() == 1)) {
      buttons[i] = value[i].asInt() == 1;
    } else {
      return false;
    }
  }
  return true;
}

// Answers one request; false once the process should exit.
bool Answer(const Json::Value& request) {
  const Json::Value id = request.get("id", Json::Value());
  const std::string sCmd = request.get("cmd", "").asString();
  const bool bFrame = request.get("frame", true).asBool();
  std::vector<int16_t> pcm;

  if (sCmd == "hello") {
    Json::Value reply;
    reply["id"] = id;
    reply["meta"] = Meta();
    Reply(reply, nullptr, nullptr);
  } else if (sCmd == "reset") {
    if (Gym::StepsTaken() >= 0) {
      ReplyError(id, "bad_request", "a gym process resets once");
      return true;
    }
    Gym::Reset(bFrame, pcm);
    Json::Value reply;
    reply["id"] = id;
    reply["state"] = State();
    Reply(reply, bFrame ? &Gym::LastFrame() : nullptr, &pcm);
  } else if (sCmd == "step") {
    Gym::Buttons buttons = {};
    if (Gym::StepsTaken() < 0) {
      ReplyError(id, "not_reset", "step before reset");
    } else if (Gym::IsFinished()) {
      ReplyError(id, "episode_over", "the stage has finished");
    } else if (!ParseButtons(request["buttons"], buttons)) {
      ReplyError(id, "bad_request", "\"buttons\" must be four booleans or 0/1");
    } else {
      Gym::Step(buttons, bFrame, pcm);
      Json::Value reply;
      reply["id"] = id;
      reply["state"] = State();
      Reply(reply, bFrame ? &Gym::LastFrame() : nullptr, &pcm);
    }
  } else if (sCmd == "state") {
    Json::Value reply;
    reply["id"] = id;
    reply["state"] = State();
    Reply(reply, nullptr, nullptr);
  } else if (sCmd == "close") {
    Json::Value reply;
    reply["id"] = id;
    Reply(reply, nullptr, nullptr);
    return false;
  } else {
    ReplyError(id, "bad_request", "unknown command \"" + sCmd + "\"");
  }
  return true;
}
}  // namespace

void GymServer::ClaimStdout() {
  g_pOut = fdopen(dup(STDOUT_FILENO), "wb");
  // What the game prints to stdout is its log, which it also writes to
  // Logs/log.txt in the data directory; stderr keeps errors and warnings.
  const int iNull = open("/dev/null", O_WRONLY);
  dup2(iNull, STDOUT_FILENO);
  close(iNull);
}

void GymServer::Serve() {
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string sLine;
  while (std::getline(std::cin, sLine)) {
    if (!sLine.empty() && sLine.back() == '\r') {
      sLine.pop_back();
    }
    if (sLine.empty()) {
      continue;
    }
    Json::Value request;
    std::string sErrors;
    if (!reader->parse(
            sLine.data(), sLine.data() + sLine.size(), &request, &sErrors) ||
        !request.isObject()) {
      ReplyError(Json::Value(), "bad_request", "not a JSON object: " + sErrors);
      continue;
    }
    if (!Answer(request)) {
      return;
    }
  }
}

#include "RageSoundDriver_Gym.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "global.h"
#include "gym/GymClock.h"

REGISTER_SOUND_DRIVER_CLASS(Gym);

namespace {
const int CHANNELS = 2;

// Mix at most this many frames between two decodes. A sound's buffer holds
// about 4352 frames, so a chunk never runs it dry, and the positions one chunk
// records fit in a sound's 64-entry position queue.
const int MAX_CHUNK_FRAMES = 1024;

RageSoundDriver_Gym* g_pDriver = nullptr;
}  // namespace

RageSoundDriver_Gym::RageSoundDriver_Gym() { DecodeOnCallingThread(); }

RageSoundDriver_Gym::~RageSoundDriver_Gym() {
  if (g_pDriver == this) {
    g_pDriver = nullptr;
  }
}

std::string RageSoundDriver_Gym::Init() {
  if (!GymClock::IsEnabled()) {
    return "the Gym sound driver only runs in gym mode (ITGMANIA_GYM=1)";
  }
  g_pDriver = this;
  return std::string();
}

int64_t RageSoundDriver_Gym::GetPosition() const { return GymClock::Samples(); }

int RageSoundDriver_Gym::GetSampleRate() const { return GymClock::SAMPLE_RATE; }

void RageSoundDriver_Gym::MixStep(int16_t* pOut, int iFrames) {
  const int64_t iNow = GymClock::Samples();
  for (int iDone = 0; iDone < iFrames;) {
    const int iChunk = std::min(iFrames - iDone, MAX_CHUNK_FRAMES);
    DecodePlayingSounds();
    Mix(pOut + iDone * CHANNELS, iChunk, iNow + iDone, iNow);
    DrainMixedPositions();
    iDone += iChunk;
  }
}

RageSoundDriver_Gym* RageSoundDriver_Gym::Get() { return g_pDriver; }

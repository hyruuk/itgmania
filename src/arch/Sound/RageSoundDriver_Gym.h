/* RageSoundDriver_Gym - the sound driver of the lock-step gym mode. */

#ifndef RAGE_SOUND_DRIVER_GYM
#define RAGE_SOUND_DRIVER_GYM

#include <cstdint>
#include <string>

#include "RageSoundDriver.h"

/* No audio device: the hardware position is the gym clock (GymClock.h), and
 * nothing is mixed until the gym loop asks for a step's audio with MixStep.
 * Decoding happens on the calling thread just before each mix, so the samples
 * never depend on how fast another thread happened to run. */
class RageSoundDriver_Gym : public RageSoundDriver {
 public:
  RageSoundDriver_Gym();
  ~RageSoundDriver_Gym();

  std::string Init();
  int64_t GetPosition() const;
  int GetSampleRate() const;

  /* Mix the iFrames frames that start at the clock's current sample into
   * pOut, interleaved stereo. */
  void MixStep(int16_t* pOut, int iFrames);

  /* The driver of this gym process, or nullptr if another one was chosen. */
  static RageSoundDriver_Gym* Get();
};

#endif

# itgmania-gym

[ITGmania](https://www.itgmania.com) (StepMania) as a [Gymnasium](https://gymnasium.farama.org) environment: the real game plays every frame, in lock step. Each `step` advances the game by exactly `1/fps` seconds, presses the dance buttons the action holds, and brings back the rendered frame and the audio that sounds while it is on screen. Nothing in a run depends on wall time, so an episode replays bit for bit from its seed and its actions.

```python
import numpy as np
from itgmania_gym import ItgEnv

env = ItgEnv("StepMania 5/Springtime", "Medium", fps=60, speed="M250", fail="off")
frame, info = env.reset(seed=1)            # info["meta"]: song, chart, options, timing windows
frame, reward, terminated, truncated, info = env.step(np.array([0, 1, 0, 0]))  # hold down
pcm = env.audio()                          # (frames, 2) int16 at env.audio_rate (48000)
env.close()
```

- **Action**: `MultiBinary(4)`, the buttons held, in dance-single's column order (left, down, up, right). A change between two steps reaches the game in the middle of the step, so presses are judged to within half a step; a button held over several steps holds a hold note.
- **Observation**: the frame, `(height, width, 3)` uint8 RGB (`screensize`, default 854x480, the theme's own canvas).
- **Reward**: the change in the game's dance points (ITG scoring). **Terminated**: the stage ended (the song finished, or a fail type that stops it fired).
- **Info**: the game's state after the step: the judgments it just made (column, score, offset, row), the presses it received, scores and combo, life, the song position.

Every `reset` launches a fresh game process (about half a second to the first frame), which is what makes an episode depend on nothing but its seed and its actions.

## The game

The package needs the fork's binary, which has the gym mode built in. In order, it uses: `ItgEnv(binary=...)`; `$ITGMANIA_GYM_BIN`; the build of the checkout the package lives in; the prebuilt pack for its version (macOS arm64, Linux x86_64), fetched once from the `gym-v<version>` release into `~/.cache/itgmania-gym/` (`ITGMANIA_GYM_OFFLINE=1` forbids fetching). It needs a display (on a headless Linux machine, `xvfb-run`); the window is never shown. On a Mac with no GPU (a CI virtual machine) it draws with Apple's software renderer, about seven times slower. On Linux the game uses the system's OpenGL, X11, GTK 3, ALSA and PulseAudio libraries, which a desktop install has; on a minimal Ubuntu: `sudo apt install libgl1 libglu1-mesa libgtk-3-0 libasound2 libpulse0 libxtst6 libxrandr2 libxinerama1`.

To build it from the checkout (submodules included):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## Songs

The three songs of `Songs/StepMania 5` come with the game (CC-NC). `songs_dir=` adds a folder of song groups (`<songs_dir>/<Group>/<Song>/`), and `song="Group/Song"` names one.

## Tests

`pytest` plays the real game: the env checker, the audio of each step, a scripted player that hits every note, failing, and two processes that must produce the same bytes.

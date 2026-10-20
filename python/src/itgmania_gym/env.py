"""ItgEnv: one song of ITGmania as a Gymnasium environment.

Action: ``MultiBinary(4)``, the dance buttons held during the step, in
dance-single's column order -- 0 left, 1 down, 2 up, 3 right. A button pressed
or released between two steps reaches the game in the middle of the step, so
a step of ``1/fps`` seconds judges a press to within ``1/(2 fps)`` of when the
action changed. Holds are held for as long as the button stays set.

Observation: the rendered frame, ``(height, width, 3)`` uint8 RGB.

Reward: the change in the game's own dance points (ITG scoring: a perfect
step adds the most, a miss takes some away), so a whole episode's return is the
chart's actual dance points.

An episode is the stage: it terminates when the game leaves gameplay (the song
ended, or the player failed with a fail type that stops the song). Every
``reset`` launches a fresh game process, so an episode is fully determined by
its seed and its actions.

Sound: :meth:`ItgEnv.audio` is the step's audio, ``(frames, 2)`` int16 at
:attr:`ItgEnv.audio_rate` Hz -- exactly what sounds while the step's frame is on
screen, ``audio_rate / fps`` frames on average.
"""

from __future__ import annotations

import os
from typing import Any, ClassVar

import gymnasium
import numpy as np
from gymnasium import spaces

from .engine import Engine

__all__ = ["FAIL_TYPES", "ItgEnv"]

# The game's fail types, by the mod that selects each.
FAIL_TYPES = {
    "off": "failoff",
    "immediate": "failimmediate",
    "immediate_continue": "failimmediatecontinue",
    "end_of_song": "failatend",
}

_MAX_SEED = 999_999_999


class ItgEnv(gymnasium.Env):
    """One song of ITGmania, played in lock step.

    :param song: the song as ``"Group/Folder"`` under the game's Songs/ (or
        ``songs_dir``), e.g. ``"StepMania 5/Springtime"``.
    :param difficulty: the dance-single chart: Beginner, Easy, Medium, Hard
        or Challenge.
    :param fps: steps per second. The game advances exactly ``1/fps`` seconds
        per step whatever the value, so the song always plays at its speed.
    :param screensize: the frame size, ``"WIDTHxHEIGHT"``.
    :param speed: the scroll speed mod: ``"C400"`` (constant), ``"M250"``
        (maximum), ``"1.5x"`` (multiple of the song's BPM).
    :param fail: one of :data:`FAIL_TYPES`.
    :param music_rate: the song's playback rate (1.0 = as written).
    :param mods: further modifiers, by the game's own names.
    :param theme: the theme that draws the screen.
    :param game_mode: Simply Love's game mode (ITG, FA+, Casual), which sets
        the timing windows and the life bar; ``None`` for another theme.
    :param prefs: further game preferences, by name.
    :param songs_dir: a folder of song groups to add to the game's own.
    :param binary: the ITGmania executable; see :func:`itgmania_gym.find_binary`.
    :param render_mode: ``"rgb_array"`` only.
    """

    metadata: ClassVar[dict] = {"render_modes": ["rgb_array"], "render_fps": 60}

    def __init__(
        self,
        song: str = "StepMania 5/Springtime",
        difficulty: str = "Medium",
        *,
        fps: int = 60,
        screensize: str = "854x480",
        speed: str = "M250",
        fail: str = "off",
        music_rate: float = 1.0,
        mods: tuple[str, ...] | list[str] = (),
        theme: str = "Simply Love",
        game_mode: str | None = "ITG",
        prefs: dict[str, Any] | None = None,
        songs_dir: str | os.PathLike | None = None,
        binary: str | os.PathLike | None = None,
        render_mode: str | None = "rgb_array",
    ):
        if render_mode not in (None, "rgb_array"):
            raise ValueError(f"render_mode {render_mode!r}: only 'rgb_array'")
        if fail not in FAIL_TYPES:
            raise ValueError(f"fail {fail!r}: one of {sorted(FAIL_TYPES)}")
        if music_rate <= 0:
            raise ValueError(f"music_rate {music_rate}: must be positive")
        width, _, height = screensize.partition("x")
        self._size = (int(height), int(width))
        self.render_mode = render_mode
        self.metadata = {**self.metadata, "render_fps": int(fps)}
        self.audio_rate = 48000

        modifiers = [speed, FAIL_TYPES[fail], *mods]
        if music_rate != 1.0:
            modifiers.append(f"{music_rate:g}xmusic")
        self._args = [
            f"--gym-song={song}",
            "--gym-steps-type=dance-single",
            f"--gym-difficulty={difficulty}",
            f"--gym-mods={','.join(modifiers)}",
            f"--gym-fps={int(fps)}",
            f"--gym-screensize={width}x{height}",
            f"--gym-pref=Theme={theme}",
        ]
        if game_mode:
            self._args.append(f"--gym-game-mode={game_mode}")
        for name, value in (prefs or {}).items():
            self._args.append(f"--gym-pref={name}={value}")
        if songs_dir is not None:
            self._args.append(f"--gym-songs-dir={os.fspath(songs_dir)}")
        self._binary = binary

        self.action_space = spaces.MultiBinary(4)
        self.observation_space = spaces.Box(
            0, 255, (self._size[0], self._size[1], 3), np.uint8
        )
        self._engine: Engine | None = None
        self._frame: np.ndarray | None = None
        self._audio = np.zeros((0, 2), np.int16)
        self._state: dict = {}

    def reset(self, *, seed: int | None = None, options: dict | None = None):
        """Launch the game on the stage and return its first frame.

        :param seed: the episode's seed (the game's random numbers).
        :return: ``(frame, info)``; ``info`` is the game's state and, under
            ``"meta"``, what it plays (song, chart, options, timing windows).
        """
        super().reset(seed=seed)
        if options:
            raise ValueError("ItgEnv takes no reset options")
        engine_seed = (
            int(seed) % _MAX_SEED + 1
            if seed is not None
            else int(self.np_random.integers(1, _MAX_SEED + 1))
        )
        self.close()
        self._engine = Engine([*self._args, f"--gym-seed={engine_seed}"], binary=self._binary)
        reply = self._engine.call("reset")
        self._take(reply)
        return self._frame, {**self._state, "meta": self._engine.meta}

    def step(self, action):
        """Hold ``action``'s buttons for one frame.

        :return: ``(frame, reward, terminated, truncated, info)``; ``info`` is
            the game's state after the step.
        """
        if self._engine is None:
            raise RuntimeError("step before reset")
        before = self._state["actual_dance_points"]
        buttons = [int(b) for b in np.asarray(action, dtype=np.int8).reshape(4)]
        reply = self._engine.call("step", buttons=buttons)
        self._take(reply)
        reward = float(self._state["actual_dance_points"] - before)
        return self._frame, reward, bool(self._state["finished"]), False, dict(self._state)

    def render(self):
        """The frame the last reset or step returned."""
        return self._frame

    def audio(self) -> np.ndarray:
        """The audio of the last reset or step: ``(frames, 2)`` int16."""
        return self._audio

    def sample(self) -> dict:
        """The game's state after the last step, for logging."""
        return dict(self._state)

    def close(self):
        if self._engine is not None:
            self._engine.close()
            self._engine = None

    def _take(self, reply) -> None:
        state = reply.data["state"]
        if state["script_errors"]:
            raise RuntimeError("the theme's Lua failed: " + "; ".join(state["script_errors"]))
        self._state = state
        self._frame = reply.frame
        self._audio = reply.audio

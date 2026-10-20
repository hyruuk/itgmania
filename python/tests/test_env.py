"""ItgEnv against the real game: every test launches it."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import pytest
from gymnasium.utils.env_checker import check_env

from itgmania_gym import EngineDied, ItgEnv, find_binary

SONG = "StepMania 5/MechaTribe Assault"


def _song_file(song: str) -> Path:
    _, root = find_binary()
    return next((root / "Songs" / song).glob("*.ssc"))


def _chart(song: str, difficulty: str) -> list[tuple[float, int, str]]:
    """``(beat, column, kind)`` of each note of a dance-single chart."""
    text = _song_file(song).read_text(encoding="utf-8", errors="replace")
    for block in text.split("#NOTEDATA:")[1:]:
        steps_type = re.search(r"#STEPSTYPE:([^;]*);", block).group(1).strip()
        level = re.search(r"#DIFFICULTY:([^;]*);", block).group(1).strip()
        if steps_type != "dance-single" or level.lower() != difficulty.lower():
            continue
        data = re.sub(r"//[^\n]*", "", re.search(r"#NOTES:([^;]*);", block, re.S).group(1))
        notes = []
        for m, measure in enumerate(data.split(",")):
            rows = [r.strip() for r in measure.strip().splitlines() if r.strip()]
            for i, row in enumerate(rows):
                for c, ch in enumerate(row[:4]):
                    kind = {"1": "tap", "2": "hold", "4": "hold", "3": "tail"}.get(ch)
                    if kind:
                        notes.append((4 * m + 4 * i / len(rows), c, kind))
        return notes
    raise KeyError(difficulty)


def test_env_checker():
    env = ItgEnv(SONG)
    try:
        check_env(env, skip_render_check=False)
    finally:
        env.close()


@pytest.mark.parametrize("fps", [60, 100, 120, 144])
def test_each_step_mixes_exactly_its_share_of_audio(fps):
    env = ItgEnv(SONG, fps=fps)
    try:
        env.reset(seed=1)
        frames = [len(env.audio())]
        for _ in range(3 * fps):
            env.step(np.zeros(4, np.int8))
            frames.append(len(env.audio()))
        # n + 1 chunks (the reset's and n steps') cover exactly (n + 1) / fps.
        assert sum(frames) == len(frames) * 48000 // fps
        assert max(frames) - min(frames) <= 1
    finally:
        env.close()


def test_a_scripted_player_hits_every_note_within_half_a_step():
    fps = 60
    env = ItgEnv(SONG, "Medium", fps=fps)
    try:
        _, info = env.reset(seed=1)
        b0, m0 = info["song_beat"], info["music_seconds"]
        _, _, _, _, info = env.step(np.zeros(4, np.int8))
        # Beats per second of music: the beat and the music position are read
        # off the same (sample-quantised) song position, so their ratio is
        # exact where either one per step is not.
        bps = (info["song_beat"] - b0) / (info["music_seconds"] - m0)
        held = defaultdict(lambda: np.zeros(4, np.int8))
        hold_start = {}
        for beat, column, kind in sorted(_chart(SONG, "Medium")):
            step = int((beat - b0) / bps * fps)  # the step whose interval holds the note
            if kind == "tap":
                held[step][column] = 1
            elif kind == "hold":
                hold_start[column] = step
            else:
                for s in range(hold_start.pop(column), step + 1):
                    held[s][column] = 1
        judgments, total, step, terminated = [], 0.0, 1, False
        while not terminated:
            _, reward, terminated, _, info = env.step(held.get(step, np.zeros(4, np.int8)))
            judgments += info["judgments"]
            total += reward
            step += 1
        taps = [j for j in judgments if "offset" in j]
        assert Counter(j["tap_note_score"] for j in taps) == {"W1": 350}
        assert Counter(j["hold_note_score"] for j in judgments if "hold_note_score" in j) == {
            "Held": 50
        }
        assert max(abs(j["offset"]) for j in taps) <= 0.5 / fps
        assert info["percent_dance_points"] == 1.0
        assert total == info["actual_dance_points"]
    finally:
        env.close()


def test_without_presses_failing_ends_the_episode():
    env = ItgEnv(SONG, fail="immediate")
    try:
        env.reset(seed=1)
        for _ in range(60 * 60):
            _, _, terminated, _, info = env.step(np.zeros(4, np.int8))
            if terminated:
                break
        assert terminated and info["failed"]
    finally:
        env.close()


_RUN = """
import hashlib, json, sys
import numpy as np
from itgmania_gym import ItgEnv
env = ItgEnv(sys.argv[1])
frames, sound_and_state = hashlib.sha256(), hashlib.sha256()
obs, info = env.reset(seed=7)
renderer = info["meta"]["renderer"]
rng = np.random.default_rng(3)
for _ in range(900):
    obs, reward, terminated, truncated, info = env.step(rng.integers(0, 2, 4))
    frames.update(obs.tobytes())
    sound_and_state.update(env.audio().tobytes())
    sound_and_state.update(json.dumps(info, sort_keys=True).encode())
env.close()
print(json.dumps({"frames": frames.hexdigest(), "sound_and_state": sound_and_state.hexdigest(),
                  "renderer": renderer}))
"""


@pytest.fixture(scope="module")
def two_runs():
    """The same 900 random steps in two processes, under two hash seeds."""

    def run(hashseed: str) -> dict:
        out = subprocess.run(
            [sys.executable, "-c", _RUN, SONG],
            capture_output=True, text=True, check=True,
            env={**__import__("os").environ, "PYTHONHASHSEED": hashseed},
        )
        return json.loads(out.stdout)

    return run("1"), run("2")


def test_two_processes_play_the_same_sound_and_state(two_runs):
    first, second = two_runs
    assert first["sound_and_state"] == second["sound_and_state"]


def test_two_processes_draw_the_same_frames(two_runs):
    first, second = two_runs
    if first["renderer"] == "Apple Software Renderer":
        pytest.skip("Apple's software renderer (a Mac with no GPU, as on CI) rounds a few "
                    "pixels differently from run to run under load")
    assert first["frames"] == second["frames"]


def test_an_unknown_song_is_refused_with_the_reason():
    env = ItgEnv("StepMania 5/No Such Song")
    with pytest.raises(EngineDied, match="no song"):
        env.reset(seed=1)
    env.close()

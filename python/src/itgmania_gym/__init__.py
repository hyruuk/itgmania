"""ITGmania as a Gymnasium environment.

The game itself plays every frame: this package launches it in its lock-step
gym mode (``ITGMANIA_GYM=1 ITGmania --gym``), where each ``step`` advances it
by exactly one frame, presses the dance buttons the action holds, and brings
back the rendered frame and the audio that sounds while it is on screen. A run
replays bit for bit from its seed and its actions.
"""

from __future__ import annotations

import gymnasium

from .binary import BinaryNotFound, find_binary
from .engine import CommandFailed, Engine, EngineDied, ProtocolError
from .env import FAIL_TYPES, ItgEnv

__version__ = "0.1.0"

__all__ = [
    "BinaryNotFound",
    "CommandFailed",
    "Engine",
    "EngineDied",
    "FAIL_TYPES",
    "ItgEnv",
    "ProtocolError",
    "find_binary",
]

gymnasium.register(id="ITGmania-v0", entry_point="itgmania_gym.env:ItgEnv")

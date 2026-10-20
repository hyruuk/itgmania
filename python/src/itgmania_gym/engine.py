"""One ITGmania gym process and its protocol.

The game answers one JSON object per line on its stdout, each paired with the
request on its stdin by ``id``; a reply that carries a frame or audio is
followed by their raw bytes (``src/gym/GymServer.h`` in the fork). Requests and
replies strictly alternate, so a reply is read before the next request is
written.

One process plays one stage: its song, chart, modifiers and seed are fixed on
the command line, and a new episode is a new process. That is what makes an
episode replay bit for bit from its seed and its actions.
"""

from __future__ import annotations

import json
import os
import select
import shutil
import subprocess
import tempfile
import weakref
from dataclasses import dataclass
from typing import Any

import numpy as np

from .binary import find_binary

__all__ = ["CommandFailed", "Engine", "EngineDied", "ProtocolError", "Reply"]

PROTOCOL_VERSION = 1


class EngineDied(RuntimeError):
    """The game exited or closed its pipe; the message ends with its stderr."""


class ProtocolError(RuntimeError):
    """The game answered something this client cannot read."""


class CommandFailed(RuntimeError):
    """The game refused a command (``{"ok": false}``)."""

    def __init__(self, kind: str, error: str, request: dict):
        super().__init__(f"{request.get('cmd')}: {kind}: {error}")
        self.kind = kind
        self.error = error
        self.request = request


@dataclass(frozen=True)
class Reply:
    """A reply: its JSON, and the frame and audio that followed it, if any.

    :ivar data: the reply's JSON object.
    :ivar frame: ``(height, width, 3)`` uint8 RGB, top row first, or ``None``.
    :ivar audio: ``(frames, 2)`` int16 at 48 kHz, or ``None``.
    """

    data: dict
    frame: np.ndarray | None
    audio: np.ndarray | None


def _shutdown(proc: subprocess.Popen, datadir: str, keep: bool) -> None:
    if proc.poll() is None:
        try:
            proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    if not keep:
        shutil.rmtree(datadir, ignore_errors=True)


class Engine:
    """A running gym process.

    :param args: the stage's ``--gym-*`` arguments (song, chart, modifiers,
        fps, frame size, seed, ...); ``--gym`` and ``--gym-datadir`` are added.
    :param binary: an ITGmania executable; see :func:`find_binary`.
    :param timeout: seconds to wait for any one reply before giving up.
    :param keep_datadir: keep the process's data folder (its Logs/log.txt)
        after it closes; it is always kept when the process died.
    """

    def __init__(
        self,
        args: list[str],
        *,
        binary: str | os.PathLike | None = None,
        timeout: float = 60.0,
        keep_datadir: bool = False,
    ):
        executable, root = find_binary(binary)
        self.datadir = tempfile.mkdtemp(prefix="itgmania-gym-")
        self.stderr_path = os.path.join(self.datadir, "stderr.txt")
        self.timeout = timeout
        self._keep = keep_datadir
        with open(self.stderr_path, "wb") as stderr:
            self._proc = subprocess.Popen(
                [str(executable), "--gym", f"--gym-datadir={self.datadir}", *args],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=stderr,
                env={**os.environ, "ITGMANIA_GYM": "1"},
                cwd=root,
            )
        self._fd = self._proc.stdout.fileno()
        self._buffer = bytearray()
        self._id = 0
        self._finalizer = weakref.finalize(
            self, _shutdown, self._proc, self.datadir, keep_datadir
        )
        self.meta = self.call("hello").data["meta"]
        if self.meta.get("protocol") != PROTOCOL_VERSION:
            self.close()
            raise ProtocolError(
                f"the game speaks protocol {self.meta.get('protocol')}, "
                f"this client {PROTOCOL_VERSION}"
            )

    def call(self, cmd: str, **fields: Any) -> Reply:
        """Send one command and read its reply.

        :raises CommandFailed: the game refused it.
        :raises EngineDied: the game exited or stopped answering.
        """
        self._id += 1
        request = {"id": self._id, "cmd": cmd, **fields}
        try:
            self._proc.stdin.write((json.dumps(request) + "\n").encode())
            self._proc.stdin.flush()
        except (BrokenPipeError, ValueError) as e:
            raise self._died() from e
        try:
            reply = json.loads(self._read_line())
        except json.JSONDecodeError as e:
            raise ProtocolError(f"unreadable reply to {cmd}: {e}") from e
        if reply.get("id") != self._id:
            raise ProtocolError(f"reply id {reply.get('id')} to request {self._id}")
        if not reply.get("ok"):
            raise CommandFailed(reply.get("kind", "?"), reply.get("error", ""), request)
        frame = audio = None
        if "frame" in reply:
            f = reply["frame"]
            if f.get("format") != "rgb8":
                raise ProtocolError(f"unknown frame format {f.get('format')}")
            frame = np.frombuffer(
                self._read_exact(f["width"] * f["height"] * 3), np.uint8
            ).reshape(f["height"], f["width"], 3)
        if "audio" in reply:
            a = reply["audio"]
            if a.get("format") != "s16le" or a.get("channels") != 2:
                raise ProtocolError(f"unknown audio format {a}")
            audio = np.frombuffer(self._read_exact(a["frames"] * 4), "<i2").reshape(-1, 2)
        return Reply(reply, frame, audio)

    def close(self) -> None:
        """Ask the game to exit, and clean up after it."""
        if self._proc.poll() is None:
            try:
                self.call("close")
            except (EngineDied, ProtocolError, CommandFailed):
                pass
        self._finalizer()

    def _read_line(self) -> bytes:
        while True:
            end = self._buffer.find(b"\n")
            if end >= 0:
                line = bytes(self._buffer[:end])
                del self._buffer[: end + 1]
                return line
            self._fill()

    def _read_exact(self, n: int) -> bytearray:
        while len(self._buffer) < n:
            self._fill()
        data = self._buffer[:n]
        del self._buffer[:n]
        return data

    def _fill(self) -> None:
        ready, _, _ = select.select([self._fd], [], [], self.timeout)
        if not ready:
            self._proc.kill()
            raise self._died(f"no reply within {self.timeout:g} s")
        data = os.read(self._fd, 1 << 20)
        if not data:
            raise self._died()
        self._buffer += data

    def _died(self, why: str = "") -> EngineDied:
        status = self._proc.wait()
        self._keep = True
        self._finalizer.detach()
        try:
            with open(self.stderr_path, errors="replace") as f:
                tail = f.read()[-2000:].strip()
        except OSError:
            tail = ""
        message = f"the game exited (status {status})"
        if why:
            message = f"{why}; {message}"
        message += f"; its data folder is {self.datadir}"
        if tail:
            message += f"; stderr:\n{tail}"
        return EngineDied(message)

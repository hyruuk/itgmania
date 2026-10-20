"""Finding the ITGmania binary that plays the game, and the folder it runs in.

The game is the single source of truth for the rules, the timing and the
pictures, so this package is useless without it. A build takes minutes, so it
is never started from here: a checkout's own build is used when the package
lives in one, and otherwise the prebuilt pack for this package version is
fetched once from its GitHub release.

ITGmania runs in place: it reads Data/, Themes/, NoteSkins/, Songs/ and the
rest from the folder its executable lives in (on macOS, the folder that holds
ITGmania.app). A checkout is laid out that way, and so is a pack.
"""

from __future__ import annotations

import hashlib
import os
import platform
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

__all__ = ["BinaryNotFound", "find_binary"]

_ENV_VAR = "ITGMANIA_GYM_BIN"
# Set to a non-empty value to never touch the network.
_OFFLINE_VAR = "ITGMANIA_GYM_OFFLINE"
# The file that identifies a checkout of the fork with gym mode in it.
_MARKER = Path("src") / "gym" / "Gym.h"
_RELEASES = "https://github.com/hyruuk/itgmania/releases/download"
# The pack each platform's release job builds.
_PACKS: dict[tuple[str, str], str] = {
    ("darwin", "arm64"): "ITGmania-gym-macos-arm64.tar.xz",
    ("linux", "x86_64"): "ITGmania-gym-linux-x86_64.tar.xz",
    ("linux", "amd64"): "ITGmania-gym-linux-x86_64.tar.xz",
}


class BinaryNotFound(RuntimeError):
    """No ITGmania binary with gym mode could be found or fetched."""


def _executable(root: Path) -> Path:
    """Where the executable is inside a folder ITGmania runs in."""
    if sys.platform == "darwin":
        return root / "ITGmania.app" / "Contents" / "MacOS" / "ITGmania"
    return root / "itgmania"


def _root_of(executable: Path) -> Path:
    """The folder an executable runs in: its own, or the one holding the .app."""
    if sys.platform == "darwin" and executable.parent.name == "MacOS":
        return executable.parents[3]
    return executable.parent


def _checkout() -> Path | None:
    """The fork checkout this package lives in, if it lives in one."""
    for folder in Path(__file__).resolve().parents:
        if (folder / _MARKER).is_file():
            return folder
    return None


def _cache_dir(version: str) -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(Path.home(), ".cache")
    return Path(base) / "itgmania-gym" / f"gym-v{version}"


def _pack_name() -> str:
    key = (sys.platform, platform.machine().lower())
    if key not in _PACKS:
        raise BinaryNotFound(
            f"no prebuilt ITGmania gym pack for {key[0]} {key[1]}; build the fork "
            f"(github.com/hyruuk/itgmania, branch gym) and set {_ENV_VAR}"
        )
    return _PACKS[key]


def _download(version: str, target: Path) -> None:
    """Fetch this version's pack, check it against SHA256SUMS, unpack it."""
    if os.environ.get(_OFFLINE_VAR):
        raise BinaryNotFound(
            f"no ITGmania gym pack in {target} and {_OFFLINE_VAR} forbids fetching one"
        )
    name = _pack_name()
    base = f"{_RELEASES}/gym-v{version}"
    with urllib.request.urlopen(f"{base}/SHA256SUMS", timeout=60) as response:
        sums = dict(
            reversed(line.split(None, 1))
            for line in response.read().decode().splitlines()
            if line.strip()
        )
    if name not in sums:
        raise BinaryNotFound(f"release gym-v{version} has no {name}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent) as tmp:
        archive = Path(tmp) / name
        digest = hashlib.sha256()
        with urllib.request.urlopen(f"{base}/{name}", timeout=60) as response, open(
            archive, "wb"
        ) as out:
            for chunk in iter(lambda: response.read(1 << 20), b""):
                digest.update(chunk)
                out.write(chunk)
        if digest.hexdigest() != sums[name].strip():
            raise BinaryNotFound(f"{name} does not match its SHA256SUMS entry")
        unpacked = Path(tmp) / "pack"
        with tarfile.open(archive) as tar:
            tar.extractall(unpacked, filter="data")
        os.replace(unpacked, target)


def find_binary(binary: str | os.PathLike | None = None, version: str | None = None):
    """The ITGmania executable to launch, and the folder to launch it in.

    In order: ``binary``; ``$ITGMANIA_GYM_BIN``; the build of the checkout this
    package lives in; the release pack for this package's version, fetched
    into ``~/.cache/itgmania-gym/`` the first time.

    :param binary: path of an ITGmania executable built with gym mode.
    :param version: the pack version to fetch (default: this package's).
    :return: ``(executable, root)`` as :class:`pathlib.Path`.
    :raises BinaryNotFound: nothing found, with what to do about it.
    """
    explicit = binary or os.environ.get(_ENV_VAR)
    if explicit:
        executable = Path(explicit).expanduser().resolve()
        if not executable.is_file():
            raise BinaryNotFound(f"no ITGmania executable at {executable}")
        return executable, _root_of(executable)

    checkout = _checkout()
    if checkout is not None:
        executable = _executable(checkout)
        if not executable.is_file():
            raise BinaryNotFound(
                f"the checkout {checkout} has no build at {executable}; build it with\n"
                "    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
            )
        return executable, checkout

    if version is None:
        from . import __version__ as version
    target = _cache_dir(version)
    if not _executable(target).is_file():
        if target.exists():
            shutil.rmtree(target)
        _download(version, target)
    return _executable(target), target

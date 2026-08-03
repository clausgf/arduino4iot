"""
PlatformIO pre-build script for arduino4iot (wired via library.json ->
build.extraScript).

It derives the *firmware* version and commit from the consuming project's git
repository at build time and injects them as the library-scoped preprocessor
defines IOT_FW_VERSION / IOT_FW_COMMIT, which Iot::getFirmwareVersion() /
getFirmwareCommit() report in system telemetry. This runs in the consumer's
project directory, so git targets the firmware repo (not the framework).

Graceful fallback: without git (e.g. a tarball install) the defines are not set
and the getters return empty / an app-provided override via setFirmwareVersion().
Skip entirely with -DIOT_NO_GIT_VERSION.
"""

import os
import subprocess

Import("env")  # noqa: F821 - provided by PlatformIO


def _has_define(name):
    for define in env.get("CPPDEFINES", []):
        key = define[0] if isinstance(define, (list, tuple)) else define
        if key == name:
            return True
    return False


def _git(args, default=""):
    try:
        return subprocess.check_output(
            ["git"] + args, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return default


if not _has_define("IOT_NO_GIT_VERSION"):
    # Prefer the CI-provided tag (no tags fetch needed), then git, then fallback.
    version = os.environ.get("GITHUB_REF_NAME", "") \
        or _git(["describe", "--tags", "--abbrev=0"]) or "0.0.0"
    version = version.lstrip("v")

    commit = _git(["rev-parse", "--short", "HEAD"], "unknown")
    if _git(["status", "--porcelain"]):
        commit += "-dirty"

    env.Append(CPPDEFINES=[
        ("IOT_FW_VERSION", env.StringifyMacro(version)),
        ("IOT_FW_COMMIT", env.StringifyMacro(commit)),
    ])

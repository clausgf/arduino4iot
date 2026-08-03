"""
PlatformIO pre-build script for arduino4iot (wired via library.json ->
build.extraScript).

It derives the *firmware* version from the consuming project's git repository at
build time and injects it as the library-scoped preprocessor define
IOT_FW_VERSION, which Iot::getFirmwareVersion() reports in system telemetry. git
is run against the project directory ($PROJECT_DIR) - not the process CWD, which
PlatformIO points at the library's own libdeps checkout, so a bare "git" would
report arduino4iot's version instead of the firmware's.

The version is a single `git describe --tags --dirty --always` string:
  - exactly on a tag, clean:  0.10.0
  - N commits past a tag:      0.10.0-3-gcce20b9   (+ "-dirty" if modified)
  - no tag at all:             cce20b9             (+ "-dirty")

Graceful fallback: without git (e.g. a tarball install) the define is not set and
getFirmwareVersion() returns empty / an app-provided override via
setFirmwareVersion(). Skip entirely with -DIOT_NO_GIT_VERSION.
"""

import os
import subprocess

Import("env")  # noqa: F821 - provided by PlatformIO

# The firmware repo, not the process CWD (which is the library's libdeps checkout).
PROJECT_DIR = env.subst("$PROJECT_DIR")


def _has_define(name):
    for define in env.get("CPPDEFINES", []):
        key = define[0] if isinstance(define, (list, tuple)) else define
        if key == name:
            return True
    return False


def _git(args, default=""):
    try:
        return subprocess.check_output(
            ["git", "-C", PROJECT_DIR] + args, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return default


if not _has_define("IOT_NO_GIT_VERSION"):
    # Prefer the CI-provided tag (no tags fetch needed), then git describe, then
    # a fallback. describe yields "<tag>", "<tag>-<n>-g<hash>" or (no tag) the
    # bare hash, each with a "-dirty" suffix when the working tree is modified.
    version = os.environ.get("GITHUB_REF_NAME", "") \
        or _git(["describe", "--tags", "--dirty", "--always"]) or "0.0.0"
    version = version.lstrip("v")

    env.Append(CPPDEFINES=[
        ("IOT_FW_VERSION", env.StringifyMacro(version)),
    ])

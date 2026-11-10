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

Two more defines, both part of Iot::firmwareId():
  - IOT_FW_DATE: the project's last commit date in UTC, "2026-11-10 13:03".
    The commit date rather than the build time, so it changes only with a
    commit and does not force a full rebuild on every build.
  - IOT_LIB_VERSION: `git describe` of arduino4iot itself, e.g. 4.0.0-5-ga57b942,
    when the library is a git checkout of its own (a local clone, or lib_deps
    from a git URL); unset for a registry install, where IOT_VERSION_* says it.

Graceful fallback: without git (e.g. a tarball install) the defines are not set and
firmwareVersion() returns empty / an app-provided override via
setFirmwareVersion(). Skip entirely with -DIOT_NO_GIT_VERSION.
"""

import inspect
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


def _library_dir():
    # SCons executes this file without __file__; the frame knows its path
    try:
        script = inspect.getframeinfo(inspect.currentframe()).filename
        return os.path.dirname(os.path.dirname(os.path.realpath(script)))
    except Exception:
        return os.getcwd()


LIBRARY_DIR = _library_dir()


def _git(args, default="", cwd=PROJECT_DIR):
    try:
        return subprocess.check_output(
            ["git", "-C", cwd] + args, stderr=subprocess.DEVNULL,
            env=dict(os.environ, TZ="UTC"),
        ).decode().strip()
    except Exception:
        return default


def _library_git_version():
    # only if the library is the top of its own repository: a registry install
    # under .pio/libdeps has no .git, and git would find the project's instead
    top = _git(["rev-parse", "--show-toplevel"], cwd=LIBRARY_DIR)
    if not top or os.path.realpath(top) != os.path.realpath(LIBRARY_DIR):
        return ""
    return _git(["describe", "--tags", "--dirty", "--always"], cwd=LIBRARY_DIR).lstrip("v")


if not _has_define("IOT_NO_GIT_VERSION"):
    # Prefer the CI-provided tag (no tags fetch needed), then git describe, then
    # a fallback. describe yields "<tag>", "<tag>-<n>-g<hash>" or (no tag) the
    # bare hash, each with a "-dirty" suffix when the working tree is modified.
    version = os.environ.get("GITHUB_REF_NAME", "") \
        or _git(["describe", "--tags", "--dirty", "--always"]) or "0.0.0"
    version = version.lstrip("v")

    defines = [("IOT_FW_VERSION", env.StringifyMacro(version))]

    date = _git(["log", "-1", "--format=%cd", "--date=format-local:%Y-%m-%d %H:%M"])
    if date:
        defines.append(("IOT_FW_DATE", env.StringifyMacro(date)))

    library_version = _library_git_version()
    if library_version:
        defines.append(("IOT_LIB_VERSION", env.StringifyMacro(library_version)))

    env.Append(CPPDEFINES=defines)

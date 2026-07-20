#!/usr/bin/env bash
# POSIX build of the CLI + unit-test binary (P35). Native bash so cmake's `-D` args pass cleanly —
# pwsh's argument parser splits a version-shaped token (`-DPOLYGLOT_VERSION=0.0.0-dev` became
# `-DPOLYGLOT_VERSION=0` + `.0.0-dev`), so the version define never reached CMake and the binary fell
# back to a bare `git describe` SHA. The Windows build stays scripts/msbuild-solution.ps1 (msbuild).
#
# POLYGLOT_VERSION defaults to the shallow-PR dev placeholder: actions/checkout has no tags, so CMake's
# `git describe` fallback would inject a commit SHA and fail the version-shape unit test. Override by
# exporting POLYGLOT_VERSION (release.yml passes the real tag).
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

version="${POLYGLOT_VERSION:-0.0.0-dev}"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DPOLYGLOT_VERSION=${version}"
cmake --build build -j"$(nproc)"

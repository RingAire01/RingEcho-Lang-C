#!/usr/bin/env bash
set -euo pipefail

# Pin both the release and its upstream asset digest.
version=20260616
archive="llvm-mingw-${version}-ucrt-ubuntu-22.04-x86_64.tar.xz"
checksum=534b92e067b22a6b4441f48ae9240a3341b17825d04d577eab0cf85c44b4deda
: "${RUNNER_TEMP:?RUNNER_TEMP must be set}"
: "${GITHUB_PATH:?GITHUB_PATH must be set}"
tool_dir="$(mktemp -d "${RUNNER_TEMP}/ringecho-toolchain.XXXXXX")"
curl --fail --location --retry 3 --connect-timeout 30 --max-time 300 \
  "https://github.com/mstorsjo/llvm-mingw/releases/download/${version}/${archive}" \
  --output "${tool_dir}/${archive}"
printf '%s  %s\n' "$checksum" "${tool_dir}/${archive}" | sha256sum --check --strict
tar -xf "${tool_dir}/${archive}" -C "$tool_dir"
printf '%s\n' "${tool_dir}/${archive%.tar.xz}/bin" >> "$GITHUB_PATH"

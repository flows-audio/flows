#!/usr/bin/env bash
set -euo pipefail
# The browser build is tested with this exact Emscripten release.
version=6.0.13
commit=35659964bc2762fffec0d257997fb61475f765b7
destination="${1:-$RUNNER_TEMP/emsdk}"
git clone --branch "$version" --depth 1 https://github.com/emscripten-core/emsdk.git "$destination"
test "$(git -C "$destination" rev-parse HEAD)" = "$commit"
"$destination/emsdk" install "$version"
"$destination/emsdk" activate "$version"
# Make emcc/emcmake available to later workflow steps.
# shellcheck disable=SC1091
source "$destination/emsdk_env.sh" >/dev/null
{
  echo "EMSDK=$EMSDK"
  echo "EMSCRIPTEN_ROOT=$destination/upstream/emscripten"
} >>"$GITHUB_ENV"
{
  echo "$destination"
  echo "$destination/upstream/emscripten"
} >>"$GITHUB_PATH"

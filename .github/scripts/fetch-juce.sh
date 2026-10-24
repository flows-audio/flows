#!/usr/bin/env bash
set -euo pipefail
# Wrapper adapters and source archives are tied to this exact JUCE release.
git clone --branch 8.0.7 --depth 1 https://github.com/juce-framework/JUCE.git ../JUCE
test "$(git -C ../JUCE rev-parse HEAD)" = "10a589619b452c261b2940767eb253171eb5a823"

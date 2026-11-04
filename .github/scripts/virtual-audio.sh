#!/usr/bin/env bash
set -euo pipefail
# Hosted runners have no sound card. Without an output device Chromium reports
# a running AudioContext that renders nothing and Firefox never starts one, so
# the browser tests could not hear the audio worklet. A PulseAudio null sink
# gives every browser a real output to render into.
sudo apt-get update -qq
sudo apt-get install -y -qq --no-install-recommends pulseaudio pulseaudio-utils >/dev/null
pulseaudio --start --exit-idle-time=-1
pactl load-module module-null-sink sink_name=flows_ci >/dev/null
pactl set-default-sink flows_ci
pactl info | grep -i '^Default Sink'

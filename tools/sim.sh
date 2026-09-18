#!/bin/sh
# Simulate the video path (rtl/bennugd_ctl.sv + rtl/bennugd_video.sv) with iverilog.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/bennugd_tb_video
iverilog -g2012 -o "$OUT" "$HERE/rtl/bennugd_ctl.sv" "$HERE/rtl/bennugd_video.sv" "$HERE/rtl/tb/tb_video.sv"
vvp -n "$OUT"

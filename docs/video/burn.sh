#!/usr/bin/env bash
# burn.sh <input video> [subs.srt] [output.mp4]
#   Shrinks the recording a little and adds a black strip at the bottom, then burns the
#   subtitles into that strip, so they never cover the terminal (the TUI status bar is on the
#   last line). Output is 1920x1080 H.264, silent.
set -euo pipefail
in=${1:?usage: burn.sh <input video> [subs.srt] [output.mp4]}
srt=${2:-$(dirname "$0")/subs.srt}
out=${3:-${in%.*}_subs.mp4}

# video area 1920x930, subtitle strip 150 px. ASS units for SRT: PlayResY 288 -> 1 unit = 3.75 px
vf="scale=1920:930:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(930-ih)/2:black"
vf+=",subtitles='${srt}':force_style='FontName=Noto Sans CJK TC,FontSize=13,Bold=1,"
vf+="PrimaryColour=&H00FFFFFF,OutlineColour=&H00000000,BorderStyle=1,Outline=1,Shadow=0,"
vf+="Alignment=2,MarginV=4'"

ffmpeg -hide_banner -loglevel warning -stats -y -i "$in" -vf "$vf" \
    -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -an "$out"
echo "wrote $out"

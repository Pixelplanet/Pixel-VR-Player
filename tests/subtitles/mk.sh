#!/bin/bash
# Test files for the subtitle work: a short clip with subtitle files next to it, a Matroska file
# with the same tracks inside (and a font attached), an MP4 with a text track, a Latin-1 .srt.
set -e
cd "$(dirname "$0")"
ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=24 -f lavfi -i sine=frequency=440 -t 60 \
    -c:v libx264 -preset ultrafast -g 48 -pix_fmt yuv420p -c:a aac clip.mkv 2>/dev/null ||
ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=24 -f lavfi -i sine=frequency=440 -t 60 \
    -c:v mpeg4 -g 48 -c:a aac clip.mkv
iconv -f UTF-8 -t CP1252 > clip.latin.srt <<'EOF'
1
00:00:01,000 --> 00:00:30,000
Größe: äöü ß – “quotes” … €
EOF
ffmpeg -v error -y -i clip.mkv -i clip.multi.srt -i clip.styled.ass -attach /usr/share/fonts/noto/NotoSerif-Italic.ttf \
    -map 0:v -map 0:a -map 1 -map 2 -c copy -c:s:0 srt -c:s:1 ass -metadata:s:t mimetype=font/ttf inside.mkv
ffmpeg -v error -y -i clip.mkv -i clip.multi.srt -map 0:v -map 0:a -map 1 -c copy -c:s mov_text inside.mp4
ls -la

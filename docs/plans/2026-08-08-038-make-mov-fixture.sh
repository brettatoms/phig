#!/bin/sh
# Regenerates tests/data/live_photo.mov — a 1-second clip carrying the same
# QuickTime key a real Live Photo's video does. Synthetic on purpose: real Live
# Photos are personal media and must not be committed as fixtures.
#
# -movflags use_metadata_tags is required. Without it ffmpeg silently drops
# arbitrary QuickTime keys and the fixture tests nothing at all.
set -e
out="$(dirname "$0")/../../tests/data/live_photo.mov"
ffmpeg -f lavfi -i testsrc=size=64x48:rate=10 -t 1 -c:v libx264 -pix_fmt yuv420p \
  -movflags use_metadata_tags \
  -metadata com.apple.quicktime.content.identifier=AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE \
  -y "$out"
echo "wrote $out"

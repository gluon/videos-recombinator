#!/bin/sh
# Integration test: generates short movies with ffmpeg, renders plans with vrc, and checks
# with ffprobe that every output file holds exactly as many frames as the plan announced
# by `vrc --dry-run` for the same arguments and seed.
#
# usage: test_render.sh <vrc> <test_io> <ffmpeg> <ffprobe> <work-dir>
# A missing tool is a failure, never a skip.

set -eu

VRC=$1
TEST_IO=$2
FFMPEG=$3
FFPROBE=$4
WORK=$5

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

for tool in "$VRC" "$TEST_IO" "$FFMPEG" "$FFPROBE"; do
    [ -x "$tool" ] || fail "$tool is not an executable, the integration test cannot run"
done

rm -rf "$WORK"
mkdir -p "$WORK/out" "$WORK/scaling" "$WORK/outpath"
cd "$WORK"

gen() {
    "$FFMPEG" -nostdin -v error -y "$@" || fail "ffmpeg could not generate $*"
}

# a: long GOP h264 with B-frames, so frames need a seek and a decode forward.
gen -f lavfi -i testsrc2=size=320x240:rate=25 -frames:v 60 \
    -c:v libx264 -g 12 -bf 2 -pix_fmt yuv420p a.mov
# b: intra ProRes at 30000/1001, a different size and aspect ratio from a.
gen -f lavfi -i testsrc=size=256x256:rate=30000/1001 -frames:v 45 \
    -c:v prores_ks -profile:v 2 b.mov
# c: open GOP h264, leading B-frames reference the previous GOP.
gen -f lavfi -i testsrc2=size=320x180:rate=24 -frames:v 50 \
    -c:v libx264 -x264-params keyint=10:min-keyint=10:open-gop=1:bframes=3 \
    -pix_fmt yuv420p c.mov
# d: full range mjpeg.
gen -f lavfi -i testsrc=size=160x120:rate=25 -frames:v 30 \
    -c:v mjpeg -pix_fmt yuvj420p d.mov

"$TEST_IO" decode a.mov 60 320 240 25 || fail "decode a.mov"
"$TEST_IO" decode b.mov 45 256 256 29.970029970029970 || fail "decode b.mov"
"$TEST_IO" decode c.mov 50 320 180 24 || fail "decode c.mov"
"$TEST_IO" decode d.mov 30 160 120 25 || fail "decode d.mov"
"$TEST_IO" cache a.mov || fail "cache"
"$TEST_IO" scaling "$WORK/scaling" || fail "scaling"
"$TEST_IO" outpath "$WORK/outpath" || fail "outpath"

field() {  # field <name> <vrc output>: second column of the line starting with <name>
    echo "$2" | awk -v k="$1" '$1 == k { print $2 }'
}

count_frames() {
    "$FFPROBE" -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 "$1"
}

dims() {
    "$FFPROBE" -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0:s=x "$1"
}

expected_index=0

# render <expected WxH> <vrc args...>: dry-run, render, compare.
render() {
    want_dims=$1
    shift
    dry=$("$VRC" --dry-run "$@") || fail "vrc --dry-run $*"
    planned=$(field frames "$dry")
    digest=$(field plan "$dry")
    [ -n "$planned" ] || fail "no frames line in: $dry"
    [ -n "$digest" ] || fail "no plan line in: $dry"

    out=$("$VRC" --out "$WORK/out" "$@") || fail "vrc --out $*"
    [ "$(field frames "$out")" = "$planned" ] || fail "render and dry-run counts differ for $*"
    [ "$(field plan "$out")" = "$digest" ] || fail "render and dry-run timelines differ for $*"
    file=$(field wrote "$out")
    expected=$(printf '%s/out/vid%03d.mov' "$WORK" "$expected_index")
    [ "$file" = "$expected" ] || fail "wrote $file, expected $expected"
    expected_index=$((expected_index + 1))

    got=$(count_frames "$file")
    got_dims=$(dims "$file")
    echo "$* -> $file: plan $planned frames, ffprobe $got frames, $got_dims"
    [ "$got" = "$planned" ] || fail "$file has $got frames, the plan has $planned"
    [ "$got_dims" = "$want_dims" ] || fail "$file is $got_dims, expected $want_dims"
}

for codec in prores422 prores4444 h264 mjpeg; do
    render 320x240 --seed 42 --segments 5 --black 7 --black-run 3 --codec "$codec" a.mov b.mov
done
render 640x360 --seed 7 --segments 9 --black 20 --black-run 5 --allow-adjacent \
    --codec h264 --width 640 --height 360 --fps 30 a.mov b.mov c.mov d.mov

# The end criterion as stated: no seed given, so vrc draws one and reports it; a dry run
# with that seed must announce the frame count the file holds.
#
# The frame count does not depend on the seed, so it alone would pass even if the seed were
# mangled on its way back in. The plan digest is what actually proves the round trip:
# entropy_seed() sets the top bit about half the time, and a seed read with strtoll instead
# of strtoull saturates there, changing almost every slot while keeping the count.
out=$("$VRC" --out "$WORK/out" a.mov b.mov) || fail "vrc --out a.mov b.mov"
seed=$(field seed "$out")
file=$(field wrote "$out")
digest=$(field plan "$out")
dry=$("$VRC" --dry-run --seed "$seed" a.mov b.mov) || fail "vrc --dry-run --seed $seed"
planned=$(field frames "$dry")
got=$(count_frames "$file")
echo "drawn seed $seed -> $file: plan $planned frames, ffprobe $got frames, digest $digest"
[ "$got" = "$planned" ] || fail "$file has $got frames, the plan for seed $seed has $planned"
[ "$(field plan "$dry")" = "$digest" ] \
    || fail "seed $seed does not replay: digest $(field plan "$dry") instead of $digest"

# The high half of the seed space must survive the command line, since that is where half
# the drawn seeds live.
refuses() {  # refuses <args...>: the command must exit non-zero
    if "$VRC" "$@" >/dev/null 2>&1; then
        fail "vrc accepted $*"
    fi
}

big=18446744073709551615
d1=$(field plan "$("$VRC" --dry-run --seed "$big" a.mov b.mov)")
d2=$(field plan "$("$VRC" --dry-run --seed 9223372036854775807 a.mov b.mov)")
[ -n "$d1" ] || fail "no plan line for seed $big"
[ "$d1" != "$d2" ] || fail "--seed $big was truncated to INT64_MAX: both give digest $d1"

refuses --dry-run --seed 18446744073709551616 a.mov b.mov   # above 2^64-1
refuses --dry-run --seed -1 a.mov b.mov
refuses --dry-run --scaling squash a.mov b.mov
refuses --seed 1 a.mov                                      # a render with no --out

echo "render: ok"

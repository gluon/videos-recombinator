#!/bin/sh
# Checks what vrc's prepass measures against what ffprobe reads, file by file.
#
# This is verification step 3 of CLAUDE.md, the one generated test movies cannot cover.
# The prepass counts video packets the demuxer does not flag as discarded and takes the
# mean rate over the measured span. That is exact on constant-rate material and is the
# first thing to doubt on real camera or editor output: an edit list that trims content
# the demuxer still delivers would be counted long, and a variable-rate file gets re-timed
# to its own mean.
#
# usage: tools/check-sources.sh <path-to-vrc> <movie-or-directory>...
#
# Exits non-zero if any file disagrees. ffprobe -count_frames decodes the whole file, so
# this is slow on long ProRes: it is the only count worth trusting.

set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <path-to-vrc> <movie-or-directory>..." >&2
    exit 2
fi

VRC=$1
shift
[ -x "$VRC" ] || { echo "$VRC is not executable" >&2; exit 2; }
command -v ffprobe >/dev/null || { echo "ffprobe is not in the PATH" >&2; exit 2; }

# Collect the files to look at: a directory contributes the movies directly inside it.
files=""
for arg in "$@"; do
    if [ -d "$arg" ]; then
        for f in "$arg"/*.mov "$arg"/*.MOV "$arg"/*.mp4 "$arg"/*.MP4 "$arg"/*.mxf "$arg"/*.MXF; do
            [ -f "$f" ] && files="$files
$f"
        done
    elif [ -f "$arg" ]; then
        files="$files
$arg"
    else
        echo "not a file or directory: $arg" >&2
        exit 2
    fi
done
[ -n "$files" ] || { echo "no movie found" >&2; exit 2; }

probe() {  # probe <file> <entry>
    ffprobe -v error -select_streams v:0 -show_entries "stream=$2" -of csv=p=0 "$1"
}

problems=0

printf '%s\n' "$files" | while IFS= read -r f; do
    [ -n "$f" ] || continue

    name=$(basename "$f")
    echo "--- $name"

    size=$(probe "$f" width,height | tr ',' 'x')
    avg=$(probe "$f" avg_frame_rate)
    rbase=$(probe "$f" r_frame_rate)
    container=$(probe "$f" nb_frames)

    # The truth: every frame actually decoded.
    counted=$(ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 "$f")

    # vrc's own measurement. One segment and no black frames, so `frames` is exactly the
    # frame count the prepass found.
    if ! out=$("$VRC" --dry-run --segments 1 --black 0 "$f" 2>&1); then
        echo "    vrc could not open it:"
        echo "$out" | sed 's/^/      /'
        problems=$((problems + 1))
        echo "$problems" > /tmp/vrc_check_problems.$$
        continue
    fi
    vrc_frames=$(echo "$out" | awk '$1 == "frames" { print $2 }')

    echo "    size            $size"
    echo "    ffprobe counted $counted frames"
    echo "    vrc  prepass    $vrc_frames frames"
    echo "    container says  ${container:-n/a}"
    echo "    rate            avg $avg, base $rbase"

    if [ "$counted" != "$vrc_frames" ]; then
        echo "    MISMATCH: the prepass disagrees with the decoder by $((vrc_frames - counted)) frames."
        echo "              Do not render from this file until that is understood."
        problems=$((problems + 1))
        echo "$problems" > /tmp/vrc_check_problems.$$
    elif [ "$avg" != "$rbase" ]; then
        echo "    variable rate: avg and base differ, so the output is re-timed to the mean."
    else
        echo "    ok"
    fi
done

# The loop runs in a subshell, so its count comes back through a file.
if [ -f /tmp/vrc_check_problems.$$ ]; then
    n=$(cat /tmp/vrc_check_problems.$$)
    rm -f /tmp/vrc_check_problems.$$
    echo
    echo "$n file(s) to look into before rendering."
    exit 1
fi

echo
echo "every file agrees with the decoder."

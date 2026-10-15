# videos-recombinator

C++ port of `videosRecombinator`, a Max/MSP/Jitter patch that cuts two movies into
segments, shuffles the segments across both sources, sprinkles black frames into them,
and renders the result to a movie file.

- `docs/MAX_SOURCE_AUDIT.md`: what the original patch does, read from the files, including
  the seven places where its behaviour and its stated intent disagree.
- `docs/SPEC.md`: what this program does, including which of those seven are corrected and
  how.

## State

| Part | State |
| --- | --- |
| `src/core` planner | implemented, tested |
| `src/io` decode, cache, encode | implemented, tested |
| `src/cli` vrc | plans and renders |
| `src/app` vrc-ui | the seven steps, previews, threaded render; logic tested, drawing checked by eye only |

`ctest` runs three tests. `core` and `app` need nothing but a compiler: the first covers the
planner, the second drives the whole interface with fake decoders, so the seven steps, the
seed bookkeeping, the previews and the render thread are exercised with no window, no GPU
and no movie files. `render` generates movies with ffmpeg, renders them with `vrc` and
counts the result with ffprobe; a missing ffmpeg or ffprobe makes it fail rather than skip.

What no test covers is the drawing itself, `src/app/ui_draw.cpp` and the sokol_app shell.
That needs a pair of eyes.

## Using it

```
vrc --dry-run --seed 42 --segments 8 --black 40 --black-run 5 a.mov b.mov
vrc --seed 42 --segments 8 --black 40 --black-run 5 --out OUT a.mov b.mov
```

`--dry-run` plans without decoding anything. The `plan` line it prints is a fingerprint of
the whole timeline: two runs printing the same `plan` render the same frames in the same
order. The `frames` count does not depend on the seed, so it cannot tell two seeds apart,
which is why the digest exists.

`--out DIR` is required to render. There is no default directory on purpose: these files
are large.

`--scaling fit|stretch|fill` decides what happens to a source whose size differs from the
output. `fit` keeps the aspect ratio and pads with opaque black, `fill` crops to cover,
`stretch` ignores the aspect ratio.

`--reorder-seed N` keeps the cuts a `--seed` produced and redraws only the order the
segments play in. It is the shell equivalent of the interface's "new order, same cuts"
button, and it is why a state reached by clicking in the interface can be reproduced from a
script: the interface prints the exact `vrc` line for whatever is on screen.

## The interface

```
./build/vrc-ui
```

The seven numbered steps of the original patch, in its order: the source folder and its
extension filter, one movie per voice, the cut parameters, CALCULATE, a second button that
redraws the order while keeping the cuts, the output settings, and RENDER with a progress
bar and a cancel button. The right half shows a preview of each voice and of the output,
each with a slider: the output slider scrubs through the plan, frame by frame, before
anything is written.

What it adds to the patch:

- Both seeds are on screen, with the plan digest, so a result worth keeping can be found
  again. The patch's `Math.random()` could not be replayed.
- The equivalent `vrc` command line, with a copy button: explore by clicking, then batch
  from the shell.
- More than two voices, up to eight. The patch was limited to two because it encoded the
  voice in the sign of the frame number.
- The predicted frame count covers every voice, not just the first.
- A render runs in its own thread with its own decoders, so the previews stay live and the
  window keeps responding while a file is being written.

## Before rendering from real footage

```
tools/check-sources.sh build/vrc ~/path/to/rushes
```

For every movie it finds, this compares what `vrc`'s prepass measured against a full
`ffprobe -count_frames`, and reports variable-rate files. It exits non-zero if any file
disagrees. Worth running once per batch of new material: the prepass is exact on
constant-rate footage and approximate on files carrying an edit list, which only real
camera and editor output exercises.

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Dependencies: ffmpeg development packages for `vrc`, fetched sokol headers for `vrc-ui`.
Neither is needed for the core tests:

```
cmake -B build -DVRC_BUILD_CLI=OFF -DVRC_BUILD_UI=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS: `brew install ffmpeg pkg-config cmake`.

## Verified toolchain

Configured, built and tested on macOS with AppleClang 17.0.0, CMake, and ffmpeg from
Homebrew: libavformat 62.12.101, libavcodec 62.28.101, libavutil 60.26.101,
libswscale 9.5.101. Both `ctest` tests pass there. Also built and tested on Linux with
GCC 13 against ffmpeg 8.1.3 compiled from source, libavcodec 62.28.103. `src/io` targets
that API generation: `avcodec_send_packet` / `avcodec_receive_frame`. The pre-59 decode
calls do not exist there.

## ffmpeg behaviour worth knowing before changing src/io

Two traps cost a debugging session each. Both are handled in the code; do not undo them.

**Seeking a mov by dts lands one GOP early.** The mov demuxer indexes its seeks on a clock
that does not match the packet dts when B-frames are present: asking for dts 12288 landed
at 6144, so every random access decoded a superfluous GOP. `Decoder::seek_to` therefore
tries the keyframe pts first, then its dts, then the start of the stream, and verifies
where it actually landed. Measured after the fix: 383 seeks out of 383 land on the
intended keyframe.

**libavcodec always decodes mjpeg as BT.601 full range.** Whatever matrix the container
declares, the JFIF path wins, so encoding mjpeg with BT.709 coefficients shifts the colours
of every frame in every ffmpeg-based player (red came back as 232 instead of 255). The
mjpeg encoder is therefore set to BT.601 full range and tagged to match. The other codecs
stay BT.709.

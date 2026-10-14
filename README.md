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
| `src/app` vrc-ui | **window only**, no preview, no parameters |

`ctest` runs two tests: `core`, which needs nothing but a compiler, and `render`, which
generates movies with ffmpeg, renders them with `vrc` and counts the result with ffprobe.
A missing ffmpeg or ffprobe makes `render` fail rather than skip.

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

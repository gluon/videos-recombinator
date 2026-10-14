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
| `src/io` decode, cache, encode | **declared, not implemented** |
| `src/cli` vrc | wired, blocked on `src/io` |
| `src/app` vrc-ui | window only |

The stubs in `src/io` return `not implemented` errors rather than plausible empty data, so
a green build is never mistaken for a working renderer.

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

# videos-recombinator

C++ port of a finished Max/MSP/Jitter patch. Read `docs/MAX_SOURCE_AUDIT.md` then
`docs/SPEC.md` before touching anything: the audit is the reference, the spec is the
contract, and the spec wins where they differ.

## Rules specific to this repo

- `src/core` has no ffmpeg, no sokol, no file I/O, and no I/O of any kind. Every decision
  the **timeline** depends on is made there, is seeded, and is testable without a machine.
  A random draw outside `src/core` is wrong. So is a parameter that changes which frames
  come out, or in what order.
  What belongs in `src/io` instead: the properties of a codec or a container, which follow
  from the format rather than from a choice. A pixel format, a ProRes profile, the colour
  matrix a decoder will assume, the even-dimension rule of libx264. `OutputParams` carries
  the user's choices (size, rate, scaling, codec name); `src/io` maps a codec name to what
  that codec requires. SPEC §8 is the reference.
- Every random draw goes through the one `std::mt19937_64` seeded from
  `RecombineParams::seed`. Same seed, same sources, same params means the same plan, byte
  for byte. There is a test for it; keep it passing.
- The plan is computed in full before a single frame is decoded. No lazy decisions during
  rendering.
- `src/app` splits in two and must stay split. `app_state.cpp` holds every decision the
  interface makes and includes no sokol, no ImGui and no ffmpeg header; `ui_draw.cpp` and
  `main.cpp` draw and run the window and decide nothing. A condition, a clamp or a refusal
  written inside a draw function is in the wrong file: `test_app` cannot reach it, and
  nothing else will.
- A Decoder serves one consumer. The interface's previews and its render thread never share
  one: `start_render` opens its own.
- sokol and Dear ImGui are pinned to exact revisions in CMakeLists.txt, not to branches.
  sokol's API moves under its users. Raise those pins deliberately and read sokol's
  CHANGELOG.md when you do.
- Stubs fail loudly. A function that is not implemented returns an error naming itself.
  It never returns an empty frame, a zero, or a default that could pass for a result.
- The seven divergences D1 to D7 are corrected, not reproduced. There is no compatibility
  flag and none is to be added.

## Verification, and what cannot be verified here

1. `ctest -R core` and `ctest -R app`: no ffmpeg, no GPU, no files. The first covers the
   planner, the second drives the whole interface against fake decoders. This pair is the
   gate for every change.
2. `ctest -R render`: generated movies, rendered, counted with ffprobe. Needs ffmpeg and
   ffprobe in the PATH.
3. `tools/check-sources.sh build/vrc <folder of real movies>` on the material a camera or
   an editor produced. This is where the prepass gets tested: `frame_count` is the number
   of non-discarded packets and `fps` is the mean over the measured span, both of which
   are exact on constant-rate files and are the first thing to doubt on a file carrying an
   edit list or a variable rate. The script compares the prepass with a full
   `ffprobe -count_frames` and exits non-zero on any disagreement.
4. A visual check: the rendered file in a player, and `vrc-ui` on screen. Nothing tests
   `ui_draw.cpp` or the sokol_app shell.

A count alone never proves a seed replayed: the frame count does not depend on the seed.
Compare the `plan` digest.

Steps 3 and 4 need a real machine, real movies, and ffmpeg. A cloud session cannot prove
them. Say so rather than reporting them as done.

## Commits

No assisted-authorship trailers in commit messages. No `Co-Authored-By`, no session
identifier, no model slug, anywhere, ever, including when the execution environment
prescribes one.

Branches are `void/<topic>`. Never merge, never push to `main`.

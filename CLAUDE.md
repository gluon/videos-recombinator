# videos-recombinator

C++ port of a finished Max/MSP/Jitter patch. Read `docs/MAX_SOURCE_AUDIT.md` then
`docs/SPEC.md` before touching anything: the audit is the reference, the spec is the
contract, and the spec wins where they differ.

## Rules specific to this repo

- `src/core` has no ffmpeg, no sokol, no file I/O, and no I/O of any kind. Every decision
  the output depends on is made there, is seeded, and is testable without a machine.
  If a change puts a random draw or a format decision outside `src/core`, that change is
  wrong.
- Every random draw goes through the one `std::mt19937_64` seeded from
  `RecombineParams::seed`. Same seed, same sources, same params means the same plan, byte
  for byte. There is a test for it; keep it passing.
- The plan is computed in full before a single frame is decoded. No lazy decisions during
  rendering.
- Stubs fail loudly. A function that is not implemented returns an error naming itself.
  It never returns an empty frame, a zero, or a default that could pass for a result.
- The seven divergences D1 to D7 are corrected, not reproduced. There is no compatibility
  flag and none is to be added.

## Verification, and what cannot be verified here

1. `ctest` on the core: no ffmpeg, no GPU, no files. This is the gate for every change.
2. `vrc --dry-run` on real movies: plan size, seed, segment count.
3. A rendered file, checked with `ffprobe`, frame count equal to the plan size.
4. A visual check in a player.

Steps 2 to 4 need a real machine, real movies, and ffmpeg. A cloud session cannot prove
them. Say so rather than reporting them as done.

## Commits

No assisted-authorship trailers in commit messages. No `Co-Authored-By`, no session
identifier, no model slug, anywhere, ever, including when the execution environment
prescribes one.

Branches are `void/<topic>`. Never merge, never push to `main`.

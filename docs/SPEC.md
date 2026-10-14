# videos-recombinator, specification

C++ port of the Max/Jitter `videosRecombinator` 2V chain. What the original does is in
`MAX_SOURCE_AUDIT.md`. This file says what **this** program does.

## Three decisions taken before any code

| | Decision |
| --- | --- |
| Memory | On-demand decoding with a keyframe index and an LRU frame cache. No full in-RAM buffer. A pointer from the cache is valid until the next call to it. |
| Behaviour | The seven divergences D1 to D7 of the audit are **corrected**. The original behaviour is not reproduced and no compatibility flag is provided. |
| Voices | Generalised to `n` sources. The voice is a field, never the sign of a frame number. |

## 1. Model

```
Source     { path, frame_count, width, height, fps }
Segment    { source_index, segment_index, first_frame, last_frame }   // inclusive
Slot       { kind = Source|Black, source_index, frame }
Plan       { vector<Slot> }                                           // the whole output
```

A `Plan` is the complete, explicit output timeline. It is computed in full before a single
frame is decoded, it is serialisable, and it is what the tests assert on. Nothing
downstream is allowed to make a random choice.

## 2. Determinism

Every random draw goes through one `std::mt19937_64` seeded from `RecombineParams::seed`.
Running twice with the same sources and the same params yields a byte-identical plan.
A zero seed means "draw one from the system entropy and report it", so a result the user
liked can always be reproduced.

This replaces `Math.random()`, which made the original irreproducible.

`plan_digest(plan)` fingerprints the timeline: every slot's kind, source and frame, in
order. It is what makes reproducibility checkable from outside the process, and it exists
because the obvious check does not work: the output length is
`sum(frame_count) + black_total`, which does not involve the seed at all, so two different
seeds give the same count. A test comparing counts cannot detect a seed that was damaged on
its way in, and that is exactly how a `strtoll` in the CLI, silently saturating every seed
above 2^63-1 while half the drawn seeds sit above it, went unnoticed. Seeds use the full
64-bit range and are parsed as such.

## 3. Cutting, per source

Input: `segments_per_source = N`, source frame count `F`.

- Draw `N - 1` **distinct** cut points uniformly from `[1, F - 1]` (sampling without
  replacement, the role `bigUrn.js` played).
- Boundaries are `0, c_1 … c_{N-1}, F`.
- Segment `i` is `[b_i, b_{i+1} - 1]`, inclusive on both ends.

Consequences, all deliberate:

- Fixes **D3**: the final boundary is `F`, not `F - 1`, so the last frame of every source
  is emitted. The union of the segments is exactly `[0, F-1]`, each frame once.
- Fixes **D7**: cut points are drawn from `[1, F-1]` and are distinct, so no segment is
  empty. A one-frame segment is still possible and is legal.
- `N` is clamped to `[1, F]`. Asking for more segments than frames is an error, reported,
  not silently truncated.

## 4. Black frames

Input: `black_total`, `black_run_max >= 1`.

While black frames remain to place:

1. `run = uniform(1, min(black_run_max, remaining))`
2. pick a segment uniformly among all segments of all sources
3. pick an insertion position uniformly in `[0, segment_length]`
4. insert the `run` black slots **contiguously** at that position

Fixes **D1**: a run is contiguous. The original spliced each black frame at an
independent index, which scattered them.

Black insertion positions are held as `(segment, offset, run_length)` records attached to
the segment, so that re-ordering the segments (section 5) moves their black frames with
them. Only a full recalculation redraws them.

A black frame is **opaque** black: RGB zero, alpha 255. Zeroed RGBA would be transparent
in any output format that carries an alpha channel, prores4444 among them, and a
transparent frame is not a black frame. The same opaque black pads a `Fit` output.

## 5. Segment ordering

The pool is every segment of every source: `n_sources * N` entries. It is shuffled with
Fisher-Yates over the session RNG.

When `avoid_adjacent_segments` is set, the order must contain no pair where
`b.source == a.source && b.segment_index == a.segment_index + 1`, that is, no two
temporally contiguous pieces of the same source played back to back.

Algorithm: shuffle, then a repair pass that rescans from the modified position after each
swap, then **verify**. Up to `kOrderAttempts = 64` reshuffles. If no valid order is found,
the function returns an error naming the constraint; it never returns a silently invalid
order.

Fixes **D2**: the original swept forward once, tested one direction only, and could
reintroduce a violating pair behind itself. It returned that order as if it were valid.

A plan can be re-ordered without recutting (`recalculate` in the original) or recomputed
from scratch.

## 6. Output format

Explicit parameters, defaulted from source 0 and overridable:

| Parameter | Default |
| --- | --- |
| `out_width`, `out_height` | source 0 dimensions |
| `out_fps` | source 0 fps |
| `scaling` | `Fit` (preserve aspect ratio, pad with black) |

`scaling` is `Stretch`, `Fit` or `Fill`. Fixes **D5**: sources of differing resolution are
resampled under a policy the user chose and that is recorded in the run manifest.
Fixes **D6**: the output frame rate is one named value, not whichever `r frameRate`
message arrived last.

Predicted output length is `sum(F_s for all sources) + black_total`, exactly, and it is
asserted against the plan size. Fixes **D4**.

## 7. Decoding and cache

Per source: open once with libavformat, build an index of packet PTS and keyframe
positions in a prepass, keep the dimensions, frame count and fps from that prepass rather
than trusting container metadata.

What the prepass measures, and its limits. `frame_count` is the number of video packets the
demuxer does not flag as discarded, `fps` is the mean rate over the span between the first
and the last pts, and the dimensions come from decoding frame 0. This is exact for
constant-rate material. Two cases are known to be approximations and are not silently
hidden: a variable-rate source is re-timed to its own mean rate, and a file whose edit list
trims content the demuxer still delivers would be counted long. A frame decoded at a pts
the prepass did not index is a hard error naming the file, so the failure is loud rather
than a run of wrong frames. Real camera and editor output is the material that tests this;
generated test movies do not.

`FrameCache` is an LRU of decoded RGBA frames, sized by a byte budget
(`--cache-mb`, default 1024). A miss seeks to the keyframe at or before the target and
decodes forward. Because the plan is known in full before rendering, the renderer walks it
in order and can prefetch, so the pathological case is a plan that alternates sources
every frame; that is accepted, it is what the patch is for.

A black slot allocates nothing. One zeroed frame is kept and reused.

## 8. Encoding

libavcodec, muxed by libavformat. Codec is a named parameter mapping to the original four:
`prores422` (`prores_ks`, profile 2), `prores4444` (`prores_ks`, profile 4), `h264`
(`libx264`), `mjpeg`. Output path is `<dir>/vid%03d.mov`, the original's naming, with the
counter resolved at start by scanning for the first free index rather than kept in a UI
widget. The output directory has no default: a render without one is refused.

A codec name selects the pixel format, the profile, the colour matrix and the dimension
constraints that codec requires. Those are properties of the format, not choices, so they
live in `src/io` and not in `OutputParams`. YUV output is BT.709, except mjpeg, which is
BT.601 full range because libavcodec decodes every JPEG that way whatever the container
declares. `h264` currently takes the libx264 defaults; no rate or quality parameter is
exposed yet.

Encoding is not real time and is not clocked by anything. No `delay 8`, no gate: the
renderer is a loop over the plan.

## 9. Binaries

- `vrc`, headless CLI. Does the whole job: scan, plan, render. This is the testable
  product and the one that must exist first.
- `vrc-ui`, sokol application. Preview of each source and of the output, parameters,
  plan inspector, render trigger. It calls into the same core and owns no logic.

The split is hard: `src/core` and `src/io` have no sokol dependency, `src/app` has no
algorithm. A plan computed by the UI and a plan computed by the CLI from the same seed and
params are identical, and there is a test for it.

## 10. Layout

```
src/core/     rng, types, cuts, plan, ordering      pure, no I/O, no ffmpeg
src/io/       decoder, frame_cache, encoder          ffmpeg
src/cli/      vrc
src/app/      vrc-ui                                 sokol
tests/        core tests, runnable without ffmpeg
```

## 11. Verification, in order

1. `ctest -R core`: cut coverage, black-run contiguity, ordering constraint, predicted
   length, seed reproducibility, digest behaviour. No ffmpeg, no GPU, no files.
2. `ctest -R render`: movies generated with ffmpeg, rendered by `vrc`, counted with
   `ffprobe`, and the `plan` digest compared between the dry run and the render so that the
   seed round trip is actually tested and not just the length.
3. `vrc --dry-run` on real camera or editor output, checking `frames` against what the
   files really hold. This is what tests the prepass, section 7.
4. A visual check in a player: the output alternates the sources and shows black runs where
   the plan says.

Steps 3 and 4 need a real machine with ffmpeg and real source movies. They cannot be
proved in a cloud session and must not be reported as proved there.

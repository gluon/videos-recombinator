# Audit of the original Max/Jitter project

Source material: `videosRecombinator.maxproj` (Max 8.6.2), authored 05/2024 by Julien Bayle
for a private commission. Standalone bundle id `com.structure-void.videorecombinator2V`.

This document records **what the patch does**, read from the files. It is the reference
the C++ port is checked against. `SPEC.md` records **what the port does**, including the
places where it deliberately differs.

## Files actually in the chain

The project ships three top patchers and five JS files. Only one chain was audited in
full, because only one is `toplevel`:

| File | Status |
| --- | --- |
| `main_2V_black.maxpat` | audited line by line, `toplevel: 1`, the two-source chain |
| `calculateCuts_black_NEW.js` | audited, instantiated twice (`js … 1` and `js … -1`) |
| `bigUrn.js` | audited, instantiated once per source inside `p prepareAllFrames` |
| `shuffleTheTwo.js` | audited, instantiated once inside the merge subpatcher |
| `main_1V.maxpat`, `main_1V_black.maxpat` | **not audited**, not supplied |
| `calculateCuts.js`, `calculateCuts_black.js` | **not referenced** by any object in `main_2V_black.maxpat`; earlier revisions carried by the 1V patchers |

Everything below is about the 2V chain only.

## Subsystems

### 1. Source acquisition, two voices

`opendialog fold` picks a folder. A `umenu` with `autopopulate 1` lists it, filtered by
an extension `umenu` (`.mov`, `.jpg`, `.mp4`, `ALL`; `ALL` sends a bare `types` message,
clearing the filter). The selection is sent as `read <file>` to
`jit.movie @vol 0. @unique 1 @autostart 0 @engine viddll` inside `p videoSource`.

On `read … 1`, a `t b getframecount getmovie_dim getfps b` fires. The results are routed
and broadcast as `frameNumber1/2`, `dimension1/2`, `frameRate1/2`. Note `framecount`
passes through `- 1` before being sent, so `frameNumberN` is the **last frame index**,
not the count. A `p uiLed` subpatch shows red while loading and flips green after
`del 7000`.

### 2. Full in-RAM buffering

`framedump` on `jit.movie` emits every frame. Inside `p videoBuffer` a `counter` indexes
them into `jit.matrixset` configured with `matrixcount <frameNumberN>` and
`dim <dimensionN>`. Playback is `outputmatrix <index>`. The entire movie lives in RAM.
A `led` lights on buffer completion and a comment counts buffered frames.

### 3. Cut points: `bigUrn.js` inside `p prepareAllFrames`

The subpatcher inlet carries the value of the "CHOOSE SEQ NUMB" number box, call it `N`.
It goes through `- 1` into a `uzi`, so the left inlet of `js bigUrn.js` is banged `N - 1`
times. The right inlet receives `frameNumberN` and rebuilds a shuffled pool
`[0 … frameNumberN-1]`, from which each bang draws one value without replacement.

The `N - 1` draws go through `list.group`, `list.sort 1`, `prepend 0` and `append 0`,
where the trailing `0` is overwritten by `frameNumberN` arriving at the right inlet of
`append` from a `t i i`.

Result: `N + 1` sorted boundaries, from 0 to the last frame index, hence `N` segments.
The same `N` is sent to the JS as `nSeq`, and the merge subpatcher later asks
`shuffleTheTwo.js` for a permutation of `2N`.

### 4. Segment expansion and black frames: `calculateCuts_black_NEW.js`

`listCuts(b0, b1, … bk)` builds one sub-list per consecutive boundary pair, each holding
`z` for `z` in `[b[i], b[i+1])`.

The object argument `minusFlag` (`1` for voice 1, `-1` for voice 2) multiplies every
index, so the **voice is encoded in the sign of the frame number**. Index 0 cannot carry
a sign, so it is emitted as the symbol `zero` or `-zero`.

`nBlackFrames(total, maxConsecutive)` sets the black-frame budget. The insertion loop
picks a random segment, computes `min(maxConsecutive, remaining)`, and inserts that many
`n` symbols **each at an independent random position** in that segment.

Each segment is written to `coll cuts1` / `coll cuts2` as `<index> <frame> <frame> …`.

### 5. Merge and segment ordering

The dump subpatcher (`p`, obj-225) reads `coll cuts1` into `coll cuts3` keeping indices
`0…N-1`, then `coll cuts2` into `coll cuts3` with a `counter` + `+ 1` offset giving
`N…2N-1`. `js shuffleTheTwo.js` is then asked for
`calculateTheSequenceOfSequences(2N)`, which returns a permutation of `0…2N-1`.
That permutation drives `coll cuts3`, and `list.group @zlmaxsize 32767` concatenates
every segment into one flat frame list.

Two separate entry points exist: a full recalculation (new cuts and new order) and
`recalculate`, which keeps the segments and redraws only the order.

### 6. Playback and dispatch

The flat list feeds `list.queue @zlmaxsize 32767`, pulled one element at a time through a
`gate`. `p DISPATCHER` does `route zero -zero n -n`, then `if $i1 > 0 then $i1 else out2 $i1`
with an `abs` on the negative branch. Routing:

| token | destination |
| --- | --- |
| `zero` | player 1, frame 0 |
| `-zero` | player 2, frame 0 |
| `n` | black frame |
| positive int | player 1, that frame |
| negative int | player 2, `abs` of that frame |

`sel n` upstream of each player turns the black token into a bare `jit.matrix` (zeroed),
which is what produces the black frame.

### 7. Recording

`jit.matrix 4 char 1280 720 @adapt 0` is resized by `prepend dim` fed from `dimension1`,
and feeds `jit.record @realtime 0`. Filename comes from
`sprintf symout %svid%.3d.mov` with a `counter` that has a manual reset button; the
output folder is a second `opendialog fold`. The codec `umenu` offers `prores422`,
`prores4442`, `h264`, `jpeg` and is appended to the `write` message.

Pacing is self-clocked: each matrix written to `jit.record` triggers `delay 8` then
`deferlow`, which reopens the `gate` for the next queue element. End of queue (`sel 0` on
the `list.queue` right outlet) sends `stop`, and a `route write` lights the
"file written" led.

### 8. Interface

Presentation mode, steps numbered 1 to 7 via `live.text`. Three `jit.pwindow` previews
(voice 1, voice 2, output). Counters for total frames, predicted frames, buffered frames.
`p displayHandler` guards the combination readout with `if $i1 <= 145 then bang else
out2 bang` and a `"too big for displaying..."` message. A global `RESET ALL` broadcasts
`resetALL`, which clears every `coll`.

## Divergences between stated intent and observed behaviour

Found by reading, not by running. Each one is resolved explicitly in `SPEC.md`.

| # | Where | Observed |
| --- | --- | --- |
| D1 | `calculateCuts_black_NEW.js`, insertion loop | `maxConsecutiveMinusOnes` never produces a consecutive run. Every `n` of a batch is spliced at its own independent random index, so the batch scatters through the segment. The name and the behaviour disagree. |
| D2 | `getRandomNonConsecutivePermutation` | Does not guarantee non-consecutive values. The repair pass tests only `result[k] === result[k+1] - 1`, in a single forward sweep, and a swap can recreate a violating pair at `k-1`. |
| D3 | `listCuts` plus the boundary list | The final boundary is `frameNumber` which is already `framecount - 1`, and sub-lists are half-open. The last frame of each source is never emitted. |
| D4 | "total frames predicted" | Computed as `frameNumber1 + blackFrames`. Voice 2 is not counted, although the 2V output contains both voices. |
| D5 | Output matrix | Forced to `dimension1`. If the two movies differ in resolution, voice 2 is implicitly resampled with no stated policy. |
| D6 | `write` message | `frameRate1` and `frameRate2` both land on the same `prepend set`. Last value received wins, so the output frame rate depends on load order. |
| D7 | `bigUrn.js` draw range | The pool is `[0 … frameNumber-1]`, so `0` can be drawn as a cut point and duplicate the leading boundary, producing an empty segment. Likewise two adjacent draws produce a one-frame segment. Neither is excluded. |

# Vector lineart benchmark

`KomaLineartBenchmark` measures the interactive work behind vector lineart on a
dense layer: a 2400x1600 px image at 300 ppi with a vector layer holding 220
crossing lines, each rendered with a round brush and a pressure taper, built
the way `KomaLineartRecorder` builds lines.

- **Ink stroke:** 640 input samples recorded over the dense layer; each event
  is followed by the live preview frame the canvas paints (at 100% and 400%
  zoom), clipped to the rect the event updated.
- **Eraser** (touched area, up to intersection, whole line): 400 samples
  crossing about a hundred lines, with the eraser overlay frame after each
  event, and the geometry computed when the pen is lifted.
- **Control point drag** and **Adjust Line Width:** a continuous drag of 240
  edits at 120 Hz with the event loop running, reporting the time each edit
  takes, the interval between the updates the layer's projection receives
  (the frame rate while dragging) and how long the oldest change not on
  screen yet waits for the update that shows it; then 60 single edits, each timed until the
  projection shows it; then 60 edits whose dirty-region repaint is timed
  alone, with the largest share of the layer one edit marks dirty.
- **Layer repaint:** the whole vector layer regenerated with cold and warm
  render caches, and the lines painted directly at 1x and 4x the image
  resolution into a 1920x1080 view (the canvas shows the layer's projection
  at any zoom; this is the cost of rendering the lines at a larger scale).

Run it from the build directory:

```bash
QT_QPA_PLATFORM=offscreen ./bin/KomaLineartBenchmark denseInkStroke denseEraser denseControlPointDrag denseAdjustWidth denseLayerRepaint
```

Targets: per-input-event work p95 under 8 ms and max under 16 ms; the
dirty-region repaint after each event p95 under 16 ms.

## Baseline

Commit 8e56e39 (before the lineart optimizations), RelWithDebInfo build,
Intel Core i5-10310U (4 cores, 8 threads), Qt 6.11, offscreen. Milliseconds.

| Measurement | p50 | p95 | max |
| --- | ---: | ---: | ---: |
| Ink stroke over 220 lines (640 samples): per input event | 0.061 | 0.099 | 0.302 |
| Ink stroke over 220 lines: preview frame at 100% | 0.008 | 0.011 | 0.053 |
| Ink stroke over 220 lines: preview frame at 400% | 0.101 | 0.149 | 0.271 |
| Eraser touched-area over 220 lines (400 samples): per input event | 0.056 | 0.101 | 0.131 |
| Eraser touched-area over 220 lines (400 samples): overlay frame at 100% | 0.859 | 1.321 | 1.586 |
| Eraser touched-area over 220 lines (400 samples): overlay frame at 400% | 0.705 | 1.095 | 1.403 |
| Eraser touched-area over 220 lines (400 samples): release geometry | 4.170 | 4.213 | 4.224 |
| Eraser up-to-intersection over 220 lines (400 samples): per input event | 0.056 | 0.099 | 0.135 |
| Eraser up-to-intersection over 220 lines (400 samples): overlay frame at 100% | 0.870 | 1.284 | 1.624 |
| Eraser up-to-intersection over 220 lines (400 samples): overlay frame at 400% | 0.712 | 1.099 | 1.319 |
| Eraser up-to-intersection over 220 lines (400 samples): release geometry | 210.184 | 211.350 | 211.813 |
| Eraser whole-line over 220 lines (400 samples): per input event | 0.051 | 0.099 | 0.134 |
| Eraser whole-line over 220 lines (400 samples): overlay frame at 100% | 6.433 | 14.303 | 18.511 |
| Eraser whole-line over 220 lines (400 samples): overlay frame at 400% | 3.669 | 7.988 | 11.112 |
| Control point drag over 220 lines: per input event | 1.075 | 1.197 | 1.901 |
| Control point drag over 220 lines: event to layer projection updated (120 Hz input) | 50.007 | 50.985 | 51.543 |
| Control point drag over 220 lines: dirty-region layer repaint | 18.505 | 19.278 | 23.094 |
| Adjust Line Width over 220 lines: per input event | 2.953 | 3.913 | 4.768 |
| Adjust Line Width over 220 lines: event to layer projection updated (120 Hz input) | 50.054 | 89.329 | 125.365 |
| Adjust Line Width over 220 lines: dirty-region layer repaint | 64.341 | 89.800 | 114.997 |
| Full vector layer repaint, 220 lines, cold render cache | 1538.408 | 1538.408 | 1559.639 |
| Full vector layer repaint, 220 lines, warm render cache | 61.338 | 61.338 | 62.536 |
| Lines painted directly at 100% of image resolution into a 1920x1080 view, first pass | 15.896 | 17.148 | 17.219 |
| Lines painted directly at 100% of image resolution into a 1920x1080 view, repeated | 16.231 | 17.829 | 18.078 |
| Lines painted directly at 400% of image resolution into a 1920x1080 view, first pass | 202.633 | 828.226 | 926.848 |
| Lines painted directly at 400% of image resolution into a 1920x1080 view, repeated | 203.560 | 820.232 | 935.229 |

Already within the targets: every per-input-event cost, the ink preview and
the touched-area and up-to-intersection eraser overlays. Outside them: the
dirty-region repaint after a control-point drag or a width adjustment, the
event-to-projection latency (the vector layer's 25 ms update compressor and a
second repaint per edit), the whole-line eraser overlay at 100% (max over
16 ms), and the up-to-intersection release (211 ms when the pen is lifted).

### Editing, measured during a continuous drag

The editing measurements above waited for each edit to reach the projection
before making the next, which hides how the vector layer limits its update
rate while input keeps arriving. They now drag continuously at 120 Hz and
report the interval between projection updates. Re-measured on 5130d01
(before the optimizations):

| Measurement | p50 | p95 | max |
| --- | ---: | ---: | ---: |
| Control point drag over 220 lines: per input event | 0.091 | 1.856 | 4.771 |
| Control point drag over 220 lines: projection update interval during a 120 Hz drag | 16.495 | 25.423 | 52.533 |
| Control point drag over 220 lines: single edit to projection updated | 44.479 | 63.974 | 69.311 |
| Control point drag over 220 lines: dirty-region layer repaint | 20.236 | 22.975 | 30.889 |
| Adjust Line Width over 220 lines: per input event | 1.552 | 5.872 | 22.910 |
| Adjust Line Width over 220 lines: projection update interval during a 120 Hz drag | 23.188 | 68.648 | 116.952 |
| Adjust Line Width over 220 lines: single edit to projection updated | 45.983 | 80.578 | 220.810 |
| Adjust Line Width over 220 lines: dirty-region layer repaint | 21.470 | 71.094 | 73.489 |

Largest share of the layer one edit marks dirty: 6.1% (control point), 34.0%
(adjust width).

After d44719b (each edit repainted once):

| Measurement | p50 | p95 | max |
| --- | ---: | ---: | ---: |
| Control point drag over 220 lines: per input event | 0.078 | 0.113 | 0.154 |
| Control point drag over 220 lines: projection update interval during a 120 Hz drag | 16.494 | 22.235 | 26.037 |
| Control point drag over 220 lines: single edit to projection updated | 22.109 | 43.459 | 49.898 |
| Control point drag over 220 lines: dirty-region layer repaint | 16.715 | 17.406 | 31.160 |
| Adjust Line Width over 220 lines: per input event | 1.194 | 2.243 | 4.093 |
| Adjust Line Width over 220 lines: projection update interval during a 120 Hz drag | 19.012 | 62.089 | 105.653 |
| Adjust Line Width over 220 lines: single edit to projection updated | 33.629 | 64.841 | 65.459 |
| Adjust Line Width over 220 lines: dirty-region layer repaint | 19.966 | 59.172 | 61.444 |

Largest share of the layer one edit marks dirty: 3.0% (control point), 32.9%
(adjust width).

Requesting the repaint right after each Correct Line edit, instead of letting
the layer's update compressor schedule it, was measured on top of d44719b and
left out: with the repaint slower than the input, it added up to 20 ms of
GUI-thread work per edit and made Adjust Line Width frames slower (interval
p50 39 ms instead of 19 ms).

## Results

Commit 3a0a757, same machine and build type. Milliseconds.

| Measurement | p50 | p95 | max |
| --- | ---: | ---: | ---: |
| Ink stroke over 220 lines (640 samples): per input event | 0.063 | 0.107 | 0.268 |
| Ink stroke over 220 lines: preview frame at 100% | 0.009 | 0.012 | 0.063 |
| Ink stroke over 220 lines: preview frame at 400% | 0.098 | 0.151 | 0.232 |
| Eraser touched-area over 220 lines (400 samples): per input event | 0.057 | 0.103 | 0.135 |
| Eraser touched-area over 220 lines (400 samples): overlay frame at 100% | 0.864 | 1.321 | 1.642 |
| Eraser touched-area over 220 lines (400 samples): overlay frame at 400% | 0.700 | 1.088 | 1.387 |
| Eraser touched-area over 220 lines (400 samples): release geometry | 4.299 | 4.359 | 4.450 |
| Eraser up-to-intersection over 220 lines (400 samples): per input event | 0.060 | 0.108 | 0.139 |
| Eraser up-to-intersection over 220 lines (400 samples): overlay frame at 100% | 0.844 | 1.311 | 1.618 |
| Eraser up-to-intersection over 220 lines (400 samples): overlay frame at 400% | 0.710 | 1.061 | 1.144 |
| Eraser up-to-intersection over 220 lines (400 samples): release geometry | 24.695 | 24.795 | 25.701 |
| Eraser whole-line over 220 lines (400 samples): per input event | 0.054 | 0.106 | 0.152 |
| Eraser whole-line over 220 lines (400 samples): overlay frame at 100% | 1.722 | 6.689 | 17.116 |
| Eraser whole-line over 220 lines (400 samples): overlay frame at 400% | 2.199 | 6.245 | 10.940 |
| Control point drag over 220 lines: per input event | 0.103 | 0.159 | 0.256 |
| Control point drag over 220 lines: projection update interval during a 120 Hz drag | 8.318 | 10.763 | 12.129 |
| Control point drag over 220 lines: oldest change to projection updated during a 120 Hz drag | 6.426 | 7.978 | 12.062 |
| Control point drag over 220 lines: single edit to projection updated | 7.129 | 7.742 | 8.074 |
| Control point drag over 220 lines: dirty-region layer repaint | 7.115 | 7.541 | 8.089 |
| Adjust Line Width over 220 lines: per input event | 1.581 | 3.691 | 5.258 |
| Adjust Line Width over 220 lines: projection update interval during a 120 Hz drag | 9.481 | 19.571 | 85.783 |
| Adjust Line Width over 220 lines: oldest change to projection updated during a 120 Hz drag | 5.690 | 13.829 | 28.072 |
| Adjust Line Width over 220 lines: single edit to projection updated | 8.951 | 12.930 | 14.599 |
| Adjust Line Width over 220 lines: dirty-region layer repaint | 7.407 | 9.396 | 10.267 |
| Full vector layer repaint, 220 lines, cold render cache | 433.303 | 433.303 | 436.286 |
| Full vector layer repaint, 220 lines, warm render cache | 32.442 | 32.442 | 33.164 |
| Lines painted directly at 100% of image resolution into a 1920x1080 view, first pass | 16.839 | 18.312 | 18.514 |
| Lines painted directly at 100% of image resolution into a 1920x1080 view, repeated | 17.080 | 18.449 | 18.488 |
| Lines painted directly at 400% of image resolution into a 1920x1080 view, first pass | 145.192 | 624.888 | 704.588 |
| Lines painted directly at 400% of image resolution into a 1920x1080 view, repeated | 144.380 | 624.716 | 701.861 |

Largest share of the layer one edit marks dirty: 3.0% (control point), 4.5%
(adjust width).

Every per-input-event cost is under 8 ms at p95 (the slowest, Adjust Line
Width, 3.7 ms) and under 16 ms at max. The dirty-region repaint after an edit
is under 16 ms at p95 and at max (7.5 ms for a control point, 9.4 ms for a
width change), and while dragging, the oldest change not on screen yet is
shown within 8.0 ms (control point) and 13.8 ms (Adjust Line Width) at p95.
That wait was first measured on ed8c43a, before rendering an edited line's
pieces in parallel: about 15 and 19 ms. Since the baseline, the
dirty-region repaint p95 fell from 23.0 to 7.5 ms (control point) and from
71.1 to 9.4 ms (adjust width). The
whole-line eraser overlay fell from 14.3 to 6.7 ms at p95, the
up-to-intersection release from 211 to 25 ms, and the cold layer repaint from
1538 to 433 ms.

The projection update interval while adjusting a width still has a long
tail (max 86 ms): those are stretches where the brush passes between lines,
changes nothing and no frame is due, which the wait measurement leaves out.
A change that lands just after a repaint started waits for that repaint and
its own, which is the wait's max (28 ms in this run). Painting the lines
directly at 400% is limited by the render cache size; the canvas never
takes that path, as it shows the layer's projection at any zoom.

While a Correct Line drag is in progress the edited lines are rendered in
pieces at the brush's pixel grid; the exact full render replaces them when
the drag ends, so the finished result and the saved vector data are the same
as before.

Checked in the running app with the komapaint MCP server (1920x1080 window,
mouse and pen strokes with pressure, all three eraser modes, control point
drags and Adjust Line Width on 220 lines), against the same scenario run on
the build before the optimizations:

- The saved vector data is the same: every saved line is byte-identical,
  and with the lines put in the reference file's order the whole SVG is
  byte-identical. Only the order differs, which varies between runs of
  the old build too (the layer saves its shapes in pointer order), with the
  `shapeN` ids numbered in that order and the random layer uuids.
- Opening the saved files and rendering them gives pixel-identical images
  once the lines are in the same order; in save order, overlapping
  anti-aliased lines round differently by one channel step on 39 pixels.
- The live canvas matches within that rounding, except for 9 pixels near a
  dragged control point where the old build left a faint grey remnant of the
  line's previous position (a missed repaint); a fresh render of the old
  build's own file has no remnant there, and the new build shows it the same
  as the fresh render.
- The logs are identical apart from the MCP bridge port.

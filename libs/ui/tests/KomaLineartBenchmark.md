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
  takes and the interval between the updates the layer's projection receives
  (the frame rate while dragging); then 60 single edits, each timed until the
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

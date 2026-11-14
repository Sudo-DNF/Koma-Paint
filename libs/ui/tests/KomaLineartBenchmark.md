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
- **Control point drag** and **Adjust Line Width:** 240 edits at a 120 Hz
  input cadence with the event loop running. Reported per event: the edit
  itself, the time until the layer's projection shows it, and (60 more edits)
  the cost of repainting the layer's dirty region alone, plus the largest
  share of the layer one event marked dirty.
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

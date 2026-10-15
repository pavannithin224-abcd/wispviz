# Add your own animation

A style is one function: it reads `bands[0..nbars-1]` (each 0..1, already smoothed) and paints into the pixel buffer.

1. **Write it** in `src/main.cpp`, next to `sCapsule`:

```cpp
static void sDots(uint32_t c) {                 // c = current colour 0xRRGGBB
    float pad = 10;
    for (int b = 0; b < nbars; b++) {
        float cx = pad + b * 10 + 4, cy = H / 2.f - (bands[b] - .5f) * (H - 12);
        fillRound(cx, cy, 3, 3, 3, c, .95f);     // cx, cy, half-w, half-h, corner radius, colour, alpha
    }
}
```
Helpers: `fillRound` (rounded rectangle / circle), `fillSeg` (thick line with round caps), `fillTri`. Use `WW` (not `W`) for the wave area width, since the song panel may sit beside it. `peaks[b]` holds a slowly falling peak per bar. `kick` (0..1) spikes on every bass hit: multiply sizes by `(1 + kick * .25f)` to make your style punch.

2. **Register it**: add the name to `STYN[]`, the function to `STYLES[]` (and bump both array sizes), give it a size in `geometry()`, and add one menu line in `showMenu()` (the loop over `STYN` already does this).

3. `make`, run, right-click, *Animation*, pick yours.

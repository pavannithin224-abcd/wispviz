// wispviz - tiny audio visualiser for Windows.
//
// HOW IT WORKS (read top to bottom):
//  1. We make a small borderless "layered" window. A layered window lets us supply our OWN
//     pixels *with per-pixel alpha* (UpdateLayeredWindow), so soft edges / glass look right.
//  2. We ask DWM (the Windows compositor) to blur whatever is behind the window (acrylic).
//     That is the "macOS glass" look: blurred backdrop + a faint white tint + a thin highlight.
//  3. Every ~33 ms we render the bars into a plain memory buffer (software rasteriser using
//     signed-distance maths - no GDI+/D2D libs needed, so the exe stays tiny) and push it.
//  4. Bar heights come from `bands[]`. In step 1 they are fake sine waves; in step 2 the
//     audio module will fill the same array from real sound.  Drawing never changes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>        // SHAppBarMessage: is the taskbar auto-hidden?
#include <shlobj.h>          // Start Menu shortcut (IShellLink) + SHCreateDirectoryEx
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initguid.h>
#include <mmdeviceapi.h>   // pick the audio device
#include <audioclient.h>   // read audio from it
#include "media.h"           // song title / thumbnail / transport controls (media.cpp)


// COM identifiers (fixed values published by Microsoft; this compiler's libs don't ship them)
DEFINE_GUID(W_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(W_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(W_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(W_IID_IAudioCaptureClient,  0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);

// ---- settings: all saved to wispviz.ini next to the exe, edited from the right-click menu ----
static int style = 0, nbars = 14, colorIdx = 0, cycleSec = 0, onTop = 1, posX = -1, posY = -1, showInfo = 0, delayMs = 0;
static int sizeV = 14;                                    // the Size slider, 6..50 (14 = original size)
static float SC = 1.f;                                    // scale factor = sizeV / 14. EVERYTHING (wave, art, text, buttons) is drawn in
                                                          // 'logical' units and multiplied by SC at the lowest drawing level.
static int LWW = 170, LOX = 0, LH = 40;                   // logical: wave width, wave x-offset (song panel sits left of it), total height
static HWND gMain;
static const int PW = 246, PH = 52;                       // song panel width / minimum height
static const int MINB = 6, MAXB = 48;                    // hard limits for the bar count
static const int NCOL = 10, NSTY = 11;
static const uint32_t PAL[NCOL] = {0xE6A8F2, 0x7AD7FF, 0x8CF0C0, 0xF5C26B, 0xF2F2F2, 0xFF7A6B, 0xA78BFA, 0xB6F04A, 0x4DA3FF, 0xFF3B5C};
static const char* PALN[NCOL] = {"Pink", "Cyan", "Mint", "Amber", "White", "Coral", "Violet", "Lime", "Sky", "Crimson"};
static const char* STYN[NSTY] = {"Capsule", "Bars", "Mirror", "Wave", "Radial", "Pulse", "Ripple", "Aurora", "Dots", "Burst", "Strings"};
static const char* ENN[3] = {"Calm", "Normal", "Punchy"};
// ---- SCREEN MODE: the visualiser becomes a full-width strip standing on the bottom edge of the screen ----
static int screenMode = 0, screenLook = 0, screenH = 16, screenOp = 35, balanceIdx = 1, screenTop = 0, nbarsUser = 14;
static int savedX = -1, savedY = -1, forceX = 0, forceY = 0, forcePos = 0;
static const char* LOOKN[2] = {"Classic bars", "Thin needles"};
static const char* BALN[3] = {"Natural", "Balanced", "Even"};
static const float BALV[3] = {0.f, .65f, .95f};           // 0 = one shared loudness (bass dominates), 1 = every band normalised on its own
static int autoStyle = 0;                                 // 1 = pick a new animation every time a new song starts
static int energy = 2;                                    // 0 calm, 1 normal, 2 punchy (default)
static float kick = 0.f;                                  // 0..1, jumps to 1 on every bass hit then decays: styles use it to "punch"

static int W = 170, H = 40;                              // window size (derived from style + bar count)
static FILE* dump = 0;                                   // debug: run with -d to log bands to wispviz_log.txt
static float bands[256], peaks[256];                       // bands = bar heights 0..1, peaks = falling caps
static uint32_t* px;                                     // our pixel buffer (premultiplied ARGB)
static HDC memdc; static HBITMAP dib;
static char iniPath[MAX_PATH];
static bool dirty = true;                                // something changed -> redraw even if silent

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void loadCfg() {
    GetModuleFileNameA(0, iniPath, MAX_PATH); strcpy(strrchr(iniPath, '.'), ".ini");
    style = clampi(GetPrivateProfileIntA("wispviz", "style", 0, iniPath), 0, NSTY - 1);
    energy = clampi(GetPrivateProfileIntA("wispviz", "energy", 2, iniPath), 0, 2);
    autoStyle = GetPrivateProfileIntA("wispviz", "style_per_song", 0, iniPath) != 0;
    screenMode = GetPrivateProfileIntA("wispviz", "screen_mode", 0, iniPath) != 0;
    screenLook = clampi(GetPrivateProfileIntA("wispviz", "screen_look", 0, iniPath), 0, 1);
    screenH = clampi(GetPrivateProfileIntA("wispviz", "screen_height", 16, iniPath), 8, 60);
    screenOp = clampi(GetPrivateProfileIntA("wispviz", "screen_opacity", 35, iniPath), 8, 100);
    balanceIdx = clampi(GetPrivateProfileIntA("wispviz", "balance", 1, iniPath), 0, 2);
    screenTop = GetPrivateProfileIntA("wispviz", "screen_on_top", 0, iniPath) != 0;
    nbars = clampi(GetPrivateProfileIntA("wispviz", "bars", 14, iniPath), MINB, MAXB);
    colorIdx = clampi(GetPrivateProfileIntA("wispviz", "color", 0, iniPath), 0, NCOL - 1);
    cycleSec = clampi(GetPrivateProfileIntA("wispviz", "cycle_seconds", 2, iniPath), 0, 60);   // default: change colour every 2 s (menu: Colour cycling > Off to stop)
    onTop = GetPrivateProfileIntA("wispviz", "always_on_top", 1, iniPath) != 0;
    posX = GetPrivateProfileIntA("wispviz", "x", -1, iniPath);
    posY = GetPrivateProfileIntA("wispviz", "y", -1, iniPath);
    showInfo = GetPrivateProfileIntA("wispviz", "show_info", 0, iniPath) != 0;
    sizeV = clampi(GetPrivateProfileIntA("wispviz", "size", 14, iniPath), 6, 50); SC = sizeV / 14.f;
    delayMs = clampi(GetPrivateProfileIntA("wispviz", "delay_ms", 0, iniPath), 0, 500);   // Bluetooth headphones lag: delay the visuals to match
}
static void wi(const char* k, int v) { char b[16]; wsprintfA(b, "%d", v); WritePrivateProfileStringA("wispviz", k, b, iniPath); }
static void saveCfg() {
    wi("style", style); wi("bars", screenMode ? nbarsUser : nbars); wi("color", colorIdx); wi("cycle_seconds", cycleSec);
    wi("always_on_top", onTop); wi("x", screenMode ? savedX : posX); wi("y", screenMode ? savedY : posY); wi("show_info", showInfo); wi("energy", energy); wi("style_per_song", autoStyle); wi("screen_mode", screenMode); wi("screen_look", screenLook); wi("screen_height", screenH); wi("screen_opacity", screenOp); wi("balance", balanceIdx); wi("screen_on_top", screenTop); wi("size", sizeV); wi("delay_ms", delayMs);
}

// Screen mode bar count: one bar per ~22 px (Classic) or ~8 px (Needles) of screen width.
static void screenBars() { int sx = GetSystemMetrics(SM_CXSCREEN); nbars = clampi(screenLook == 0 ? sx / 22 : sx / 8, 16, 240); }

// ---- tiny software rasteriser: every shape is a signed-distance function -----------------
// distance < 0 inside the shape, > 0 outside; coverage = clamp(0.5 - d) gives free anti-aliasing.
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float sdRound(float x, float y, float cx, float cy, float hw, float hh, float r) {
    float dx = fabsf(x - cx) - (hw - r), dy = fabsf(y - cy) - (hh - r);
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0, in = dx > dy ? dx : dy; if (in > 0) in = 0;
    return sqrtf(ox * ox + oy * oy) + in - r;
}
static inline void over(int i, uint32_t c, float a) {    // premultiplied "over" blend of colour c, alpha a
    uint32_t d = px[i]; float ia = 1 - a;
    float R = ((c >> 16) & 255) * a + ((d >> 16) & 255) * ia;
    float G = ((c >> 8) & 255) * a + ((d >> 8) & 255) * ia;
    float B = (c & 255) * a + (d & 255) * ia;
    float A = 255 * a + (d >> 24) * ia;
    px[i] = ((uint32_t)A << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}
// Only the shape's bounding box is visited, so drawing stays cheap.
static void fillRound(float cx, float cy, float hw, float hh, float r, uint32_t c, float a) {
    cx = (cx + LOX) * SC; cy *= SC; hw *= SC; hh *= SC; r *= SC;   // logical -> device pixels (wave area starts after the song panel)
    int x0 = clampi((int)(cx - hw - 1), 0, W - 1), x1 = clampi((int)(cx + hw + 1), 0, W - 1);
    int y0 = clampi((int)(cy - hh - 1), 0, H - 1), y1 = clampi((int)(cy + hh + 1), 0, H - 1);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float cv = clamp01(.5f - sdRound(x + .5f, y + .5f, cx, cy, hw, hh, r));
        if (cv > 0) over(y * W + x, c, a * cv);
    }
}
static void fillSeg(float ax, float ay, float bx, float by, float rad, uint32_t c, float a) {  // thick line, round caps
    ax = (ax + LOX) * SC; bx = (bx + LOX) * SC; ay *= SC; by *= SC; rad *= SC;
    int x0 = clampi((int)(fminf(ax, bx) - rad - 1), 0, W - 1), x1 = clampi((int)(fmaxf(ax, bx) + rad + 1), 0, W - 1);
    int y0 = clampi((int)(fminf(ay, by) - rad - 1), 0, H - 1), y1 = clampi((int)(fmaxf(ay, by) + rad + 1), 0, H - 1);
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy + 1e-6f;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px_ = x + .5f - ax, py_ = y + .5f - ay, t = clamp01((px_ * dx + py_ * dy) / l2);
        float ex = px_ - dx * t, ey = py_ - dy * t, cv = clamp01(.5f - (sqrtf(ex * ex + ey * ey) - rad));
        if (cv > 0) over(y * W + x, c, a * cv);
    }
}

// ============================ STYLES (the "extension" part) ==============================
// A style is just: void fn(uint32_t colour). It reads bands[0..nbars-1] (0..1) and paints into px.
// To add one: write the function, add its name to STYN[], add it to STYLES[] and to geometry().
static void sCapsule(uint32_t c) {
    float pad = 10, bw = 6, gap = 4;
    for (int b = 0; b < nbars; b++) {
        float cx = pad + b * (bw + gap) + bw / 2, h = 6 + bands[b] * (LH - 14);
        fillRound(cx, LH / 2.f, bw / 2, h / 2, bw / 2, c, .95f);
    }
}
static void sBars(uint32_t c) {                           // bars from a baseline, with falling peak caps
    float pad = 8, bw = 4, gap = 2;
    for (int b = 0; b < nbars; b++) {
        float cx = pad + b * (bw + gap) + bw / 2, h = 2 + bands[b] * (LH - 16);
        fillRound(cx, LH - 6 - h / 2, bw / 2, h / 2, 1.2f, c, .95f);
        fillRound(cx, LH - 6 - 2 - peaks[b] * (LH - 16) - 2, bw / 2, 1.2f, 1.f, c, .9f);
    }
    fillRound(LWW / 2.f, LH - 3.f, (LWW - 2 * pad) / 2, 1.f, 1.f, c, .45f);
}
static void sMirror(uint32_t c) {                         // bars grow up and down from a centre line + soft reflection
    float pad = 8, bw = 4, gap = 2, mid = LH * .5f - 2;
    for (int b = 0; b < nbars; b++) {
        float cx = pad + b * (bw + gap) + bw / 2, h = 2 + bands[b] * (mid - 6);
        fillRound(cx, mid - 2 - h / 2, bw / 2, h / 2, 1.2f, c, .95f);
        fillRound(cx, mid + 3 + h * .25f, bw / 2, h * .25f, 1.2f, c, .25f);
    }
}
static float wphase = 0;
static void sWave(uint32_t c) {                           // a smooth travelling line whose height follows the sound
    float pad = 10, px0 = 0, py0 = 0; bool first = true;
    for (float x = pad; x <= LWW - pad; x += 2) {
        float u = (x - pad) / (LWW - 2 * pad) * (nbars - 1); int i = (int)u; if (i >= nbars - 1) i = nbars - 2;
        float f = u - i; f = f * f * (3 - 2 * f);
        float v = bands[i] + (bands[i + 1] - bands[i]) * f;
        float taper = sinf(3.14159f * (x - pad) / (LWW - 2 * pad));          // ends fade to the centre line
        float amp = fminf(1.f, powf(v, .7f) * 1.55f + kick * .3f);        // Wave only: lifts quiet parts (v^0.7), ~55 % more swing, big kick bounce
        float y = LH / 2.f + sinf(x * .16f + wphase) * amp * (LH / 2.f - 5) * sqrtf(taper);
        if (!first) fillSeg(px0, py0, x, y, 3.2f, c, .95f);
        px0 = x; py0 = y; first = false;
    }
}
static void sRadial(uint32_t c) {                         // ring of bars (cava style), mirrored left/right
    float cx = LWW / 2.f, cy = LH / 2.f, r0 = LWW * .22f, rmax = LWW * .46f; int n2 = nbars * 2;
    float th = fmaxf(1.2f, 6.2832f * r0 / n2 * .32f);
    for (int j = 0; j < n2; j++) {
        int b = j < nbars ? j : n2 - 1 - j; float a = (j + .5f) / n2 * 6.2832f - 1.5708f;
        float r1 = r0 + 2 + bands[b] * (rmax - r0 - 2);
        fillSeg(cx + cosf(a) * r0, cy + sinf(a) * r0, cx + cosf(a) * r1, cy + sinf(a) * r1, th, c, .95f);
    }
}

// ---- soft glow: a blurry ellipse behind the wave whose strength follows `kick` ----------------------
static void glow(uint32_t c) {
    float cx = (LOX + LWW / 2.f) * SC, cy = LH * SC / 2.f, rx = LWW * SC * .5f, ry = LH * SC * .5f;   // fits INSIDE the window: fades to 0 at the edge, so no visible box
    int x0 = clampi((int)(cx - rx), 0, W - 1), x1 = clampi((int)(cx + rx), 0, W - 1), y0 = clampi((int)(cy - ry), 0, H - 1), y1 = clampi((int)(cy + ry), 0, H - 1);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float dx = (x - cx) / rx, dy = (y - cy) / ry, d2 = dx * dx + dy * dy; if (d2 >= 1) continue;
        float f = 1 - d2; over(y * W + x, c, f * f * kick * (energy == 2 ? .22f : .12f));
    }
}
static void fillRing(float cx, float cy, float r, float th, uint32_t c, float a) {   // circle outline, logical coords
    cx = (cx + LOX) * SC; cy *= SC; r *= SC; th *= SC * .5f;
    int x0 = clampi((int)(cx - r - th - 1), 0, W - 1), x1 = clampi((int)(cx + r + th + 1), 0, W - 1);
    int y0 = clampi((int)(cy - r - th - 1), 0, H - 1), y1 = clampi((int)(cy + r + th + 1), 0, H - 1);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float d = fabsf(sqrtf((x + .5f - cx) * (x + .5f - cx) + (y + .5f - cy) * (y + .5f - cy)) - r) - th;
        float cv = clamp01(.5f - d); if (cv > 0) over(y * W + x, c, a * cv);
    }
}
static float rings[6]; static int nrings = 0; static float lastKick = 0;
static void updateRipples() {                              // runs once per frame from the timer (state, not drawing)
    if (kick > .6f && lastKick <= .6f && nrings < 6) rings[nrings++] = 0;   // rising edge of a hit (kick is already decayed once by now, so not 1.0)
    lastKick = kick;
    for (int i = 0; i < nrings; i++) rings[i] += 1.8f + (float)LWW / 70.f;
    int j = 0; for (int i = 0; i < nrings; i++) if (rings[i] < (LH * .5f - 8.f)) rings[j++] = rings[i]; nrings = j;
}

static void sPulse(uint32_t c) {                           // capsule bars that breathe with the bass
    float pad = 10, bw = 6, gap = 4, s = 1.f + kick * .25f;
    for (int b = 0; b < nbars; b++) {
        float cx = pad + b * (bw + gap) + bw / 2, h = (6 + bands[b] * (LH - 12)) * s; if (h > LH - 2) h = LH - 2.f;
        fillRound(cx, LH / 2.f, bw / 2 * (1.f + kick * .15f), h / 2, bw / 2, c, .95f);
    }
}
static void sRipple(uint32_t c) {                          // a core that thumps + rings expanding on every beat
    float cx = LWW / 2.f, cy = LH / 2.f, lvl = 0; for (int b = 0; b < nbars; b++) lvl += bands[b]; lvl /= nbars;
    for (int i = 0; i < nrings; i++) { float r = 6 + rings[i], a = clamp01(1.f - rings[i] / ((LH * .5f - 8.f))); fillRing(cx, cy, r, 2.2f, c, a * .9f); }
    float r0 = LH * .17f * (1.f + kick * .7f + lvl * .5f); fillRound(cx, cy, r0, r0, r0, c, .95f);
}
static void sAurora(uint32_t c) {                          // filled spectrum silhouette: vertical fade + bright rim, drawn per device column
    float pad = 10, w = LWW - 2 * pad, base = (LH - 4) * SC, py = 0, plx = 0; bool first = true;
    int xs = (int)((LOX + pad) * SC), xe = (int)((LOX + LWW - pad) * SC);
    for (int x = xs; x < xe && x < W; x++) {
        float lx = (x + .5f) / SC - LOX - pad;                  // x in logical units, 0..w
        float u = lx / w * (nbars - 1); int i = (int)u; if (i < 0) i = 0; if (i >= nbars - 1) i = nbars - 2; float f = u - i; f = f * f * (3 - 2 * f);
        float v = bands[i] + (bands[i + 1] - bands[i]) * f;
        float h = (2 + v * (LH - 10) * (1.f + .08f * sinf(lx * .3f + wphase))) * SC, top = base - h;
        for (int y = clampi((int)top, 0, H - 1); y <= clampi((int)base, 0, H - 1); y++) {
            float t = clamp01((y - top) / h), cov = y < top + 1 ? clamp01(y + 1 - top) : 1.f;
            over(y * W + x, c, (.6f - .5f * t) * cov);          // bright at the top, fading toward the baseline
        }
        float yl = LH - 4 - h / SC;
        if (!first) fillSeg(plx, py, lx + pad, yl, 1.1f, c, .95f);
        plx = lx + pad; py = yl; first = false;
    }
}
static void sDots(uint32_t c) {                            // LED-matrix columns
    float pad = 8, cw = 8; int rows = 7;
    for (int b = 0; b < nbars; b++) {
        int lit = (int)(bands[b] * rows + .5f);
        for (int r = 0; r < rows; r++) {
            float cx = pad + b * cw + cw / 2, cy = LH - 6 - r * ((LH - 10.f) / rows), a = r < lit ? .95f : .13f;
            fillRound(cx, cy, 2.4f, 2.4f, 2.4f, c, a);
        }
    }
}
static void sBurst(uint32_t c) {                           // radial bars that rotate + a core that flashes on the beat
    float cx = LWW / 2.f, cy = LH / 2.f, r0 = LWW * .2f * (1.f + kick * .18f), rmax = LWW * .47f; int n2 = nbars * 2;
    float th = fmaxf(1.2f, 6.2832f * r0 / n2 * .32f);
    for (int j = 0; j < n2; j++) {
        int b = j < nbars ? j : n2 - 1 - j; float a = (j + .5f) / n2 * 6.2832f + wphase * .06f;
        float r1 = r0 + 2 + bands[b] * (rmax - r0 - 2);
        fillSeg(cx + cosf(a) * r0, cy + sinf(a) * r0, cx + cosf(a) * r1, cy + sinf(a) * r1, th, c, .95f);
    }
    float cr = LWW * .09f * (1.f + kick * .9f); fillRound(cx, cy, cr, cr, cr, 0xFFFFFF, .25f + kick * .6f);
}
static void sStrings(uint32_t c) {                         // five bouncy lines, outer ones lag behind
    float pad = 10;
    for (int s2 = 0; s2 < 5; s2++) {
        float px0 = 0, py0 = 0; bool first = true;
        for (float x = pad; x <= LWW - pad; x += 2) {
            float u = (x - pad) / (LWW - 2 * pad) * (nbars - 1); int i = (int)u; if (i >= nbars - 1) i = nbars - 2; float f = u - i;
            float v = bands[i] + (bands[i + 1] - bands[i]) * f, taper = sqrtf(sinf(3.14159f * (x - pad) / (LWW - 2 * pad)));
            float y = LH / 2.f + (s2 - 2) * 5.f + sinf(x * .13f + wphase * (1.f + s2 * .15f) + s2) * v * (LH / 2.f - 6 - fabsf(s2 - 2.f) * 2.f) * taper * (1.f + kick * .4f);
            if (!first) fillSeg(px0, py0, x, y, 1.1f, c, .35f + s2 * .16f); px0 = x; py0 = y; first = false;
        }
    }
}
typedef void (*StyleFn)(uint32_t);
static const StyleFn STYLES[NSTY] = {sCapsule, sBars, sMirror, sWave, sRadial, sPulse, sRipple, sAurora, sDots, sBurst, sStrings};
static void geometry(int* w, int* h) {                    // sets the logical layout, returns the DEVICE size of the window
    if (screenMode) {                                      // screen mode: full screen width, `screenH` percent of the height
        int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
        *w = sx; *h = clampi(sy * screenH / 100, 80, sy); LWW = *w; LH = *h; LOX = 0; return;
    }
    int ww, hh;
    switch (style) {
    case 0: ww = nbars * 10 + 10; hh = 40; break;
    case 1: case 2: ww = nbars * 6 + 14; hh = 56; break;
    case 3: ww = nbars * 22 + 40; hh = 84; break;      // Wave only: much longer and taller than the other styles
    case 4: case 6: case 9: ww = hh = 64 + nbars * 3; break;
    case 5: ww = nbars * 10 + 10; hh = 48; break;
    case 7: ww = nbars * 9 + 20; hh = 44; break;
    case 8: ww = nbars * 8 + 16; hh = 48; break;
    default: ww = nbars * 9 + 20; hh = 48; break;
    }
    LWW = ww; LOX = showInfo ? PW : 0; LH = showInfo && hh < PH ? PH : hh;
    *w = (int)ceilf((LWW + LOX) * SC); *h = (int)ceilf(LH * SC);
}

// ---- song panel: art + title/artist text + prev / play-pause / next ---------------------------------
// Layout is in logical units (x: art 8..44, text 52..176, buttons 176..242) and scaled by SC.
// Text: GDI draws white text into a scratch bitmap; its brightness becomes coverage and we blend our own
// colour (+ a soft shadow) into the real buffer, so text keeps proper per-pixel transparency.
static const int THUMB_L = 36, TXT_L = 124, BTN0 = 176, BTNW = 22;
static HDC tdc; static HBITMAP tbmp; static uint32_t* tpx; static HFONT fTitle, fArtist; static int hoverBtn = -1;
static int txtW = 124, rowH = 20;                         // device size of one text row
static void initText() {                                  // (re)build text resources for the current scale
    if (tdc) { DeleteDC(tdc); DeleteObject(tbmp); DeleteObject(fTitle); DeleteObject(fArtist); }
    txtW = (int)(TXT_L * SC + .5f); if (txtW < 8) txtW = 8;
    rowH = (int)(20 * SC + .5f); if (rowH < 8) rowH = 8;
    BITMAPINFO bi = {}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), txtW, -2 * rowH, 1, 32, BI_RGB};
    tdc = CreateCompatibleDC(0); tbmp = CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, (void**)&tpx, 0, 0); SelectObject(tdc, tbmp);
    int f1 = (int)(14 * SC + .5f), f2 = (int)(12 * SC + .5f); if (f1 < 8) f1 = 8; if (f2 < 7) f2 = 7;
    fTitle = CreateFontA(-f1, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    fArtist = CreateFontA(-f2, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    SetBkMode(tdc, TRANSPARENT); SetTextColor(tdc, 0xFFFFFF);
}
static void blitText(int row0, int x, int y, float alpha) {   // scratch rows row0..row0+rowH -> buffer at device (x,y)
    int sh = (int)(1 * SC + .5f); if (sh < 1) sh = 1;
    for (int j = 0; j < rowH; j++) for (int i = 0; i < txtW; i++) {
        float cv = (tpx[(row0 + j) * txtW + i] & 255) / 255.f; if (cv <= 0) continue;
        int dx = x + i, dy = y + j; if (dx < 0 || dx >= W || dy < 0 || dy >= H) continue;
        if (dx + sh < W && dy + sh < H) over((dy + sh) * W + dx + sh, 0x000000, cv * .45f);   // shadow: readable on bright wallpapers
        over(dy * W + dx, 0xFFFFFF, cv * alpha);
    }
}
static void fillTri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t c, float a) {   // logical coords
    ax *= SC; ay *= SC; bx *= SC; by *= SC; cx *= SC; cy *= SC;
    int x0 = clampi((int)fminf(ax, fminf(bx, cx)) - 1, 0, W - 1), x1 = clampi((int)fmaxf(ax, fmaxf(bx, cx)) + 1, 0, W - 1);
    int y0 = clampi((int)fminf(ay, fminf(by, cy)) - 1, 0, H - 1), y1 = clampi((int)fmaxf(ay, fmaxf(by, cy)) + 1, 0, H - 1);
    float e[3][4] = {{ax, ay, bx, by}, {bx, by, cx, cy}, {cx, cy, ax, ay}};
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax), sg = area > 0 ? 1.f : -1.f;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float worst = -1e9f;
        for (int k = 0; k < 3; k++) {
            float dx = e[k][2] - e[k][0], dy = e[k][3] - e[k][1], l = sqrtf(dx * dx + dy * dy) + 1e-6f;
            float d = sg * ((x + .5f - e[k][0]) * dy - (y + .5f - e[k][1]) * dx) / l;   // >0 = outside this edge
            if (d > worst) worst = d;
        }
        float cv = clamp01(.5f - worst); if (cv > 0) over(y * W + x, c, a * cv);
    }
}
static void rawRound(float cx, float cy, float hw, float hh, float r, uint32_t c, float a) {   // fillRound without the panel x-offset
    int sv = LOX; LOX = 0; fillRound(cx, cy, hw, hh, r, c, a); LOX = sv;
}
static void drawPanel() {
    float cy = LH / 2.f, cyd = cy * SC;
    int ts = (int)(THUMB_L * SC + .5f); if (ts < 6) ts = 6;
    int tx = (int)(8 * SC + .5f), ty = (int)(cyd - ts / 2.f);
    if (gMedia.active && gMedia.hasThumb) {                          // album art, rounded corners, box-filtered when shrunk
        int n = (int)ceilf((float)THUMB / ts); if (n < 1) n = 1; if (n > 4) n = 4;
        float rad = 7 * SC;
        for (int j = 0; j < ts; j++) for (int i = 0; i < ts; i++) {
            int dx = tx + i, dy = ty + j; if (dx < 0 || dx >= W || dy < 0 || dy >= H) continue;
            float cv = clamp01(.5f - sdRound(i + .5f, j + .5f, ts / 2.f, ts / 2.f, ts / 2.f, ts / 2.f, rad));
            if (cv <= 0) continue;
            float sa = 0, sr = 0, sg2 = 0, sb = 0;
            for (int yy = 0; yy < n; yy++) for (int xx = 0; xx < n; xx++) {
                int u = (int)((i + (xx + .5f) / n) * THUMB / ts), v = (int)((j + (yy + .5f) / n) * THUMB / ts);
                if (u >= THUMB) u = THUMB - 1; if (v >= THUMB) v = THUMB - 1;
                uint32_t t = gMedia.thumb[v * THUMB + u]; sa += t >> 24; sr += (t >> 16) & 255; sg2 += (t >> 8) & 255; sb += t & 255;
            }
            float k = 1.f / (n * n); sa *= k; sr *= k; sg2 *= k; sb *= k;
            float ta = sa / 255.f * cv; int idx = dy * W + dx; uint32_t d = px[idx];
            float R = sr * cv + ((d >> 16) & 255) * (1 - ta), G = sg2 * cv + ((d >> 8) & 255) * (1 - ta);
            float B = sb * cv + (d & 255) * (1 - ta), A = 255 * ta + (d >> 24) * (1 - ta);
            px[idx] = ((uint32_t)A << 24) | ((uint32_t)fminf(R, 255) << 16) | ((uint32_t)fminf(G, 255) << 8) | (uint32_t)fminf(B, 255);
        }
    } else rawRound(8 + THUMB_L / 2.f, cy, THUMB_L / 2.f, THUMB_L / 2.f, 7.f, 0xFFFFFF, .14f);
    for (int i = 0; i < txtW * 2 * rowH; i++) tpx[i] = 0;
    RECT r1 = {0, 0, txtW, rowH}, r2 = {0, rowH, txtW, 2 * rowH};
    const wchar_t* t1 = gMedia.active && gMedia.title[0] ? gMedia.title : L"Nothing playing";
    SelectObject(tdc, fTitle);  DrawTextW(tdc, t1, -1, &r1, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);
    SelectObject(tdc, fArtist); DrawTextW(tdc, gMedia.active ? gMedia.artist : L"", -1, &r2, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);
    GdiFlush();
    bool has = gMedia.active; int xt = (int)((8 + THUMB_L + 8) * SC + .5f);
    blitText(0, xt, (int)(cyd - 19 * SC), has ? .95f : .6f);
    blitText(rowH, xt, (int)(cyd - 3 * SC), .62f);
    for (int b = 0; b < 3; b++) {                                    // prev / play-pause / next; hover brightens
        float bx = BTN0 + b * BTNW + BTNW / 2.f, a = (hoverBtn == b ? 1.f : .78f) * (has ? 1.f : .45f);
        if (b == 1) {
            if (gMedia.playing) { rawRound(bx - 3.5f, cy, 1.8f, 6.f, 1.f, 0xFFFFFF, a); rawRound(bx + 3.5f, cy, 1.8f, 6.f, 1.f, 0xFFFFFF, a); }
            else fillTri(bx - 4.5f, cy - 7, bx - 4.5f, cy + 7, bx + 6.f, cy, 0xFFFFFF, a);
        } else {
            float d = b == 0 ? -1.f : 1.f;                           // skip icon = triangle + bar
            fillTri(bx + d * 3.f, cy, bx - d * 4.f, cy - 6, bx - d * 4.f, cy + 6, 0xFFFFFF, a);   // apex points the way we skip
            rawRound(bx + d * 5.f, cy, 1.1f, 6.f, .8f, 0xFFFFFF, a);                              // bar on the far side
        }
    }
}
static int hitButton(int xd, int yd) {                    // device pixel -> button index, -1 = none
    float x = xd / SC, y = yd / SC;
    if (!showInfo || y < LH / 2.f - 14 || y > LH / 2.f + 14 || x < BTN0 || x >= BTN0 + 3 * BTNW) return -1;
    return (int)((x - BTN0) / BTNW);
}

// ---- colour: 5 fixed colours, optional automatic cycling with a soft crossfade ----------
static uint32_t lastCol = PAL[0]; static DWORD switchT = 0, fadeT0 = 0; static uint32_t fromCol = PAL[0];
static uint32_t colorNow(DWORD now, bool* fading) {
    if (cycleSec && now - switchT >= (DWORD)cycleSec * 1000) {
        fromCol = lastCol; colorIdx = (colorIdx + 1) % NCOL; switchT = fadeT0 = now;
    }
    float t = clamp01((now - fadeT0) / 700.f); *fading = t < 1;
    uint32_t to = PAL[colorIdx], o = 0;
    for (int s = 16; s >= 0; s -= 8) {
        float f = ((fromCol >> s) & 255) * (1 - t) + ((to >> s) & 255) * t; o |= (uint32_t)f << s;
    }
    return lastCol = o;
}
static void pickColor(int i) { fromCol = lastCol; colorIdx = i; fadeT0 = GetTickCount(); switchT = fadeT0; dirty = true; }

// ---- SCREEN MODE drawing: bars standing on the bottom edge, with a vertical fade, drawn straight into the buffer ----
static void barGrad(float x0, float x1, float ytop, float ybot, float r, uint32_t c, float aTop, float aBot) {
    aTop *= screenOp / 100.f; aBot *= screenOp / 100.f;                      // Screen mode opacity (menu: Opacity...)
    int ix0 = clampi((int)x0 - 1, 0, W - 1), ix1 = clampi((int)x1 + 1, 0, W - 1), iy0 = clampi((int)ytop - 1, 0, H - 1), iy1 = clampi((int)ybot, 0, H - 1);
    float cx = (x0 + x1) * .5f, hw = (x1 - x0) * .5f, hh = (ybot + r * 2 - ytop) * .5f, cy = ytop + hh, span = fmaxf(ybot - ytop, 1.f);
    if (r > hw) r = hw;
    for (int y = iy0; y <= iy1; y++) {
        float a = aTop + (aBot - aTop) * clamp01((y - ytop) / span);          // brighter at the tip, softer at the base
        for (int x = ix0; x <= ix1; x++) {
            float cv = clamp01(.5f - sdRound(x + .5f, y + .5f, cx, cy, hw, hh, r));   // rounded top, square (off-screen) bottom
            if (cv > 0) over(y * W + x, c, a * cv);
        }
    }
}
static void drawScreen(uint32_t c) {
    float pitch = (float)W / nbars;
    if (screenLook == 0) {                                 // 1 - classic bars with falling peak caps
        float bw = pitch * .8f;
        for (int b = 0; b < nbars; b++) {
            float cx = (b + .5f) * pitch, h = 4 + bands[b] * (H - 8) * (1.f + kick * .06f); if (h > H - 2) h = H - 2.f;
            barGrad(cx - bw / 2, cx + bw / 2, H - h, (float)H, fminf(3.f, bw / 2), c, .95f, .55f);
            float ph = 4 + peaks[b] * (H - 8); if (ph > h + 5) barGrad(cx - bw / 2, cx + bw / 2, H - ph - 3, H - ph, 1.f, c, .85f, .85f);
        }
    } else {                                               // 2 - thin needles over a soft glow that pulses on the beat
        int gh = (int)(H * .4f);
        for (int y = H - gh; y < H; y++) {
            float t = (float)(y - (H - gh)) / gh, a = t * t * (.04f + kick * .2f) * (screenOp / 100.f);
            for (int x = 0; x < W; x++) over(y * W + x, c, a);
        }
        float bw = fmaxf(2.f, pitch * .55f);
        for (int b = 0; b < nbars; b++) {
            float cx = (b + .5f) * pitch, h = 3 + bands[b] * (H - 6) * (1.f + kick * .05f); if (h > H - 2) h = H - 2.f;
            barGrad(cx - bw / 2, cx + bw / 2, H - h, (float)H, bw / 2, c, .98f, .35f);
        }
    }
}

static void render(uint32_t col) {
    if (screenMode) { memset(px, 0, (size_t)W * H * 4); drawScreen(col); return; }   // click-through, so no 1/255 hit-test alpha needed
    // alpha=1/255 everywhere: invisible, but Windows only lets you click/drag pixels with alpha>0
    for (int i = 0; i < W * H; i++) px[i] = 0x01000000;
    if (energy && kick > .05f) glow(col);                   // soft halo behind the wave that pulses on every beat
    STYLES[style](col);
    if (showInfo) drawPanel();
}

// ============================ AUDIO: capture =============================================
// WASAPI "loopback" = ask Windows for a copy of whatever is going to the speakers.
// That is why it works for Spotify, YouTube, games... with no mic and no virtual cable.
// A background thread reads small packets (~10 ms) and drops mono samples into a ring buffer.
static const int FFT_N = 2048;                    // analysis window: 2048 samples ~ 43 ms @48 kHz
static float ring[8192];                          // newest audio lives here (power of 2 size)
static volatile LONG wpos = 0;                    // total samples ever written
static volatile LONG srate = 48000;               // real rate is read from the device
static volatile DWORD lastData = 0;               // tick of the last non-silent packet

static void pushSample(float v) { ring[wpos & 8191] = v; MemoryBarrier(); InterlockedIncrement(&wpos); }

static DWORD WINAPI captureThread(LPVOID) {
    CoInitializeEx(0, COINIT_MULTITHREADED);
    for (;;) {                                    // outer loop: (re)connect, e.g. after you switch headphones
        IMMDeviceEnumerator* en = 0; IMMDevice* dev = 0; IAudioClient* ac = 0;
        IAudioCaptureClient* cc = 0; WAVEFORMATEX* wf = 0; LPWSTR id = 0;
        bool ok = SUCCEEDED(CoCreateInstance(W_CLSID_MMDeviceEnumerator, 0, CLSCTX_ALL, W_IID_IMMDeviceEnumerator, (void**)&en))
            && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))
            && SUCCEEDED(dev->GetId(&id))
            && SUCCEEDED(dev->Activate(W_IID_IAudioClient, CLSCTX_ALL, 0, (void**)&ac))
            && SUCCEEDED(ac->GetMixFormat(&wf))
            && SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, wf, 0))
            && SUCCEEDED(ac->GetService(W_IID_IAudioCaptureClient, (void**)&cc))
            && SUCCEEDED(ac->Start());
        if (ok) {
            srate = wf->nSamplesPerSec;
            int ch = wf->nChannels, bits = wf->wBitsPerSample;
            bool isFloat = wf->wFormatTag == 3 ||
                (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)wf)->SubFormat.Data1 == 3);
            for (int tick = 0;; tick++) {
                Sleep(10);
                UINT32 n = 0;
                if (FAILED(cc->GetNextPacketSize(&n))) break;
                while (n) {
                    BYTE* d; UINT32 fr; DWORD fl;
                    if (FAILED(cc->GetBuffer(&d, &fr, &fl, 0, 0))) goto out;
                    bool silent = (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    for (UINT32 i = 0; i < fr; i++) {          // mix all channels down to mono
                        float m = 0;
                        if (!silent) for (int c = 0; c < ch; c++) {
                            if (isFloat && bits == 32) m += ((float*)d)[i * ch + c];
                            else if (bits == 16) m += ((short*)d)[i * ch + c] / 32768.f;
                            else if (bits == 32) m += ((int*)d)[i * ch + c] / 2147483648.f;
                        }
                        pushSample(m / ch);
                    }
                    if (!silent) lastData = GetTickCount();
                    cc->ReleaseBuffer(fr);
                    if (FAILED(cc->GetNextPacketSize(&n))) goto out;
                }
                if (tick % 100 == 99) {                         // once a second: did the default device change?
                    IMMDevice* d2 = 0; LPWSTR id2 = 0; bool same = true;
                    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d2))) {
                        if (SUCCEEDED(d2->GetId(&id2))) { same = wcscmp(id, id2) == 0; CoTaskMemFree(id2); }
                        d2->Release();
                    }
                    if (!same) break;
                }
            }
        }
    out:
        if (ac) ac->Stop();
        if (cc) cc->Release(); if (ac) ac->Release(); if (dev) dev->Release(); if (en) en->Release();
        if (wf) CoTaskMemFree(wf); if (id) CoTaskMemFree(id);
        Sleep(1000);
    }
}

// ============================ AUDIO: FFT -> bands ========================================
// FFT = "which frequencies are in this slice of sound, and how loud". We then group the
// frequencies into nbars log-spaced bands (bass on the left, treble on the right) because
// that is how ears hear: each octave matters equally.
static void fft(float* re, float* im, int n) {                    // in-place radix-2
    for (int i = 1, j = 0; i < n; i++) {                           // bit-reversal reorder
        int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {                       // butterflies
        float ang = -6.2831853f / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

static float rawv[256], bandAvg[256], bandPk[256], agc = .6f, bassAvg = 0;
static float target[256];                          // what the audio says right now (0..1) per bar
static void analyze() {
    static float re[FFT_N], im[FFT_N], win[FFT_N]; static bool init = false;
    if (!init) { for (int i = 0; i < FFT_N; i++) win[i] = .5f - .5f * cosf(6.2831853f * i / (FFT_N - 1)); init = true; }
    if (GetTickCount() - lastData > 250) { for (int b = 0; b < nbars; b++) target[b] = 0; kick = 0; agc *= .99f; return; }  // silence
    LONG end = wpos - (LONG)((long long)srate * delayMs / 1000);   // read slightly in the past = visuals arrive later
    for (int i = 0; i < FFT_N; i++) { re[i] = ring[(end - FFT_N + i) & 8191] * win[i]; im[i] = 0; }
    fft(re, im, FFT_N);
    float binHz = (float)srate / FFT_N, fLo = 50.f, fHi = fminf(16000.f, srate * .45f);
    for (int b = 0; b < nbars; b++) {
        float f0 = fLo * powf(fHi / fLo, (float)b / nbars), f1 = fLo * powf(fHi / fLo, (float)(b + 1) / nbars);
        float k0 = f0 / binHz, k1 = f1 / binHz, mag;
        if (k1 - k0 < 1.f) {                                       // narrow (bass) band: interpolate between bins
            float kc = (k0 + k1) * .5f; int i = (int)kc; float fr = kc - i;
            float m0 = sqrtf(re[i] * re[i] + im[i] * im[i]), m1 = sqrtf(re[i + 1] * re[i + 1] + im[i + 1] * im[i + 1]);
            mag = m0 + (m1 - m0) * fr;
        } else {                                                    // wide band: loudest bin inside it
            mag = 0; for (int i = (int)k0; i <= (int)k1 && i < FFT_N / 2; i++) {
                float m = sqrtf(re[i] * re[i] + im[i] * im[i]); if (m > mag) mag = m; }
        }
        mag *= 4.f / FFT_N;                                         // normalise: full-scale sine -> ~1.0
        float db = 20.f * log10f(mag + 1e-9f);
        db += 3.f * log2f(sqrtf(f0 * f1) / 1000.f);                // +3 dB/octave tilt: real music is bass-heavy
        float v = (db + 78.f) / 60.f;                               // map -78..-18 dB onto 0..1 (NOT clamped yet)
        rawv[b] = v < 0 ? 0 : v;
    }
    // AUTO-GAIN: a bass-boosted song used to hit the 1.0 ceiling and stay there (flat, boring). Now the loudest band
    // of recent frames defines "full height", so loud songs are scaled down into the range and quiet ones scaled up.
    float fm = 0; for (int b = 0; b < nbars; b++) if (rawv[b] > fm) fm = rawv[b];
    agc += (fm - agc) * (fm > agc ? .35f : .012f);                  // rises fast, relaxes over ~3 s
    if (agc < .45f) agc = .45f;
    float bass = 0, bal = screenMode ? BALV[balanceIdx] : 0.f;
    for (int b = 0; b < nbars; b++) {
        // BALANCE (screen mode): each band is also measured against its OWN recent peak, so bass cannot dominate and
        // every part of the spectrum gets tall bars. bal = 0 gives exactly the old shared-gain behaviour.
        bandPk[b] += (rawv[b] - bandPk[b]) * (rawv[b] > bandPk[b] ? .3f : .006f); if (bandPk[b] < .30f) bandPk[b] = .30f;
        float n = rawv[b] / (agc * (1.f - bal) + bandPk[b] * bal);                                    // ~0..1 relative to the song's own peak
        n = powf(n < 0 ? 0 : n, 1.7f + 1.9f * bal);                 // contrast: quiet bands stay low, peaks stand out (more when Balanced/Even, so bars vary instead of forming a wall)
        // PUNCH: a sustained level only reaches ~60 % height; the part ABOVE the band's own slow average (a hit,
        // a new note) adds the rest. A constant sub-bass therefore sits mid-height and every drum hit visibly jumps.
        float punch = (n - bandAvg[b]) * 2.6f; punch = punch < 0 ? 0 : punch > .5f ? .5f : punch;
        bandAvg[b] += (n - bandAvg[b]) * .05f;
        float o = n * (.62f - .12f * bal) + punch;
        if (b < nbars / 3) o *= 1.f + .1f * (1.f - bal);            // bass emphasis (fades out as Balance rises)
        target[b] = o > 1 ? 1 : o;
        if (b < (nbars / 4 > 2 ? nbars / 4 : 2)) bass += target[b];
    }
    // KICK DETECTOR: bass energy jumping above its own recent average = a drum hit.
    bass /= (nbars / 4 > 2 ? nbars / 4 : 2);
    float onset = bass - bassAvg; bassAvg += (bass - bassAvg) * .08f;
    if (onset > .16f) { float k = onset * 3.2f; if (k > 1) k = 1; if (k > kick) kick = k; }
}


static void present(HWND hw, uint32_t col) {
    render(col);
    POINT src = {0, 0}; SIZE sz = {W, H};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hw, NULL, NULL, &sz, memdc, &src, 0, &bf, ULW_ALPHA);   // push our pixels to the screen
}

// (Re)create the pixel buffer + resize the window when style/bar-count change; keep it centred in place.
static void applyGeometry(HWND hw) {
    int nw, nh; geometry(&nw, &nh);
    if (nw == W && nh == H && dib) return;
    RECT r; GetWindowRect(hw, &r); int cxm = (r.left + r.right) / 2, cym = (r.top + r.bottom) / 2;
    W = nw; H = nh;
    BITMAPINFO bi = {}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    HBITMAP nd = CreateDIBSection(memdc, &bi, DIB_RGB_COLORS, (void**)&px, 0, 0);
    SelectObject(memdc, nd); if (dib) DeleteObject(dib); dib = nd;
    int nx = cxm - W / 2, ny = cym - H / 2, sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    if (forcePos) { nx = forceX; ny = forceY; forcePos = 0; }   // leaving screen mode: go back to where the widget was
    nx = clampi(nx, 0, sx > W ? sx - W : 0); ny = clampi(ny, 0, sy > H ? sy - H : 0);   // growing must never push it off-screen
    if (screenMode) { nx = 0; ny = sy - H; }                    // screen mode: stand on the bottom edge, full width
    SetWindowPos(hw, 0, nx, ny, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
    dirty = true;
}
// "Always on top" = visible over every app. "Desktop only" = parked just above the desktop, so any
// app window covers it. WM_WINDOWPOSCHANGING (below) keeps it there even when you click/drag it.
static int effTop() { return screenMode ? screenTop : onTop; }   // screen mode has its own behind/over setting
static void applyVisibility(HWND hw) {
    SetWindowPos(hw, effTop() ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    dirty = true;
}


// Snap onto the taskbar strip: the window is vertically centred in the taskbar, placed just left of the tray
// area. (Windows 11 has no real taskbar extensions any more, so we simply sit on top of the strip.)
static void snapToTaskbar(HWND hw) {
    HWND tb = FindWindowA("Shell_TrayWnd", 0); if (!tb) return;
    RECT t, r; GetWindowRect(tb, &t); GetWindowRect(hw, &r);
    HWND tray = FindWindowExA(tb, 0, "TrayNotifyWnd", 0); RECT n = t; if (tray) GetWindowRect(tray, &n);
    int w = r.right - r.left, h = r.bottom - r.top, x, y;
    bool horiz = (t.right - t.left) >= (t.bottom - t.top);
    APPBARDATA ab = {sizeof ab}; bool hidden = (SHAppBarMessage(ABM_GETSTATE, &ab) & ABS_AUTOHIDE) != 0;
    if (hidden && horiz) {                      // auto-hide parks the bar off-screen: use the spot where it slides in
        int sh = GetSystemMetrics(SM_CYSCREEN), th = t.bottom - t.top, dy = t.top >= sh - 4 ? t.top - (sh - th) : 0;
        t.top -= dy; t.bottom -= dy; n.top -= dy; n.bottom -= dy;
    }
    if (horiz) { x = (tray ? n.left : t.right) - w - 12; y = t.top + ((t.bottom - t.top) - h) / 2; }
    else { x = t.left + ((t.right - t.left) - w) / 2; y = (tray ? n.top : t.bottom) - h - 12; }
    if (dump) { fprintf(dump, "snap: taskbar %ld,%ld,%ld,%ld tray %ld,%ld,%ld,%ld hidden=%d screen=%dx%d win=%dx%d -> %d,%d\n", t.left, t.top, t.right, t.bottom, n.left, n.top, n.right, n.bottom, hidden, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), w, h, x, y); fflush(dump); }
    onTop = 1; applyVisibility(hw);
    SetWindowPos(hw, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    posX = x; posY = y; saveCfg();
}


// ---- Size slider: a tiny custom-drawn popup (Windows menus cannot hold a slider) ----------------------
// Drag the knob (or press Left/Right): the whole widget rescales live; the value is saved when you let go.
static const int SLW = 250, SLH = 58, SLX0 = 18, SLX1 = 232;      // popup size and the track's left/right ends
static HWND popW;
static void applySize() { SC = sizeV / 14.f; initText(); applyGeometry(gMain); dirty = true; }
static COLORREF rgbOf(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static int* slV = &sizeV; static int slMin = 6, slMax = 50; static const char* slName = "Size"; static void (*slAfter)() = applySize;   // what the popup edits
static void applyScreenH() { applyGeometry(gMain); dirty = true; }
static void setSize(int v) { v = clampi(v, slMin, slMax); if (v != *slV) { *slV = v; slAfter(); if (popW) InvalidateRect(popW, 0, FALSE); } }
static LRESULT CALLBACK popProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        HBRUSH bg = CreateSolidBrush(RGB(28, 29, 33)), tr = CreateSolidBrush(RGB(72, 74, 84)), fl = CreateSolidBrush(rgbOf(PAL[colorIdx]));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 72, 82));
        SelectObject(dc, bg); SelectObject(dc, pen); RoundRect(dc, 0, 0, SLW, SLH, 16, 16);
        const int ty = 40; int kx = SLX0 + (int)((*slV - slMin) / (float)(slMax - slMin) * (SLX1 - SLX0) + .5f);
        RECT a = {SLX0, ty - 2, SLX1, ty + 2}, b = {SLX0, ty - 2, kx, ty + 2}; FillRect(dc, &a, tr); FillRect(dc, &b, fl);
        HBRUSH wh = CreateSolidBrush(RGB(245, 245, 248)); SelectObject(dc, wh); SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, kx - 8, ty - 8, kx + 8, ty + 8);
        HFONT f = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(225, 226, 232));
        char t[16]; wsprintfA(t, "%d", *slV); RECT r1 = {SLX0, 8, SLX1, 28};
        DrawTextA(dc, slName, -1, &r1, DT_LEFT | DT_SINGLELINE); DrawTextA(dc, t, -1, &r1, DT_RIGHT | DT_SINGLELINE);
        DeleteObject(f); DeleteObject(wh); DeleteObject(pen); DeleteObject(bg); DeleteObject(tr); DeleteObject(fl);
        EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: SetCapture(h);                           // fall through: a click also jumps the knob
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            float t = ((short)LOWORD(l) - SLX0) / (float)(SLX1 - SLX0); t = t < 0 ? 0 : t > 1 ? 1 : t;
            setSize(slMin + (int)(t * (slMax - slMin) + .5f));
        }
        return 0;
    case WM_LBUTTONUP: ReleaseCapture(); saveCfg(); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(h);
        else if (w == VK_LEFT || w == VK_DOWN) { setSize(*slV - 1); saveCfg(); }
        else if (w == VK_RIGHT || w == VK_UP) { setSize(*slV + 1); saveCfg(); }
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) DestroyWindow(h); return 0;   // click anywhere else = close
    case WM_DESTROY: popW = 0; saveCfg(); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static void openSizePopup(HWND hw) {
    static bool reg = false; HINSTANCE hi = GetModuleHandleA(0);
    if (!reg) { WNDCLASSA c = {}; c.lpfnWndProc = popProc; c.hInstance = hi; c.lpszClassName = "wispviz_pop"; c.hCursor = LoadCursor(0, IDC_HAND); RegisterClassA(&c); reg = true; }
    if (popW) DestroyWindow(popW);
    RECT r; GetWindowRect(hw, &r); int sx = GetSystemMetrics(SM_CXSCREEN);
    int x = (r.left + r.right) / 2 - SLW / 2, y = r.top - SLH - 8; if (y < 0) y = r.bottom + 8;
    x = clampi(x, 0, sx - SLW);
    popW = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "wispviz_pop", "", WS_POPUP, x, y, SLW, SLH, hw, 0, hi, 0);
    SetWindowRgn(popW, CreateRoundRectRgn(0, 0, SLW + 1, SLH + 1, 16, 16), TRUE);
    ShowWindow(popW, SW_SHOW); SetForegroundWindow(popW); SetFocus(popW);
}

// ============================ INSTALL / AUTOSTART (no admin needed: everything is per-user) =================
// `wispviz.exe --install`   copies itself to %LOCALAPPDATA%\Programs\wispviz, starts with Windows (HKCU Run key), adds a Start Menu
//                            shortcut and an entry in Settings > Apps, then launches.
// `wispviz.exe --uninstall` removes all of that. First launch of a freshly downloaded exe offers to install.
#define WV_VERSION "1.0.0"
static const char* RUNKEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* UNKEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\wispviz";
static void installDir(char* b) { GetEnvironmentVariableA("LOCALAPPDATA", b, MAX_PATH); lstrcatA(b, "\\Programs\\wispviz"); }
static bool regSet(const char* key, const char* name, const char* val) {
    HKEY k; if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, 0, 0, KEY_SET_VALUE, 0, &k, 0)) return false;
    LONG r = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)lstrlenA(val) + 1); RegCloseKey(k); return r == 0;
}
static bool autostartOn() {
    HKEY k; bool on = false;
    if (!RegOpenKeyExA(HKEY_CURRENT_USER, RUNKEY, 0, KEY_QUERY_VALUE, &k)) { on = !RegQueryValueExA(k, "wispviz", 0, 0, 0, 0); RegCloseKey(k); }
    return on;
}
static void setAutostart(bool on) {
    if (on) { char e[MAX_PATH], q[MAX_PATH + 4]; GetModuleFileNameA(0, e, MAX_PATH); wsprintfA(q, "\"%s\"", e); regSet(RUNKEY, "wispviz", q); }
    else { HKEY k; if (!RegOpenKeyExA(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k)) { RegDeleteValueA(k, "wispviz"); RegCloseKey(k); } }
}
static void startMenuLink(char* out) { GetEnvironmentVariableA("APPDATA", out, MAX_PATH); lstrcatA(out, "\\Microsoft\\Windows\\Start Menu\\Programs\\wispviz.lnk"); }
static void makeShortcut(const char* lnk, const char* target, const char* dir) {
    CoInitialize(0); IShellLinkA* sl = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER, IID_IShellLinkA, (void**)&sl)) && sl) {
        sl->SetPath(target); sl->SetWorkingDirectory(dir); sl->SetDescription("wispviz - tiny audio visualiser"); sl->SetIconLocation(target, 0);
        IPersistFile* pf = 0; if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
            wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, lnk, -1, w, MAX_PATH); pf->Save(w, TRUE); pf->Release(); }
        sl->Release();
    }
}
static void closeRunning() {                              // ask a running copy to quit so its exe file is not locked
    for (int i = 0; i < 20; i++) { HWND h = FindWindowA("wispviz", 0); if (!h) return; PostMessageA(h, WM_CLOSE, 0, 0); Sleep(150); }
}
static bool inInstallDir() {
    char e[MAX_PATH], d[MAX_PATH]; GetModuleFileNameA(0, e, MAX_PATH); installDir(d);
    char* sl = strrchr(e, '\\'); if (sl) *sl = 0; return lstrcmpiA(e, d) == 0;
}
static int doInstall(bool quiet) {
    char self[MAX_PATH], dir[MAX_PATH], dst[MAX_PATH], lnk[MAX_PATH];
    GetModuleFileNameA(0, self, MAX_PATH); installDir(dir); wsprintfA(dst, "%s\\wispviz.exe", dir); startMenuLink(lnk);
    SHCreateDirectoryExA(0, dir, 0);
    bool same = lstrcmpiA(self, dst) == 0;
    if (!same) { closeRunning(); if (!CopyFileA(self, dst, FALSE)) { if (!quiet) MessageBoxA(0, "Could not copy wispviz into your user folder.", "wispviz", MB_ICONERROR); return 1; } }
    setAutostart(false); char q[MAX_PATH + 4]; wsprintfA(q, "\"%s\"", dst); regSet(RUNKEY, "wispviz", q);   // autostart -> the INSTALLED copy
    makeShortcut(lnk, dst, dir);
    char un[MAX_PATH + 20]; wsprintfA(un, "\"%s\" --uninstall", dst);
    regSet(UNKEY, "DisplayName", "wispviz"); regSet(UNKEY, "DisplayVersion", WV_VERSION); regSet(UNKEY, "Publisher", "Pavan");
    regSet(UNKEY, "DisplayIcon", dst); regSet(UNKEY, "InstallLocation", dir); regSet(UNKEY, "UninstallString", un);
    if (!same || !FindWindowA("wispviz", 0)) ShellExecuteA(0, "open", dst, 0, dir, SW_SHOWNORMAL);
    if (!quiet) MessageBoxA(0, "wispviz is installed.\n\nIt will start with Windows. Right-click the bars for settings, or press Ctrl+Alt+V for Screen mode.\nTo remove it: Settings > Apps > wispviz.", "wispviz", MB_ICONINFORMATION);
    return 0;
}
static int doUninstall(bool quiet) {
    if (!quiet && MessageBoxA(0, "Remove wispviz (stop it, remove autostart, the Start Menu entry and its files and settings)?", "wispviz", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    char dir[MAX_PATH], lnk[MAX_PATH]; installDir(dir); startMenuLink(lnk);
    closeRunning(); setAutostart(false); DeleteFileA(lnk); RegDeleteKeyA(HKEY_CURRENT_USER, UNKEY);
    int n = lstrlenA(dir), tail = lstrlenA("\\Programs\\wispviz");
    if (n > tail && lstrcmpiA(dir + n - tail, "\\Programs\\wispviz") == 0) {           // safety: only ever delete that exact folder
        char cmd[MAX_PATH * 2 + 60]; wsprintfA(cmd, "cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"%s\"", dir);   // wait for this exe to exit, then delete
        STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        if (CreateProcessA(0, cmd, 0, 0, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, 0, 0, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    }
    if (!quiet) MessageBoxA(0, "wispviz was removed.", "wispviz", MB_ICONINFORMATION);
    return 0;
}

// ---- turn Screen mode on/off ----------------------------------------------------------------------------
// Click-through = WS_EX_TRANSPARENT: mouse clicks fall straight through to whatever is under the bars.
static void setClickThrough(HWND hw, int on) {
    LONG ex = GetWindowLongA(hw, GWL_EXSTYLE);
    SetWindowLongA(hw, GWL_EXSTYLE, on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT));
}
static void setScreen(HWND hw, int on) {
    if (on == screenMode) return;
    if (on) { RECT r; GetWindowRect(hw, &r); savedX = r.left; savedY = r.top; nbarsUser = nbars; screenMode = 1; screenBars(); }
    else { screenMode = 0; nbars = nbarsUser; forceX = savedX; forceY = savedY; forcePos = 1; }
    setClickThrough(hw, screenMode);
    applyGeometry(hw); applyVisibility(hw); dirty = true; saveCfg();
}
static void openHeightPopup(HWND hw) { slV = &screenH; slMin = 8; slMax = 60; slName = "Screen height %"; slAfter = applyScreenH; openSizePopup(hw); }
static void openOpacityPopup(HWND hw) { slV = &screenOp; slMin = 8; slMax = 100; slName = "Screen opacity %"; slAfter = applyScreenH; openSizePopup(hw); }
static void openSizeSlider(HWND hw) { slV = &sizeV; slMin = 6; slMax = 50; slName = "Size"; slAfter = applySize; openSizePopup(hw); }

static void showMenu(HWND hw) {
    HMENU m = CreatePopupMenu(), sa = CreatePopupMenu(), sc = CreatePopupMenu(), sy = CreatePopupMenu(), sv = CreatePopupMenu(), se = CreatePopupMenu(), ss = CreatePopupMenu();
    char t[64];
    for (int i = 0; i < NSTY; i++) AppendMenuA(sa, MF_STRING | (style == i ? MF_CHECKED : 0), 100 + i, STYN[i]);
    AppendMenuA(sa, MF_SEPARATOR, 0, 0);
    AppendMenuA(sa, MF_STRING | (autoStyle ? MF_CHECKED : 0), 800, "Change animation every song");
    char tsz[32]; wsprintfA(tsz, "Size...   (%d)", sizeV);   // own buffer: `t` is reused below for the cycling labels
    for (int i = 0; i < NCOL; i++) AppendMenuA(sc, MF_STRING | (colorIdx == i ? MF_CHECKED : 0), 300 + i, PALN[i]);
    for (int i = 0; i < 3; i++) AppendMenuA(se, MF_STRING | (energy == i ? MF_CHECKED : 0), 700 + i, ENN[i]);
    AppendMenuA(sy, MF_STRING | (!cycleSec ? MF_CHECKED : 0), 400, "Off");
    static const int CS[3] = {2, 3, 5};
    for (int i = 0; i < 3; i++) { wsprintfA(t, "Every %d seconds", CS[i]); AppendMenuA(sy, MF_STRING | (cycleSec == CS[i] ? MF_CHECKED : 0), 400 + CS[i], t); }
    AppendMenuA(sv, MF_STRING | (onTop ? MF_CHECKED : 0), 500, "Show over every app");
    AppendMenuA(sv, MF_STRING | (!onTop ? MF_CHECKED : 0), 501, "Desktop only (behind apps)");
    AppendMenuA(sv, MF_SEPARATOR, 0, 0);
    AppendMenuA(sv, MF_STRING, 502, "Snap to taskbar");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sa, "Animation");
    AppendMenuA(m, MF_STRING, 200, tsz);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)se, "Energy");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sc, "Colour");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sy, "Colour cycling");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sv, "Visibility");
    AppendMenuA(ss, MF_STRING | (screenMode ? MF_CHECKED : 0), 850, "Screen mode   (Ctrl+Alt+V)");
    AppendMenuA(ss, MF_SEPARATOR, 0, 0);
    for (int i = 0; i < 2; i++) AppendMenuA(ss, MF_STRING | (screenLook == i ? MF_CHECKED : 0), 860 + i, LOOKN[i]);
    AppendMenuA(ss, MF_SEPARATOR, 0, 0);
    char th[40]; wsprintfA(th, "Height...   (%d%%)", screenH); AppendMenuA(ss, MF_STRING, 870, th);
    char to[40]; wsprintfA(to, "Opacity...   (%d%%)", screenOp); AppendMenuA(ss, MF_STRING, 871, to);
    for (int i = 0; i < 3; i++) { char bt[40]; wsprintfA(bt, "Balance: %s%s", BALN[i], i == 1 ? "  (default)" : ""); AppendMenuA(ss, MF_STRING | (balanceIdx == i ? MF_CHECKED : 0), 880 + i, bt); }
    AppendMenuA(ss, MF_SEPARATOR, 0, 0);
    AppendMenuA(ss, MF_STRING | (!screenTop ? MF_CHECKED : 0), 890, "Behind windows");
    AppendMenuA(ss, MF_STRING | (screenTop ? MF_CHECKED : 0), 891, "Over every app");
    AppendMenuA(m, MF_POPUP | (screenMode ? MF_CHECKED : 0), (UINT_PTR)ss, "Screen mode");
    AppendMenuA(m, MF_STRING | (showInfo ? MF_CHECKED : 0), 600, "Song info + controls");
    AppendMenuA(m, MF_STRING | (autostartOn() ? MF_CHECKED : 0), 950, "Start with Windows");
    AppendMenuA(m, MF_SEPARATOR, 0, 0);
    AppendMenuA(m, MF_STRING, 900, "Quit wispviz");
    POINT p; GetCursorPos(&p); SetForegroundWindow(hw);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hw, 0);
    PostMessage(hw, WM_NULL, 0, 0); DestroyMenu(m);                  // destroys the submenus too
    if (id >= 100 && id < 100 + NSTY) style = id - 100;
    else if (id >= 700 && id < 703) energy = id - 700;
    else if (id == 800) autoStyle = !autoStyle;
    else if (id == 200) { openSizeSlider(hw); return; }
    else if (id == 850) { setScreen(hw, !screenMode); return; }
    else if (id == 860 || id == 861) { screenLook = id - 860; if (screenMode) screenBars(); }
    else if (id == 870) { openHeightPopup(hw); return; }
    else if (id == 871) { openOpacityPopup(hw); return; }
    else if (id >= 880 && id < 883) balanceIdx = id - 880;
    else if (id == 890 || id == 891) { screenTop = id == 891; applyVisibility(hw); }
    else if (id >= 300 && id < 300 + NCOL) pickColor(id - 300);
    else if (id >= 400 && id < 410) { cycleSec = id - 400; switchT = GetTickCount(); }
    else if (id == 500 || id == 501) { onTop = id == 500; applyVisibility(hw); }
    else if (id == 502) { snapToTaskbar(hw); return; }
    else if (id == 600) showInfo = !showInfo;
    else if (id == 950) { setAutostart(!autostartOn()); return; }
    else if (id == 900) { PostQuitMessage(0); return; }
    else return;
    applyGeometry(hw); dirty = true;
    if (!screenMode) { RECT r; GetWindowRect(hw, &r); posX = r.left; posY = r.top; }
    saveCfg();
}

static NOTIFYICONDATAA nid;
static LRESULT CALLBACK proc(HWND hw, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: SetTimer(hw, 1, 33, 0); return 0;
    case WM_TIMER: {                                    // ~30 fps
        analyze();                                      // real audio -> target[]
        float mx = 0;
        for (int i = 0; i < nbars; i++) {
            static const float ATK[3] = {.55f, .7f, .85f}, REL[3] = {.14f, .2f, .26f}, KG[3] = {0.f, .12f, .3f};
            float t = target[i] * (1.f + kick * KG[energy]); if (t > 1) t = 1;   // a kick makes everything jump a bit higher
            bands[i] += (t - bands[i]) * (t > bands[i] ? ATK[energy] : REL[energy]);   // fast attack, quicker release = snappy
            if (bands[i] < .004f) bands[i] = 0;
            peaks[i] = fmaxf(bands[i], peaks[i] - .02f);
            if (bands[i] > mx) mx = bands[i];
        }
        if (autoStyle) {                                // one animation per song: switch when the track changes
            static wchar_t lastT[128] = L"", lastA[128] = L""; static bool seeded = false;
            wchar_t t2[128], a2[128]; wcsncpy(t2, gMedia.title, 127); t2[127] = 0; wcsncpy(a2, gMedia.artist, 127); a2[127] = 0;
            if (gMedia.active && t2[0] && (wcscmp(t2, lastT) || wcscmp(a2, lastA))) {
                if (seeded) {                           // (the very first song just seeds, it keeps your chosen animation)
                    int ns = (int)(GetTickCount() * 2654435761u >> 7) % NSTY; if (ns == style) ns = (ns + 1) % NSTY;
                    if (screenMode) { screenLook ^= 1; screenBars(); } else { style = ns; applyGeometry(hw); }
                    dirty = true; saveCfg();
                    if (dump) { fprintf(dump, "SONG CHANGE -> style %d\n", style); fflush(dump); }
                }
                wcscpy(lastT, t2); wcscpy(lastA, a2); seeded = true;
            }
        }
        static LONG seenVer = 0; if (showInfo && gMedia.ver != seenVer) { seenVer = gMedia.ver; dirty = true; }
        bool fading; uint32_t col = colorNow(GetTickCount(), &fading);
        wphase += .25f + kick * .5f;
        kick *= .84f; if (kick < .01f) kick = 0;       // kick decays every frame
        updateRipples();
        bool wasIdleNow = (mx == 0 && !fading && !dirty);
        if (wasIdleNow) return 0;                       // silent + nothing changing -> skip drawing (saves CPU)
        bool again = fading || mx > 0;                  // keep animating while sound/fade runs, else one last draw
        if (dump) { fprintf(dump, "cmdN=%ld hr=%lx ", gMedia.lastCmdN, gMedia.lastCmdHr); fprintf(dump, "%lu", GetTickCount()); for (int i = 0; i < nbars; i++) fprintf(dump, " %.2f", bands[i]); fprintf(dump, "\n"); fflush(dump); }
        present(hw, col); dirty = again; return 0; }
    case WM_NCHITTEST: {                                // whole window drags
        if (screenMode) return HTTRANSPARENT;           // screen mode: clicks pass through to whatever is below = placeable anywhere ... except the 3 buttons
        RECT r; GetWindowRect(hw, &r);
        int b = hitButton((short)LOWORD(l) - r.left, (short)HIWORD(l) - r.top);
        if (b != hoverBtn) dirty = true; hoverBtn = b;
        return b >= 0 ? HTCLIENT : HTCAPTION; }
    case WM_LBUTTONUP: { int b = hitButton((short)LOWORD(l), (short)HIWORD(l)); if (dump) { fprintf(dump, "LBUTTONUP x=%d y=%d btn=%d\n", (short)LOWORD(l), (short)HIWORD(l), b); fflush(dump); } if (b >= 0) { mediaCmd(b); dirty = true; } return 0; }
    case WM_LBUTTONDOWN: if (dump) { fprintf(dump, "LBUTTONDOWN\n"); fflush(dump); } return 0;
    case WM_NCLBUTTONDOWN: if (dump) { fprintf(dump, "NCLBUTTONDOWN ht=%d\n", (int)w); fflush(dump); } break;
    case WM_NCRBUTTONUP: case WM_RBUTTONUP: showMenu(hw); return 0;
    case WM_WINDOWPOSCHANGING:
        if (!effTop()) { WINDOWPOS* wp = (WINDOWPOS*)l; HWND sh = GetShellWindow();
            wp->hwndInsertAfter = sh ? sh : HWND_BOTTOM; wp->flags &= ~SWP_NOZORDER; }
        break;
    case WM_EXITSIZEMOVE: { RECT r; GetWindowRect(hw, &r); posX = r.left; posY = r.top; saveCfg(); return 0; }
    case WM_HOTKEY: setScreen(hw, !screenMode); return 0;                 // Ctrl+Alt+V
    case WM_APP + 1:                                                      // tray icon: left-click toggles, right-click = menu
        if (LOWORD(l) == WM_LBUTTONUP) setScreen(hw, !screenMode);
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) showMenu(hw);
        return 0;
    case WM_DESTROY: Shell_NotifyIconA(NIM_DELETE, &nid); PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hw, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmdArgs, int) {
    bool quiet = strstr(cmdArgs, "--quiet") != 0;
    if (strstr(cmdArgs, "--uninstall")) return doUninstall(quiet);
    if (strstr(cmdArgs, "--install")) return doInstall(quiet);
    {   // a freshly downloaded exe (no arguments, not installed, no settings file beside it) offers to install itself
        char e[MAX_PATH], ini[MAX_PATH]; GetModuleFileNameA(0, e, MAX_PATH); lstrcpyA(ini, e); strcpy(strrchr(ini, '.'), ".ini");
        if (!cmdArgs[0] && !inInstallDir() && GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
            int r = MessageBoxA(0, "Install wispviz?\n\nYes: install for this user and start it with Windows (no admin needed).\nNo: just run it once without installing.", "wispviz " WV_VERSION, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDYES) return doInstall(false);
            if (r == IDCANCEL) return 0;
        }
    }
    HANDLE once = CreateMutexA(0, TRUE, "Local\\wispviz_single_instance");      // only one copy at a time (autostart + manual launch)
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDPIAware();                              // use real pixels (no blurry bitmap-stretching on scaled displays)
    loadCfg();
    savedX = posX; savedY = posY;
    if (screenMode) { nbarsUser = nbars; screenBars(); }
    lastCol = fromCol = PAL[colorIdx];
    if (strstr(GetCommandLineA(), " -d")) dump = fopen("wispviz_log.txt", "w");
    CreateThread(0, 0, captureThread, 0, 0, 0);        // audio runs on its own thread
    mediaStart(); initText();                          // now-playing info runs on another thread
    WNDCLASSA wc = {}; wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = "wispviz";
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(1)); RegisterClassA(&wc);
    memdc = CreateCompatibleDC(0);
    geometry(&W, &H); int cw = W, ch = H; W = H = 0;     // force applyGeometry to build the buffer
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    int x = posX >= 0 ? posX : (sx - cw) / 2, y = posY >= 0 ? posY : sy - ch - 70;
    HWND hw = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "wispviz", "wispviz",
                              WS_POPUP, x, y, cw, ch, 0, 0, hi, 0);
    gMain = hw; if (screenMode) setClickThrough(hw, 1);
    applyGeometry(hw); applyVisibility(hw);
    RegisterHotKey(hw, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');          // Ctrl+Alt+V toggles screen mode
    nid.cbSize = sizeof nid; nid.hWnd = hw; nid.uID = 1; nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(1));      // our logo (src/wispviz.rc) lstrcpyA(nid.szTip, "wispviz  (Ctrl+Alt+V: screen mode)");
    Shell_NotifyIconA(NIM_ADD, &nid);                                          // needed: in screen mode the bars cannot be right-clicked
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    if (strstr(GetCommandLineA(), "--slider")) openSizeSlider(hw);   // test hook
    if (strstr(GetCommandLineA(), "--snap")) snapToTaskbar(hw);   // launch already docked on the taskbar
    MSG msg; while (GetMessage(&msg, 0, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}

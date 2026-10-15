# How wispviz works

Four stages, on three threads.

```
 audio thread                 UI thread (30 fps timer)                  media thread
 WASAPI loopback              analyze() -> smooth -> draw -> push       WinRT media session
 -> ring buffer  --samples--> FFT -> bands[] -> style fn -> pixels      -> title / art / buttons
```

## 1. Capture (`captureThread` in `src/main.cpp`)
WASAPI *loopback* asks Windows for a copy of what is being sent to the speakers, so it works for any app with no microphone or virtual cable. A thread reads ~10 ms packets, mixes channels to mono and writes them into a ring buffer. Once a second it checks whether the default output device changed (you plugged headphones in) and reconnects.

## 2. Analyse (`fft`, `analyze`)
An FFT turns the newest 2048 samples (~43 ms) into frequency strengths. They are grouped into log-spaced bands (50 Hz to 16 kHz): each octave gets equal width, as ears hear. Magnitudes become decibels, get a +3 dB/octave tilt (real music is bass-heavy), and map to 0..1. Bars then rise fast and fall slowly (`attack .6`, `release .16`), which is what makes it feel alive. Silence skips drawing entirely.

### Energy: why bars used to look flat
A fixed loudness range saturates on bass-boosted songs: the bass bar sits at the ceiling and stops moving. `analyze()` now (1) auto-gains against the loudest band of the last few seconds, (2) applies a contrast curve, (3) splits each band into a *sustained* part (max ~60 % height) and a *punch* part (how far it is above its own slow average), and (4) runs a kick detector (bass energy above its running average) that sets `kick` (0..1, decays every frame). Styles read `kick` to scale bars, spawn ripples and flash the core, and `glow()` pulses a soft halo.

## 3. Draw (`render`, styles)
A layered window (`WS_EX_LAYERED` + `UpdateLayeredWindow`) takes *our own pixels with per-pixel alpha*, which is how the background is truly transparent. We rasterise by hand with signed-distance functions (`sdRound`, `fillRound`, `fillSeg`): negative inside the shape, positive outside, and `clamp(0.5 - d)` is free anti-aliasing. No graphics library, so the exe stays tiny. Every pixel starts at alpha 1/255: invisible, but Windows only hit-tests (lets you click/drag) pixels with alpha above 0.

## 3b. Screen mode
`setScreen()` stores the widget's position, resizes the same layered window to full screen width, parks it on the bottom edge and adds `WS_EX_TRANSPARENT` plus `HTTRANSPARENT` in `WM_NCHITTEST`, so every click falls through to the app underneath. Because it can no longer be right-clicked, Ctrl+Alt+V (`RegisterHotKey`) and a tray icon (`Shell_NotifyIcon`) toggle it. `Balance` mixes the shared auto-gain with a per-band gain so no single region of the spectrum dominates. Leaving screen mode restores the widget exactly where it was.

## 4. Window behaviour
- Drag: `WM_NCHITTEST` returns `HTCAPTION` everywhere, so the whole window is a title bar (except the 3 buttons).
- Desktop only vs over every app: `HWND_TOPMOST` vs sitting just above the desktop window, enforced in `WM_WINDOWPOSCHANGING`.
- Taskbar: `snapToTaskbar` finds `Shell_TrayWnd` and `TrayNotifyWnd`, and sits just left of the tray.

## 5. Now playing (`src/media.cpp`)
Windows keeps one list of media sessions (`GlobalSystemMediaTransportControlsSessionManager`). The compiler ships no header for it, so we call it *by hand*: a COM/WinRT object is a pointer to a table of function pointers (the vtable). Each method's slot number comes from Microsoft's IDL (slots 0-2 are `IUnknown`, 3-5 `IInspectable`, so the first real method is slot 6). Async calls are polled through `IAsyncInfo`. The thumbnail is a WinRT stream -> `IStream` -> WIC decoder -> scaled to 36x36. Buttons call `TrySkipPrevious / TogglePlayPause / TrySkipNext`.

Lesson learned the hard way: `Try...Async` button calls return a plain `bool`, not an object. Releasing it crashed the app (fixed in `mediaThread`).

## Debugging
`wispviz.exe -d` writes band values and snap/click info to `wispviz_log.txt`.

## 6. Install / autostart (`doInstall`, `doUninstall`)
No installer program: the exe installs itself, per user, with no admin rights. `--install` copies it to `%LOCALAPPDATA%\\Programs\\wispviz`, writes `HKCU\\...\\Run` (start with Windows), creates a Start Menu shortcut with `IShellLink`, and registers an uninstall entry under `HKCU\\...\\Uninstall` so it appears in Settings > Apps. `--uninstall` reverses all of it and deletes the folder through a detached `cmd` after the exe exits. A named mutex keeps autostart and a manual launch from running two copies.

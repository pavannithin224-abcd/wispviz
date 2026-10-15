// media.h - shared state between the media thread (media.cpp) and the UI (main.cpp).
#pragma once
#include <windows.h>
#include <stdint.h>
#define THUMB 130                                 // decoded album-art size; the UI scales it down/up (max Size 50 needs ~129)

struct MediaState {
    wchar_t title[128], artist[128];
    volatile bool playing, active, hasThumb;     // playing = actually playing, active = some player is open
    volatile LONG lastCmdHr, lastCmdN;           // debug: result of the last button press
    volatile LONG ver;                           // bumps every poll; UI redraws when it changes
    uint32_t* thumb;                             // THUMB*THUMB premultiplied BGRA, heap-allocated by mediaStart() so it costs no disk space
};
extern MediaState gMedia;
void mediaStart();                               // starts the polling thread
void mediaCmd(int c);                            // 0 = previous, 1 = play/pause, 2 = next

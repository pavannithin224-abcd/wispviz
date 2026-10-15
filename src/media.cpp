// media.cpp - "what is playing?" for wispviz: title, artist, play state, thumbnail, and transport controls.
//
// HOW IT WORKS
//  Windows keeps ONE system-wide list of media sessions (Spotify, Chrome/Edge tabs playing YouTube, the Media
//  Player app ...). Every player that shows a volume-flyout "now playing" card is in that list. The API is
//  WinRT: Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager. Our compiler ships no
//  header for it, so we call it "by hand": a COM/WinRT object is a pointer to a table of function pointers
//  (the "vtable"). We know each method's slot number from Microsoft's IDL (windows.media.control.idl);
//  the first 6 slots are always IUnknown (3) + IInspectable (3), so the first real method is slot 6.
//  Async calls (they return IAsyncOperation) are polled via IAsyncInfo until finished - no templates needed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <wincodec.h>
#include <string.h>
#include <stdint.h>
#include "media.h"

MediaState gMedia;                                   // read by the UI thread
static volatile LONG gCmd = -1;                      // command queued by the UI: 0 prev, 1 play/pause, 2 next

// ---- raw WinRT plumbing ---------------------------------------------------------------------
typedef void* HStr;
typedef HRESULT(WINAPI* PFN_RoInit)(int);
typedef HRESULT(WINAPI* PFN_RoGetFactory)(HStr, REFIID, void**);
typedef HRESULT(WINAPI* PFN_WCreate)(const wchar_t*, UINT32, HStr*);
typedef const wchar_t*(WINAPI* PFN_WRaw)(HStr, UINT32*);
typedef HRESULT(WINAPI* PFN_WDelete)(HStr);
typedef HRESULT(WINAPI* PFN_CSOR)(IUnknown*, REFIID, void**);
static PFN_RoGetFactory pGetFactory; static PFN_WCreate pWCreate; static PFN_WRaw pWRaw; static PFN_WDelete pWDelete;
static PFN_CSOR pCSOR;

// GUIDs copied from Microsoft's IDL (windows.media.control.idl / windows.storage.streams.idl)
DEFINE_GUID(G_ManagerStatics, 0x2050C4EE, 0x11A0, 0x57DE, 0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45);
DEFINE_GUID(G_Manager,        0xCACE8EAC, 0xE86E, 0x504A, 0xAB, 0x31, 0x5F, 0xF8, 0xFF, 0x1B, 0xCE, 0x49);
DEFINE_GUID(G_Session,        0x7148C835, 0x9B14, 0x5AE2, 0xAB, 0x85, 0xDC, 0x9B, 0x1C, 0x14, 0xE1, 0xA8);
DEFINE_GUID(G_MediaProps,     0x68856CF6, 0xADB4, 0x54B2, 0xAC, 0x16, 0x05, 0x83, 0x79, 0x07, 0xAC, 0xB6);
DEFINE_GUID(G_PlaybackInfo,   0x94B4B6CF, 0xE8BA, 0x51AD, 0x87, 0xA7, 0xC1, 0x0A, 0xDE, 0x10, 0x61, 0x27);
DEFINE_GUID(G_AsyncInfo,      0x00000036, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(G_IStream,        0x0000000C, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(G_WICFactory,     0xCACAF262, 0x9370, 0x4615, 0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A);
DEFINE_GUID(G_WICFactoryI,    0xEC5EC8A9, 0xC395, 0x4314, 0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70);
DEFINE_GUID(G_WICFmt32PBGRA,  0x6FDDC324, 0x4E03, 0x4BFE, 0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x10);

typedef HRESULT(STDMETHODCALLTYPE* Fn0)(void*, void**);          // method(void** out)
typedef HRESULT(STDMETHODCALLTYPE* FnU)(void*);
#define VT(o, i) ((void**)(*(void***)(o)))[i]                   // slot i of object o's vtable
static void rel(void* o) { if (o) ((FnU)VT(o, 2))(o); }          // IUnknown::Release is slot 2
static HRESULT callOut(void* o, int slot, void** out) { *out = 0; return ((Fn0)VT(o, slot))(o, out); }

// Wait for an IAsyncOperation to finish (max ~2 s) and return its result pointer (caller releases).
static void* await(void* op, int resultSlot = 8) {
    if (!op) return 0;
    void* info = 0; void* res = 0;
    typedef HRESULT(STDMETHODCALLTYPE* QI)(void*, REFIID, void**);
    if (SUCCEEDED(((QI)VT(op, 0))(op, G_AsyncInfo, &info))) {
        for (int i = 0; i < 200; i++) {
            int st = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, int*))VT(info, 7))(info, &st);   // get_Status
            if (st == 1) { callOut(op, resultSlot, &res); break; }                              // Completed
            if (st != 0) break;                                                                  // canceled / error
            Sleep(10);
        }
        rel(info);
    }
    rel(op);
    return res;
}
static void hstrToBuf(HStr h, wchar_t* dst, int n) {
    dst[0] = 0; if (!h) return;
    UINT32 len = 0; const wchar_t* p = pWRaw(h, &len);
    if (p) { if ((int)len >= n) len = n - 1; memcpy(dst, p, len * 2); dst[len] = 0; }
    pWDelete(h);
}
static void* getProp(void* o, int slot) { void* r = 0; callOut(o, slot, &r); return r; }

// ---- thumbnail: WinRT stream -> IStream -> WIC decode -> scale to THUMB x THUMB (cover-crop) ---
static bool loadThumb(void* props) {
    bool ok = false;
    void* ref = getProp(props, 15);                                   // MediaProperties.Thumbnail
    if (!ref) return false;
    void* op = 0; ((Fn0)VT(ref, 6))(ref, &op);                        // IRandomAccessStreamReference::OpenReadAsync
    void* stream = await(op);
    rel(ref);
    if (!stream) return false;
    IStream* is = 0;
    if (pCSOR && SUCCEEDED(pCSOR((IUnknown*)stream, G_IStream, (void**)&is)) && is) {
        IWICImagingFactory* f = 0;
        if (SUCCEEDED(CoCreateInstance(G_WICFactory, 0, CLSCTX_INPROC_SERVER, G_WICFactoryI, (void**)&f))) {
            IWICBitmapDecoder* dec = 0; IWICBitmapFrameDecode* fr = 0; IWICFormatConverter* cv = 0; IWICBitmapScaler* sc = 0;
            if (SUCCEEDED(f->CreateDecoderFromStream(is, 0, WICDecodeMetadataCacheOnDemand, &dec)) &&
                SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(fr, G_WICFmt32PBGRA, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom))) {
                UINT w = 0, h = 0; cv->GetSize(&w, &h);
                if (w && h && SUCCEEDED(f->CreateBitmapScaler(&sc))) {
                    // scale so the SHORT side == THUMB, then crop the centre (like CSS object-fit: cover)
                    UINT sw, sh; if (w < h) { sw = THUMB; sh = (UINT)((uint64_t)h * THUMB / w); } else { sh = THUMB; sw = (UINT)((uint64_t)w * THUMB / h); }
                    if (sw < THUMB) sw = THUMB; if (sh < THUMB) sh = THUMB;
                    if (SUCCEEDED(sc->Initialize(cv, sw, sh, WICBitmapInterpolationModeFant))) {
                        WICRect r = {(INT)((sw - THUMB) / 2), (INT)((sh - THUMB) / 2), THUMB, THUMB};
                        // decode straight into the shared buffer (a half-written frame for a few ms is harmless)
                        if (SUCCEEDED(sc->CopyPixels(&r, THUMB * 4, THUMB * THUMB * 4, (BYTE*)gMedia.thumb))) ok = true;
                    }
                }
            }
            if (sc) sc->Release(); if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); f->Release();
        }
        is->Release();
    }
    rel(stream);
    return ok;
}

static DWORD WINAPI mediaThread(LPVOID) {
    CoInitializeEx(0, COINIT_MULTITHREADED);
    HMODULE cb = LoadLibraryA("combase.dll"), sh = LoadLibraryA("shcore.dll");
    if (!cb) return 0;
    PFN_RoInit ri = (PFN_RoInit)GetProcAddress(cb, "RoInitialize");
    pGetFactory = (PFN_RoGetFactory)GetProcAddress(cb, "RoGetActivationFactory");
    pWCreate = (PFN_WCreate)GetProcAddress(cb, "WindowsCreateString");
    pWRaw = (PFN_WRaw)GetProcAddress(cb, "WindowsGetStringRawBuffer");
    pWDelete = (PFN_WDelete)GetProcAddress(cb, "WindowsDeleteString");
    if (sh) pCSOR = (PFN_CSOR)GetProcAddress(sh, "CreateStreamOverRandomAccessStream");
    if (!pGetFactory || !pWCreate || !pWRaw || !pWDelete) return 0;
    (void)ri;

    void* mgr = 0;
    const wchar_t* cls = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    HStr hs = 0; pWCreate(cls, (UINT32)wcslen(cls), &hs);
    void* statics = 0;
    if (SUCCEEDED(pGetFactory(hs, G_ManagerStatics, &statics)) && statics) {
        void* op = 0; ((Fn0)VT(statics, 6))(statics, &op);              // RequestAsync
        mgr = await(op);
        rel(statics);
    }
    pWDelete(hs);
    if (!mgr) return 0;                                                  // unsupported Windows -> panel stays empty

    wchar_t lastKey[260] = L"";
    for (;;) {
        // 1) run a queued button press on the CURRENT session
        void* sess = 0; callOut(mgr, 6, &sess);                          // Manager.GetCurrentSession
        LONG c = InterlockedExchange(&gCmd, -1);
        if (sess && c >= 0) {
            static const int SLOT[3] = {17, 20, 16};                     // SkipPrevious, TogglePlayPause, SkipNext
            void* op = 0; HRESULT hr = ((Fn0)VT(sess, SLOT[c]))(sess, &op); await(op);   // result is a plain bool (true/false), NOT an object - so there is nothing to Release
            gMedia.lastCmdHr = (LONG)hr; InterlockedIncrement(&gMedia.lastCmdN);
        }
        // 2) read what's playing
        wchar_t title[128] = L"", artist[128] = L""; int status = 0; bool gotThumb = false; bool any = false;
        if (sess) {
            void* op = 0; ((Fn0)VT(sess, 7))(sess, &op);                 // TryGetMediaPropertiesAsync
            void* props = await(op);
            if (props) {
                any = true;
                HStr t = 0, a = 0; callOut(props, 6, (void**)&t); callOut(props, 9, (void**)&a);   // Title, Artist
                hstrToBuf(t, title, 128); hstrToBuf(a, artist, 128);
                wchar_t key[260]; wcscpy(key, title); wcscat(key, L"|"); wcscat(key, artist);
                if (wcscmp(key, lastKey) != 0) {                                                      // track changed
                    gotThumb = loadThumb(props); wcscpy(lastKey, key);
                    gMedia.hasThumb = gotThumb;
                }
                rel(props);
            }
            void* pi = 0; callOut(sess, 9, &pi);                         // GetPlaybackInfo
            if (pi) { ((HRESULT(STDMETHODCALLTYPE*)(void*, int*))VT(pi, 7))(pi, &status); rel(pi); }   // PlaybackStatus
            rel(sess);
        } else lastKey[0] = 0;
        wcscpy(gMedia.title, title); wcscpy(gMedia.artist, artist);
        gMedia.playing = (status == 4); gMedia.active = any;
        if (!any) gMedia.hasThumb = false;
        InterlockedIncrement(&gMedia.ver);
        for (int i = 0; i < 10 && gCmd < 0; i++) Sleep(100);              // poll ~1/s, wake early on a button press
    }
}

void mediaStart() {
    gMedia.thumb = (uint32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, THUMB * THUMB * 4);
    CreateThread(0, 0, mediaThread, 0, 0, 0);
}
void mediaCmd(int c) { InterlockedExchange(&gCmd, c); }

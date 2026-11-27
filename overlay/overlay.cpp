// overlay.cpp - FPS overlay as a Peer Pressure sideload (title) DLL.
//
// Adapted from XenonFPS by Pooka (https://github.com/pookatv/XenonFPS), MIT License,
// Copyright (c) 2026 Pooka. See XenonFPS's LICENSE for the full text.
//
// How it works:
//   1. A worker thread waits 20 s, then patches the first 4 instructions of the kernel's VdSwap
//      (called once per presented frame) to jump to HookVdSwap. (XenonFPS)
//   2. The worker reads fps.ini, and twice a second measures FPS, keeps min/avg, reads the
//      temperatures, and renders the panel (core/fps_core.c) into a one-byte-per-pixel image.
//      It then turns every non-transparent pixel into a GPU MEM_WRITE command, one command list
//      per known front buffer, and publishes the new version.
//   3. HookVdSwap counts frames, records frame times, calls the real VdSwap, then points the
//      GPU ring's NOP area at the matching command list, so the GPU overwrites those pixels of
//      the front buffer in order with the game's own commands. (XenonFPS)
//   4. On game exit the kernel's title-terminate notification puts VdSwap back.
//
// Changes from XenonFPS: title DLL at base 0x8B000000; trampoline baked in as const code (title
// DLL code pages are not writable); any position and frame width via the general tiling formula;
// command lists built on the worker instead of in the hook; fps.ini, toggle combo, min/avg,
// frametime graph, temperatures; title thread instead of system thread; cleanup on exit.
// Diagnostics go to Hdd1:\fps_diag.txt every 5 s.

#include <xtl.h>
#include <stdlib.h>
#include <string.h>
#include "../core/fps_core.h"

#define START_DELAY_MS    20000
#define LOG_EVERY_MS      5000
#define INPUT_POLL_MS     50
#define TEMP_EVERY_MS     2000
#define FB_ENTRIES        3           // front buffers remembered (games use 2 or 3)
#define SLOT_COUNT        (FB_ENTRIES * 2)
#define FT_RING           512         // frame times kept for the graph
#define INI_PATH          "\\Device\\Harddisk0\\Partition1\\PeerPressure\\SideLoading\\fps.ini"
#define DIAG_PATH         "\\Device\\Harddisk0\\Partition1\\fps_diag.txt"

typedef LONG NTSTATUS;

typedef struct _KSTR { USHORT Length; USHORT MaximumLength; char *Buffer; } KSTR;
typedef struct _KOBJATTR { HANDLE RootDirectory; KSTR *ObjectName; ULONG Attributes; } KOBJATTR;
typedef struct _KIOSB { NTSTATUS Status; ULONG Information; } KIOSB;

extern "C" {
    NTSTATUS ExCreateThread(PHANDLE, DWORD, LPDWORD, PVOID, LPTHREAD_START_ROUTINE, LPVOID, DWORD);
    NTSTATUS KeDelayExecutionThread(CHAR WaitMode, BOOL Alertable, PLARGE_INTEGER Interval);
    VOID XapiThreadStartup(VOID (__cdecl *)(VOID *), PVOID, DWORD);
    void     __stdcall RtlInitAnsiString(KSTR *dst, const char *src);
    NTSTATUS __stdcall NtCreateFile(HANDLE *h, ULONG access, KOBJATTR *oa, KIOSB *iosb,
                                    LARGE_INTEGER *alloc, ULONG attrs, ULONG share,
                                    ULONG disposition, ULONG options);
    NTSTATUS __stdcall NtWriteFile(HANDLE h, HANDLE ev, void *apc, void *apcctx, KIOSB *iosb,
                                   const void *buf, ULONG len, LARGE_INTEGER *offset);
    NTSTATUS __stdcall NtReadFile(HANDLE h, HANDLE ev, void *apc, void *apcctx, KIOSB *iosb,
                                  void *buf, ULONG len, LARGE_INTEGER *offset);
    NTSTATUS __stdcall NtClose(HANDLE h);
    void     __stdcall HalSendSMCMessage(void *input, void *output);
}

// Title termination notification (layout from community kernel headers; routine takes no args).
typedef struct _EX_TITLE_TERMINATE_REGISTRATION {
    void      *NotificationRoutine;
    DWORD      Priority;
    LIST_ENTRY ListEntry;
} EX_TITLE_TERMINATE_REGISTRATION;
extern "C" void ExRegisterTitleTerminateNotification(EX_TITLE_TERMINATE_REGISTRATION *reg, BOOL create);

static VOID DelayMs(DWORD ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(0, FALSE, &t);
}

#define TB_PER_SEC  49875000ULL       // CPU timebase
#define TB_PER_MS   49875ULL

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------

enum {
    D_MAGIC, D_STATE, D_PATCH, D_STEP, D_FRAMES, D_FPS, D_QUEUED, D_HIDDEN, D_REJ_GATE,
    D_REJ_NOSTREAM, D_REJ_RING, D_REJ_PTR, D_EXC, D_FB, D_WIDTH, D_HEIGHT, D_PITCH, D_FMT,
    D_LAST_D0, D_LAST_D1, D_LAST_D2, D_LAST_RB0, D_LAST_RB4, D_PTR_B, D_PTR_F,
    D_PANEL_W, D_PANEL_H, D_WRITES, D_VERSION, D_STREAMS, D_CMDVA, D_CMDBYTES,
    D_INI_BYTES, D_INI_KEYS, D_MODE, D_TOGGLES, D_PADS, D_FPS_MIN, D_FPS_AVG,
    D_TEMP_CPU, D_TEMP_GPU, D_TEMP_RAM, D_TEMP_MB,
    D_MODES,      // modes in the combo cycle (2 = memory fallback: off and FPS only)
    // Ring diagnostics (flicker investigation). "bad" = last skipped frame, "good" = first drawn.
    D_BAD_A,      // ring address of the last skipped frame
    D_BAD_NOP,    // index of the first NOP packet header (0xC0xx1000) in rb[0..63], or 0xFFFFFFFF
    D_BAD0,       // rb[0..15] of the last skipped frame
    D_BAD_LAST = D_BAD0 + 15,
    D_GOOD_A,
    D_GOOD0,      // rb[0..15] of the first drawn frame (before our patch)
    D_GOOD_LAST = D_GOOD0 + 15,
    D_SHIFTED,    // frames drawn where the fence was not at rb[0]
    D_SHIFT_K,    // where it was, last time
    D_COUNT
};
static const char *kDiagNames[D_COUNT] = {
    "magic", "state", "patch", "step", "frames", "fps", "queued", "hidden", "rej_gate",
    "rej_nostream", "rej_ring", "rej_ptr", "exc", "fb", "width", "height", "pitch", "fmt",
    "last_d0", "last_d1", "last_d2", "last_rb0", "last_rb4", "ptr_b", "ptr_f",
    "panel_w", "panel_h", "writes", "version", "streams", "cmdva", "cmdbytes",
    "ini_bytes", "ini_keys", "mode", "toggles", "pads", "fps_min", "fps_avg",
    "temp_cpu", "temp_gpu", "temp_ram", "temp_mb", "modes",
    "bad_a", "bad_nop",
    "bad0", "bad1", "bad2", "bad3", "bad4", "bad5", "bad6", "bad7",
    "bad8", "bad9", "bad10", "bad11", "bad12", "bad13", "bad14", "bad15",
    "good_a",
    "good0", "good1", "good2", "good3", "good4", "good5", "good6", "good7",
    "good8", "good9", "good10", "good11", "good12", "good13", "good14", "good15",
    "shifted", "shift_k"
};
static volatile DWORD g_diag[D_COUNT] = { 0xD1A60200 };

// Trampoline: title DLL code pages are execute+read only, so it is fixed at build time.
// [0..10]: the 4 overwritten VdSwap instructions replayed, then a jump back to 0x800C8BA0
// [16..19]: cache-flush helper: icbi 0,r3 ; sync ; isync ; blr
#pragma section(".tramp", read, execute)
__declspec(allocate(".tramp")) __declspec(align(128)) static const DWORD g_tramp[32] = {
    0x7D8802A6,   // mflr r12
    0x3D608010,   // lis r11,0x8010
    0x616BCBB4,   // ori r11,r11,0xCBB4  (register-save helper)
    0x7D6903A6,   // mtctr r11
    0x4E800421,   // bctrl  (replaces the original bl)
    0x9421FF40,   // stwu r1,-0xC0(r1)
    0x3D608017,   // lis r11,0x8017
    0x3D80800C,   // lis r12,0x800C
    0x618C8BA0,   // ori r12,r12,0x8BA0
    0x7D8903A6,   // mtctr r12
    0x4E800420,   // bctr
    0, 0, 0, 0, 0,
    0x7C001FAC,   // icbi 0,r3
    0x7C0004AC,   // sync
    0x4C00012C,   // isync
    0x4E800020    // blr
};
typedef VOID (*FlushFn)(PVOID);

// ---------------------------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------------------------

static FcConfig        g_cfg;                   // fps.ini as read
static FcConfig        g_modeCfg;               // what the current mode draws
static int             g_modes = 2;             // modes in the cycle
static int             g_maxW, g_maxH;          // largest panel in the cycle (buffer size)
static unsigned char  *g_canvas[2];             // panel image per version parity
static int             g_cvW[2], g_cvH[2];      // that image's size
static volatile DWORD  g_ver = 0;               // published version, 0 = nothing yet
static volatile DWORD  g_overlayOn = 0;
static volatile DWORD  g_stop = 0;

// Frame geometry seen by the hook (last accepted frame)
struct Geo { DWORD wid, hgt, tpr, fmt; };
static volatile DWORD  g_geoWid, g_geoHgt, g_geoTpr, g_geoFmt;

// Command lists: SLOT_COUNT slots of g_slotBytes, GPU readable. Slot = fbEntry * 2 + (ver & 1).
static DWORD           g_cmdVa = 0, g_cmdPhys = 0, g_slotBytes = 0;
static volatile DWORD  g_fbTab[FB_ENTRIES];
static DWORD           g_fbNext = 0;
static volatile DWORD  g_fbDirty = 0;           // hook saw a buffer/geometry with no list
static volatile DWORD  g_slotFb[SLOT_COUNT], g_slotVer[SLOT_COUNT], g_slotLen[SLOT_COUNT];
static Geo             g_slotGeo[SLOT_COUNT];

// Frame times (hook writes, worker reads), in 0.1 ms
static unsigned short  g_ftRing[FT_RING];
static volatile DWORD  g_ftHead = 0;
static ULONGLONG       g_lastTb = 0;

static DWORD PhysOf(DWORD va)
{
    if (va >= 0xA0000000u && va < 0xE0000000u) return va & 0x1FFFFFFFu;
    if (va >= 0xE0000000u)                     return (va & 0x1FFFFFFFu) + 0x1000u;
    return 0;
}

// XenonFPS only read 0x7xxxxxxx; in a title the texture fetch constant lives in title memory.
static DWORD Peek(PVOID p)
{
    DWORD a = (DWORD)p;
    if ((a & 3) || a < 0x10000) return 0xDEADDEAD;
    __try {
        return *(volatile DWORD *)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFA17FA17;
    }
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

static void InitAttr(KOBJATTR *oa, KSTR *name, const char *path)
{
    RtlInitAnsiString(name, path);
    oa->RootDirectory = 0;
    oa->ObjectName = name;
    oa->Attributes = 0x40;                      // case insensitive
}

static void WriteNtFile(const char *path, const char *data, ULONG len)
{
    KSTR name;
    KOBJATTR oa;
    KIOSB iosb;
    HANDLE h = 0;
    InitAttr(&oa, &name, path);
    if (NtCreateFile(&h, 0x40000000 | 0x00100000, &oa, &iosb, 0, 0x80, 0, 5 /*overwrite_if*/,
                     0x20 | 0x40) >= 0) {
        NtWriteFile(h, 0, 0, 0, &iosb, data, len, 0);
        NtClose(h);
    }
}

// Reads up to max-1 bytes and NUL-terminates. Returns bytes read, or -1 if it cannot open.
static int ReadNtFile(const char *path, char *buf, ULONG max)
{
    KSTR name;
    KOBJATTR oa;
    KIOSB iosb;
    HANDLE h = 0;
    LARGE_INTEGER off;
    int n = -1;
    InitAttr(&oa, &name, path);
    if (NtCreateFile(&h, 0x80000000 | 0x00100000, &oa, &iosb, 0, 0x80, 1 /*share read*/,
                     1 /*open*/, 0x20 | 0x40) >= 0) {
        off.QuadPart = 0;
        n = 0;
        if (NtReadFile(h, 0, 0, 0, &iosb, buf, max - 1, &off) >= 0)
            n = (int)iosb.Information;
        NtClose(h);
    }
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static char *PutStr(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static char *PutHex(char *p, DWORD v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) *p++ = hex[(v >> i) & 0xF];
    return p;
}

static char *PutDec(char *p, DWORD v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static void WriteDiagFile()
{
    static char buf[8192];                      // ~40 bytes per line
    char *p = buf;
    for (DWORD i = 0; i < D_COUNT; i++) {
        p = PutStr(p, kDiagNames[i]);
        p = PutStr(p, " = 0x");
        p = PutHex(p, g_diag[i]);
        p = PutStr(p, " (");
        p = PutDec(p, g_diag[i]);
        p = PutStr(p, ")\r\n");
    }
    WriteNtFile(DIAG_PATH, buf, (ULONG)(p - buf));
}

// ---------------------------------------------------------------------------------------------
// Command lists (worker side)
// ---------------------------------------------------------------------------------------------

// Byte offset of pixel (x, y) in a 32-bit tiled surface with tpr 32x32 tiles per row
// (Xenia texture_address Tiled2D; XenonFPS used it with tpr = 40)
static DWORD TiledOff(DWORD x, DWORD y, DWORD tpr)
{
    DWORD outer = ((y >> 5) * tpr + (x >> 5)) << 6;
    DWORD inner = (((y >> 1) & 7) << 3) | (x & 7);
    DWORD oib   = (outer | inner) << 2;
    DWORD bank  = (y >> 4) & 1;
    DWORD pipe  = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11)
         | (oib & 15) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8)
         | ((oib >> 8) << 12);
}

// Pixel value for an RGB color. White and black use XenonFPS's known-good values. Other colors
// assume XenonFPS's component order (blue in the high bits); unverified on hardware.
static DWORD Encode(DWORD rgb, DWORD fmt)
{
    if (rgb == 0xFFFFFF) return 0xFFFFFFFF;
    if (rgb == 0)        return 0x00000003;
    DWORD r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (fmt == 54)
        return ((b * 1023 / 255) << 22) | ((g * 1023 / 255) << 12) | ((r * 1023 / 255) << 2) | 3;
    return (b << 24) | (g << 16) | (r << 8) | 3;
}

static DWORD ColorOf(int kind, DWORD fmt)
{
    switch (kind) {
    case FC_PX_TEXT:
    case FC_PX_GRAPH: return Encode(g_cfg.text_rgb, fmt);
    case FC_PX_GRID:  return Encode(0x808080, fmt);
    default:          return Encode(0, fmt);       // backing box and shadow
    }
}

// Panel top-left in this frame: fps.ini x/y are in 1280x720 space, scaled to the frame
static void PanelOrigin(const Geo &g, int pw, int ph, DWORD *ox, DWORD *oy)
{
    int x = g_cfg.x * (int)g.wid / 1280, y = g_cfg.y * (int)g.hgt / 720;
    if (x + pw > (int)g.wid) x = (int)g.wid - pw;
    if (y + ph > (int)g.hgt) y = (int)g.hgt - ph;
    *ox = (DWORD)(x < 0 ? 0 : x);
    *oy = (DWORD)(y < 0 ? 0 : y);
}

// Writes the command list for (front buffer entry e, version ver) from that version's canvas
static void BuildSlot(DWORD e, DWORD ver, const Geo &g)
{
    DWORD fb = g_fbTab[e];
    DWORD slot = e * 2 + (ver & 1);
    if (!fb || !g_cmdVa) return;
    if (g_slotFb[slot] == fb && g_slotVer[slot] == ver && g_slotGeo[slot].wid == g.wid &&
        g_slotGeo[slot].hgt == g.hgt && g_slotGeo[slot].tpr == g.tpr && g_slotGeo[slot].fmt == g.fmt)
        return;                                 // already current

    g_slotFb[slot] = 0;                         // invalid while rewriting
    __sync();

    const unsigned char *cv = g_canvas[ver & 1];
    int pw = g_cvW[ver & 1], ph = g_cvH[ver & 1];
    volatile DWORD *cb = (volatile DWORD *)(g_cmdVa + slot * g_slotBytes);
    DWORD maxDw = g_slotBytes / 4, n = 0, ox, oy;
    DWORD col[8];
    for (int k = 0; k < 8; k++) col[k] = ColorOf(k, g.fmt);
    PanelOrigin(g, pw, ph, &ox, &oy);

    for (int y = 0; y < ph; y++) {
        DWORD sy = oy + (DWORD)y;
        if (sy >= g.hgt) break;
        for (int x = 0; x < pw; x++) {
            int kind = cv[y * pw + x];
            DWORD sx = ox + (DWORD)x;
            if (kind == FC_PX_SKIP || sx >= g.wid || n + 3 > maxDw) continue;
            cb[n++] = 0xC0013D00;                                   // MEM_WRITE, one dword
            cb[n++] = PhysOf(fb + TiledOff(sx, sy, g.tpr)) | 2;     // physical | endian 8in32
            cb[n++] = col[kind & 7];
        }
    }
    for (DWORD o = 0; o < n; o += 32)           // the GPU reads RAM: flush every 128-byte line
        __dcbf(0, (void *)&cb[o]);
    __sync();

    g_slotLen[slot] = n;
    g_slotVer[slot] = ver;
    g_slotGeo[slot] = g;
    __sync();
    g_slotFb[slot]  = fb;                       // valid from here
    g_diag[D_STREAMS] = g_diag[D_STREAMS] + 1;
    g_diag[D_WRITES]  = n / 3;
}

static void CurrentGeo(Geo *g)
{
    g->wid = g_geoWid; g->hgt = g_geoHgt; g->tpr = g_geoTpr; g->fmt = g_geoFmt;
}

static void BuildAll(DWORD ver)
{
    Geo g;
    CurrentGeo(&g);
    if (!g.wid) return;                         // no accepted frame yet
    for (DWORD e = 0; e < FB_ENTRIES; e++) BuildSlot(e, ver, g);
}

// ---------------------------------------------------------------------------------------------
// Hook (render thread): count, time, and point the ring at a ready command list
// ---------------------------------------------------------------------------------------------

typedef DWORD (*VdSwapFn)(PVOID, PVOID, PVOID, PVOID, PVOID, PVOID, PVOID, PVOID, PVOID, PVOID);

static DWORD HookVdSwap(PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g, PVOID h, PVOID i, PVOID j)
{
    g_diag[D_FRAMES] = g_diag[D_FRAMES] + 1;

    ULONGLONG now = __mftb();
    if (g_lastTb) {
        ULONGLONG dt = (now - g_lastTb) * 2 / 9975;   // 0.1 ms units (timebase / 4987.5)
        g_ftRing[g_ftHead % FT_RING] = (unsigned short)(dt > 65535 ? 65535 : dt);
        g_ftHead = g_ftHead + 1;
    }
    g_lastTb = now;

    DWORD fb    = Peek(f);
    DWORD d0    = Peek(b);
    DWORD d1    = Peek((PBYTE)b + 4);
    DWORD d2    = Peek((PBYTE)b + 8);
    DWORD pitch = ((d0 >> 22) & 0x1FF) * 32;
    DWORD fmt   = d1 & 0x3F;
    DWORD wid   = (d2 & 0x1FFF) + 1;
    DWORD hgt   = ((d2 >> 13) & 0x1FFF) + 1;

    g_diag[D_PTR_B] = (DWORD)b;   g_diag[D_PTR_F] = (DWORD)f;
    g_diag[D_WIDTH] = wid;        g_diag[D_HEIGHT] = hgt;
    g_diag[D_PITCH] = pitch;      g_diag[D_FMT] = fmt;

    // 32-bit tiled front buffers of a sensible size (XenonFPS allowed only 1280x720 and 960x720)
    bool ok = ((d0 >> 31) == 1 && (fmt == 6 || fmt == 54) && pitch == wid &&
               wid >= 640 && wid <= 1920 && hgt >= 480 && hgt <= 1088 &&
               fb >= 0xA0000000 && (fb & 0xFFF) == 0 && fb == (d1 & 0xFFFFF000));
    if (!ok) {
        g_diag[D_REJ_GATE] = g_diag[D_REJ_GATE] + 1;
        g_diag[D_LAST_D0] = d0; g_diag[D_LAST_D1] = d1; g_diag[D_LAST_D2] = d2;
    } else {
        if (g_geoWid != wid || g_geoHgt != hgt || g_geoTpr != pitch / 32 || g_geoFmt != fmt) {
            g_geoWid = wid; g_geoHgt = hgt; g_geoTpr = pitch / 32; g_geoFmt = fmt;
            g_fbDirty = 1;
        }
    }

    DWORD ret = ((VdSwapFn)(PVOID)&g_tramp[0])(a, b, c, d, e, f, g, h, i, j);

    if (!ok) return ret;
    if (!g_overlayOn) { g_diag[D_HIDDEN] = g_diag[D_HIDDEN] + 1; return ret; }
    if ((((DWORD)a) & 3) || ((DWORD)a) < 0xA0000000) {
        g_diag[D_REJ_PTR] = g_diag[D_REJ_PTR] + 1;
        return ret;
    }

    // Find this front buffer's entry; a new one gets a list from the worker shortly
    DWORD ent = 0;
    for (; ent < FB_ENTRIES; ent++) if (g_fbTab[ent] == fb) break;
    if (ent == FB_ENTRIES) {
        ent = g_fbNext++ % FB_ENTRIES;
        g_fbTab[ent] = fb;
        g_fbDirty = 1;
        g_diag[D_REJ_NOSTREAM] = g_diag[D_REJ_NOSTREAM] + 1;
        return ret;
    }
    DWORD ver  = g_ver;
    DWORD slot = ent * 2 + (ver & 1);
    if (!ver || g_slotFb[slot] != fb || g_slotVer[slot] != ver || g_slotGeo[slot].wid != wid ||
        g_slotGeo[slot].hgt != hgt || g_slotGeo[slot].fmt != fmt || !g_slotLen[slot]) {
        g_fbDirty = 1;
        g_diag[D_REJ_NOSTREAM] = g_diag[D_REJ_NOSTREAM] + 1;
        return ret;
    }

    __try {
        // a = the GPU ring, a 64-dword block holding the kernel's fence packet (0xC0025800,
        // 4 dwords) followed by a NOP packet that fills the rest of the block. Usually the fence
        // is at rb[0] (XenonFPS's case: NOP 0xC03A1000 at rb[4]); on some frames it is further
        // in (seen: fence at rb[10], NOP 0xC0301000 at rb[14]). Find it wherever it is.
        volatile DWORD *rb = (volatile DWORD *)a;
        int k = -1;
        DWORD cnt = 0;
        for (int q = 0; q <= 32; q++) {
            if (rb[q] != 0xC0025800) continue;
            DWORD v = rb[q + 4];
            cnt = (v >> 16) & 0x3FFF;                       // NOP payload = cnt + 1 dwords
            if ((v & 0xC000FF00) == 0xC0001000 && cnt >= 3 && q + 4 + cnt + 1 <= 64)
                k = q;
            break;
        }
        if (k >= 0) {
            if (k == 0 && !g_diag[D_GOOD_A]) {              // one sample of a normal ring
                for (DWORD q = 0; q < 16; q++) g_diag[D_GOOD0 + q] = rb[q];
                g_diag[D_GOOD_A] = (DWORD)a;
            }
            if (k > 0) {
                g_diag[D_SHIFTED] = g_diag[D_SHIFTED] + 1;
                g_diag[D_SHIFT_K] = (DWORD)k;
            }
            // Replace the NOP's start with an INDIRECT_BUFFER (3 dwords) to our list, then a
            // shorter NOP over the rest of the original NOP's payload.
            rb[k + 4] = 0xC0013F00;                         // INDIRECT_BUFFER
            rb[k + 5] = g_cmdPhys + slot * g_slotBytes;
            rb[k + 6] = g_slotLen[slot];                    // dwords
            rb[k + 7] = 0xC0001000 | ((cnt - 3) << 16);     // NOP (0xC0371000 when cnt = 0x3A)
            __sync();
            g_diag[D_FB]     = fb;
            g_diag[D_QUEUED] = g_diag[D_QUEUED] + 1;
        } else {
            DWORD r0 = rb[0], r4 = rb[4];
            g_diag[D_REJ_RING] = g_diag[D_REJ_RING] + 1;
            g_diag[D_LAST_RB0] = r0;
            g_diag[D_LAST_RB4] = r4;
            g_diag[D_BAD_A]    = (DWORD)a;
            for (DWORD q = 0; q < 16; q++) g_diag[D_BAD0 + q] = rb[q];
            DWORD nop = 0xFFFFFFFF;
            for (DWORD q = 0; q < 64; q++)
                if ((rb[q] & 0xC000FF00) == 0xC0001000) { nop = q; break; }   // type-3 NOP header
            g_diag[D_BAD_NOP] = nop;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_diag[D_EXC] = g_diag[D_EXC] + 1;
    }
    return ret;
}

// ---------------------------------------------------------------------------------------------
// Hook install / remove (XenonFPS, guarded)
// step: 1 read VdSwap, 2 trampoline ready, 3 trampoline ran, 4 kernel patched
// patch: 0xBAD wrong kernel, 0xE1 cannot read kernel, 0xE2 trampoline not executable,
//        0xE3 cannot write kernel, 3 installed, 4 removed
// ---------------------------------------------------------------------------------------------

static void InstallHook()
{
    volatile DWORD *kp = (volatile DWORD *)0x800C8B90;
    DWORD k0, k1, k2, k3;

    __try {
        k0 = kp[0]; k1 = kp[1]; k2 = kp[2]; k3 = kp[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_diag[D_PATCH] = 0xE1;
        return;
    }
    g_diag[D_STEP] = 1;
    if (k0 != 0x7D8802A6 || k1 != 0x48044021 || k2 != 0x9421FF40 || k3 != 0x3D608017) {
        g_diag[D_PATCH] = 0xBAD;
        g_diag[D_LAST_D0] = k0;
        g_diag[D_LAST_D1] = k1;
        return;
    }
    g_diag[D_STEP] = 2;

    FlushFn flush = (FlushFn)(PVOID)&g_tramp[16];
    __try {
        flush((PVOID)&g_tramp[0]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_diag[D_PATCH] = 0xE2;
        return;
    }
    g_diag[D_STEP]  = 3;
    g_diag[D_PATCH] = 2;

    DWORD hk = (DWORD)(PVOID)&HookVdSwap;
    __try {
        kp[1] = 0x616B0000 | (hk & 0xFFFF);    // ori r11,r11,lo
        kp[2] = 0x7D6903A6;                    // mtctr r11
        kp[3] = 0x4E800420;                    // bctr
        kp[0] = 0x3D600000 | (hk >> 16);       // lis r11,hi  (written last)
        __dcbst(0, (void *)kp);
        __sync();
        flush((PVOID)kp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_diag[D_PATCH] = 0xE3;
        return;
    }
    g_diag[D_STEP]  = 4;
    g_diag[D_PATCH] = 3;
}

// Puts the original 4 VdSwap instructions back. Returns true if it restored the patch.
static bool RemoveHook()
{
    if (g_diag[D_PATCH] != 3) return false;
    volatile DWORD *kp = (volatile DWORD *)0x800C8B90;
    __try {
        kp[1] = 0x48044021;                    // bl (original)
        kp[2] = 0x9421FF40;                    // stwu r1,-0xC0(r1)
        kp[3] = 0x3D608017;                    // lis r11,0x8017
        kp[0] = 0x7D8802A6;                    // mflr r12
        __dcbst(0, (void *)kp);
        __sync();
        ((FlushFn)(PVOID)&g_tramp[16])((PVOID)kp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_diag[D_PATCH] = 4;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------------------------

static unsigned short PadButtons()
{
    unsigned short all = 0;
    DWORD pads = 0;
    for (DWORD u = 0; u < 4; u++) {
        XINPUT_STATE st;
        if (XInputGetState(u, &st) == ERROR_SUCCESS) {
            pads |= (1u << u);
            all |= st.Gamepad.wButtons;          // a combo on any pad counts
        }
    }
    g_diag[D_PADS] = pads;
    return all;
}

static bool ReadTemps(int out_c[4])
{
    unsigned char msg[16] = { 0x07 }, reply[16] = { 0 };
    __try {
        HalSendSMCMessage(msg, reply);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (reply[0] != 0x07) return false;
    fc_decode_temps(reply, out_c);
    return true;
}

static void LoadConfig()
{
    static char ini[4096];
    fc_config_defaults(&g_cfg);
    int n = ReadNtFile(INI_PATH, ini, sizeof(ini));
    g_diag[D_INI_BYTES] = (DWORD)n;              // 0xFFFFFFFF = no fps.ini, defaults used
    if (n > 0) g_diag[D_INI_KEYS] = (DWORD)fc_config_parse(&g_cfg, ini);
}

static DWORD WINAPI Worker(LPVOID)
{
    g_diag[D_STATE] = 1;
    LoadConfig();

    // Buffers are sized for the largest mode in the cycle (the last one). If the game cannot
    // spare that much memory, fall back to FPS only (modes: off, FPS).
    g_modes = fc_mode_count(&g_cfg);
    for (int attempt = 0; attempt < 2 && g_diag[D_STATE] != 2; attempt++) {
        if (attempt == 1) g_modes = 2;
        FcConfig big;
        fc_mode_config(&g_cfg, g_modes - 1, &big);
        fc_panel_size(&big, &g_maxW, &g_maxH);
        g_slotBytes = ((DWORD)(g_maxW * g_maxH) * 12 + 127) & ~127u;   // 3 dwords per pixel
        free(g_canvas[0]); free(g_canvas[1]);
        g_canvas[0] = (unsigned char *)malloc((size_t)(g_maxW * g_maxH));
        g_canvas[1] = (unsigned char *)malloc((size_t)(g_maxW * g_maxH));
        PVOID cb = XPhysicalAlloc(SLOT_COUNT * g_slotBytes, (ULONG_PTR)(~(ULONG_PTR)0), 0, PAGE_READWRITE);
        if (cb && PhysOf((DWORD)cb) && g_canvas[0] && g_canvas[1]) {
            g_cmdVa   = (DWORD)cb;
            g_cmdPhys = PhysOf((DWORD)cb);
            g_diag[D_CMDVA]    = g_cmdVa;
            g_diag[D_CMDBYTES] = SLOT_COUNT * g_slotBytes;
            g_diag[D_STATE]    = 2;
        } else if (cb) {
            XPhysicalFree(cb);
        }
    }
    g_diag[D_MODES]   = (DWORD)g_modes;
    g_diag[D_PANEL_W] = (DWORD)g_maxW;
    g_diag[D_PANEL_H] = (DWORD)g_maxH;
    WriteDiagFile();

    // The combo steps through the modes: off, FPS, then each extra added in turn
    int mode = g_cfg.enabled ? 1 : 0;
    fc_mode_config(&g_cfg, mode, &g_modeCfg);
    FcToggle tog;
    fc_toggle_init(&tog, g_cfg.combo, (unsigned)g_cfg.hold_ms, TB_PER_SEC, 0);
    g_overlayOn = mode != 0;
    g_diag[D_MODE] = (DWORD)mode;

    for (DWORD w = 0; w < START_DELAY_MS && !g_stop; w += INPUT_POLL_MS)
        DelayMs(INPUT_POLL_MS);
    if (g_stop) return 0;
    if (g_diag[D_STATE] != 2) { WriteDiagFile(); return 0; }    // no memory: nothing to draw
    InstallHook();
    WriteDiagFile();
    if (g_diag[D_PATCH] != 3) return 0;

    FcStats stats;
    fc_stats_init(&stats);
    FcPanelData pd;
    memset(&pd, 0, sizeof(pd));
    static unsigned short ft[FT_RING];

    ULONGLONG t0 = __mftb(), tLog = t0, tTemp = 0;
    DWORD f0 = g_diag[D_FRAMES];
    DWORD fps = 0, shownFps = 0xFFFFFFFF;
    bool modeChanged = true;

    while (!g_stop) {
        DelayMs(INPUT_POLL_MS);
        ULONGLONG t = __mftb();

        if (fc_toggle_update(&tog, PadButtons(), t)) {      // combo held: next mode
            mode = (mode + 1) % g_modes;
            fc_mode_config(&g_cfg, mode, &g_modeCfg);
            g_overlayOn = mode != 0;
            g_diag[D_MODE] = (DWORD)mode;
            g_diag[D_TOGGLES] = g_diag[D_TOGGLES] + 1;
            modeChanged = true;
            tTemp = 0;                           // fresh temperatures when they appear
        }

        if (g_fbDirty) {                         // new front buffer or frame size: lists for it
            g_fbDirty = 0;
            if (g_ver) BuildAll(g_ver);
        }

        bool tick = t - t0 >= TB_PER_SEC / 2;    // FPS window closes twice a second
        if (tick) {
            DWORD f = g_diag[D_FRAMES];
            fps = (DWORD)(((ULONGLONG)(f - f0) * TB_PER_SEC + (t - t0) / 2) / (t - t0));
            f0 = f;
            t0 = t;
            g_diag[D_FPS] = fps;
            fc_stats_add(&stats, fps);
            g_diag[D_FPS_MIN] = fc_stats_min(&stats);
            g_diag[D_FPS_AVG] = fc_stats_avg(&stats);
        }

        if (g_modeCfg.show_temps && (tTemp == 0 || t - tTemp >= TEMP_EVERY_MS * TB_PER_MS)) {
            tTemp = t;
            pd.temps_valid = ReadTemps(pd.temp_c) ? 1 : 0;
            g_diag[D_TEMP_CPU] = (DWORD)pd.temp_c[0];
            g_diag[D_TEMP_GPU] = (DWORD)pd.temp_c[1];
            g_diag[D_TEMP_RAM] = (DWORD)pd.temp_c[2];
            g_diag[D_TEMP_MB]  = (DWORD)pd.temp_c[3];
        }

        // Redraw right away on a mode change; otherwise twice a second when the number changed
        // or extras (which change every reading) are shown
        bool extras = g_modeCfg.show_stats || g_modeCfg.show_graph || g_modeCfg.show_temps;
        bool redraw = modeChanged || (tick && (fps != shownFps || extras));
        if (redraw && g_overlayOn) {
            pd.fps     = fps;
            pd.fps_min = fc_stats_min(&stats);
            pd.fps_avg = fc_stats_avg(&stats);
            pd.ft_x10  = 0;
            pd.ft_count = 0;
            if (g_modeCfg.show_graph) {
                DWORD head = g_ftHead, cnt = head < FT_RING ? head : FT_RING;
                for (DWORD k = 0; k < cnt; k++)  // oldest first
                    ft[k] = g_ftRing[(head - cnt + k) % FT_RING];
                pd.ft_x10 = ft;
                pd.ft_count = (int)cnt;
            }
            DWORD nv = g_ver + 1;
            int pw, ph;
            fc_panel_size(&g_modeCfg, &pw, &ph);
            g_cvW[nv & 1] = pw;
            g_cvH[nv & 1] = ph;
            fc_panel_render(&g_modeCfg, &pd, g_canvas[nv & 1], pw, ph);
            BuildAll(nv);                        // lists for the new version, spare slots
            __sync();
            g_ver = nv;                          // publish
            g_diag[D_VERSION] = nv;
            shownFps = fps;
            modeChanged = false;
        }

        if (t - tLog >= (ULONGLONG)LOG_EVERY_MS * TB_PER_MS) {
            WriteDiagFile();
            tLog = t;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Cleanup on game exit. Exiting with the Guide button kills the game without
// DLL_PROCESS_DETACH, so the kernel's title-terminate notification undoes the patch. The
// registration lives in this DLL, so the callback also unregisters itself (tested: clean exits
// back to back).
// ---------------------------------------------------------------------------------------------

static void OnTitleTerminate();
static EX_TITLE_TERMINATE_REGISTRATION g_termReg = { (void *)OnTitleTerminate, 0 };
static volatile DWORD g_termRegistered = 0;

static void OnTitleTerminate()
{
    g_stop = 1;
    RemoveHook();
    if (g_termRegistered) {
        g_termRegistered = 0;
        ExRegisterTitleTerminateNotification(&g_termReg, FALSE);
    }
}

extern "C" BOOL WINAPI DllMain(HANDLE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        if (g_termRegistered) {
            g_termRegistered = 0;
            ExRegisterTitleTerminateNotification(&g_termReg, FALSE);
        }
        g_stop = 1;
        RemoveHook();
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    ExRegisterTitleTerminateNotification(&g_termReg, TRUE);
    g_termRegistered = 1;

    // Normal title thread (flags 0), so it ends with the game
    HANDLE h = NULL;
    DWORD tid = 0;
    if (ExCreateThread(&h, 0, &tid, (PVOID)XapiThreadStartup, (LPTHREAD_START_ROUTINE)Worker, NULL, 0) >= 0 && h != NULL) {
        XSetThreadProcessor(h, 5);
        CloseHandle(h);
    }
    return TRUE;
}

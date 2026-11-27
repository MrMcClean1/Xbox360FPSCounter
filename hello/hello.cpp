/*
 * hello.cpp - Milestone 2c: on-screen debug channel.
 *
 * Peer Pressure loads this DLL before the game's entry point. On attach we
 * start a kernel thread (the same way Peer Pressure shows its own toast),
 * wait 3 seconds, then:
 *   1. pop up "fps_hello: running"
 *   2. try three marker-file writes and pop up the kernel status for each
 *      (0 = success, anything starting with C = failure code).
 *
 * Only kernel and XAM functions are used, so nothing depends on the game's
 * own startup code having run.
 */
#include <xtl.h>

/* 1 = freeze test build (game will hang if our entry point runs). Set to 0 for normal builds. */
#define HANG_TEST 0
/* 1 = crash test: entry point writes to address 0 on ANY call. */
#define CRASH_TEST 0
/* 1 = no-op test: DllMain returns TRUE immediately and does nothing else. */
#define NOOP_TEST 0

typedef long KNTSTATUS;
typedef struct _KSTR { USHORT Length; USHORT MaximumLength; char *Buffer; } KSTR;
typedef struct _KOBJATTR { HANDLE RootDirectory; KSTR *ObjectName; ULONG Attributes; } KOBJATTR;
typedef struct _KIOSB { KNTSTATUS Status; ULONG Information; } KIOSB;

extern "C" {
    void      __stdcall RtlInitAnsiString(KSTR *dst, const char *src);
    KNTSTATUS __stdcall NtCreateFile(HANDLE *h, ULONG access, KOBJATTR *oa, KIOSB *iosb,
                                     LARGE_INTEGER *alloc, ULONG attrs, ULONG share,
                                     ULONG disposition, ULONG options);
    KNTSTATUS __stdcall NtWriteFile(HANDLE h, HANDLE ev, void *apc, void *apcctx, KIOSB *iosb,
                                    const void *buf, ULONG len, LARGE_INTEGER *offset);
    KNTSTATUS __stdcall NtClose(HANDLE h);
    KNTSTATUS __stdcall ExCreateThread(HANDLE *h, ULONG stack, ULONG *tid, void *xapiStartup,
                                       void *start, void *param, ULONG flags);
    KNTSTATUS __stdcall KeDelayExecutionThread(ULONG waitMode, ULONG alertable, LARGE_INTEGER *interval);
    KNTSTATUS __stdcall XexGetModuleHandle(const char *name, HANDLE *h);
    KNTSTATUS __stdcall XexGetProcedureAddress(HANDLE h, ULONG ordinal, void **addr);
    void XapiThreadStartup(void); /* xapilib; passed to ExCreateThread as the XAPI startup routine */
}

/* xam.xex ordinal 656 */
typedef void (*XNOTIFYQUEUEUI_FN)(ULONG type, ULONG userIndex, ULONGLONG areas,
                                  const wchar_t *text, void *ctx);

#define K_GENERIC_WRITE        0x40000000
#define K_SYNCHRONIZE          0x00100000
#define K_FILE_ATTR_NORMAL     0x00000080
#define K_FILE_OVERWRITE_IF    5
#define K_FILE_SYNC_IO_NONALRT 0x00000020
#define K_FILE_NON_DIRECTORY   0x00000040
#define K_OBJ_CASE_INSENSITIVE 0x00000040

static void Toast(const wchar_t *text)
{
    HANDLE xam = 0;
    void *fn = 0;
    if (XexGetModuleHandle("xam.xex", &xam) >= 0 &&
        XexGetProcedureAddress(xam, 656, &fn) >= 0 && fn) {
        ((XNOTIFYQUEUEUI_FN)fn)(14, 0xFF, 2, text, 0);
    }
}

static void SleepMs(int ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(1, 0, &t);
}

static KNTSTATUS WriteMarker(const char *path, const char *text)
{
    KSTR name;
    KOBJATTR oa;
    KIOSB iosb;
    HANDLE h = 0;
    ULONG len = 0;
    KNTSTATUS st;

    while (text[len]) len++;
    RtlInitAnsiString(&name, path);
    oa.RootDirectory = 0;
    oa.ObjectName = &name;
    oa.Attributes = K_OBJ_CASE_INSENSITIVE;

    st = NtCreateFile(&h, K_GENERIC_WRITE | K_SYNCHRONIZE, &oa, &iosb, 0,
                      K_FILE_ATTR_NORMAL, 0, K_FILE_OVERWRITE_IF,
                      K_FILE_SYNC_IO_NONALRT | K_FILE_NON_DIRECTORY);
    if (st >= 0) {
        st = NtWriteFile(h, 0, 0, 0, &iosb, text, len, 0);
        NtClose(h);
    }
    return st;
}

/* Append "label=XXXXXXXX " to a wide buffer. */
static wchar_t *PutHex(wchar_t *p, const wchar_t *label, ULONG v)
{
    static const wchar_t hex[] = L"0123456789ABCDEF";
    int i;
    while (*label) *p++ = *label++;
    *p++ = L'=';
    for (i = 28; i >= 0; i -= 4) *p++ = hex[(v >> i) & 0xF];
    *p++ = L' ';
    *p = 0;
    return p;
}

static ULONG __stdcall DebugThread(void *param)
{
    wchar_t msg[96];
    wchar_t *p = msg;
    KNTSTATUS a, b, c;
    (void)param;

    SleepMs(10000);
    Toast(L"fps_hello: running");

    a = WriteMarker("\\Device\\Harddisk0\\Partition1\\fps_hello_nt.txt", "fps_hello (nt path)\r\n");
    b = WriteMarker("\\??\\game:\\fps_hello_game.txt", "fps_hello (game:)\r\n");
    c = WriteMarker("\\??\\Hdd1:\\fps_hello_hdd1.txt", "fps_hello (Hdd1:)\r\n");

    SleepMs(4000);
    p = PutHex(p, L"nt", (ULONG)a);
    p = PutHex(p, L"game", (ULONG)b);
    p = PutHex(p, L"hdd1", (ULONG)c);
    Toast(msg);
    return 0;
}

BOOL WINAPI DllMain(HANDLE hModule, DWORD reason, LPVOID reserved)
{
    (void)hModule; (void)reserved;
#if NOOP_TEST
    return TRUE;
#endif
#if CRASH_TEST
    *(volatile ULONG *)0 = 0xDEAD0000 | reason;
#endif
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE th = 0;
#if HANG_TEST
        /* Diagnostic: prove the entry point runs. Shows a toast, then stalls forever. */
        Toast(L"fps_hello: DllMain");
        for (;;) SleepMs(1000);
#endif
        /* Same pattern as XenonFPS: XAPI startup routine, flag 2, created suspended. */
        if (ExCreateThread(&th, 0, 0, (void *)XapiThreadStartup, (void *)DebugThread, 0,
                           2 | CREATE_SUSPENDED) >= 0 && th) {
            ResumeThread(th);
            CloseHandle(th);
        }
    }
    return TRUE;
}

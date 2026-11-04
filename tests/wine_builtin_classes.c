/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A thread can create its first window while another holds the loader lock
 * (patch 0981). Thread B creates the process's first window, so it registers
 * win32u's builtin classes and needs the loader lock for their cursors; the
 * main thread holds the loader lock, as a DllMain does, and then creates a
 * window too. Before the patch the main thread waited in the registration's
 * pthread_once for B, and B waited for the loader lock. Run it headless:
 * with a display, B first waits for explorer's desktop and the race is gone. */
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI *LOCK_FN)(ULONG, ULONG *, ULONG_PTR *);
typedef LONG (WINAPI *UNLOCK_FN)(ULONG, ULONG_PTR);

static volatile LONG main_done, b_done;
static HANDLE go;

static DWORD WINAPI watchdog(void *arg)
{
    (void)arg;
    Sleep(15000);
    if (!main_done)
    {
        /* ExitProcess needs the loader lock; TerminateProcess does not. */
        printf("builtin-classes verdict=hang thread_b=%s\n", b_done ? "done" : "waiting");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2);
    }
    return 0;
}

static DWORD WINAPI thread_b(void *arg)
{
    (void)arg;
    WaitForSingleObject(go, INFINITE);
    CreateWindowA("STATIC", "B", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    b_done = 1;
    return 0;
}

int main(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    LOCK_FN lock = (LOCK_FN)(void *)GetProcAddress(ntdll, "LdrLockLoaderLock");
    UNLOCK_FN unlock = (UNLOCK_FN)(void *)GetProcAddress(ntdll, "LdrUnlockLoaderLock");
    ULONG_PTR cookie = 0;
    HANDLE b;
    int i;

    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    go = CreateEventA(NULL, TRUE, FALSE, NULL);
    b = CreateThread(NULL, 0, thread_b, NULL, 0, NULL);
    Sleep(200);  /* B's thread attach takes the loader lock: let it finish */
    if (lock(0, NULL, &cookie))
    {
        printf("builtin-classes verdict=fail LdrLockLoaderLock\n");
        return 1;
    }
    SetEvent(go);
    /* B gets into the class registration and waits for the loader lock. */
    for (i = 0; i < 50 && !b_done; i++) Sleep(10);
    CreateWindowA("STATIC", "main", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    main_done = 1;
    unlock(0, cookie);
    if (WaitForSingleObject(b, 5000))
    {
        printf("builtin-classes verdict=hang thread_b=waiting\n");
        return 1;
    }
    /* Headless, both windows may be NULL: the check is that neither waits. */
    printf("builtin-classes verdict=pass\n");
    return 0;
}

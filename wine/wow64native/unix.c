/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Initial platform contract for the hardware WoW64 backend. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wow64native.h"
#include "wine/debug.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

WINE_DEFAULT_DEBUG_CHANNEL(wow);
#ifdef __PROSPERO__
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

/* libvulkan.prx (wine/ps5/pw_vk_radv_profile.c) counts presents and, on
 * each thread's first entry into RADV after a present, calls the frame hook
 * with the present count and the TSC. The hook is installed by name once
 * that module is loaded; until then every report retries the lookup. */
#define PW_NATIVE_FRAME_HOOK_SETTER "pw_vk_radv_profile_set_frame_hook"
typedef void (*pw_native_frame_hook)(unsigned long long frame, unsigned long long tsc);

static __thread struct pw_native_thread_state thread_state;
static __thread int thread_initialized;
#ifdef __PROSPERO__
/* libkernel's compat signal trampoline reads a host signal TCB through the
 * qword at guest TEB32.FiberData/ArbitraryUserPointer (FS:0x10). Keep the
 * upper ArbitraryUserPointer dword untouched by placing the shim below 4 GiB
 * and using FiberData's existing non-fiber sentinel offset. */
static __thread void *signal_tcb_page;
static __thread ULONG saved_fiber_data;
static __thread ULONG signal_tcb_fiber_data;
#define PW_SIGNAL_TCB_PAGE_SIZE 0x4000
#define PW_SIGNAL_TCB_POOL_SLOTS 128
static void *signal_tcb_pool;
static unsigned char signal_tcb_pool_used[PW_SIGNAL_TCB_POOL_SLOTS];
static pthread_mutex_t signal_tcb_pool_lock = PTHREAD_MUTEX_INITIALIZER;
/* A nonzero slot owns a page in signal_tcb_pool; zero means individually mapped. */
static __thread unsigned int signal_tcb_pool_slot;

static void reserve_signal_tcb_pool(void)
{
    const size_t bytes = (size_t)PW_SIGNAL_TCB_PAGE_SIZE * PW_SIGNAL_TCB_POOL_SLOTS;
    void *pool;

    if (signal_tcb_pool) return;
    pool = mmap((void *)0x10000000, bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    if (pool == MAP_FAILED)
    {
        WINE_MESSAGE("PW_NATIVE_STAGE signal-tcb-pool unavailable errno=%d bytes=%u\n",
                     errno, (unsigned)bytes);
        return;
    }
    if ((ULONG_PTR)pool > 0xffffffffu - bytes)
    {
        WINE_MESSAGE("PW_NATIVE_STAGE signal-tcb-pool out-of-range addr=%p bytes=%u\n",
                     pool, (unsigned)bytes);
        munmap(pool, bytes);
        return;
    }
    signal_tcb_pool = pool;
    WINE_MESSAGE("PW_NATIVE_STAGE signal-tcb-pool ready addr=%p slots=%u bytes=%u\n",
                 pool, PW_SIGNAL_TCB_POOL_SLOTS, (unsigned)bytes);
}

static void *alloc_signal_tcb_page(void)
{
    void *page = NULL;
    unsigned int i;

    if (signal_tcb_pool)
    {
        pthread_mutex_lock(&signal_tcb_pool_lock);
        for (i = 0; i < PW_SIGNAL_TCB_POOL_SLOTS; ++i)
            if (!signal_tcb_pool_used[i])
            {
                signal_tcb_pool_used[i] = 1;
                signal_tcb_pool_slot = i + 1;
                page = (char *)signal_tcb_pool + (size_t)i * PW_SIGNAL_TCB_PAGE_SIZE;
                break;
            }
        pthread_mutex_unlock(&signal_tcb_pool_lock);
        if (page) return page;
        WINE_MESSAGE("PW_NATIVE_STAGE signal-tcb-pool exhausted slots=%u\n",
                     PW_SIGNAL_TCB_POOL_SLOTS);
    }

    signal_tcb_pool_slot = 0;
    page = mmap((void *)0x10000000, PW_SIGNAL_TCB_PAGE_SIZE,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED)
    {
        int hint_errno = errno;
        page = mmap(NULL, PW_SIGNAL_TCB_PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
        WINE_MESSAGE("PW_NATIVE_STAGE signal-tcb-fallback hint_errno=%d page=%p errno=%d\n",
                     hint_errno, page, errno);
    }
    if (page == MAP_FAILED) return NULL;
    if ((ULONG_PTR)page > 0xffffffffu - 0x1e00)
    {
        munmap(page, PW_SIGNAL_TCB_PAGE_SIZE);
        return NULL;
    }
    return page;
}

static void free_signal_tcb_page(void)
{
    if (!signal_tcb_page) return;
    if (signal_tcb_pool_slot)
    {
        unsigned int slot = signal_tcb_pool_slot - 1;
        pthread_mutex_lock(&signal_tcb_pool_lock);
        if (slot < PW_SIGNAL_TCB_POOL_SLOTS) signal_tcb_pool_used[slot] = 0;
        pthread_mutex_unlock(&signal_tcb_pool_lock);
        signal_tcb_pool_slot = 0;
    }
    else munmap(signal_tcb_page, PW_SIGNAL_TCB_PAGE_SIZE);
    signal_tcb_page = NULL;
}
#endif
static int process_ready;
static int profile_enabled;
static unsigned long long profile_tsc_hz;
static __thread struct pw_native_profile thread_profile;
/* The counters at this thread's last frame mark, for the per-frame report. */
static __thread struct { unsigned long long frame, tsc; struct pw_native_profile counters; int primed; } frame_mark;
static int frame_hook_installed;

static unsigned long long read_tsc(void)
{
    unsigned int low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high));
    return ((unsigned long long)high << 32) | low;
}

/* The TSC rate the reports are paced by and printed with: PW_QPC_TSC_HZ when
 * the title measured it, else a 20 ms measurement against CLOCK_MONOTONIC.
 * 0 when neither works; reports then come every 262144 host switches only. */
static unsigned long long measure_tsc_hz(void)
{
    const char *text = getenv("PW_QPC_TSC_HZ");
    struct timespec start, now;
    unsigned long long tsc0, tsc1, ns;
    if (text && *text)
    {
        char *end;
        unsigned long long hz = strtoull(text, &end, 10);
        if (!*end && hz >= 100000000ull && hz <= 10000000000ull) return hz;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return 0;
    tsc0 = read_tsc();
    do
    {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
        ns = (unsigned long long)(now.tv_sec - start.tv_sec) * 1000000000ull + now.tv_nsec - start.tv_nsec;
    } while (ns < 20000000ull);
    tsc1 = read_tsc();
    return tsc1 > tsc0 ? (tsc1 - tsc0) * 1000000000ull / ns : 0;
}

/* The core clock this thread runs at: a chain of dependent 1-cycle adds
 * (eight per iteration, so the loop counter runs alongside) timed with
 * CLOCK_MONOTONIC. 2M iterations are 16M cycles: under 5 ms at 3.5 GHz,
 * 10 ms at 1.6 GHz. The TSC runs at a fixed rate whatever the core does,
 * so only this shows a lower power state. cpu is RDTSCP's TSC_AUX (the
 * CPU number where the kernel sets it). */
static void report_cpu_clock(unsigned int tid)
{
    struct timespec start, end;
    unsigned long long iterations = 2000000ull, chain = 0, ns;
    unsigned int low, high, aux;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return;
    __asm__ volatile("1:\n\t"
                     "addq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\t"
                     "addq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\t"
                     "decq %1\n\tjnz 1b"
                     : "+r"(chain), "+r"(iterations) : : "cc");
    if (clock_gettime(CLOCK_MONOTONIC, &end)) return;
    __asm__ volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(aux));
    ns = (unsigned long long)(end.tv_sec - start.tv_sec) * 1000000000ull + end.tv_nsec - start.tv_nsec;
    if (!ns || chain != 16000000ull) return;
    WINE_MESSAGE("PW_NATIVE_PROFILE cpu_clock_mhz=%llu cpu=%u tid=%04x\n", chain * 1000ull / ns, aux, tid);
}
static unsigned long long clock_next_tsc;

/* The per-frame report (version 3), from the frame hook: this thread's
 * host-side time since its previous mark, by bucket, for the frames it has
 * just seen end. It runs inside a Unix call, so host FS is active and the
 * thread's own counters are current up to the switch that entered RADV;
 * the stay in progress lands in the next frame's line. Threads that never
 * enter RADV get no frame lines. */
static void profile_frame(unsigned long long frame, unsigned long long tsc)
{
    const struct pw_native_profile *p = &thread_profile;
    if (!thread_initialized || !thread_state.profile) return;
    if (frame_mark.primed && frame > frame_mark.frame && tsc > frame_mark.tsc)
    {
        const struct pw_native_profile *was = &frame_mark.counters;
        WINE_MESSAGE("PW_NATIVE_PROFILE version=3 frame=%llu frames=%llu tid=%04x tsc=%llu tsc_hz=%llu wall=%llu "
                     "unix=%llu syscall=%llu other=%llu fs=%llu unix_calls=%llu syscall_calls=%llu\n",
                     frame, frame - frame_mark.frame, thread_state.tid, tsc, profile_tsc_hz, tsc - frame_mark.tsc,
                     p->unix_host_ticks - was->unix_host_ticks, p->syscall_host_ticks - was->syscall_host_ticks,
                     p->other_host_ticks - was->other_host_ticks,
                     (p->host_sysarch_ticks + p->guest_sysarch_ticks) - (was->host_sysarch_ticks + was->guest_sysarch_ticks),
                     p->unix_calls - was->unix_calls, p->syscall_calls - was->syscall_calls);
    }
    frame_mark.frame = frame;
    frame_mark.tsc = tsc;
    frame_mark.counters = *p;
    frame_mark.primed = 1;
}

/* Cumulative per-thread counters; tools/native_profile_split.py turns two
 * consecutive reports of a thread into an interval's split. tid is the
 * Windows thread id, as in Wine's log prefixes. */
static void profile_report(void *arg)
{
    const struct pw_native_thread_state *state = arg;
    struct pw_native_profile *p = (void *)state->profile;
    if (!p) return;
    WINE_MESSAGE("PW_NATIVE_PROFILE version=2 tid=%04x teb=%llx tsc=%llu tsc_hz=%llu host_calls=%llu guest_calls=%llu "
                 "host_sysarch_ticks=%llu guest_sysarch_ticks=%llu unix_calls=%llu syscall_calls=%llu "
                 "unix_host_ticks=%llu syscall_host_ticks=%llu other_host_ticks=%llu\n",
                 state->tid, state->guest_fs, p->last_tsc, profile_tsc_hz, p->host_calls, p->guest_calls,
                 p->host_sysarch_ticks, p->guest_sysarch_ticks, p->unix_calls, p->syscall_calls,
                 p->unix_host_ticks, p->syscall_host_ticks, p->other_host_ticks);
    p->next_report_tsc = profile_tsc_hz ? p->last_tsc + 2 * profile_tsc_hz : ~0ull;
    /* About once a minute, on whichever thread reports first after that:
     * the busy ones report most often. */
    if (profile_tsc_hz && p->last_tsc >= __atomic_load_n(&clock_next_tsc, __ATOMIC_RELAXED))
    {
        __atomic_store_n(&clock_next_tsc, p->last_tsc + 60 * profile_tsc_hz, __ATOMIC_RELAXED);
        report_cpu_clock(state->tid);
    }
#ifndef __PROSPERO__
    (void)profile_frame;    /* only the console has a module to hook */
#else
    if (!__atomic_load_n(&frame_hook_installed, __ATOMIC_ACQUIRE))
    {
        void (*set_hook)(pw_native_frame_hook) = dlsym(RTLD_DEFAULT, PW_NATIVE_FRAME_HOOK_SETTER);
        if (set_hook)
        {
            set_hook(profile_frame);
            __atomic_store_n(&frame_hook_installed, 1, __ATOMIC_RELEASE);
            WINE_MESSAGE("PW_NATIVE_PROFILE frame_hook=installed tid=%04x\n", state->tid);
        }
    }
#endif
}

static void profile_watch_register(struct pw_native_thread_state *state)
{
    unsigned int i;
    int expected = 0;
    int rc;
    clockid_t cpu_clock = 0;
    rc = pthread_once(&profile_watch_key_once, profile_watch_key_create);
    if (rc || profile_watch_key_status)
    {
        WINE_MESSAGE("PW_NATIVE_PROFILE watcher-key failed once=%d key=%d teb=%llx\n",
                     rc, profile_watch_key_status, state->guest_fs);
        return;
    }
    int cpu_clock_valid = !pthread_getcpuclockid(pthread_self(), &cpu_clock);
    if (!cpu_clock_valid)
        WINE_MESSAGE("PW_NATIVE_PROFILE cpu-clock-unavailable teb=%llx\n", state->guest_fs);
    pthread_mutex_lock(&profile_watch_lock);
    for (i = 0; i < PW_PROFILE_WATCH_MAX_THREADS; ++i)
        if (!profile_watch_entries[i].state && !profile_watch_entries[i].readers)
        {
            profile_watch_entries[i].state = state;
            profile_watch_entries[i].native_thread = pthread_self();
            profile_watch_entries[i].native_teb = (unsigned long long)(uintptr_t)NtCurrentTeb();
            profile_watch_entries[i].cpu_clock = cpu_clock;
            profile_watch_entries[i].cpu_clock_valid = cpu_clock_valid;
            profile_watch_entries[i].cpu_ns = 0;
            profile_watch_entries[i].samples = 0;
            break;
        }
    pthread_mutex_unlock(&profile_watch_lock);
    if (i == PW_PROFILE_WATCH_MAX_THREADS)
    {
        WINE_MESSAGE("PW_NATIVE_PROFILE watcher-full max=%u teb=%llx\n",
                     PW_PROFILE_WATCH_MAX_THREADS, state->guest_fs);
        return;
    }
    rc = pthread_setspecific(profile_watch_key, state);
    if (rc)
    {
        profile_watch_unregister(state);
        WINE_MESSAGE("PW_NATIVE_PROFILE watcher-key-set failed rc=%d teb=%llx\n",
                     rc, state->guest_fs);
        return;
    }
    if (!__atomic_compare_exchange_n(&profile_watch_started, &expected, 1, 0,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (pthread_create(&profile_watch_thread, NULL, profile_watch_main, NULL))
    {
        __atomic_store_n(&profile_watch_started, 0, __ATOMIC_RELAXED);
        WINE_MESSAGE("PW_NATIVE_PROFILE watcher-start failed\n");
        return;
    }
    pthread_detach(profile_watch_thread);
    WINE_MESSAGE("PW_NATIVE_PROFILE watcher-start interval=2s max_threads=%u teb=%llx\n",
                 PW_PROFILE_WATCH_MAX_THREADS, state->guest_fs);
}

static void profile_watch_unregister(struct pw_native_thread_state *state)
{
    if (profile_watch_key_status == 0)
        pthread_setspecific(profile_watch_key, NULL);
    pthread_mutex_lock(&profile_watch_lock);
    for (unsigned int i = 0; i < PW_PROFILE_WATCH_MAX_THREADS; ++i)
        if (profile_watch_entries[i].state == state)
        {
            profile_watch_entries[i].state = NULL;
            while (profile_watch_entries[i].readers)
                pthread_cond_wait(&profile_watch_cond, &profile_watch_lock);
            break;
        }
    pthread_mutex_unlock(&profile_watch_lock);
}
#endif

static NTSTATUS capture_host_fs(unsigned long long *base)
{
    *base = 0;
#ifdef __PROSPERO__
    extern int sysarch(int, void *);
    /* Called with host FS active, never from a guest-FS transition. */
    if (sysarch(128 /* AMD64_GET_FSBASE */, base)) return STATUS_UNSUCCESSFUL;
    if (!*base) return STATUS_INVALID_ADDRESS;
    return STATUS_SUCCESS;
#elif defined(__linux__) && defined(__x86_64__)
    *base = (unsigned long long)__builtin_thread_pointer();
    return STATUS_SUCCESS;
#else
    return STATUS_NOT_SUPPORTED;
#endif
}

static NTSTATUS process_init(void *args)
{
    struct pw_native_init_params *params = args;
    WINE_MESSAGE("PW_NATIVE_STAGE process_init enter args=%p version=%u expected=%u\n",
                 args, params ? params->version : 0, PW_NATIVE_ABI_VERSION);
    process_ready = 0;
    profile_enabled = 0;
    if (!params || params->version != PW_NATIVE_ABI_VERSION)
    {
        WINE_MESSAGE("PW_NATIVE_STAGE process_init reject params=%p version=%u\n",
                     args, params ? params->version : 0);
        return STATUS_INVALID_PARAMETER;
    }
    params->transitions_ready = 0;
    params->fs32_selector = 0;
    params->fs_set_proc = 0;
#ifdef __PROSPERO__
    {
        extern int sysarch(int, void *);
        unsigned int (*query)(unsigned int);
        unsigned int caps;
        /* Resolve through ntdll's Wine dl facade: this optional export is
         * absent in older runtimes, which must keep the backend disabled. */
        query = dlsym(RTLD_DEFAULT, PW_NATIVE_SIGNAL_QUERY);
        WINE_MESSAGE("PW_NATIVE_STAGE process_init query=%p\n", query);
        if (!query)
        {
            WINE_MESSAGE("PW_NATIVE_STAGE process_init missing-query\n");
            return STATUS_NOT_SUPPORTED;
        }
        caps = query(PW_NW64_CAPS_VERSION);
        profile_pc_query = dlsym(RTLD_DEFAULT, "__wine_prospero_native_profile_sample");
        /* PS4 libkernel's signal trampoline treats guest TEB FS:0x10 as its
         * native thread pointer. Synthetic SIGPROF therefore faults before
         * reaching the sampler while native WoW64 runs with guest FS active.
         * Keep PC sampling opt-in until the per-thread signal/TLS bridge is
         * fixed; ordinary CPU-time and transition counters need no signal. */
        profile_pc_enabled = (caps & 0x20u) && profile_pc_query &&
                             getenv("PW_NATIVE_PC") && !strcmp(getenv("PW_NATIVE_PC"), "1");
        WINE_MESSAGE("PW_NATIVE_STAGE process_init caps=%08x required=%08x\n",
                     caps, PW_NW64_CAP_REQUIRED);
        WINE_MESSAGE("PW_NATIVE_STAGE process_init pc_sampler=%d query=%p\n",
                     profile_pc_enabled, profile_pc_query);
        if ((caps & PW_NW64_CAP_REQUIRED) != PW_NW64_CAP_REQUIRED)
        {
            WINE_MESSAGE("PW_NATIVE_STAGE process_init insufficient-caps\n");
            return STATUS_NOT_SUPPORTED;
        }
        reserve_signal_tcb_pool();
        params->fs_set_proc = (unsigned long long)sysarch;
        params->transitions_ready = 1;
        process_ready = 1;
        /* Keep the hot transition path free of profiling unless explicitly
         * requested by the game's diagnostic profile. */
        {
            const char *value = getenv("PW_NATIVE_PROFILE");
            profile_enabled = value && !strcmp(value, "1");
            if (profile_enabled)
            {
                profile_tsc_hz = measure_tsc_hz();
                report_cpu_clock(0);    /* at startup, before any thread report */
                clock_next_tsc = read_tsc() + 60 * profile_tsc_hz;
            }
        }
        WINE_MESSAGE("PW_NATIVE_STAGE process_init ready fs_set_proc=%p profile=%d\n",
                     (void *)(ULONG_PTR)params->fs_set_proc, profile_enabled);
        return STATUS_SUCCESS;
    }
#endif
    /* Non-console builds never authorize native console execution. */
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS thread_init(void *args)
{
    struct pw_native_thread_params *params = args;
    NTSTATUS status;
    WINE_MESSAGE("PW_NATIVE_STAGE thread_init enter args=%p version=%u expected=%u teb=%08x initialized=%d\n",
                 args, params ? params->version : 0, PW_NATIVE_ABI_VERSION,
                 params ? (unsigned)params->guest_teb : 0, thread_initialized);
    if (!params || params->version != PW_NATIVE_ABI_VERSION)
    {
        WINE_MESSAGE("PW_NATIVE_STAGE thread_init reject params=%p version=%u\n",
                     args, params ? params->version : 0);
        return STATUS_INVALID_PARAMETER;
    }
    params->state = 0;
    if (params->guest_teb < 0x10000 || params->guest_teb > 0xfffff000)
    {
        WINE_MESSAGE("PW_NATIVE_STAGE thread_init invalid-teb teb=%08x\n",
                     (unsigned)params->guest_teb);
        return STATUS_INVALID_ADDRESS;
    }
    if (thread_initialized)
    {
        if (thread_state.guest_fs != params->guest_teb)
        {
            WINE_MESSAGE("PW_NATIVE_STAGE thread_init teb-mismatch old=%llx new=%llx\n",
                         thread_state.guest_fs, params->guest_teb);
            return STATUS_INVALID_PARAMETER;
        }
        thread_state.status = process_ready ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
        params->state = (unsigned long long)&thread_state;
        WINE_MESSAGE("PW_NATIVE_STAGE thread_init reuse state=%p status=%08x ready=%d\n",
                     &thread_state, (unsigned)thread_state.status, process_ready);
        return thread_state.status;
    }
    status = capture_host_fs(&thread_state.host_fs);
    WINE_MESSAGE("PW_NATIVE_STAGE thread_init host-fs status=%08x base=%llx\n",
                 (unsigned)status, thread_state.host_fs);
    if (status) return status;
#ifdef __PROSPERO__
    {
        const size_t page_size = 0x4000;
        const ULONG fiber_offset = 0x1e00;
        ULONG *fiber_data = (ULONG *)(ULONG_PTR)(params->guest_teb + 0x10);
        void *page = alloc_signal_tcb_page();
        WINE_MESSAGE("PW_NATIVE_STAGE thread_init tcb-page=%p slot=%u\n",
                     page, signal_tcb_pool_slot);
        if (!page)
        {
            WINE_MESSAGE("PW_NATIVE_STAGE thread_init tcb-page-unavailable errno=%d\n", errno);
            return STATUS_NO_MEMORY;
        }
        memset(page, 0, page_size);
        signal_tcb_page = page;
        saved_fiber_data = *fiber_data;
        signal_tcb_fiber_data = (ULONG)((ULONG_PTR)page + fiber_offset);
        *fiber_data = signal_tcb_fiber_data;
        WINE_MESSAGE("PW_NATIVE_STAGE thread_init tcb teb=%08x page=%p fiber=%08x previous=%08x\n",
                     (unsigned)params->guest_teb, page, signal_tcb_fiber_data, saved_fiber_data);
        WINE_MESSAGE("PW_NATIVE_SIGNAL_TCB guest_teb=%08x page=%p fiber=%08x previous=%08x\n",
                     (unsigned)params->guest_teb, page, signal_tcb_fiber_data, saved_fiber_data);
    }
#endif
    thread_profile = (struct pw_native_profile){0};
    frame_mark.primed = 0;
    if (profile_enabled)
    {
        thread_profile.report_proc = (unsigned long long)profile_report;
        thread_state.profile = (unsigned long long)&thread_profile;
    }
    thread_state.guest_fs = params->guest_teb;
    thread_state.tid = params->tid;
    thread_state.status = process_ready ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    thread_initialized = 1;
    params->state = (unsigned long long)&thread_state;
    WINE_MESSAGE("PW_NATIVE_STAGE thread_init ready state=%p status=%08x ready=%d\n",
                 &thread_state, (unsigned)thread_state.status, process_ready);
#ifdef __PROSPERO__
    /* The optional watcher samples thread-local state from another thread.
     * Its POSIX-key destructor removes entries when a thread exits without
     * reaching BTCpuThreadTerm; enable it only for targeted diagnostics. */
    if (profile_watch_enabled)
        profile_watch_register(&thread_state);
#endif
    return thread_state.status;
}

static NTSTATUS thread_get(void *args)
{
    struct pw_native_thread_params *params = args;
    if (!params || params->version != PW_NATIVE_ABI_VERSION) return STATUS_INVALID_PARAMETER;
    params->state = 0;
    if (!thread_initialized || thread_state.guest_fs != params->guest_teb)
        return STATUS_INVALID_PARAMETER;
    thread_state.status = process_ready ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    params->state = (unsigned long long)&thread_state;
    return thread_state.status;
}

static NTSTATUS thread_term(void *args)
{
#ifdef __PROSPERO__
    WINE_MESSAGE("PW_NATIVE_STAGE thread_term enter args=%p initialized=%d teb=%llx page=%p\n",
                 args, thread_initialized, thread_state.guest_fs, signal_tcb_page);
    if (signal_tcb_page)
    {
        ULONG *fiber_data = (ULONG *)(ULONG_PTR)(thread_state.guest_fs + 0x10);
        if (*fiber_data == signal_tcb_fiber_data) *fiber_data = saved_fiber_data;
        free_signal_tcb_page();
        saved_fiber_data = signal_tcb_fiber_data = 0;
    }
#endif
    profile_report(&thread_state);
#ifdef __PROSPERO__
    profile_watch_unregister(&thread_state);
#endif
    thread_profile = (struct pw_native_profile){0};
    frame_mark.primed = 0;
    thread_state = (struct pw_native_thread_state){0};
    thread_initialized = 0;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = { process_init, thread_init, thread_get, thread_term };
C_ASSERT(sizeof(struct pw_native_thread_state) == 32);
C_ASSERT(sizeof(struct pw_native_init_params) == 24);
C_ASSERT(sizeof(struct pw_native_thread_params) == 24);
C_ASSERT(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == pw_native_funcs_count);

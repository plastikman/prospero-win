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
#ifdef __PROSPERO__
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(wow);
#ifdef __PROSPERO__
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

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
static int profile_watch_enabled;
static __thread struct pw_native_profile thread_profile;
#ifdef __PROSPERO__
#define PW_PROFILE_WATCH_MAX_THREADS 64
/* The test title can sit in guest code forever, so BTCpuThreadTerm is not a
 * reliable place to collect counters. A low-rate host thread samples native
 * threads while they are alive. */
struct pw_profile_watch_entry
{
    struct pw_native_thread_state *state;
    unsigned int readers;
    pthread_t native_thread;
    unsigned long long native_teb;
    clockid_t cpu_clock;
    int cpu_clock_valid;
    unsigned long long cpu_ns;
    unsigned long long host_calls, guest_calls, unix_calls, syscall_calls;
    unsigned int samples;
};
typedef unsigned int (*pw_native_pc_query)(unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *,
                                           unsigned long long *, unsigned long long *);
static pw_native_pc_query profile_pc_query;
static int profile_pc_enabled;
static pthread_mutex_t profile_watch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t profile_watch_cond = PTHREAD_COND_INITIALIZER;
static struct pw_profile_watch_entry profile_watch_entries[PW_PROFILE_WATCH_MAX_THREADS];
static pthread_t profile_watch_thread;
static volatile int profile_watch_started;
static pthread_key_t profile_watch_key;
static pthread_once_t profile_watch_key_once = PTHREAD_ONCE_INIT;
static int profile_watch_key_status = -1;
static void profile_watch_unregister(struct pw_native_thread_state *state);

/* BTCpuThreadTerm is not reached on every native thread exit. Use a POSIX
 * thread-key destructor as a second cleanup path so the watcher never keeps
 * a pointer into a thread's TLS after pthread teardown. */
static void profile_watch_key_destructor(void *value)
{
    if (value) profile_watch_unregister(value);
}

static void profile_watch_key_create(void)
{
    profile_watch_key_status = pthread_key_create(&profile_watch_key,
                                                  profile_watch_key_destructor);
}
#endif
static void profile_read(const struct pw_native_thread_state *state,
                         unsigned long long *host_calls, unsigned long long *guest_calls,
                         unsigned long long *host_ticks, unsigned long long *guest_ticks,
                         unsigned long long *unix_calls, unsigned long long *syscall_calls,
                         unsigned long long *last_tsc)
{
    const struct pw_native_profile *p = (void *)state->profile;
    if (!p) return;
    *host_calls = __atomic_load_n(&p->host_calls, __ATOMIC_RELAXED);
    *guest_calls = __atomic_load_n(&p->guest_calls, __ATOMIC_RELAXED);
    *host_ticks = __atomic_load_n(&p->host_sysarch_ticks, __ATOMIC_RELAXED);
    *guest_ticks = __atomic_load_n(&p->guest_sysarch_ticks, __ATOMIC_RELAXED);
    *unix_calls = __atomic_load_n(&p->unix_calls, __ATOMIC_RELAXED);
    *syscall_calls = __atomic_load_n(&p->syscall_calls, __ATOMIC_RELAXED);
    *last_tsc = __atomic_load_n(&p->last_tsc, __ATOMIC_RELAXED);
}

static void profile_report_active_syscalls(const struct pw_native_thread_state *state,
                                           unsigned long long syscall_calls)
{
    const struct pw_native_profile *p = (void *)state->profile;
    unsigned int end;
    if (!p) return;
    end = (unsigned int)(syscall_calls >> PW_NATIVE_PROFILE_SAMPLE_SHIFT);
    WINE_MESSAGE("PW_NATIVE_ACTIVE_SYSCALLS teb=%llx calls=%llu pc=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x caller=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x id=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                 state->guest_fs, syscall_calls,
                 p->syscall_samples[(end - 0) & 63], p->syscall_samples[(end - 1) & 63],
                 p->syscall_samples[(end - 2) & 63], p->syscall_samples[(end - 3) & 63],
                 p->syscall_samples[(end - 4) & 63], p->syscall_samples[(end - 5) & 63],
                 p->syscall_samples[(end - 6) & 63], p->syscall_samples[(end - 7) & 63],
                 p->syscall_samples[(end - 8) & 63], p->syscall_samples[(end - 9) & 63],
                 p->syscall_samples[(end - 10) & 63], p->syscall_samples[(end - 11) & 63],
                 p->syscall_samples[(end - 12) & 63], p->syscall_samples[(end - 13) & 63],
                 p->syscall_samples[(end - 14) & 63], p->syscall_samples[(end - 15) & 63],
                 p->syscall_caller_samples[(end - 0) & 63], p->syscall_caller_samples[(end - 1) & 63],
                 p->syscall_caller_samples[(end - 2) & 63], p->syscall_caller_samples[(end - 3) & 63],
                 p->syscall_caller_samples[(end - 4) & 63], p->syscall_caller_samples[(end - 5) & 63],
                 p->syscall_caller_samples[(end - 6) & 63], p->syscall_caller_samples[(end - 7) & 63],
                 p->syscall_caller_samples[(end - 8) & 63], p->syscall_caller_samples[(end - 9) & 63],
                 p->syscall_caller_samples[(end - 10) & 63], p->syscall_caller_samples[(end - 11) & 63],
                 p->syscall_caller_samples[(end - 12) & 63], p->syscall_caller_samples[(end - 13) & 63],
                 p->syscall_caller_samples[(end - 14) & 63], p->syscall_caller_samples[(end - 15) & 63],
                 p->syscall_id_samples[(end - 0) & 63], p->syscall_id_samples[(end - 1) & 63],
                 p->syscall_id_samples[(end - 2) & 63], p->syscall_id_samples[(end - 3) & 63],
                 p->syscall_id_samples[(end - 4) & 63], p->syscall_id_samples[(end - 5) & 63],
                 p->syscall_id_samples[(end - 6) & 63], p->syscall_id_samples[(end - 7) & 63],
                 p->syscall_id_samples[(end - 8) & 63], p->syscall_id_samples[(end - 9) & 63],
                 p->syscall_id_samples[(end - 10) & 63], p->syscall_id_samples[(end - 11) & 63],
                 p->syscall_id_samples[(end - 12) & 63], p->syscall_id_samples[(end - 13) & 63],
                 p->syscall_id_samples[(end - 14) & 63], p->syscall_id_samples[(end - 15) & 63]);
}

static void profile_report(void *arg)
{
    const struct pw_native_thread_state *state = arg;
    unsigned long long host_calls = 0, guest_calls = 0, host_ticks = 0, guest_ticks = 0;
    unsigned long long unix_calls = 0, syscall_calls = 0, last_tsc = 0;
    const struct pw_native_profile *p = (void *)state->profile;
    unsigned int sys_end, unix_end;
    if (!p) return;
    profile_read(state, &host_calls, &guest_calls, &host_ticks, &guest_ticks,
                 &unix_calls, &syscall_calls, &last_tsc);
    WINE_MESSAGE("PW_NATIVE_PROFILE version=2 teb=%llx tsc=%llu host_calls=%llu guest_calls=%llu host_sysarch_ticks=%llu guest_sysarch_ticks=%llu unix_calls=%llu syscall_calls=%llu\n",
                 state->guest_fs, last_tsc, host_calls, guest_calls,
                 host_ticks, guest_ticks, unix_calls, syscall_calls);
    sys_end = (unsigned int)(syscall_calls >> PW_NATIVE_PROFILE_SAMPLE_SHIFT);
    unix_end = (unsigned int)(unix_calls >> PW_NATIVE_PROFILE_SAMPLE_SHIFT);
    WINE_MESSAGE("PW_NATIVE_GUEST_PC teb=%llx syscall=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x unix=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                 state->guest_fs,
                 p->syscall_samples[(sys_end - 0) & 63], p->syscall_samples[(sys_end - 1) & 63],
                 p->syscall_samples[(sys_end - 2) & 63], p->syscall_samples[(sys_end - 3) & 63],
                 p->syscall_samples[(sys_end - 4) & 63], p->syscall_samples[(sys_end - 5) & 63],
                 p->syscall_samples[(sys_end - 6) & 63], p->syscall_samples[(sys_end - 7) & 63],
                 p->syscall_samples[(sys_end - 8) & 63], p->syscall_samples[(sys_end - 9) & 63],
                 p->syscall_samples[(sys_end - 10) & 63], p->syscall_samples[(sys_end - 11) & 63],
                 p->syscall_samples[(sys_end - 12) & 63], p->syscall_samples[(sys_end - 13) & 63],
                 p->syscall_samples[(sys_end - 14) & 63], p->syscall_samples[(sys_end - 15) & 63],
                 p->unix_samples[(unix_end - 0) & 63], p->unix_samples[(unix_end - 1) & 63],
                 p->unix_samples[(unix_end - 2) & 63], p->unix_samples[(unix_end - 3) & 63],
                 p->unix_samples[(unix_end - 4) & 63], p->unix_samples[(unix_end - 5) & 63],
                 p->unix_samples[(unix_end - 6) & 63], p->unix_samples[(unix_end - 7) & 63],
                 p->unix_samples[(unix_end - 8) & 63], p->unix_samples[(unix_end - 9) & 63],
                 p->unix_samples[(unix_end - 10) & 63], p->unix_samples[(unix_end - 11) & 63],
                 p->unix_samples[(unix_end - 12) & 63], p->unix_samples[(unix_end - 13) & 63],
                 p->unix_samples[(unix_end - 14) & 63], p->unix_samples[(unix_end - 15) & 63]);
    WINE_MESSAGE("PW_NATIVE_SYSCALL_CALLER teb=%llx caller=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                 state->guest_fs,
                 p->syscall_caller_samples[(sys_end - 0) & 63], p->syscall_caller_samples[(sys_end - 1) & 63],
                 p->syscall_caller_samples[(sys_end - 2) & 63], p->syscall_caller_samples[(sys_end - 3) & 63],
                 p->syscall_caller_samples[(sys_end - 4) & 63], p->syscall_caller_samples[(sys_end - 5) & 63],
                 p->syscall_caller_samples[(sys_end - 6) & 63], p->syscall_caller_samples[(sys_end - 7) & 63],
                 p->syscall_caller_samples[(sys_end - 8) & 63], p->syscall_caller_samples[(sys_end - 9) & 63],
                 p->syscall_caller_samples[(sys_end - 10) & 63], p->syscall_caller_samples[(sys_end - 11) & 63],
                 p->syscall_caller_samples[(sys_end - 12) & 63], p->syscall_caller_samples[(sys_end - 13) & 63],
                 p->syscall_caller_samples[(sys_end - 14) & 63], p->syscall_caller_samples[(sys_end - 15) & 63]);
}

#ifdef __PROSPERO__
static void *profile_watch_main(void *unused)
{
    unsigned int owner_tid = 0, owner_scan_cycles = 0;
    int scan_owner;
    (void)unused;
    for (;;)
    {
        struct pw_profile_watch_entry snapshot[PW_PROFILE_WATCH_MAX_THREADS];
        unsigned int slots[PW_PROFILE_WATCH_MAX_THREADS];
        unsigned int count = 0;
        sleep(2);
        scan_owner = owner_scan_cycles != 0;
        if (owner_scan_cycles) --owner_scan_cycles;
        pthread_mutex_lock(&profile_watch_lock);
        for (unsigned int i = 0; i < PW_PROFILE_WATCH_MAX_THREADS; ++i)
        {
            struct pw_profile_watch_entry *entry = &profile_watch_entries[i];
            if (!entry->state) continue;
            ++entry->readers;
            snapshot[count] = *entry;
            slots[count++] = i;
        }
        pthread_mutex_unlock(&profile_watch_lock);

        for (unsigned int n = 0; n < count; ++n)
        {
            struct pw_profile_watch_entry sample = snapshot[n];
            unsigned long long host_calls = 0, guest_calls = 0, host_ticks = 0, guest_ticks = 0;
            unsigned long long unix_calls = 0, syscall_calls = 0, last_tsc = 0;
            unsigned long long cpu_ns = 0, cpu_delta_ns = 0;
            struct timespec cpu_time;
            int changed;
            int pc_active;
            profile_read(sample.state, &host_calls, &guest_calls, &host_ticks, &guest_ticks,
                         &unix_calls, &syscall_calls, &last_tsc);
            if (sample.cpu_clock_valid && !clock_gettime(sample.cpu_clock, &cpu_time))
            {
                cpu_ns = (unsigned long long)cpu_time.tv_sec * 1000000000ull + cpu_time.tv_nsec;
                if (sample.samples && cpu_ns >= sample.cpu_ns) cpu_delta_ns = cpu_ns - sample.cpu_ns;
            }
            pc_active = cpu_delta_ns > 100000000ull;
            if (profile_pc_enabled && (pc_active || scan_owner))
            {
                unsigned long long before = 0, sequence = 0, rip = 0, rsp = 0, cs = 0;
                unsigned long long rax = 0, rdi = 0, rsi = 0, rdx = 0, r10 = 0, rbp = 0, rsp0 = 0;
                unsigned long long sample_teb = 0, frames64[8] = {0};
                unsigned long long stack_limit = 0, stack_base = 0, stack_words[8] = {0};
                unsigned long long stack_deep_words[4] = {0};
                unsigned long long rsp_words[4] = {0};
                unsigned long long umtx_words[4] = {0};
                unsigned long long native_tid = 0;
                int owner_match, is_waiter, is_owner, sample_valid;
                unsigned int frames[8] = {0};
                unsigned int frame_count = 0;
                int rc;
                profile_pc_query(&before, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                rc = pthread_kill(sample.native_thread, SIGPROF);
                if (!rc)
                {
                    struct timespec pause = {0, 1000000};
                    for (unsigned int attempt = 0; attempt < 50; ++attempt)
                    {
                        nanosleep(&pause, NULL);
                        profile_pc_query(&sequence, &rip, &rsp, &cs, &rax, &rdi, &rsi, &rdx, &r10, &rbp, &rsp0,
                                         &sample_teb, frames64, &stack_limit, &stack_base, stack_words,
                                         stack_deep_words, rsp_words, umtx_words, &native_tid);
                        if (sequence != before) break;
                    }
                    sample_valid = sequence != before;
                    owner_match = sample_valid && sample_teb == sample.native_teb;
                    is_waiter = owner_match && (cs & 0xffff) == 0x43 && rax == 0x1c6 && rsi == 0x11;
                    if (is_waiter)
                    {
                        unsigned int sampled_owner = (unsigned int)umtx_words[0] & 0x7fffffff;
                        if (sampled_owner != owner_tid)
                        {
                            owner_tid = sampled_owner;
                            owner_scan_cycles = 2;
                        }
                        if (owner_scan_cycles) scan_owner = 1;
                    }
                    is_owner = sample_valid && owner_tid && native_tid == owner_tid;
                    if (scan_owner)
                        WINE_MESSAGE("PW_NATIVE_OWNER_SCAN target=%x slot=%u guest_teb=%llx native_teb=%llx sample_teb=%llx sample_valid=%d native_tid=%llx target_match=%d signal_rc=%d seq=%llu rip=%llx cs=%llx rax=%llx\n",
                                     owner_tid, slots[n], sample.state->guest_fs, sample.native_teb,
                                     sample_teb, sample_valid, native_tid, is_owner, rc,
                                     sequence, rip, cs, rax);
                    if ((owner_match && (pc_active || is_waiter)) || is_owner)
                    {
                    /* Walk at most four conventional 32-bit EBP frames. The
                     * return-address-only sample was not enough to validate
                     * the caller in stripped game DLLs. Keep every read near
                     * the sampled guest stack and stop on a non-monotonic or
                     * implausible frame pointer. This is opt-in diagnostic
                     * code and runs on the watcher thread. */
                    if (owner_match && (cs & 0xffff) == 0x33 && rbp >= rsp && rbp - rsp < 0x100000 &&
                        rbp < 0x7ffff000)
                    {
                        unsigned long long frame = rbp;
                        for (unsigned int i = 0; i < 4; ++i)
                        {
                            unsigned int previous, ret;
                            if (frame < rsp || frame - rsp >= 0x100000 || frame >= 0x7ffff000) break;
                            memcpy(&previous, (const void *)(uintptr_t)frame, sizeof(previous));
                            memcpy(&ret, (const void *)(uintptr_t)(frame + sizeof(previous)), sizeof(ret));
                            frames[2 * i] = previous;
                            frames[2 * i + 1] = ret;
                            frame_count++;
                            if (previous <= frame || previous - rsp >= 0x100000 || previous >= 0x7ffff000) break;
                            frame = previous;
                        }
                    }
                    WINE_MESSAGE("PW_NATIVE_PC teb=%llx native_teb=%llx sample_teb=%llx owner_match=%d seq=%llu rip=%llx rsp=%llx rsp0=%llx rbp=%llx ret=%08x frames=%u[%08x/%08x,%08x/%08x,%08x/%08x,%08x/%08x] host_frames=[%llx/%llx,%llx/%llx,%llx/%llx,%llx/%llx] host_bounds=%llx-%llx host_words=[%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx] host_deep=[%llx,%llx,%llx,%llx] rsp_words=[%llx,%llx,%llx,%llx] umtx_words=[%llx,%llx,%llx,%llx] native_tid=%llx umtx_owner_tid=%x umtx_owner_match=%d cs=%llx rax=%llx rdi=%llx rsi=%llx rdx=%llx r10=%llx sample=%s\n",
                                 sample.state->guest_fs, sample.native_teb, sample_teb, owner_match, sequence, rip, rsp, rsp0, rbp, frames[1], frame_count,
                                 frames[0], frames[1], frames[2], frames[3], frames[4], frames[5],
                                 frames[6], frames[7], frames64[0], frames64[1], frames64[2], frames64[3],
                                 frames64[4], frames64[5], frames64[6], frames64[7], stack_limit, stack_base,
                                 stack_words[0], stack_words[1], stack_words[2], stack_words[3],
                                 stack_words[4], stack_words[5], stack_words[6], stack_words[7],
                                 stack_deep_words[0], stack_deep_words[1], stack_deep_words[2], stack_deep_words[3],
                                 rsp_words[0], rsp_words[1], rsp_words[2], rsp_words[3],
                                 umtx_words[0], umtx_words[1], umtx_words[2], umtx_words[3], native_tid, owner_tid, is_owner, cs,
                                 rax, rdi, rsi, rdx, r10,
                                 sequence != before ? "ok" : "timeout");
                    }
                }
                else WINE_MESSAGE("PW_NATIVE_PC teb=%llx signal-error=%d\n", sample.state->guest_fs, rc);
            }
            changed = !sample.samples || host_calls != sample.host_calls ||
                      guest_calls != sample.guest_calls || unix_calls != sample.unix_calls ||
                      syscall_calls != sample.syscall_calls;
            if (sample.state->profile && (changed || !(sample.samples % 5)))
                WINE_MESSAGE("PW_NATIVE_PROFILE version=3 teb=%llx tsc=%llu cpu_ns=%llu cpu_delta_ns=%llu host_calls=%llu guest_calls=%llu host_sysarch_ticks=%llu guest_sysarch_ticks=%llu unix_calls=%llu syscall_calls=%llu samples=%u changed=%d\n",
                             sample.state->guest_fs, last_tsc, cpu_ns, cpu_delta_ns,
                             host_calls, guest_calls, host_ticks, guest_ticks, unix_calls, syscall_calls,
                             sample.samples, changed);
            if (sample.state->profile && cpu_delta_ns > 100000000ull)
                profile_report_active_syscalls(sample.state, syscall_calls);

            pthread_mutex_lock(&profile_watch_lock);
            struct pw_profile_watch_entry *entry = &profile_watch_entries[slots[n]];
            if (entry->state == sample.state)
            {
                entry->cpu_ns = cpu_ns;
                entry->host_calls = host_calls;
                entry->guest_calls = guest_calls;
                entry->unix_calls = unix_calls;
                entry->syscall_calls = syscall_calls;
                ++entry->samples;
            }
            if (--entry->readers == 0) pthread_cond_broadcast(&profile_watch_cond);
            pthread_mutex_unlock(&profile_watch_lock);
        }
    }
    return NULL;
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
            const char *profile = getenv("PW_NATIVE_PROFILE");
            const char *watch = getenv("PW_NATIVE_WATCH");
            const char *pc = getenv("PW_NATIVE_PC");
            profile_enabled = profile && !strcmp(profile, "1");
            profile_watch_enabled = (watch && !strcmp(watch, "1")) ||
                                    (pc && !strcmp(pc, "1"));
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
    if (profile_enabled)
    {
        thread_profile.report_proc = (unsigned long long)profile_report;
        thread_state.profile = (unsigned long long)&thread_profile;
    }
    thread_state.guest_fs = params->guest_teb;
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
    thread_state = (struct pw_native_thread_state){0};
    thread_initialized = 0;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = { process_init, thread_init, thread_get, thread_term };
C_ASSERT(sizeof(struct pw_native_thread_state) == 32);
C_ASSERT(sizeof(struct pw_native_init_params) == 24);
C_ASSERT(sizeof(struct pw_native_thread_params) == 24);
C_ASSERT(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == pw_native_funcs_count);

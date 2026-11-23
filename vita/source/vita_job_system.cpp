#include "vita_job_system.hpp"
#include "vita_profiler.hpp"
#include <atomic>

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
#include <psp2/kernel/threadmgr.h>
#endif

namespace outrun::vita_jobs {

namespace {

struct Slice {
    std::uint32_t begin{};
    std::uint32_t end{};
};

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_completed{0};
bool g_in_parallel = false;

JobFn g_current_fn = nullptr;
void* g_current_user = nullptr;
Slice g_slices[2]{};

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
SceUID g_evf = -1;
SceUID g_worker_c2 = -1;

int worker_entry(SceSize /*args*/, void* /*argp*/) {
    for (;;) {
        unsigned int out_bits = 0;
        const int res = sceKernelWaitEventFlag(
            g_evf, 1u,
            SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT,
            &out_bits, nullptr);
        if (res < 0) break;
        if (g_shutdown.load(std::memory_order_acquire)) break;

        JobFn fn = g_current_fn;
        void* user = g_current_user;
        const std::uint32_t b = g_slices[1].begin;
        const std::uint32_t e = g_slices[1].end;
        if (fn && b < e) {
            const SceUInt64 t0 = sceKernelGetProcessTimeWide();
            fn(b, e, 1u, user);
            outrun::vita_profile::g_core2_busy_us.fetch_add(sceKernelGetProcessTimeWide() - t0, std::memory_order_relaxed);
        }
        g_completed.fetch_add(1, std::memory_order_release);
    }
    return 0;
}
#endif

} // namespace

void init() {
    if (g_initialized.load(std::memory_order_acquire)) return;
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    // Core 0 = vitaGL Render Thread (vita_render_thread)
    // Core 1 = Main/Game Thread (caller)
    // Core 2 = Parallel CPU Worker (or2_worker_c2)
    g_shutdown.store(false, std::memory_order_release);
    g_evf = sceKernelCreateEventFlag("or2_jobs_evf", 0, 0, nullptr);
    if (g_evf < 0) return;

    int prio = sceKernelGetThreadCurrentPriority();
    if (prio <= 0) prio = 0x10000100;

    g_worker_c2 = sceKernelCreateThread(
        "or2_worker_c2", worker_entry, prio, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (g_worker_c2 >= 0) {
        if (sceKernelStartThread(g_worker_c2, 0, nullptr) >= 0) {
            g_initialized.store(true, std::memory_order_release);
        }
    }
#endif
}

void shutdown() {
    if (!g_initialized.exchange(false, std::memory_order_acq_rel)) return;
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    g_shutdown.store(true, std::memory_order_release);
    if (g_evf >= 0) {
        sceKernelSetEventFlag(g_evf, 1u);
    }
    if (g_worker_c2 >= 0) {
        sceKernelWaitThreadEnd(g_worker_c2, nullptr, nullptr);
        sceKernelDeleteThread(g_worker_c2);
        g_worker_c2 = -1;
    }
    if (g_evf >= 0) {
        sceKernelDeleteEventFlag(g_evf);
        g_evf = -1;
    }
#endif
}

bool is_initialized() {
    return g_initialized.load(std::memory_order_relaxed);
}

void parallel_for(std::uint32_t count, std::uint32_t min_chunk, JobFn fn, void* user_data) {
    if (!fn || count == 0) return;

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_initialized.load(std::memory_order_relaxed) || g_in_parallel || count < min_chunk * 2u) {
        fn(0, count, 0, user_data);
        return;
    }

    g_in_parallel = true;
    const std::uint32_t mid = count / 2u;
    g_slices[0] = {0u, mid};
    g_slices[1] = {mid, count};
    g_current_fn = fn;
    g_current_user = user_data;
    g_completed.store(0, std::memory_order_release);

    // Wake Core 2 while Core 1 executes Slice 0 (Core 0 stays 100% free for vitaGL!)
    sceKernelSetEventFlag(g_evf, 1u);

    if (g_slices[0].begin < g_slices[0].end) {
        fn(g_slices[0].begin, g_slices[0].end, 0u, user_data);
    }

    std::uint32_t spins = 0;
    while (g_completed.load(std::memory_order_acquire) < 1) {
        if (++spins < 4000u) {
#if defined(__arm__)
            asm volatile("yield" ::: "memory");
#endif
        } else {
            sceKernelDelayThread(0);
        }
    }

    g_in_parallel = false;
#else
    (void)min_chunk;
    fn(0, count, 0, user_data);
#endif
}

} // namespace outrun::vita_jobs

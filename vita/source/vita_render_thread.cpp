#include "vita_render_thread.hpp"
#include "vita_profiler.hpp"
#include <cstdint>

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
#include <psp2/kernel/threadmgr.h>
#endif

namespace outrun::vita_render {

namespace {

enum class MsgKind : std::uint8_t { None = 0, Sync = 1, Async = 2, Shutdown = 3 };

struct RenderMsg {
    MsgKind kind{MsgKind::None};
    const std::function<void()>* sync_fn{nullptr};
    void (*async_fn)(void*){nullptr};
    void* async_arg{nullptr};
    std::atomic<bool>* slot_busy{nullptr};
};

// Single-producer (Core 1) / single-consumer (Core 0) lock-free ring of 8 messages.
constexpr std::uint32_t kRingSize = 8u;
RenderMsg g_ring[kRingSize]{};
std::atomic<std::uint32_t> g_head{0}; // written by Core 1
std::atomic<std::uint32_t> g_tail{0}; // written by Core 0
std::atomic<bool> g_sync_done{false};
std::atomic<bool> g_initialized{false};

#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
SceUID g_thread = -1;
SceUID g_evf = -1;
constexpr unsigned int kEvWake = 1u;

int render_thread_entry(SceSize /*args*/, void* /*argp*/) {
    for (;;) {
        std::uint32_t t = g_tail.load(std::memory_order_relaxed);
        std::uint32_t h = g_head.load(std::memory_order_acquire);
        if (t == h) {
            unsigned int out_bits = 0;
            if (sceKernelWaitEventFlag(g_evf, kEvWake, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, &out_bits, nullptr) < 0) {
                break;
            }
            continue;
        }
        RenderMsg msg = g_ring[t & (kRingSize - 1u)];
        if (msg.kind == MsgKind::Shutdown) {
            g_tail.store(t + 1u, std::memory_order_release);
            break;
        } else if (msg.kind == MsgKind::Sync) {
            const SceUInt64 t0 = sceKernelGetProcessTimeWide();
            if (msg.sync_fn) (*msg.sync_fn)();
            outrun::vita_profile::g_core0_busy_us.fetch_add(sceKernelGetProcessTimeWide() - t0, std::memory_order_relaxed);
            g_tail.store(t + 1u, std::memory_order_release);
            g_sync_done.store(true, std::memory_order_release);
        } else if (msg.kind == MsgKind::Async) {
            const SceUInt64 t0 = sceKernelGetProcessTimeWide();
            if (msg.async_fn) msg.async_fn(msg.async_arg);
            outrun::vita_profile::g_core0_busy_us.fetch_add(sceKernelGetProcessTimeWide() - t0, std::memory_order_relaxed);
            if (msg.slot_busy) msg.slot_busy->store(false, std::memory_order_release);
            g_tail.store(t + 1u, std::memory_order_release);
        } else {
            g_tail.store(t + 1u, std::memory_order_release);
        }
    }
    return 0;
}

inline void spin_pause(std::uint32_t& spins) {
    if (++spins < 4000u) {
#if defined(__arm__)
        asm volatile("yield" ::: "memory");
#endif
    } else {
        sceKernelDelayThread(100);
    }
}
#endif

} // namespace

void init() {
    if (g_initialized.load(std::memory_order_acquire)) return;
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    // Pin main/game thread to Core 1; Core 0 is 100% dedicated to vitaGL!
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);

    g_evf = sceKernelCreateEventFlag("or2_vitagl_evf", 0, 0, nullptr);
    if (g_evf < 0) return;

    int prio = sceKernelGetThreadCurrentPriority();
    if (prio <= 0) prio = 0x10000100;
    // Slightly higher priority (smaller number) on Core 0 so vitaGL wakes immediately
    const int render_prio = (prio > 64) ? (prio - 2) : prio;

    g_thread = sceKernelCreateThread(
        "or2_vitagl_c0", render_thread_entry, render_prio,
        512 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (g_thread >= 0) {
        if (sceKernelStartThread(g_thread, 0, nullptr) >= 0) {
            g_initialized.store(true, std::memory_order_release);
        }
    }
#endif
}

void shutdown() {
    if (!g_initialized.exchange(false, std::memory_order_acq_rel)) return;
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    wait_idle();
    const std::uint32_t h = g_head.load(std::memory_order_relaxed);
    g_ring[h & (kRingSize - 1u)] = RenderMsg{MsgKind::Shutdown, nullptr, nullptr, nullptr, nullptr};
    g_head.store(h + 1u, std::memory_order_release);
    sceKernelSetEventFlag(g_evf, kEvWake);
    if (g_thread >= 0) {
        sceKernelWaitThreadEnd(g_thread, nullptr, nullptr);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
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

bool is_render_thread() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    return g_thread >= 0 && sceKernelGetThreadId() == g_thread;
#else
    return true;
#endif
}

void wait_slot(const std::atomic<bool>& slot_busy) {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_initialized.load(std::memory_order_relaxed) || is_render_thread()) return;
    std::uint32_t spins = 0;
    while (slot_busy.load(std::memory_order_acquire)) {
        spin_pause(spins);
    }
#else
    (void)slot_busy;
#endif
}

void wait_idle() {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_initialized.load(std::memory_order_relaxed) || is_render_thread()) return;
    std::uint32_t spins = 0;
    while (g_tail.load(std::memory_order_acquire) != g_head.load(std::memory_order_relaxed)) {
        spin_pause(spins);
    }
#endif
}

void run_sync(const std::function<void()>& fn) {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_initialized.load(std::memory_order_relaxed) || is_render_thread()) {
        fn();
        return;
    }
    wait_idle();
    g_sync_done.store(false, std::memory_order_release);
    const std::uint32_t h = g_head.load(std::memory_order_relaxed);
    g_ring[h & (kRingSize - 1u)] = RenderMsg{MsgKind::Sync, &fn, nullptr, nullptr, nullptr};
    g_head.store(h + 1u, std::memory_order_release);
    sceKernelSetEventFlag(g_evf, kEvWake);

    std::uint32_t spins = 0;
    while (!g_sync_done.load(std::memory_order_acquire)) {
        spin_pause(spins);
    }
#else
    fn();
#endif
}

void submit_async(void (*fn)(void*), void* arg, std::atomic<bool>* slot_busy) {
#if defined(OR2_VITA) || defined(__VITA__) || defined(__psp2__)
    if (!g_initialized.load(std::memory_order_relaxed) || is_render_thread()) {
        if (fn) fn(arg);
        if (slot_busy) slot_busy->store(false, std::memory_order_release);
        return;
    }
    if (slot_busy) slot_busy->store(true, std::memory_order_release);
    // Ensure ring has space (should always have space with double buffering)
    std::uint32_t spins = 0;
    std::uint32_t h = g_head.load(std::memory_order_relaxed);
    while ((h - g_tail.load(std::memory_order_acquire)) >= (kRingSize - 1u)) {
        spin_pause(spins);
    }
    g_ring[h & (kRingSize - 1u)] = RenderMsg{MsgKind::Async, nullptr, fn, arg, slot_busy};
    g_head.store(h + 1u, std::memory_order_release);
    sceKernelSetEventFlag(g_evf, kEvWake);
#else
    if (fn) fn(arg);
    if (slot_busy) slot_busy->store(false, std::memory_order_release);
#endif
}

} // namespace outrun::vita_render

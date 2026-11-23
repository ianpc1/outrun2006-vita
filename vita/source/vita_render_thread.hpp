#pragma once
#include <atomic>
#include <functional>

namespace outrun::vita_render {

void init();
void shutdown();
bool is_initialized();
bool is_render_thread();

// Waits for any in-flight async frame to complete, then executes fn() on Core 0 (vitaGL thread).
void run_sync(const std::function<void()>& fn);

// Submits an async frame command buffer to Core 0 and returns IMMEDIATELY to Core 1.
// Sets *slot_busy = true before waking Core 0; Core 0 sets *slot_busy = false when done.
void submit_async(void (*fn)(void*), void* arg, std::atomic<bool>* slot_busy);

// Waits until a specific double-buffer slot is no longer in use by Core 0.
void wait_slot(const std::atomic<bool>& slot_busy);

// Waits until Core 0 is completely idle (no async frame in flight).
void wait_idle();

} // namespace outrun::vita_render

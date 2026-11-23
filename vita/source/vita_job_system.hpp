#pragma once
#include <cstdint>

namespace outrun::vita_jobs {

using JobFn = void(*)(std::uint32_t begin, std::uint32_t end, std::uint32_t worker_idx, void* user_data);

void init();
void shutdown();
bool is_initialized();

// Splits [0, count) across all 3 Vita user cores (Core 0 = caller, Core 1 = worker 1, Core 2 = worker 2).
// worker_idx is 0 for Core 0, 1 for Core 1, 2 for Core 2.
void parallel_for(std::uint32_t count, std::uint32_t min_chunk, JobFn fn, void* user_data);

} // namespace outrun::vita_jobs

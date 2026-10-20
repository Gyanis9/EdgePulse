#include "Base/Runtime/IdGenerator.h"

#include <cstdint>
#include <atomic>
#include <chrono>

namespace EdgePulse::Common
{
    std::int64_t nextIdentifier()
    {
        static std::atomic<std::uint64_t> s_sequence{0};
        const auto                        milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        // 低 20 位留给同毫秒内的并发调用；跨毫秒天然递增，便于按 ID 近似排序
        const std::uint64_t sequence = s_sequence.fetch_add(1, std::memory_order_relaxed) & 0xFFFFFULL;
        return static_cast<std::int64_t>((static_cast<std::uint64_t>(milliseconds) << 20U) | sequence);
    }
} // namespace EdgePulse::Common

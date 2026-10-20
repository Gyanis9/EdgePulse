#include "Base/Runtime/IdGenerator.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace EdgePulse::Common
{
    namespace
    {
        // 只留 10 位序列：毫秒时间左移 10 位后仍远小于 2^53，管理端的 JS 数字解析不会丢精度
        constexpr std::uint64_t kSequenceBits = 10;
        constexpr std::uint64_t kSequenceMask = (std::uint64_t{1} << kSequenceBits) - 1;
    } // namespace

    std::int64_t nextIdentifier()
    {
        static std::atomic<std::uint64_t> s_sequence{0};
        const auto                        milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        // 低 10 位留给同毫秒内的并发调用；跨毫秒天然递增，便于按 ID 近似排序
        const std::uint64_t sequence = s_sequence.fetch_add(1, std::memory_order_relaxed) & kSequenceMask;
        return static_cast<std::int64_t>((static_cast<std::uint64_t>(milliseconds) << kSequenceBits) | sequence);
    }
} // namespace EdgePulse::Common

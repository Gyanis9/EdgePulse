/**
 * @file EventLoopRunner.h
 * @brief 在测试线程外驱动事件循环并等待协程结果
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace EdgePulse::TestSupport
{
    /**
     * @brief 自持事件循环的协程运行器
     * @details 协程帧交给本对象保管到析构；测试线程只等 future，不直接读循环内部状态。
     */
    class EventLoopRunner
    {
    public:
        EventLoopRunner() : m_loopThread([this] { m_loop.run(); })
        {
        }

        ~EventLoopRunner()
        {
            // 循环停止后先 join，再销毁挂起的帧；成员声明顺序保证帧晚于线程销毁
            m_loop.stop();
            if (m_loopThread.joinable())
            {
                m_loopThread.join();
            }
        }

        EventLoopRunner(const EventLoopRunner &)            = delete;
        EventLoopRunner &operator=(const EventLoopRunner &) = delete;

        /// 事件循环，供仓储与测试协程使用
        [[nodiscard]] AsynGyanis::Core::EventLoop &loop() noexcept
        {
            return m_loop;
        }

        /**
         * @brief 调度一个协程并等待其结果
         * @tparam ResultType 协程返回值
         * @param task 惰性任务
         * @return 协程结果
         * @throws std::runtime_error 五秒内未完成
         * @throws 协程内抛出的任意异常
         */
        template<typename ResultType>
        ResultType run(AsynGyanis::Core::Task<ResultType> task)
        {
            auto                     promise = std::make_shared<std::promise<ResultType>>();
            auto                     future  = promise->get_future();
            AsynGyanis::Core::Task<> wrapper = runAndCapture(std::move(task), promise);
            m_loop.scheduler().scheduleRemote(wrapper.handle());
            m_pendingRequests.push_back(std::move(wrapper));
            if (future.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
            {
                throw std::runtime_error("协程未在五秒内完成");
            }
            return future.get();
        }

    private:
        template<typename ResultType>
        static AsynGyanis::Core::Task<> runAndCapture(AsynGyanis::Core::Task<ResultType> task, std::shared_ptr<std::promise<ResultType>> promise)
        {
            try
            {
                if constexpr (std::is_void_v<ResultType>)
                {
                    co_await task;
                    promise->set_value();
                } else
                {
                    promise->set_value(co_await task);
                }
            } catch (...)
            {
                // 异常跨线程只能通过 future 交回；在循环线程上重抛会直接终止进程
                promise->set_exception(std::current_exception());
            }
        }

        AsynGyanis::Core::EventLoop           m_loop;            ///< 被驱动的循环
        std::jthread                          m_loopThread;      ///< 循环承载线程
        std::vector<AsynGyanis::Core::Task<>> m_pendingRequests; ///< 任务帧，活到运行器析构
    };
} // namespace EdgePulse::TestSupport

/**
 * @file AgentLoop.h
 * @brief Agent 用例共用的事件循环夹具：在后台线程驱动循环并等待协程结果
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace EdgePulse::Agent
{
    /**
     * @brief 自持事件循环与协程帧的执行器
     * @details 协程帧留在本对象里，直到循环停止后才销毁，避免帧与循环的销毁顺序颠倒。
     */
    class AgentLoop
    {
    public:
        AgentLoop() : m_thread([this] { m_loop.run(); })
        {
        }

        ~AgentLoop()
        {
            stop();
        }

        AgentLoop(const AgentLoop &)            = delete;
        AgentLoop &operator=(const AgentLoop &) = delete;

        /**
         * @brief 停止并等待循环线程退出
         * @note 依赖循环的对象必须在调用本函数之后才析构，客户端连接池不能在循环运行时收尾
         */
        void stop()
        {
            m_loop.stop();
            if (m_thread.joinable())
            {
                m_thread.join();
            }
        }

        /**
         * @brief 取循环引用
         * @return AsynGyanis::Core::EventLoop & 本夹具驱动的循环
         */
        [[nodiscard]] AsynGyanis::Core::EventLoop &loop() noexcept
        {
            return m_loop;
        }

        /**
         * @brief 把一条协程投到循环上并等待结果
         * @tparam T 协程返回值类型
         * @param operation 待执行的协程
         * @param timeout 等待上限
         * @param what 用例失败时的动作名
         * @return 有时限内完成时返回结果；超时或抛出异常时为空
         */
        template<typename T>
        std::optional<T> run(AsynGyanis::Core::Task<T> operation, const std::chrono::seconds timeout, const char *what)
        {
            auto promise = std::make_shared<std::promise<T>>();
            auto future  = promise->get_future();
            auto frame   = forward(std::move(operation), promise);
            m_loop.scheduler().scheduleRemote(frame.handle());
            m_frames.push_back(std::move(frame));

            if (future.wait_for(timeout) != std::future_status::ready)
            {
                ADD_FAILURE() << what << " 未在时限内完成";
                return std::nullopt;
            }
            try
            {
                return future.get();
            } catch (const std::exception &failure)
            {
                ADD_FAILURE() << what << " 抛出异常：" << failure.what();
                return std::nullopt;
            }
        }

        /**
         * @brief 把一条无返回值的协程投到循环上并等待完成
         * @param operation 待执行的协程
         * @param timeout 等待上限
         * @param what 用例失败时的动作名
         * @return 在时限内正常完成返回 true
         */
        bool run(AsynGyanis::Core::Task<> operation, const std::chrono::seconds timeout, const char *what)
        {
            auto promise = std::make_shared<std::promise<void>>();
            auto future  = promise->get_future();
            auto frame   = forwardVoid(std::move(operation), promise);
            m_loop.scheduler().scheduleRemote(frame.handle());
            m_frames.push_back(std::move(frame));

            if (future.wait_for(timeout) != std::future_status::ready)
            {
                ADD_FAILURE() << what << " 未在时限内完成";
                return false;
            }
            try
            {
                future.get();
                return true;
            } catch (const std::exception &failure)
            {
                ADD_FAILURE() << what << " 抛出异常：" << failure.what();
                return false;
            }
        }

    private:
        /**
         * @brief 把有返回值的协程结果转交给承诺
         * @tparam T 协程返回值类型
         * @param operation 待执行的协程
         * @param promise 结果通道
         * @return Task<> 转发协程
         */
        template<typename T>
        static AsynGyanis::Core::Task<> forward(AsynGyanis::Core::Task<T> operation, std::shared_ptr<std::promise<T>> promise)
        {
            try
            {
                promise->set_value(co_await std::move(operation));
            } catch (...)
            {
                promise->set_exception(std::current_exception());
            }
            co_return;
        }

        /**
         * @brief 把无返回值的协程完成事件转交给承诺
         * @param operation 待执行的协程
         * @param promise 完成通道
         * @return Task<> 转发协程
         */
        static AsynGyanis::Core::Task<> forwardVoid(AsynGyanis::Core::Task<> operation, std::shared_ptr<std::promise<void>> promise)
        {
            try
            {
                co_await std::move(operation);
                promise->set_value();
            } catch (...)
            {
                promise->set_exception(std::current_exception());
            }
            co_return;
        }

        AsynGyanis::Core::EventLoop               m_loop;   ///< 执行 Agent 协程的循环
        std::jthread                              m_thread; ///< 驱动循环的线程
        std::vector<AsynGyanis::Core::Task<>>     m_frames; ///< 协程帧，活到本对象析构
    };

    /**
     * @brief 在用例离开作用域时先停掉 Agent 循环
     * @details 依赖循环的客户端与分发器通常声明在循环之后，本守卫必须最后声明，才能保证它们先析构。
     */
    class LoopStopGuard
    {
    public:
        /**
         * @brief 记录要停止的循环
         * @param loop 受保护的循环
         */
        explicit LoopStopGuard(AgentLoop &loop) : m_loop(loop)
        {
        }

        ~LoopStopGuard()
        {
            m_loop.stop();
        }

        LoopStopGuard(const LoopStopGuard &)            = delete;
        LoopStopGuard &operator=(const LoopStopGuard &) = delete;

    private:
        AgentLoop &m_loop; ///< 受保护的循环
    };
} // namespace EdgePulse::Agent

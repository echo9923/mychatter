#pragma once

#include <atomic>
#include <cstdint>

namespace llfc {

class SessionLifecycle final
{
public:
	/**
	 * @brief 会话生命周期状态机（Open -> Draining/Closing -> Closed，只进不退）
	 *
	 * 与 CSession 的对应关系：
	 * - Open     ：正常服务阶段。TCP 连接存活，可接收/发送业务消息、绑定用户（TrySetUserId）、
	 *              心跳检测；IsOpen() 为 true，Send/Start/绑定用户等操作均被允许。
	 * - Draining ：优雅排空阶段。SendAndClose 已入队终帧（如错误响应）后进入，期间新的
	 *              Send 一律被拒绝（_send_lock 内置关闭标志），等待发送队列排空后由
	 *              HandleWrite 触发 Close；若对端不读，排空定时器 _drain_timer 超时也会
	 *              强制 Close。对应“正在写完最后一帧、即将关闭”的过渡状态。
	 * - Closing  ：关闭进行阶段。Close() 已通过 BeginClose 抢占成功（幂等，重复调用直接返回），
	 *              socket 正在 cancel/shutdown/close，会话即将从 CServer 连接表摘除、
	 *              清理在线状态（CleanupUserPresence）。对应“TCP 已失效、清理尚未完成”。
	 * - Closed   ：终态。CompleteClose 已在 socket 所属 IO 线程完成，socket 已关闭，
	 *              会话不再接受任何工作，只等引用计数归零后析构。
	 */
	enum class State : std::uint8_t
	{
		Open,
		Draining,
		Closing,
		Closed
	};

	SessionLifecycle() noexcept = default;
	SessionLifecycle(const SessionLifecycle&) = delete;
	SessionLifecycle& operator=(const SessionLifecycle&) = delete;

	bool IsOpen() const noexcept
	{
		return _state.load(std::memory_order_acquire) == State::Open;
	}

	State GetState() const noexcept
	{
		return _state.load(std::memory_order_acquire);
	}

	bool BeginDrain() noexcept
	{
		State expected = State::Open;
		return _state.compare_exchange_strong(
			expected, State::Draining,
			std::memory_order_acq_rel, std::memory_order_acquire);
	}

	bool BeginClose() noexcept
	{
		State current = _state.load(std::memory_order_acquire);
		while (current == State::Open || current == State::Draining) {
			if (_state.compare_exchange_weak(
				current, State::Closing,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
				return true;
			}
		}
		return false;
	}

	bool CompleteClose() noexcept
	{
		State expected = State::Closing;
		return _state.compare_exchange_strong(
			expected, State::Closed,
			std::memory_order_acq_rel, std::memory_order_acquire);
	}

private:
	std::atomic<State> _state{State::Open};
};

} // namespace llfc

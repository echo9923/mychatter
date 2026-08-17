#pragma once

#include <atomic>
#include <cstdint>

namespace llfc {

class SessionLifecycle final
{
public:
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

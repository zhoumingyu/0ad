/* Copyright (C) 2024 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef INCLUDED_FUTURE
#define INCLUDED_FUTURE

#include "ps/FutureForward.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>

template<typename Callback>
class PackagedTask;

namespace FutureSharedStateDetail
{
enum class Status
{
	PENDING,
	STARTED,
	DONE,
	CANCELED
};

template<typename T>
using ResultHolder = std::conditional_t<std::is_void_v<T>, std::nullopt_t, std::optional<T>>;

/**
 * Responsible for syncronization between the task and the receiving thread.
 */
template<typename ResultType>
class Receiver : public ResultHolder<ResultType>
{
	static constexpr bool VoidResult = std::is_same_v<ResultType, void>;
public:
	Receiver() :
		ResultHolder<ResultType>{std::nullopt}
	{}
	~Receiver()
	{
		ENSURE(IsDoneOrCanceled());
	}

	Receiver(const Receiver&) = delete;
	Receiver(Receiver&&) = delete;

	bool IsDoneOrCanceled() const
	{
		return m_Status == Status::DONE || m_Status == Status::CANCELED;
	}

	void Wait()
	{
		// Fast path: we're already done.
		if (IsDoneOrCanceled())
			return;
		// Slow path: we aren't done when we run the above check. Lock and wait until we are.
		std::unique_lock<std::mutex> lock(m_Mutex);
		m_ConditionVariable.wait(lock, [this]() -> bool { return IsDoneOrCanceled(); });
	}

	/**
	 * If the task is pending, cancel it: the status becomes CANCELED and if the task was completed, the result is destroyed.
	 * @return true if the task was indeed cancelled, false otherwise (the task is running or already done).
	 */
	bool Cancel()
	{
		Status expected = Status::PENDING;
		bool cancelled = m_Status.compare_exchange_strong(expected, Status::CANCELED);
		// If we're done, invalidate, if we're pending, atomically cancel, otherwise fail.
		if (cancelled || m_Status == Status::DONE)
		{
			if (m_Status == Status::DONE)
				m_Status = Status::CANCELED;
			if constexpr (!VoidResult)
				this->reset();
			m_ConditionVariable.notify_all();
			return cancelled;
		}
		return false;
	}

	/**
	 * Move the result away from the shared state, mark the future invalid.
	 */
	template<typename _ResultType = ResultType>
	std::enable_if_t<!std::is_same_v<_ResultType, void>, ResultType> GetResult()
	{
		// The caller must ensure that this is only called if we have a result.
		ENSURE(this->has_value());
		m_Status = Status::CANCELED;
		ResultType ret = std::move(**this);
		this->reset();
		return ret;
	}

	std::atomic<Status> m_Status = Status::PENDING;
	std::mutex m_Mutex;
	std::condition_variable m_ConditionVariable;
};

/**
 * The shared state between futures and packaged state.
 */
template<typename Callback>
struct SharedState
{
	SharedState(Callback&& callbackFunc) :
		callback{std::forward<Callback>(callbackFunc)}
	{}

	Callback callback;
	Receiver<std::invoke_result_t<Callback>> receiver;
};

} // namespace FutureSharedStateDetail

/**
 * Corresponds to std::future.
 * Unlike std::future, Future can request the cancellation of the task that would produce the result.
 * This makes it more similar to Java's CancellableTask or C#'s Task.
 * The name Future was kept over Task so it would be more familiar to C++ users,
 * but this all should be revised once Concurrency TS wraps up.
 *
 * Future is _not_ thread-safe. Call it from a single thread or ensure synchronization externally.
 *
 * The callback never runs after the @p Future is destroyed.
 * TODO:
 *  - Handle exceptions.
 */
template<typename ResultType>
class Future
{
	template<typename T>
	friend class PackagedTask;

	static constexpr bool VoidResult = std::is_same_v<ResultType, void>;

	using Status = FutureSharedStateDetail::Status;
public:
	Future() = default;
	Future(const Future& o) = delete;
	Future(Future&&) = default;
	Future& operator=(Future&& other)
	{
		CancelOrWait();
		m_Receiver = std::move(other.m_Receiver);
		return *this;
	}
	~Future()
	{
		CancelOrWait();
	}

	/**
	 * Make the future wait for the result of @a callback.
	 */
	template<typename Callback>
	PackagedTask<Callback> Wrap(Callback&& callback);

	/**
	 * Move the result out of the future, and invalidate the future.
	 * If the future is not complete, calls Wait().
	 * If the future is canceled, asserts.
	 */
	template<typename SfinaeType = ResultType>
	std::enable_if_t<!std::is_same_v<SfinaeType, void>, ResultType> Get()
	{
		ENSURE(!!m_Receiver);

		Wait();
		if constexpr (VoidResult)
			return;
		else
		{
			ENSURE(m_Receiver->m_Status != Status::CANCELED);

			// This mark the state invalid - can't call Get again.
			return m_Receiver->GetResult();
		}
	}

	/**
	 * @return true if the shared state is valid and has a result (i.e. Get can be called).
	 */
	bool IsReady() const
	{
		return !!m_Receiver && m_Receiver->m_Status == Status::DONE;
	}

	/**
	 * @return true if the future has a shared state and it's not been invalidated, ie. pending, started or done.
	 */
	bool Valid() const
	{
		return !!m_Receiver && m_Receiver->m_Status != Status::CANCELED;
	}

	void Wait()
	{
		if (Valid())
			m_Receiver->Wait();
	}

	/**
	 * Cancels the task, waiting if the task is currently started.
	 * Use this function over Cancel() if you need to ensure determinism (i.e. in the simulation).
	 * @see Cancel.
	 */
	void CancelOrWait()
	{
		if (!Valid())
			return;
		if (!m_Receiver->Cancel())
			m_Receiver->Wait();
		m_Receiver.reset();
	}

protected:
	std::shared_ptr<FutureSharedStateDetail::Receiver<ResultType>> m_Receiver;
};

/**
 * Corresponds somewhat to std::packaged_task.
 * Like packaged_task, this holds a function acting as a promise.
 * This type is mostly just the shared state and the call operator,
 * handling the promise & continuation logic.
 */
template<typename Callback>
class PackagedTask
{
public:
	PackagedTask() = delete;
	PackagedTask(std::shared_ptr<FutureSharedStateDetail::SharedState<Callback>> ss) :
		m_SharedState(std::move(ss))
	{}

	void operator()()
	{
		FutureSharedStateDetail::Status expected = FutureSharedStateDetail::Status::PENDING;
		if (!m_SharedState->receiver.m_Status.compare_exchange_strong(expected,
			FutureSharedStateDetail::Status::STARTED))
		{
			return;
		}

		if constexpr (std::is_void_v<std::invoke_result_t<Callback>>)
			m_SharedState->callback();
		else
			m_SharedState->receiver.emplace(m_SharedState->callback());

		// Because we might have threads waiting on us, we need to make sure that they either:
		// - don't wait on our condition variable
		// - receive the notification when we're done.
		// This requires locking the mutex (@see Wait).
		{
			std::lock_guard<std::mutex> lock(m_SharedState->receiver.m_Mutex);
			m_SharedState->receiver.m_Status = FutureSharedStateDetail::Status::DONE;
		}

		m_SharedState->receiver.m_ConditionVariable.notify_all();

		// We no longer need the shared state, drop it immediately.
		m_SharedState.reset();
	}

	void Cancel()
	{
		m_SharedState->Cancel();
		m_SharedState.reset();
	}

private:
	std::shared_ptr<FutureSharedStateDetail::SharedState<Callback>> m_SharedState;
};

template<typename ResultType>
template<typename Callback>
PackagedTask<Callback> Future<ResultType>::Wrap(Callback&& callback)
{
	static_assert(std::is_same_v<std::invoke_result_t<Callback>, ResultType>,
		"The return type of the wrapped function is not the same as the type the Future expects.");
	CancelOrWait();
	auto temp = std::make_shared<FutureSharedStateDetail::SharedState<Callback>>(std::move(callback));
	m_Receiver = {temp, &temp->receiver};
	return PackagedTask<Callback>(std::move(temp));
}

#endif // INCLUDED_FUTURE

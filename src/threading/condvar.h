// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <mutex>

#ifdef __vita__

#include <climits>
#include <deque>
#include <psp2/kernel/threadmgr.h>

/*
	Condition variable for the PlayStation Vita, built on kernel semaphores.
	The condition variables of its pthreads library can lose wakeups, which
	left threads waiting forever.

	Every waiting thread sleeps on a semaphore of its own and is queued while
	it still holds the mutex, so a notify that follows a state change made
	under the mutex always finds it. A notify only wakes threads that were
	already waiting: a thread that starts waiting later can't take the
	wakeup meant for another one (a semaphore shared by all waiters would
	allow that, which hung the two-way SSCSM channel). Like
	std::condition_variable, wait in a loop that checks the condition.
*/
class CondVar
{
public:
	CondVar() = default;
	CondVar(const CondVar &) = delete;
	CondVar &operator=(const CondVar &) = delete;

	void wait(std::unique_lock<std::mutex> &lock)
	{
		const SceUID sema = threadSemaphore();
		{
			std::lock_guard guard(m_waiters_mutex);
			m_waiters.push_back(sema);
		}
		lock.unlock();
		sceKernelWaitSema(sema, 1, nullptr);
		lock.lock();
	}

	template <typename Predicate>
	void wait(std::unique_lock<std::mutex> &lock, Predicate pred)
	{
		while (!pred())
			wait(lock);
	}

	void notify_one()
	{
		SceUID sema = -1;
		{
			std::lock_guard guard(m_waiters_mutex);
			if (!m_waiters.empty()) {
				sema = m_waiters.front();
				m_waiters.pop_front();
			}
		}
		if (sema >= 0)
			sceKernelSignalSema(sema, 1);
	}

	void notify_all()
	{
		std::deque<SceUID> waiters;
		{
			std::lock_guard guard(m_waiters_mutex);
			waiters.swap(m_waiters);
		}
		for (SceUID sema : waiters)
			sceKernelSignalSema(sema, 1);
	}

private:
	// One semaphore per thread, signalled once per queued wait
	static SceUID threadSemaphore()
	{
		struct ThreadSemaphore {
			SceUID id = sceKernelCreateSema("luanti_condvar", 0, 0, 1, nullptr);
			~ThreadSemaphore() { sceKernelDeleteSema(id); }
		};
		thread_local ThreadSemaphore sema;
		return sema.id;
	}

	std::mutex m_waiters_mutex;
	std::deque<SceUID> m_waiters;
};

#else

#include <condition_variable>

using CondVar = std::condition_variable;

#endif

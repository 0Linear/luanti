// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <mutex>

#ifdef __vita__

#include <atomic>
#include <climits>
#include <psp2/kernel/threadmgr.h>

/*
	Condition variable on top of a kernel semaphore, for the PlayStation Vita.
	The condition variables of its pthreads library can lose wakeups, which
	left threads waiting forever.

	A waiter registers itself while it still holds the mutex, so a notify
	that follows a state change made under the mutex always sees it. Like
	std::condition_variable, it may wake up spuriously: always wait in a
	loop that checks the condition.
*/
class CondVar
{
public:
	CondVar()
	{
		m_sema = sceKernelCreateSema("luanti_condvar", 0, 0, INT_MAX, nullptr);
	}

	~CondVar()
	{
		sceKernelDeleteSema(m_sema);
	}

	CondVar(const CondVar &) = delete;
	CondVar &operator=(const CondVar &) = delete;

	void wait(std::unique_lock<std::mutex> &lock)
	{
		m_waiters.fetch_add(1);
		lock.unlock();
		sceKernelWaitSema(m_sema, 1, nullptr);
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
		int waiters = m_waiters.load();
		while (waiters > 0) {
			if (m_waiters.compare_exchange_weak(waiters, waiters - 1)) {
				sceKernelSignalSema(m_sema, 1);
				return;
			}
		}
	}

	void notify_all()
	{
		int waiters = m_waiters.exchange(0);
		if (waiters > 0)
			sceKernelSignalSema(m_sema, waiters);
	}

private:
	SceUID m_sema;
	std::atomic<int> m_waiters{0};
};

#else

#include <condition_variable>

using CondVar = std::condition_variable;

#endif

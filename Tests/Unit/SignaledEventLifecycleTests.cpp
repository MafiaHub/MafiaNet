/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/signaled_event.h"
#include "mafianet/get_time.h"

using namespace MafiaNet;

/*
SignaledEvent had no tests, and it had a hole: on POSIX the condition variable and
mutex are created by InitEvent, not by the constructor, while SetEvent called
pthread_cond_broadcast unconditionally. Broadcasting on an uninitialised
pthread_cond_t is undefined, and on glibc it does not fail -- it blocks forever in
__condvar_quiesce_and_switch_g1. Any caller that signalled before starting its
threads hung, with no diagnostic.

That is reachable through a public API: ThreadPool::AddInput signals
quitAndIncomingDataEvents, and nothing requires StartThreads first. It is also
platform-specific, which is why it went unnoticed -- on Windows SetEvent on
INVALID_HANDLE_VALUE just fails, and on macOS the uninitialised broadcast happens
to return.

The cases below cover the lifecycle out of order: signal and wait before init,
init twice, close without init, and init again after close. Each timing
assertion is deliberately loose -- they are there to turn a hang into a failure,
not to measure anything.
*/

namespace
{
	// A wait is "prompt" if it returns far sooner than the timeout it was given.
	const unsigned int kGenerousMS = 2000;
}

TEST(SignaledEventLifecycle, SetEventBeforeInitEventReturns)
{
	SignaledEvent e;

	const TimeMS start = GetTimeMS();
	e.SetEvent(); // hangs forever against the unguarded version
	const TimeMS elapsed = GetTimeMS() - start;

	ASSERT_LT(elapsed, kGenerousMS) << "SetEvent blocked before InitEvent";
}

TEST(SignaledEventLifecycle, WaitOnEventBeforeInitEventReturnsPromptly)
{
	SignaledEvent e;

	const TimeMS start = GetTimeMS();
	e.WaitOnEvent(30000);
	const TimeMS elapsed = GetTimeMS() - start;

	ASSERT_LT(elapsed, kGenerousMS) << "WaitOnEvent blocked with no condition variable";
}

// The signal still has to be recorded, so the state is not lost just because the
// condition variable did not exist yet.
TEST(SignaledEventLifecycle, ASignalRaisedBeforeInitIsStillDeliveredAfterIt)
{
	SignaledEvent e;

	e.SetEvent();
	e.InitEvent();

	const TimeMS start = GetTimeMS();
	e.WaitOnEvent(30000);
	const TimeMS elapsed = GetTimeMS() - start;

	e.CloseEvent();

	ASSERT_LT(elapsed, kGenerousMS) << "the signal raised before InitEvent was lost";
}

TEST(SignaledEventLifecycle, SetThenWaitAfterInitReturnsImmediately)
{
	SignaledEvent e;
	e.InitEvent();

	e.SetEvent();

	const TimeMS start = GetTimeMS();
	e.WaitOnEvent(30000);
	const TimeMS elapsed = GetTimeMS() - start;

	e.CloseEvent();

	ASSERT_LT(elapsed, kGenerousMS) << "a set event did not satisfy the wait";
}

// A second InitEvent must not re-create the condition variable: pthread_cond_init
// on an already-initialised one is undefined.
TEST(SignaledEventLifecycle, InitEventTwiceIsSafe)
{
	SignaledEvent e;
	e.InitEvent();
	e.InitEvent();

	e.SetEvent();

	const TimeMS start = GetTimeMS();
	e.WaitOnEvent(30000);
	const TimeMS elapsed = GetTimeMS() - start;

	e.CloseEvent();

	ASSERT_LT(elapsed, kGenerousMS);
}

TEST(SignaledEventLifecycle, CloseEventWithoutInitEventIsSafe)
{
	SignaledEvent e;
	e.CloseEvent();
	e.CloseEvent();

	// And it is still usable afterwards.
	e.InitEvent();
	e.SetEvent();

	const TimeMS start = GetTimeMS();
	e.WaitOnEvent(30000);
	const TimeMS elapsed = GetTimeMS() - start;

	e.CloseEvent();

	ASSERT_LT(elapsed, kGenerousMS);
}

// Stop/start cycles close and re-open the event, which has to work.
TEST(SignaledEventLifecycle, InitEventAfterCloseEventWorksAgain)
{
	SignaledEvent e;

	e.InitEvent();
	e.SetEvent();
	e.WaitOnEvent(1000);
	e.CloseEvent();

	e.InitEvent();
	e.SetEvent();

	const TimeMS start = GetTimeMS();
	e.WaitOnEvent(30000);
	const TimeMS elapsed = GetTimeMS() - start;

	e.CloseEvent();

	ASSERT_LT(elapsed, kGenerousMS) << "the event did not work after being re-initialised";
}

// Signalling after a close must be a no-op rather than a broadcast on a destroyed
// condition variable.
TEST(SignaledEventLifecycle, SetEventAfterCloseEventReturns)
{
	SignaledEvent e;
	e.InitEvent();
	e.CloseEvent();

	const TimeMS start = GetTimeMS();
	e.SetEvent();
	e.WaitOnEvent(30000);
	const TimeMS elapsed = GetTimeMS() - start;

	ASSERT_LT(elapsed, kGenerousMS) << "SetEvent or WaitOnEvent used a destroyed condition variable";
}

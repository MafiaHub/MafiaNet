/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Start/Stop lifecycle tests for TCPInterface.
 *
 *  These exist because of a specific failure: TCPInterface uses 0 as its
 *  "no socket" value, and Stop() closed remoteClients[i].socket for every slot,
 *  including the ones that were never used and still held that 0. On POSIX, 0 is
 *  a perfectly good descriptor -- stdin in a normal process -- so Stop() closed
 *  it, once per unused slot. The visible consequence was worse than the obvious
 *  one: with descriptor 0 free, the next socket created anywhere in the process
 *  was handed number 0, and every `listenSocket != 0` check then read it as
 *  "no socket". A second TCPInterface in the same process would start, report
 *  success, and never accept anything.
 *
 *  They run here rather than in the unit suite because proving a listener works
 *  takes a real connection.
 */

#include <gtest/gtest.h>

#include "mafianet/tcp_interface.h"

#if _RAKNET_SUPPORT_TCPInterface == 1

#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "TestHelpers.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

using namespace MafiaNet;

namespace
{
	class TCPInterfaceLifecycle : public ::testing::Test
	{
	protected:
		// Brings up a listener and a client, connects them, and tears both down.
		// Returns true if both sides observed the connection.
		static bool RunOneCycle(unsigned int timeoutMS = 10000)
		{
			TCPInterface server;
			TCPInterface client;

			// Four slots, one of which gets used: the other three are the unused
			// slots whose socket value is 0.
			const unsigned short port = TestHelpers::StartTcpOnAFreePort(server, 4);
			if (port == 0)
				return false;
			if (client.Start(0, 0) == false)
				return false;

			client.Connect("127.0.0.1", port, false);

			SystemAddress completed = UNASSIGNED_SYSTEM_ADDRESS;
			SystemAddress incoming = UNASSIGNED_SYSTEM_ADDRESS;
			const TimeMS start = GetTimeMS();
			while ((completed == UNASSIGNED_SYSTEM_ADDRESS || incoming == UNASSIGNED_SYSTEM_ADDRESS)
				&& GetTimeMS() - start < timeoutMS)
			{
				if (completed == UNASSIGNED_SYSTEM_ADDRESS)
					completed = client.HasCompletedConnectionAttempt();
				if (incoming == UNASSIGNED_SYSTEM_ADDRESS)
					incoming = server.HasNewIncomingConnection();
				if (completed == UNASSIGNED_SYSTEM_ADDRESS || incoming == UNASSIGNED_SYSTEM_ADDRESS)
					RakSleep(10);
			}

			const bool ok = (completed != UNASSIGNED_SYSTEM_ADDRESS)
				&& (incoming != UNASSIGNED_SYSTEM_ADDRESS);

			client.Stop();
			server.Stop();
			return ok;
		}
	};
}

// The behavioural form, portable: three Start/Stop cycles in one process. The
// first one always worked; the second was the one that silently stopped
// accepting once descriptor 0 had been freed and reassigned.
TEST_F(TCPInterfaceLifecycle, RepeatedStartStopCyclesKeepAccepting)
{
	ASSERT_TRUE(RunOneCycle()) << "cycle 1 failed";
	ASSERT_TRUE(RunOneCycle()) << "cycle 2 failed -- a previous Stop() left the process in a bad state";
	ASSERT_TRUE(RunOneCycle()) << "cycle 3 failed";
}

#ifndef _WIN32
// The direct form: descriptor 0 must still be open afterwards. POSIX only --
// on Windows descriptor 0 is not a socket handle and SOCKET 0 does not collide
// with stdin the same way.
TEST_F(TCPInterfaceLifecycle, StopDoesNotCloseDescriptorZero)
{
	ASSERT_NE(fcntl(0, F_GETFD), -1) << "descriptor 0 was already closed before the test ran";

	{
		TCPInterface tcp;
		// Eight slots, none of them ever used, so eight slots hold the 0 value.
		ASSERT_NE(TestHelpers::StartTcpOnAFreePort(tcp, 8), 0);
		tcp.Stop();
	}

	ASSERT_NE(fcntl(0, F_GETFD), -1)
		<< "Stop() closed descriptor 0 -- it closed the unused remoteClients slots' 0 value";
}

// Stopping twice must not close anything a second time either.
TEST_F(TCPInterfaceLifecycle, StoppingTwiceDoesNotCloseDescriptorZero)
{
	ASSERT_NE(fcntl(0, F_GETFD), -1);

	{
		TCPInterface tcp;
		ASSERT_NE(TestHelpers::StartTcpOnAFreePort(tcp, 4), 0);
		tcp.Stop();
		tcp.Stop();
	}

	ASSERT_NE(fcntl(0, F_GETFD), -1);
}
#endif

// Start() discarded CreateListenSocket's result, so a bind failure produced a
// started interface with no listener and a return value of true.
TEST_F(TCPInterfaceLifecycle, StartFailsWhenThePortIsAlreadyInUse)
{
	TCPInterface first;
	const unsigned short port = TestHelpers::StartTcpOnAFreePort(first, 2);
	ASSERT_NE(port, 0) << "the first listener should have bound a port";

	TCPInterface second;
	ASSERT_FALSE(second.Start(port, 2)) << "Start() must report a failed bind";

	// And the first one still works afterwards.
	TCPInterface client;
	ASSERT_TRUE(client.Start(0, 0));
	client.Connect("127.0.0.1", port, false);

	SystemAddress incoming = UNASSIGNED_SYSTEM_ADDRESS;
	const TimeMS start = GetTimeMS();
	while (incoming == UNASSIGNED_SYSTEM_ADDRESS && GetTimeMS() - start < 10000)
	{
		incoming = first.HasNewIncomingConnection();
		if (incoming == UNASSIGNED_SYSTEM_ADDRESS)
			RakSleep(10);
	}

	client.Stop();
	first.Stop();
	second.Stop();

	ASSERT_NE(incoming, UNASSIGNED_SYSTEM_ADDRESS) << "the surviving listener stopped accepting";
}

// A failed Start must leave the interface stoppable and restartable.
TEST_F(TCPInterfaceLifecycle, AnInterfaceWhoseStartFailedCanStartAgain)
{
	TCPInterface blocker;
	const unsigned short taken = TestHelpers::StartTcpOnAFreePort(blocker, 1);
	ASSERT_NE(taken, 0);

	TCPInterface tcp;
	ASSERT_FALSE(tcp.Start(taken, 1));

	tcp.Stop(); // must be harmless after a failed Start

	const bool restarted = TestHelpers::StartTcpOnAFreePort(tcp, 1) != 0;

	tcp.Stop();
	blocker.Stop();

	ASSERT_TRUE(restarted) << "a failed Start left the interface unusable";
}

#endif // _RAKNET_SUPPORT_TCPInterface

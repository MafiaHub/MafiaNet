/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "GuidUtilTest.h"

#include "mafianet/guid_util.h"
#include "mafianet/types.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace MafiaNet;

int GuidUtilTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

	destroyList.Clear(false, _FILE_AND_LINE_);

	const uint64_t samples[] = { 0u, 1u, 42u, 0x123456789ABCDEF0ull, (uint64_t)-2 };
	const size_t sampleCount = sizeof(samples) / sizeof(samples[0]);

	// --- to_string matches the legacy thread-safe member, value-for-value ---
	for (size_t i = 0; i < sampleCount; ++i)
	{
		const RakNetGUID g(samples[i]);
		char buffer[64];
		g.ToString(buffer, sizeof(buffer));
		if (to_string(g) != std::string(buffer))
		{
			if (isVerbose) printf("to_string(0x%llx)=\"%s\" != member ToString \"%s\"\n",
				(unsigned long long) samples[i], to_string(g).c_str(), buffer);
			return 1;
		}
	}

	// --- the UNASSIGNED sentinel stringifies to the documented label ---
	if (to_string(UNASSIGNED_RAKNET_GUID) != std::string("UNASSIGNED_RAKNET_GUID"))
	{
		if (isVerbose) printf("to_string(UNASSIGNED_RAKNET_GUID)=\"%s\"\n",
			to_string(UNASSIGNED_RAKNET_GUID).c_str());
		return 2;
	}

	// --- thread-safety: many threads stringify distinct GUIDs concurrently ---
	// The legacy member used a process-wide rotating static buffer, so two
	// concurrent callers could read each other's half-written output. to_string
	// owns a local buffer, so every result must equal its own expected text no
	// matter how many threads run. A shared buffer would corrupt these.
	{
		const unsigned threadCount = 16;
		const unsigned iterations = 20000;
		std::atomic<bool> corrupted(false);

		std::vector<std::thread> threads;
		threads.reserve(threadCount);
		for (unsigned t = 0; t < threadCount; ++t)
		{
			// Each thread owns a distinct value, so its expected string is fixed.
			const uint64_t value = 0x1000000000000000ull + t;
			const RakNetGUID g(value);
			const std::string expected = to_string(g);
			threads.emplace_back([g, expected, iterations, &corrupted]() {
				for (unsigned k = 0; k < iterations && !corrupted.load(std::memory_order_relaxed); ++k)
				{
					if (to_string(g) != expected)
						corrupted.store(true, std::memory_order_relaxed);
				}
			});
		}
		for (std::thread &th : threads)
			th.join();

		if (corrupted.load())
		{
			if (isVerbose) printf("to_string produced a corrupted result under concurrency\n");
			return 3;
		}
	}

	// --- connected_address maps the "none" sentinel to std::nullopt ---
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	destroyList.Push(peer, _FILE_AND_LINE_);
	SocketDescriptor sd(0, 0);
	if (peer->Startup(8, &sd, 1) != RAKNET_STARTED)
		return 4;

	// GetSystemAddressFromGuid(UNASSIGNED_RAKNET_GUID) returns the sentinel.
	if (connected_address(*peer, UNASSIGNED_RAKNET_GUID).has_value())
	{
		if (isVerbose) printf("connected_address(UNASSIGNED_RAKNET_GUID) should be nullopt\n");
		return 5;
	}

	// An arbitrary guid we are not connected to also has no address.
	if (connected_address(*peer, RakNetGUID(0xDEADBEEFull)).has_value())
	{
		if (isVerbose) printf("connected_address(unknown guid) should be nullopt\n");
		return 6;
	}

	// --- the wrapper faithfully mirrors the legacy lookup it wraps ---
	// has_value() iff the legacy call did not return the sentinel, and when
	// engaged the address is exactly what the legacy call returned.
	for (size_t i = 0; i < sampleCount; ++i)
	{
		const RakNetGUID g(samples[i]);
		const SystemAddress legacy = peer->GetSystemAddressFromGuid(g);
		const std::optional<SystemAddress> modern = connected_address(*peer, g);
		const bool legacyHas = (legacy != UNASSIGNED_SYSTEM_ADDRESS);
		if (modern.has_value() != legacyHas || (legacyHas && modern.value() != legacy))
		{
			if (isVerbose) printf("connected_address disagrees with GetSystemAddressFromGuid for 0x%llx\n",
				(unsigned long long) samples[i]);
			return 7;
		}
	}

	printf("guid_util: to_string is thread-safe and connected_address maps the sentinel to nullopt\n");
	return 0;
}

GuidUtilTest::GuidUtilTest(void)
{
}

GuidUtilTest::~GuidUtilTest(void)
{
}

RakString GuidUtilTest::GetTestName(void)
{
	return "GuidUtilTest";
}

RakString GuidUtilTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "to_string did not match the legacy ToString(char*, size_t) member";
	case 2:  return "to_string(UNASSIGNED_RAKNET_GUID) was not \"UNASSIGNED_RAKNET_GUID\"";
	case 3:  return "to_string produced a corrupted result under concurrent calls";
	case 4:  return "Peer failed to start";
	case 5:  return "connected_address(UNASSIGNED_RAKNET_GUID) was not std::nullopt";
	case 6:  return "connected_address(unknown guid) was not std::nullopt";
	case 7:  return "connected_address disagreed with GetSystemAddressFromGuid";
	default: return "Undefined Error";
	}
}

void GuidUtilTest::DestroyPeers(void)
{
	int theSize = destroyList.Size();
	for (int i = 0; i < theSize; i++)
		destroyList[i]->Shutdown(100);
	for (int i = 0; i < theSize; i++)
		RakPeerInterface::DestroyInstance(destroyList[i]);
}

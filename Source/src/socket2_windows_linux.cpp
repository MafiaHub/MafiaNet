/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2019, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/empty_header.h"
#include <cstdlib>

#ifdef RAKNET_SOCKET_2_INLINE_FUNCTIONS

#ifndef RAKNETSOCKET2_WINDOWS_LINUX_CPP
#define RAKNETSOCKET2_WINDOWS_LINUX_CPP

#ifndef _WIN32
#include <sys/types.h> // used for getifaddrs()
#include <ifaddrs.h>   // used for getifaddrs()
#endif                 // _WIN32

#include <vector> // used for std::vector

#include "mafianet/socket_layer.h"     // SocketLayer::OrderInternalIds
#include "mafianet/sockaddr_convert.h" // HopLimitOptionsFor, HopLimitToRestore

// Teredo (2001:0::/32) addresses are tunnel endpoints, not addresses a LAN peer
// can reach; they are never advertised as internal IDs.
static bool IsTeredoIPv6(const sockaddr_in6 &address)
{
	const unsigned char *b = reinterpret_cast<const unsigned char *>(&address.sin6_addr);
	return b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00;
}

#ifdef _WIN32

// based on https://stackoverflow.com/questions/122208/get-the-ip-address-of-local-computer#1317284
// Appends every IPv4 and IPv6 unicast address of the non-loopback adapters to
// \a found. Link-local and mapped addresses are filtered by OrderInternalIds.
static void GetMyIP_Windows(std::vector<SystemAddress> &found)
{
	ULONG outBufLen = 45 * 1024; // reserve 45 KB of memory which is the upper limit taken from the sample in MSDN (ie. 15 KB * 3 iterations)
	PIP_ADAPTER_ADDRESSES pAddresses = static_cast<IP_ADAPTER_ADDRESSES *>(std::malloc(outBufLen));
	if (pAddresses == nullptr)
	{
		// #med - error logging and/or throw exception?
		return;
	}

	DWORD error = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME, nullptr, pAddresses, &outBufLen);
	if (error != ERROR_SUCCESS)
	{
		// #med - error logging and/or throw exception?
		std::free(pAddresses);
		return;
	}

	for (PIP_ADAPTER_ADDRESSES pCurAdapter = pAddresses; pCurAdapter != nullptr; pCurAdapter = pCurAdapter->Next)
	{
		// skip loopback adapters
		if (pCurAdapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;

		// parse the adapter's unicast addresses
		for (PIP_ADAPTER_UNICAST_ADDRESS curAddress = pCurAdapter->FirstUnicastAddress; curAddress != nullptr; curAddress = curAddress->Next)
		{
			const ADDRESS_FAMILY curAddressFamily = curAddress->Address.lpSockaddr->sa_family;
			SystemAddress systemAddress;
			if (curAddressFamily == AF_INET)
			{
				const sockaddr_in *const curSocketAddress = reinterpret_cast<const sockaddr_in *>(curAddress->Address.lpSockaddr);

				// #med - review, is this really necessary?
				// skip source only addresses (aka: 0.0.0.0/8 - see RFC1700 p.4)
				if (curSocketAddress->sin_addr.S_un.S_un_b.s_b1 == 0)
					continue;
				systemAddress.address.addr4 = *curSocketAddress;
			}
			else if (curAddressFamily == AF_INET6)
			{
				const sockaddr_in6 *const curSocketAddress = reinterpret_cast<const sockaddr_in6 *>(curAddress->Address.lpSockaddr);
				if (IsTeredoIPv6(*curSocketAddress))
					continue;
				systemAddress.address.addr6 = *curSocketAddress;
			}
			else
			{
				continue; // neither IPv4 nor IPv6
			}
			found.push_back(systemAddress);
		}
	}
	std::free(pAddresses);
}

#else // _WIN32

// based on https://stackoverflow.com/questions/212528/get-the-ip-address-of-the-machine#265978
// Appends every IPv4 and IPv6 address of the non-loopback interfaces to
// \a found. Link-local and mapped addresses are filtered by OrderInternalIds.
static void GetMyIP_Linux(std::vector<SystemAddress> &found)
{
	struct ifaddrs *pAddresses = nullptr;
	if (getifaddrs(&pAddresses) != 0)
		return;

	for (struct ifaddrs *pCurAdapter = pAddresses; pCurAdapter != nullptr; pCurAdapter = pCurAdapter->ifa_next)
	{
		// skip interfaces which don't have any address assigned (according to the manual, this would only apply for BSD, but still we'd check for null here just in case)
		// and loopback adapters
		if (pCurAdapter->ifa_addr == nullptr || (pCurAdapter->ifa_flags & IFF_LOOPBACK) != 0)
			continue;

		SystemAddress systemAddress;
		if (pCurAdapter->ifa_addr->sa_family == AF_INET)
		{
			const sockaddr_in *const curSocketAddress = reinterpret_cast<const sockaddr_in *>(pCurAdapter->ifa_addr);

			// #med - review, is this really necessary?
			// skip source only addresses (aka: 0.0.0.0/8 - see RFC1700 p.4)
			if (reinterpret_cast<const unsigned char *>(&curSocketAddress->sin_addr)[0] == 0)
				continue;
			systemAddress.address.addr4 = *curSocketAddress;
		}
		else if (pCurAdapter->ifa_addr->sa_family == AF_INET6)
		{
			const sockaddr_in6 *const curSocketAddress = reinterpret_cast<const sockaddr_in6 *>(pCurAdapter->ifa_addr);
			if (IsTeredoIPv6(*curSocketAddress))
				continue;
			systemAddress.address.addr6 = *curSocketAddress;
		}
		else
		{
			continue; // neither IPv4 nor IPv6 (e.g. AF_PACKET / AF_LINK)
		}
		found.push_back(systemAddress);
	}

	freeifaddrs(pAddresses);
}

#endif // _WIN32

void GetMyIP_Windows_Linux(SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS])
{
	// Gather everything first and only then cut down to
	// MAXIMUM_NUMBER_OF_INTERNAL_IDS: truncating in interface order would let
	// IPv6 addresses push out the LAN IPv4 ones NAT punchthrough relies on.
	std::vector<SystemAddress> found;
#ifdef _WIN32
	GetMyIP_Windows(found);
#else
	GetMyIP_Linux(found);
#endif
	const int count = SocketLayer::OrderInternalIds(found.data(), (int) found.size());

	int index = 0;
	for (; index < count; ++index)
		addresses[index] = found[(size_t) index];
	for (; index < MAXIMUM_NUMBER_OF_INTERNAL_IDS; ++index)
		addresses[index] = UNASSIGNED_SYSTEM_ADDRESS;
}

// The TTL a fresh IPv4 socket sends with: the system default. Only needed to
// undo a temporary hop limit on a dual-stack socket that had none set; see
// HopLimitToRestore. 0 when it cannot be read.
static int SystemDefaultIPv4Ttl()
{
	const RNS2Socket probe = socket__(AF_INET, SOCK_DGRAM, 0);
	if (probe == (RNS2Socket) INVALID_SOCKET)
		return 0;
	int ttl = 0;
	socklen_t len = sizeof(ttl);
	if (getsockopt__(probe, IPPROTO_IP, IP_TTL, (char *) &ttl, &len) != 0)
		ttl = 0;
	closesocket__(probe);
	return ttl;
}

RNS2SendResult RNS2_Windows_Linux::Send_Windows_Linux(RNS2Socket rns2Socket, int socketFamily, bool dualStack, RNS2_SendParameters *sendParameters, const char *file, unsigned int line)
{
	// IPv4 targets become ::ffff:a.b.c.d on a dual-stack socket; a target the
	// socket cannot reach at all is refused here rather than by sendto.
	sockaddr_storage to;
	const socklen_t toLen = SystemAddressToSockaddr(sendParameters->systemAddress, dualStack, &to, socketFamily);
	if (toLen == 0)
		return -1;

	int len = 0;
	do
	{
		(void) file;
		(void) line;


		// A temporary TTL (NAT punchthrough): set it with the first option the
		// socket accepts for this target (see HopLimitOptionsFor), and put the
		// old value back after the send.
		HopLimitOption ttlOption = {0, 0};
		int oldTTL = 0;
		bool restoreTTL = false;
		if (sendParameters->ttl > 0)
		{
			HopLimitOption candidates[MAX_HOP_LIMIT_OPTIONS];
			const int candidateCount = HopLimitOptionsFor(socketFamily, dualStack, sendParameters->systemAddress.address.addr4.sin_family, candidates);
			for (int i = 0; i < candidateCount && !restoreTTL; ++i)
			{
				int saved = 0;
				socklen_t opLen = sizeof(saved);
				if (getsockopt__(rns2Socket, candidates[i].level, candidates[i].name, (char *) &saved, &opLen) != 0)
					continue;
				int newTTL = sendParameters->ttl;
				if (setsockopt__(rns2Socket, candidates[i].level, candidates[i].name, (char *) &newTTL, sizeof(newTTL)) != 0)
					continue;
				ttlOption = candidates[i];
				oldTTL = saved;
				restoreTTL = true;
			}
		}


		len = sendto__(rns2Socket, sendParameters->data, sendParameters->length, 0, (const sockaddr *) &to, toLen);

		if (len < 0)
		{
			RAKNET_DEBUG_PRINTF("sendto failed with code %i for char %i and length %i.\n", len, sendParameters->data[0], sendParameters->length);
		}


		if (restoreTTL)
		{
			// On macOS a dual-stack socket's -1 hop limit is restored as the IPv4
			// default TTL (net.inet.ip.ttl) even for an IPv6 target, rather than
			// net.inet6.ip6.hlim: one option governs both families there, so the
			// IPv4 traffic is what must not change. Equal at the defaults (64).
			int restoreTo = HopLimitToRestore(oldTTL, dualStack, oldTTL < 0 && dualStack ? SystemDefaultIPv4Ttl() : 0);
			setsockopt__(rns2Socket, ttlOption.level, ttlOption.name, (char *) &restoreTo, sizeof(restoreTo));
		}

	} while (len == 0);
	return len;
}

#endif // RAKNETSOCKET2_WINDOWS_LINUX_CPP

#endif // RAKNET_SOCKET_2_INLINE_FUNCTIONS

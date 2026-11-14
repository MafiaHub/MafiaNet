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

#ifdef RAKNET_SOCKET_2_INLINE_FUNCTIONS

#ifndef RAKNETSOCKET2_WINDOWS_LINUX_CPP
#define RAKNETSOCKET2_WINDOWS_LINUX_CPP

#ifndef _WIN32
#include <sys/types.h> // used for getifaddrs()
#include <ifaddrs.h>   // used for getifaddrs()
#endif                 // _WIN32

#include <vector> // used for std::vector

#include "mafianet/socket_layer.h" // SocketLayer::OrderInternalIds

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
	PIP_ADAPTER_ADDRESSES pAddresses = static_cast<IP_ADAPTER_ADDRESSES *>(rakMalloc_Ex(outBufLen, _FILE_AND_LINE_));
	if (pAddresses == nullptr)
	{
		// #med - error logging and/or throw exception?
		return;
	}

	DWORD error = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME, nullptr, pAddresses, &outBufLen);
	if (error != ERROR_SUCCESS)
	{
		// #med - error logging and/or throw exception?
		rakFree_Ex(pAddresses, _FILE_AND_LINE_);
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
	rakFree_Ex(pAddresses, _FILE_AND_LINE_);
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

RNS2SendResult RNS2_Windows_Linux::Send_Windows_Linux(RNS2Socket rns2Socket, RNS2_SendParameters *sendParameters, const char *file, unsigned int line)
{

	int len = 0;
	do
	{
		(void) file;
		(void) line;


		int oldTTL = -1;
		if (sendParameters->ttl > 0)
		{
			socklen_t opLen = sizeof(oldTTL);
			// Get the current TTL
			if (getsockopt__(rns2Socket, sendParameters->systemAddress.GetIPPROTO(), IP_TTL, (char *) &oldTTL, &opLen) != -1)
			{
				int newTTL = sendParameters->ttl;
				setsockopt__(rns2Socket, sendParameters->systemAddress.GetIPPROTO(), IP_TTL, (char *) &newTTL, sizeof(newTTL));
			}
		}


		if (sendParameters->systemAddress.address.addr4.sin_family == AF_INET)
		{
			len = sendto__(rns2Socket, sendParameters->data, sendParameters->length, 0, (const sockaddr *) &sendParameters->systemAddress.address.addr4, sizeof(sockaddr_in));
		}
		else
		{
			len = sendto__(rns2Socket, sendParameters->data, sendParameters->length, 0, (const sockaddr *) &sendParameters->systemAddress.address.addr6, sizeof(sockaddr_in6));
		}

		if (len < 0)
		{
			RAKNET_DEBUG_PRINTF("sendto failed with code %i for char %i and length %i.\n", len, sendParameters->data[0], sendParameters->length);
		}


		if (oldTTL != -1)
		{
			setsockopt__(rns2Socket, sendParameters->systemAddress.GetIPPROTO(), IP_TTL, (char *) &oldTTL, sizeof(oldTTL));
		}

	} while (len == 0);
	return len;
}

#endif // RAKNETSOCKET2_WINDOWS_LINUX_CPP

#endif // RAKNET_SOCKET_2_INLINE_FUNCTIONS

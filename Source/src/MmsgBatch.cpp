/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/MmsgBatch.h"

namespace MafiaNet
{

bool SockaddrToSystemAddress(const sockaddr_storage &from, SystemAddress *out)
{
	// Mirrors the byte-order handling of RNS2_Berkley::RecvFromBlockingIPV4And6:
	// the sockaddr keeps the port in network order (copied verbatim into the
	// address union), and debugPort caches the host-order value.
	if (from.ss_family == AF_INET)
	{
		memcpy(&out->address.addr4,
		       reinterpret_cast<const sockaddr_in *>(&from), sizeof(sockaddr_in));
		out->debugPort = ntohs(out->address.addr4.sin_port);
		return true;
	}
#if RAKNET_SUPPORT_IPV6 == 1
	if (from.ss_family == AF_INET6)
	{
		memcpy(&out->address.addr6,
		       reinterpret_cast<const sockaddr_in6 *>(&from), sizeof(sockaddr_in6));
		out->debugPort = ntohs(out->address.addr6.sin6_port);
		return true;
	}
#endif

	// A family this build cannot represent (an IPv6 source on an IPv4-only
	// build, or AF_UNSPEC from a failed read). Overwrite rather than leave the
	// recycled struct's previous sender in place -- that would attribute the
	// datagram to the wrong peer.
	*out = UNASSIGNED_SYSTEM_ADDRESS;
	return false;
}

void DispatchRecvBatch(RNS2EventHandler *handler,
                       RNS2RecvStruct **slots, unsigned allocated,
                       const int *lens, const sockaddr_storage *addrs,
                       unsigned received, RakNetSocket2 *socket,
                       MafiaNet::TimeUS now)
{
	if (received > allocated)
		received = allocated;

	for (unsigned i = 0; i < received; ++i)
	{
		RNS2RecvStruct *s = slots[i];
		if (lens[i] > 0 && SockaddrToSystemAddress(addrs[i], &s->systemAddress))
		{
			s->bytesRead = lens[i];
			s->timeRead = now;
			s->socket = socket;
			handler->OnRNS2Recv(s);
		}
		else
		{
			// Zero-length read (same as the scalar bytesRead<=0 branch) or an
			// undecodable source address -- free the struct rather than
			// surfacing a packet we cannot attribute to a peer.
			handler->DeallocRNS2RecvStruct(s, _FILE_AND_LINE_);
		}
	}

	// Hand back the unused tail so no preallocated buffer leaks.
	for (unsigned i = received; i < allocated; ++i)
		handler->DeallocRNS2RecvStruct(slots[i], _FILE_AND_LINE_);
}

} // namespace MafiaNet

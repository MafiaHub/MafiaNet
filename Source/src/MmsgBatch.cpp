/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/MmsgBatch.h"

namespace MafiaNet
{

void SockaddrToSystemAddress(const sockaddr_storage &from, SystemAddress *out)
{
	// Mirrors the byte-order handling of RNS2_Berkley::RecvFromBlockingIPV4And6:
	// the sockaddr keeps the port in network order (copied verbatim into the
	// address union), and debugPort caches the host-order value.
	if (from.ss_family == AF_INET)
	{
		memcpy(&out->address.addr4,
		       reinterpret_cast<const sockaddr_in *>(&from), sizeof(sockaddr_in));
		out->debugPort = ntohs(out->address.addr4.sin_port);
	}
#if RAKNET_SUPPORT_IPV6 == 1
	else
	{
		memcpy(&out->address.addr6,
		       reinterpret_cast<const sockaddr_in6 *>(&from), sizeof(sockaddr_in6));
		out->debugPort = ntohs(out->address.addr6.sin6_port);
	}
#endif
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
		if (lens[i] > 0)
		{
			s->bytesRead = lens[i];
			s->timeRead = now;
			s->socket = socket;
			SockaddrToSystemAddress(addrs[i], &s->systemAddress);
			handler->OnRNS2Recv(s);
		}
		else
		{
			// Zero-length read: same as the scalar bytesRead<=0 branch -- free
			// the struct rather than surfacing an empty packet.
			handler->DeallocRNS2RecvStruct(s, __FILE__, __LINE__);
		}
	}

	// Hand back the unused tail so no preallocated buffer leaks.
	for (unsigned i = received; i < allocated; ++i)
		handler->DeallocRNS2RecvStruct(slots[i], __FILE__, __LINE__);
}

} // namespace MafiaNet

/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/sockaddr_convert.h"

#include <string.h> // memcpy, memset

namespace MafiaNet
{

bool SockaddrToSystemAddress(const sockaddr_storage &from, SystemAddress *out)
{
	// The sockaddr keeps the port in network order (copied verbatim into the
	// address union), and debugPort caches the host-order value.
	if (from.ss_family == AF_INET)
	{
		memcpy(&out->address.addr4,
		       reinterpret_cast<const sockaddr_in *>(&from), sizeof(sockaddr_in));
		out->debugPort = ntohs(out->address.addr4.sin_port);
		return true;
	}
	if (from.ss_family == AF_INET6)
	{
		memcpy(&out->address.addr6,
		       reinterpret_cast<const sockaddr_in6 *>(&from), sizeof(sockaddr_in6));
		out->debugPort = ntohs(out->address.addr6.sin6_port);
		// An IPv4 peer on a dual-stack socket: hand the library the canonical
		// AF_INET form, so it compares equal to the same peer reached over an
		// IPv4 socket, and IsLoopback/ban lists/GUID lookups keep working.
		out->UnmapIPv4();
		return true;
	}

	// A family SystemAddress cannot represent (AF_UNSPEC from a failed read,
	// or anything else that is neither IPv4 nor IPv6). Overwrite rather than leave the
	// recycled struct's previous sender in place -- that would attribute the
	// datagram to the wrong peer.
	*out = UNASSIGNED_SYSTEM_ADDRESS;
	return false;
}

socklen_t SystemAddressToSockaddr(const SystemAddress &to, bool socketIsDualStack, sockaddr_storage *out, int socketFamily)
{
	memset(out, 0, sizeof(*out));
	const int family = to.address.addr4.sin_family;
	if (family == AF_INET)
	{
		if (socketIsDualStack)
		{
			// ::ffff:a.b.c.d, the form a dual-stack socket sends IPv4 with.
			sockaddr_in6 *in6 = reinterpret_cast<sockaddr_in6 *>(out);
			in6->sin6_family = AF_INET6;
			in6->sin6_port = to.address.addr4.sin_port;
			unsigned char *b = reinterpret_cast<unsigned char *>(&in6->sin6_addr);
			b[10] = 0xff;
			b[11] = 0xff;
			memcpy(b + 12, &to.address.addr4.sin_addr, 4);
			return (socklen_t) sizeof(sockaddr_in6);
		}
		if (socketFamily == AF_INET6)
			return 0; // IPv6-only socket
		memcpy(out, &to.address.addr4, sizeof(sockaddr_in));
		return (socklen_t) sizeof(sockaddr_in);
	}
	if (family == AF_INET6)
	{
		if (!socketIsDualStack && socketFamily == AF_INET)
			return 0; // an IPv4 socket cannot reach an IPv6 address
		memcpy(out, &to.address.addr6, sizeof(sockaddr_in6));
		return (socklen_t) sizeof(sockaddr_in6);
	}
	return 0;
}

int HopLimitOptionsFor(int socketFamily, bool socketIsDualStack, int targetFamily, HopLimitOption out[MAX_HOP_LIMIT_OPTIONS])
{
	const HopLimitOption ipTtl = {IPPROTO_IP, IP_TTL};
	const HopLimitOption unicastHops = {IPPROTO_IPV6, IPV6_UNICAST_HOPS};
	if (socketIsDualStack)
		socketFamily = AF_INET6;

	if (targetFamily == AF_INET)
	{
		if (socketFamily == AF_INET || socketFamily == AF_UNSPEC)
		{
			out[0] = ipTtl;
			return 1;
		}
		if (!socketIsDualStack)
			return 0; // IPv6-only socket
		out[0] = ipTtl;
		out[1] = unicastHops;
		return 2;
	}
	if (targetFamily == AF_INET6)
	{
		if (socketFamily == AF_INET)
			return 0;
		out[0] = unicastHops;
		return 1;
	}
	return 0;
}

int HopLimitToRestore(int saved, bool socketIsDualStack, int ipv4DefaultTtl)
{
	if (saved >= 0 || !socketIsDualStack)
		return saved;
	return ipv4DefaultTtl > 0 ? ipv4DefaultTtl : 64;
}

} // namespace MafiaNet

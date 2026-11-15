/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Helpers for TCPInterface tests over real loopback connections.
 */

#pragma once

#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "mafianet/socket_includes.h"
#include "mafianet/tcp_interface.h"

#include <cstring>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace TcpTestHelpers
{
// TCPInterface does not report the port it listens on; this reads it from the
// listening socket, so a test can listen on a port the OS assigns (Start(0, ...)).
class ListeningTcp : public MafiaNet::TCPInterface
{
public:
	unsigned short ListenPort() const
	{
		sockaddr_storage local{};
		if (!LocalAddress(&local))
			return 0;
		return local.ss_family == AF_INET ? ntohs(((const sockaddr_in *) &local)->sin_port) : ntohs(((const sockaddr_in6 *) &local)->sin6_port);
	}
	int ListenFamily() const
	{
		sockaddr_storage local{};
		return LocalAddress(&local) ? local.ss_family : AF_UNSPEC;
	}

private:
	bool LocalAddress(sockaddr_storage *local) const
	{
		memset(local, 0, sizeof(*local));
		socklen_t length = sizeof(*local);
		return listenSocket != 0 && getsockname(listenSocket, (sockaddr *) local, &length) == 0;
	}
};

// Whether a TCP socket can bind ::1 on this host (Winsock must be started on Windows).
inline bool HasIpv6Loopback()
{
	bool ok = false;
	const auto s = socket(AF_INET6, SOCK_STREAM, 0);
#ifdef _WIN32
	if (s != INVALID_SOCKET)
#else
	if (s >= 0)
#endif
	{
		sockaddr_in6 addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin6_family = AF_INET6;
		addr.sin6_addr.s6_addr[15] = 1; // ::1
		ok = bind(s, (sockaddr *) &addr, sizeof(addr)) == 0;
#ifdef _WIN32
		closesocket(s);
#else
		close(s);
#endif
	}
	return ok;
}

// TCP is a byte stream: one Send may arrive as several packets. Appends what \a tcp receives from \a from (any
// address if UNASSIGNED_SYSTEM_ADDRESS) to \a received, deallocating every packet and dropping those from other
// addresses, until it holds \a count bytes or \a timeoutMs pass. \a sender, if given, receives the address of the
// last packet kept. Returns whether \a count bytes arrived.
inline bool ReceiveBytes(MafiaNet::TCPInterface *tcp, const MafiaNet::SystemAddress &from, size_t count, std::string *received, MafiaNet::SystemAddress *sender = nullptr, MafiaNet::TimeMS timeoutMs = 5000)
{
	const MafiaNet::TimeMS start = MafiaNet::GetTimeMS();
	while (received->size() < count && MafiaNet::GetTimeMS() - start < timeoutMs)
	{
		MafiaNet::Packet *p = tcp->Receive();
		if (p == nullptr)
		{
			RakSleep(30);
			continue;
		}
		if (from == MafiaNet::UNASSIGNED_SYSTEM_ADDRESS || p->systemAddress == from)
		{
			received->append((const char *) p->data, p->length);
			if (sender != nullptr)
				*sender = p->systemAddress;
		}
		tcp->DeallocatePacket(p);
	}
	return received->size() >= count;
}
} // namespace TcpTestHelpers

/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Dual-stack is the default socket (SocketDescriptor::socketFamily ==
 *  AF_UNSPEC). What only real sockets can show:
 *  - a default wildcard socket reports 127.0.0.1|port as its bound address and
 *    an IPv4 client reaches it there;
 *  - requireDualStack on the wildcard gives a dual-stack socket;
 *  - RakPeer::SendTTL (NAT punchthrough) really sets the TTL / hop limit of the
 *    datagram on a dual-stack socket, for IPv4 and IPv6 targets, and leaves the
 *    socket's later datagrams at the system default. Measured with a receiving
 *    socket that reports each datagram's TTL (IP_RECVTTL / IPV6_RECVHOPLIMIT),
 *    which Windows does not offer through recvmsg, so those tests skip there.
 */

#if defined(__APPLE__)
#define __APPLE_USE_RFC_3542 1 // IPV6_RECVHOPLIMIT / IPV6_HOPLIMIT
#endif

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/peer_interface.h"
#include "mafianet/sleep.h"
#include "mafianet/socket2.h"

#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

using namespace MafiaNet;

namespace
{
bool IsDualStackPeer(RakPeerInterface *peer)
{
	DataStructures::List<RakNetSocket2 *> sockets;
	peer->GetSockets(sockets);
	return sockets.Size() == 1 && sockets[0]->IsDualStack();
}

#ifndef _WIN32
// A plain OS UDP socket on a loopback address that reports the TTL (IPv4) or
// hop limit (IPv6) each datagram arrived with.
class TtlReceiver
{
public:
	explicit TtlReceiver(int family)
	    : family_(family)
	{
		fd_ = socket(family, SOCK_DGRAM, 0);
		if (fd_ < 0)
			return;
		const int one = 1;
		sockaddr_storage ss;
		memset(&ss, 0, sizeof(ss));
		socklen_t len;
		if (family == AF_INET)
		{
			if (setsockopt(fd_, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one)) != 0)
			{
				Close();
				return;
			}
			sockaddr_in *in4 = reinterpret_cast<sockaddr_in *>(&ss);
			in4->sin_family = AF_INET;
			in4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			len = sizeof(sockaddr_in);
		}
		else
		{
			if (setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0 ||
			    setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one)) != 0)
			{
				Close();
				return;
			}
			sockaddr_in6 *in6 = reinterpret_cast<sockaddr_in6 *>(&ss);
			in6->sin6_family = AF_INET6;
			in6->sin6_addr = in6addr_loopback;
			len = sizeof(sockaddr_in6);
		}
		if (bind(fd_, reinterpret_cast<sockaddr *>(&ss), len) != 0 ||
		    getsockname(fd_, reinterpret_cast<sockaddr *>(&ss), &len) != 0)
		{
			Close();
			return;
		}
		port_ = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in *>(&ss)->sin_port
		                                : reinterpret_cast<sockaddr_in6 *>(&ss)->sin6_port);
	}
	~TtlReceiver() { Close(); }
	TtlReceiver(const TtlReceiver &) = delete;
	TtlReceiver &operator=(const TtlReceiver &) = delete;

	bool Ok() const { return fd_ >= 0 && port_ != 0; }
	unsigned short Port() const { return port_; }

	// Waits up to \a timeoutMs for the next datagram whose first byte is
	// \a firstByte (other datagrams are discarded) and returns the TTL / hop
	// limit it arrived with, or -1 on timeout.
	int NextTtl(unsigned char firstByte, TimeMS timeoutMs)
	{
		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < timeoutMs)
		{
			pollfd p = {fd_, POLLIN, 0};
			if (poll(&p, 1, 30) <= 0)
				continue;
			unsigned char data[2048];
			alignas(cmsghdr) unsigned char control[256];
			iovec iov = {data, sizeof(data)};
			msghdr msg;
			memset(&msg, 0, sizeof(msg));
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = control;
			msg.msg_controllen = sizeof(control);
			const ssize_t n = recvmsg(fd_, &msg, 0);
			if (n <= 0 || data[0] != firstByte)
				continue;
			for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
			{
				if (family_ == AF_INET && c->cmsg_level == IPPROTO_IP)
				{
					// Linux reports IP_TTL with an int, the BSDs IP_RECVTTL with a byte.
					if (c->cmsg_type == IP_TTL && c->cmsg_len >= CMSG_LEN(sizeof(int)))
					{
						int ttl;
						memcpy(&ttl, CMSG_DATA(c), sizeof(ttl));
						return ttl;
					}
#if defined(IP_RECVTTL) && !defined(__linux__)
					if (c->cmsg_type == IP_RECVTTL)
						return *CMSG_DATA(c);
#endif
				}
				if (family_ == AF_INET6 && c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT)
				{
					int hops;
					memcpy(&hops, CMSG_DATA(c), sizeof(hops));
					return hops;
				}
			}
			return -2; // datagram without the ancillary data
		}
		return -1;
	}

	// The TTL a datagram from a fresh socket of \a senderFamily gets: the
	// system default this test compares against.
	int BaselineTtl(int senderFamily)
	{
		const int s = socket(senderFamily, SOCK_DGRAM, 0);
		if (s < 0)
			return -1;
		sockaddr_storage to;
		memset(&to, 0, sizeof(to));
		socklen_t len;
		if (family_ == AF_INET)
		{
			sockaddr_in *in4 = reinterpret_cast<sockaddr_in *>(&to);
			in4->sin_family = AF_INET;
			in4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			in4->sin_port = htons(port_);
			len = sizeof(sockaddr_in);
		}
		else
		{
			sockaddr_in6 *in6 = reinterpret_cast<sockaddr_in6 *>(&to);
			in6->sin6_family = AF_INET6;
			in6->sin6_addr = in6addr_loopback;
			in6->sin6_port = htons(port_);
			len = sizeof(sockaddr_in6);
		}
		const unsigned char probe = 0xEE;
		const ssize_t sent = sendto(s, &probe, 1, 0, reinterpret_cast<sockaddr *>(&to), len);
		close(s);
		if (sent != 1)
			return -1;
		return NextTtl(probe, 5000);
	}

private:
	void Close()
	{
		if (fd_ >= 0)
			close(fd_);
		fd_ = -1;
	}

	int family_;
	int fd_ = -1;
	unsigned short port_ = 0;
};

// What SendTTL puts on the wire: two bytes, the first 0.
const unsigned char SEND_TTL_FIRST_BYTE = 0;
const int PROBE_TTL = 3;
#endif // _WIN32
} // namespace

class DualStackDefaultLive : public ::testing::Test
{
protected:
	void TearDown() override
	{
		for (RakPeerInterface *peer : {client, server})
		{
			if (peer == nullptr)
				continue;
			peer->Shutdown(100);
			RakPeerInterface::DestroyInstance(peer);
		}
		client = server = nullptr;
	}

	// Starts \a peer with \a sd; a socket that is not dual-stack skips the test,
	// or fails it under MAFIANET_REQUIRE_IPV6.
	void StartExpectingDualStack(RakPeerInterface *peer, SocketDescriptor sd, unsigned short maxIncoming)
	{
		const StartupResult result = peer->Startup(4, &sd, 1);
		if (result == SOCKET_FAMILY_NOT_SUPPORTED && sd.requireDualStack && std::getenv("MAFIANET_REQUIRE_IPV6") == nullptr)
			GTEST_SKIP() << "no dual-stack socket on this host (IPv6 unavailable)";
		ASSERT_EQ(result, RAKNET_STARTED);
		if (!IsDualStackPeer(peer))
		{
			if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
				FAIL() << "socket is not dual-stack but MAFIANET_REQUIRE_IPV6 is set";
			GTEST_SKIP() << "no dual-stack socket on this host (IPv6 unavailable)";
		}
		peer->SetMaximumIncomingConnections(maxIncoming);
	}

	RakPeerInterface *server = nullptr;
	RakPeerInterface *client = nullptr;
};

// The 0.26 contract "a wildcard socket reports 127.0.0.1|port" holds for the
// new default, and an IPv4-only client can connect to exactly that address.
TEST_F(DualStackDefaultLive, DefaultSocketServesIpv4ClientAtBoundAddress)
{
	server = RakPeerInterface::GetInstance();
	client = RakPeerInterface::GetInstance();

	StartExpectingDualStack(server, SocketDescriptor(0, nullptr), 1);
	if (IsSkipped() || HasFatalFailure())
		return;

	const SystemAddress bound = server->GetMyBoundAddress();
	ASSERT_EQ(bound.GetIPVersion(), 4) << bound.ToString(true);
	ASSERT_EQ(std::string(bound.ToString(false)), "127.0.0.1");
	ASSERT_NE(bound.GetPort(), 0);

	SocketDescriptor clientSd(0, "127.0.0.1");
	clientSd.socketFamily = AF_INET; // an IPv4-only client, whatever the default
	ASSERT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED);
	ASSERT_FALSE(IsDualStackPeer(client));

	char boundHost[SystemAddress::TO_STRING_BUFFER_SIZE];
	bound.ToString(false, boundHost, sizeof(boundHost));
	ASSERT_EQ(client->Connect(boundHost, bound.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);

	bool accepted = false;
	bool incoming = false;
	SystemAddress clientAsSeen;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 15000 && !(accepted && incoming))
	{
		for (Packet *p = client->Receive(); p != nullptr; p = client->Receive())
		{
			if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
				accepted = true;
			client->DeallocatePacket(p);
		}
		for (Packet *p = server->Receive(); p != nullptr; p = server->Receive())
		{
			if (p->data[0] == ID_NEW_INCOMING_CONNECTION)
			{
				incoming = true;
				clientAsSeen = p->systemAddress;
			}
			server->DeallocatePacket(p);
		}
		RakSleep(30);
	}
	ASSERT_TRUE(accepted) << "IPv4 client never connected to " << bound.ToString(true);
	ASSERT_TRUE(incoming) << "server never saw the IPv4 client";
	EXPECT_EQ(clientAsSeen.GetIPVersion(), 4) << clientAsSeen.ToString(true);
	EXPECT_EQ(clientAsSeen.GetPort(), client->GetMyBoundAddress().GetPort());
}

TEST_F(DualStackDefaultLive, RequireDualStackOnWildcardIsDualStack)
{
	server = RakPeerInterface::GetInstance();
	SocketDescriptor sd(0, nullptr);
	sd.requireDualStack = true;
	StartExpectingDualStack(server, sd, 0);
	if (IsSkipped() || HasFatalFailure())
		return;
	EXPECT_TRUE(IsDualStackPeer(server));
	EXPECT_EQ(std::string(server->GetMyBoundAddress().ToString(false)), "127.0.0.1");
}

#ifndef _WIN32
// SendTTL to an IPv4 target from a dual-stack socket: the datagram leaves with
// the requested TTL (IP_TTL on Linux; macOS rejects IP_TTL on an AF_INET6 socket
// and takes IPV6_UNICAST_HOPS for IPv4-mapped datagrams instead), and the
// socket's next datagram leaves with the system default again.
TEST_F(DualStackDefaultLive, SendTtlToIpv4TargetSetsAndRestoresTtl)
{
	server = RakPeerInterface::GetInstance();
	StartExpectingDualStack(server, SocketDescriptor(0, nullptr), 0);
	if (IsSkipped() || HasFatalFailure())
		return;

	TtlReceiver receiver(AF_INET);
	ASSERT_TRUE(receiver.Ok());
	const int defaultTtl = receiver.BaselineTtl(AF_INET);
	ASSERT_GT(defaultTtl, PROBE_TTL);

	server->SendTTL("127.0.0.1", receiver.Port(), PROBE_TTL);
	EXPECT_EQ(receiver.NextTtl(SEND_TTL_FIRST_BYTE, 5000), PROBE_TTL);

	// An ordinary datagram afterwards: the old TTL is back.
	ASSERT_TRUE(server->Ping("127.0.0.1", receiver.Port(), false));
	EXPECT_EQ(receiver.NextTtl(ID_UNCONNECTED_PING, 5000), defaultTtl);
}

// The same for an IPv6 target (IPV6_UNICAST_HOPS everywhere), and the probe
// must not disturb the TTL of the IPv4 datagrams the socket sends afterwards.
TEST_F(DualStackDefaultLive, SendTtlToIpv6TargetSetsAndRestoresHopLimit)
{
	server = RakPeerInterface::GetInstance();
	StartExpectingDualStack(server, SocketDescriptor(0, nullptr), 0);
	if (IsSkipped() || HasFatalFailure())
		return;

	TtlReceiver receiver6(AF_INET6);
	ASSERT_TRUE(receiver6.Ok());
	TtlReceiver receiver4(AF_INET);
	ASSERT_TRUE(receiver4.Ok());
	const int defaultHops = receiver6.BaselineTtl(AF_INET6);
	ASSERT_GT(defaultHops, PROBE_TTL);
	const int defaultTtl = receiver4.BaselineTtl(AF_INET);
	ASSERT_GT(defaultTtl, PROBE_TTL);

	server->SendTTL("::1", receiver6.Port(), PROBE_TTL);
	EXPECT_EQ(receiver6.NextTtl(SEND_TTL_FIRST_BYTE, 5000), PROBE_TTL);

	ASSERT_TRUE(server->Ping("::1", receiver6.Port(), false));
	EXPECT_EQ(receiver6.NextTtl(ID_UNCONNECTED_PING, 5000), defaultHops);
	ASSERT_TRUE(server->Ping("127.0.0.1", receiver4.Port(), false));
	EXPECT_EQ(receiver4.NextTtl(ID_UNCONNECTED_PING, 5000), defaultTtl);
}

// A one-shot send to a host name on a dual-stack socket goes to its IPv4
// address when it has one ("localhost" lists ::1 first on macOS): an IPv4-only
// receiver must get the probe.
TEST_F(DualStackDefaultLive, SendTtlToHostNamePrefersIpv4)
{
	server = RakPeerInterface::GetInstance();
	StartExpectingDualStack(server, SocketDescriptor(0, nullptr), 0);
	if (IsSkipped() || HasFatalFailure())
		return;

	TtlReceiver receiver(AF_INET);
	ASSERT_TRUE(receiver.Ok());

	server->SendTTL("localhost", receiver.Port(), PROBE_TTL);
	EXPECT_EQ(receiver.NextTtl(SEND_TTL_FIRST_BYTE, 5000), PROBE_TTL);
}
#endif // _WIN32

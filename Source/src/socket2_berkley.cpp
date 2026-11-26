/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/empty_header.h"

#ifdef RAKNET_SOCKET_2_INLINE_FUNCTIONS

#ifndef RAKNETSOCKET2_BERKLEY_CPP
#define RAKNETSOCKET2_BERKLEY_CPP

// Berkeley socket implementation for Windows and Linux

#ifdef _WIN32
#include <tchar.h> // used for _tprintf() (via RAKNET_DEBUG_TPRINTF)
#else
#include "mafianet/linux_strings.h" // used for _stricmp()
#include <sys/types.h>              // used for getaddrinfo()
#include <sys/socket.h>             // used for getaddrinfo()
#include <netdb.h>                  // used for getaddrinfo()
#endif

#include "mafianet/itoa.h"
#include "mafianet/wsa_startup_singleton.h"

// Domain name resolution. Writes the numeric form of the first address
// getaddrinfo returns for \a addressFamily (AF_INET, AF_INET6, or AF_UNSPEC for
// either, in the resolver's RFC 6724 order) to \a ip, or "" on failure.
void DomainNameToIP_Berkley_IPV4And6(const char *domainName, char ip[65], int addressFamily)
{
	struct addrinfo hints, *res = nullptr;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = addressFamily;
	hints.ai_socktype = SOCK_DGRAM;

	ip[0] = '\0';
	WSAStartupSingleton::AddRef();
	const int status = getaddrinfo(domainName, nullptr, &hints, &res);
	WSAStartupSingleton::Deref();
	if (status != 0 || res == nullptr)
		return;

	if (res->ai_family == AF_INET)
	{
		struct sockaddr_in *ipv4 = (struct sockaddr_in *) res->ai_addr;
		inet_ntop(AF_INET, &ipv4->sin_addr, ip, 65);
	}
	else if (res->ai_family == AF_INET6)
	{
		if (getnameinfo(res->ai_addr, sizeof(struct sockaddr_in6), ip, 65, nullptr, 0, NI_NUMERICHOST) != 0)
			ip[0] = '\0';
	}
	freeaddrinfo(res);
}

// Every address getaddrinfo returns for \a host in \a addressFamily, in its
// order, with port \a port; see RakNetSocket2::Resolve. \a addressFamily is
// what the socket reaches: AF_UNSPEC (dual-stack) reaches both families.
static int Resolve_Berkley(const char *host, unsigned short port, int addressFamily, MafiaNet::SystemAddress *out, int maxOut)
{
	if (host == nullptr || host[0] == '\0' || out == nullptr || maxOut <= 0)
		return 0;

	struct addrinfo hints, *res = nullptr;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = addressFamily;
	hints.ai_socktype = SOCK_DGRAM; // one entry per address, not one per socket type

	WSAStartupSingleton::AddRef();
	const int status = getaddrinfo(host, nullptr, &hints, &res);
	WSAStartupSingleton::Deref();
	if (status != 0 || res == nullptr)
		return 0;

	int count = 0;
	for (const struct addrinfo *ai = res; ai != nullptr && count < maxOut; ai = ai->ai_next)
	{
		if (ai->ai_addr == nullptr || (ai->ai_family != AF_INET && ai->ai_family != AF_INET6))
			continue;
		sockaddr_storage ss;
		memset(&ss, 0, sizeof(ss));
		memcpy(&ss, ai->ai_addr, ai->ai_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
		if (!MafiaNet::SockaddrToSystemAddress(ss, &out[count]))
			continue;
		// An IPv4-mapped answer (::ffff:a.b.c.d) comes back as plain IPv4, which an IPv6-only socket cannot send to.
		if (addressFamily != AF_UNSPEC && out[count].GetIPVersion() != (addressFamily == AF_INET6 ? 6 : 4))
			continue;
		out[count].SetPortHostOrder(port);
		++count;
	}
	freeaddrinfo(res);
	return count;
}

void RNS2_Berkley::SetSocketOptions(void)
{
	[[maybe_unused]] int r; // only checked by RakAssert (debug builds)
	// This doubles the max throughput rate
	int sock_opt = 1024 * 256;
	r = setsockopt__(rns2Socket, SOL_SOCKET, SO_RCVBUF, (char *) &sock_opt, sizeof(sock_opt));
	RakAssert(r == 0);

	// Immediate hard close. Don't linger the socket, or recreating the socket quickly on Vista fails.
	// Fails on some platforms when the socket protocol is not plain UDP

	sock_opt = 0;
	r = setsockopt__(rns2Socket, SOL_SOCKET, SO_LINGER, (char *) &sock_opt, sizeof(sock_opt));
	// Do not assert, ignore failure

#ifdef _WIN32
// Disable SIO_UDP_CONNRESET behavior on Windows
// By default, if a UDP sendto() results in an ICMP "port unreachable" response,
// Windows will cause subsequent recvfrom() calls to fail with WSAECONNRESET (10054).
// This is undesirable for UDP applications that may send to unreachable hosts.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
	BOOL bNewBehavior = FALSE;
	DWORD dwBytesReturned = 0;
	WSAIoctl(rns2Socket, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), nullptr, 0, &dwBytesReturned, nullptr, nullptr);
	// Ignore errors - this call may fail on older Windows versions
#endif

	// This doesn't make much difference: 10% maybe
	// Not supported on console 2
	sock_opt = 1024 * 16;
	r = setsockopt__(rns2Socket, SOL_SOCKET, SO_SNDBUF, (char *) &sock_opt, sizeof(sock_opt));
	RakAssert(r == 0);
}

void RNS2_Berkley::SetNonBlockingSocket(unsigned long nonblocking)
{
#ifdef _WIN32
	SLNET_VERIFY(ioctlsocket__(rns2Socket, FIONBIO, &nonblocking) == 0);
#else
	if (nonblocking)
		fcntl(rns2Socket, F_SETFL, O_NONBLOCK);
#endif
}
void RNS2_Berkley::SetBroadcastSocket(int broadcast)
{
	setsockopt__(rns2Socket, SOL_SOCKET, SO_BROADCAST, (char *) &broadcast, sizeof(broadcast));
}
void RNS2_Berkley::SetIPHdrIncl(int ipHdrIncl)
{

	setsockopt__(rns2Socket, IPPROTO_IP, IP_HDRINCL, (char *) &ipHdrIncl, sizeof(ipHdrIncl));
}
void RNS2_Berkley::SetDoNotFragment([[maybe_unused]] int opt) // unused where IP_DONTFRAGMENT does not exist
{
#if defined(IP_DONTFRAGMENT)
#if defined(_WIN32) && !defined(_DEBUG)
	// If this assert hit you improperly linked against WSock32.h
	RakAssert(IP_DONTFRAGMENT == 14);
#endif
	// The socket's own family, not the reported bound address (127.0.0.1 on a
	// dual-stack socket).
	setsockopt__(rns2Socket, SocketFamily() == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP, IP_DONTFRAGMENT, (char *) &opt, sizeof(opt));
#endif
}

void RNS2_Berkley::GetSystemAddressIPV4And6(RNS2Socket rns2Socket, SystemAddress *systemAddressOut)
{
	socklen_t slen;
	sockaddr_storage ss;
	slen = sizeof(ss);

	if (getsockname__(rns2Socket, (struct sockaddr *) &ss, &slen) != 0)
	{
#if defined(_WIN32) && defined(_DEBUG)
		DWORD dwIOError = GetLastError();
		LPVOID messageBuffer;
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		              nullptr, dwIOError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
		              (LPTSTR) &messageBuffer, 0, nullptr);
		// something has gone wrong here...
		RAKNET_DEBUG_TPRINTF(_T("getsockname failed:Error code - %lu\n%s"), dwIOError, static_cast<LPTSTR>(messageBuffer));

		//Free the buffer.
		LocalFree(messageBuffer);
#endif
		systemAddressOut->FromString(0);
		return;
	}

	if (ss.ss_family == AF_INET)
	{
		memcpy(&systemAddressOut->address.addr4, (sockaddr_in *) &ss, sizeof(sockaddr_in));
		systemAddressOut->debugPort = ntohs(systemAddressOut->address.addr4.sin_port);

		uint32_t zero = 0;
		if (memcmp(&systemAddressOut->address.addr4.sin_addr.s_addr, &zero, sizeof(zero)) == 0)
			systemAddressOut->SetToLoopback(4);
		//	systemAddressOut->address.addr4.sin_port=ntohs(systemAddressOut->address.addr4.sin_port);
	}
	else
	{
		memcpy(&systemAddressOut->address.addr6, (sockaddr_in6 *) &ss, sizeof(sockaddr_in6));
		systemAddressOut->debugPort = ntohs(systemAddressOut->address.addr6.sin6_port);

		char zero[16];
		memset(zero, 0, sizeof(zero));
		if (memcmp(&systemAddressOut->address.addr6.sin6_addr, &zero, sizeof(zero)) == 0)
			systemAddressOut->SetToLoopback(6);

		//	systemAddressOut->address.addr6.sin6_port=ntohs(systemAddressOut->address.addr6.sin6_port);
	}
}

void PrepareAddrInfoHints2(addrinfo *hints)
{
	memset(hints, 0, sizeof(addrinfo)); // make sure the struct is empty
	hints->ai_socktype = SOCK_DGRAM;    // UDP sockets
	hints->ai_flags = AI_PASSIVE;       // fill in my IP for me
}

// Whether \a host, a SocketDescriptor::hostAddress, means "every address":
// empty, the legacy "UNASSIGNED_SYSTEM_ADDRESS", or the IPv6 wildcard "::".
static bool IsWildcardBindHost(const char *host)
{
	if (host == nullptr || host[0] == 0 || _stricmp(host, "UNASSIGNED_SYSTEM_ADDRESS") == 0)
		return true;
	unsigned char address[16];
	if (inet_pton(AF_INET6, host, address) != 1)
		return false;
	for (unsigned char b : address)
	{
		if (b != 0)
			return false;
	}
	return true;
}

RNS2BindResult RNS2_Berkley::BindSharedIPV4And6(RNS2_BerkleyBindParameters *bindParameters)
{
	dualStack = false;
	if (bindParameters->addressFamily != AF_UNSPEC)
		return BindAddressFamily(bindParameters, bindParameters->addressFamily, false, nullptr);

	// AF_UNSPEC: one AF_INET6 socket with IPV6_V6ONLY=0 serves both families.
	// Only when the host cannot provide that (no IPv6 stack, an IPv4-only bind
	// address, or a system that refuses V6ONLY=0) fall back to
	// a plain IPv4 socket -- unless the caller requires dual-stack, which then
	// fails instead. A bind() failure -- the port is taken -- is reported as
	// is: an IPv4 socket on that port would hide the conflict.
	//
	// A specific bind address is never dual-stack, so unless dual-stack is
	// required it binds IPv4 first: a name with both an A and an AAAA record
	// ("localhost") then stays reachable by IPv4 clients, as with the
	// pre-0.27.0 AF_INET default. Only a host with no IPv4 address (an IPv6
	// literal, an AAAA-only name) binds IPv6, IPv6-only.
	if (!bindParameters->requireDualStack && !IsWildcardBindHost(bindParameters->hostAddress))
	{
		bool ipv4Unavailable = false;
		const RNS2BindResult ipv4Result = BindAddressFamily(bindParameters, AF_INET, false, &ipv4Unavailable);
		if (!ipv4Unavailable)
			return ipv4Result;
		return BindAddressFamily(bindParameters, AF_INET6, false, nullptr);
	}

	bool dualStackUnavailable = false;
	const RNS2BindResult result = IsWildcardBindHost(bindParameters->hostAddress) ? BindDualStackWildcard(bindParameters, &dualStackUnavailable) : BindAddressFamily(bindParameters, AF_INET6, true, &dualStackUnavailable);
	if (bindParameters->requireDualStack)
	{
		if (dualStackUnavailable)
			return BR_FAMILY_NOT_SUPPORTED;
		if (result == BR_SUCCESS && !dualStack)
		{
			// Bound, but to one IPv6 address (hostAddress was set): it cannot
			// exchange IPv4 traffic, so it is not the socket that was asked for.
			closesocket__(rns2Socket);
			rns2Socket = (RNS2Socket) INVALID_SOCKET;
			boundAddress = UNASSIGNED_SYSTEM_ADDRESS;
			return BR_FAMILY_NOT_SUPPORTED;
		}
		return result;
	}
	if (!dualStackUnavailable)
		return result;
	return BindAddressFamily(bindParameters, AF_INET, false, nullptr);
}

// An IPv4 UDP socket bound to the wildcard address and \a port (0: the OS
// picks one), whose port is written to \a *boundPort. INVALID_SOCKET when there
// is none; \a *bindFailed then tells a refused bind() (the port is taken for
// IPv4) from a host without IPv4 sockets.
static RNS2Socket ReserveIpv4Port(unsigned short port, unsigned short *boundPort, bool *bindFailed)
{
	*bindFailed = false;
	RNS2Socket reservation = socket__(AF_INET, SOCK_DGRAM, 0);
	if (reservation == (RNS2Socket) INVALID_SOCKET)
		return reservation;
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	sockaddr_in bound;
	socklen_t boundLength = sizeof(bound);
	if (bind__(reservation, (const sockaddr *) &address, sizeof(address)) != 0)
	{
		*bindFailed = true;
		closesocket__(reservation);
		return (RNS2Socket) INVALID_SOCKET;
	}
	if (getsockname__(reservation, (sockaddr *) &bound, &boundLength) != 0)
	{
		closesocket__(reservation);
		return (RNS2Socket) INVALID_SOCKET;
	}
	*boundPort = ntohs(bound.sin_port);
	return reservation;
}

RNS2BindResult RNS2_Berkley::BindDualStackWildcard(RNS2_BerkleyBindParameters *bindParameters, bool *dualStackUnavailable)
{
	// Only UDP has the two port spaces to keep in step (Startup may pass a
	// raw-socket protocol).
	const bool udp = bindParameters->type == SOCK_DGRAM && (bindParameters->protocol == 0 || bindParameters->protocol == IPPROTO_UDP);
	const unsigned short requestedPort = bindParameters->port;
	// An OS-assigned port held by another IPv6 socket is drawn again; after
	// that many tries, bind as the OS sees fit rather than fail Startup.
	const int attempts = !udp ? 0 : (requestedPort == 0 ? 16 : 1);
	for (int attempt = 0; attempt < attempts; ++attempt)
	{
		unsigned short port = 0;
		bool bindFailed = false;
		const RNS2Socket reservation = ReserveIpv4Port(requestedPort, &port, &bindFailed);
		if (reservation == (RNS2Socket) INVALID_SOCKET)
		{
			if (bindFailed && requestedPort != 0)
				return BR_FAILED_TO_BIND_SOCKET; // taken for IPv4: Linux refuses this bind too
			break;                               // no IPv4 here to collide with
		}

		RNS2_BerkleyBindParameters onPort = *bindParameters;
		onPort.port = port;
		// macOS: binds next to our own IPv4 socket, which no other socket
		// could hold the port alongside, and claims it once that one closes.
		// Linux refuses it (below), so the failure is not printed.
		RNS2BindResult result = BindAddressFamily(&onPort, AF_INET6, true, dualStackUnavailable, false);
		closesocket__(reservation);
		if (result != BR_FAILED_TO_BIND_SOCKET || *dualStackUnavailable)
			return result;
		// Linux (one port space for both families): the reservation itself
		// refused the bind. The port was free for IPv4, and a dual-stack bind
		// there now claims it for both. A failure on an OS-assigned port is
		// retried, so it is not printed.
		result = BindAddressFamily(&onPort, AF_INET6, true, dualStackUnavailable, requestedPort != 0);
		if (result != BR_FAILED_TO_BIND_SOCKET || *dualStackUnavailable || requestedPort != 0)
			return result;
		// Held by an IPv6 socket: try another port.
	}
	return BindAddressFamily(bindParameters, AF_INET6, true, dualStackUnavailable);
}

// reportBindFailure is unused in Windows release builds, which print nothing.
RNS2BindResult RNS2_Berkley::BindAddressFamily(RNS2_BerkleyBindParameters *bindParameters, int family, bool wantDualStack, bool *dualStackUnavailable, [[maybe_unused]] bool reportBindFailure)
{
	struct addrinfo hints;
	struct addrinfo *servinfo = nullptr, *aip; // will point to the results
	PrepareAddrInfoHints2(&hints);
	hints.ai_family = family;
	// hints keep SOCK_DGRAM: they only pick the address to bind (a numeric port
	// is rejected for SOCK_RAW by some resolvers); the socket itself is created
	// with the caller's type and protocol below.
	char portStr[32];
	Itoa(bindParameters->port, portStr, 10);

	// On Ubuntu, "" returns "No address associated with hostname" while 0 works.
	const char *node = bindParameters->hostAddress;
	if (node && (_stricmp(node, "UNASSIGNED_SYSTEM_ADDRESS") == 0 || node[0] == 0))
		node = nullptr;

	WSAStartupSingleton::AddRef();
	const int gaiResult = getaddrinfo(node, portStr, &hints, &servinfo);
	WSAStartupSingleton::Deref();
	if (gaiResult != 0 || servinfo == nullptr)
	{
		// No IPv6 address for this host (an IPv4 literal, an A-only name, or no
		// IPv6 at all): dual-stack is not possible, IPv4 may still be.
		if (dualStackUnavailable)
			*dualStackUnavailable = true;
		return RNS2_IsFamilyNotSupportedResolveError(gaiResult) ? BR_FAMILY_NOT_SUPPORTED : BR_FAILED_TO_BIND_SOCKET;
	}

	// Set once any candidate gets a socket, or fails for a reason other than
	// the family. Only when every candidate was refused for its family is the
	// result BR_FAMILY_NOT_SUPPORTED; a failed bind() stays BR_FAILED_TO_BIND_SOCKET.
	bool onlyFamilyErrors = true;
	bool anySocket = false;

	// Try all returned addresses until one works
	for (aip = servinfo; aip != nullptr; aip = aip->ai_next)
	{
		// Open socket. The address type depends on what getaddrinfo() gave us;
		// type and protocol are the caller's (protocol may be a raw-socket one).
		rns2Socket = socket__(aip->ai_family, bindParameters->type, bindParameters->protocol);

		if (rns2Socket == (RNS2Socket) INVALID_SOCKET)
		{
			if (!RNS2_IsFamilyNotSupportedSocketError(RNS2_GetLastSocketError()))
				onlyFamilyErrors = false;
			continue;
		}
		onlyFamilyErrors = false;
		anySocket = true;

		// IPV6_V6ONLY is always set explicitly, never left to the OS default
		// (dual-stack on Linux/Windows, configurable per host). An explicit
		// AF_INET6 socket is IPv6-only, so an IPv4 socket can bind the same port
		// alongside it; an AF_UNSPEC one is dual-stack.
		bool v6onlyOff = false;
		if (aip->ai_family == AF_INET6)
		{
			int ipv6only = wantDualStack ? 0 : 1;
			const int r = setsockopt__(rns2Socket, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &ipv6only, sizeof(ipv6only));
			if (wantDualStack)
			{
				if (r != 0)
				{
					// This system cannot do dual-stack; let the caller
					// fall back to IPv4 rather than serve IPv6 only.
					closesocket__(rns2Socket);
					rns2Socket = (RNS2Socket) INVALID_SOCKET;
					freeaddrinfo(servinfo);
					*dualStackUnavailable = true;
					return BR_FAMILY_NOT_SUPPORTED;
				}
				v6onlyOff = true;
			}
			// Otherwise ignore errors: V6ONLY=1 is only needed to share the port.
		}

		if (bind__(rns2Socket, aip->ai_addr, (int) aip->ai_addrlen) >= 0)
		{
			if (aip->ai_family == AF_INET)
				memcpy(&boundAddress.address.addr4, aip->ai_addr, sizeof(sockaddr_in));
			else
				memcpy(&boundAddress.address.addr6, aip->ai_addr, sizeof(sockaddr_in6));

			// Dual-stack only on the wildcard address: a socket bound to one
			// IPv6 address cannot exchange IPv4 traffic whatever V6ONLY says.
			if (v6onlyOff)
			{
				static const unsigned char any6[16] = {0};
				dualStack = memcmp(&((const sockaddr_in6 *) aip->ai_addr)->sin6_addr, any6, sizeof(any6)) == 0;
			}

			freeaddrinfo(servinfo); // free the linked-list

			SetSocketOptions();
			SetNonBlockingSocket(bindParameters->nonBlockingSocket);
			SetBroadcastSocket(bindParameters->setBroadcast);
			SetIPHdrIncl(bindParameters->setIPHdrIncl);

			GetSystemAddressIPV4And6(rns2Socket, &boundAddress);
			// A dual-stack wildcard socket reports 127.0.0.1|port, like an IPv4
			// wildcard socket: callers connect to and compare with the bound
			// address, and IPv4 loopback always reaches this socket while ::1
			// may be disabled. IsDualStack() tells the two sockets apart.
			if (dualStack)
			{
				const unsigned short port = boundAddress.GetPortNetworkOrder();
				boundAddress.SetToLoopback(4);
				boundAddress.SetPortNetworkOrder(port);
			}

			return BR_SUCCESS;
		}

		// Say why, as the IPv4 bind path always did: usually the port is taken.
#if defined(_WIN32)
#if defined(_DEBUG)
		const DWORD bindError = static_cast<DWORD>(WSAGetLastError());
		if (reportBindFailure)
			RAKNET_DEBUG_PRINTF("bind__() failed for an %s socket on port %u: error %lu\n", aip->ai_family == AF_INET6 ? "IPv6" : "IPv4", (unsigned) bindParameters->port, bindError);
#endif
#else
		const int bindError = errno;
		if (reportBindFailure)
			RAKNET_DEBUG_PRINTF("bind__() failed for an %s socket on port %u: %s (errno %d)\n", aip->ai_family == AF_INET6 ? "IPv6" : "IPv4", (unsigned) bindParameters->port, strerror(bindError), bindError);
#endif
		closesocket__(rns2Socket);
		rns2Socket = (RNS2Socket) INVALID_SOCKET;
	}

	freeaddrinfo(servinfo);
	// No IPv6 socket could be opened at all: the host has no usable IPv6 stack.
	if (!anySocket && dualStackUnavailable)
		*dualStackUnavailable = true;
	return onlyFamilyErrors ? BR_FAMILY_NOT_SUPPORTED : BR_FAILED_TO_BIND_SOCKET;
}

SystemAddress RNS2_Berkley::SelfSendAddress(void) const
{
	// A dual-stack socket already reports 127.0.0.1 (see BindAddressFamily).
	return boundAddress;
}

void RNS2_Berkley::RecvFromBlockingIPV4And6(RNS2RecvStruct *recvFromStruct)
{
	sockaddr_storage their_addr;
	sockaddr *sockAddrPtr;
	socklen_t sockLen;
	socklen_t *socketlenPtr = (socklen_t *) &sockLen;
	memset(&their_addr, 0, sizeof(their_addr));
	int dataOutSize;
	const int flag = 0;

	{
		sockLen = sizeof(their_addr);
		sockAddrPtr = (sockaddr *) &their_addr;
	}

	dataOutSize = MAXIMUM_MTU_SIZE;

	recvFromStruct->bytesRead = recvfrom__(rns2Socket, recvFromStruct->data, dataOutSize, flag, sockAddrPtr, socketlenPtr);

#if defined(_WIN32) && defined(_DEBUG)
	if (recvFromStruct->bytesRead == -1)
	{
		DWORD dwIOError = GetLastError();
		// 10035 = WSAEWOULDBLOCK (expected for non-blocking sockets)
		// 10054 = WSAECONNRESET (expected for UDP - prior sendto received ICMP port unreachable)
		if (dwIOError != 10035 && dwIOError != 10054)
		{
			LPVOID messageBuffer;
			FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			              nullptr, dwIOError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
			              (LPTSTR) &messageBuffer, 0, nullptr);
			// I see this hit on XP with IPV6 for some reason
			RAKNET_DEBUG_TPRINTF(_T("Warning: recvfrom failed:Error code - %lu\n%s"), dwIOError, static_cast<LPTSTR>(messageBuffer));
			LocalFree(messageBuffer);
		}
	}
#endif

	if (recvFromStruct->bytesRead <= 0)
		return;
	recvFromStruct->timeRead = MafiaNet::GetTimeUS();

	// Same decoding as the batched recvmmsg path, including turning the
	// ::ffff:a.b.c.d a dual-stack socket reports for IPv4 peers into AF_INET.
	// A source that cannot be decoded is dropped, not attributed to anyone.
	if (!SockaddrToSystemAddress(their_addr, &recvFromStruct->systemAddress))
		recvFromStruct->bytesRead = -1;
}

#if defined(__linux__)
void RNS2_Berkley::RecvFromBatchedLoop(void)
{
	RNS2RecvStruct *slots[MMSG_BATCH_MAX];
	struct mmsghdr msgs[MMSG_BATCH_MAX];
	struct iovec iovecs[MMSG_BATCH_MAX];
	sockaddr_storage addrs[MMSG_BATCH_MAX];
	int lens[MMSG_BATCH_MAX];

	// Recv structs live across passes rather than being rebuilt each time. Only
	// the slots that actually received a datagram change hands; the untouched
	// tail is carried over, so the steady state costs one alloc/free round trip
	// per datagram -- the same as the scalar path -- instead of MMSG_BATCH_MAX of
	// them per pass. Each of those round trips takes the event handler's pool
	// mutex, and at typical packet rates recvmmsg returns far fewer than
	// MMSG_BATCH_MAX datagrams, so rebuilding the batch would dominate the
	// syscall saving the batching exists for. The same carry-over covers failed
	// passes: Linux reports asynchronous errors on a connectionless UDP socket
	// (ECONNREFUSED when a peer's port is closed) and those must not churn the
	// pool either.
	// Already known missing on this system (another socket hit it first):
	// hand straight back to RecvFromLoopInt's scalar loop.
	if (MmsgUnavailable())
		return;

	unsigned allocated = 0;
	unsigned consecutiveErrors = 0;

	while (endThreads == false)
	{
		// Top the batch back up to full, then point each iovec at the struct's
		// data buffer so recvmmsg writes straight into the structs we hand to
		// the event handler -- no extra copy.
		for (; allocated < MMSG_BATCH_MAX; ++allocated)
		{
			RNS2RecvStruct *s = binding.eventHandler->AllocRNS2RecvStruct();
			if (s == nullptr)
				break;
			s->socket = this;
			slots[allocated] = s;
		}
		if (allocated == 0)
		{
			// Out of recv structs entirely; back off rather than spin.
			RakSleep(1);
			continue;
		}

		// Rebuilt every pass, not just on allocation: recvmmsg writes back
		// msg_namelen and msg_flags, so a reused header must be reset.
		for (unsigned i = 0; i < allocated; ++i)
		{
			iovecs[i].iov_base = slots[i]->data;
			iovecs[i].iov_len = sizeof(slots[i]->data);
			memset(&msgs[i], 0, sizeof(msgs[i]));
			msgs[i].msg_hdr.msg_iov = &iovecs[i];
			msgs[i].msg_hdr.msg_iovlen = 1;
			msgs[i].msg_hdr.msg_name = &addrs[i];
			msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
		}

		// MSG_WAITFORONE: block until at least one datagram is ready, then return
		// everything already queued. Without it recvmmsg would wait to fill the
		// whole array (or hit a timeout), adding latency at low packet rates.
		int n = recvmmsg(rns2Socket, msgs, allocated, MSG_WAITFORONE, nullptr);
		// Read errno before anything else can clobber it -- GetTimeUS() below
		// is a syscall too, and errno is only meaningful immediately after the
		// call that failed.
		const int recvErrno = (n < 0) ? errno : 0;
		MafiaNet::TimeUS now = MafiaNet::GetTimeUS();

		if (n < 0 && MmsgSyscallMissing(recvErrno))
		{
			// No recvmmsg on this kernel/sandbox. Backing off would spin this
			// thread forever without ever surfacing a packet, so give the slots
			// back and let RecvFromLoopInt's scalar loop take over permanently.
			MarkMmsgUnavailable();
			break;
		}

		if (n <= 0)
		{
			// Interrupted, an asynchronous socket error, or -- not expected under
			// MSG_WAITFORONE, but not worth a hot spin if a kernel ever does it --
			// a pass that returned no datagrams at all. (Shutdown does not
			// come through here: BlockOnStopRecvPollingThread pokes the socket
			// with a real datagram, so it surfaces as a normal n>=1 pass and the
			// loop condition below catches endThreads.) Keep the batch and back
			// off progressively so a persistent error cannot spin this thread at
			// 100%; the loop condition is still checked first, so shutdown stays
			// prompt.
			if (consecutiveErrors < MMSG_ERROR_BACKOFF_MAX_MS)
				++consecutiveErrors;
			RakSleep(consecutiveErrors);
			continue;
		}
		consecutiveErrors = 0;

		const unsigned received = (unsigned) n;
		for (unsigned i = 0; i < received; ++i)
			lens[i] = (int) msgs[i].msg_len;

		// Hand the handler exactly the slots that received a datagram -- it takes
		// ownership of those and of those alone. DispatchRecvBatch is the same
		// portable, unit-tested routine exercised by the hermetic MmsgBatch tests.
		DispatchRecvBatch(binding.eventHandler, slots, received, lens, addrs,
		                  received, this, now);

		// Carry the untouched tail over to the next pass; only the consumed slots
		// are topped back up at the head of the loop. The arithmetic lives in
		// CompactRecvSlots so it is unit tested (MmsgBatchTests) rather than only
		// ever exercised here, on Linux, at runtime.
		allocated = CompactRecvSlots(slots, allocated, received);
	}

	// Release whatever the last pass was still holding.
	for (unsigned i = 0; i < allocated; ++i)
		binding.eventHandler->DeallocRNS2RecvStruct(slots[i]);
}
#endif // __linux__

#endif // file header

#endif // #ifdef RAKNET_SOCKET_2_INLINE_FUNCTIONS

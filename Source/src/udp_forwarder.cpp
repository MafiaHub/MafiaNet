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

#include "mafianet/udp_forwarder.h"

#if _RAKNET_SUPPORT_UDPForwarder == 1

#include "mafianet/get_time.h"
#include "mafianet/mtu_size.h"
#include "mafianet/socket_layer.h"
#include "mafianet/wsa_startup_singleton.h"
#include "mafianet/sleep.h"
#include "mafianet/vector_util.h"
#include "mafianet/linux_strings.h"
#include "mafianet/socket_defines.h"
#include "mafianet/sockaddr_convert.h"
#include "errno.h"
#include <cassert>
#include <utility>

#ifdef _WIN32
#include <tchar.h>
#else
#ifndef _T
#define _T(x) (x)
#endif
#include <sys/types.h>  // used for getaddrinfo()
#include <sys/socket.h> // used for getaddrinfo()
#include <netdb.h>      // used for getaddrinfo()
#endif

#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif

using namespace MafiaNet;
static const unsigned short DEFAULT_MAX_FORWARD_ENTRIES = 64;

namespace MafiaNet
{
RAK_THREAD_DECLARATION(UpdateUDPForwarderGlobal);
}

UDPForwarder::ForwardEntry::ForwardEntry()
{
	socket = INVALID_SOCKET;
	socketFamily = AF_INET;
	dualStack = false;
	timeLastDatagramForwarded = MafiaNet::GetTimeMS();
	addr1Confirmed = UNASSIGNED_SYSTEM_ADDRESS;
	addr2Confirmed = UNASSIGNED_SYSTEM_ADDRESS;
}
UDPForwarder::ForwardEntry::~ForwardEntry()
{
	if (socket != INVALID_SOCKET)
		closesocket__(socket);
}

UDPForwarder::UDPForwarder()
{
#ifdef _WIN32
	WSAStartupSingleton::AddRef();
#endif

	maxForwardEntries = DEFAULT_MAX_FORWARD_ENTRIES;
	nextInputId = 0;
}
UDPForwarder::~UDPForwarder()
{
	Shutdown();

#ifdef _WIN32
	WSAStartupSingleton::Deref();
#endif
}
void UDPForwarder::Startup(void)
{
	if (isRunning.GetValue() > 0)
		return;

	isRunning.Increment();

	int errorCode;


	errorCode = MafiaNet::RakThread::Create(UpdateUDPForwarderGlobal, this);

	if (errorCode != 0)
	{
		RakAssert(0);
		return;
	}

	while (threadRunning.GetValue() == 0)
		RakSleep(30);
}
void UDPForwarder::Shutdown(void)
{
	if (isRunning.GetValue() == 0)
		return;
	isRunning.Decrement();

	while (threadRunning.GetValue() > 0)
		RakSleep(30);

	unsigned int j;
	for (j = 0; j < forwardListNotUpdated.size(); j++)
		delete forwardListNotUpdated[j];
	forwardListNotUpdated.clear();
}
void UDPForwarder::SetMaxForwardEntries(unsigned short maxEntries)
{
	RakAssert(maxEntries > 0 && maxEntries < 65535 / 2);
	maxForwardEntries = maxEntries;
}
int UDPForwarder::GetMaxForwardEntries(void) const
{
	return maxForwardEntries;
}
int UDPForwarder::GetUsedForwardEntries(void) const
{
	return (int) forwardListNotUpdated.size();
}
UDPForwarderResult UDPForwarder::StartForwarding(SystemAddress source, SystemAddress destination, MafiaNet::TimeMS timeoutOnNoDataMS, const char *forceHostAddress, unsigned short socketFamily,
                                                 unsigned short *forwardingPort, __UDPSOCKET__ *forwardingSocket)
{
	// Invalid parameters?
	if (timeoutOnNoDataMS == 0 || timeoutOnNoDataMS > UDP_FORWARDER_MAXIMUM_TIMEOUT || source == UNASSIGNED_SYSTEM_ADDRESS || destination == UNASSIGNED_SYSTEM_ADDRESS)
		return UDPFORWARDER_INVALID_PARAMETERS;

	if (isRunning.GetValue() == 0)
		return UDPFORWARDER_NOT_RUNNING;

	unsigned int inputId = nextInputId++;

	StartForwardingInputStruct sfis;
	sfis.source = source;
	sfis.destination = destination;
	sfis.timeoutOnNoDataMS = timeoutOnNoDataMS;
	RakAssert(timeoutOnNoDataMS != 0);
	if (forceHostAddress && forceHostAddress[0])
		sfis.forceHostAddress = forceHostAddress;
	sfis.socketFamily = socketFamily;
	sfis.inputId = inputId;
	startForwardingInput.Push(std::move(sfis));

	for (;;)
	{
		RakSleep(0);
		startForwardingOutputMutex.Lock();
		for (unsigned int i = 0; i < startForwardingOutput.size(); i++)
		{
			if (startForwardingOutput[i].inputId == inputId)
			{
				if (startForwardingOutput[i].result == UDPFORWARDER_SUCCESS)
				{
					if (forwardingPort)
						*forwardingPort = startForwardingOutput[i].forwardingPort;
					if (forwardingSocket)
						*forwardingSocket = startForwardingOutput[i].forwardingSocket;
				}
				UDPForwarderResult res = startForwardingOutput[i].result;
				startForwardingOutput.erase(startForwardingOutput.begin() + i);
				startForwardingOutputMutex.Unlock();
				return res;
			}
		}
		startForwardingOutputMutex.Unlock();
	}
}
void UDPForwarder::StopForwarding(SystemAddress source, SystemAddress destination)
{
	StopForwardingStruct sfs;
	sfs.destination = destination;
	sfs.source = source;
	stopForwardingCommands.Push(sfs);
}
void UDPForwarder::RecvFrom(MafiaNet::TimeMS curTime, ForwardEntry *forwardEntry)
{
	char data[MAXIMUM_MTU_SIZE];

	sockaddr_storage their_addr;
	memset(&their_addr, 0, sizeof(their_addr));
	sockaddr *sockAddrPtr;
	socklen_t sockLen;
	socklen_t *socketlenPtr = (socklen_t *) &sockLen;
	sockLen = sizeof(their_addr);
	sockAddrPtr = (sockaddr *) &their_addr;

#if defined(__GNUC__)
#if defined(MSG_DONTWAIT)
	const int flag = MSG_DONTWAIT;
#else
	const int flag = 0x40;
#endif
#else
	const int flag = 0;
#endif

	int receivedDataLen;
	//unsigned short portnum=0;

	receivedDataLen = recvfrom__(forwardEntry->socket, data, MAXIMUM_MTU_SIZE, flag, sockAddrPtr, socketlenPtr);

	if (receivedDataLen < 0)
	{
#if defined(_WIN32) && defined(_DEBUG)
		DWORD dwIOError = WSAGetLastError();

		if (dwIOError != WSAECONNRESET && dwIOError != WSAEINTR && dwIOError != WSAETIMEDOUT && dwIOError != WSAEWOULDBLOCK)
		{
			LPTSTR messageBuffer;
			FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			              nullptr, dwIOError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
			              (LPTSTR) &messageBuffer, 0, nullptr);
			// something has gone wrong here...
			RAKNET_DEBUG_TPRINTF(_T("recvfrom failed:Error code - %lu\n%s"), dwIOError, messageBuffer);

			//Free the buffer.
			LocalFree(messageBuffer);
		}
#else
		if (errno != EAGAIN && errno != 0
#if defined(__GNUC__)
		    && errno != EWOULDBLOCK
#endif
		)
		{
			printf("errno=%i\n", errno);
		}
#endif
	}

	if (receivedDataLen <= 0)
		return;

	// An IPv4 peer seen through a dual-stack socket is a mapped address: compare it canonically
	SystemAddress receivedAddr;
	if (!SockaddrToSystemAddress(their_addr, &receivedAddr))
		return;

	SystemAddress forwardTarget;

	bool confirmed1 = forwardEntry->addr1Confirmed != UNASSIGNED_SYSTEM_ADDRESS;
	bool confirmed2 = forwardEntry->addr2Confirmed != UNASSIGNED_SYSTEM_ADDRESS;
	bool matchConfirmed1 =
	    confirmed1 &&
	    forwardEntry->addr1Confirmed == receivedAddr;
	bool matchConfirmed2 =
	    confirmed2 &&
	    forwardEntry->addr2Confirmed == receivedAddr;
	bool matchUnconfirmed1 = forwardEntry->addr1Unconfirmed.EqualsExcludingPort(receivedAddr);
	bool matchUnconfirmed2 = forwardEntry->addr2Unconfirmed.EqualsExcludingPort(receivedAddr);

	if (matchConfirmed1 == true || (matchConfirmed2 == false && confirmed1 == false && matchUnconfirmed1 == true))
	{
		// Forward to addr2
		if (forwardEntry->addr1Confirmed == UNASSIGNED_SYSTEM_ADDRESS)
		{
			forwardEntry->addr1Confirmed = receivedAddr;
		}
		if (forwardEntry->addr2Confirmed != UNASSIGNED_SYSTEM_ADDRESS)
			forwardTarget = forwardEntry->addr2Confirmed;
		else
			forwardTarget = forwardEntry->addr2Unconfirmed;
	}
	else if (matchConfirmed2 == true || (confirmed2 == false && matchUnconfirmed2 == true))
	{
		// Forward to addr1
		if (forwardEntry->addr2Confirmed == UNASSIGNED_SYSTEM_ADDRESS)
		{
			forwardEntry->addr2Confirmed = receivedAddr;
		}
		if (forwardEntry->addr1Confirmed != UNASSIGNED_SYSTEM_ADDRESS)
			forwardTarget = forwardEntry->addr1Confirmed;
		else
			forwardTarget = forwardEntry->addr1Unconfirmed;
	}
	else
	{
		return;
	}

	// Forward to dest, in the form this socket can send to (an IPv4 target is mapped on a dual-stack socket)
	sockaddr_storage target;
	const socklen_t targetLength = SystemAddressToSockaddr(forwardTarget, forwardEntry->dualStack, &target, forwardEntry->socketFamily);
	if (targetLength == 0)
		return; // this socket cannot reach that family
	sendto__(forwardEntry->socket, data, receivedDataLen, 0, (const sockaddr *) &target, targetLength);

	forwardEntry->timeLastDatagramForwarded = curTime;
}

// Opens a UDP socket of exactly \a family (AF_INET or AF_INET6) bound to an OS-chosen port on \a host (nullptr: the
// wildcard). An AF_INET6 socket is IPv6 only unless \a dualStack; a system that refuses IPV6_V6ONLY=0 then yields no
// socket. Returns INVALID_SOCKET on failure.
static __UDPSOCKET__ BindForwardingSocket(const char *host, int family, bool dualStack)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(addrinfo)); // make sure the struct is empty
	hints.ai_family = family;
	hints.ai_socktype = SOCK_DGRAM;      // UDP sockets
	hints.ai_flags = AI_PASSIVE;         // fill in my IP for me
	struct addrinfo *servinfo = 0, *aip; // will point to the results
	if (getaddrinfo(host, "0", &hints, &servinfo) != 0 || servinfo == nullptr)
		return INVALID_SOCKET;

	__UDPSOCKET__ bound = INVALID_SOCKET;
	for (aip = servinfo; aip != nullptr; aip = aip->ai_next)
	{
		__UDPSOCKET__ candidate = socket__(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
		if (candidate == INVALID_SOCKET)
			continue;
		if (aip->ai_family == AF_INET6)
		{
			// Always explicit, never the OS default.
			int v6only = dualStack ? 0 : 1;
			if (setsockopt__(candidate, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &v6only, sizeof(v6only)) != 0 && dualStack)
			{
				closesocket__(candidate); // this system cannot do dual-stack (OpenBSD)
				continue;
			}
		}
		if (bind__(candidate, aip->ai_addr, (int) aip->ai_addrlen) >= 0)
		{
			bound = candidate;
			break;
		}
		closesocket__(candidate);
	}
	freeaddrinfo(servinfo);
	return bound;
}
void UDPForwarder::UpdateUDPForwarder(void)
{
	MafiaNet::TimeMS curTime = MafiaNet::GetTimeMS();

	StartForwardingOutputStruct sfos;
	sfos.forwardingSocket = INVALID_SOCKET;
	sfos.forwardingPort = 0;
	sfos.inputId = 0;
	sfos.result = UDPFORWARDER_RESULT_COUNT;

	while (auto sfis = startForwardingInput.TryPop())
	{

		if (GetUsedForwardEntries() > maxForwardEntries)
		{
			sfos.result = UDPFORWARDER_NO_SOCKETS;
		}
		else
		{
			sfos.result = UDPFORWARDER_RESULT_COUNT;

			for (unsigned int i = 0; i < forwardListNotUpdated.size(); i++)
			{
				if (
				    (forwardListNotUpdated[i]->addr1Unconfirmed == sfis->source &&
				     forwardListNotUpdated[i]->addr2Unconfirmed == sfis->destination) ||
				    (forwardListNotUpdated[i]->addr1Unconfirmed == sfis->destination &&
				     forwardListNotUpdated[i]->addr2Unconfirmed == sfis->source))
				{
					ForwardEntry *fe = forwardListNotUpdated[i];
					sfos.forwardingPort = SocketLayer::GetLocalPort(fe->socket);
					sfos.forwardingSocket = fe->socket;
					sfos.result = UDPFORWARDER_FORWARDING_ALREADY_EXISTS;
					break;
				}
			}

			if (sfos.result == UDPFORWARDER_RESULT_COUNT)
			{
				int sock_opt;
				ForwardEntry *fe = new UDPForwarder::ForwardEntry;
				fe->addr1Unconfirmed = sfis->source;
				fe->addr2Unconfirmed = sfis->destination;
				fe->timeoutOnNoDataMS = sfis->timeoutOnNoDataMS;

				const char *host = nullptr;
				if (!sfis->forceHostAddress.IsEmpty() && sfis->forceHostAddress != "UNASSIGNED_SYSTEM_ADDRESS")
					host = sfis->forceHostAddress.C_String();

				if (sfis->socketFamily == AF_UNSPEC)
				{
					// One AF_INET6 socket with IPV6_V6ONLY=0 serves both families, whatever order the resolver
					// returns the wildcard addresses in. Fall back to IPv4 only when the host cannot do that.
					fe->socket = BindForwardingSocket(host, AF_INET6, true);
					fe->socketFamily = AF_INET6;
					fe->dualStack = true;
					if (fe->socket == INVALID_SOCKET)
					{
						fe->socket = BindForwardingSocket(host, AF_INET, false);
						fe->socketFamily = AF_INET;
						fe->dualStack = false;
					}
				}
				else
				{
					fe->socket = BindForwardingSocket(host, sfis->socketFamily, false);
					fe->socketFamily = (short) sfis->socketFamily;
					fe->dualStack = false;
				}

				if (fe->socket == INVALID_SOCKET)
					sfos.result = UDPFORWARDER_BIND_FAILED;
				else
					sfos.result = UDPFORWARDER_SUCCESS;

				if (sfos.result == UDPFORWARDER_SUCCESS)
				{
					sfos.forwardingPort = SocketLayer::GetLocalPort(fe->socket);
					sfos.forwardingSocket = fe->socket;

					sock_opt = 1024 * 256;
					setsockopt__(fe->socket, SOL_SOCKET, SO_RCVBUF, (char *) &sock_opt, sizeof(sock_opt));
					sock_opt = 0;
					setsockopt__(fe->socket, SOL_SOCKET, SO_LINGER, (char *) &sock_opt, sizeof(sock_opt));
#ifdef _WIN32
					unsigned long nonblocking = 1;
					ioctlsocket__(fe->socket, FIONBIO, &nonblocking);


#else
					fcntl(fe->socket, F_SETFL, O_NONBLOCK);
#endif

					forwardListNotUpdated.push_back(fe);
				}
				else
				{
					// Binding failed: the entry was never registered, do not leak it
					delete fe;
				}
			}
		}

		// Push result
		sfos.inputId = sfis->inputId;
		startForwardingOutputMutex.Lock();
		startForwardingOutput.push_back(sfos);
		startForwardingOutputMutex.Unlock();
	}

	while (auto sfs = stopForwardingCommands.TryPop())
	{

		ForwardEntry *fe;
		for (unsigned int i = 0; i < forwardListNotUpdated.size(); i++)
		{
			if (
			    (forwardListNotUpdated[i]->addr1Unconfirmed == sfs->source &&
			     forwardListNotUpdated[i]->addr2Unconfirmed == sfs->destination) ||
			    (forwardListNotUpdated[i]->addr1Unconfirmed == sfs->destination &&
			     forwardListNotUpdated[i]->addr2Unconfirmed == sfs->source))
			{
				fe = forwardListNotUpdated[i];
				SwapRemove(forwardListNotUpdated, i);
				delete fe;
				break;
			}
		}
	}

	unsigned int i;

	i = 0;
	while (i < forwardListNotUpdated.size())
	{
		if (curTime > forwardListNotUpdated[i]->timeLastDatagramForwarded && // Account for timestamp wrap
		    curTime > forwardListNotUpdated[i]->timeLastDatagramForwarded + forwardListNotUpdated[i]->timeoutOnNoDataMS)
		{
			delete forwardListNotUpdated[i];
			forwardListNotUpdated.erase(forwardListNotUpdated.begin() + i);
		}
		else
			i++;
	}

	ForwardEntry *forwardEntry;
	for (i = 0; i < forwardListNotUpdated.size(); i++)
	{
		forwardEntry = forwardListNotUpdated[i];
		RecvFrom(curTime, forwardEntry);
	}
}

namespace MafiaNet
{
RAK_THREAD_DECLARATION(UpdateUDPForwarderGlobal)
{


	UDPForwarder *udpForwarder = (UDPForwarder *) arguments;


	udpForwarder->threadRunning.Increment();
	while (udpForwarder->isRunning.GetValue() > 0)
	{
		udpForwarder->UpdateUDPForwarder();

		// 12/1/2010 Do not change from 0
		// See http://www.jenkinssoftware.com/forum/index.php?topic=4033.0;topicseen
		// Avoid 100% reported CPU usage
		if (udpForwarder->forwardListNotUpdated.size() == 0)
			RakSleep(30);
		else
			RakSleep(0);
	}
	udpForwarder->threadRunning.Decrement();


	return 0;
}

} // namespace MafiaNet

#endif // #if _RAKNET_SUPPORT_FileOperations==1

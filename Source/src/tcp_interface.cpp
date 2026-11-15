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

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_TCPInterface == 1

/// \file
/// \brief A simple TCP based server allowing sends and receives.  Can be connected to by a telnet client.
///


#include "mafianet/tcp_interface.h"
#ifdef _WIN32
typedef int socklen_t;
#else
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h> // EINPROGRESS, EINTR (ConnectAbortable)
#include <fcntl.h> // O_NONBLOCK (ConnectAbortable)
#endif
#include <string.h>
#include "mafianet/assert.h"
#include <stdio.h>
#include "mafianet/assert.h"
#include "mafianet/sleep.h"
#include "mafianet/string_compressor.h"
#include "mafianet/string_table.h"
#include "mafianet/itoa.h"
#include "mafianet/socket_layer.h"
#include "mafianet/socket_defines.h"
#include "mafianet/sockaddr_convert.h"
#if (defined(__GNUC__) || defined(__GCCXML__)) && !defined(__WIN32__)
#include <netdb.h>
#endif

#ifdef _DO_PRINTF
#endif

#ifdef _WIN32
#include "mafianet/wsa_startup_singleton.h"
#endif
namespace MafiaNet
{
RAK_THREAD_DECLARATION(UpdateTCPInterfaceLoop);
RAK_THREAD_DECLARATION(ConnectionAttemptLoop);
}
#ifdef _MSC_VER
#pragma warning(push)
#endif
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(TCPInterface, TCPInterface);

TCPInterface::TCPInterface()
{
	listenSocket = 0;
	remoteClients = 0;
	remoteClientsLength = 0;

	StringCompressor::AddReference();
	MafiaNet::StringTable::AddReference();


#ifdef _WIN32
	WSAStartupSingleton::AddRef();
#endif
}
TCPInterface::~TCPInterface()
{
	Stop();
#ifdef _WIN32
	WSAStartupSingleton::Deref();
#endif

	MafiaNet::OP_DELETE_ARRAY(remoteClients, _FILE_AND_LINE_);

	StringCompressor::RemoveReference();
	MafiaNet::StringTable::RemoveReference();
}

// The FromString() ipVersion matching a socket family: AF_INET resolves IPv4 only, AF_INET6 IPv6 only, anything else both.
static int SocketFamilyToIpVersion(unsigned short socketFamily)
{
	if (socketFamily == AF_INET)
		return 4;
	if (socketFamily == AF_INET6)
		return 6;
	return 0;
}

// The host string SocketConnect resolves for Connect(host). A host name goes verbatim, so SocketConnect tries every
// address it resolves to (in the socket family) rather than only the first. A numeric literal names a single address
// and goes as FromString() parsed it into \a parsed (nullptr if it failed), which maps a loopback literal into the
// requested family.
static std::string ConnectHost(const char *hostPart, const SystemAddress *parsed)
{
	if (parsed != nullptr)
	{
		struct addrinfo hints, *res = nullptr;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_flags = AI_NUMERICHOST;
		const bool numeric = getaddrinfo(hostPart, nullptr, &hints, &res) == 0;
		if (res)
			freeaddrinfo(res);
		if (numeric)
		{
			char buffer[SystemAddress::TO_STRING_BUFFER_SIZE];
			parsed->ToString(false, buffer, sizeof(buffer));
			return buffer;
		}
	}
	return hostPart;
}

static bool SetSocketBlocking(__TCPSOCKET__ s, bool blocking)
{
#ifdef _WIN32
	u_long nonBlocking = blocking ? 0 : 1;
	return ioctlsocket__(s, FIONBIO, &nonBlocking) == 0;
#else
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags == -1)
		return false;
	return fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

// How often a connect in progress checks whether Stop() was called.
static const long CONNECT_POLL_MS = 50;

enum ConnectResult
{
	CONNECT_SUCCEEDED,
	CONNECT_FAILED,
	CONNECT_ABORTED // isStarted dropped: Stop() is waiting for this thread
};

// connect() that Stop() can interrupt: a blocking connect() cannot be woken
// portably from another thread (close() does not wake it on Linux, shutdown()
// races with it starting). The socket connects in non-blocking mode while this
// polls isStarted, and is blocking again when it returns CONNECT_SUCCEEDED.
static ConnectResult ConnectAbortable(__TCPSOCKET__ s, const sockaddr *address, int addressLength, const MafiaNet::LocklessUint32_t &isStarted)
{
	if (isStarted.GetValue() == 0)
		return CONNECT_ABORTED;
	if (!SetSocketBlocking(s, false))
		return CONNECT_FAILED;
	if (connect__(s, address, addressLength) != 0)
	{
#ifdef _WIN32
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			return CONNECT_FAILED;
#else
		if (errno != EINPROGRESS && errno != EINTR)
			return CONNECT_FAILED;
#endif
		for (;;)
		{
			if (isStarted.GetValue() == 0)
				return CONNECT_ABORTED;
			fd_set writeSet, exceptSet; // Winsock reports a failed connect in exceptSet
			FD_ZERO(&writeSet);
			FD_ZERO(&exceptSet);
			FD_SET(s, &writeSet);
			FD_SET(s, &exceptSet);
			timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = CONNECT_POLL_MS * 1000;
			const int ready = select__(static_cast<int>(s + 1), nullptr, &writeSet, &exceptSet, &tv);
			if (ready == 0)
				continue;
			if (ready < 0)
			{
#ifndef _WIN32
				if (errno == EINTR)
					continue;
#endif
				return CONNECT_FAILED;
			}
			int error = 0;
			socklen_t errorLength = sizeof(error);
			if (getsockopt__(s, SOL_SOCKET, SO_ERROR, (char *) &error, &errorLength) != 0 || error != 0)
				return CONNECT_FAILED;
			break;
		}
	}
	return SetSocketBlocking(s, true) ? CONNECT_SUCCEEDED : CONNECT_FAILED;
}

// A connection socket must never raise SIGPIPE: a peer can reset it at any time, and another thread may shut it
// down while the update thread still writes to it. send() also passes MSG_NOSIGNAL where it exists, but SO_NOSIGPIPE
// (macOS, the BSDs) covers older SDKs and libcs without it.
static void DisableSigpipe(__TCPSOCKET__ s)
{
#ifdef SO_NOSIGPIPE
	int on = 1;
	setsockopt__(s, SOL_SOCKET, SO_NOSIGPIPE, (char *) &on, sizeof(on));
#else
	(void) s;
#endif
}

// Binds a listening TCP socket of exactly \a family (AF_INET or AF_INET6). An AF_INET6 socket is IPv6 only unless
// \a dualStack, in which case a system that refuses IPV6_V6ONLY=0 yields no socket. Returns 0 on failure.
static __TCPSOCKET__ BindTcpListenSocket(const char *portStr, int family, bool dualStack, const char *bindAddress)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(addrinfo)); // make sure the struct is empty
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;     // TCP sockets
	hints.ai_flags = AI_PASSIVE;         // wildcard address when bindAddress is empty
	struct addrinfo *servinfo = 0, *aip; // will point to the results

	const char *host = (bindAddress && bindAddress[0]) ? bindAddress : nullptr;
	if (getaddrinfo(host, portStr, &hints, &servinfo) != 0 || servinfo == nullptr)
		return 0;

	__TCPSOCKET__ bound = 0;
	for (aip = servinfo; aip != nullptr; aip = aip->ai_next)
	{
		__TCPSOCKET__ candidate = socket__(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
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

bool TCPInterface::CreateListenSocket(unsigned short port, unsigned short maxIncomingConnections, unsigned short socketFamily, const char *bindAddress)
{
	char portStr[32];
	Itoa(port, portStr, 10);

	if (socketFamily == AF_UNSPEC)
	{
		// One AF_INET6 socket with IPV6_V6ONLY=0 serves both families, whatever order the resolver returns the
		// wildcard addresses in. Only when the host cannot do that (no IPv6, an IPv4 bind address, or a system that
		// refuses V6ONLY=0) fall back to a plain IPv4 socket.
		listenSocket = BindTcpListenSocket(portStr, AF_INET6, true, bindAddress);
		if (listenSocket == 0)
			listenSocket = BindTcpListenSocket(portStr, AF_INET, false, bindAddress);
	}
	else
		listenSocket = BindTcpListenSocket(portStr, socketFamily, false, bindAddress);

	if (listenSocket == 0)
		return false;

	SocketLayer::SetSocketOptions(listenSocket, false, false);

	listen__(listenSocket, maxIncomingConnections);

	return true;
}

bool TCPInterface::Start(unsigned short port, unsigned short maxIncomingConnections, unsigned short maxConnections, int _threadPriority, unsigned short socketFamily, const char *bindAddress)
{
	if (isStarted.GetValue() > 0)
		return false;

	threadPriority = _threadPriority;

	if (threadPriority == -99999)
	{


#if defined(_WIN32)
		threadPriority = 0;


#else
		threadPriority = 1000;
#endif
	}

	isStarted.Increment();
	if (maxConnections == 0)
		maxConnections = maxIncomingConnections;
	if (maxConnections == 0)
		maxConnections = 1;
	remoteClientsLength = maxConnections;
	remoteClients = MafiaNet::OP_NEW_ARRAY<RemoteClient>(maxConnections, _FILE_AND_LINE_);


	listenSocket = 0;
	if (maxIncomingConnections > 0)
	{
		if (CreateListenSocket(port, maxIncomingConnections, socketFamily, bindAddress) == false)
		{
			isStarted.Decrement();
			MafiaNet::OP_DELETE_ARRAY(remoteClients, _FILE_AND_LINE_);
			remoteClients = 0;
			remoteClientsLength = 0;
			return false;
		}
	}


	// Start the update thread
	int errorCode;


	errorCode = MafiaNet::RakThread::Create(UpdateTCPInterfaceLoop, this, threadPriority);


	if (errorCode != 0)
		return false;

	while (threadRunning.GetValue() == 0)
		RakSleep(0);

	unsigned int i;
	for (i = 0; i < messageHandlerList.Size(); i++)
		messageHandlerList[i]->OnRakPeerStartup();

	return true;
}
void TCPInterface::Stop(void)
{
	unsigned int i;
	for (i = 0; i < messageHandlerList.Size(); i++)
		messageHandlerList[i]->OnRakPeerShutdown();

	if (isStarted.GetValue() == 0)
		return;


	isStarted.Decrement();

	if (listenSocket != 0)
	{
#ifdef _WIN32
		shutdown__(listenSocket, SD_BOTH);

#else
		shutdown__(listenSocket, SHUT_RDWR);
#endif
		closesocket__(listenSocket);
	}

	// A connect in progress (SocketConnect) sees isStarted drop within
	// CONNECT_POLL_MS and gives up on its own.

	// Wait for the update thread and every ConnectionAttemptLoop to stop: both use remoteClients
	while (threadRunning.GetValue() > 0)
		RakSleep(15);

	RakSleep(100);

	listenSocket = 0;

	// Stuff from here on to the end of the function is not threadsafe
	for (i = 0; i < remoteClientsLength; i++)
	{
		remoteClients[i].isActiveMutex.Lock();
		// 0 marks an unused slot; closing it would close the process's stdin
		// and let the next socket() return descriptor 0, which this class
		// treats as "no socket".
		if (remoteClients[i].socket != 0)
			closesocket__(remoteClients[i].socket);
		remoteClients[i].socket = 0;
		// Connections retired since the update thread's last pass.
		remoteClients[i].CloseRetiredSockets();
		remoteClients[i].isActiveMutex.Unlock();
	}
	remoteClientsLength = 0;
	MafiaNet::OP_DELETE_ARRAY(remoteClients, _FILE_AND_LINE_);
	remoteClients = 0;

	// #low review whether we'd rather use PopInaccurate() here (i.e. check whether related threads accessing the queue terminated already)
	// consider even adding a dtor to Packet which would then clear its data (at this point drop this explicit packet deallocation here)
	MafiaNet::Packet *packet = incomingMessages.Pop();
	while (packet != nullptr)
	{
		DeallocatePacket(packet);
		packet = incomingMessages.Pop();
	}
	incomingMessages.Clear(_FILE_AND_LINE_);
	newIncomingConnections.Clear(_FILE_AND_LINE_);
	newRemoteClients.Clear(_FILE_AND_LINE_);
	lostConnections.Clear(_FILE_AND_LINE_);
	requestedCloseConnections.Clear(_FILE_AND_LINE_);
	failedConnectionAttempts.Clear(_FILE_AND_LINE_);
	completedConnectionAttempts.Clear(_FILE_AND_LINE_);
	failedConnectionAttempts.Clear(_FILE_AND_LINE_);
	for (i = 0; i < headPush.Size(); i++)
		DeallocatePacket(headPush[i]);
	headPush.Clear(_FILE_AND_LINE_);
	for (i = 0; i < tailPush.Size(); i++)
		DeallocatePacket(tailPush[i]);
	tailPush.Clear(_FILE_AND_LINE_);
}
SystemAddress TCPInterface::Connect(const char *host, unsigned short remotePort, bool block, unsigned short socketFamily, const char *bindAddress)
{
	if (threadRunning.GetValue() == 0)
		return UNASSIGNED_SYSTEM_ADDRESS;

	int newRemoteClientIndex = -1;
	for (newRemoteClientIndex = 0; newRemoteClientIndex < remoteClientsLength; newRemoteClientIndex++)
	{
		remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
		if (remoteClients[newRemoteClientIndex].isActive == false)
		{
			remoteClients[newRemoteClientIndex].SetActive(true);
			remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();
			break;
		}
		remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();
	}
	if (newRemoteClientIndex == remoteClientsLength)
		return UNASSIGNED_SYSTEM_ADDRESS; // every slot is in use

	if (block)
	{
		SystemAddress systemAddress;
		// #med - call should be changed to ...FromString(host, '\0') so to not try to extract an optional port (which is overwritten with the removePort directly afterwards)
		// Resolve in the family the socket will use: the first answer of an unrestricted lookup may be one it cannot reach.
		// This is only the address reported if the attempt fails; SocketConnect resolves the host itself.
		const bool parsed = systemAddress.FromString(host, '|', SocketFamilyToIpVersion(socketFamily));
		if (!parsed)
			systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
		systemAddress.SetPortHostOrder(remotePort);
		systemAddress.systemIndex = (SystemIndex) newRemoteClientIndex;

		// The host without the optional "|port" suffix FromString accepted above.
		char hostPart[256];
		char portPart[32];
		SystemAddress peerAddress;
		__TCPSOCKET__ sockfd = 0;
		if (SplitHostAndPort(host, '|', hostPart, sizeof(hostPart), portPart, sizeof(portPart)))
			sockfd = SocketConnect(ConnectHost(hostPart, parsed ? &systemAddress : nullptr).c_str(), remotePort, socketFamily, bindAddress, &peerAddress);
		if (sockfd == 0)
		{
			remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
			remoteClients[newRemoteClientIndex].SetActive(false);
			remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();

			failedConnectionAttemptMutex.Lock();
			failedConnectionAttempts.Push(systemAddress, _FILE_AND_LINE_);
			failedConnectionAttemptMutex.Unlock();

			return UNASSIGNED_SYSTEM_ADDRESS;
		}

		if (peerAddress != UNASSIGNED_SYSTEM_ADDRESS)
		{
			peerAddress.systemIndex = systemAddress.systemIndex;
			systemAddress = peerAddress;
		}
		// The update thread reads the slot under isActiveMutex as soon as it is active.
		remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
		remoteClients[newRemoteClientIndex].socket = sockfd;
		remoteClients[newRemoteClientIndex].systemAddress = systemAddress;
		remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();

		completedConnectionAttemptMutex.Lock();
		completedConnectionAttempts.Push(systemAddress, _FILE_AND_LINE_);
		completedConnectionAttemptMutex.Unlock();

		return systemAddress;
	}
	else
	{
		ThisPtrPlusSysAddr *s = MafiaNet::OP_NEW<ThisPtrPlusSysAddr>(_FILE_AND_LINE_);
		const bool parsed = s->systemAddress.FromStringExplicitPort(host, remotePort, SocketFamilyToIpVersion(socketFamily));
		s->host = ConnectHost(host ? host : "", parsed ? &s->systemAddress : nullptr);
		s->systemAddress.SetPortHostOrder(remotePort); // also when parsing failed: the thread connects to this port
		s->systemAddress.systemIndex = (SystemIndex) newRemoteClientIndex;
		if (bindAddress)
			strcpy_s(s->bindAddress, bindAddress);
		else
			s->bindAddress[0] = 0;
		s->tcpInterface = this;
		s->socketFamily = socketFamily;

		// Start the connection thread. It is counted in threadRunning before it
		// starts, so Stop() waits for it before freeing the slots it writes.
		int errorCode;

		threadRunning.Increment();
		errorCode = MafiaNet::RakThread::Create(ConnectionAttemptLoop, s, threadPriority);

		if (errorCode != 0)
		{
			threadRunning.Decrement();
			const SystemAddress failed = s->systemAddress;
			MafiaNet::OP_DELETE(s, _FILE_AND_LINE_);
			remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
			remoteClients[newRemoteClientIndex].SetActive(false);
			remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();
			failedConnectionAttemptMutex.Lock();
			failedConnectionAttempts.Push(failed, _FILE_AND_LINE_);
			failedConnectionAttemptMutex.Unlock();
		}
		return UNASSIGNED_SYSTEM_ADDRESS;
	}
}
void TCPInterface::Send(const char *data, unsigned length, const SystemAddress &systemAddress, bool broadcast)
{
	SendList(&data, &length, 1, systemAddress, broadcast);
}
bool TCPInterface::SendList(const char **data, const unsigned int *lengths, const int numParameters, const SystemAddress &systemAddress, bool broadcast)
{
	if (isStarted.GetValue() == 0)
		return false;
	if (data == 0)
		return false;
	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS && broadcast == false)
		return false;
	unsigned int totalLength = 0;
	int i;
	for (i = 0; i < numParameters; i++)
	{
		if (lengths[i] > 0)
			totalLength += lengths[i];
	}
	if (totalLength == 0)
		return false;

	// Slots are matched and filled under their lock: the update thread and
	// Connect retire and reuse them concurrently.
	if (broadcast)
	{
		// Send to all, possible exception system
		for (i = 0; i < remoteClientsLength; i++)
		{
			remoteClients[i].isActiveMutex.Lock();
			if (remoteClients[i].systemAddress != systemAddress)
			{
				remoteClients[i].SendOrBuffer(data, lengths, numParameters);
			}
			remoteClients[i].isActiveMutex.Unlock();
		}
	}
	else
	{
		// Send to this player
		bool sent = false;
		if (systemAddress.systemIndex < remoteClientsLength)
		{
			RemoteClient &indexed = remoteClients[systemAddress.systemIndex];
			indexed.isActiveMutex.Lock();
			if (indexed.isActive && indexed.systemAddress == systemAddress)
			{
				indexed.SendOrBuffer(data, lengths, numParameters);
				sent = true;
			}
			indexed.isActiveMutex.Unlock();
		}
		if (!sent)
		{
			for (i = 0; i < remoteClientsLength; i++)
			{
				remoteClients[i].isActiveMutex.Lock();
				if (remoteClients[i].systemAddress == systemAddress)
				{
					remoteClients[i].SendOrBuffer(data, lengths, numParameters);
				}
				remoteClients[i].isActiveMutex.Unlock();
			}
		}
	}


	return true;
}
bool TCPInterface::ReceiveHasPackets(void)
{
	return headPush.IsEmpty() == false || incomingMessages.IsEmpty() == false || tailPush.IsEmpty() == false;
}
Packet *TCPInterface::Receive(void)
{
	unsigned int i;
	for (i = 0; i < messageHandlerList.Size(); i++)
		messageHandlerList[i]->Update();

	Packet *outgoingPacket = ReceiveInt();

	if (outgoingPacket)
	{
		PluginReceiveResult pluginResult;
		for (i = 0; i < messageHandlerList.Size(); i++)
		{
			pluginResult = messageHandlerList[i]->OnReceive(outgoingPacket);
			if (pluginResult == RR_STOP_PROCESSING_AND_DEALLOCATE)
			{
				DeallocatePacket(outgoingPacket);
				outgoingPacket = 0; // Will do the loop again and get another packet
				break;              // break out of the enclosing for
			}
			else if (pluginResult == RR_STOP_PROCESSING)
			{
				outgoingPacket = 0;
				break;
			}
		}
	}


	return outgoingPacket;
}
Packet *TCPInterface::ReceiveInt(void)
{
	if (isStarted.GetValue() == 0)
		return 0;
	if (headPush.IsEmpty() == false)
		return headPush.Pop();
	Packet *p = incomingMessages.PopInaccurate();
	if (p)
		return p;
	if (tailPush.IsEmpty() == false)
		return tailPush.Pop();
	return 0;
}


void TCPInterface::AttachPlugin(PluginInterface2 *plugin)
{
	if (messageHandlerList.GetIndexOf(plugin) == MAX_UNSIGNED_LONG)
	{
		messageHandlerList.Insert(plugin, _FILE_AND_LINE_);
		plugin->SetTCPInterface(this);
		plugin->OnAttach();
	}
}
void TCPInterface::DetachPlugin(PluginInterface2 *plugin)
{
	if (plugin == 0)
		return;

	unsigned int index;
	index = messageHandlerList.GetIndexOf(plugin);
	if (index != MAX_UNSIGNED_LONG)
	{
		messageHandlerList[index]->OnDetach();
		// Unordered list so delete from end for speed
		messageHandlerList[index] = messageHandlerList[messageHandlerList.Size() - 1];
		messageHandlerList.RemoveFromEnd();
		plugin->SetTCPInterface(0);
	}
}
void TCPInterface::CloseConnection(SystemAddress systemAddress)
{
	if (isStarted.GetValue() == 0)
		return;
	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
		return;

	unsigned int i;
	for (i = 0; i < messageHandlerList.Size(); i++)
		messageHandlerList[i]->OnClosedConnection(systemAddress, UNASSIGNED_RAKNET_GUID, LCR_CLOSED_BY_USER);

	// Match the slot under its lock: it may have been retired and reused for
	// another connection since the caller got the address.
	bool closed = false;
	if (systemAddress.systemIndex < remoteClientsLength)
	{
		RemoteClient &indexed = remoteClients[systemAddress.systemIndex];
		indexed.isActiveMutex.Lock();
		if (indexed.isActive && indexed.systemAddress == systemAddress)
		{
			indexed.SetActive(false);
			closed = true;
		}
		indexed.isActiveMutex.Unlock();
	}
	if (!closed)
	{
		for (i = 0; i < remoteClientsLength; i++)
		{
			remoteClients[i].isActiveMutex.Lock();
			if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
			{
				remoteClients[i].SetActive(false);
				remoteClients[i].isActiveMutex.Unlock();
				break;
			}
			remoteClients[i].isActiveMutex.Unlock();
		}
	}
}
void TCPInterface::DeallocatePacket(Packet *packet)
{
	if (packet == 0)
		return;
	if (packet->deleteData)
	{
		rakFree_Ex(packet->data, _FILE_AND_LINE_);
		incomingMessages.Deallocate(packet, _FILE_AND_LINE_);
	}
	else
	{
		// Came from userspace AllocatePacket
		rakFree_Ex(packet->data, _FILE_AND_LINE_);
		MafiaNet::OP_DELETE(packet, _FILE_AND_LINE_);
	}
}
Packet *TCPInterface::AllocatePacket(unsigned dataSize)
{
	Packet *p = MafiaNet::OP_NEW<Packet>(_FILE_AND_LINE_);
	p->data = (unsigned char *) rakMalloc_Ex(dataSize, _FILE_AND_LINE_);
	p->length = dataSize;
	p->bitSize = BYTES_TO_BITS(dataSize);
	p->deleteData = false;
	p->guid = UNASSIGNED_RAKNET_GUID;
	p->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
	p->systemAddress.systemIndex = (SystemIndex) -1;
	return p;
}
void TCPInterface::PushBackPacket(Packet *packet, bool pushAtHead)
{
	if (pushAtHead)
		headPush.Push(packet, _FILE_AND_LINE_);
	else
		tailPush.Push(packet, _FILE_AND_LINE_);
}
bool TCPInterface::WasStarted(void) const
{
	return threadRunning.GetValue() > 0;
}
SystemAddress TCPInterface::HasCompletedConnectionAttempt(void)
{
	SystemAddress sysAddr = UNASSIGNED_SYSTEM_ADDRESS;
	completedConnectionAttemptMutex.Lock();
	if (completedConnectionAttempts.IsEmpty() == false)
		sysAddr = completedConnectionAttempts.Pop();
	completedConnectionAttemptMutex.Unlock();

	if (sysAddr != UNASSIGNED_SYSTEM_ADDRESS)
	{
		unsigned int i;
		for (i = 0; i < messageHandlerList.Size(); i++)
			messageHandlerList[i]->OnNewConnection(sysAddr, UNASSIGNED_RAKNET_GUID, true);
	}

	return sysAddr;
}
SystemAddress TCPInterface::HasFailedConnectionAttempt(void)
{
	SystemAddress sysAddr = UNASSIGNED_SYSTEM_ADDRESS;
	failedConnectionAttemptMutex.Lock();
	if (failedConnectionAttempts.IsEmpty() == false)
		sysAddr = failedConnectionAttempts.Pop();
	failedConnectionAttemptMutex.Unlock();

	if (sysAddr != UNASSIGNED_SYSTEM_ADDRESS)
	{
		unsigned int i;
		for (i = 0; i < messageHandlerList.Size(); i++)
		{
			Packet p;
			p.systemAddress = sysAddr;
			p.data = 0;
			p.length = 0;
			p.bitSize = 0;
			messageHandlerList[i]->OnFailedConnectionAttempt(&p, FCAR_CONNECTION_ATTEMPT_FAILED);
		}
	}

	return sysAddr;
}
SystemAddress TCPInterface::HasNewIncomingConnection(void)
{
	SystemAddress *out, out2;
	out = newIncomingConnections.PopInaccurate();
	if (out)
	{
		out2 = *out;
		newIncomingConnections.Deallocate(out, _FILE_AND_LINE_);

		unsigned int i;
		for (i = 0; i < messageHandlerList.Size(); i++)
			messageHandlerList[i]->OnNewConnection(out2, UNASSIGNED_RAKNET_GUID, true);

		return out2; // out was just returned to the pool
	}
	else
	{
		return UNASSIGNED_SYSTEM_ADDRESS;
	}
}
SystemAddress TCPInterface::HasLostConnection(void)
{
	SystemAddress *out, out2;
	out = lostConnections.PopInaccurate();
	if (out)
	{
		out2 = *out;
		lostConnections.Deallocate(out, _FILE_AND_LINE_);

		unsigned int i;
		for (i = 0; i < messageHandlerList.Size(); i++)
			messageHandlerList[i]->OnClosedConnection(out2, UNASSIGNED_RAKNET_GUID, LCR_DISCONNECTION_NOTIFICATION);

		return out2; // out was just returned to the pool
	}
	else
	{
		return UNASSIGNED_SYSTEM_ADDRESS;
	}
}
void TCPInterface::GetConnectionList(SystemAddress *remoteSystems, unsigned short *numberOfSystems) const
{
	unsigned short systemCount = 0;
	unsigned short maxToWrite = *numberOfSystems;
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive)
		{
			if (systemCount < maxToWrite)
				remoteSystems[systemCount] = remoteClients[i].systemAddress;
			systemCount++;
		}
	}
	*numberOfSystems = systemCount;
}
unsigned short TCPInterface::GetConnectionCount(void) const
{
	unsigned short systemCount = 0;
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive)
			systemCount++;
	}
	return systemCount;
}

unsigned int TCPInterface::GetOutgoingDataBufferSize(SystemAddress systemAddress) const
{
	unsigned bytesWritten = 0;
	if (systemAddress.systemIndex < remoteClientsLength &&
	    remoteClients[systemAddress.systemIndex].isActive &&
	    remoteClients[systemAddress.systemIndex].systemAddress == systemAddress)
	{
		remoteClients[systemAddress.systemIndex].outgoingDataMutex.Lock();
		bytesWritten = remoteClients[systemAddress.systemIndex].outgoingData.GetBytesWritten();
		remoteClients[systemAddress.systemIndex].outgoingDataMutex.Unlock();
		return bytesWritten;
	}

	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
		{
			remoteClients[i].outgoingDataMutex.Lock();
			bytesWritten += remoteClients[i].outgoingData.GetBytesWritten();
			remoteClients[i].outgoingDataMutex.Unlock();
		}
	}
	return bytesWritten;
}
__TCPSOCKET__ TCPInterface::SocketConnect(const char *host, unsigned short remotePort, unsigned short socketFamily, const char *bindAddress, SystemAddress *peerAddress)
{
	if (peerAddress)
		*peerAddress = UNASSIGNED_SYSTEM_ADDRESS;

	struct addrinfo hints, *res = nullptr;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = socketFamily;
	hints.ai_socktype = SOCK_STREAM;
	char portStr[32];
	Itoa(remotePort, portStr, 10);
	if (getaddrinfo(host, portStr, &hints, &res) != 0 || res == nullptr)
		return 0;

	// Try every answer in order until one connects: a name may resolve to an address this host cannot reach.
	__TCPSOCKET__ connected = 0;
	for (struct addrinfo *aip = res; aip != nullptr && connected == 0; aip = aip->ai_next)
	{
		__TCPSOCKET__ sockfd = socket__(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
		if (sockfd == INVALID_SOCKET)
			continue;
		DisableSigpipe(sockfd);

		if (aip->ai_family == AF_INET6)
		{
			// Always explicit, never the OS default. The peer is a single remote
			// address, so IPv6 only loses nothing.
			int v6only = 1;
			setsockopt__(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &v6only, sizeof(v6only));
		}

		// Optional local address to connect from, in the family of the remote one
		if (bindAddress && bindAddress[0])
		{
			struct addrinfo bindHints, *bindRes = nullptr;
			memset(&bindHints, 0, sizeof bindHints);
			bindHints.ai_family = aip->ai_family;
			bindHints.ai_socktype = SOCK_STREAM;
			bindHints.ai_flags = AI_PASSIVE;
			const bool bound = getaddrinfo(bindAddress, "0", &bindHints, &bindRes) == 0 && bindRes != nullptr &&
			                   bind__(sockfd, bindRes->ai_addr, (int) bindRes->ai_addrlen) >= 0;
			if (bindRes)
				freeaddrinfo(bindRes);
			if (!bound)
			{
				closesocket__(sockfd);
				continue;
			}
		}

		int sock_opt = 1024 * 256;
		setsockopt__(sockfd, SOL_SOCKET, SO_RCVBUF, (char *) &sock_opt, sizeof(sock_opt));

		// Abortable: Stop() lowering isStarted ends the attempt within CONNECT_POLL_MS,
		// rather than waiting out the OS connect timeout (minutes for a silent host).
		const ConnectResult result = ConnectAbortable(sockfd, aip->ai_addr, static_cast<int>(aip->ai_addrlen), isStarted);
		if (result != CONNECT_SUCCEEDED)
		{
			closesocket__(sockfd);
			if (result == CONNECT_ABORTED)
				break;
			continue;
		}
		connected = sockfd;
	}
	freeaddrinfo(res); // free the linked-list

	if (connected != 0 && peerAddress)
	{
		sockaddr_storage peer;
		memset(&peer, 0, sizeof(peer));
		socklen_t peerLength = sizeof(peer);
		if (getpeername(connected, (sockaddr *) &peer, &peerLength) != 0 || !SockaddrToSystemAddress(peer, peerAddress))
			*peerAddress = UNASSIGNED_SYSTEM_ADDRESS;
	}

	return connected;
}

RAK_THREAD_DECLARATION(MafiaNet::ConnectionAttemptLoop)
{


	TCPInterface::ThisPtrPlusSysAddr *s = (TCPInterface::ThisPtrPlusSysAddr *) arguments;


	SystemAddress systemAddress = s->systemAddress;
	TCPInterface *tcpInterface = s->tcpInterface;
	int newRemoteClientIndex = systemAddress.systemIndex;
	unsigned short socketFamily = s->socketFamily;
	const std::string host = s->host;
	char bindAddress[sizeof(s->bindAddress)];
	memcpy(bindAddress, s->bindAddress, sizeof(bindAddress));
	MafiaNet::OP_DELETE(s, _FILE_AND_LINE_);

	// Connect() counted this thread in threadRunning; uncount it on every return.
	struct ThreadRunningGuard
	{
		TCPInterface *tcp;
		~ThreadRunningGuard() { tcp->threadRunning.Decrement(); }
	} threadRunningGuard{tcpInterface};

	SystemAddress peerAddress;
	__TCPSOCKET__ sockfd = tcpInterface->SocketConnect(host.c_str(), systemAddress.GetPort(), socketFamily, bindAddress, &peerAddress);
	if (sockfd == 0)
	{
		tcpInterface->remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
		tcpInterface->remoteClients[newRemoteClientIndex].SetActive(false);
		tcpInterface->remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();

		tcpInterface->failedConnectionAttemptMutex.Lock();
		tcpInterface->failedConnectionAttempts.Push(systemAddress, _FILE_AND_LINE_);
		tcpInterface->failedConnectionAttemptMutex.Unlock();
		return 0;
	}

	if (peerAddress != UNASSIGNED_SYSTEM_ADDRESS)
	{
		peerAddress.systemIndex = systemAddress.systemIndex;
		systemAddress = peerAddress;
	}
	// The update thread reads the slot under isActiveMutex as soon as it is active.
	tcpInterface->remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
	tcpInterface->remoteClients[newRemoteClientIndex].socket = sockfd;
	tcpInterface->remoteClients[newRemoteClientIndex].systemAddress = systemAddress;
	tcpInterface->remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();

	// Notify user that the connection attempt has completed, unless Stop() is
	// under way (it closes the slot's socket once this thread has returned).
	if (tcpInterface->isStarted.GetValue() > 0)
	{
		tcpInterface->completedConnectionAttemptMutex.Lock();
		tcpInterface->completedConnectionAttempts.Push(systemAddress, _FILE_AND_LINE_);
		tcpInterface->completedConnectionAttemptMutex.Unlock();
	}


	return 0;
}

RAK_THREAD_DECLARATION(MafiaNet::UpdateTCPInterfaceLoop)
{


	TCPInterface *sts = (TCPInterface *) arguments;


	//	const int BUFF_SIZE=8096;
	const unsigned int BUFF_SIZE = 1048576;
	//char data[ BUFF_SIZE ];
	char *data = (char *) rakMalloc_Ex(BUFF_SIZE, _FILE_AND_LINE_);
	Packet *incomingMessage;
	fd_set readFD, exceptionFD, writeFD;
	sts->threadRunning.Increment();

	struct sockaddr_storage sockAddr;
	socklen_t sockAddrSize = sizeof(sockAddr);

	int len;
	__TCPSOCKET__ newSock;
	int selectResult;


	timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 30000;

	// Retires slot \a index once its connection \a lostSocket is gone, and reports it -- unless another thread
	// retired that connection after this thread's snapshot (recv on a shut-down socket returns 0 too): then the
	// slot is inactive, or Connect already reused it for a new connection, which must be left alone.
	auto retireLostConnection = [sts](unsigned index, __TCPSOCKET__ lostSocket, const SystemAddress &lostAddress)
	{
		RemoteClient &rc = sts->remoteClients[index];
		rc.isActiveMutex.Lock();
		const bool current = rc.isActive && rc.socket == lostSocket;
		if (current)
			rc.SetActive(false);
		rc.isActiveMutex.Unlock();
		if (current)
		{
			SystemAddress *lostConnectionSystemAddress = sts->lostConnections.Allocate(_FILE_AND_LINE_);
			*lostConnectionSystemAddress = lostAddress;
			sts->lostConnections.Push(lostConnectionSystemAddress);
		}
	};


	while (sts->isStarted.GetValue() > 0)
	{


		__TCPSOCKET__ largestDescriptor = 0; // see select__()'s first parameter's documentation under linux


		// Linux' select__() implementation changes the timeout

		tv.tv_sec = 0;
		tv.tv_usec = 30000;


#ifdef _MSC_VER
#pragma warning(disable : 4127) // warning C4127: conditional expression is constant
#endif
		for (;;)
		{
			// Reset readFD, writeFD, and exceptionFD since select seems to clear it
			FD_ZERO(&readFD);
			FD_ZERO(&exceptionFD);
			FD_ZERO(&writeFD);
			largestDescriptor = 0;
			if (sts->listenSocket != 0)
			{
				FD_SET(sts->listenSocket, &readFD);
				FD_SET(sts->listenSocket, &exceptionFD);
				largestDescriptor = sts->listenSocket; // @see largestDescriptor def
			}

			unsigned i;
			for (i = 0; i < sts->remoteClientsLength; i++)
			{
				sts->remoteClients[i].isActiveMutex.Lock();
				// Close what was retired since the last pass. Nothing else closes a
				// socket, so every descriptor put in the sets below stays open --
				// its number cannot be handed to a new connection -- until this
				// thread comes back here.
				sts->remoteClients[i].CloseRetiredSockets();
				if (sts->remoteClients[i].isActive)
				{
					// calling FD_ISSET with -1 as socket (that’s what 0 is set to) produces a bus error under Linux 64-Bit
					__TCPSOCKET__ socketCopy = sts->remoteClients[i].socket;
					if (socketCopy != 0)
					{
						FD_SET(socketCopy, &readFD);
						FD_SET(socketCopy, &exceptionFD);
						sts->remoteClients[i].outgoingDataMutex.Lock();
						const bool hasOutgoingData = sts->remoteClients[i].outgoingData.GetBytesWritten() > 0;
						sts->remoteClients[i].outgoingDataMutex.Unlock();
						if (hasOutgoingData)
							FD_SET(socketCopy, &writeFD);
						if (socketCopy > largestDescriptor) // @see largestDescriptorDef
							largestDescriptor = socketCopy;
					}
				}
				sts->remoteClients[i].isActiveMutex.Unlock();
			}

#ifdef _MSC_VER
#pragma warning(disable : 4244) // warning C4127: conditional expression is constant
#endif


			selectResult = (int) select__(largestDescriptor + 1, &readFD, &writeFD, &exceptionFD, &tv);


			if (selectResult <= 0)
				break;

			if (sts->listenSocket != 0 && FD_ISSET(sts->listenSocket, &readFD))
			{
				sockAddrSize = sizeof(sockAddr); // in/out parameter, reset for every accept
				newSock = accept__(sts->listenSocket, (sockaddr *) &sockAddr, (socklen_t *) &sockAddrSize);

				if (newSock != 0 && newSock != INVALID_SOCKET)
				{
					DisableSigpipe(newSock);
					int newRemoteClientIndex = -1;
					for (newRemoteClientIndex = 0; newRemoteClientIndex < sts->remoteClientsLength; newRemoteClientIndex++)
					{
						sts->remoteClients[newRemoteClientIndex].isActiveMutex.Lock();
						if (sts->remoteClients[newRemoteClientIndex].isActive == false)
						{
							sts->remoteClients[newRemoteClientIndex].socket = newSock;

							// A dual-stack listener reports IPv4 clients as mapped IPv6 addresses: store them canonically
							SystemAddress acceptedAddress;
							SockaddrToSystemAddress(sockAddr, &acceptedAddress);
							acceptedAddress.systemIndex = (SystemIndex) newRemoteClientIndex;
							sts->remoteClients[newRemoteClientIndex].systemAddress = acceptedAddress;
							sts->remoteClients[newRemoteClientIndex].SetActive(true);
							sts->remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();


							SystemAddress *newConnectionSystemAddress = sts->newIncomingConnections.Allocate(_FILE_AND_LINE_);
							*newConnectionSystemAddress = sts->remoteClients[newRemoteClientIndex].systemAddress;
							sts->newIncomingConnections.Push(newConnectionSystemAddress);

							break;
						}
						sts->remoteClients[newRemoteClientIndex].isActiveMutex.Unlock();
					}
					if (newRemoteClientIndex == sts->remoteClientsLength)
					{
						// No free slot: refuse this connection, keep listening
						closesocket__(newSock);
					}
				}
				else
				{
#ifdef _DO_PRINTF
					RAKNET_DEBUG_PRINTF("Error: connection failed\n");
#endif
				}
			}
			else if (sts->listenSocket != 0 && FD_ISSET(sts->listenSocket, &exceptionFD))
			{
#ifdef _DO_PRINTF
				int err;
				int errlen = sizeof(err);
				getsockopt__(sts->listenSocket, SOL_SOCKET, SO_ERROR, (char *) &err, &errlen);
				RAKNET_DEBUG_PRINTF("Socket error %s on listening socket\n", err);
#endif
			}

			{
				i = 0;
				while (i < sts->remoteClientsLength)
				{
					// Snapshot the slot under its lock, as the first loop does: Connect and CloseConnection publish
					// and retire slots from other threads. The snapshot socket stays open for this whole pass (only
					// this thread closes sockets, before the next select), so recv/send below never reach a closed or
					// reused descriptor; a slot that was retired and reused meanwhile holds a socket that is not in
					// the sets.
					sts->remoteClients[i].isActiveMutex.Lock();
					const bool slotActive = sts->remoteClients[i].isActive;
					// calling FD_ISSET with -1 as socket (that’s what 0 is set to) produces a bus error under Linux 64-Bit
					__TCPSOCKET__ socketCopy = sts->remoteClients[i].socket;
					const SystemAddress slotAddress = sts->remoteClients[i].systemAddress;
					sts->remoteClients[i].isActiveMutex.Unlock();
					if (slotActive == false || socketCopy == 0)
					{
						i++;
						continue;
					}

					if (FD_ISSET(socketCopy, &exceptionFD))
					{
						// #ifdef _DO_PRINTF
						// 						if (sts->listenSocket!=-1)
						// 						{
						// 							int err;
						// 							int errlen = sizeof(err);
						// 							getsockopt__(sts->listenSocket, SOL_SOCKET, SO_ERROR,(char*)&err, &errlen);
						// 							in_addr in;
						// 							in.s_addr = sts->remoteClients[i].systemAddress.binaryAddress;
						//							char ip[65];
						//							inet_ntop(sts->remoteClients[i].systemAddress.address.addr4.sin_family, &in, ip, 65);
						// 							RAKNET_DEBUG_PRINTF("Socket error %i on %s:%i\n", err,ip, sts->remoteClients[i].systemAddress.GetPort() );
						// 						}
						//
						// #endif
						// Connection lost abruptly
						retireLostConnection(i, socketCopy, slotAddress);
					}
					else
					{
						if (FD_ISSET(socketCopy, &readFD))
						{
							// if recv returns 0 this was a graceful close
							len = sts->remoteClients[i].Recv(socketCopy, data, BUFF_SIZE);


							// removeme
							// 								data[len]=0;
							// 								printf(data);

							if (len > 0)
							{
								incomingMessage = sts->incomingMessages.Allocate(_FILE_AND_LINE_);
								incomingMessage->data = (unsigned char *) rakMalloc_Ex(len + 1, _FILE_AND_LINE_);
								memcpy(incomingMessage->data, data, len);
								incomingMessage->data[len] = 0; // Null terminate this so we can print it out as regular strings.  This is different from RakNet which does not do this.
								// printf("RECV: %s\n",incomingMessage->data);
								/*
								if (1)
								{
									static FILE *fp=0;
									if (fp==0)
									{
										fopen_s(&fp, "tcpRcv.txt", "wb");
									}
									fwrite(data,1,len,fp);
								}
								*/
								incomingMessage->length = len;
								incomingMessage->deleteData = true; // actually means came from SPSC, rather than AllocatePacket
								incomingMessage->systemAddress = slotAddress;
								sts->incomingMessages.Push(incomingMessage);
							}
							else
							{
								// Connection lost gracefully
								retireLostConnection(i, socketCopy, slotAddress);
								continue;
							}
						}
						if (FD_ISSET(socketCopy, &writeFD))
						{
							RemoteClient *rc = &sts->remoteClients[i];
							unsigned int bytesInBuffer;
							int bytesAvailable;
							int bytesSent;
							rc->outgoingDataMutex.Lock();
							bytesInBuffer = rc->outgoingData.GetBytesWritten();
							if (bytesInBuffer > 0)
							{
								unsigned int contiguousLength;
								char *contiguousBytesPointer = rc->outgoingData.PeekContiguousBytes(&contiguousLength);
								if (contiguousLength < (unsigned int) BUFF_SIZE && contiguousLength < bytesInBuffer)
								{
									if (bytesInBuffer > BUFF_SIZE)
										bytesAvailable = BUFF_SIZE;
									else
										bytesAvailable = bytesInBuffer;
									rc->outgoingData.ReadBytes(data, bytesAvailable, true);
									bytesSent = rc->Send(socketCopy, data, bytesAvailable);
								}
								else
								{
									bytesSent = rc->Send(socketCopy, contiguousBytesPointer, contiguousLength);
								}

								if (bytesSent > 0)
									rc->outgoingData.IncrementReadOffset(bytesSent);
								bytesInBuffer = rc->outgoingData.GetBytesWritten();
							}
							rc->outgoingDataMutex.Unlock();
						}

						i++; // Nothing deleted so increment the index
					}
				}
			}
		}

		// Sleep 0 on Linux monopolizes the CPU
		RakSleep(30);
	}
	sts->threadRunning.Decrement();

	rakFree_Ex(data, _FILE_AND_LINE_);


	return 0;
}

// A connection retired by another thread is shut down while this thread may
// still be sending on it, and a peer can reset one at any time: either must be
// an error from send, not a SIGPIPE that kills the process.
#ifdef MSG_NOSIGNAL
static const int kSendFlags = MSG_NOSIGNAL;
#else
static const int kSendFlags = 0;
#endif

void RemoteClient::SetActive(bool a)
{
	if (isActive != a)
	{
		isActive = a;
		Reset();
		if (isActive == false && socket != 0)
		{
			// Not closed here: the update thread may be using it right now, and a
			// closed descriptor's number can be handed straight to a new socket.
			// shutdown ends the connection at once (the peer sees it now, the
			// update thread's recv/send fail) but keeps the descriptor reserved
			// until the update thread closes it.
#ifdef _WIN32
			shutdown__(socket, SD_BOTH);
#else
			shutdown__(socket, SHUT_RDWR);
#endif
			socketsToClose.push_back(socket);
			socket = 0;
		}
	}
}
void RemoteClient::CloseRetiredSockets(void)
{
	for (__TCPSOCKET__ retired : socketsToClose)
		closesocket__(retired);
	socketsToClose.clear();
}
void RemoteClient::SendOrBuffer(const char **data, const unsigned int *lengths, const int numParameters)
{
	// True can save memory and buffer copies, but gives worse performance overall
	// Keep false unless sends from the user thread are known to be safe on the target
	const bool ALLOW_SEND_FROM_USER_THREAD = false;

	int parameterIndex;
	if (isActive == false)
		return;
	parameterIndex = 0;
	for (; parameterIndex < numParameters; parameterIndex++)
	{
		outgoingDataMutex.Lock();
		if (ALLOW_SEND_FROM_USER_THREAD && outgoingData.GetBytesWritten() == 0)
		{
			outgoingDataMutex.Unlock();
			int bytesSent = Send(socket, data[parameterIndex], lengths[parameterIndex]);
			if (bytesSent < (int) lengths[parameterIndex])
			{
				// Push remainder
				outgoingDataMutex.Lock();
				outgoingData.WriteBytes(data[parameterIndex] + bytesSent, lengths[parameterIndex] - bytesSent, _FILE_AND_LINE_);
				outgoingDataMutex.Unlock();
			}
		}
		else
		{
			outgoingData.WriteBytes(data[parameterIndex], lengths[parameterIndex], _FILE_AND_LINE_);
			outgoingDataMutex.Unlock();
		}
	}
}
int RemoteClient::Send(__TCPSOCKET__ s, const char *data, unsigned int length)
{
	return send__(s, data, length, kSendFlags);
}
int RemoteClient::Recv(__TCPSOCKET__ s, char *data, const int dataSize)
{
	return recv__(s, data, dataSize, 0);
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // _RAKNET_SUPPORT_*

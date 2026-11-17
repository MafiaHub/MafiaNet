/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief A simple TCP based server allowing sends and receives.  Can be connected by any TCP client, including telnet.
///


#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_TCPInterface == 1

#ifndef __SIMPLE_TCP_SERVER
#define __SIMPLE_TCP_SERVER

#include "mafianet/types.h"
#include "mafianet/export.h"
#include "mafianet/thread.h"
#include "mafianet/simple_mutex.h"
#include "mafianet/defines.h"
#include "mafianet/socket_includes.h"
#include "mafianet/locked_queue.h"
#include "mafianet/lockless_types.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/remote_procedure_call.h"

#include <cstddef>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace MafiaNet
{
/// Forward declarations
struct RemoteClient;

/// \internal
/// \brief As the name says, a simple multithreaded TCP server.  Used by TelnetTransport
class RAK_DLL_EXPORT TCPInterface
{
public:
	// GetInstance() and DestroyInstance(instance*)
	STATIC_FACTORY_DECLARATIONS(TCPInterface)

	TCPInterface();
	/// Enable named RPC messages on this endpoint. Use PacketizedTCP for message framing.
	RPC &GetRPC();
	virtual ~TCPInterface();
	// Owns threads, sockets and LockedQueues; MSVC dllexport needs the deleted copy spelled out.
	TCPInterface(const TCPInterface &) = delete;
	TCPInterface &operator=(const TCPInterface &) = delete;

	// TODO - add socketdescriptor
	/// Starts the TCP server on the indicated port
	/// \param[in] port Which port to listen on.
	/// \param[in] maxIncomingConnections Max incoming connections we will accept
	/// \param[in] maxConnections Max total connections, which should be >= maxIncomingConnections
	/// \param[in] threadPriority Passed to the thread creation routine. Use THREAD_PRIORITY_NORMAL for Windows. For Linux based systems, you MUST pass something reasonable based on the thread priorities for your application.
	/// \param[in] socketFamily IP version: For IPV4, use AF_INET (default). For IPV6, use AF_INET6. To autoselect, use AF_UNSPEC.
	bool Start(unsigned short port, unsigned short maxIncomingConnections, unsigned short maxConnections = 0, int _threadPriority = -99999, unsigned short socketFamily = AF_INET, const char *bindAddress = 0);

	/// Stops the TCP server
	void Stop(void);

	/// Connect to the specified host on the specified port
	SystemAddress Connect(const char *host, unsigned short remotePort, bool block = true, unsigned short socketFamily = AF_INET, const char *bindAddress = 0);

	/// Sends a byte stream
	virtual void Send(const char *data, unsigned int length, const SystemAddress &systemAddress, bool broadcast);

	// Sends a concatenated list of byte streams
	virtual bool SendList(const char **data, const unsigned int *lengths, const int numParameters, const SystemAddress &systemAddress, bool broadcast);

	// Get how many bytes are waiting to be sent. If too many, you may want to skip sending
	unsigned int GetOutgoingDataBufferSize(SystemAddress systemAddress) const;

	/// Returns if Receive() will return data
	/// Do not use on PacketizedTCP
	virtual bool ReceiveHasPackets(void);

	/// Returns data received
	virtual Packet *Receive(void);

	/// Disconnects a player/address
	void CloseConnection(SystemAddress systemAddress);

	/// Deallocates a packet returned by Receive
	void DeallocatePacket(Packet *packet);

	/// Fills the array remoteSystems with the SystemAddress of all the systems we are connected to
	/// \param[out] remoteSystems An array of SystemAddress structures to be filled with the SystemAddresss of the systems we are connected to. Pass 0 to remoteSystems to only get the number of systems we are connected to
	/// \param[in, out] numberOfSystems As input, the size of remoteSystems array.  As output, the number of elements put into the array
	void GetConnectionList(SystemAddress *remoteSystems, unsigned short *numberOfSystems) const;

	/// Returns just the number of connections we have
	unsigned short GetConnectionCount(void) const;

	/// Has a previous call to connect succeeded?
	/// \return UNASSIGNED_SYSTEM_ADDRESS = no. Anything else means yes.
	SystemAddress HasCompletedConnectionAttempt(void);

	/// Has a previous call to connect failed?
	/// \return UNASSIGNED_SYSTEM_ADDRESS = no. Anything else means yes.
	SystemAddress HasFailedConnectionAttempt(void);

	/// Queued events of new incoming connections
	SystemAddress HasNewIncomingConnection(void);

	/// Queued events of lost connections
	SystemAddress HasLostConnection(void);

	/// Return an allocated but empty packet, for custom use
	Packet *AllocatePacket(unsigned dataSize);

	// Push a packet back to the queue
	virtual void PushBackPacket(Packet *packet, bool pushAtHead);

	/// Returns if Start() was called successfully
	bool WasStarted(void) const;

	void AttachPlugin(PluginInterface2 *plugin);
	void DetachPlugin(PluginInterface2 *plugin);

private:
	std::unique_ptr<RPC> rpc;

protected:
	Packet *DispatchRPC(Packet *packet);
	Packet *ReceiveInt(void);

	bool CreateListenSocket(unsigned short port, unsigned short maxIncomingConnections, unsigned short socketFamily, const char *hostAddress);

	// Plugins
	std::vector<PluginInterface2 *> messageHandlerList;

	MafiaNet::LocklessUint32_t isStarted, threadRunning;
	__TCPSOCKET__ listenSocket;

	// PushBackPacket output; user thread only.
	std::deque<Packet *> headPush, tailPush;
	RemoteClient *remoteClients;
	unsigned short remoteClientsLength;

	// Assuming remoteClients is only used by one thread!
	// std::vector<RemoteClient*> remoteClients;
	// Use this thread-safe queue to add to remoteClients
	// LockedQueue<RemoteClient*> remoteClientsInsertionQueue;
	// SimpleMutex remoteClientsInsertionQueueMutex;

	/*
	struct OutgoingMessage
	{
		unsigned char* data;
		SystemAddress systemAddress;
		bool broadcast;
		unsigned int length;
	};
	*/
	// Frees a packet the TCP thread queued: its std::malloc'd data, then the packet itself. A packet popped
	// for the user is released from its unique_ptr and freed by DeallocatePacket the same way.
	struct IncomingPacketDeleter
	{
		void operator()(Packet *packet) const
		{
			std::free(packet->data);
			delete packet;
		}
	};
	// Filled by the TCP thread, drained by the user thread (Receive, HasNewIncomingConnection, HasLostConnection).
	LockedQueue<std::unique_ptr<Packet, IncomingPacketDeleter>> incomingMessages;
	LockedQueue<SystemAddress> newIncomingConnections, lostConnections;
	// Filled by Connect (or its connection thread), drained by the user thread.
	LockedQueue<SystemAddress> completedConnectionAttempts, failedConnectionAttempts;

	int threadPriority;


	friend RAK_THREAD_DECLARATION(UpdateTCPInterfaceLoop);
	friend RAK_THREAD_DECLARATION(ConnectionAttemptLoop);

	//	void DeleteRemoteClient(RemoteClient *remoteClient, fd_set *exceptionFD);
	//	void InsertRemoteClient(RemoteClient* remoteClient);
	// Tries every address \a host resolves to (in \a socketFamily) in order until one connects. On success, if
	// \a peerAddress is given, it receives the connected peer's address (an IPv4-mapped one in its plain IPv4 form).
	// Returns 0 on failure.
	__TCPSOCKET__ SocketConnect(const char *host, unsigned short remotePort, unsigned short socketFamily, const char *bindAddress, SystemAddress *peerAddress = nullptr);

	struct ThisPtrPlusSysAddr
	{
		TCPInterface *tcpInterface;
		SystemAddress systemAddress;
		// What SocketConnect resolves (see ConnectHost in the .cpp); owned, as the caller's string may be gone.
		std::string host;
		char bindAddress[64];
		unsigned short socketFamily;
	};
};

/// \internal Unsent bytes of a TCP connection's send buffer \a buffer: buffer[offset, size).
inline std::size_t TcpOutgoingPending(const std::vector<unsigned char> &buffer, std::size_t offset)
{
	return buffer.size() - offset;
}

/// \internal Marks \a sent more bytes of \a buffer as sent (\a sent <= TcpOutgoingPending()). The
/// sent prefix is only dropped once it is at least half the buffer (or everything was sent), so
/// draining n bytes in any chunk sizes moves fewer than n bytes in total; erasing the sent prefix
/// on every send moved the whole backlog each time, O(n^2). Returns the bytes this call moved.
inline std::size_t TcpOutgoingConsume(std::vector<unsigned char> &buffer, std::size_t &offset, std::size_t sent)
{
	offset += sent;
	if (offset >= buffer.size())
	{
		buffer.clear();
		offset = 0;
		return 0;
	}
	if (offset < buffer.size() - offset)
		return 0;
	const std::size_t moved = buffer.size() - offset;
	buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
	offset = 0;
	return moved;
}

/// Stores information about a remote client.
// Thread ownership: socket, systemAddress, isActive and socketsToClose are
// written under isActiveMutex. Only the update thread uses a socket (select,
// recv, send) and only the update thread closes one: SetActive(false) shuts the
// connection down and hands the socket to socketsToClose, which the update
// thread drains under the lock before each select. A descriptor the update thread snapshotted therefore
// stays open, and cannot be reused for another connection, while it uses it.
struct RemoteClient
{
	RemoteClient()
	{
		isActive = false;
		socket = 0;
	}
	__TCPSOCKET__ socket;
	SystemAddress systemAddress;
	// Bytes queued for the socket, oldest first; outgoingData[outgoingOffset, size) is unsent (see
	// TcpOutgoingConsume). Both guarded by outgoingDataMutex.
	std::vector<unsigned char> outgoingData;
	std::size_t outgoingOffset = 0;
	bool isActive;
	SimpleMutex outgoingDataMutex;
	SimpleMutex isActiveMutex;
	// Sockets of retired connections, waiting for the update thread to close them.
	std::vector<__TCPSOCKET__> socketsToClose;

	// \a s is the update thread's snapshot of socket, never the member itself.
	int Send(__TCPSOCKET__ s, const char *data, unsigned int length);
	int Recv(__TCPSOCKET__ s, char *data, const int dataSize);
	void Reset(void)
	{
		outgoingDataMutex.Lock();
		outgoingData.clear();
		outgoingOffset = 0;
		outgoingDataMutex.Unlock();
	}
	// Requires isActiveMutex. Deactivating shuts the socket down but does not close it: see socketsToClose.
	void SetActive(bool a);
	// Requires isActiveMutex. Closes every socket in socketsToClose (update thread, or after it stopped).
	void CloseRetiredSockets(void);
	// Requires isActiveMutex.
	void SendOrBuffer(const char **data, const unsigned int *lengths, const int numParameters);
};

} // namespace MafiaNet

#endif

#endif // _RAKNET_SUPPORT_*

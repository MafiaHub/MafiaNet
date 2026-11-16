/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief A simple TCP based server allowing sends and receives.  Can be connected by any TCP client, including telnet.
///


#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1

#ifndef __PACKETIZED_TCP
#define __PACKETIZED_TCP

#include "mafianet/tcp_interface.h"

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace MafiaNet
{

class RAK_DLL_EXPORT PacketizedTCP : public TCPInterface
{
public:
	// GetInstance() and DestroyInstance(instance*)
	STATIC_FACTORY_DECLARATIONS(PacketizedTCP)

	PacketizedTCP();
	virtual ~PacketizedTCP();
	PacketizedTCP(const PacketizedTCP &) = delete;
	PacketizedTCP &operator=(const PacketizedTCP &) = delete;

	/// Every message is framed by a 4-byte big-endian length prefix. A peer that announces a message
	/// longer than this many bytes is disconnected (reported by HasLostConnection()) instead of being
	/// buffered: the prefix is untrusted input, and honouring it would let one peer hold up to 4 GiB.
	static constexpr uint32_t DEFAULT_MAX_MESSAGE_LENGTH = 64u * 1024u * 1024u;

	/// Sets the longest message accepted from a peer; see DEFAULT_MAX_MESSAGE_LENGTH. Messages this
	/// side sends are not checked: the receiving side's limit applies to them.
	void SetMaxMessageLength(uint32_t bytes);
	uint32_t GetMaxMessageLength(void) const;

	/// Stops the TCP server
	void Stop(void);

	/// Sends a byte stream
	void Send(const char *data, unsigned length, const SystemAddress &systemAddress, bool broadcast);

	// Sends a concatenated list of byte streams
	bool SendList(const char **data, const unsigned int *lengths, const int numParameters, const SystemAddress &systemAddress, bool broadcast);

	/// Returns data received
	Packet *Receive(void);

	/// Disconnects a player/address
	void CloseConnection(SystemAddress systemAddress);

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

protected:
	void ClearAllConnections(void);
	void RemoveFromConnectionList(const SystemAddress &sa);
	void AddToConnectionList(const SystemAddress &sa);
	void PushNotificationsToQueues(void);
	Packet *ReturnOutgoingPacket(void);

	// Appends \a length received bytes to the stream of \a systemAddress and queues every message it
	// completes (or a download-progress notification) on waitingPackets. Drops the connection when a
	// length prefix exceeds maxMessageLength.
	void OnBytesReceived(const SystemAddress &systemAddress, std::vector<unsigned char> &stream, const unsigned char *data, unsigned int length);

	// A single TCP recieve may generate multiple split packets. They are stored in the waitingPackets list until Receive is called
	std::deque<Packet *> waitingPackets;
	// Bytes received from each connection that do not form a whole message yet.
	std::map<SystemAddress, std::vector<unsigned char>> connections;
	uint32_t maxMessageLength;

	// Mirrors single producer / consumer, but processes them in Receive() before returning to user
	std::deque<SystemAddress> _newIncomingConnections, _lostConnections, _failedConnectionAttempts, _completedConnectionAttempts;
};

} // namespace MafiaNet

#endif

#endif // _RAKNET_SUPPORT_*

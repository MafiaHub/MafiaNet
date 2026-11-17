/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/native_feature_includes.h"
#include <cstdlib>
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1

#include <algorithm> // used for std::min
#include "mafianet/packetized_tcp.h"
#include "mafianet/native_types.h"
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

#include <cstring>

using namespace MafiaNet;

typedef uint32_t PTCPHeader;

STATIC_FACTORY_DEFINITIONS(PacketizedTCP, PacketizedTCP);

namespace
{
constexpr std::size_t HEADER_LENGTH = sizeof(PTCPHeader);
constexpr unsigned int PROGRESS_CHUNK_SIZE = 65536;

// The length prefix at \a header: 4 bytes in network (big-endian) order, as Send writes it.
PTCPHeader ReadLengthPrefix(const unsigned char *header)
{
	PTCPHeader length;
	std::memcpy(&length, header, sizeof(length));
	if (MafiaNet::BitStream::DoEndianSwap())
		MafiaNet::BitStream::ReverseBytesInPlace((unsigned char *) &length, sizeof(length));
	return length;
}
} // namespace

PacketizedTCP::PacketizedTCP()
    : maxMessageLength(DEFAULT_MAX_MESSAGE_LENGTH)
{
}
PacketizedTCP::~PacketizedTCP()
{
	for (Packet *packet : waitingPackets)
		DeallocatePacket(packet);
	ClearAllConnections();
}

void PacketizedTCP::SetMaxMessageLength(uint32_t bytes)
{
	maxMessageLength = bytes;
}
uint32_t PacketizedTCP::GetMaxMessageLength(void) const
{
	return maxMessageLength;
}

void PacketizedTCP::Stop(void)
{
	TCPInterface::Stop();
	for (Packet *packet : waitingPackets)
		DeallocatePacket(packet);
	waitingPackets.clear();
	ClearAllConnections();
}

void PacketizedTCP::Send(const char *data, unsigned length, const SystemAddress &systemAddress, bool broadcast)
{
	PTCPHeader dataLength;
	dataLength = length;
#ifndef __BITSTREAM_NATIVE_END
	if (MafiaNet::BitStream::DoEndianSwap())
		MafiaNet::BitStream::ReverseBytes((unsigned char *) &length, (unsigned char *) &dataLength, sizeof(dataLength));
#else
	dataLength = length;
#endif

	unsigned int lengthsArray[2];
	const char *dataArray[2];
	dataArray[0] = (char *) &dataLength;
	dataArray[1] = data;
	lengthsArray[0] = sizeof(dataLength);
	lengthsArray[1] = length;
	TCPInterface::SendList(dataArray, lengthsArray, 2, systemAddress, broadcast);
}
bool PacketizedTCP::SendList(const char **data, const unsigned int *lengths, const int numParameters, const SystemAddress &systemAddress, bool broadcast)
{
	if (isStarted.GetValue() == 0)
		return false;
	if (data == 0)
		return false;
	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS && broadcast == false)
		return false;
	PTCPHeader totalLengthOfUserData = 0;
	int i;
	for (i = 0; i < numParameters; i++)
	{
		if (lengths[i] > 0)
			totalLengthOfUserData += lengths[i];
	}
	if (totalLengthOfUserData == 0)
		return false;

	PTCPHeader dataLength;
#ifndef __BITSTREAM_NATIVE_END
	if (MafiaNet::BitStream::DoEndianSwap())
		MafiaNet::BitStream::ReverseBytes((unsigned char *) &totalLengthOfUserData, (unsigned char *) &dataLength, sizeof(dataLength));
#else
	dataLength = totalLengthOfUserData;
#endif


	unsigned int lengthsArray[512];
	const char *dataArray[512];
	dataArray[0] = (char *) &dataLength;
	lengthsArray[0] = sizeof(dataLength);
	for (i = 0; i < 512 && i < numParameters; i++)
	{
		dataArray[i + 1] = data[i];
		lengthsArray[i + 1] = lengths[i];
	}
	return TCPInterface::SendList(dataArray, lengthsArray, std::min(numParameters, 511) + 1, systemAddress, broadcast);
}
void PacketizedTCP::PushNotificationsToQueues(void)
{
	SystemAddress sa;
	sa = TCPInterface::HasNewIncomingConnection();
	if (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		_newIncomingConnections.push_back(sa);
		AddToConnectionList(sa);
	}

	sa = TCPInterface::HasFailedConnectionAttempt();
	if (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		_failedConnectionAttempts.push_back(sa);
	}

	sa = TCPInterface::HasLostConnection();
	if (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		_lostConnections.push_back(sa);
		RemoveFromConnectionList(sa);
	}

	sa = TCPInterface::HasCompletedConnectionAttempt();
	if (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		_completedConnectionAttempts.push_back(sa);
		AddToConnectionList(sa);
	}
}
Packet *PacketizedTCP::Receive(void)
{
	PushNotificationsToQueues();

	unsigned int i;
	for (i = 0; i < messageHandlerList.size(); i++)
		messageHandlerList[i]->Update();

	Packet *outgoingPacket = ReturnOutgoingPacket();
	if (outgoingPacket)
		return outgoingPacket;

	Packet *incomingPacket;
	incomingPacket = TCPInterface::ReceiveInt();

	while (incomingPacket)
	{
		const auto connection = connections.find(incomingPacket->systemAddress);
		if (connection == connections.end())
		{
			DeallocatePacket(incomingPacket);
			incomingPacket = TCPInterface::ReceiveInt();
			continue;
		}

		if (incomingPacket->deleteData == true)
		{
			// Came from network
			OnBytesReceived(incomingPacket->systemAddress, connection->second, incomingPacket->data, incomingPacket->length);
			DeallocatePacket(incomingPacket);
			incomingPacket = 0;
		}
		else
			waitingPackets.push_back(incomingPacket);

		incomingPacket = TCPInterface::ReceiveInt();
	}

	return ReturnOutgoingPacket();
}
void PacketizedTCP::OnBytesReceived(const SystemAddress &systemAddress, std::vector<unsigned char> &stream, const unsigned char *data, unsigned int length)
{
	const std::size_t oldWritten = stream.size();
	stream.insert(stream.end(), data, data + length);

	// Take every complete message off the front. The prefix is peer input: check it against the
	// limit before waiting for (or allocating) the payload it announces. 64-bit arithmetic, so a
	// prefix near 4 GiB cannot wrap the comparison.
	std::size_t offset = 0;
	bool tooLong = false;
	while (stream.size() - offset >= HEADER_LENGTH)
	{
		const PTCPHeader dataLength = ReadLengthPrefix(stream.data() + offset);
		if (dataLength > maxMessageLength)
		{
			tooLong = true;
			break;
		}
		if (stream.size() - offset - HEADER_LENGTH < static_cast<std::size_t>(dataLength))
			break;

		Packet *outgoingPacket = new Packet;
		outgoingPacket->length = dataLength;
		outgoingPacket->bitSize = BYTES_TO_BITS(dataLength);
		outgoingPacket->guid = UNASSIGNED_RAKNET_GUID;
		outgoingPacket->systemAddress = systemAddress;
		outgoingPacket->deleteData = false; // Did not come from the network
		outgoingPacket->data = (unsigned char *) std::malloc(dataLength);
		if (outgoingPacket->data == 0 && dataLength != 0)
		{
			delete outgoingPacket;
			break; // Out of memory: keep the bytes and try again on the next read
		}
		if (dataLength != 0)
			std::memcpy(outgoingPacket->data, stream.data() + offset + HEADER_LENGTH, dataLength);
		waitingPackets.push_back(outgoingPacket);
		offset += HEADER_LENGTH + dataLength;
	}
	const bool extractedAny = offset != 0;
	stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(offset));

	if (tooLong)
	{
		// Malformed or hostile stream: nothing after this prefix can be framed. Drop the
		// connection and report it like any other lost connection.
		CloseConnection(systemAddress);
		_lostConnections.push_back(systemAddress);
		return;
	}

	// A large message still on its way: return ID_DOWNLOAD_PROGRESS each time another 64 KiB arrived
	if (extractedAny || stream.size() < HEADER_LENGTH)
		return;
	const std::size_t newWritten = stream.size();
	if (newWritten / PROGRESS_CHUNK_SIZE == oldWritten / PROGRESS_CHUNK_SIZE)
		return;

	const PTCPHeader dataLength = ReadLengthPrefix(stream.data());
	Packet *outgoingPacket = new Packet;
	outgoingPacket->length = sizeof(MessageID) +
	                         sizeof(unsigned int) * 2 +
	                         sizeof(unsigned int) +
	                         PROGRESS_CHUNK_SIZE;
	outgoingPacket->bitSize = BYTES_TO_BITS(length);
	outgoingPacket->guid = UNASSIGNED_RAKNET_GUID;
	outgoingPacket->systemAddress = systemAddress;
	outgoingPacket->deleteData = false;
	outgoingPacket->data = (unsigned char *) std::malloc(outgoingPacket->length);
	if (outgoingPacket->data == 0)
	{
		delete outgoingPacket;
		return;
	}

	outgoingPacket->data[0] = (MessageID) ID_DOWNLOAD_PROGRESS;
	unsigned int totalParts = dataLength / PROGRESS_CHUNK_SIZE;
	auto partIndex = static_cast<unsigned int>(newWritten / PROGRESS_CHUNK_SIZE);
	unsigned int oneChunkSize = PROGRESS_CHUNK_SIZE;
	memcpy(outgoingPacket->data + sizeof(MessageID), &partIndex, sizeof(unsigned int));
	memcpy(outgoingPacket->data + sizeof(MessageID) + sizeof(unsigned int) * 1, &totalParts, sizeof(unsigned int));
	memcpy(outgoingPacket->data + sizeof(MessageID) + sizeof(unsigned int) * 2, &oneChunkSize, sizeof(unsigned int));
	// The start of the payload received so far (the prefix skipped), zero-filled if shorter.
	unsigned char *chunk = outgoingPacket->data + sizeof(MessageID) + sizeof(unsigned int) * 3;
	const std::size_t available = std::min<std::size_t>(stream.size() - HEADER_LENGTH, oneChunkSize);
	std::memcpy(chunk, stream.data() + HEADER_LENGTH, available);
	std::memset(chunk + available, 0, oneChunkSize - available);

	waitingPackets.push_back(outgoingPacket);
}
Packet *PacketizedTCP::ReturnOutgoingPacket(void)
{
	Packet *outgoingPacket = 0;
	unsigned int i;
	while (outgoingPacket == 0 && !waitingPackets.empty())
	{
		outgoingPacket = waitingPackets.front();
		waitingPackets.pop_front();
		PluginReceiveResult pluginResult;
		for (i = 0; i < messageHandlerList.size(); i++)
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
void PacketizedTCP::CloseConnection(SystemAddress systemAddress)
{
	RemoveFromConnectionList(systemAddress);
	TCPInterface::CloseConnection(systemAddress);
}
void PacketizedTCP::RemoveFromConnectionList(const SystemAddress &sa)
{
	if (sa == UNASSIGNED_SYSTEM_ADDRESS)
		return;
	connections.erase(sa);
}
void PacketizedTCP::AddToConnectionList(const SystemAddress &sa)
{
	if (sa == UNASSIGNED_SYSTEM_ADDRESS)
		return;
	// A fresh stream: a reconnect from the same address does not inherit old bytes.
	connections[sa].clear();
}
void PacketizedTCP::ClearAllConnections(void)
{
	connections.clear();
}
SystemAddress PacketizedTCP::HasCompletedConnectionAttempt(void)
{
	PushNotificationsToQueues();

	if (!_completedConnectionAttempts.empty())
	{
		const SystemAddress sa = _completedConnectionAttempts.front();
		_completedConnectionAttempts.pop_front();
		return sa;
	}
	return UNASSIGNED_SYSTEM_ADDRESS;
}
SystemAddress PacketizedTCP::HasFailedConnectionAttempt(void)
{
	PushNotificationsToQueues();

	if (!_failedConnectionAttempts.empty())
	{
		const SystemAddress sa = _failedConnectionAttempts.front();
		_failedConnectionAttempts.pop_front();
		return sa;
	}
	return UNASSIGNED_SYSTEM_ADDRESS;
}
SystemAddress PacketizedTCP::HasNewIncomingConnection(void)
{
	PushNotificationsToQueues();

	if (!_newIncomingConnections.empty())
	{
		const SystemAddress sa = _newIncomingConnections.front();
		_newIncomingConnections.pop_front();
		return sa;
	}
	return UNASSIGNED_SYSTEM_ADDRESS;
}
SystemAddress PacketizedTCP::HasLostConnection(void)
{
	PushNotificationsToQueues();

	if (!_lostConnections.empty())
	{
		const SystemAddress sa = _lostConnections.front();
		_lostConnections.pop_front();
		return sa;
	}
	return UNASSIGNED_SYSTEM_ADDRESS;
}

#endif // _RAKNET_SUPPORT_*

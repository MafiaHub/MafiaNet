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
#if _RAKNET_SUPPORT_PacketizedTCP==1 && _RAKNET_SUPPORT_TCPInterface==1

#include <algorithm> // used for std::min
#include "mafianet/packetized_tcp.h"
#include "mafianet/native_types.h"
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/alloca.h"

using namespace MafiaNet;

typedef uint32_t PTCPHeader;

STATIC_FACTORY_DEFINITIONS(PacketizedTCP,PacketizedTCP);

PacketizedTCP::PacketizedTCP()
{

}
PacketizedTCP::~PacketizedTCP()
{
	// Anything still in waitingPackets was leaked: only Stop() freed it, and
	// nothing calls Stop() on the way out.
	ClearWaitingPackets();
	ClearAllConnections();
}

void PacketizedTCP::Stop(void)
{
	TCPInterface::Stop();
	// The pointers used to be left in the queue after being freed, so a second
	// Stop() double-freed them and a Receive() after Stop() handed one back to the
	// caller.
	ClearWaitingPackets();
	ClearAllConnections();
}

void PacketizedTCP::Send( const char *data, unsigned length, const SystemAddress &systemAddress, bool broadcast )
{
	PTCPHeader dataLength;
	dataLength=length;
#ifndef __BITSTREAM_NATIVE_END
	if (MafiaNet::BitStream::DoEndianSwap())
		MafiaNet::BitStream::ReverseBytes((unsigned char*) &length,(unsigned char*) &dataLength,sizeof(dataLength));
#else
		dataLength=length;
#endif

	unsigned int lengthsArray[2];
	const char *dataArray[2];
	dataArray[0]=(char*) &dataLength;
	dataArray[1]=data;
	lengthsArray[0]=sizeof(dataLength);
	lengthsArray[1]=length;
	TCPInterface::SendList(dataArray,lengthsArray,2,systemAddress,broadcast);
}
bool PacketizedTCP::SendList( const char **data, const unsigned int *lengths, const int numParameters, const SystemAddress &systemAddress, bool broadcast )
{
	if (isStarted.GetValue()==0)
		return false;
	if (data==0)
		return false;
	if (systemAddress==UNASSIGNED_SYSTEM_ADDRESS && broadcast==false)
		return false;
	PTCPHeader totalLengthOfUserData=0;
	int i;
	for (i=0; i < numParameters; i++)
	{
		if (lengths[i]>0)
			totalLengthOfUserData+=lengths[i];
	}
	if (totalLengthOfUserData==0)
		return false;

	PTCPHeader dataLength;
#ifndef __BITSTREAM_NATIVE_END
	if (MafiaNet::BitStream::DoEndianSwap())
		MafiaNet::BitStream::ReverseBytes((unsigned char*) &totalLengthOfUserData,(unsigned char*) &dataLength,sizeof(dataLength));
#else
	dataLength=totalLengthOfUserData;
#endif
	

	unsigned int lengthsArray[512];
	const char *dataArray[512];
	dataArray[0]=(char*) &dataLength;
	lengthsArray[0]=sizeof(dataLength);
	for (i=0; i < 512 && i < numParameters; i++)
	{
		dataArray[i+1]=data[i];
		lengthsArray[i+1]=lengths[i];
	}	
	return TCPInterface::SendList(dataArray,lengthsArray,std::min(numParameters, 511)+1,systemAddress,broadcast);
}
void PacketizedTCP::PushNotificationsToQueues(void)
{
	SystemAddress sa;
	sa = TCPInterface::HasNewIncomingConnection();
	if (sa!=UNASSIGNED_SYSTEM_ADDRESS)
	{
		_newIncomingConnections.push_back(sa);
		AddToConnectionList(sa);
	}

	sa = TCPInterface::HasFailedConnectionAttempt();
	if (sa!=UNASSIGNED_SYSTEM_ADDRESS)
	{
		_failedConnectionAttempts.push_back(sa);
	}

	sa = TCPInterface::HasLostConnection();
	if (sa!=UNASSIGNED_SYSTEM_ADDRESS)
	{
		_lostConnections.push_back(sa);
		RemoveFromConnectionList(sa);
	}

	sa = TCPInterface::HasCompletedConnectionAttempt();
	if (sa!=UNASSIGNED_SYSTEM_ADDRESS)
	{
		_completedConnectionAttempts.push_back(sa);
		AddToConnectionList(sa);
	}
}
Packet* PacketizedTCP::Receive( void )
{
	PushNotificationsToQueues();

	unsigned int i;
	for (i=0; i < messageHandlerList.size(); i++)
		messageHandlerList[i]->Update();

	Packet *outgoingPacket=ReturnOutgoingPacket();
	if (outgoingPacket)
		return outgoingPacket;

	Packet *incomingPacket;
	incomingPacket = TCPInterface::ReceiveInt();

	while (incomingPacket)
	{
		std::map<SystemAddress, DataStructures::ByteQueue *>::iterator connection =
			connections.find(incomingPacket->systemAddress);
		if (connection == connections.end())
		{
			DeallocatePacket(incomingPacket);
			incomingPacket = TCPInterface::ReceiveInt();
			continue;
		}


		if (incomingPacket->deleteData==true)
		{
			// Came from network
			SystemAddress systemAddressFromPacket;
			{
				DataStructures::ByteQueue *bq = connection->second;
				// Buffer data
				bq->WriteBytes((const char*) incomingPacket->data,incomingPacket->length, _FILE_AND_LINE_);
				systemAddressFromPacket=incomingPacket->systemAddress;
				PTCPHeader dataLength;

				// Peek the header to see if a full message is waiting
				bq->ReadBytes((char*) &dataLength,sizeof(PTCPHeader),true);
				if (MafiaNet::BitStream::DoEndianSwap())
					MafiaNet::BitStream::ReverseBytesInPlace((unsigned char*) &dataLength,sizeof(dataLength));
				// Header indicates packet length. If enough data is available, read out and return one packet
				if (bq->GetBytesWritten()>=dataLength+sizeof(PTCPHeader))
				{
					do 
					{
						bq->IncrementReadOffset(sizeof(PTCPHeader));
						outgoingPacket = MafiaNet::OP_NEW<Packet>(_FILE_AND_LINE_);
						outgoingPacket->length=dataLength;
						outgoingPacket->bitSize=BYTES_TO_BITS(dataLength);
						outgoingPacket->guid=UNASSIGNED_RAKNET_GUID;
						outgoingPacket->systemAddress=systemAddressFromPacket;
						outgoingPacket->deleteData=false; // Did not come from the network
						outgoingPacket->data=(unsigned char*) rakMalloc_Ex(dataLength, _FILE_AND_LINE_);
						if (outgoingPacket->data==0)
						{
							notifyOutOfMemory(_FILE_AND_LINE_);
							MafiaNet::OP_DELETE(outgoingPacket,_FILE_AND_LINE_);
							return 0;
						}
						bq->ReadBytes((char*) outgoingPacket->data,dataLength,false);

						waitingPackets.push_back(outgoingPacket);

						// Peek the header to see if a full message is waiting
						if (bq->ReadBytes((char*) &dataLength,sizeof(PTCPHeader),true))
						{
							if (MafiaNet::BitStream::DoEndianSwap())
								MafiaNet::BitStream::ReverseBytesInPlace((unsigned char*) &dataLength,sizeof(dataLength));
						}
						else
							break;
					} while (bq->GetBytesWritten()>=dataLength+sizeof(PTCPHeader));
				}
				else
				{

					unsigned int oldWritten = bq->GetBytesWritten()-incomingPacket->length;
					unsigned int newWritten = bq->GetBytesWritten();

					// Return ID_DOWNLOAD_PROGRESS
					if (newWritten/65536!=oldWritten/65536)
					{
						outgoingPacket = MafiaNet::OP_NEW<Packet>(_FILE_AND_LINE_);
						outgoingPacket->length=sizeof(MessageID) +
							sizeof(unsigned int)*2 +
							sizeof(unsigned int) +
							65536;
						// This took its bit size from the *incoming* packet, which has
						// nothing to do with the progress packet's own length, so
						// bitSize and length disagreed on every ID_DOWNLOAD_PROGRESS.
						outgoingPacket->bitSize=BYTES_TO_BITS(outgoingPacket->length);
						outgoingPacket->guid=UNASSIGNED_RAKNET_GUID;
						outgoingPacket->systemAddress=incomingPacket->systemAddress;
						outgoingPacket->deleteData=false;
						outgoingPacket->data=(unsigned char*) rakMalloc_Ex(outgoingPacket->length, _FILE_AND_LINE_);
						if (outgoingPacket->data==0)
						{
							notifyOutOfMemory(_FILE_AND_LINE_);
							MafiaNet::OP_DELETE(outgoingPacket,_FILE_AND_LINE_);
							return 0;
						}

						outgoingPacket->data[0]=(MessageID)ID_DOWNLOAD_PROGRESS;
						unsigned int totalParts=dataLength/65536;
						unsigned int partIndex=newWritten/65536;
						unsigned int oneChunkSize=65536;
						memcpy(outgoingPacket->data+sizeof(MessageID), &partIndex, sizeof(unsigned int));
						memcpy(outgoingPacket->data+sizeof(MessageID)+sizeof(unsigned int)*1, &totalParts, sizeof(unsigned int));
						memcpy(outgoingPacket->data+sizeof(MessageID)+sizeof(unsigned int)*2, &oneChunkSize, sizeof(unsigned int));
						bq->IncrementReadOffset(sizeof(PTCPHeader));
						bq->ReadBytes((char*) outgoingPacket->data+sizeof(MessageID)+sizeof(unsigned int)*3,oneChunkSize,true);
						bq->DecrementReadOffset(sizeof(PTCPHeader));

						waitingPackets.push_back(outgoingPacket);
					}
				}

			}

			DeallocatePacket(incomingPacket);
			incomingPacket=0;
		}
		else
			waitingPackets.push_back(incomingPacket);

		incomingPacket = TCPInterface::ReceiveInt();
	}

	return ReturnOutgoingPacket();
}
Packet *PacketizedTCP::ReturnOutgoingPacket(void)
{
	Packet *outgoingPacket=0;
	unsigned int i;
	while (outgoingPacket==0 && waitingPackets.empty()==false)
	{
		outgoingPacket=waitingPackets.front();
		waitingPackets.pop_front();
		PluginReceiveResult pluginResult;
		for (i=0; i < messageHandlerList.size(); i++)
		{
			pluginResult=messageHandlerList[i]->OnReceive(outgoingPacket);
			if (pluginResult==RR_STOP_PROCESSING_AND_DEALLOCATE)
			{
				DeallocatePacket( outgoingPacket );
				outgoingPacket=0; // Will do the loop again and get another packet
				break; // break out of the enclosing for
			}
			else if (pluginResult==RR_STOP_PROCESSING)
			{
				outgoingPacket=0;
				break;
			}
		}
	}

	return outgoingPacket;
}
void PacketizedTCP::CloseConnection( SystemAddress systemAddress )
{
	RemoveFromConnectionList(systemAddress);
	TCPInterface::CloseConnection(systemAddress);
}
void PacketizedTCP::RemoveFromConnectionList(const SystemAddress &sa)
{
	if (sa==UNASSIGNED_SYSTEM_ADDRESS)
		return;
	std::map<SystemAddress, DataStructures::ByteQueue *>::iterator it = connections.find(sa);
	if (it != connections.end())
	{
		MafiaNet::OP_DELETE(it->second,_FILE_AND_LINE_);
		connections.erase(it);
	}
}
void PacketizedTCP::AddToConnectionList(const SystemAddress &sa)
{
	if (sa==UNASSIGNED_SYSTEM_ADDRESS)
		return;
	// DataStructures::Map::SetNew asserted on a duplicate key under _DEBUG and then
	// replaced the value, which leaked the old buffer. Keep the assert, and if it
	// does happen, free what was there instead of dropping it on the floor.
	std::map<SystemAddress, DataStructures::ByteQueue *>::iterator existing = connections.find(sa);
	RakAssert(existing == connections.end());
	if (existing != connections.end())
	{
		MafiaNet::OP_DELETE(existing->second,_FILE_AND_LINE_);
		existing->second = MafiaNet::OP_NEW<DataStructures::ByteQueue>(_FILE_AND_LINE_);
		return;
	}
	connections[sa] = MafiaNet::OP_NEW<DataStructures::ByteQueue>(_FILE_AND_LINE_);
}
void PacketizedTCP::ClearWaitingPackets(void)
{
	for (size_t i=0; i < waitingPackets.size(); i++)
		DeallocatePacket(waitingPackets[i]);
	waitingPackets.clear();
}
void PacketizedTCP::ClearAllConnections(void)
{
	for (std::map<SystemAddress, DataStructures::ByteQueue *>::iterator it = connections.begin();
		it != connections.end(); ++it)
		MafiaNet::OP_DELETE(it->second,_FILE_AND_LINE_);
	connections.clear();
}
SystemAddress PacketizedTCP::HasCompletedConnectionAttempt(void)
{
	PushNotificationsToQueues();

	if (_completedConnectionAttempts.empty()==false)
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

	if (_failedConnectionAttempts.empty()==false)
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

	if (_newIncomingConnections.empty()==false)
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

	if (_lostConnections.empty()==false)
	{
		const SystemAddress sa = _lostConnections.front();
		_lostConnections.pop_front();
		return sa;
	}
	return UNASSIGNED_SYSTEM_ADDRESS;
}

#endif // _RAKNET_SUPPORT_*

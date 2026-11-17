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

#include "mafianet/native_feature_includes.h"
#include <cstring>
#if _RAKNET_SUPPORT_RPC4Plugin == 1

#include "mafianet/rpc4_plugin.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/packetized_tcp.h"
#include "mafianet/sleep.h"
#include "mafianet/defines.h"
//#include "mafianet/get_time.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#include <algorithm>
#include <deque>

using namespace MafiaNet;

/// Slot objects sorted by LocalSlotObjectComp: highest callPriority first, then registration order.
struct RPC4::LocalSlot
{
	std::vector<LocalSlotObject> slotObjects;
};

namespace
{
// The identifier as a map key, copied verbatim. The implicit RakString(const char *format, ...)
// conversion would run it through vsnprintf, and names also arrive from the network.
RakString IdentifierKey(const char *identifier)
{
	RakString key;
	key = identifier;
	return key;
}
} // namespace

STATIC_FACTORY_DEFINITIONS(RPC4, RPC4);

struct GlobalRegistration
{
	void (*registerFunctionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context);
	void (*registerBlockingFunctionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context);
	void *context;
	char functionName[RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH];
	MessageID messageId;
	int callPriority;
};
static GlobalRegistration globalRegistrationBuffer[RPC4_GLOBAL_REGISTRATION_MAX_FUNCTIONS];
static unsigned int globalRegistrationIndex = 0;

RPC4GlobalRegistration::RPC4GlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context)
{
	RakAssert(globalRegistrationIndex != RPC4_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
	{
		globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = uniqueID[i];
	}
	RakAssert(uniqueID[i] == 0); // uniqueID must fit in functionName including the NUL terminator
	globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = '\0';
	globalRegistrationBuffer[globalRegistrationIndex].registerFunctionPointer = functionPointer;
	globalRegistrationBuffer[globalRegistrationIndex].registerBlockingFunctionPointer = 0;
	globalRegistrationBuffer[globalRegistrationIndex].context = context;
	globalRegistrationBuffer[globalRegistrationIndex].callPriority = 0xFFFFFFFF;
	globalRegistrationIndex++;
}
RPC4GlobalRegistration::RPC4GlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context, int callPriority)
{
	RakAssert(globalRegistrationIndex != RPC4_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
	{
		globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = uniqueID[i];
	}
	RakAssert(uniqueID[i] == 0); // uniqueID must fit in functionName including the NUL terminator
	globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = '\0';
	globalRegistrationBuffer[globalRegistrationIndex].registerFunctionPointer = functionPointer;
	globalRegistrationBuffer[globalRegistrationIndex].registerBlockingFunctionPointer = 0;
	globalRegistrationBuffer[globalRegistrationIndex].context = context;
	RakAssert(callPriority != (int) 0xFFFFFFFF);
	globalRegistrationBuffer[globalRegistrationIndex].callPriority = callPriority;
	globalRegistrationIndex++;
}
RPC4GlobalRegistration::RPC4GlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context), void *context)
{
	RakAssert(globalRegistrationIndex != RPC4_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
	{
		globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = uniqueID[i];
	}
	RakAssert(uniqueID[i] == 0); // uniqueID must fit in functionName including the NUL terminator
	globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = '\0';
	globalRegistrationBuffer[globalRegistrationIndex].registerFunctionPointer = 0;
	globalRegistrationBuffer[globalRegistrationIndex].registerBlockingFunctionPointer = functionPointer;
	globalRegistrationBuffer[globalRegistrationIndex].context = context;
	globalRegistrationIndex++;
}
RPC4GlobalRegistration::RPC4GlobalRegistration(const char *uniqueID, MessageID messageId)
{
	RakAssert(globalRegistrationIndex != RPC4_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
	{
		globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = uniqueID[i];
	}
	RakAssert(uniqueID[i] == 0); // uniqueID must fit in functionName including the NUL terminator
	globalRegistrationBuffer[globalRegistrationIndex].functionName[i] = '\0';
	globalRegistrationBuffer[globalRegistrationIndex].registerFunctionPointer = 0;
	globalRegistrationBuffer[globalRegistrationIndex].registerBlockingFunctionPointer = 0;
	globalRegistrationBuffer[globalRegistrationIndex].context = 0;
	globalRegistrationBuffer[globalRegistrationIndex].messageId = messageId;
	globalRegistrationIndex++;
}

enum RPC4Identifiers
{
	ID_RPC4_CALL,
	ID_RPC4_RETURN,
	ID_RPC4_SIGNAL,
};
int RPC4::LocalSlotObjectComp(const LocalSlotObject &key, const LocalSlotObject &data)
{
	if (key.callPriority > data.callPriority)
		return -1;
	if (key.callPriority == data.callPriority)
	{
		if (key.registrationCount < data.registrationCount)
			return -1;
		if (key.registrationCount == data.registrationCount)
			return 0;
		return 1;
	}

	return 1;
}

RPC4::RPC4()
{
	gotBlockingReturnValue = false;
	nextSlotRegistrationCount = 0;
	interruptSignal = false;
}
RPC4::~RPC4() = default;
bool RPC4::RegisterFunction(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context)
{
	RegisteredNonblockingFunction rnf;
	rnf.functionPointer = functionPointer;
	rnf.context = context;
	return registeredNonblockingFunctions.emplace(IdentifierKey(uniqueID), rnf).second;
}
void RPC4::RegisterSlot(const char *sharedIdentifier, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context, int callPriority)
{
	LocalSlotObject lso(nextSlotRegistrationCount++, callPriority, functionPointer, context);
	std::unique_ptr<LocalSlot> &localSlot = localSlots[IdentifierKey(sharedIdentifier)];
	if (!localSlot)
		localSlot.reset(new LocalSlot);
	std::vector<LocalSlotObject> &slotObjects = localSlot->slotObjects;
	slotObjects.insert(std::lower_bound(slotObjects.begin(), slotObjects.end(), lso, [](const LocalSlotObject &a, const LocalSlotObject &b)
	                                    { return LocalSlotObjectComp(a, b) < 0; }),
	                   lso);
}
bool RPC4::RegisterBlockingFunction(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context), void *context)
{
	RegisteredBlockingFunction rbf;
	rbf.functionPointer = functionPointer;
	rbf.context = context;
	return registeredBlockingFunctions.emplace(IdentifierKey(uniqueID), rbf).second;
}
void RPC4::RegisterLocalCallback(const char *uniqueID, MessageID messageId)
{
	const MafiaNet::RakString str = IdentifierKey(uniqueID);
	LocalCallback &lc = localCallbacks[messageId];
	lc.messageId = messageId;
	const auto position = std::lower_bound(lc.functions.begin(), lc.functions.end(), str);
	if (position == lc.functions.end() || !(*position == str))
		lc.functions.insert(position, str);
}
bool RPC4::UnregisterFunction(const char *uniqueID)
{
	return registeredNonblockingFunctions.erase(IdentifierKey(uniqueID)) != 0;
}
bool RPC4::UnregisterBlockingFunction(const char *uniqueID)
{
	return registeredBlockingFunctions.erase(IdentifierKey(uniqueID)) != 0;
}
bool RPC4::UnregisterLocalCallback(const char *uniqueID, MessageID messageId)
{
	const MafiaNet::RakString str = IdentifierKey(uniqueID);
	const auto callback = localCallbacks.find(messageId);
	if (callback == localCallbacks.end())
		return false;
	std::vector<MafiaNet::RakString> &functions = callback->second.functions;
	const auto position = std::lower_bound(functions.begin(), functions.end(), str);
	if (position == functions.end() || !(*position == str))
		return false;
	functions.erase(position);
	if (functions.empty())
		localCallbacks.erase(callback);
	return true;
}
bool RPC4::UnregisterSlot(const char *sharedIdentifier)
{
	return localSlots.erase(IdentifierKey(sharedIdentifier)) != 0;
}
void RPC4::CallLoopback(const char *uniqueID, MafiaNet::BitStream *bitStream)
{
	Packet *p = 0;

	if (registeredNonblockingFunctions.find(IdentifierKey(uniqueID)) == registeredNonblockingFunctions.end())
	{
		if (rakPeerInterface)
			p = AllocatePacketUnified(sizeof(MessageID) + sizeof(unsigned char) + (unsigned int) strlen(uniqueID) + 1);
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
		else
			p = tcpInterface->AllocatePacket(sizeof(MessageID) + sizeof(unsigned char) + (unsigned int) strlen(uniqueID) + 1);
#endif

		if (rakPeerInterface)
			p->guid = rakPeerInterface->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
		else
			p->guid = UNASSIGNED_RAKNET_GUID;
#endif

		p->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
		p->systemAddress.systemIndex = (SystemIndex) -1;
		p->data[0] = ID_RPC_REMOTE_ERROR;
		p->data[1] = RPC_ERROR_FUNCTION_NOT_REGISTERED;
		strcpy_s((char *) p->data + 2, p->length - 2, uniqueID);

		PushBackPacketUnified(p, false);

		return;
	}

	MafiaNet::BitStream out;
	out.Write((MessageID) ID_RPC_PLUGIN);
	out.Write((MessageID) ID_RPC4_CALL);
	out.WriteCompressed(uniqueID);
	out.Write(false); // nonblocking
	if (bitStream)
	{
		bitStream->ResetReadPointer();
		out.AlignWriteToByteBoundary();
		out.Write(bitStream);
	}
	if (rakPeerInterface)
		p = AllocatePacketUnified(out.GetNumberOfBytesUsed());
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
	else
		p = tcpInterface->AllocatePacket(out.GetNumberOfBytesUsed());
#endif

	if (rakPeerInterface)
		p->guid = rakPeerInterface->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
	else
		p->guid = UNASSIGNED_RAKNET_GUID;
#endif
	p->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
	p->systemAddress.systemIndex = (SystemIndex) -1;
	memcpy(p->data, out.GetData(), out.GetNumberOfBytesUsed());
	PushBackPacketUnified(p, false);
	return;
}
void RPC4::Call(const char *uniqueID, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast)
{
	MafiaNet::BitStream out;
	out.Write((MessageID) ID_RPC_PLUGIN);
	out.Write((MessageID) ID_RPC4_CALL);
	out.WriteCompressed(uniqueID);
	out.Write(false); // Nonblocking
	if (bitStream)
	{
		bitStream->ResetReadPointer();
		out.AlignWriteToByteBoundary();
		out.Write(bitStream);
	}
	SendUnified(&out, priority, reliability, orderingChannel, systemIdentifier, broadcast);
}
bool RPC4::CallBlocking(const char *uniqueID, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, MafiaNet::BitStream *returnData)
{
	MafiaNet::BitStream out;
	out.Write((MessageID) ID_RPC_PLUGIN);
	out.Write((MessageID) ID_RPC4_CALL);
	out.WriteCompressed(uniqueID);
	out.Write(true); // Blocking
	if (bitStream)
	{
		bitStream->ResetReadPointer();
		out.AlignWriteToByteBoundary();
		out.Write(bitStream);
	}
	RakAssert(returnData);
	RakAssert(rakPeerInterface);
	ConnectionState cs;
	cs = rakPeerInterface->GetConnectionState(systemIdentifier);
	if (cs != IS_CONNECTED)
		return false;

	SendUnified(&out, priority, reliability, orderingChannel, systemIdentifier, false);

	returnData->Reset();
	blockingReturnValue.Reset();
	gotBlockingReturnValue = false;
	Packet *packet;
	std::deque<Packet *> packetQueue;
	while (gotBlockingReturnValue == false)
	{
		// TODO - block, filter until gotBlockingReturnValue==true or ID_CONNECTION_LOST or ID_DISCONNECTION_NOTIFICXATION or ID_RPC_REMOTE_ERROR/RPC_ERROR_FUNCTION_NOT_REGISTERED
		RakSleep(30);

		packet = rakPeerInterface->Receive();

		if (packet)
		{
			if (
			    (packet->data[0] == ID_CONNECTION_LOST || packet->data[0] == ID_DISCONNECTION_NOTIFICATION) &&
			    ((systemIdentifier.rakNetGuid != UNASSIGNED_RAKNET_GUID && packet->guid == systemIdentifier.rakNetGuid) ||
			     (systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS && packet->systemAddress == systemIdentifier.systemAddress)))
			{
				// Push back to head in reverse order
				rakPeerInterface->PushBackPacket(packet, true);
				while (!packetQueue.empty())
				{
					rakPeerInterface->PushBackPacket(packetQueue.front(), true);
					packetQueue.pop_front();
				}
				return false;
			}
			else if (packet->data[0] == ID_RPC_REMOTE_ERROR && packet->data[1] == RPC_ERROR_FUNCTION_NOT_REGISTERED)
			{
				MafiaNet::RakString functionName;
				MafiaNet::BitStream bsIn(packet->data, packet->length, false);
				bsIn.IgnoreBytes(2);
				bsIn.Read(functionName);
				if (functionName == uniqueID)
				{
					// Push back to head in reverse order
					rakPeerInterface->PushBackPacket(packet, true);
					while (!packetQueue.empty())
					{
						rakPeerInterface->PushBackPacket(packetQueue.front(), true);
						packetQueue.pop_front();
					}
					return false;
				}
				else
				{
					packetQueue.push_front(packet);
				}
			}
			else
			{
				packetQueue.push_front(packet);
			}
		}
	}

	returnData->Write(blockingReturnValue);
	returnData->ResetReadPointer();
	return true;
}
void RPC4::Signal(const char *sharedIdentifier, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, bool invokeLocal)
{
	MafiaNet::BitStream out;
	out.Write((MessageID) ID_RPC_PLUGIN);
	out.Write((MessageID) ID_RPC4_SIGNAL);
	out.WriteCompressed(sharedIdentifier);
	if (bitStream)
	{
		bitStream->ResetReadPointer();
		out.AlignWriteToByteBoundary();
		out.Write(bitStream);
	}
	SendUnified(&out, priority, reliability, orderingChannel, systemIdentifier, broadcast);

	if (invokeLocal)
	{
		//TimeUS t1 = GetTimeUS();

		LocalSlot *localSlot = GetLocalSlot(sharedIdentifier);
		//TimeUS t2 = GetTimeUS();
		if (localSlot == nullptr)
			return;

		Packet p;
		p.guid = rakPeerInterface->GetMyGUID();
		p.systemAddress = rakPeerInterface->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS);
		p.wasGeneratedLocally = true;
		MafiaNet::BitStream *bsptr, bstemp;
		if (bitStream)
		{
			bitStream->ResetReadPointer();
			p.length = bitStream->GetNumberOfBytesUsed();
			p.bitSize = bitStream->GetNumberOfBitsUsed();
			bsptr = bitStream;
		}
		else
		{
			p.length = 0;
			p.bitSize = 0;
			bsptr = &bstemp;
		}

		//TimeUS t3 = GetTimeUS();
		InvokeSignal(localSlot, bsptr, &p);
		//TimeUS t4 = GetTimeUS();
		//printf("b1: %I64d\n", t2-t1);
		//printf("b2: %I64d\n", t3-t2);
		//printf("b3: %I64d\n", t4-t3);
	}
}
void RPC4::InvokeSignal(LocalSlot *localSlot, MafiaNet::BitStream *serializedParameters, Packet *packet)
{
	if (localSlot == nullptr)
		return;

	//TimeUS t1 = GetTimeUS();
	//TimeUS t2=0;
	//TimeUS t3=0;

	interruptSignal = false;
	size_t i = 0;
	while (i < localSlot->slotObjects.size())
	{
		//t2 = GetTimeUS();

		localSlot->slotObjects[i].functionPointer(serializedParameters, packet, localSlot->slotObjects[i].context);

		//t3 = GetTimeUS();

		// Not threadsafe
		if (interruptSignal == true)
			break;

		serializedParameters->ResetReadPointer();

		i++;
	}

	//TimeUS t4 = GetTimeUS();

	//printf("b1: %I64d\n", t2-t1);
	//printf("b2: %I64d\n", t3-t2);
	//printf("b3: %I64d\n", t4-t3);
}
void RPC4::InterruptSignal(void)
{
	interruptSignal = true;
}
void RPC4::OnAttach(void)
{
	unsigned int i;
	for (i = 0; i < globalRegistrationIndex; i++)
	{
		if (globalRegistrationBuffer[i].registerFunctionPointer)
		{
			if (globalRegistrationBuffer[i].callPriority == (int) 0xFFFFFFFF)
				RegisterFunction(globalRegistrationBuffer[i].functionName, globalRegistrationBuffer[i].registerFunctionPointer, globalRegistrationBuffer[i].context);
			else
				RegisterSlot(globalRegistrationBuffer[i].functionName, globalRegistrationBuffer[i].registerFunctionPointer, globalRegistrationBuffer[i].context, globalRegistrationBuffer[i].callPriority);
		}
		else if (globalRegistrationBuffer[i].registerBlockingFunctionPointer)
			RegisterBlockingFunction(globalRegistrationBuffer[i].functionName, globalRegistrationBuffer[i].registerBlockingFunctionPointer, globalRegistrationBuffer[i].context);
		else
			RegisterLocalCallback(globalRegistrationBuffer[i].functionName, globalRegistrationBuffer[i].messageId);
	}
}
PluginReceiveResult RPC4::OnReceive(Packet *packet)
{
	if (packet->data[0] == ID_RPC_PLUGIN)
	{
		MafiaNet::BitStream bsIn(packet->data, packet->length, false);
		bsIn.IgnoreBytes(2);

		if (packet->data[1] == ID_RPC4_CALL)
		{
			MafiaNet::RakString functionName;
			bsIn.ReadCompressed(functionName);
			bool isBlocking = false;
			bsIn.Read(isBlocking);
			if (isBlocking == false)
			{
				// Look the received name up as is, never through RakString's printf-style constructor.
				const auto function = registeredNonblockingFunctions.find(functionName);
				if (function == registeredNonblockingFunctions.end())
				{
					MafiaNet::BitStream bsOut;
					bsOut.Write((unsigned char) ID_RPC_REMOTE_ERROR);
					bsOut.Write((unsigned char) RPC_ERROR_FUNCTION_NOT_REGISTERED);
					bsOut.Write(functionName.C_String(), (unsigned int) functionName.GetLength() + 1);
					SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
					return RR_STOP_PROCESSING_AND_DEALLOCATE;
				}

				const RegisteredNonblockingFunction rnf = function->second;
				bsIn.AlignReadToByteBoundary();
				rnf.functionPointer(&bsIn, packet, rnf.context);
			}
			else
			{
				const auto function = registeredBlockingFunctions.find(functionName);
				if (function == registeredBlockingFunctions.end())
				{
					MafiaNet::BitStream bsOut;
					bsOut.Write((unsigned char) ID_RPC_REMOTE_ERROR);
					bsOut.Write((unsigned char) RPC_ERROR_FUNCTION_NOT_REGISTERED);
					bsOut.Write(functionName.C_String(), (unsigned int) functionName.GetLength() + 1);
					SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
					return RR_STOP_PROCESSING_AND_DEALLOCATE;
				}

				const RegisteredBlockingFunction rbf = function->second;
				MafiaNet::BitStream returnData;
				bsIn.AlignReadToByteBoundary();
				rbf.functionPointer(&bsIn, &returnData, packet, rbf.context);

				MafiaNet::BitStream out;
				out.Write((MessageID) ID_RPC_PLUGIN);
				out.Write((MessageID) ID_RPC4_RETURN);
				returnData.ResetReadPointer();
				out.AlignWriteToByteBoundary();
				out.Write(returnData);
				SendUnified(&out, MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
			}
		}
		else if (packet->data[1] == ID_RPC4_SIGNAL)
		{
			MafiaNet::RakString sharedIdentifier;
			bsIn.ReadCompressed(sharedIdentifier);
			const auto slot = localSlots.find(sharedIdentifier);
			MafiaNet::BitStream serializedParameters;
			bsIn.AlignReadToByteBoundary();
			bsIn.Read(&serializedParameters);
			if (slot != localSlots.end())
				InvokeSignal(slot->second.get(), &serializedParameters, packet);
		}
		else
		{
			RakAssert(packet->data[1] == ID_RPC4_RETURN);
			blockingReturnValue.Reset();
			blockingReturnValue.Write(bsIn);
			gotBlockingReturnValue = true;
		}

		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	}

	const auto callback = localCallbacks.find(packet->data[0]);
	if (callback != localCallbacks.end())
	{
		LocalCallback &lc = callback->second;
		for (size_t index2 = 0; index2 < lc.functions.size(); index2++)
		{
			MafiaNet::BitStream bsIn(packet->data, packet->length, false);

			const auto function = registeredNonblockingFunctions.find(lc.functions[index2]);
			if (function != registeredNonblockingFunctions.end())
			{
				const RegisteredNonblockingFunction rnf = function->second;
				bsIn.AlignReadToByteBoundary();
				rnf.functionPointer(&bsIn, packet, rnf.context);
			}
		}
	}

	return RR_CONTINUE_PROCESSING;
}
RPC4::LocalSlot *RPC4::GetLocalSlot(const char *sharedIdentifier)
{
	const auto slot = localSlots.find(IdentifierKey(sharedIdentifier));
	return slot == localSlots.end() ? nullptr : slot->second.get();
}

#endif // _RAKNET_SUPPORT_*

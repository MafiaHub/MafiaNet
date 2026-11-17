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

#include "mafianet/remote_procedure_call.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/tcp_interface.h"
#include "mafianet/sleep.h"
#include "mafianet/defines.h"

#include <algorithm>
#include <deque>

using namespace MafiaNet;

/// Slot objects sorted by LocalSlotObjectComp: highest callPriority first, then registration order.
struct RPC::LocalSlot
{
	std::vector<LocalSlotObject> slotObjects;
};

namespace
{
// Registration APIs historically treat a null identifier as the empty name.
const char *IdentifierKey(const char *identifier)
{
	return identifier ? identifier : "";
}
} // namespace


struct GlobalRegistration
{
	void (*registerFunctionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context);
	void (*registerBlockingFunctionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context);
	void *context;
	char functionName[RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH];
	MessageID messageId;
	int callPriority;
};
static GlobalRegistration globalRegistrationBuffer[RPC_GLOBAL_REGISTRATION_MAX_FUNCTIONS];
static unsigned int globalRegistrationIndex = 0;

RPCGlobalRegistration::RPCGlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context)
{
	RakAssert(globalRegistrationIndex != RPC_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
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
RPCGlobalRegistration::RPCGlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context, int callPriority)
{
	RakAssert(globalRegistrationIndex != RPC_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
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
RPCGlobalRegistration::RPCGlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context), void *context)
{
	RakAssert(globalRegistrationIndex != RPC_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
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
RPCGlobalRegistration::RPCGlobalRegistration(const char *uniqueID, MessageID messageId)
{
	RakAssert(globalRegistrationIndex != RPC_GLOBAL_REGISTRATION_MAX_FUNCTIONS);
	unsigned int i;
	for (i = 0; uniqueID[i] && i < RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1; i++)
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

enum RPCIdentifiers
{
	ID_RPC_CALL,
	ID_RPC_RETURN,
	ID_RPC_SIGNAL,
};
int RPC::LocalSlotObjectComp(const LocalSlotObject &key, const LocalSlotObject &data)
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

RPC::RPC(RakPeerInterface &peer)
    : rakPeerInterface(&peer)
{
}
#if _RAKNET_SUPPORT_TCPInterface == 1
RPC::RPC(TCPInterface &tcp)
    : tcpInterface(&tcp)
{
}
#endif
void RPC::Send(const BitStream *data, Priority priority, Reliability reliability, char channel, const AddressOrGUID target, bool broadcast)
{
#if _RAKNET_SUPPORT_TCPInterface == 1
	if (tcpInterface)
	{
		tcpInterface->Send(reinterpret_cast<const char *>(data->GetData()), data->GetNumberOfBytesUsed(), target.systemAddress, broadcast);
		return;
	}
#endif
	rakPeerInterface->Send(data, priority, reliability, channel, target, broadcast);
}
RPC::~RPC() = default;
bool RPC::RegisterFunction(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context)
{
	RegisteredNonblockingFunction rnf;
	rnf.functionPointer = functionPointer;
	rnf.context = context;
	return registeredNonblockingFunctions.emplace(IdentifierKey(uniqueID), rnf).second;
}
void RPC::RegisterSlot(const char *sharedIdentifier, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context, int callPriority)
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
bool RPC::RegisterBlockingFunction(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context), void *context)
{
	RegisteredBlockingFunction rbf;
	rbf.functionPointer = functionPointer;
	rbf.context = context;
	return registeredBlockingFunctions.emplace(IdentifierKey(uniqueID), rbf).second;
}
void RPC::RegisterLocalCallback(const char *uniqueID, MessageID messageId)
{
	const char *name = IdentifierKey(uniqueID);
	LocalCallback &lc = localCallbacks[messageId];
	lc.messageId = messageId;
	const auto position = std::lower_bound(lc.functions.begin(), lc.functions.end(), name);
	if (position == lc.functions.end() || *position != name)
		lc.functions.insert(position, name);
}
bool RPC::UnregisterFunction(const char *uniqueID)
{
	return registeredNonblockingFunctions.erase(IdentifierKey(uniqueID)) != 0;
}
bool RPC::UnregisterBlockingFunction(const char *uniqueID)
{
	return registeredBlockingFunctions.erase(IdentifierKey(uniqueID)) != 0;
}
bool RPC::UnregisterLocalCallback(const char *uniqueID, MessageID messageId)
{
	const char *name = IdentifierKey(uniqueID);
	const auto callback = localCallbacks.find(messageId);
	if (callback == localCallbacks.end())
		return false;
	auto &functions = callback->second.functions;
	const auto position = std::lower_bound(functions.begin(), functions.end(), name);
	if (position == functions.end() || *position != name)
		return false;
	functions.erase(position);
	if (functions.empty())
		localCallbacks.erase(callback);
	return true;
}
bool RPC::UnregisterSlot(const char *sharedIdentifier)
{
	return localSlots.erase(IdentifierKey(sharedIdentifier)) != 0;
}
void RPC::CallLoopback(const char *uniqueID, MafiaNet::BitStream *bitStream)
{
	BitStream out;
	if (registeredNonblockingFunctions.find(IdentifierKey(uniqueID)) == registeredNonblockingFunctions.end())
	{
		out.Write((MessageID) ID_RPC_REMOTE_ERROR);
		out.Write((unsigned char) RPC_ERROR_FUNCTION_NOT_REGISTERED);
		out.Write(uniqueID, (unsigned int) strlen(uniqueID) + 1);
	}
	else
	{
		out.Write((MessageID) ID_RPC);
		out.Write((MessageID) ID_RPC_CALL);
		out.WriteCompressed(uniqueID);
		out.Write(false);
		if (bitStream)
		{
			bitStream->ResetReadPointer();
			out.AlignWriteToByteBoundary();
			out.Write(bitStream);
		}
	}
	const unsigned int size = out.GetNumberOfBytesUsed();
#if _RAKNET_SUPPORT_TCPInterface == 1
	if (tcpInterface)
	{
		Packet *packet = tcpInterface->AllocatePacket(size);
		memcpy(packet->data, out.GetData(), size);
		packet->wasGeneratedLocally = true;
		tcpInterface->PushBackPacket(packet, false);
		return;
	}
#endif
	Packet *packet = rakPeerInterface->AllocatePacket(size);
	memcpy(packet->data, out.GetData(), size);
	packet->guid = rakPeerInterface->GetMyGUID();
	packet->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
	packet->wasGeneratedLocally = true;
	rakPeerInterface->PushBackPacket(packet, false);
}
void RPC::Call(const char *uniqueID, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast)
{
	MafiaNet::BitStream out;
	out.Write((MessageID) ID_RPC);
	out.Write((MessageID) ID_RPC_CALL);
	out.WriteCompressed(uniqueID);
	out.Write(false); // Nonblocking
	if (bitStream)
	{
		bitStream->ResetReadPointer();
		out.AlignWriteToByteBoundary();
		out.Write(bitStream);
	}
	Send(&out, priority, reliability, orderingChannel, systemIdentifier, broadcast);
}
bool RPC::CallBlocking(const char *uniqueID, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, MafiaNet::BitStream *returnData)
{
	MafiaNet::BitStream out;
	out.Write((MessageID) ID_RPC);
	out.Write((MessageID) ID_RPC_CALL);
	out.WriteCompressed(uniqueID);
	out.Write(true); // Blocking
	if (bitStream)
	{
		bitStream->ResetReadPointer();
		out.AlignWriteToByteBoundary();
		out.Write(bitStream);
	}
	if (!returnData || !rakPeerInterface || blockingPeer != UNASSIGNED_RAKNET_GUID ||
	    rakPeerInterface->GetConnectionState(systemIdentifier) != IS_CONNECTED)
		return false;

	blockingPeer = systemIdentifier.rakNetGuid;
	if (blockingPeer == UNASSIGNED_RAKNET_GUID)
		blockingPeer = rakPeerInterface->GetGuidFromSystemAddress(systemIdentifier.systemAddress);
	if (blockingPeer == UNASSIGNED_RAKNET_GUID)
		return false;
	returnData->Reset();
	blockingReturnValue.Reset();
	gotBlockingReturnValue = false;
	Send(&out, priority, reliability, orderingChannel, systemIdentifier, false);

	const size_t nameSize = strlen(uniqueID) + 1;
	std::deque<Packet *> pending;
	while (!gotBlockingReturnValue && rakPeerInterface->GetConnectionState(systemIdentifier) == IS_CONNECTED)
	{
		RakSleep(30);
		Packet *packet = rakPeerInterface->Receive();
		if (!packet)
			continue;
		pending.push_front(packet);
		if (packet->guid != blockingPeer)
			continue;
		if (packet->data[0] == ID_CONNECTION_LOST || packet->data[0] == ID_DISCONNECTION_NOTIFICATION)
			break;
		if (packet->data[0] == ID_RPC_REMOTE_ERROR && packet->length == 2 + nameSize &&
		    packet->data[1] == RPC_ERROR_FUNCTION_NOT_REGISTERED && memcmp(packet->data + 2, uniqueID, nameSize) == 0)
			break;
	}
	blockingPeer = UNASSIGNED_RAKNET_GUID;
	for (Packet *packet : pending)
	{
		if (rakPeerInterface->IsActive())
			rakPeerInterface->PushBackPacket(packet, true);
		else
			rakPeerInterface->DeallocatePacket(packet);
	}
	if (!gotBlockingReturnValue)
		return false;
	returnData->Write(blockingReturnValue);
	returnData->ResetReadPointer();
	return true;
}
void RPC::Signal(const char *sharedIdentifier, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, bool invokeLocal)
{
	MafiaNet::BitStream out;
	out.Write((MessageID) ID_RPC);
	out.Write((MessageID) ID_RPC_SIGNAL);
	out.WriteCompressed(sharedIdentifier);
	if (bitStream)
	{
		bitStream->ResetReadPointer();
		out.AlignWriteToByteBoundary();
		out.Write(bitStream);
	}
	Send(&out, priority, reliability, orderingChannel, systemIdentifier, broadcast);

	if (invokeLocal)
	{
		LocalSlot *localSlot = GetLocalSlot(sharedIdentifier);
		if (localSlot == nullptr)
			return;

		Packet p{};
		p.guid = rakPeerInterface ? rakPeerInterface->GetMyGUID() : UNASSIGNED_RAKNET_GUID;
		p.systemAddress = rakPeerInterface ? rakPeerInterface->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS) : UNASSIGNED_SYSTEM_ADDRESS;
		p.wasGeneratedLocally = true;
		p.data = bitStream ? bitStream->GetData() : nullptr;
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
		InvokeSignal(localSlot, bsptr, &p);
	}
}
void RPC::InvokeSignal(LocalSlot *localSlot, MafiaNet::BitStream *serializedParameters, Packet *packet)
{
	if (localSlot == nullptr)
		return;

	interruptSignal = false;
	size_t i = 0;
	while (i < localSlot->slotObjects.size())
	{
		localSlot->slotObjects[i].functionPointer(serializedParameters, packet, localSlot->slotObjects[i].context);

		// Not threadsafe
		if (interruptSignal == true)
			break;

		serializedParameters->ResetReadPointer();

		i++;
	}
}
void RPC::InterruptSignal(void)
{
	interruptSignal = true;
}
void RPC::RegisterGlobals()
{
	while (appliedGlobalRegistrations < globalRegistrationIndex)
	{
		const auto &registration = globalRegistrationBuffer[appliedGlobalRegistrations++];
		if (registration.registerFunctionPointer)
		{
			if (registration.callPriority == (int) 0xFFFFFFFF)
				RegisterFunction(registration.functionName, registration.registerFunctionPointer, registration.context);
			else
				RegisterSlot(registration.functionName, registration.registerFunctionPointer, registration.context, registration.callPriority);
		}
		else if (registration.registerBlockingFunctionPointer)
			RegisterBlockingFunction(registration.functionName, registration.registerBlockingFunctionPointer, registration.context);
		else
			RegisterLocalCallback(registration.functionName, registration.messageId);
	}
}
bool RPC::ProcessPacket(Packet *packet)
{
	if (packet->length == 0)
		return false;
	if (packet->data[0] == ID_RPC)
	{
		if (packet->length < 2)
			return true; // No sub-identifier: malformed, drop it

		MafiaNet::BitStream bsIn(packet->data, packet->length, false);
		bsIn.IgnoreBytes(2);

		if (packet->data[1] == ID_RPC_CALL)
		{
			MafiaNet::RakString functionName;
			bool isBlocking;
			if (!bsIn.ReadCompressed(functionName) || !bsIn.Read(isBlocking))
				return true;
			if (isBlocking == false)
			{
				const auto function = registeredNonblockingFunctions.find(functionName.C_String());
				if (function == registeredNonblockingFunctions.end())
				{
					MafiaNet::BitStream bsOut;
					bsOut.Write((unsigned char) ID_RPC_REMOTE_ERROR);
					bsOut.Write((unsigned char) RPC_ERROR_FUNCTION_NOT_REGISTERED);
					bsOut.Write(functionName.C_String(), (unsigned int) functionName.GetLength() + 1);
					Send(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
					return true;
				}

				const RegisteredNonblockingFunction rnf = function->second;
				bsIn.AlignReadToByteBoundary();
				rnf.functionPointer(&bsIn, packet, rnf.context);
			}
			else
			{
				const auto function = registeredBlockingFunctions.find(functionName.C_String());
				if (function == registeredBlockingFunctions.end())
				{
					MafiaNet::BitStream bsOut;
					bsOut.Write((unsigned char) ID_RPC_REMOTE_ERROR);
					bsOut.Write((unsigned char) RPC_ERROR_FUNCTION_NOT_REGISTERED);
					bsOut.Write(functionName.C_String(), (unsigned int) functionName.GetLength() + 1);
					Send(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
					return true;
				}

				const RegisteredBlockingFunction rbf = function->second;
				MafiaNet::BitStream returnData;
				bsIn.AlignReadToByteBoundary();
				rbf.functionPointer(&bsIn, &returnData, packet, rbf.context);

				MafiaNet::BitStream out;
				out.Write((MessageID) ID_RPC);
				out.Write((MessageID) ID_RPC_RETURN);
				returnData.ResetReadPointer();
				out.AlignWriteToByteBoundary();
				out.Write(returnData);
				Send(&out, MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
			}
		}
		else if (packet->data[1] == ID_RPC_SIGNAL)
		{
			MafiaNet::RakString sharedIdentifier;
			if (!bsIn.ReadCompressed(sharedIdentifier))
				return true;
			const auto slot = localSlots.find(sharedIdentifier.C_String());
			MafiaNet::BitStream serializedParameters;
			bsIn.AlignReadToByteBoundary();
			bsIn.Read(&serializedParameters);
			if (slot != localSlots.end())
				InvokeSignal(slot->second.get(), &serializedParameters, packet);
		}
		else if (packet->data[1] == ID_RPC_RETURN && blockingPeer != UNASSIGNED_RAKNET_GUID && packet->guid == blockingPeer && !gotBlockingReturnValue)
		{
			blockingReturnValue.Reset();
			blockingReturnValue.Write(bsIn);
			gotBlockingReturnValue = true;
		}

		return true;
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

	return false;
}
RPC::LocalSlot *RPC::GetLocalSlot(const char *sharedIdentifier)
{
	const auto slot = localSlots.find(IdentifierKey(sharedIdentifier));
	return slot == localSlots.end() ? nullptr : slot->second.get();
}

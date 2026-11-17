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

#include "mafianet/variable_delta_serializer.h"
#include "mafianet/vector_util.h"

using namespace MafiaNet;

VariableDeltaSerializer::VariableDeltaSerializer()
{
	didComparisonThisTick = false;
}
VariableDeltaSerializer::~VariableDeltaSerializer()
{
	RemoveRemoteSystemVariableHistory();
}

VariableDeltaSerializer::SerializationContext::SerializationContext()
{
	variableHistoryIdentical = 0;
	variableHistoryUnique = 0;
}
VariableDeltaSerializer::SerializationContext::~SerializationContext() {}

void VariableDeltaSerializer::OnMessageReceipt(RakNetGUID guid, uint32_t receiptId, bool messageArrived)
{
	// Module?
	if (messageArrived)
		FreeVarsAssociatedWithReceipt(guid, receiptId);
	else
		DirtyAndFreeVarsAssociatedWithReceipt(guid, receiptId);
}

void VariableDeltaSerializer::BeginUnreliableAckedSerialize(SerializationContext *context, RakNetGUID _guid, BitStream *_bitStream, uint32_t _sendReceipt)
{
	RakAssert(_guid != UNASSIGNED_RAKNET_GUID);
	context->anyVariablesWritten = false;
	context->guid = _guid;
	context->bitStream = _bitStream;
	if (context->variableHistoryUnique == 0)
		context->variableHistoryUnique = StartVariableHistoryWrite(_guid);
	context->variableHistory = context->variableHistoryUnique;
	context->sendReceipt = _sendReceipt;
	context->changedVariables = AllocChangedVariablesList();
	context->newSystemSend = false;
	context->serializationMode = MafiaNet::Reliability::UnreliableWithAckReceipt;
}

void VariableDeltaSerializer::BeginUniqueSerialize(SerializationContext *context, RakNetGUID _guid, BitStream *_bitStream)
{
	RakAssert(_guid != UNASSIGNED_RAKNET_GUID);
	context->anyVariablesWritten = false;
	context->guid = _guid;
	context->bitStream = _bitStream;
	if (context->variableHistoryUnique == 0)
		context->variableHistoryUnique = StartVariableHistoryWrite(_guid);
	context->variableHistory = context->variableHistoryUnique;
	context->newSystemSend = false;

	context->serializationMode = MafiaNet::Reliability::Reliable;
}


void VariableDeltaSerializer::BeginIdenticalSerialize(SerializationContext *context, bool _isFirstSendToRemoteSystem, BitStream *_bitStream)
{
	context->anyVariablesWritten = false;
	context->guid = UNASSIGNED_RAKNET_GUID;
	context->bitStream = _bitStream;
	context->serializationMode = MafiaNet::Reliability::Reliable;
	if (context->variableHistoryIdentical == 0)
		context->variableHistoryIdentical = StartVariableHistoryWrite(UNASSIGNED_RAKNET_GUID);
	context->variableHistory = context->variableHistoryIdentical;
	context->newSystemSend = _isFirstSendToRemoteSystem;
}

void VariableDeltaSerializer::EndSerialize(SerializationContext *context)
{
	if (context->serializationMode == MafiaNet::Reliability::UnreliableWithAckReceipt)
	{
		if (context->anyVariablesWritten == false)
		{
			context->bitStream->Reset();
			FreeChangedVariablesList(context->changedVariables);
			return;
		}

		StoreChangedVariablesList(context->variableHistory, context->changedVariables, context->sendReceipt);
	}
	else
	{
		if (context->variableHistoryIdentical)
		{
			if (didComparisonThisTick == false)
			{
				didComparisonThisTick = true;
				identicalSerializationBs.Reset();

				if (context->anyVariablesWritten == false)
				{
					context->bitStream->Reset();
					return;
				}

				identicalSerializationBs.Write(context->bitStream);
				context->bitStream->ResetReadPointer();
			}
			else
			{
				context->bitStream->Write(&identicalSerializationBs);
				identicalSerializationBs.ResetReadPointer();
			}
		}
		else if (context->anyVariablesWritten == false)
		{
			context->bitStream->Reset();
			return;
		}
	}
}

void VariableDeltaSerializer::BeginDeserialize(DeserializationContext *context, BitStream *_bitStream)
{
	context->bitStream = _bitStream;
}

void VariableDeltaSerializer::EndDeserialize(DeserializationContext *context)
{
	(void) context;
}

void VariableDeltaSerializer::AddRemoteSystemVariableHistory(RakNetGUID guid)
{
	(void) guid;
}

void VariableDeltaSerializer::RemoveRemoteSystemVariableHistory(RakNetGUID guid)
{
	unsigned int idx;
	idx = GetVarsWrittenPerRemoteSystemListIndex(guid);
	if (idx == (unsigned int) -1)
		return;

	if (remoteSystemVariableHistoryList[idx]->guid == guid)
	{
		for (const auto &entry : remoteSystemVariableHistoryList[idx]->updatedVariablesHistory)
			FreeChangedVariablesList(entry.second);

		delete remoteSystemVariableHistoryList[idx];
		SwapRemove(remoteSystemVariableHistoryList, idx);
		return;
	}
}

void VariableDeltaSerializer::FreeVarsAssociatedWithReceipt(RakNetGUID guid, uint32_t receiptId)
{
	unsigned int idx;
	idx = GetVarsWrittenPerRemoteSystemListIndex(guid);
	if (idx == (unsigned int) -1)
		return;

	RemoteSystemVariableHistory *vprs = remoteSystemVariableHistoryList[idx];
	auto it = vprs->updatedVariablesHistory.find(receiptId);
	if (it != vprs->updatedVariablesHistory.end())
	{
		// Free this history node
		FreeChangedVariablesList(it->second);
		vprs->updatedVariablesHistory.erase(it);
	}
}

void VariableDeltaSerializer::DirtyAndFreeVarsAssociatedWithReceipt(RakNetGUID guid, uint32_t receiptId)
{
	unsigned int idx;
	idx = GetVarsWrittenPerRemoteSystemListIndex(guid);
	if (idx == (unsigned int) -1)
		return;

	RemoteSystemVariableHistory *vprs = remoteSystemVariableHistoryList[idx];
	auto it = vprs->updatedVariablesHistory.find(receiptId);
	if (it != vprs->updatedVariablesHistory.end())
	{
		// 'Dirty' all variables sent this update, meaning they will be resent the next time Serialize() is called
		vprs->variableListDeltaTracker.FlagDirtyFromBitArray(it->second->bitField);

		// Free this history node
		FreeChangedVariablesList(it->second);
		vprs->updatedVariablesHistory.erase(it);
	}
}
unsigned int VariableDeltaSerializer::GetVarsWrittenPerRemoteSystemListIndex(RakNetGUID guid)
{
	unsigned int idx;
	for (idx = 0; idx < remoteSystemVariableHistoryList.size(); idx++)
	{
		if (remoteSystemVariableHistoryList[idx]->guid == guid)
			return idx;
	}
	return (unsigned int) -1;
}
void VariableDeltaSerializer::RemoveRemoteSystemVariableHistory(void)
{
	for (RemoteSystemVariableHistory *history : remoteSystemVariableHistoryList)
	{
		for (const auto &entry : history->updatedVariablesHistory)
			FreeChangedVariablesList(entry.second);

		delete history;
	}
	remoteSystemVariableHistoryList.clear();
}

VariableDeltaSerializer::RemoteSystemVariableHistory *VariableDeltaSerializer::GetRemoteSystemVariableHistory(RakNetGUID guid)
{
	unsigned int rshli = GetRemoteSystemHistoryListIndex(guid);
	return remoteSystemVariableHistoryList[rshli];
}

VariableDeltaSerializer::ChangedVariablesList *VariableDeltaSerializer::AllocChangedVariablesList(void)
{
	VariableDeltaSerializer::ChangedVariablesList *p = new ChangedVariablesList;
	p->bitWriteIndex = 0;
	p->bitField[0] = 0;
	return p;
}
void VariableDeltaSerializer::FreeChangedVariablesList(ChangedVariablesList *changedVariables)
{
	delete changedVariables;
}
void VariableDeltaSerializer::StoreChangedVariablesList(RemoteSystemVariableHistory *variableHistory, ChangedVariablesList *changedVariables, uint32_t sendReceipt)
{
	changedVariables->sendReceipt = sendReceipt;
	// A receipt is stored once; a duplicate is a caller bug. Free it rather than leak it.
	const bool inserted = variableHistory->updatedVariablesHistory.emplace(sendReceipt, changedVariables).second;
	RakAssert(inserted);
	if (!inserted)
		FreeChangedVariablesList(changedVariables);
}

VariableDeltaSerializer::RemoteSystemVariableHistory *VariableDeltaSerializer::StartVariableHistoryWrite(RakNetGUID guid)
{
	RemoteSystemVariableHistory *variableHistory;

	unsigned int rshli = GetRemoteSystemHistoryListIndex(guid);
	if (rshli == (unsigned int) -1)
	{
		variableHistory = new RemoteSystemVariableHistory;
		variableHistory->guid = guid;
		remoteSystemVariableHistoryList.push_back(variableHistory);
	}
	else
	{
		variableHistory = remoteSystemVariableHistoryList[rshli];
	}

	variableHistory->variableListDeltaTracker.StartWrite();
	return variableHistory;
}
unsigned int VariableDeltaSerializer::GetRemoteSystemHistoryListIndex(RakNetGUID guid)
{
	// Find the variable tracker for the target system
	unsigned int idx;
	for (idx = 0; idx < remoteSystemVariableHistoryList.size(); idx++)
	{
		if (remoteSystemVariableHistoryList[idx]->guid == guid)
		{
			return idx;
		}
	}
	return (unsigned int) -1;
}

void VariableDeltaSerializer::OnPreSerializeTick(void)
{
	didComparisonThisTick = false;
}

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
#include <cstring>
#if _RAKNET_SUPPORT_MessageFilter == 1

#include "mafianet/message_filter.h"
#include "mafianet/assert.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/assert.h"
#include "mafianet/peer_interface.h"
#include "mafianet/packetized_tcp.h"
#include "mafianet/bit_stream.h"

#include <algorithm>
#include <utility>

using namespace MafiaNet;

namespace
{
// First filter set whose id is not less than filterSetID.
std::vector<std::unique_ptr<FilterSet>>::iterator LowerBoundFilterSet(std::vector<std::unique_ptr<FilterSet>> &filterList, int filterSetID)
{
	return std::lower_bound(filterList.begin(), filterList.end(), filterSetID, [](const std::unique_ptr<FilterSet> &entry, int id)
	                        { return FilterSetComp(id, entry.get()) > 0; });
}
} // namespace

int MafiaNet::MessageFilterStrComp(char *const &key, char *const &data)
{
	return strcmp(key, data);
}

int MafiaNet::FilterSetComp(const int &key, FilterSet *const &data)
{
	if (key < data->filterSetID)
		return -1;
	else if (key == data->filterSetID)
		return 0;
	else
		return 1;
}
STATIC_FACTORY_DEFINITIONS(MessageFilter, MessageFilter);

MessageFilter::MessageFilter()
{
	autoAddNewConnectionsToFilter = -1; // Documented default; was left uninitialized
	whenLastTimeoutCheck = MafiaNet::GetTime();
}
MessageFilter::~MessageFilter()
{
	Clear();
}
void MessageFilter::SetAutoAddNewConnectionsToFilter(int filterSetID)
{
	autoAddNewConnectionsToFilter = filterSetID;
}
void MessageFilter::SetAllowMessageID(bool allow, int messageIDStart, int messageIDEnd, int filterSetID)
{
	RakAssert(messageIDStart <= messageIDEnd);
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	int i;
	for (i = messageIDStart; i <= messageIDEnd; ++i)
		filterSet->allowedIDs[i] = allow;
}
void MessageFilter::SetAllowRPC(bool allow, const char *uniqueID, int filterSetID)
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	// Verbatim, as RPC stores it: RakString(const char *) would treat uniqueID as a printf format.
	MafiaNet::RakString name;
	name = uniqueID;
	if (allow)
	{
		if (filterSet->allowedRPC.insert(name).second)
			filterSet->allowedIDs[ID_RPC] = true;
	}
	else
	{
		if (filterSet->allowedRPC.erase(name) != 0 && filterSet->allowedRPC.empty())
			filterSet->allowedIDs[ID_RPC] = false;
	}
}
void MessageFilter::SetActionOnDisallowedMessage(bool kickOnDisallowed, bool banOnDisallowed, MafiaNet::TimeMS banTimeMS, int filterSetID)
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->kickOnDisallowedMessage = kickOnDisallowed;
	filterSet->disallowedMessageBanTimeMS = banTimeMS;
	filterSet->banOnDisallowedMessage = banOnDisallowed;
}
void MessageFilter::SetDisallowedMessageCallback(int filterSetID, void *userData, void (*invalidMessageCallback)(RakPeerInterface *peer, AddressOrGUID systemAddress, int filterSetID, void *userData, unsigned char messageID))
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->invalidMessageCallback = invalidMessageCallback;
	filterSet->disallowedCallbackUserData = userData;
}
void MessageFilter::SetTimeoutCallback(int filterSetID, void *userData, void (*invalidMessageCallback)(RakPeerInterface *peer, AddressOrGUID systemAddress, int filterSetID, void *userData))
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->timeoutCallback = invalidMessageCallback;
	filterSet->timeoutUserData = userData;
}
void MessageFilter::SetFilterMaxTime(int allowedTimeMS, bool banOnExceed, MafiaNet::TimeMS banTimeMS, int filterSetID)
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->maxMemberTimeMS = allowedTimeMS;
	filterSet->banOnFilterTimeExceed = banOnExceed;
	filterSet->timeExceedBanTimeMS = banTimeMS;
}
int MessageFilter::GetSystemFilterSet(AddressOrGUID systemAddress)
{
	// 	bool objectExists;
	// 	unsigned index = systemList.GetIndexFromKey(systemAddress, &objectExists);
	// 	if (objectExists==false)
	// 		return -1;
	// 	else
	// 		return systemList[index].filter->filterSetID;

	const auto system = systemList.find(systemAddress);
	if (system == systemList.end())
		return -1;
	else
		return system->second.filter->filterSetID;
}
void MessageFilter::SetSystemFilterSet(AddressOrGUID addressOrGUID, int filterSetID)
{
	// Allocate this filter set if it doesn't exist.
	RakAssert(addressOrGUID.IsUndefined() == false);
	//	bool objectExists;
	// 	unsigned index = systemList.GetIndexFromKey(addressOrGUID, &objectExists);
	// 	if (objectExists==false)
	const auto system = systemList.find(addressOrGUID);
	if (system == systemList.end())
	{
		if (filterSetID < 0)
			return;

		FilteredSystem filteredSystem;
		filteredSystem.filter = GetFilterSetByID(filterSetID);
		//	filteredSystem.addressOrGUID=addressOrGUID;
		filteredSystem.timeEnteredThisSet = MafiaNet::GetTimeMS();
		systemList.emplace(addressOrGUID, filteredSystem);
	}
	else
	{
		if (filterSetID >= 0)
		{
			FilterSet *filterSet = GetFilterSetByID(filterSetID);
			system->second.timeEnteredThisSet = MafiaNet::GetTimeMS();
			system->second.filter = filterSet;
		}
		else
		{
			systemList.erase(system);
		}
	}
}
unsigned MessageFilter::GetSystemCount(int filterSetID) const
{
	if (filterSetID == -1)
	{
		return (unsigned) systemList.size();
	}
	else
	{
		unsigned count = 0;
		for (const auto &system : systemList)
			if (system.second.filter->filterSetID == filterSetID)
				++count;
		return count;
	}
}
unsigned MessageFilter::GetFilterSetCount(void) const
{
	return (unsigned) filterList.size();
}
int MessageFilter::GetFilterSetIDByIndex(unsigned index)
{
	return filterList[index]->filterSetID;
}
void MessageFilter::DeleteFilterSet(int filterSetID)
{
	const auto position = LowerBoundFilterSet(filterList, filterSetID);
	if (position != filterList.end() && (*position)->filterSetID == filterSetID)
	{
		const FilterSet *filterSet = position->get();

		// Systems in the set become unrestricted; drop them before the set is freed.
		for (auto system = systemList.begin(); system != systemList.end();)
		{
			if (system->second.filter == filterSet)
				system = systemList.erase(system);
			else
				++system;
		}

		DeallocateFilterSet(position->release());
		filterList.erase(position);

		/*
		// Don't reference this pointer any longer
		i=0;
		while (i < systemList.Size())
		{
			if (systemList[i].filter==filterSet)
				systemList.RemoveAtIndex(i);
			else
				++i;
		}
		*/
	}
}
void MessageFilter::Clear(void)
{
	systemList.clear();
	for (std::unique_ptr<FilterSet> &filterSet : filterList)
		DeallocateFilterSet(filterSet.release());
	filterList.clear();
}
void MessageFilter::DeallocateFilterSet(FilterSet *filterSet)
{
	delete filterSet;
}
FilterSet *MessageFilter::GetFilterSetByID(int filterSetID)
{
	RakAssert(filterSetID >= 0);
	const auto position = LowerBoundFilterSet(filterList, filterSetID);
	if (position != filterList.end() && (*position)->filterSetID == filterSetID)
		return position->get();
	else
	{
		FilterSet *newFilterSet = new FilterSet;
		memset(newFilterSet->allowedIDs, 0, MESSAGE_FILTER_MAX_MESSAGE_ID * sizeof(bool));
		newFilterSet->banOnFilterTimeExceed = false;
		newFilterSet->kickOnDisallowedMessage = false;
		newFilterSet->banOnDisallowedMessage = false;
		newFilterSet->disallowedMessageBanTimeMS = 0;
		newFilterSet->timeExceedBanTimeMS = 0;
		newFilterSet->maxMemberTimeMS = 0;
		newFilterSet->filterSetID = filterSetID;
		newFilterSet->invalidMessageCallback = 0;
		newFilterSet->timeoutCallback = 0;
		newFilterSet->timeoutUserData = 0;
		filterList.emplace(position, newFilterSet);
		return newFilterSet;
	}
}
void MessageFilter::OnInvalidMessage(FilterSet *filterSet, AddressOrGUID systemAddress, unsigned char messageID)
{
	if (filterSet->invalidMessageCallback)
		filterSet->invalidMessageCallback(rakPeerInterface, systemAddress, filterSet->filterSetID, filterSet->disallowedCallbackUserData, messageID);
	if (filterSet->banOnDisallowedMessage && rakPeerInterface)
	{
		char str1[64];
		systemAddress.systemAddress.ToString(false, str1, static_cast<size_t>(64));
		rakPeerInterface->AddToBanList(str1, filterSet->disallowedMessageBanTimeMS);
	}
	if (filterSet->kickOnDisallowedMessage)
	{
		if (rakPeerInterface)
			rakPeerInterface->CloseConnection(systemAddress, true, 0);
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
		else
			tcpInterface->CloseConnection(systemAddress.systemAddress);
#endif
	}
}
void MessageFilter::Update(void)
{
	// Update all timers for all systems.  If those systems' filter sets are expired, take the appropriate action.
	MafiaNet::Time curTime = MafiaNet::GetTime();
	if (GreaterThan(curTime - 1000, whenLastTimeoutCheck))
	{
		// A snapshot: the callbacks and CloseConnection below may change systemList.
		const std::vector<std::pair<AddressOrGUID, FilteredSystem>> snapshot(systemList.begin(), systemList.end());

		for (const auto &entry : snapshot)
		{
			const AddressOrGUID &key = entry.first;
			const FilteredSystem &item = entry.second;
			if (item.filter &&
			    item.filter->maxMemberTimeMS > 0 &&
			    curTime - item.timeEnteredThisSet >= item.filter->maxMemberTimeMS)
			{
				if (item.filter->timeoutCallback)
					item.filter->timeoutCallback(rakPeerInterface, key, item.filter->filterSetID, item.filter->timeoutUserData);

				if (item.filter->banOnFilterTimeExceed && rakPeerInterface)
				{
					char str1[64];
					key.ToString(false, str1, sizeof(str1));
					rakPeerInterface->AddToBanList(str1, item.filter->timeExceedBanTimeMS);
				}
				if (rakPeerInterface)
					rakPeerInterface->CloseConnection(key, true, 0);
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
				else
					tcpInterface->CloseConnection(key.systemAddress);
#endif

				systemList.erase(key);
			}
		}

		whenLastTimeoutCheck = curTime + 1000;
	}
}
void MessageFilter::OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming)
{
	(void) systemAddress;
	(void) rakNetGUID;
	(void) isIncoming;

	AddressOrGUID aog;
	aog.rakNetGuid = rakNetGUID;
	aog.systemAddress = systemAddress;

	// New system, automatically assign to filter set if appropriate
	if (autoAddNewConnectionsToFilter >= 0 && systemList.count(aog) == 0)
		SetSystemFilterSet(aog, autoAddNewConnectionsToFilter);
}
void MessageFilter::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) rakNetGUID;
	(void) lostConnectionReason;

	AddressOrGUID aog;
	aog.rakNetGuid = rakNetGUID;
	aog.systemAddress = systemAddress;

	// Lost system, remove from the list
	systemList.erase(aog);
}
PluginReceiveResult MessageFilter::OnReceive(Packet *packet)
{
	unsigned char messageId;

	switch (packet->data[0])
	{
	case ID_NEW_INCOMING_CONNECTION:
	case ID_CONNECTION_REQUEST_ACCEPTED:
	case ID_CONNECTION_LOST:
	case ID_DISCONNECTION_NOTIFICATION:
	case ID_CONNECTION_ATTEMPT_FAILED:
	case ID_NO_FREE_INCOMING_CONNECTIONS:
	case ID_IP_RECENTLY_CONNECTED:
	case ID_CONNECTION_BANNED:
	case ID_INVALID_PASSWORD:
	case ID_UNCONNECTED_PONG:
	case ID_ALREADY_CONNECTED:
	case ID_ADVERTISE_SYSTEM:
	case ID_REMOTE_DISCONNECTION_NOTIFICATION:
	case ID_REMOTE_CONNECTION_LOST:
	case ID_REMOTE_NEW_INCOMING_CONNECTION:
	case ID_DOWNLOAD_PROGRESS:
		break;
	default:
		if (packet->data[0] == ID_TIMESTAMP)
		{
			if (packet->length < sizeof(MessageID) + sizeof(MafiaNet::TimeMS))
				return RR_STOP_PROCESSING_AND_DEALLOCATE; // Invalid message
			messageId = packet->data[sizeof(MessageID) + sizeof(MafiaNet::TimeMS)];
		}
		else
			messageId = packet->data[0];
		// If this system is filtered, check if this message is allowed.  If not allowed, return RR_STOP_PROCESSING_AND_DEALLOCATE
		// index = systemList.GetIndexFromKey(packet->addressOrGUID, &objectExists);
		const auto system = systemList.find(packet);
		if (system == systemList.end())
			break;
		FilterSet *filter = system->second.filter;
		if (filter->allowedIDs[messageId] == false)
		{
			OnInvalidMessage(filter, packet, packet->data[0]);
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
		if (packet->data[0] == ID_RPC)
		{
			MafiaNet::BitStream bsIn(packet->data, packet->length, false);
			bsIn.IgnoreBytes(2);
			MafiaNet::RakString functionName;
			if (packet->length < 2 || !bsIn.ReadCompressed(functionName) || filter->allowedRPC.count(functionName) == 0)
			{
				OnInvalidMessage(filter, packet, packet->data[0]);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			}
		}

		break;
	}

	return RR_CONTINUE_PROCESSING;
}

#endif // _RAKNET_SUPPORT_*

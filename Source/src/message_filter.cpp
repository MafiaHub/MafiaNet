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
#if _RAKNET_SUPPORT_MessageFilter==1

#include "mafianet/message_filter.h"
#include <algorithm>
#include "mafianet/assert.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/assert.h"
#include "mafianet/peer_interface.h"
#include "mafianet/packetized_tcp.h"
#include "mafianet/bit_stream.h"

using namespace MafiaNet;

int MafiaNet::MessageFilterStrComp( char *const &key,char *const &data )
{
	return strcmp(key,data);
}

int MafiaNet::FilterSetComp( const int &key, FilterSet * const &data )
{
	if (key < data->filterSetID)
		return -1;
	else if (key==data->filterSetID)
		return 0;
	else
		return 1;
}
STATIC_FACTORY_DEFINITIONS(MessageFilter,MessageFilter);

MessageFilter::MessageFilter()
{
		whenLastTimeoutCheck= MafiaNet::GetTime();
}
MessageFilter::~MessageFilter()
{
	Clear();
}
unsigned int MessageFilter::IndexForFilterSetID(int filterSetID, bool *found) const
{
	// filterList is sorted ascending by filterSetID; GetFilterSetIDByIndex publishes the index.
	std::vector<FilterSet *>::const_iterator it = std::lower_bound(
		filterList.begin(), filterList.end(), filterSetID,
		[](FilterSet * const &candidate, int key)
		{
			return candidate->filterSetID < key;
		});
	unsigned int i = (unsigned int) (it - filterList.begin());
	*found = (it != filterList.end() && (*it)->filterSetID == filterSetID);
	return i;
}
void MessageFilter::SetAutoAddNewConnectionsToFilter(int filterSetID)
{
	autoAddNewConnectionsToFilter=filterSetID;
}
void MessageFilter::SetAllowMessageID(bool allow, int messageIDStart, int messageIDEnd,int filterSetID)
{
	RakAssert(messageIDStart <= messageIDEnd);
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	int i;
	for (i=messageIDStart; i <= messageIDEnd; ++i)
		filterSet->allowedIDs[i]=allow;
}
void MessageFilter::SetAllowRPC4(bool allow, const char* uniqueID, int filterSetID)
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	bool objectExists;
	unsigned int idx = filterSet->allowedRPC4.GetIndexFromKey(uniqueID, &objectExists);
	if (allow)
	{
		if (objectExists==false)
		{
			filterSet->allowedRPC4.InsertAtIndex(uniqueID, idx, _FILE_AND_LINE_);
			filterSet->allowedIDs[ID_RPC_PLUGIN]=true;
		}
	}
	else
	{
		if (objectExists==true)
		{
			filterSet->allowedRPC4.RemoveAtIndex(idx);
			if (filterSet->allowedRPC4.Size()==0)
			{
				filterSet->allowedIDs[ID_RPC_PLUGIN]=false;
			}
		}
	}
}
void MessageFilter::SetActionOnDisallowedMessage(bool kickOnDisallowed, bool banOnDisallowed, MafiaNet::TimeMS banTimeMS, int filterSetID)
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->kickOnDisallowedMessage=kickOnDisallowed;
	filterSet->disallowedMessageBanTimeMS=banTimeMS;
	filterSet->banOnDisallowedMessage=banOnDisallowed;
}
void MessageFilter::SetDisallowedMessageCallback(int filterSetID, void *userData, void (*invalidMessageCallback)(RakPeerInterface *peer, AddressOrGUID systemAddress, int filterSetID, void *userData, unsigned char messageID))
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->invalidMessageCallback=invalidMessageCallback;
	filterSet->disallowedCallbackUserData=userData;
}
void MessageFilter::SetTimeoutCallback(int filterSetID, void *userData, void (*invalidMessageCallback)(RakPeerInterface *peer, AddressOrGUID systemAddress, int filterSetID, void *userData))
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->timeoutCallback=invalidMessageCallback;
	filterSet->timeoutUserData=userData;
}
void MessageFilter::SetFilterMaxTime(int allowedTimeMS, bool banOnExceed, MafiaNet::TimeMS banTimeMS, int filterSetID)
{
	FilterSet *filterSet = GetFilterSetByID(filterSetID);
	filterSet->maxMemberTimeMS=allowedTimeMS;
	filterSet->banOnFilterTimeExceed=banOnExceed;
	filterSet->timeExceedBanTimeMS=banTimeMS;
}
int MessageFilter::GetSystemFilterSet(AddressOrGUID systemAddress)
{
// 	bool objectExists;
// 	unsigned index = systemList.GetIndexFromKey(systemAddress, &objectExists);
// 	if (objectExists==false)
// 		return -1;
// 	else
// 		return systemList[index].filter->filterSetID;

	std::unordered_map<AddressOrGUID, FilteredSystem, AddressOrGUIDKeyHash>::const_iterator it =
		systemList.find(systemAddress);
	if (it == systemList.end())
		return -1;
	else
		return it->second.filter->filterSetID;
}
void MessageFilter::SetSystemFilterSet(AddressOrGUID addressOrGUID, int filterSetID)
{
	// Allocate this filter set if it doesn't exist.
	RakAssert(addressOrGUID.IsUndefined()==false);
//	bool objectExists;
// 	unsigned index = systemList.GetIndexFromKey(addressOrGUID, &objectExists);
// 	if (objectExists==false)
	std::unordered_map<AddressOrGUID, FilteredSystem, AddressOrGUIDKeyHash>::iterator index =
		systemList.find(addressOrGUID);
	if (index == systemList.end())
	{
		if (filterSetID<0)
			return;

		FilteredSystem filteredSystem;
		filteredSystem.filter = GetFilterSetByID(filterSetID);
	//	filteredSystem.addressOrGUID=addressOrGUID;
		filteredSystem.timeEnteredThisSet= MafiaNet::GetTimeMS();
	//	systemList.Insert(addressOrGUID, filteredSystem, true, _FILE_AND_LINE_);
		systemList.insert(std::make_pair(addressOrGUID, filteredSystem));
	}
	else
	{
		if (filterSetID>=0)
		{
			FilterSet *filterSet = GetFilterSetByID(filterSetID);
			index->second.timeEnteredThisSet= MafiaNet::GetTimeMS();
			index->second.filter=filterSet;
		}
		else
		{
			systemList.erase(index);
		}
	}	
}
unsigned MessageFilter::GetSystemCount(int filterSetID) const
{
	if (filterSetID==-1)
	{
		return systemList.size();
	}
	else
	{
		unsigned count=0;
		for (std::unordered_map<AddressOrGUID, FilteredSystem, AddressOrGUIDKeyHash>::const_iterator
			it = systemList.begin(); it != systemList.end(); ++it)
			if (it->second.filter->filterSetID==filterSetID)
				++count;
		return count;
	}
}
unsigned MessageFilter::GetFilterSetCount(void) const
{
	return filterList.size();
}
int MessageFilter::GetFilterSetIDByIndex(unsigned index)
{
	return filterList[index]->filterSetID;
}
void MessageFilter::DeleteFilterSet(int filterSetID)
{
	FilterSet *filterSet;
	bool objectExists;
	unsigned i,index;
	index = IndexForFilterSetID(filterSetID, &objectExists);
	if (objectExists)
	{
		filterSet=filterList[index];
		DeallocateFilterSet(filterSet);
		filterList.erase(filterList.begin() + index);

		for (std::unordered_map<AddressOrGUID, FilteredSystem, AddressOrGUIDKeyHash>::iterator
			it = systemList.begin(); it != systemList.end(); )
		{
			if (it->second.filter==filterSet)
				it = systemList.erase(it);
			else
				++it;
		}

		/*
		// Don't reference this pointer any longer
		i=0;
		while (i < systemList.size())
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
	unsigned i;
	systemList.clear();
	for (i=0; i < filterList.size(); i++)
		DeallocateFilterSet(filterList[i]);
	filterList.clear();
}
void MessageFilter::DeallocateFilterSet(FilterSet* filterSet)
{
	MafiaNet::OP_DELETE(filterSet, _FILE_AND_LINE_);
}
FilterSet* MessageFilter::GetFilterSetByID(int filterSetID)
{
	RakAssert(filterSetID>=0);
	bool objectExists;
	unsigned index;
	index = IndexForFilterSetID(filterSetID, &objectExists);
	if (objectExists)
		return filterList[index];
	else
	{
		FilterSet *newFilterSet = MafiaNet::OP_NEW<FilterSet>( _FILE_AND_LINE_ );
		memset(newFilterSet->allowedIDs, 0, MESSAGE_FILTER_MAX_MESSAGE_ID * sizeof(bool));
		newFilterSet->banOnFilterTimeExceed=false;
		newFilterSet->kickOnDisallowedMessage=false;
		newFilterSet->banOnDisallowedMessage=false;
		newFilterSet->disallowedMessageBanTimeMS=0;
		newFilterSet->timeExceedBanTimeMS=0;
		newFilterSet->maxMemberTimeMS=0;
		newFilterSet->filterSetID=filterSetID;
		newFilterSet->invalidMessageCallback=0;
		newFilterSet->timeoutCallback=0;
		newFilterSet->timeoutUserData=0;
		filterList.insert(filterList.begin() + index, newFilterSet);
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
#if _RAKNET_SUPPORT_PacketizedTCP==1 && _RAKNET_SUPPORT_TCPInterface==1
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
		// A snapshot, because the callbacks below can close connections and so reach back into
		// systemList; the DataStructures::List copy this replaces had the same effect.
		std::vector<FilteredSystem> itemList;
		std::vector<AddressOrGUID> keyList;
		itemList.reserve(systemList.size());
		keyList.reserve(systemList.size());
		for (std::unordered_map<AddressOrGUID, FilteredSystem, AddressOrGUIDKeyHash>::const_iterator
			it = systemList.begin(); it != systemList.end(); ++it)
		{
			itemList.push_back(it->second);
			keyList.push_back(it->first);
		}

		unsigned int index;
		for (index=0; index < itemList.size(); index++)
		{
			if (itemList[index].filter &&
				itemList[index].filter->maxMemberTimeMS>0 &&
				curTime-itemList[index].timeEnteredThisSet >= itemList[index].filter->maxMemberTimeMS)
			{
				if (itemList[index].filter->timeoutCallback)
					itemList[index].filter->timeoutCallback(rakPeerInterface, keyList[index], itemList[index].filter->filterSetID, itemList[index].filter->timeoutUserData);

				if (itemList[index].filter->banOnFilterTimeExceed && rakPeerInterface)
				{
					char str1[64];
					keyList[index].ToString(false, str1, 64);
					rakPeerInterface->AddToBanList(str1, itemList[index].filter->timeExceedBanTimeMS);
				}
				if (rakPeerInterface)
					rakPeerInterface->CloseConnection(keyList[index], true, 0);
#if _RAKNET_SUPPORT_PacketizedTCP==1 && _RAKNET_SUPPORT_TCPInterface==1
				else
					tcpInterface->CloseConnection(keyList[index].systemAddress);
#endif

				systemList.erase(keyList[index]);
			}
		}

		whenLastTimeoutCheck=curTime+1000;
	}
}
void MessageFilter::OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming)
{
	(void) systemAddress;
	(void) rakNetGUID;
	(void) isIncoming;

	AddressOrGUID aog;
	aog.rakNetGuid=rakNetGUID;
	aog.systemAddress=systemAddress;

	// New system, automatically assign to filter set if appropriate
	if (autoAddNewConnectionsToFilter>=0 && systemList.find(aog)==systemList.end())
		SetSystemFilterSet(aog, autoAddNewConnectionsToFilter);
}
void MessageFilter::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
	(void) rakNetGUID;
	(void) lostConnectionReason;

	AddressOrGUID aog;
	aog.rakNetGuid=rakNetGUID;
	aog.systemAddress=systemAddress;

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
		if (packet->data[0]==ID_TIMESTAMP)
		{
			if (packet->length<sizeof(MessageID) + sizeof(MafiaNet::TimeMS))
				return RR_STOP_PROCESSING_AND_DEALLOCATE; // Invalid message
			messageId=packet->data[sizeof(MessageID) + sizeof(MafiaNet::TimeMS)];
		}
		else
			messageId=packet->data[0];
		// If this system is filtered, check if this message is allowed.  If not allowed, return RR_STOP_PROCESSING_AND_DEALLOCATE
		// index = systemList.GetIndexFromKey(packet->addressOrGUID, &objectExists);
		std::unordered_map<AddressOrGUID, FilteredSystem, AddressOrGUIDKeyHash>::const_iterator sysIt =
			systemList.find(packet);
		if (sysIt == systemList.end())
			break;
		if (sysIt->second.filter->allowedIDs[messageId]==false)
		{
			OnInvalidMessage(sysIt->second.filter, packet, packet->data[0]);
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
		if (packet->data[0]==ID_RPC_PLUGIN)
		{
			MafiaNet::BitStream bsIn(packet->data,packet->length,false);
			bsIn.IgnoreBytes(2);
			MafiaNet::RakString functionName;
			bsIn.ReadCompressed(functionName);
			if (sysIt->second.filter->allowedRPC4.HasData(functionName)==false)
			{
				OnInvalidMessage(sysIt->second.filter, packet, packet->data[0]);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			}
		}
		
		break;
	}
	
	return RR_CONTINUE_PROCESSING;
}

#endif // _RAKNET_SUPPORT_*

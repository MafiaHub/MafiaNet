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
#include <cstdlib>
#if _RAKNET_SUPPORT_CloudServer == 1

#include "mafianet/cloud_server.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"
#include "mafianet/peer_interface.h"

#include <algorithm>

enum ServerToServerCommands
{
	STSC_PROCESS_GET_REQUEST,
	STSC_PROCESS_GET_RESPONSE,
	STSC_ADD_UPLOADED_AND_SUBSCRIBED_KEYS,
	STSC_ADD_UPLOADED_KEY,
	STSC_ADD_SUBSCRIBED_KEY,
	STSC_REMOVE_UPLOADED_KEY,
	STSC_REMOVE_SUBSCRIBED_KEY,
	STSC_DATA_CHANGED,
};

using namespace MafiaNet;

namespace
{
// OrderedList::GetIndexFromKey over a vector kept sorted by one of the 3-way comparators below
// (negative when \a key sorts before the element): the index of the first element not before
// \a key, and whether that element matches it.
template <class T, class Key, class Comp>
unsigned int SortedIndex(const std::vector<T> &list, const Key &key, Comp comp, bool *exists)
{
	const auto it = std::lower_bound(list.begin(), list.end(), key, [&comp](const T &element, const Key &k)
	                                 { return comp(k, element) > 0; });
	*exists = it != list.end() && comp(key, *it) == 0;
	return static_cast<unsigned int>(it - list.begin());
}
// OrderedList::Insert(key, data, true): adds \a data at its sorted position unless \a key is already there.
template <class T, class Key, class Comp>
void SortedInsert(std::vector<T> &list, const Key &key, const T &data, Comp comp)
{
	bool exists;
	const unsigned int index = SortedIndex(list, key, comp, &exists);
	if (!exists)
		list.insert(list.begin() + index, data);
}
} // namespace

int CloudServer::RemoteServerComp(const RakNetGUID &key, RemoteServer *const &data)
{
	if (key < data->serverAddress)
		return -1;
	if (key > data->serverAddress)
		return 1;
	return 0;
}
int CloudServer::KeySubscriberIDComp(const CloudKey &key, KeySubscriberID *const &data)
{
	if (key.primaryKey < data->key.primaryKey)
		return -1;
	if (key.primaryKey > data->key.primaryKey)
		return 1;
	if (key.secondaryKey < data->key.secondaryKey)
		return -1;
	if (key.secondaryKey > data->key.secondaryKey)
		return 1;
	return 0;
}
int CloudServer::KeyDataPtrComp(const RakNetGUID &key, CloudData *const &data)
{
	if (key < data->clientGUID)
		return -1;
	if (key > data->clientGUID)
		return 1;
	return 0;
}
int CloudServer::KeyDataListComp(const CloudKey &key, CloudDataList *const &data)
{
	if (key.primaryKey < data->key.primaryKey)
		return -1;
	if (key.primaryKey > data->key.primaryKey)
		return 1;
	if (key.secondaryKey < data->key.secondaryKey)
		return -1;
	if (key.secondaryKey > data->key.secondaryKey)
		return 1;
	return 0;
}
int CloudServer::BufferedGetResponseFromServerComp(const RakNetGUID &key, CloudServer::BufferedGetResponseFromServer *const &data)
{
	if (key < data->serverAddress)
		return -1;
	if (key > data->serverAddress)
		return 1;
	return 0;
}
void CloudServer::CloudQueryWithAddresses::Serialize(bool writeToBitstream, BitStream *bitStream)
{
	cloudQuery.Serialize(writeToBitstream, bitStream);

	if (writeToBitstream)
	{
		bitStream->WriteCasted<uint16_t>(specificSystems.size());
		RakAssert(specificSystems.size() < (uint16_t) -1);
		for (uint16_t i = 0; i < specificSystems.size(); i++)
		{
			bitStream->Write(specificSystems[i]);
		}
	}
	else
	{
		uint16_t specificSystemsCount;
		RakNetGUID addressOrGuid;
		bitStream->Read(specificSystemsCount);
		for (uint16_t i = 0; i < specificSystemsCount; i++)
		{
			bitStream->Read(addressOrGuid);
			specificSystems.push_back(addressOrGuid);
		}
	}
}
bool CloudServer::GetRequest::AllRemoteServersHaveResponded(void) const
{
	for (const BufferedGetResponseFromServer *response : remoteServerResponses)
		if (response->gotResult == false)
			return false;
	return true;
}
void CloudServer::GetRequest::Clear(CloudAllocator *allocator)
{
	for (BufferedGetResponseFromServer *response : remoteServerResponses)
	{
		response->Clear(allocator);
		delete response;
	}
	remoteServerResponses.clear();
}
void CloudServer::BufferedGetResponseFromServer::Clear(CloudAllocator *allocator)
{
	for (CloudQueryRow *row : queryResult.rowsReturned)
	{
		allocator->DeallocateRowData(row->data);
		allocator->DeallocateCloudQueryRow(row);
	}
	queryResult.rowsReturned.clear();
}
CloudServer::CloudServer()
{
	maxUploadBytesPerClient = 0;
	maxBytesPerDowload = 0;
	nextGetRequestId = 0;
	nextGetRequestsCheck = 0;
}
CloudServer::~CloudServer()
{
	Clear();
}
void CloudServer::SetMaxUploadBytesPerClient(uint64_t bytes)
{
	maxUploadBytesPerClient = bytes;
}
void CloudServer::SetMaxBytesPerDownload(uint64_t bytes)
{
	maxBytesPerDowload = bytes;
}
void CloudServer::Update(void)
{
	// Timeout getRequests
	MafiaNet::Time time = MafiaNet::Time();
	if (time > nextGetRequestsCheck)
	{
		nextGetRequestsCheck = time + 1000;

		auto it = getRequests.begin();
		while (it != getRequests.end())
		{
			if (time - it->second->requestStartTime > 3000)
			{
				// Remote server is not responding, just send back data with whoever did respond
				ProcessAndTransmitGetRequest(it->second);
				it->second->Clear(this);
				delete it->second;
				it = getRequests.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}
PluginReceiveResult CloudServer::OnReceive(Packet *packet)
{
	switch (packet->data[0])
	{
	case ID_CLOUD_POST_REQUEST:
		OnPostRequest(packet);
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	case ID_CLOUD_RELEASE_REQUEST:
		OnReleaseRequest(packet);
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	case ID_CLOUD_GET_REQUEST:
		OnGetRequest(packet);
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	case ID_CLOUD_UNSUBSCRIBE_REQUEST:
		OnUnsubscribeRequest(packet);
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	case ID_CLOUD_SERVER_TO_SERVER_COMMAND:
		if (packet->length > 1)
		{
			switch (packet->data[1])
			{
			case STSC_PROCESS_GET_REQUEST:
				OnServerToServerGetRequest(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case STSC_PROCESS_GET_RESPONSE:
				OnServerToServerGetResponse(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case STSC_ADD_UPLOADED_AND_SUBSCRIBED_KEYS:
				OnSendUploadedAndSubscribedKeysToServer(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case STSC_ADD_UPLOADED_KEY:
				OnSendUploadedKeyToServers(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case STSC_ADD_SUBSCRIBED_KEY:
				OnSendSubscribedKeyToServers(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case STSC_REMOVE_UPLOADED_KEY:
				OnRemoveUploadedKeyFromServers(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case STSC_REMOVE_SUBSCRIBED_KEY:
				OnRemoveSubscribedKeyFromServers(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case STSC_DATA_CHANGED:
				OnServerDataChanged(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			}
		}
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	}
	return RR_CONTINUE_PROCESSING;
}
void CloudServer::OnPostRequest(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID));
	CloudKey key;
	key.Serialize(false, &bsIn);
	uint32_t dataLengthBytes;
	bsIn.Read(dataLengthBytes);
	if (maxUploadBytesPerClient > 0 && dataLengthBytes > maxUploadBytesPerClient)
		return; // Exceeded max upload bytes

	bsIn.AlignReadToByteBoundary();
	for (unsigned int filterIndex = 0; filterIndex < queryFilters.size(); filterIndex++)
	{
		if (queryFilters[filterIndex]->OnPostRequest(packet->guid, packet->systemAddress, key, dataLengthBytes, (const char *) bsIn.GetData() + BITS_TO_BYTES(bsIn.GetReadOffset())) == false)
			return;
	}

	unsigned char *data;
	if (dataLengthBytes > CLOUD_SERVER_DATA_STACK_SIZE)
	{
		data = (unsigned char *) std::malloc(dataLengthBytes);
		if (data == 0)
		{
			return;
		}
		bsIn.ReadAlignedBytes(data, dataLengthBytes);
	}
	else
		data = 0;

	// Add this system to remoteSystems if they aren't there already
	const auto remoteSystemIt = remoteSystems.find(packet->guid);
	RemoteCloudClient *remoteCloudClient;
	if (remoteSystemIt == remoteSystems.end())
	{
		remoteCloudClient = new RemoteCloudClient;
		remoteCloudClient->uploadedKeys.insert(key);
		remoteCloudClient->uploadedBytes = 0;
		remoteSystems[packet->guid] = remoteCloudClient;
	}
	else
	{
		remoteCloudClient = remoteSystemIt->second;
		// Add to RemoteCloudClient::uploadedKeys if it isn't there already
		remoteCloudClient->uploadedKeys.insert(key);
	}

	bool cloudDataAlreadyUploaded;
	unsigned int dataRepositoryIndex;
	bool dataRepositoryExists;
	CloudDataList *cloudDataList = GetOrAllocateCloudDataList(key, &dataRepositoryExists, dataRepositoryIndex);
	if (dataRepositoryExists == false)
	{
		cloudDataList->uploaderCount = 1;
		cloudDataAlreadyUploaded = false;
	}
	else
	{
		cloudDataAlreadyUploaded = cloudDataList->uploaderCount > 0;
		cloudDataList->uploaderCount++;
	}

	CloudData *cloudData;
	bool keyDataListExists;
	unsigned int keyDataListIndex = SortedIndex(cloudDataList->keyData, packet->guid, KeyDataPtrComp, &keyDataListExists);
	if (keyDataListExists == false)
	{
		if (maxUploadBytesPerClient > 0 && remoteCloudClient->uploadedBytes + dataLengthBytes > maxUploadBytesPerClient)
		{
			// Undo prior insertion of cloudDataList into dataRepository if needed. (This tested
			// keyDataListExists, always false here, and so also dropped a list other systems use.)
			if (dataRepositoryExists == false)
			{
				delete cloudDataList;
				dataRepository.erase(dataRepository.begin() + dataRepositoryIndex);
			}
			else
				cloudDataList->uploaderCount--; // counted above, before the limit check

			// This system had no data under the key, so it was only added to uploadedKeys above.
			remoteCloudClient->uploadedKeys.erase(key);
			if (remoteCloudClient->IsUnused())
			{
				delete remoteCloudClient;
				remoteSystems.erase(packet->guid);
			}

			if (dataLengthBytes > CLOUD_SERVER_DATA_STACK_SIZE)
				std::free(data);

			return;
		}

		cloudData = new CloudData;
		cloudData->dataLengthBytes = dataLengthBytes;
		cloudData->isUploaded = true;
		if (forceAddress != UNASSIGNED_SYSTEM_ADDRESS)
		{
			cloudData->serverSystemAddress = forceAddress;
			cloudData->serverSystemAddress.SetPortHostOrder(rakPeerInterface->GetExternalID(packet->systemAddress).GetPort());
		}
		else
		{
			cloudData->serverSystemAddress = rakPeerInterface->GetExternalID(packet->systemAddress);
			if (cloudData->serverSystemAddress.IsLoopback())
				cloudData->serverSystemAddress.FromString(rakPeerInterface->GetLocalIP(0));
		}
		if (cloudData->serverSystemAddress.GetPort() == 0)
		{
			// Fix localhost port
			cloudData->serverSystemAddress.SetPortHostOrder(rakPeerInterface->GetSocket(UNASSIGNED_SYSTEM_ADDRESS)->GetBoundAddress().GetPort());
		}
		cloudData->clientSystemAddress = packet->systemAddress;
		cloudData->serverGUID = rakPeerInterface->GetMyGUID();
		cloudData->clientGUID = packet->guid;
		SortedInsert(cloudDataList->keyData, packet->guid, cloudData, KeyDataPtrComp);
	}
	else
	{
		cloudData = cloudDataList->keyData[keyDataListIndex];

		if (cloudDataAlreadyUploaded == false)
		{
			if (forceAddress != UNASSIGNED_SYSTEM_ADDRESS)
			{
				cloudData->serverSystemAddress = forceAddress;
				cloudData->serverSystemAddress.SetPortHostOrder(rakPeerInterface->GetExternalID(packet->systemAddress).GetPort());
			}
			else
			{
				cloudData->serverSystemAddress = rakPeerInterface->GetExternalID(packet->systemAddress);
			}
			if (cloudData->serverSystemAddress.GetPort() == 0)
			{
				// Fix localhost port
				cloudData->serverSystemAddress.SetPortHostOrder(rakPeerInterface->GetSocket(UNASSIGNED_SYSTEM_ADDRESS)->GetBoundAddress().GetPort());
			}

			cloudData->clientSystemAddress = packet->systemAddress;
		}

		if (maxUploadBytesPerClient > 0 && remoteCloudClient->uploadedBytes - cloudData->dataLengthBytes + dataLengthBytes > maxUploadBytesPerClient)
		{
			// Undo prior insertion of cloudDataList into cloudData if needed
			if (dataRepositoryExists == false)
			{
				delete cloudDataList;
				dataRepository.erase(dataRepository.begin() + dataRepositoryIndex);
			}
			else
				cloudDataList->uploaderCount--; // counted above, before the limit check
			// A subscription placeholder (never uploaded by this system) was only added to uploadedKeys above.
			if (cloudData->isUploaded == false)
			{
				remoteCloudClient->uploadedKeys.erase(key);
				if (remoteCloudClient->IsUnused())
				{
					delete remoteCloudClient;
					remoteSystems.erase(packet->guid);
				}
			}
			if (dataLengthBytes > CLOUD_SERVER_DATA_STACK_SIZE)
				std::free(data);
			return;
		}
		else
		{
			// Subtract already used bytes we are overwriting
			remoteCloudClient->uploadedBytes -= cloudData->dataLengthBytes;
		}

		if (cloudData->allocatedData != 0)
			std::free(cloudData->allocatedData);
	}

	if (dataLengthBytes > CLOUD_SERVER_DATA_STACK_SIZE)
	{
		// Data already allocated
		cloudData->allocatedData = data;
		cloudData->dataPtr = data;
	}
	else
	{
		// Read to stack
		if (dataLengthBytes > 0)
			bsIn.ReadAlignedBytes(cloudData->stackData, dataLengthBytes);
		cloudData->allocatedData = 0;
		cloudData->dataPtr = cloudData->stackData;
	}
	// Update how many bytes were written for this data
	cloudData->dataLengthBytes = dataLengthBytes;
	remoteCloudClient->uploadedBytes += dataLengthBytes;

	if (cloudDataAlreadyUploaded == false)
	{
		// New data field
		SendUploadedKeyToServers(cloudDataList->key);
	}

	// Existing data field changed
	NotifyClientSubscribersOfDataChange(cloudData, cloudDataList->key, cloudData->specificSubscribers, true);
	NotifyClientSubscribersOfDataChange(cloudData, cloudDataList->key, cloudDataList->nonSpecificSubscribers, true);

	// Send update to all remote servers that subscribed to this key
	NotifyServerSubscribersOfDataChange(cloudData, cloudDataList->key, true);

	// I could have also subscribed to a key not yet updated locally
	// This means I have to go through every RemoteClient that wants this key
	// Seems like cloudData->specificSubscribers is unnecessary in that case
}
void CloudServer::OnReleaseRequest(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID));

	uint16_t keyCount;
	bsIn.Read(keyCount);

	if (keyCount == 0)
		return;

	const auto remoteSystemIt = remoteSystems.find(packet->guid);
	if (remoteSystemIt == remoteSystems.end())
		return;

	RemoteCloudClient *remoteCloudClient = remoteSystemIt->second;

	CloudKey key;

	// Read all in a list first so I can run filter on it
	std::vector<CloudKey> cloudKeys;
	for (uint16_t keyCountIndex = 0; keyCountIndex < keyCount; keyCountIndex++)
	{
		key.Serialize(false, &bsIn);
		cloudKeys.push_back(key);
	}

	for (unsigned int filterIndex = 0; filterIndex < queryFilters.size(); filterIndex++)
	{
		if (queryFilters[filterIndex]->OnReleaseRequest(packet->guid, packet->systemAddress, cloudKeys) == false)
			return;
	}

	// A filter may have edited the list
	for (size_t keyCountIndex = 0; keyCountIndex < cloudKeys.size(); keyCountIndex++)
	{
		// Serialize in list above so I can run the filter on it
		// key.Serialize(false, &bsIn);
		key = cloudKeys[keyCountIndex];

		// Remove remote systems uploaded keys
		const auto uploadedKey = remoteCloudClient->uploadedKeys.find(key);
		if (uploadedKey != remoteCloudClient->uploadedKeys.end())
		{
			bool dataRepositoryExists;
			unsigned int dataRepositoryIndex = SortedIndex(dataRepository, key, KeyDataListComp, &dataRepositoryExists);
			if (dataRepositoryExists == false)
				continue;
			CloudDataList *cloudDataList = dataRepository[dataRepositoryIndex];

			CloudData *cloudData;
			bool keyDataListExists;
			unsigned int keyDataListIndex = SortedIndex(cloudDataList->keyData, packet->guid, KeyDataPtrComp, &keyDataListExists);
			if (keyDataListExists == false)
				continue;
			cloudData = cloudDataList->keyData[keyDataListIndex];

			remoteCloudClient->uploadedKeys.erase(uploadedKey);
			remoteCloudClient->uploadedBytes -= cloudData->dataLengthBytes;
			cloudDataList->uploaderCount--;

			// Broadcast destruction of this key to subscribers
			NotifyClientSubscribersOfDataChange(cloudData, cloudDataList->key, cloudData->specificSubscribers, false);
			NotifyClientSubscribersOfDataChange(cloudData, cloudDataList->key, cloudDataList->nonSpecificSubscribers, false);
			NotifyServerSubscribersOfDataChange(cloudData, cloudDataList->key, false);

			cloudData->Clear();

			if (cloudData->IsUnused())
			{
				delete cloudData;
				cloudDataList->keyData.erase(cloudDataList->keyData.begin() + keyDataListIndex);
				if (cloudDataList->IsNotUploaded())
				{
					// Tell other servers that this key is no longer uploaded, so they do not request it from us
					RemoveUploadedKeyFromServers(cloudDataList->key);
				}

				if (cloudDataList->IsUnused())
				{
					delete cloudDataList;
					dataRepository.erase(dataRepository.begin() + dataRepositoryIndex);
				}
			}

			if (remoteCloudClient->IsUnused())
			{
				delete remoteCloudClient;
				remoteSystems.erase(remoteSystemIt);
				break;
			}
		}
	}
}
void CloudServer::OnGetRequest(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID));
	uint16_t specificSystemsCount;
	CloudKey cloudKey;

	// Create a new GetRequest
	GetRequest *getRequest;
	getRequest = new GetRequest;
	getRequest->cloudQueryWithAddresses.cloudQuery.Serialize(false, &bsIn);
	getRequest->requestingClient = packet->guid;

	RakNetGUID addressOrGuid;
	bsIn.Read(specificSystemsCount);
	for (uint16_t i = 0; i < specificSystemsCount; i++)
	{
		bsIn.Read(addressOrGuid);
		getRequest->cloudQueryWithAddresses.specificSystems.push_back(addressOrGuid);
	}

	if (getRequest->cloudQueryWithAddresses.cloudQuery.keys.empty())
	{
		delete getRequest;
		return;
	}

	for (unsigned int filterIndex = 0; filterIndex < queryFilters.size(); filterIndex++)
	{
		if (queryFilters[filterIndex]->OnGetRequest(packet->guid, packet->systemAddress, getRequest->cloudQueryWithAddresses.cloudQuery, getRequest->cloudQueryWithAddresses.specificSystems) == false)
		{
			delete getRequest;
			return;
		}
	}

	getRequest->requestStartTime = MafiaNet::GetTime();
	getRequest->requestId = nextGetRequestId++;

	// Send request to servers that have this data
	std::vector<RemoteServer *> remoteServersWithData;
	GetServersWithUploadedKeys(getRequest->cloudQueryWithAddresses.cloudQuery.keys, remoteServersWithData);

	if (remoteServersWithData.empty())
	{
		ProcessAndTransmitGetRequest(getRequest);
	}
	else
	{
		MafiaNet::BitStream bsOut;
		bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
		bsOut.Write((MessageID) STSC_PROCESS_GET_REQUEST);
		getRequest->cloudQueryWithAddresses.Serialize(true, &bsOut);
		bsOut.Write(getRequest->requestId);

		for (size_t remoteServerIndex = 0; remoteServerIndex < remoteServersWithData.size(); remoteServerIndex++)
		{
			BufferedGetResponseFromServer *bufferedGetResponseFromServer = new BufferedGetResponseFromServer;
			bufferedGetResponseFromServer->serverAddress = remoteServersWithData[remoteServerIndex]->serverAddress;
			bufferedGetResponseFromServer->gotResult = false;
			SortedInsert(getRequest->remoteServerResponses, remoteServersWithData[remoteServerIndex]->serverAddress, bufferedGetResponseFromServer, BufferedGetResponseFromServerComp);

			SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, remoteServersWithData[remoteServerIndex]->serverAddress, false);
		}

		// Record that this system made this request
		getRequests[getRequest->requestId] = getRequest;
	}

	if (getRequest->cloudQueryWithAddresses.cloudQuery.subscribeToResults)
	{
		// Add to key subscription list for the client, which contains a keyId / specificUploaderList pair
		const auto remoteSystemIt = remoteSystems.find(packet->guid);
		RemoteCloudClient *remoteCloudClient;
		if (remoteSystemIt == remoteSystems.end())
		{
			remoteCloudClient = new RemoteCloudClient;
			remoteCloudClient->uploadedBytes = 0;
			remoteSystems[packet->guid] = remoteCloudClient;
		}
		else
		{
			remoteCloudClient = remoteSystemIt->second;
		}

		size_t keyIndex;
		for (keyIndex = 0; keyIndex < getRequest->cloudQueryWithAddresses.cloudQuery.keys.size(); keyIndex++)
		{
			cloudKey = getRequest->cloudQueryWithAddresses.cloudQuery.keys[keyIndex];

			unsigned int keySubscriberIndex;
			bool hasKeySubscriber;
			keySubscriberIndex = SortedIndex(remoteCloudClient->subscribedKeys, cloudKey, KeySubscriberIDComp, &hasKeySubscriber);
			KeySubscriberID *keySubscriberId;
			if (hasKeySubscriber)
			{
				std::vector<RakNetGUID> specificSystems;
				UnsubscribeFromKey(remoteCloudClient, packet->guid, keySubscriberIndex, cloudKey, specificSystems);
				// The unsubscribe may have removed the entry (it did unless specific systems remain).
				keySubscriberIndex = SortedIndex(remoteCloudClient->subscribedKeys, cloudKey, KeySubscriberIDComp, &hasKeySubscriber);
				if (hasKeySubscriber)
				{
					delete remoteCloudClient->subscribedKeys[keySubscriberIndex];
					remoteCloudClient->subscribedKeys.erase(remoteCloudClient->subscribedKeys.begin() + keySubscriberIndex);
				}
			}

			keySubscriberId = new KeySubscriberID;
			keySubscriberId->key = cloudKey;

			size_t specificSystemIndex;
			for (specificSystemIndex = 0; specificSystemIndex < getRequest->cloudQueryWithAddresses.specificSystems.size(); specificSystemIndex++)
			{
				keySubscriberId->specificSystemsSubscribedTo.insert(getRequest->cloudQueryWithAddresses.specificSystems[specificSystemIndex]);
			}

			remoteCloudClient->subscribedKeys.insert(remoteCloudClient->subscribedKeys.begin() + keySubscriberIndex, keySubscriberId);

			// Add CloudData in a similar way
			unsigned int dataRepositoryIndex;
			bool dataRepositoryExists;
			CloudDataList *cloudDataList = GetOrAllocateCloudDataList(cloudKey, &dataRepositoryExists, dataRepositoryIndex);

			// If this is the first local client to subscribe to this key, call SendSubscribedKeyToServers
			if (cloudDataList->subscriberCount == 0)
				SendSubscribedKeyToServers(cloudKey);

			// If the subscription is specific, may have to also allocate CloudData
			if (!getRequest->cloudQueryWithAddresses.specificSystems.empty())
			{
				CloudData *cloudData;
				bool keyDataListExists;

				for (specificSystemIndex = 0; specificSystemIndex < getRequest->cloudQueryWithAddresses.specificSystems.size(); specificSystemIndex++)
				{
					RakNetGUID specificSystem = getRequest->cloudQueryWithAddresses.specificSystems[specificSystemIndex];

					unsigned int keyDataListIndex = SortedIndex(cloudDataList->keyData, specificSystem, KeyDataPtrComp, &keyDataListExists);
					if (keyDataListExists == false)
					{
						cloudData = new CloudData;
						cloudData->dataLengthBytes = 0;
						cloudData->allocatedData = 0;
						cloudData->isUploaded = false;
						cloudData->dataPtr = 0;
						cloudData->serverSystemAddress = UNASSIGNED_SYSTEM_ADDRESS;
						cloudData->clientSystemAddress = UNASSIGNED_SYSTEM_ADDRESS;
						cloudData->serverGUID = rakPeerInterface->GetMyGUID();
						cloudData->clientGUID = specificSystem;
						SortedInsert(cloudDataList->keyData, specificSystem, cloudData, KeyDataPtrComp);
					}
					else
					{
						cloudData = cloudDataList->keyData[keyDataListIndex];
					}

					++cloudDataList->subscriberCount;
					cloudData->specificSubscribers.insert(packet->guid);
				}
			}
			else
			{
				++cloudDataList->subscriberCount;
				cloudDataList->nonSpecificSubscribers.insert(packet->guid);

				// Remove packet->guid from CloudData::specificSubscribers among all instances of cloudDataList->keyData
				unsigned int subscribedKeysIndex;
				bool subscribedKeysIndexExists;
				subscribedKeysIndex = SortedIndex(remoteCloudClient->subscribedKeys, cloudDataList->key, KeySubscriberIDComp, &subscribedKeysIndexExists);
				if (subscribedKeysIndexExists)
				{
					keySubscriberId = remoteCloudClient->subscribedKeys[subscribedKeysIndex];
					for (const RakNetGUID &subscribedTo : keySubscriberId->specificSystemsSubscribedTo)
					{
						bool keyDataExists;
						unsigned int keyDataIndex = SortedIndex(cloudDataList->keyData, subscribedTo, KeyDataPtrComp, &keyDataExists);
						if (keyDataExists)
						{
							CloudData *keyData = cloudDataList->keyData[keyDataIndex];
							keyData->specificSubscribers.erase(packet->guid);
							--cloudDataList->subscriberCount;
						}
					}
				}
			}
		}

		if (remoteCloudClient->IsUnused())
		{
			// Didn't do anything
			remoteSystems.erase(packet->guid);
			delete remoteCloudClient;
		}
	}

	if (remoteServersWithData.empty())
		delete getRequest;
}
void CloudServer::OnUnsubscribeRequest(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID));

	const auto remoteSystemIt = remoteSystems.find(packet->guid);
	if (remoteSystemIt == remoteSystems.end())
		return;

	RemoteCloudClient *remoteCloudClient = remoteSystemIt->second;

	uint16_t keyCount, specificSystemCount;
	std::vector<CloudKey> cloudKeys;
	std::vector<RakNetGUID> specificSystems;
	uint16_t index;

	CloudKey cloudKey;
	bsIn.Read(keyCount);
	for (index = 0; index < keyCount; index++)
	{
		cloudKey.Serialize(false, &bsIn);
		cloudKeys.push_back(cloudKey);
	}

	RakNetGUID specificSystem;
	bsIn.Read(specificSystemCount);
	for (index = 0; index < specificSystemCount; index++)
	{
		bsIn.Read(specificSystem);
		specificSystems.push_back(specificSystem);
	}

	for (unsigned int filterIndex = 0; filterIndex < queryFilters.size(); filterIndex++)
	{
		if (queryFilters[filterIndex]->OnUnsubscribeRequest(packet->guid, packet->systemAddress, cloudKeys, specificSystems) == false)
			return;
	}

	//	CloudDataList *cloudDataList;
	bool dataRepositoryExists;
	//	unsigned int dataRepositoryIndex;

	// A filter may have edited the list
	for (size_t keyIndex = 0; keyIndex < cloudKeys.size(); keyIndex++)
	{
		cloudKey = cloudKeys[keyIndex];

		//	dataRepositoryIndex =
		SortedIndex(dataRepository, cloudKey, KeyDataListComp, &dataRepositoryExists);
		if (dataRepositoryExists == false)
			continue;
		//		cloudDataList = dataRepository[dataRepositoryIndex];

		unsigned int keySubscriberIndex;
		bool hasKeySubscriber;
		keySubscriberIndex = SortedIndex(remoteCloudClient->subscribedKeys, cloudKey, KeySubscriberIDComp, &hasKeySubscriber);

		if (hasKeySubscriber == false)
			continue;

		UnsubscribeFromKey(remoteCloudClient, packet->guid, keySubscriberIndex, cloudKey, specificSystems);
	}

	if (remoteCloudClient->IsUnused())
	{
		delete remoteCloudClient;
		remoteSystems.erase(remoteSystemIt);
	}
}
void CloudServer::OnServerToServerGetRequest(Packet *packet)
{
	//	unsigned int remoteServerIndex;
	bool objectExists;
	//remoteServerIndex =
	SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;

	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	CloudQueryWithAddresses cloudQueryWithAddresses;
	uint32_t requestId;
	cloudQueryWithAddresses.Serialize(false, &bsIn);
	bsIn.Read(requestId);

	std::vector<CloudData *> cloudDataResultList;
	std::vector<CloudKey> cloudKeyResultList;
	ProcessCloudQueryWithAddresses(cloudQueryWithAddresses, cloudDataResultList, cloudKeyResultList);

	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
	bsOut.Write((MessageID) STSC_PROCESS_GET_RESPONSE);
	bsOut.Write(requestId);
	WriteCloudQueryRowFromResultList(cloudDataResultList, cloudKeyResultList, &bsOut);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->guid, false);
}
void CloudServer::OnServerToServerGetResponse(Packet *packet)
{
	unsigned int remoteServerIndex;
	bool objectExists;
	remoteServerIndex = SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;

	RemoteServer *remoteServer = remoteServers[remoteServerIndex];
	if (remoteServer == 0)
		return;

	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	uint32_t requestId;
	bsIn.Read(requestId);

	// Lookup request id
	const auto getRequestIt = getRequests.find(requestId);
	if (getRequestIt == getRequests.end())
		return;
	GetRequest *getRequest = getRequestIt->second;
	bool hasRemoteServer;
	unsigned int remoteServerResponsesIndex;
	remoteServerResponsesIndex = SortedIndex(getRequest->remoteServerResponses, packet->guid, BufferedGetResponseFromServerComp, &hasRemoteServer);
	if (hasRemoteServer == false)
		return;
	BufferedGetResponseFromServer *bufferedGetResponseFromServer;
	bufferedGetResponseFromServer = getRequest->remoteServerResponses[remoteServerResponsesIndex];
	if (bufferedGetResponseFromServer->gotResult == true)
		return;
	bufferedGetResponseFromServer->gotResult = true;
	uint32_t numRows;
	bufferedGetResponseFromServer->queryResult.SerializeNumRows(false, numRows, &bsIn);
	bufferedGetResponseFromServer->queryResult.SerializeCloudQueryRows(false, numRows, &bsIn, this);

	// If all results returned, then also process locally, and return to user
	if (getRequest->AllRemoteServersHaveResponded())
	{
		ProcessAndTransmitGetRequest(getRequest);

		getRequest->Clear(this);
		delete getRequest;

		getRequests.erase(getRequestIt);
	}
}
void CloudServer::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) systemAddress;

	unsigned int remoteServerIndex;
	bool objectExists;
	remoteServerIndex = SortedIndex(remoteServers, rakNetGUID, RemoteServerComp, &objectExists);
	if (objectExists)
	{
		// Update remoteServerResponses by removing this server and sending the response if it is now complete
		auto getRequestIt = getRequests.begin();
		while (getRequestIt != getRequests.end())
		{
			GetRequest *getRequest = getRequestIt->second;
			bool waitingForThisServer;
			unsigned int remoteServerResponsesIndex = SortedIndex(getRequest->remoteServerResponses, rakNetGUID, BufferedGetResponseFromServerComp, &waitingForThisServer);
			if (waitingForThisServer)
			{
				getRequest->remoteServerResponses[remoteServerResponsesIndex]->Clear(this);
				delete getRequest->remoteServerResponses[remoteServerResponsesIndex];
				getRequest->remoteServerResponses.erase(getRequest->remoteServerResponses.begin() + remoteServerResponsesIndex);

				if (getRequest->AllRemoteServersHaveResponded())
				{
					ProcessAndTransmitGetRequest(getRequest);
					getRequest->Clear(this);
					delete getRequest;

					getRequestIt = getRequests.erase(getRequestIt);
				}
				else
					++getRequestIt;
			}
			else
				++getRequestIt;
		}

		delete remoteServers[remoteServerIndex];
		remoteServers.erase(remoteServers.begin() + remoteServerIndex);
	}

	const auto remoteSystemIt = remoteSystems.find(rakNetGUID);
	if (remoteSystemIt != remoteSystems.end())
	{
		RemoteCloudClient *remoteCloudClient = remoteSystemIt->second;
		for (const CloudKey &uploadedKey : remoteCloudClient->uploadedKeys)
		{
			// Delete keys this system has uploaded
			bool keyDataRepositoryExists;
			unsigned int dataRepositoryIndex = SortedIndex(dataRepository, uploadedKey, KeyDataListComp, &keyDataRepositoryExists);
			if (keyDataRepositoryExists)
			{
				CloudDataList *cloudDataList = dataRepository[dataRepositoryIndex];
				bool keyDataExists;
				unsigned int keyDataIndex = SortedIndex(cloudDataList->keyData, rakNetGUID, KeyDataPtrComp, &keyDataExists);
				if (keyDataExists)
				{
					CloudData *cloudData = cloudDataList->keyData[keyDataIndex];
					cloudDataList->uploaderCount--;

					NotifyClientSubscribersOfDataChange(cloudData, cloudDataList->key, cloudData->specificSubscribers, false);
					NotifyClientSubscribersOfDataChange(cloudData, cloudDataList->key, cloudDataList->nonSpecificSubscribers, false);
					NotifyServerSubscribersOfDataChange(cloudData, cloudDataList->key, false);

					cloudData->Clear();

					if (cloudData->IsUnused())
					{
						delete cloudData;
						cloudDataList->keyData.erase(cloudDataList->keyData.begin() + keyDataIndex);

						if (cloudDataList->IsNotUploaded())
						{
							// Tell other servers that this key is no longer uploaded, so they do not request it from us
							RemoveUploadedKeyFromServers(cloudDataList->key);
						}

						if (cloudDataList->IsUnused())
						{
							// Tell other servers that this key is no longer uploaded, so they do not request it from us
							RemoveUploadedKeyFromServers(cloudDataList->key);

							delete cloudDataList;
							dataRepository.erase(dataRepository.begin() + dataRepositoryIndex);
						}
					}
				}
			}
		}

		for (KeySubscriberID *keySubscriberId : remoteCloudClient->subscribedKeys)
		{
			bool keyDataRepositoryExists;
			unsigned int keyDataRepositoryIndex = SortedIndex(dataRepository, keySubscriberId->key, KeyDataListComp, &keyDataRepositoryExists);
			if (keyDataRepositoryExists)
			{
				CloudDataList *cloudDataList = dataRepository[keyDataRepositoryIndex];
				if (keySubscriberId->specificSystemsSubscribedTo.empty())
				{
					cloudDataList->nonSpecificSubscribers.erase(rakNetGUID);
					--cloudDataList->subscriberCount;
				}
				else
				{
					for (const RakNetGUID &subscribedTo : keySubscriberId->specificSystemsSubscribedTo)
					{
						bool keyDataExists;
						unsigned int keyDataIndex = SortedIndex(cloudDataList->keyData, subscribedTo, KeyDataPtrComp, &keyDataExists);
						if (keyDataExists)
						{
							CloudData *keyData = cloudDataList->keyData[keyDataIndex];
							keyData->specificSubscribers.erase(rakNetGUID);
							--cloudDataList->subscriberCount;
						}
					}
				}
			}

			delete keySubscriberId;
		}

		// Delete and remove from remoteSystems
		delete remoteCloudClient;
		remoteSystems.erase(remoteSystemIt);
	}
}
void CloudServer::OnRakPeerShutdown(void)
{
	Clear();
}
void CloudServer::Clear(void)
{
	for (CloudDataList *cloudDataList : dataRepository)
	{
		for (CloudData *cloudData : cloudDataList->keyData)
		{
			cloudData->Clear();
			delete cloudData;
		}
		delete cloudDataList;
	}
	dataRepository.clear();

	for (RemoteServer *remoteServer : remoteServers)
	{
		delete remoteServer;
	}
	remoteServers.clear();

	for (auto &entry : getRequests)
	{
		entry.second->Clear(this);
		delete entry.second;
	}
	getRequests.clear();

	for (auto &entry : remoteSystems)
	{
		RemoteCloudClient *remoteCloudClient = entry.second;
		for (KeySubscriberID *keySubscriberId : remoteCloudClient->subscribedKeys)
		{
			delete keySubscriberId;
		}
		delete remoteCloudClient;
	}
	remoteSystems.clear();
}
void CloudServer::WriteCloudQueryRowFromResultList(std::vector<CloudData *> &cloudDataResultList, std::vector<CloudKey> &cloudKeyResultList, BitStream *bsOut)
{
	bsOut->WriteCasted<uint32_t>(cloudKeyResultList.size());
	unsigned int i;
	for (i = 0; i < cloudKeyResultList.size(); i++)
	{
		WriteCloudQueryRowFromResultList(i, cloudDataResultList, cloudKeyResultList, bsOut);
	}
}
void CloudServer::WriteCloudQueryRowFromResultList(unsigned int i, std::vector<CloudData *> &cloudDataResultList, std::vector<CloudKey> &cloudKeyResultList, BitStream *bsOut)
{
	CloudQueryRow cloudQueryRow;
	CloudData *cloudData = cloudDataResultList[i];
	cloudQueryRow.key = cloudKeyResultList[i];
	cloudQueryRow.data = cloudData->dataPtr;
	cloudQueryRow.length = cloudData->dataLengthBytes;
	cloudQueryRow.serverSystemAddress = cloudData->serverSystemAddress;
	cloudQueryRow.clientSystemAddress = cloudData->clientSystemAddress;
	cloudQueryRow.serverGUID = cloudData->serverGUID;
	cloudQueryRow.clientGUID = cloudData->clientGUID;
	cloudQueryRow.Serialize(true, bsOut, 0);
}
void CloudServer::NotifyClientSubscribersOfDataChange(CloudData *cloudData, CloudKey &key, const std::set<RakNetGUID> &subscribers, bool wasUpdated)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SUBSCRIPTION_NOTIFICATION);
	bsOut.Write(wasUpdated);
	CloudQueryRow row;
	row.key = key;
	row.data = cloudData->dataPtr;
	row.length = cloudData->dataLengthBytes;
	row.serverSystemAddress = cloudData->serverSystemAddress;
	row.clientSystemAddress = cloudData->clientSystemAddress;
	row.serverGUID = cloudData->serverGUID;
	row.clientGUID = cloudData->clientGUID;
	row.Serialize(true, &bsOut, 0);

	for (const RakNetGUID &subscriber : subscribers)
	{
		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, subscriber, false);
	}
}
void CloudServer::NotifyClientSubscribersOfDataChange(CloudQueryRow *row, const std::set<RakNetGUID> &subscribers, bool wasUpdated)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SUBSCRIPTION_NOTIFICATION);
	bsOut.Write(wasUpdated);
	row->Serialize(true, &bsOut, 0);

	for (const RakNetGUID &subscriber : subscribers)
	{
		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, subscriber, false);
	}
}
void CloudServer::NotifyServerSubscribersOfDataChange(CloudData *cloudData, CloudKey &key, bool wasUpdated)
{
	// Find every server that has subscribed
	// Send them change notifications
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
	bsOut.Write((MessageID) STSC_DATA_CHANGED);
	bsOut.Write(wasUpdated);
	CloudQueryRow row;
	row.key = key;
	row.data = cloudData->dataPtr;
	row.length = cloudData->dataLengthBytes;
	row.serverSystemAddress = cloudData->serverSystemAddress;
	row.clientSystemAddress = cloudData->clientSystemAddress;
	row.serverGUID = cloudData->serverGUID;
	row.clientGUID = cloudData->clientGUID;
	row.Serialize(true, &bsOut, 0);

	for (RemoteServer *remoteServer : remoteServers)
	{
		if (remoteServer->gotSubscribedAndUploadedKeys == false || remoteServer->subscribedKeys.count(key) != 0)
		{
			SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, remoteServer->serverAddress, false);
		}
	}
}
void CloudServer::AddServer(RakNetGUID systemIdentifier)
{
	ConnectionState cs = rakPeerInterface->GetConnectionState(systemIdentifier);
	if (cs == IS_DISCONNECTED || cs == IS_NOT_CONNECTED)
		return;
	bool objectExists;
	unsigned int index = SortedIndex(remoteServers, systemIdentifier, RemoteServerComp, &objectExists);
	if (objectExists == false)
	{
		RemoteServer *remoteServer = new RemoteServer;
		remoteServer->gotSubscribedAndUploadedKeys = false;
		remoteServer->serverAddress = systemIdentifier;
		remoteServers.insert(remoteServers.begin() + index, remoteServer);

		SendUploadedAndSubscribedKeysToServer(systemIdentifier);
	}
}
void CloudServer::RemoveServer(RakNetGUID systemAddress)
{
	bool objectExists;
	unsigned int index = SortedIndex(remoteServers, systemAddress, RemoteServerComp, &objectExists);
	if (objectExists == true)
	{
		delete remoteServers[index];
		remoteServers.erase(remoteServers.begin() + index);
	}
}
void CloudServer::GetRemoteServers(std::vector<RakNetGUID> &remoteServersOut)
{
	remoteServersOut.clear();

	for (const RemoteServer *remoteServer : remoteServers)
	{
		remoteServersOut.push_back(remoteServer->serverAddress);
	}
}
void CloudServer::ProcessAndTransmitGetRequest(GetRequest *getRequest)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_GET_RESPONSE);

	//	BufferedGetResponseFromServer getResponse;
	CloudQueryResult cloudQueryResult;
	cloudQueryResult.cloudQuery = getRequest->cloudQueryWithAddresses.cloudQuery;
	cloudQueryResult.subscribeToResults = getRequest->cloudQueryWithAddresses.cloudQuery.subscribeToResults;
	cloudQueryResult.SerializeHeader(true, &bsOut);

	std::vector<CloudData *> cloudDataResultList;
	std::vector<CloudKey> cloudKeyResultList;
	ProcessCloudQueryWithAddresses(getRequest->cloudQueryWithAddresses, cloudDataResultList, cloudKeyResultList);
	bool unlimitedRows = getRequest->cloudQueryWithAddresses.cloudQuery.maxRowsToReturn == 0;

	uint32_t localNumRows = (uint32_t) cloudDataResultList.size();
	if (unlimitedRows == false &&
	    localNumRows > getRequest->cloudQueryWithAddresses.cloudQuery.startingRowIndex &&
	    localNumRows - getRequest->cloudQueryWithAddresses.cloudQuery.startingRowIndex > getRequest->cloudQueryWithAddresses.cloudQuery.maxRowsToReturn)
		localNumRows = getRequest->cloudQueryWithAddresses.cloudQuery.startingRowIndex + getRequest->cloudQueryWithAddresses.cloudQuery.maxRowsToReturn;

	BitSize_t bitStreamOffset = bsOut.GetWriteOffset();
	uint32_t localRowsToWrite;
	unsigned int skipRows;
	if (localNumRows > getRequest->cloudQueryWithAddresses.cloudQuery.startingRowIndex)
	{
		localRowsToWrite = localNumRows - getRequest->cloudQueryWithAddresses.cloudQuery.startingRowIndex;
		skipRows = 0;
	}
	else
	{
		localRowsToWrite = 0;
		skipRows = getRequest->cloudQueryWithAddresses.cloudQuery.startingRowIndex - localNumRows;
	}
	cloudQueryResult.SerializeNumRows(true, localRowsToWrite, &bsOut);
	for (unsigned int i = getRequest->cloudQueryWithAddresses.cloudQuery.startingRowIndex; i < localNumRows; i++)
	{
		WriteCloudQueryRowFromResultList(i, cloudDataResultList, cloudKeyResultList, &bsOut);
	}

	// Append remote systems for remaining rows
	if (unlimitedRows == true || getRequest->cloudQueryWithAddresses.cloudQuery.maxRowsToReturn > localRowsToWrite)
	{
		uint32_t remainingRows = 0;
		uint32_t additionalRowsWritten = 0;
		if (unlimitedRows == false)
			remainingRows = getRequest->cloudQueryWithAddresses.cloudQuery.maxRowsToReturn - localRowsToWrite;

		for (BufferedGetResponseFromServer *bufferedGetResponseFromServer : getRequest->remoteServerResponses)
		{
			size_t cloudQueryRowIndex;
			for (cloudQueryRowIndex = 0; cloudQueryRowIndex < bufferedGetResponseFromServer->queryResult.rowsReturned.size(); cloudQueryRowIndex++)
			{
				if (skipRows > 0)
				{
					--skipRows;
					continue;
				}
				bufferedGetResponseFromServer->queryResult.rowsReturned[cloudQueryRowIndex]->Serialize(true, &bsOut, this);

				++additionalRowsWritten;
				if (unlimitedRows == false && --remainingRows == 0)
					break;
			}

			if (unlimitedRows == false && remainingRows == 0)
				break;
		}

		if (additionalRowsWritten > 0)
		{
			BitSize_t curOffset = bsOut.GetWriteOffset();
			bsOut.SetWriteOffset(bitStreamOffset);
			localRowsToWrite += additionalRowsWritten;
			cloudQueryResult.SerializeNumRows(true, localRowsToWrite, &bsOut);
			bsOut.SetWriteOffset(curOffset);
		}
	}

	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, getRequest->requestingClient, false);
}
void CloudServer::ProcessCloudQueryWithAddresses(CloudServer::CloudQueryWithAddresses &cloudQueryWithAddresses, std::vector<CloudData *> &cloudDataResultList, std::vector<CloudKey> &cloudKeyResultList)
{
	CloudQueryResult cloudQueryResult;
	CloudQueryRow cloudQueryRow;
	size_t queryIndex;
	bool dataRepositoryExists;
	CloudDataList *cloudDataList;
	unsigned int keyDataIndex;

	// If specificSystems list empty, applies to all systems
	// For each of keys in cloudQueryWithAddresses, return that data, limited by maxRowsToReturn
	for (queryIndex = 0; queryIndex < cloudQueryWithAddresses.cloudQuery.keys.size(); queryIndex++)
	{
		const CloudKey &key = cloudQueryWithAddresses.cloudQuery.keys[queryIndex];

		unsigned int dataRepositoryIndex = SortedIndex(dataRepository, key, KeyDataListComp, &dataRepositoryExists);
		if (dataRepositoryExists)
		{
			cloudDataList = dataRepository[dataRepositoryIndex];

			if (cloudDataList->uploaderCount > 0)
			{
				// Return all keyData that was uploaded by specificSystems, or all if not specified
				if (!cloudQueryWithAddresses.specificSystems.empty())
				{
					// Return data for matching systems
					for (const RakNetGUID &specificSystem : cloudQueryWithAddresses.specificSystems)
					{
						bool uploaderExists;
						keyDataIndex = SortedIndex(cloudDataList->keyData, specificSystem, KeyDataPtrComp, &uploaderExists);
						if (uploaderExists)
						{
							cloudDataResultList.push_back(cloudDataList->keyData[keyDataIndex]);
							cloudKeyResultList.push_back(key);
						}
					}
				}
				else
				{
					// Return data for all systems
					for (CloudData *keyData : cloudDataList->keyData)
					{
						cloudDataResultList.push_back(keyData);
						cloudKeyResultList.push_back(key);
					}
				}
			}
		}
	}
}
void CloudServer::SendUploadedAndSubscribedKeysToServer(RakNetGUID systemAddress)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
	bsOut.Write((MessageID) STSC_ADD_UPLOADED_AND_SUBSCRIBED_KEYS);
	bsOut.WriteCasted<uint16_t>(dataRepository.size());
	for (CloudDataList *cloudDataList : dataRepository)
		cloudDataList->key.Serialize(true, &bsOut);

	BitSize_t startOffset, endOffset;
	uint16_t subscribedKeyCount = 0;
	startOffset = bsOut.GetWriteOffset();
	bsOut.WriteCasted<uint16_t>(subscribedKeyCount);
	for (CloudDataList *cloudDataList : dataRepository)
	{
		if (cloudDataList->subscriberCount > 0)
		{
			cloudDataList->key.Serialize(true, &bsOut);
			subscribedKeyCount++;
		}
	}
	endOffset = bsOut.GetWriteOffset();
	bsOut.SetWriteOffset(startOffset);
	bsOut.WriteCasted<uint16_t>(subscribedKeyCount);
	bsOut.SetWriteOffset(endOffset);

	if (!dataRepository.empty() || subscribedKeyCount > 0)
		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, systemAddress, false);
}
void CloudServer::SendUploadedKeyToServers(CloudKey &cloudKey)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
	bsOut.Write((MessageID) STSC_ADD_UPLOADED_KEY);
	cloudKey.Serialize(true, &bsOut);
	for (RemoteServer *remoteServer : remoteServers)
		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, remoteServer->serverAddress, false);
}
void CloudServer::SendSubscribedKeyToServers(CloudKey &cloudKey)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
	bsOut.Write((MessageID) STSC_ADD_SUBSCRIBED_KEY);
	cloudKey.Serialize(true, &bsOut);
	for (RemoteServer *remoteServer : remoteServers)
		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, remoteServer->serverAddress, false);
}
void CloudServer::RemoveUploadedKeyFromServers(CloudKey &cloudKey)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
	bsOut.Write((MessageID) STSC_REMOVE_UPLOADED_KEY);
	cloudKey.Serialize(true, &bsOut);
	for (RemoteServer *remoteServer : remoteServers)
		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, remoteServer->serverAddress, false);
}
void CloudServer::RemoveSubscribedKeyFromServers(CloudKey &cloudKey)
{
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_CLOUD_SERVER_TO_SERVER_COMMAND);
	bsOut.Write((MessageID) STSC_REMOVE_SUBSCRIBED_KEY);
	cloudKey.Serialize(true, &bsOut);
	for (RemoteServer *remoteServer : remoteServers)
		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, remoteServer->serverAddress, false);
}
void CloudServer::OnSendUploadedAndSubscribedKeysToServer(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	bool objectExists;
	unsigned int index = SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;
	RemoteServer *remoteServer = remoteServers[index];
	remoteServer->gotSubscribedAndUploadedKeys = true;

	uint16_t numUploadedKeys, numSubscribedKeys;
	bsIn.Read(numUploadedKeys);
	for (uint16_t i = 0; i < numUploadedKeys; i++)
	{
		CloudKey cloudKey;
		cloudKey.Serialize(false, &bsIn);

		remoteServer->uploadedKeys.insert(cloudKey);
	}

	bsIn.Read(numSubscribedKeys);
	for (uint16_t i = 0; i < numSubscribedKeys; i++)
	{
		CloudKey cloudKey;
		cloudKey.Serialize(false, &bsIn);

		remoteServer->subscribedKeys.insert(cloudKey);
	}

	// Potential todo - join servers
	// For each uploaded key that we subscribe to, query it
	// For each subscribed key that we have, send it
}
void CloudServer::OnSendUploadedKeyToServers(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	bool objectExists;
	unsigned int index = SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;
	RemoteServer *remoteServer = remoteServers[index];
	CloudKey cloudKey;
	cloudKey.Serialize(false, &bsIn);
	remoteServer->uploadedKeys.insert(cloudKey);
}
void CloudServer::OnSendSubscribedKeyToServers(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	bool objectExists;
	unsigned int index = SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;
	RemoteServer *remoteServer = remoteServers[index];
	CloudKey cloudKey;
	cloudKey.Serialize(false, &bsIn);

	// Do not need to send current values, the Get request will do that as the Get request is sent at the same time
	remoteServer->subscribedKeys.insert(cloudKey);
}
void CloudServer::OnRemoveUploadedKeyFromServers(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	bool objectExists;
	unsigned int index = SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;
	RemoteServer *remoteServer = remoteServers[index];
	CloudKey cloudKey;
	cloudKey.Serialize(false, &bsIn);
	remoteServer->uploadedKeys.erase(cloudKey);
}
void CloudServer::OnRemoveSubscribedKeyFromServers(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	bool objectExists;
	unsigned int index = SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;
	RemoteServer *remoteServer = remoteServers[index];
	CloudKey cloudKey;
	cloudKey.Serialize(false, &bsIn);
	remoteServer->subscribedKeys.erase(cloudKey);
}
void CloudServer::OnServerDataChanged(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	bool objectExists;
	SortedIndex(remoteServers, packet->guid, RemoteServerComp, &objectExists);
	if (objectExists == false)
		return;

	// Find everyone that cares about this change and relay
	bool wasUpdated = false;
	bsIn.Read(wasUpdated);
	CloudQueryRow row;
	row.Serialize(false, &bsIn, this);

	CloudDataList *cloudDataList;
	bool dataRepositoryExists;
	unsigned int dataRepositoryIndex;
	dataRepositoryIndex = SortedIndex(dataRepository, row.key, KeyDataListComp, &dataRepositoryExists);
	if (dataRepositoryExists == false)
	{
		DeallocateRowData(row.data);
		return;
	}
	cloudDataList = dataRepository[dataRepositoryIndex];
	CloudData *cloudData;
	bool keyDataListExists;
	unsigned int keyDataListIndex = SortedIndex(cloudDataList->keyData, row.clientGUID, KeyDataPtrComp, &keyDataListExists);
	if (keyDataListExists == true)
	{
		cloudData = cloudDataList->keyData[keyDataListIndex];
		NotifyClientSubscribersOfDataChange(&row, cloudData->specificSubscribers, wasUpdated);
	}

	NotifyClientSubscribersOfDataChange(&row, cloudDataList->nonSpecificSubscribers, wasUpdated);
	DeallocateRowData(row.data);
}
void CloudServer::GetServersWithUploadedKeys(
    std::vector<CloudKey> &keys,
    std::vector<CloudServer::RemoteServer *> &remoteServersWithData)
{
	remoteServersWithData.clear();

	size_t i, j;
	for (i = 0; i < remoteServers.size(); i++)
	{
		remoteServers[i]->workingFlag = false;
	}

	for (i = 0; i < remoteServers.size(); i++)
	{
		if (remoteServers[i]->workingFlag == false)
		{
			if (remoteServers[i]->gotSubscribedAndUploadedKeys == false)
			{
				remoteServers[i]->workingFlag = true;
				remoteServersWithData.push_back(remoteServers[i]);
			}
			else
			{
				remoteServers[i]->workingFlag = false;
				for (j = 0; j < keys.size(); j++)
				{
					if (remoteServers[i]->workingFlag == false && remoteServers[i]->uploadedKeys.count(keys[j]) != 0)
					{
						remoteServers[i]->workingFlag = true;
						remoteServersWithData.push_back(remoteServers[i]);
						break;
					}
				}
			}
		}
	}
}

CloudServer::CloudDataList *CloudServer::GetOrAllocateCloudDataList(CloudKey key, bool *dataRepositoryExists, unsigned int &dataRepositoryIndex)
{
	CloudDataList *cloudDataList;

	dataRepositoryIndex = SortedIndex(dataRepository, key, KeyDataListComp, dataRepositoryExists);
	if (*dataRepositoryExists == false)
	{
		cloudDataList = new CloudDataList;
		cloudDataList->key = key;
		cloudDataList->uploaderCount = 0;
		cloudDataList->subscriberCount = 0;
		dataRepository.insert(dataRepository.begin() + dataRepositoryIndex, cloudDataList);
	}
	else
	{
		cloudDataList = dataRepository[dataRepositoryIndex];
	}

	return cloudDataList;
}

void CloudServer::UnsubscribeFromKey(RemoteCloudClient *remoteCloudClient, RakNetGUID remoteCloudClientGuid, unsigned int keySubscriberIndex, CloudKey &cloudKey, std::vector<RakNetGUID> &specificSystems)
{
	KeySubscriberID *keySubscriberId = remoteCloudClient->subscribedKeys[keySubscriberIndex];

	// If removing specific systems, but global subscription, fail
	if (keySubscriberId->specificSystemsSubscribedTo.empty() && !specificSystems.empty())
		return;

	bool dataRepositoryExists;
	CloudDataList *cloudDataList;
	unsigned int dataRepositoryIndex = SortedIndex(dataRepository, cloudKey, KeyDataListComp, &dataRepositoryExists);
	if (dataRepositoryExists == false)
		return;

	cloudDataList = dataRepository[dataRepositoryIndex];
	if (specificSystems.empty())
	{
		// Remove global subscriber. If returns false, have to remove specific subscribers
		if (cloudDataList->RemoveSubscriber(remoteCloudClientGuid) == false)
		{
			for (const RakNetGUID &subscribedTo : keySubscriberId->specificSystemsSubscribedTo)
			{
				RemoveSpecificSubscriber(subscribedTo, cloudDataList, remoteCloudClientGuid);
			}
		}
		keySubscriberId->specificSystemsSubscribedTo.clear();
	}
	else
	{
		for (const RakNetGUID &specificSystem : specificSystems)
		{
			if (keySubscriberId->specificSystemsSubscribedTo.erase(specificSystem) != 0)
				RemoveSpecificSubscriber(specificSystem, cloudDataList, remoteCloudClientGuid);
		}
	}

	if (keySubscriberId->specificSystemsSubscribedTo.empty())
	{
		delete keySubscriberId;
		remoteCloudClient->subscribedKeys.erase(remoteCloudClient->subscribedKeys.begin() + keySubscriberIndex);
	}

	if (cloudDataList->subscriberCount == 0)
		RemoveSubscribedKeyFromServers(cloudKey);

	if (cloudDataList->IsUnused())
	{
		delete cloudDataList;
		dataRepository.erase(dataRepository.begin() + dataRepositoryIndex);
	}
}
void CloudServer::RemoveSpecificSubscriber(RakNetGUID specificSubscriber, CloudDataList *cloudDataList, RakNetGUID remoteCloudClientGuid)
{
	bool keyDataListExists;
	unsigned int keyDataListIndex = SortedIndex(cloudDataList->keyData, specificSubscriber, KeyDataPtrComp, &keyDataListExists);
	if (keyDataListExists == false)
		return;
	CloudData *cloudData = cloudDataList->keyData[keyDataListIndex];
	if (cloudData->specificSubscribers.erase(remoteCloudClientGuid) != 0)
	{
		cloudDataList->subscriberCount--;

		if (cloudData->IsUnused())
		{
			delete cloudData;
			cloudDataList->keyData.erase(cloudDataList->keyData.begin() + keyDataListIndex);
		}
	}
}

void CloudServer::ForceExternalSystemAddress(SystemAddress forcedAddress)
{
	forceAddress = forcedAddress;
}
void CloudServer::AddQueryFilter(CloudServerQueryFilter *filter)
{
	if (std::find(queryFilters.begin(), queryFilters.end(), filter) != queryFilters.end())
		return;
	queryFilters.push_back(filter);
}
void CloudServer::RemoveQueryFilter(CloudServerQueryFilter *filter)
{
	const auto it = std::find(queryFilters.begin(), queryFilters.end(), filter);
	if (it != queryFilters.end())
		queryFilters.erase(it);
}
void CloudServer::RemoveAllQueryFilters(void)
{
	queryFilters.clear();
}

#endif

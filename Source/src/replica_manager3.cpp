/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/native_feature_includes.h"

#include "mafianet/replica_manager3.h"
#include "mafianet/virtual_world_replica3.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/network_id_manager.h"

#include "mafianet/vector_util.h"

#include <algorithm>
#include <atomic>
#include <deque>

using namespace MafiaNet;

namespace
{
// A serialize pass visits records one connection at a time. Keep that connection's
// records in adjacent blocks; deque growth preserves pointers held by its lists.
class ConnectionRecords
{
public:
	LastSerializationResult *Acquire(Replica3 *replica)
	{
		LastSerializationResult *record;
		if (available.empty())
			record = &records.emplace_back();
		else
		{
			record = available.back();
			available.pop_back();
			record->whenLastSerialized = GetTime();
			record->servedPass = 0;
		}
		record->replica = replica;
		if (++nextAllocation == 0)
			++nextAllocation; // Zero identifies records constructed outside this store.
		record->allocationNumber = nextAllocation;
		return record;
	}

	void Release(LastSerializationResult *record)
	{
		if (record->allocationNumber == 0)
		{
			delete record;
			return;
		}
		delete record->lastSerializationResultBS;
		record->lastSerializationResultBS = nullptr;
		record->replica = nullptr;
		if (available.size() + 1 == records.size())
		{
			// A completely streamed-out connection need not keep a world of empty slots.
			std::deque<LastSerializationResult>().swap(records);
			std::vector<LastSerializationResult *>().swap(available);
		}
		else
			available.push_back(record);
	}

private:
	std::deque<LastSerializationResult> records;
	std::vector<LastSerializationResult *> available;
	uint32_t nextAllocation = 0;
};
} // namespace

// Outlives its connection for as long as anyone holds or waits for it: a callback made with the lock held may
// pop the connection and free it, and the receive thread may be waiting for a connection another thread pops.
struct MafiaNet::RM3ConnectionLock
{
	std::recursive_mutex mutex;
	// The connection's own, and one for each holder.
	std::atomic<int> references{1};
	// How many worlds the connection is in, under mutex. At 0 it may be freed as soon as the mutex is let go.
	int worlds = 0;
	// Around every edit of the connection's constructedReplicaList, which also needs the world or to be the
	// thread serving the connection, and around every read of it made without either. The last lock of all.
	std::mutex constructed;
	ConnectionRecords records;
};

namespace
{
// How many connection locks this thread holds, and whose they are: that manager's Update() does nothing
// meanwhile. A pass must not start under a connection's lock, which its workers would wait for.
thread_local unsigned int connectionLocksHeld = 0;
thread_local const ReplicaManager3 *connectionLocksOwner = 0;

void DropConnectionLock(RM3ConnectionLock *lock)
{
	if (--lock->references == 0)
		delete lock;
}

// Lets go of a lock ReplicaManager3::LockConnection() or LockConnectionByGUID() took, if it took one.
class ConnectionLockHolder
{
public:
	explicit ConnectionLockHolder(RM3ConnectionLock *lock)
	    : lock(lock)
	{
	}
	~ConnectionLockHolder()
	{
		if (lock)
		{
			--connectionLocksHeld;
			lock->mutex.unlock();
			DropConnectionLock(lock);
		}
	}
	ConnectionLockHolder(const ConnectionLockHolder &) = delete;
	ConnectionLockHolder &operator=(const ConnectionLockHolder &) = delete;

	RM3ConnectionLock *const lock;
};

// The manager whose replica set this thread holds, and how deep: a change made from inside a Deserialize(), or
// from inside another change, does not take the lock again.
thread_local unsigned int replicaSetDepth = 0;
thread_local const ReplicaManager3 *replicaSetOwner = 0;

class ListLock
{
public:
	explicit ListLock(SimpleMutex &mutex)
	    : mutex(mutex)
	{
		mutex.Lock();
	}
	~ListLock() { mutex.Unlock(); }
	ListLock(const ListLock &) = delete;
	ListLock &operator=(const ListLock &) = delete;

private:
	SimpleMutex &mutex;
};
} // namespace

// DEFINE_MULTILIST_PTR_TO_MEMBER_COMPARISONS(LastSerializationResult,Replica3*,replica);

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool PRO::operator==(const PRO &right) const
{
	return priority == right.priority && reliability == right.reliability && orderingChannel == right.orderingChannel && sendReceipt == right.sendReceipt;
}

bool PRO::operator!=(const PRO &right) const
{
	return priority != right.priority || reliability != right.reliability || orderingChannel != right.orderingChannel || sendReceipt != right.sendReceipt;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

int Connection_RM3::Replica3LSRComp(Replica3 *const &replica3, LastSerializationResult *const &data)
{
	/*
	if (replica3->GetNetworkID() < data->replica->GetNetworkID())
		return -1;
	if (replica3->GetNetworkID() > data->replica->GetNetworkID())
		return 1;
		*/

	// 7/28/2013 - If GetNetworkID chagned during runtime, the list would be out of order and lookup would always fail or go out of bounds
	// I remember before that I could not directly compare
	if (replica3->referenceIndex < data->replica->referenceIndex)
		return -1;
	if (replica3->referenceIndex > data->replica->referenceIndex)
		return 1;
	return 0;
}

namespace
{
// A connection's constructedReplicaList, sorted by Connection_RM3::Replica3LSRComp.
using ConstructedList = std::vector<LastSerializationResult *>;

// Index of \a replica in \a list, or the index it would be inserted at; *objectExists says which.
unsigned int FindConstructed(const ConstructedList &list, Replica3 *replica, bool *objectExists)
{
	const auto it = std::lower_bound(list.begin(), list.end(), replica,
	                                 [](LastSerializationResult *const &data, Replica3 *const &key)
	                                 { return Connection_RM3::Replica3LSRComp(key, data) > 0; });
	*objectExists = it != list.end() && Connection_RM3::Replica3LSRComp(replica, *it) == 0;
	return (unsigned int) (it - list.begin());
}

// Inserts \a lsr keyed by \a replica. Returns its index, or (unsigned int) -1 without inserting if the
// replica is already in the list.
unsigned int InsertConstructed(ConstructedList &list, Replica3 *replica, LastSerializationResult *lsr)
{
	bool objectExists;
	const unsigned int index = FindConstructed(list, replica, &objectExists);
	if (objectExists)
	{
		// This is usually a bug!
		RakAssert("replica inserted twice into constructedReplicaList" && 0);
		return (unsigned int) -1;
	}
	list.insert(list.begin() + index, lsr);
	return index;
}
} // namespace

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

LastSerializationResult::LastSerializationResult()
{
	replica = 0;
	lastSerializationResultBS = 0;
	whenLastSerialized = MafiaNet::GetTime();
	servedPass = 0;
}
LastSerializationResult::~LastSerializationResult()
{
	if (lastSerializationResultBS)
		delete lastSerializationResultBS;
}
void LastSerializationResult::AllocBS(void)
{
	if (lastSerializationResultBS == 0)
	{
		lastSerializationResultBS = new LastSerializationResultBS;
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

class ReplicaManager3::ReplicaSetDelivery
{
public:
	explicit ReplicaSetDelivery(ReplicaManager3 *manager)
	    : manager(manager), locked(replicaSetDepth == 0 || replicaSetOwner != manager), counted(!locked || replicaSetDepth == 0)
	{
		if (locked)
			manager->replicaSetMutex.lock_shared();
		if (counted && replicaSetDepth++ == 0)
			replicaSetOwner = manager;
	}
	~ReplicaSetDelivery()
	{
		if (counted)
			--replicaSetDepth;
		if (locked)
			manager->replicaSetMutex.unlock_shared();
	}
	ReplicaSetDelivery(const ReplicaSetDelivery &) = delete;
	ReplicaSetDelivery &operator=(const ReplicaSetDelivery &) = delete;

private:
	ReplicaManager3 *manager;
	const bool locked, counted;
};

class ReplicaManager3::ReplicaSetChange
{
public:
	explicit ReplicaSetChange(ReplicaManager3 *manager)
	    : manager(manager), locked(replicaSetDepth == 0 || replicaSetOwner != manager), counted(!locked || replicaSetDepth == 0)
	{
		if (locked)
			manager->replicaSetMutex.lock();
		if (counted && replicaSetDepth++ == 0)
			replicaSetOwner = manager;
	}
	~ReplicaSetChange()
	{
		if (counted)
			--replicaSetDepth;
		if (locked)
			manager->replicaSetMutex.unlock();
	}
	ReplicaSetChange(const ReplicaSetChange &) = delete;
	ReplicaSetChange &operator=(const ReplicaSetChange &) = delete;

private:
	ReplicaManager3 *manager;
	const bool locked, counted;
};

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ReplicaManager3::ReplicaManager3(RakPeerInterface &peer)
    : ReplicaManager3()
{
	rakPeerInterface = &peer;
}

ReplicaManager3::ReplicaManager3()
{
	defaultSendParameters.orderingChannel = 0;
	defaultSendParameters.priority = MafiaNet::Priority::High;
	defaultSendParameters.reliability = MafiaNet::Reliability::ReliableOrdered;
	defaultSendParameters.sendReceipt = 0;
	nextReferenceIndex = 0;
	autoSerializeInterval = 30;
	lastAutoSerializeOccurance = 0;
	autoCreateConnections = true;
	autoDestroyConnections = true;
	currentlyDeallocatingReplica = 0;
	serializeExecutor = 0;
	serializePass = 0;
	inSerializePass = false;
	membershipGeneration = 0;
	inUpdate = false;

	for (unsigned int i = 0; i < sizeof(worldsArray) / sizeof(worldsArray[0]); i++)
		worldsArray[i] = 0;

	AddWorld(0);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ReplicaManager3::~ReplicaManager3()
{
	if (autoDestroyConnections)
	{
		for (unsigned int i = 0; i < worldsList.size(); i++)
		{
			RakAssert(worldsList[i]->connectionList.size() == 0);
		}
	}
	Clear(true);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetAutoManageConnections(bool autoCreate, bool autoDestroy)
{
	autoCreateConnections = autoCreate;
	autoDestroyConnections = autoDestroy;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool ReplicaManager3::GetAutoCreateConnections(void) const
{
	return autoCreateConnections;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool ReplicaManager3::GetAutoDestroyConnections(void) const
{
	return autoDestroyConnections;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::AutoCreateConnectionList(
    std::vector<RakNetGUID> &participantListIn,
    std::vector<Connection_RM3 *> &participantListOut,
    WorldId worldId)
{
	for (unsigned int index = 0; index < participantListIn.size(); index++)
	{
		if (GetConnectionByGUID(participantListIn[index], worldId) == nullptr)
		{
			Connection_RM3 *connection = AllocConnection(rakPeerInterface->GetSystemAddressFromGuid(participantListIn[index]), participantListIn[index]);
			if (connection)
			{
				if (PushConnection(connection, worldId))
					participantListOut.push_back(connection);
				else
					DeallocConnection(connection);
			}
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool ReplicaManager3::PushConnection(MafiaNet::Connection_RM3 *newConnection, WorldId worldId)
{
	if (newConnection == 0)
		return false;
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	if (GetConnectionByGUID(newConnection->GetRakNetGUID(), worldId))
		return false;
	// Was this intended?
	RakAssert(newConnection->GetRakNetGUID() != rakPeerInterface->GetMyGUID());

	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	if (std::find(world->connectionList.begin(), world->connectionList.end(), newConnection) == world->connectionList.end())
	{
		// Before it is in the list, where the receive thread finds it.
		ConnectionLockHolder connectionLock(LockConnection(newConnection));
		++connectionLock.lock->worlds;
		{
			ListLock listLock(m_WorldListMutex);
			world->connectionList.push_back(newConnection);
		}

		// Send message to validate the connection
		if (!newConnection->isValidated)
			newConnection->SendValidation(rakPeerInterface, worldId);

		Connection_RM3::ConstructionMode constructionMode = newConnection->QueryConstructionMode();
		if (constructionMode == Connection_RM3::QUERY_REPLICA_FOR_CONSTRUCTION || constructionMode == Connection_RM3::QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION)
		{
			unsigned int pushIdx;
			for (pushIdx = 0; pushIdx < world->userReplicaList.size(); pushIdx++)
				newConnection->OnLocalReference(world->userReplicaList[pushIdx], this);
		}
	}
	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::DeallocReplicaNoBroadcastDestruction(MafiaNet::Connection_RM3 *connection, MafiaNet::Replica3 *replica3)
{
	currentlyDeallocatingReplica = replica3;
	replica3->DeallocReplica(connection);
	currentlyDeallocatingReplica = 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

MafiaNet::Connection_RM3 *ReplicaManager3::PopConnection(unsigned int index, WorldId worldId)
{
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	++membershipGeneration;
	std::vector<Replica3 *> replicaList;
	std::vector<NetworkID> destructionList;
	std::vector<Replica3 *> broadcastList;
	MafiaNet::Connection_RM3 *connection;
	unsigned int index2;
	RM3ActionOnPopConnection action;

	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	connection = world->connectionList[index];
	// Waits for the receive thread to be done with a message from it. Whoever waits for this lock from now on
	// finds the connection gone, so the caller may free it on return.
	ConnectionLockHolder connectionLock(LockConnection(connection));

	// Clear out downloadGroup
	connection->ClearDownloadGroup(rakPeerInterface);

	RakNetGUID guid = connection->GetRakNetGUID();
	// This might be wrong, I am relying on the variable creatingSystemGuid which is transmitted
	// automatically from the first system to reference the object. However, if an object changes
	// owners then it is not going to be returned here, and therefore QueryActionOnPopConnection()
	// will not be called for the new owner.
	GetReplicasCreatedByGuid(guid, replicaList, worldId);

	for (index2 = 0; index2 < replicaList.size(); index2++)
	{
		action = replicaList[index2]->QueryActionOnPopConnection(connection);
		replicaList[index2]->OnPoppedConnection(connection);
		if (action == RM3AOPC_DELETE_REPLICA)
		{
			if (replicaList[index2]->GetNetworkIDManager())
				destructionList.push_back(replicaList[index2]->GetNetworkID());
		}
		else if (action == RM3AOPC_DELETE_REPLICA_AND_BROADCAST_DESTRUCTION)
		{
			if (replicaList[index2]->GetNetworkIDManager())
				destructionList.push_back(replicaList[index2]->GetNetworkID());

			broadcastList.push_back(replicaList[index2]);
		}
		else if (action == RM3AOPC_DO_NOTHING)
		{
			for (unsigned int index3 = 0; index3 < connection->queryToSerializeReplicaList.size(); index3++)
			{
				LastSerializationResult *lsr = connection->queryToSerializeReplicaList[index3];
				lsr->whenLastSerialized = 0;
				if (lsr->lastSerializationResultBS)
				{
					for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
						lsr->lastSerializationResultBS->bitStream[z].Reset();
				}
			}
		}
	}

	BroadcastDestructionList(broadcastList, connection->GetSystemAddress(), worldId);
	for (index2 = 0; index2 < destructionList.size(); index2++)
	{
		// Do lookup in case DeallocReplica destroyed one of of the later Replica3 instances in the list
		Replica3 *replicaToDestroy = world->networkIDManager->GET_OBJECT_FROM_ID<Replica3 *>(destructionList[index2]);
		if (replicaToDestroy)
		{
			replicaToDestroy->PreDestruction(connection);
			replicaToDestroy->DeallocReplica(connection);
		}
	}

	{
		ListLock listLock(m_WorldListMutex);
		world->connectionList.erase(world->connectionList.begin() + index);
	}
	--connectionLock.lock->worlds;
	return connection;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

MafiaNet::Connection_RM3 *ReplicaManager3::PopConnection(RakNetGUID guid, WorldId worldId)
{
	unsigned int index;

	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	for (index = 0; index < world->connectionList.size(); index++)
	{
		if (world->connectionList[index]->GetRakNetGUID() == guid)
		{
			return PopConnection(index, worldId);
		}
	}
	return 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::Reference(MafiaNet::Replica3 *replica3, WorldId worldId)
{
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	unsigned int index = ReferenceInternal(replica3, worldId);

	if (index != (unsigned int) -1)
	{
		unsigned int pushIdx;
		for (pushIdx = 0; pushIdx < world->connectionList.size(); pushIdx++)
		{
			Connection_RM3::ConstructionMode constructionMode = world->connectionList[pushIdx]->QueryConstructionMode();
			if (constructionMode == Connection_RM3::QUERY_REPLICA_FOR_CONSTRUCTION || constructionMode == Connection_RM3::QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION)
			{
				world->connectionList[pushIdx]->OnLocalReference(replica3, this);
			}
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

unsigned int ReplicaManager3::ReferenceInternal(MafiaNet::Replica3 *replica3, WorldId worldId)
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	if (std::find(world->userReplicaList.begin(), world->userReplicaList.end(), replica3) == world->userReplicaList.end())
	{
		RakAssert(world->networkIDManager);
		{
			ReplicaSetChange change(this);
			replica3->SetNetworkIDManager(world->networkIDManager);
			replica3->replicaManager = this;
			++replica3->referencingWorlds;
		}
		if (replica3->creatingSystemGUID == UNASSIGNED_RAKNET_GUID)
			replica3->creatingSystemGUID = rakPeerInterface->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
		replica3->broadcastsIdentically = replica3->BroadcastsIdentically();
		if (replica3->referenceIndex == (uint32_t) -1)
		{
			replica3->referenceIndex = nextReferenceIndex++;
		}
		ListLock listLock(m_WorldListMutex);
		world->userReplicaList.push_back(replica3);
		return (unsigned int) (world->userReplicaList.size() - 1);
	}
	return (unsigned int) -1;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::Dereference(MafiaNet::Replica3 *replica3, WorldId worldId)
{
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	++membershipGeneration;
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	unsigned int index, index2;
	bool wasReferenced = false;
	for (index = 0; index < world->userReplicaList.size(); index++)
	{
		if (world->userReplicaList[index] == replica3)
		{
			ListLock listLock(m_WorldListMutex);
			world->userReplicaList.erase(world->userReplicaList.begin() + index);
			wasReferenced = true;
			break;
		}
	}

	// Remove from all connections
	for (index2 = 0; index2 < world->connectionList.size(); index2++)
	{
		world->connectionList[index2]->OnDereference(replica3, this);
	}

	if (wasReferenced)
	{
		// Waits for a Deserialize() of it under way on the receive thread. Out of its last world, it leaves the
		// table too, with its id: no state message finds it again, and it can be destroyed with no thread
		// walking over it.
		ReplicaSetChange change(this);
		if (--replica3->referencingWorlds == 0)
		{
			const NetworkID networkId = replica3->GetNetworkID();
			replica3->SetNetworkIDManager(0);
			replica3->SetNetworkID(networkId);
			replica3->replicaManager = nullptr;
			replica3->referenceIndex = static_cast<uint32_t>(-1);
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::DereferenceList(std::vector<Replica3 *> &replicaListIn, WorldId worldId)
{
	unsigned int index;
	for (index = 0; index < replicaListIn.size(); index++)
		Dereference(replicaListIn[index], worldId);
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::GetReplicasCreatedByMe(std::vector<Replica3 *> &replicaListOut, WorldId worldId)
{
	//RakNetGUID myGuid = rakPeerInterface->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
	GetReplicasCreatedByGuid(rakPeerInterface->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS), replicaListOut, worldId);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::GetReferencedReplicaList(std::vector<Replica3 *> &replicaListOut, WorldId worldId)
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	replicaListOut = world->userReplicaList;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::GetReplicasCreatedByGuid(RakNetGUID guid, std::vector<Replica3 *> &replicaListOut, WorldId worldId)
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	replicaListOut.clear();
	unsigned int index;
	for (index = 0; index < world->userReplicaList.size(); index++)
	{
		if (world->userReplicaList[index]->creatingSystemGUID == guid)
			replicaListOut.push_back(world->userReplicaList[index]);
	}
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

unsigned ReplicaManager3::GetReplicaCount(WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	return (unsigned int) world->userReplicaList.size();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Replica3 *ReplicaManager3::GetReplicaAtIndex(unsigned index, WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	return world->userReplicaList[index];
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

unsigned int ReplicaManager3::GetConnectionCount(WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	return (unsigned int) world->connectionList.size();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Connection_RM3 *ReplicaManager3::GetConnectionAtIndex(unsigned index, WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	return world->connectionList[index];
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Connection_RM3 *ReplicaManager3::GetConnectionBySystemAddress(const SystemAddress &sa, WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	unsigned int index;
	for (index = 0; index < world->connectionList.size(); index++)
	{
		if (world->connectionList[index]->GetSystemAddress() == sa)
		{
			return world->connectionList[index];
		}
	}
	return 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Connection_RM3 *ReplicaManager3::GetConnectionByGUID(RakNetGUID guid, WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	unsigned int index;
	for (index = 0; index < world->connectionList.size(); index++)
	{
		if (world->connectionList[index]->GetRakNetGUID() == guid)
		{
			return world->connectionList[index];
		}
	}
	return 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::GetConnectionsInVirtualWorld(VirtualWorldId virtualWorld, std::vector<Connection_RM3 *> &connectionsOut, bool includeGlobal, WorldId worldId) const
{
	connectionsOut.clear();

	// Two distinct layers: the heavyweight RM3 world (worldId, default 0) is the
	// container that actually holds the connection list; the virtual world is a
	// lightweight per-connection tag we filter that list by. We are not switching
	// RM3 worlds here -- we look up the RM3 world only to get its connections,
	// then keep the ones whose virtual world matches. (Most setups only ever use
	// the auto-created RM3 world 0.) See virtual_world.h.
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];

	unsigned int index;
	for (index = 0; index < world->connectionList.size(); index++)
	{
		Connection_RM3 *connection = world->connectionList[index];
		VirtualWorldId connVirtualWorld = connection->GetVirtualWorld();
		// Membership test (not a symmetric visibility check): the connection is in
		// this virtual world, plus -- when includeGlobal -- any global observer.
		// Using VirtualWorldsCanSee here would make a GLOBAL *query* match every
		// connection, which is not what "connections in virtual world N" means.
		bool matches = (connVirtualWorld == virtualWorld) ||
		               (includeGlobal && connVirtualWorld == VIRTUAL_WORLD_GLOBAL);
		if (matches)
			connectionsOut.push_back(connection);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::GetGuidsInVirtualWorld(VirtualWorldId virtualWorld, std::vector<RakNetGUID> &guidsOut, bool includeGlobal, WorldId worldId) const
{
	guidsOut.clear();
	std::vector<Connection_RM3 *> connections;
	GetConnectionsInVirtualWorld(virtualWorld, connections, includeGlobal, worldId);

	unsigned int index;
	for (index = 0; index < connections.size(); index++)
		guidsOut.push_back(connections[index]->GetRakNetGUID());
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetPlayerVirtualWorld(Connection_RM3 *connection, VirtualWorldReplica3 *avatar, VirtualWorldId virtualWorld)
{
	if (connection)
		connection->SetVirtualWorld(virtualWorld);
	if (avatar)
		avatar->SetVirtualWorld(virtualWorld);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetDefaultOrderingChannel(char def)
{
	defaultSendParameters.orderingChannel = def;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetDefaultPacketPriority(MafiaNet::Priority def)
{
	defaultSendParameters.priority = def;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetDefaultPacketReliability(MafiaNet::Reliability def)
{
	defaultSendParameters.reliability = def;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetAutoSerializeInterval(MafiaNet::Time intervalMS)
{
	autoSerializeInterval = intervalMS;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetSerializeExecutor(RM3SerializeExecutor *executor)
{
	serializeExecutor = executor;
}

bool ReplicaManager3::HoldsConnection(const Connection_RM3 *connection, WorldId worldId) const
{
	ListLock listLock(m_WorldListMutex);
	const RM3World *world = worldsArray[worldId];
	if (world == 0)
		return false;
	for (unsigned int i = 0; i < world->connectionList.size(); i++)
	{
		if (world->connectionList[i] == connection)
			return true;
	}
	return false;
}

bool Connection_RM3::HoldsRecord(const LastSerializationResult *lsr) const
{
	for (unsigned int i = 0; i < constructedReplicaList.size(); i++)
	{
		if (constructedReplicaList[i] == lsr)
			return true;
	}
	return false;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::GetConnectionsThatHaveReplicaConstructed(Replica3 *replica, std::vector<Connection_RM3 *> &connectionsThatHaveConstructedThisReplica, WorldId worldId)
{
	// Not the world: a Deserialize() asks this under its sender's lock while a pass holds the world. The list
	// lock keeps every connection in the list, so alive, for as long as it is asked.
	connectionsThatHaveConstructedThisReplica.clear();
	ListLock listLock(m_WorldListMutex);
	RM3World *world = worldsArray[worldId];
	RakAssert(world != 0 && "World not in use");
	if (world == 0)
		return;

	unsigned int index;
	for (index = 0; index < world->connectionList.size(); index++)
	{
		if (world->connectionList[index]->HasReplicaConstructed(replica))
			connectionsThatHaveConstructedThisReplica.push_back(world->connectionList[index]);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool ReplicaManager3::GetAllConnectionDownloadsCompleted(WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	unsigned int index;
	for (index = 0; index < world->connectionList.size(); index++)
	{
		if (world->connectionList[index]->GetDownloadWasCompleted() == false)
			return false;
	}
	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::Clear(bool deleteWorlds)
{
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	for (unsigned int i = 0; i < worldsList.size(); i++)
	{
		worldsList[i]->Clear(this);
		if (deleteWorlds)
		{
			ListLock listLock(m_WorldListMutex);
			worldsArray[worldsList[i]->worldId] = 0;
			delete worldsList[i];
		}
	}
	if (deleteWorlds)
	{
		ListLock listLock(m_WorldListMutex);
		worldsList.clear();
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ReplicaManager3::RM3World::RM3World()
{
	networkIDManager = 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::RM3World::Clear(ReplicaManager3 *replicaManager3)
{
	// Out of the list first, each one once the receive thread is done with it, as PopConnection() does.
	std::vector<Connection_RM3 *> connections = connectionList;
	{
		ListLock listLock(replicaManager3->m_WorldListMutex);
		connectionList.clear();
	}
	for (Connection_RM3 *connection : connections)
	{
		ConnectionLockHolder connectionLock(replicaManager3->LockConnection(connection));
		if (--connectionLock.lock->worlds == 0)
		{
			connection->ClearDownloadGroup(replicaManager3->GetRakPeerInterface());
			if (replicaManager3->GetAutoDestroyConnections())
				replicaManager3->DeallocConnection(connection);
		}
	}

	{
		ReplicaSetChange change(replicaManager3);
		for (Replica3 *replica : userReplicaList)
		{
			if (--replica->referencingWorlds == 0)
			{
				replica->replicaManager = nullptr;
				replica->referenceIndex = static_cast<uint32_t>(-1);
				replica->SetNetworkIDManager(nullptr);
			}
		}
	}
	ListLock listLock(replicaManager3->m_WorldListMutex);
	userReplicaList.clear();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

PRO ReplicaManager3::GetDefaultSendParameters(void) const
{
	return defaultSendParameters;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::AddWorld(WorldId worldId)
{
	RakAssert(worldsArray[worldId] == 0 && "World already in use");

	RM3World *newWorld = new RM3World;
	newWorld->worldId = worldId;
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	ListLock listLock(m_WorldListMutex);
	worldsArray[worldId] = newWorld;
	worldsList.push_back(newWorld);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::RemoveWorld(WorldId worldId)
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	++membershipGeneration;
	worldsArray[worldId]->Clear(this);
	ListLock listLock(m_WorldListMutex);
	for (unsigned int i = 0; i < worldsList.size(); i++)
	{
		if (worldsList[i] == worldsArray[worldId])
		{
			delete worldsList[i];
			SwapRemove(worldsList, i);
			break;
		}
	}
	worldsArray[worldId] = 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

WorldId ReplicaManager3::GetWorldIdAtIndex(unsigned int index)
{
	RakAssert(index < worldsList.size());
	return worldsList[index]->worldId;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

unsigned int ReplicaManager3::GetWorldCount(void) const
{
	return (unsigned int) worldsList.size();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

NetworkIDManager *ReplicaManager3::GetNetworkIDManager(WorldId worldId) const
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	return world->networkIDManager;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::SetNetworkIDManager(NetworkIDManager *_networkIDManager, WorldId worldId)
{
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	world->networkIDManager = _networkIDManager;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

PluginReceiveResult ReplicaManager3::OnReceive(Packet *packet)
{
	if (packet->length < 2)
		return RR_CONTINUE_PROCESSING;

	WorldId incomingWorldId;

	MafiaNet::Time timestamp = 0;
	unsigned char packetIdentifier, packetDataOffset;
	if ((unsigned char) packet->data[0] == ID_TIMESTAMP)
	{
		if (packet->length >= 3 * sizeof(MessageID) + sizeof(MafiaNet::Time))
		{
			packetIdentifier = (unsigned char) packet->data[sizeof(unsigned char) + sizeof(MafiaNet::Time)];
			// Required for proper endian swapping
			MafiaNet::BitStream tsBs(packet->data + sizeof(MessageID), packet->length - 1, false);
			tsBs.Read(timestamp);
			// Next line assumes worldId is only 1 byte
			RakAssert(sizeof(WorldId) == 1);
			incomingWorldId = packet->data[sizeof(unsigned char) * 2 + sizeof(MafiaNet::Time)];
			packetDataOffset = sizeof(unsigned char) * 3 + sizeof(MafiaNet::Time);
		}
		else
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
	}
	else
	{
		packetIdentifier = (unsigned char) packet->data[0];
		// Next line assumes worldId is only 1 byte
		RakAssert(sizeof(WorldId) == 1);
		incomingWorldId = packet->data[sizeof(unsigned char)];
		packetDataOffset = sizeof(unsigned char) * 2;
	}

	{
		ListLock listLock(m_WorldListMutex);
		if (worldsArray[incomingWorldId] == 0)
			return RR_CONTINUE_PROCESSING;
	}

	switch (packetIdentifier)
	{
	case ID_REPLICA_MANAGER_CONSTRUCTION:
		return OnConstruction(packet, packet->data, packet->length, packet->guid, packetDataOffset, incomingWorldId);
	case ID_REPLICA_MANAGER_SERIALIZE:
		return OnSerialize(packet, packet->data, packet->length, packet->guid, timestamp, packetDataOffset, incomingWorldId);
	case ID_REPLICA_MANAGER_DOWNLOAD_STARTED:
		if (packet->wasGeneratedLocally == false)
		{
			return OnDownloadStarted(packet, packet->data, packet->length, packet->guid, packetDataOffset, incomingWorldId);
		}
		else
			break;
	case ID_REPLICA_MANAGER_DOWNLOAD_COMPLETE:
		if (packet->wasGeneratedLocally == false)
		{
			return OnDownloadComplete(packet, packet->data, packet->length, packet->guid, packetDataOffset, incomingWorldId);
		}
		else
			break;
	case ID_REPLICA_MANAGER_SCOPE_CHANGE:
	{
		RM3ConnectionLock *lock;
		Connection_RM3 *connection = LockConnectionByGUID(packet->guid, incomingWorldId, lock);
		ConnectionLockHolder connectionLock(lock);
		if (connection && connection->isValidated == false)
		{
			// This connection is now confirmed bidirectional
			connection->isValidated = true;
			// Reply back on validation
			connection->SendValidation(rakPeerInterface, incomingWorldId);
		}
	}
	}

	return RR_CONTINUE_PROCESSING;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::AutoConstructByQuery(ReplicaManager3 *replicaManager3, WorldId worldId)
{
	ValidateLists(replicaManager3);

	ConstructionMode curConstructionMode = QueryConstructionMode();

	unsigned int index;
	RM3ConstructionState constructionState;
	LastSerializationResult *lsr;
	index = 0;

	constructedReplicasCulled.clear();
	destroyedReplicasCulled.clear();

	if (curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION)
	{
		while (index < queryToConstructReplicaList.size())
		{
			lsr = queryToConstructReplicaList[index];
			constructionState = lsr->replica->QueryConstruction(this, replicaManager3);
			if (constructionState == RM3CS_ALREADY_EXISTS_REMOTELY || constructionState == RM3CS_ALREADY_EXISTS_REMOTELY_DO_NOT_CONSTRUCT)
			{
				OnReplicaAlreadyExists(index, replicaManager3);
				if (constructionState == RM3CS_ALREADY_EXISTS_REMOTELY)
					constructedReplicasCulled.push_back(lsr->replica);

				/*
				if (constructionState==RM3CS_ALREADY_EXISTS_REMOTELY)
				{
					// Serialize construction data to this connection
					MafiaNet::BitStream bsOut;
					bsOut.Write((MessageID)ID_REPLICA_MANAGER_3_SERIALIZE_CONSTRUCTION_EXISTING);
					bsOut.Write(replicaManager3->GetWorldID());
					NetworkID networkId;
					networkId=lsr->replica->GetNetworkID();
					bsOut.Write(networkId);
					BitSize_t bitsWritten = bsOut.GetNumberOfBitsUsed();
					lsr->replica->SerializeConstructionExisting(&bsOut, this);
					if (bsOut.GetNumberOfBitsUsed()!=bitsWritten)
						replicaManager3->SendUnified(&bsOut,MafiaNet::Priority::High,MafiaNet::Reliability::ReliableOrdered,0,GetSystemAddress(), false);
				}

				// Serialize first serialization to this connection.
				// This is done here, as it isn't done in PushConstruction
				SerializeParameters sp;
				MafiaNet::BitStream emptyBs;
				for (index=0; index < (unsigned int) RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; index++)
				{
					sp.lastSentBitstream[index]=&emptyBs;
					sp.pro[index]=replicaManager3->GetDefaultSendParameters();
				}
				sp.bitsWrittenSoFar=0;
				sp.destinationConnection=this;
				sp.messageTimestamp=0;
				sp.whenLastSerialized=0;

				MafiaNet::Replica3 *replica = lsr->replica;

				RM3SerializationResult res = replica->Serialize(&sp);
				if (res!=RM3SR_NEVER_SERIALIZE_FOR_THIS_CONNECTION &&
					res!=RM3SR_DO_NOT_SERIALIZE &&
					res!=RM3SR_SERIALIZED_UNIQUELY)
				{
					bool allIndices[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
					for (int z=0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
					{
						sp.bitsWrittenSoFar+=sp.outputBitstream[z].GetNumberOfBitsUsed();
						allIndices[z]=true;
					}
					if (SendSerialize(replica, allIndices, sp.outputBitstream, sp.messageTimestamp, sp.pro, replicaManager3->GetRakPeerInterface(), replicaManager3->GetWorldID())==SSICR_SENT_DATA)
						lsr->replica->whenLastSerialized=MafiaNet::GetTimeMS();
				}
				*/
			}
			else if (constructionState == RM3CS_SEND_CONSTRUCTION)
			{
				OnConstructToThisConnection(index, replicaManager3);
				RakAssert(lsr->replica);
				constructedReplicasCulled.push_back(lsr->replica);
			}
			else if (constructionState == RM3CS_NEVER_CONSTRUCT)
			{
				OnNeverConstruct(index, replicaManager3);
			}
			else //  if (constructionState==RM3CS_NO_ACTION)
			{
				// Do nothing
				index++;
			}
		}

		if (curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION)
		{
			RM3DestructionState destructionState;
			index = 0;
			while (index < queryToDestructReplicaList.size())
			{
				lsr = queryToDestructReplicaList[index];
				destructionState = lsr->replica->QueryDestruction(this, replicaManager3);
				if (destructionState == RM3DS_SEND_DESTRUCTION)
				{
					OnSendDestructionFromQuery(index, replicaManager3);
					destroyedReplicasCulled.push_back(lsr->replica);
				}
				else if (destructionState == RM3DS_DO_NOT_QUERY_DESTRUCTION)
				{
					OnDoNotQueryDestruction(index, replicaManager3);
				}
				else //  if (destructionState==RM3CS_NO_ACTION)
				{
					// Do nothing
					index++;
				}
			}
		}
	}
	else if (curConstructionMode == QUERY_CONNECTION_FOR_REPLICA_LIST)
	{
		QueryReplicaList(constructedReplicasCulled, destroyedReplicasCulled);

		unsigned int idx1, idx2;

		// Create new
		for (idx2 = 0; idx2 < constructedReplicasCulled.size(); idx2++)
			OnConstructToThisConnection(constructedReplicasCulled[idx2], replicaManager3);

		for (idx2 = 0; idx2 < destroyedReplicasCulled.size(); idx2++)
		{
			bool objectExists;
			idx1 = FindConstructed(constructedReplicaList, destroyedReplicasCulled[idx2], &objectExists);
			if (objectExists)
			{
				// The record is this connection's alone, and streaming the replica back in makes a new one.
				lsr = constructedReplicaList[idx1];
				{
					std::lock_guard<std::mutex> constructedLock(lock->constructed);
					constructedReplicaList.erase(constructedReplicaList.begin() + idx1);
				}

				unsigned int j;
				for (j = 0; j < queryToSerializeReplicaList.size(); j++)
				{
					if (queryToSerializeReplicaList[j] == lsr)
					{
						queryToSerializeReplicaList.erase(queryToSerializeReplicaList.begin() + j);
						break;
					}
				}
				lock->records.Release(lsr);
			}
		}
	}

	SendConstruction(constructedReplicasCulled, destroyedReplicasCulled, replicaManager3->defaultSendParameters, replicaManager3->rakPeerInterface, worldId, replicaManager3);
}
void ReplicaManager3::Update(void)
{
	unsigned int index, index3;

	WorldId worldId;
	RM3World *world;
	MafiaNet::Time time = MafiaNet::GetTime();

	// A game callback inside the pass -- a Serialize(), or anything that reaches RakPeer::Receive() -- would
	// start a second pass inside this one: on POSIX it locks a mutex this one holds, and anywhere it overwrites
	// this pass's state. The pass already under way covers it.
	// So would one made while this thread handles a connection's message. Another thread's Update(), or the
	// receive thread changing the world, is waited for instead.
	if (connectionLocksHeld > 0 && connectionLocksOwner == this)
		return;
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	if (inUpdate)
		return;
	inUpdate = true;

	for (index3 = 0; index3 < worldsList.size(); index3++)
	{
		world = worldsList[index3];
		worldId = world->worldId;

		for (index = 0; index < world->connectionList.size(); index++)
		{
			ConnectionLockHolder connectionLock(LockConnection(world->connectionList[index]));
			if (world->connectionList[index]->isValidated == false)
				continue;
			world->connectionList[index]->AutoConstructByQuery(this, worldId);
		}
	}

	if (time - lastAutoSerializeOccurance >= autoSerializeInterval)
	{
		for (index3 = 0; index3 < worldsList.size(); index3++)
		{
			world = worldsList[index3];
			worldId = world->worldId;

			for (index = 0; index < world->userReplicaList.size(); index++)
			{
				world->userReplicaList[index]->forceSendUntilNextUpdate = false;
				world->userReplicaList[index]->OnUserReplicaPreSerializeTick();
			}

			// A new pass: whatever replicas prepared for the last one is stale. 0 is never a pass.
			if (++serializePass == 0)
				serializePass = 1;
			inSerializePass = true;
			if (serializeExecutor != 0 && world->connectionList.size() > 1)
				SerializeConnectionsParallel(world, worldId, time);
			else
				SerializeConnections(world, worldId, time);
			inSerializePass = false;
		}

		lastAutoSerializeOccurance = time;
	}
	inUpdate = false;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ReplicaManager3::WorldGuard::WorldGuard(ReplicaManager3 &replicaManager)
    : replicaManager(replicaManager)
{
	replicaManager.worldMutex.lock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ReplicaManager3::WorldGuard::~WorldGuard()
{
	replicaManager.worldMutex.unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ReplicaManager3::StateDelivery::StateDelivery(ReplicaManager3 &replicaManager, RakNetGUID sender, WorldId worldId)
    : replicaManager(replicaManager), connection(0), lock(0), networkIDManager(0)
{
	connection = replicaManager.LockConnectionByGUID(sender, worldId, lock);
	if (connection)
	{
		ListLock listLock(replicaManager.m_WorldListMutex);
		networkIDManager = replicaManager.worldsArray[worldId] ? replicaManager.worldsArray[worldId]->networkIDManager : 0;
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ReplicaManager3::StateDelivery::~StateDelivery()
{
	ConnectionLockHolder connectionLock(lock);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool ReplicaManager3::StateDelivery::Deserialize(NetworkID networkId, DeserializeParameters *parameters)
{
	if (connection == 0 || networkIDManager == 0)
		return false;
	// From the lookup to the end of Deserialize(): the replica stays in the table and alive, see Dereference().
	ReplicaSetDelivery delivery(&replicaManager);
	Replica3 *replica = networkIDManager->GET_OBJECT_FROM_ID<Replica3 *>(networkId);
	if (replica == 0 || replica->replicaManager != &replicaManager || replica->referencingWorlds == 0)
		return false;
	parameters->sourceConnection = connection;
	replica->Deserialize(parameters);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3ConnectionLock *ReplicaManager3::LockConnection(Connection_RM3 *connection) const
{
	RM3ConnectionLock *lock = connection->lock;
	++lock->references;
	lock->mutex.lock();
	if (connectionLocksHeld++ == 0)
		connectionLocksOwner = this;
	return lock;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Connection_RM3 *ReplicaManager3::LockConnectionByGUID(RakNetGUID guid, WorldId worldId, RM3ConnectionLock *&lock) const
{
	Connection_RM3 *connection = 0;
	lock = 0;
	{
		// Its lock is taken hold of under the list lock: the connection cannot be freed before it has left the list.
		ListLock listLock(m_WorldListMutex);
		const RM3World *world = worldsArray[worldId];
		for (unsigned int index = 0; world != 0 && index < world->connectionList.size(); index++)
		{
			if (world->connectionList[index]->GetRakNetGUID() == guid)
			{
				connection = world->connectionList[index];
				lock = connection->lock;
				++lock->references;
				break;
			}
		}
	}
	if (connection == 0)
		return 0;

	lock->mutex.lock();
	// The count says whether it may have been freed, and comes first: only pointers are compared after it.
	if (lock->worlds == 0 || !HoldsConnection(connection, worldId))
	{
		// Popped from this world while this thread waited, and maybe freed since.
		lock->mutex.unlock();
		DropConnectionLock(lock);
		lock = 0;
		return 0;
	}
	if (connectionLocksHeld++ == 0)
		connectionLocksOwner = this;
	return connection;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace
{
// Which of a connection's replicas a call serves: all of them, or either side of the split a parallel
// pass makes between the broadcasts its workers serve and everything else.
enum class ServeShare
{
	All,
	Workers,
	Rest
};

// Returns false when a game callback dropped the connection, which must then not be touched again.
bool ServeSerializeList(Connection_RM3 *connection, std::vector<LastSerializationResult *> &list, SerializeParameters &sp, RakPeerInterface *rakPeer, WorldId worldId, ReplicaManager3 *replicaManager, MafiaNet::Time time, uint32_t pass, ServeShare share)
{
	uint32_t generation = replicaManager->GetMembershipGeneration();
	unsigned int index = 0;
	while (index < list.size())
	{
		LastSerializationResult *lsr = list[index];
		const uint32_t allocation = lsr->allocationNumber;
		if (lsr->servedPass == pass || (share != ServeShare::All && (lsr->replica->parallelPass == pass) != (share == ServeShare::Workers)))
		{
			index++;
			continue;
		}
		lsr->servedPass = pass;
		sp.destinationConnection = connection;
		sp.whenLastSerialized = lsr->whenLastSerialized;
		const SendSerializeIfChangedResult ssicr = connection->SendSerializeIfChanged(lsr, &sp, rakPeer, worldId, replicaManager, time);
		if (replicaManager->GetMembershipGeneration() != generation)
		{
			// A game callback dereferenced a replica or dropped a connection: this one may be gone, the record
			// freed, the list shifted. Start over, past everything already served.
			if (!replicaManager->HoldsConnection(connection, worldId))
				return false;
			generation = replicaManager->GetMembershipGeneration();
			if (ssicr == SSICR_SENT_DATA && connection->HoldsRecord(lsr) && lsr->allocationNumber == allocation)
				lsr->whenLastSerialized = time;
			index = 0;
			continue;
		}
		if (ssicr == SSICR_SENT_DATA)
		{
			lsr->whenLastSerialized = time;
			index++;
		}
		else if (ssicr != SSICR_NEVER_SERIALIZE) // NEVER removed it from the middle of the list
			index++;
	}
	return true;
}

// A connection's own serialization list (Connection_RM3::QuerySerializationList), each entry read from
// the connection's records: replica->lsr is shared by every connection, so for a replica not held here
// it is another connection's, possibly since freed.
// Returns false when a game callback dropped the connection, which must then not be touched again. The
// entries are the game's own list, held apart from ours, so a record is looked up for each one as it comes.
bool ServeOwnSerializeList(Connection_RM3 *connection, const ConstructedList &constructed, std::vector<Replica3 *> &replicasToSerialize, SerializeParameters &sp, RakPeerInterface *rakPeer, WorldId worldId, ReplicaManager3 *replicaManager, MafiaNet::Time time)
{
	for (unsigned int i = 0; i < replicasToSerialize.size(); i++)
	{
		bool held;
		const unsigned int recordIndex = FindConstructed(constructed, replicasToSerialize[i], &held);
		if (!held)
			continue;
		LastSerializationResult *lsr = constructed[recordIndex];
		const uint32_t allocation = lsr->allocationNumber;
		sp.destinationConnection = connection;
		sp.whenLastSerialized = lsr->whenLastSerialized;
		const uint32_t generation = replicaManager->GetMembershipGeneration();
		const SendSerializeIfChangedResult ssicr = connection->SendSerializeIfChanged(lsr, &sp, rakPeer, worldId, replicaManager, time);
		if (replicaManager->GetMembershipGeneration() != generation)
		{
			if (!replicaManager->HoldsConnection(connection, worldId))
				return false;
			if (!connection->HoldsRecord(lsr) || lsr->allocationNumber != allocation)
				continue;
		}
		if (ssicr == SSICR_SENT_DATA)
			lsr->whenLastSerialized = time;
	}
	return true;
}

struct ParallelSerializeContext
{
	std::vector<Connection_RM3 *> *connections;
	RakPeerInterface *rakPeer;
	WorldId worldId;
	ReplicaManager3 *replicaManager;
	MafiaNet::Time time;
	uint32_t pass;
};
} // namespace

void ReplicaManager3::SerializeConnections(RM3World *world, WorldId worldId, MafiaNet::Time time)
{
	SerializeParameters sp;
	sp.curTime = time;
	sp.messageTimestamp = 0;
	for (int i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; i++)
		sp.pro[i] = defaultSendParameters;
	std::vector<Replica3 *> replicasToSerialize;
	// By index into the live list, starting over (past what was served) when a game callback changed it.
	uint32_t generation = membershipGeneration;
	for (unsigned int index = 0; index < world->connectionList.size(); index++)
	{
		Connection_RM3 *connection = world->connectionList[index];
		if (connection->servedPass == serializePass)
			continue;
		ConnectionLockHolder connectionLock(LockConnection(connection));
		connection->servedPass = serializePass;
		sp.bitsWrittenSoFar = 0;
		replicasToSerialize.clear();
		if (connection->QuerySerializationList(replicasToSerialize))
			ServeOwnSerializeList(connection, connection->constructedReplicaList, replicasToSerialize, sp, GetRakPeerInterface(), worldId, this, time);
		else
			ServeSerializeList(connection, connection->queryToSerializeReplicaList, sp, GetRakPeerInterface(), worldId, this, time, serializePass, ServeShare::All);
		if (membershipGeneration != generation)
		{
			generation = membershipGeneration;
			index = (unsigned int) -1;
		}
	}
}

void ReplicaManager3::SerializeConnectionsParallel(RM3World *world, WorldId worldId, MafiaNet::Time time)
{
	SerializeParameters sp;
	sp.curTime = time;
	sp.messageTimestamp = 0;
	for (int i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; i++)
		sp.pro[i] = defaultSendParameters;

	// 1. Connections that choose their own list are served now, on this thread, as without an executor.
	// Game callbacks run throughout steps 1 to 3 and may drop connections, so nothing here holds one
	// across them: every loop reads the live list by index.
	std::vector<Replica3 *> replicasToSerialize;
	uint32_t generation = membershipGeneration;
	for (unsigned int index = 0; index < world->connectionList.size(); index++)
	{
		Connection_RM3 *connection = world->connectionList[index];
		if (connection->listQueriedPass == serializePass)
			continue;
		ConnectionLockHolder connectionLock(LockConnection(connection));
		connection->listQueriedPass = serializePass;
		replicasToSerialize.clear();
		connection->ownSerializationListPass = 0;
		if (connection->QuerySerializationList(replicasToSerialize))
		{
			connection->ownSerializationListPass = serializePass;
			sp.bitsWrittenSoFar = 0;
			ServeOwnSerializeList(connection, connection->constructedReplicaList, replicasToSerialize, sp, GetRakPeerInterface(), worldId, this, time);
		}
		if (membershipGeneration != generation)
		{
			generation = membershipGeneration;
			index = (unsigned int) -1;
		}
	}

	// 2. A broadcast is serialized only once some connection holding it asks for it this pass, as on one
	// thread: serialized for nobody, its new bytes would count as sent and never reach anyone.
	for (unsigned int index = 0; index < world->connectionList.size(); index++)
	{
		Connection_RM3 *connection = world->connectionList[index];
		if (connection->ownSerializationListPass == serializePass)
			continue;
		ConnectionLockHolder connectionLock(LockConnection(connection));
		const std::vector<LastSerializationResult *> &list = connection->queryToSerializeReplicaList;
		for (unsigned int i = 0; i < list.size(); i++)
		{
			Replica3 *replica = list[i]->replica;
			if (replica->broadcastsIdentically && replica->neededPass != serializePass && replica->GetNetworkID() != UNASSIGNED_NETWORK_ID && replica->QuerySerialization(connection) == RM3QSR_CALL_SERIALIZE)
				replica->neededPass = serializePass;
			if (membershipGeneration != generation)
			{
				// The list or the connection may be gone: start over; marking twice is harmless.
				generation = membershipGeneration;
				index = (unsigned int) -1;
				break;
			}
		}
	}

	// 3. Each of them serialized once, here, before any worker reads it. Only these are the workers'
	// share: a broadcast prepared later, by the first connection to reach it in step 6, is served there
	// to every connection.
	for (unsigned int index = 0; index < world->userReplicaList.size(); index++)
	{
		Replica3 *replica = world->userReplicaList[index];
		if (replica->neededPass != serializePass)
			continue;
		if (replica->preparedPass != serializePass)
			PrepareBroadcast(replica, time);
		if (replica->prepared != Replica3::PB_SERIALIZE_PER_CONNECTION)
			replica->parallelPass = serializePass;
		if (membershipGeneration != generation)
		{
			// A Serialize() changed the replica list: start over; what is prepared stays prepared.
			generation = membershipGeneration;
			index = (unsigned int) -1;
		}
	}

	// 4. The connections the workers serve, taken after every callback of steps 1 to 3 has run. While they
	// run, nothing may add or drop one (see SetSerializeExecutor).
	parallelConnections.clear();
	for (unsigned int index = 0; index < world->connectionList.size(); index++)
	{
		if (world->connectionList[index]->ownSerializationListPass != serializePass)
			parallelConnections.push_back(world->connectionList[index]);
	}

	// 5. Their share, in parallel.
	ParallelSerializeContext context;
	context.connections = &parallelConnections;
	context.rakPeer = GetRakPeerInterface();
	context.worldId = worldId;
	context.replicaManager = this;
	context.time = time;
	context.pass = serializePass;
	serializeExecutor->ParallelFor((unsigned int) parallelConnections.size(), &ReplicaManager3::ServePreparedBroadcasts, &context);
	parallelConnections.clear();

	// 6. The rest, per connection, on this thread, from the live list again.
	for (unsigned int index = 0; index < world->connectionList.size(); index++)
	{
		Connection_RM3 *connection = world->connectionList[index];
		if (connection->ownSerializationListPass == serializePass || connection->servedPass == serializePass)
			continue;
		ConnectionLockHolder connectionLock(LockConnection(connection));
		connection->servedPass = serializePass;
		sp.bitsWrittenSoFar = 0;
		ServeSerializeList(connection, connection->queryToSerializeReplicaList, sp, GetRakPeerInterface(), worldId, this, time, serializePass, ServeShare::Rest);
		if (membershipGeneration != generation)
		{
			generation = membershipGeneration;
			index = (unsigned int) -1;
		}
	}
}

void ReplicaManager3::ServePreparedBroadcasts(void *context, unsigned int index)
{
	const ParallelSerializeContext &pass = *static_cast<ParallelSerializeContext *>(context);
	Connection_RM3 *connection = (*pass.connections)[index];
	ConnectionLockHolder connectionLock(pass.replicaManager->LockConnection(connection));
	SerializeParameters sp;
	sp.curTime = pass.time;
	sp.messageTimestamp = 0;
	sp.bitsWrittenSoFar = 0;
	for (int i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; i++)
		sp.pro[i] = pass.replicaManager->defaultSendParameters;
	ServeSerializeList(connection, connection->queryToSerializeReplicaList, sp, pass.rakPeer, pass.worldId, pass.replicaManager, pass.time, pass.pass, ServeShare::Workers);
}

void ReplicaManager3::PrepareBroadcast(Replica3 *replica, MafiaNet::Time time)
{
	replica->preparedPass = serializePass;
	replica->prepared = Replica3::PB_SERIALIZE_PER_CONNECTION;

	SerializeParameters sp;
	sp.curTime = time;
	sp.messageTimestamp = 0;
	sp.bitsWrittenSoFar = 0;
	sp.destinationConnection = 0;
	sp.whenLastSerialized = replica->lastBroadcast;
	for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
	{
		sp.pro[z] = defaultSendParameters;
		sp.lastSentBitstream[z] = &replica->lastSentSerialization.bitStream[z];
	}

	const RM3SerializationResult result = replica->Serialize(&sp);
	if (result == RM3SR_DO_NOT_SERIALIZE)
	{
		replica->prepared = Replica3::PB_NOTHING_TO_SEND;
		return;
	}
	if (result != RM3SR_BROADCAST_IDENTICALLY && result != RM3SR_BROADCAST_IDENTICALLY_FORCE_SERIALIZATION)
	{
		// Broke its promise: served per connection, Serialize() and all, as any other replica.
		return;
	}

	bool any = false;
	for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
	{
		MafiaNet::BitStream &out = sp.outputBitstream[z];
		MafiaNet::BitStream &last = replica->lastSentSerialization.bitStream[z];
		const bool changed = out.GetNumberOfBitsUsed() > 0 &&
		                     (result == RM3SR_BROADCAST_IDENTICALLY_FORCE_SERIALIZATION || out.GetNumberOfBitsUsed() != last.GetNumberOfBitsUsed() || memcmp(out.GetData(), last.GetData(), out.GetNumberOfBytesUsed()) != 0);
		replica->lastSentSerialization.indicesToSend[z] = changed;
		if (changed)
		{
			last.Reset();
			out.ResetReadPointer();
			last.Write(&out);
			any = true;
		}
	}
	// Anything else reaching this replica this pass (a connection's own list) sends the same bytes.
	replica->forceSendUntilNextUpdate = true;
	replica->prepared = any ? Replica3::PB_SEND : Replica3::PB_NOTHING_TO_SEND;
	if (!any)
		return;
	replica->preparedTimestamp = sp.messageTimestamp;
	for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		replica->preparedPro[z] = sp.pro[z];
	replica->lastBroadcast = time;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) systemAddress;
	if (autoDestroyConnections)
	{
		Connection_RM3 *connection = PopConnection(rakNetGUID);
		if (connection)
			DeallocConnection(connection);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming)
{
	(void) isIncoming;
	if (autoCreateConnections)
	{
		Connection_RM3 *connection = AllocConnection(systemAddress, rakNetGUID);
		if (connection)
			PushConnection(connection);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void ReplicaManager3::OnRakPeerShutdown(void)
{
	if (autoDestroyConnections)
	{
		RM3World *world;
		unsigned int index3;
		std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
		for (index3 = 0; index3 < worldsList.size(); index3++)
		{
			world = worldsList[index3];

			while (world->connectionList.size())
			{
				Connection_RM3 *connection = PopConnection((unsigned int) (world->connectionList.size() - 1), world->worldId);
				if (connection)
					DeallocConnection(connection);
			}
		}
	}


	Clear(false);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

PluginReceiveResult ReplicaManager3::OnConstruction(Packet *packet, unsigned char *packetData, int packetDataLength, RakNetGUID senderGuid, unsigned char packetDataOffset, WorldId worldId)
{
	// From nobody we know: not worth waiting for the world.
	if (GetConnectionByGUID(senderGuid, worldId) == 0)
	{
		RakAssert("Got OnConstruction but no connection yet" && 0);
		return RR_CONTINUE_PROCESSING;
	}

	// What it constructs and destroys is the world's, for every connection.
	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	RM3World *world = worldsArray[worldId];

	RM3ConnectionLock *lock;
	Connection_RM3 *connection = LockConnectionByGUID(senderGuid, worldId, lock);
	ConnectionLockHolder connectionLock(lock);
	if (connection == 0)
	{
		// Almost certainly a bug
		RakAssert("Got OnConstruction but no connection yet" && 0);
		return RR_CONTINUE_PROCESSING;
	}
	if (connection->groupConstructionAndSerialize)
	{
		connection->downloadGroup.push_back(packet);
		return RR_STOP_PROCESSING;
	}

	MafiaNet::BitStream bsIn(packetData, packetDataLength, false);
	bsIn.IgnoreBytes(packetDataOffset);
	uint16_t constructionObjectListSize, destructionObjectListSize, index, index2;
	BitSize_t streamEnd, writeAllocationIDEnd;
	Replica3 *replica;
	NetworkID networkId;
	RakNetGUID creatingSystemGuid;
	bool actuallyCreateObject = false;

	std::vector<bool> actuallyCreateObjectList;
	std::vector<Replica3 *> constructionTickStack;

	RakAssert(world->networkIDManager);

	bsIn.Read(constructionObjectListSize);
	for (index = 0; index < constructionObjectListSize; index++)
	{
		bsIn.Read(streamEnd);
		bsIn.Read(networkId);
		Replica3 *existingReplica = world->networkIDManager->GET_OBJECT_FROM_ID<Replica3 *>(networkId);
		bsIn.Read(actuallyCreateObject);
		actuallyCreateObjectList.push_back(actuallyCreateObject);
		bsIn.AlignReadToByteBoundary();

		if (actuallyCreateObject)
		{
			bsIn.Read(creatingSystemGuid);
			bsIn.Read(writeAllocationIDEnd);

			//printf("OnConstruction: %i\n",networkId.guid.g); // Removeme
			if (existingReplica)
			{
				existingReplica->replicaManager = this;

				// Network ID already in use
				connection->OnDownloadExisting(existingReplica, this);

				constructionTickStack.push_back(0);
				bsIn.SetReadOffset(streamEnd);
				continue;
			}

			bsIn.AlignReadToByteBoundary();
			replica = connection->AllocReplica(&bsIn, this);
			if (replica == 0)
			{
				constructionTickStack.push_back(0);
				bsIn.SetReadOffset(streamEnd);
				continue;
			}

			// Go past the bitStream written to with WriteAllocationID(). Necessary in case the user didn't read out the bitStream the same way it was written
			// bitOffset2 is already aligned
			bsIn.SetReadOffset(writeAllocationIDEnd);

			replica->SetNetworkIDManager(world->networkIDManager);
			replica->SetNetworkID(networkId);

			replica->replicaManager = this;
			replica->creatingSystemGUID = creatingSystemGuid;

			if (!replica->QueryRemoteConstruction(connection) ||
			    !replica->DeserializeConstruction(&bsIn, connection))
			{
				DeallocReplicaNoBroadcastDestruction(connection, replica);
				bsIn.SetReadOffset(streamEnd);
				constructionTickStack.push_back(0);
				continue;
			}

			constructionTickStack.push_back(replica);

			// Register the replica
			ReferenceInternal(replica, worldId);
		}
		else
		{
			if (existingReplica)
			{
				existingReplica->DeserializeConstructionExisting(&bsIn, connection);
				constructionTickStack.push_back(existingReplica);
			}
			else
			{
				constructionTickStack.push_back(0);
			}
		}


		bsIn.SetReadOffset(streamEnd);
		bsIn.AlignReadToByteBoundary();
	}

	RakAssert(constructionTickStack.size() == constructionObjectListSize);
	RakAssert(actuallyCreateObjectList.size() == constructionObjectListSize);

	MafiaNet::BitStream empty;
	for (index = 0; index < constructionObjectListSize; index++)
	{
		bool pdcWritten = false;
		bsIn.Read(pdcWritten);
		if (pdcWritten)
		{
			bsIn.AlignReadToByteBoundary();
			bsIn.Read(streamEnd);
			bsIn.Read(networkId);
			if (constructionTickStack[index] != 0)
			{
				bsIn.AlignReadToByteBoundary();
				if (actuallyCreateObjectList[index])
					constructionTickStack[index]->PostDeserializeConstruction(&bsIn, connection);
				else
					constructionTickStack[index]->PostDeserializeConstructionExisting(&bsIn, connection);
			}
			bsIn.SetReadOffset(streamEnd);
		}
		else
		{
			if (constructionTickStack[index] != 0)
			{
				if (actuallyCreateObjectList[index])
					constructionTickStack[index]->PostDeserializeConstruction(&empty, connection);
				else
					constructionTickStack[index]->PostDeserializeConstructionExisting(&empty, connection);
			}
		}
	}
	bsIn.AlignReadToByteBoundary();

	for (index = 0; index < constructionObjectListSize; index++)
	{
		if (constructionTickStack[index] != 0)
		{
			if (actuallyCreateObjectList[index])
			{
				// Tell the connection(s) that this object exists since they just sent it to us
				connection->OnDownloadFromThisSystem(constructionTickStack[index], this);

				for (index2 = 0; index2 < world->connectionList.size(); index2++)
				{
					if (world->connectionList[index2] != connection)
						world->connectionList[index2]->OnDownloadFromOtherSystem(constructionTickStack[index], this);
				}
			}
		}
	}

	// Destructions
	bool b = bsIn.Read(destructionObjectListSize);
	(void) b;
	RakAssert(b);
	for (index = 0; index < destructionObjectListSize; index++)
	{
		bsIn.Read(networkId);
		bsIn.Read(streamEnd);
		replica = world->networkIDManager->GET_OBJECT_FROM_ID<Replica3 *>(networkId);
		if (replica == 0)
		{
			// Unknown object
			bsIn.SetReadOffset(streamEnd);
			continue;
		}
		bsIn.Read(replica->deletingSystemGUID);
		if (replica->DeserializeDestruction(&bsIn, connection))
		{
			// Make sure it wasn't deleted in DeserializeDestruction
			if (world->networkIDManager->GET_OBJECT_FROM_ID<Replica3 *>(networkId))
			{
				replica->PreDestruction(connection);

				// Forward deletion by remote system
				if (replica->QueryRelayDestruction(connection))
				{
					std::vector<Replica3 *> replicas{replica};
					BroadcastDestructionList(replicas, connection->GetSystemAddress(), worldId);
				}
				Dereference(replica, worldId);
				DeallocReplicaNoBroadcastDestruction(connection, replica);
			}
		}
		else
		{
			replica->PreDestruction(connection);
			connection->OnDereference(replica, this);
		}

		bsIn.AlignReadToByteBoundary();
	}
	return RR_CONTINUE_PROCESSING;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

PluginReceiveResult ReplicaManager3::OnSerialize(Packet *packet, unsigned char *packetData, int packetDataLength, RakNetGUID senderGuid, MafiaNet::Time timestamp, unsigned char packetDataOffset, WorldId worldId)
{
	RM3ConnectionLock *lock;
	Connection_RM3 *connection = LockConnectionByGUID(senderGuid, worldId, lock);
	ConnectionLockHolder connectionLock(lock);
	if (connection == 0)
		return RR_CONTINUE_PROCESSING;
	if (connection->groupConstructionAndSerialize)
	{
		connection->downloadGroup.push_back(packet);
		return RR_STOP_PROCESSING;
	}

	NetworkIDManager *networkIDManager;
	{
		ListLock listLock(m_WorldListMutex);
		networkIDManager = worldsArray[worldId] ? worldsArray[worldId]->networkIDManager : 0;
	}
	RakAssert(networkIDManager);
	if (networkIDManager == 0)
		return RR_CONTINUE_PROCESSING;
	MafiaNet::BitStream bsIn(packetData, packetDataLength, false);
	bsIn.IgnoreBytes(packetDataOffset);

	struct DeserializeParameters ds;
	ds.timeStamp = timestamp;
	ds.sourceConnection = connection;

	Replica3 *replica;
	NetworkID networkId;
	BitSize_t bitsUsed;
	bsIn.Read(networkId);
	//printf("OnSerialize: %i\n",networkId.guid.g); // Removeme
	// From the lookup to the end of Deserialize(): the replica stays in the table and alive, see Dereference().
	ReplicaSetDelivery delivery(this);
	replica = networkIDManager->GET_OBJECT_FROM_ID<Replica3 *>(networkId);
	if (replica && replica->replicaManager == this && replica->referencingWorlds > 0)
	{
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			bsIn.Read(ds.bitstreamWrittenTo[z]);
			if (ds.bitstreamWrittenTo[z])
			{
				bsIn.ReadCompressed(bitsUsed);
				bsIn.AlignReadToByteBoundary();
				bsIn.Read(ds.serializationBitstream[z], bitsUsed);
			}
		}
		replica->Deserialize(&ds);
	}
	return RR_CONTINUE_PROCESSING;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

PluginReceiveResult ReplicaManager3::OnDownloadStarted(Packet *packet, unsigned char *packetData, int packetDataLength, RakNetGUID senderGuid, unsigned char packetDataOffset, WorldId worldId)
{
	RM3ConnectionLock *lock;
	Connection_RM3 *connection = LockConnectionByGUID(senderGuid, worldId, lock);
	ConnectionLockHolder connectionLock(lock);
	if (connection == 0)
		return RR_CONTINUE_PROCESSING;
	if (connection->QueryGroupDownloadMessages() &&
	    // ID_DOWNLOAD_STARTED will be processed twice, being processed the second time once ID_DOWNLOAD_COMPLETE arrives.
	    // However, the second time groupConstructionAndSerialize will be set to true so it won't be processed a third time
	    connection->groupConstructionAndSerialize == false)
	{
		// These messages will be held by the plugin and returned when the download is complete
		connection->groupConstructionAndSerialize = true;
		RakAssert(connection->downloadGroup.size() == 0);
		connection->downloadGroup.push_back(packet);
		return RR_STOP_PROCESSING;
	}

	connection->groupConstructionAndSerialize = false;
	MafiaNet::BitStream bsIn(packetData, packetDataLength, false);
	bsIn.IgnoreBytes(packetDataOffset);
	connection->DeserializeOnDownloadStarted(&bsIn);
	return RR_CONTINUE_PROCESSING;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

PluginReceiveResult ReplicaManager3::OnDownloadComplete(Packet *packet, unsigned char *packetData, int packetDataLength, RakNetGUID senderGuid, unsigned char packetDataOffset, WorldId worldId)
{
	RM3ConnectionLock *lock;
	Connection_RM3 *connection = LockConnectionByGUID(senderGuid, worldId, lock);
	ConnectionLockHolder connectionLock(lock);
	if (connection == 0)
		return RR_CONTINUE_PROCESSING;

	if (connection->groupConstructionAndSerialize == true && connection->downloadGroup.size() > 0)
	{
		// Push back buffered packets in front of this one
		unsigned int i;
		for (i = 0; i < connection->downloadGroup.size(); i++)
			rakPeerInterface->PushBackPacket(connection->downloadGroup[i], false);

		// Push this one to be last too. It will be processed again, but the second time
		// groupConstructionAndSerialize will be false and downloadGroup will be empty, so it will go past this block
		connection->downloadGroup.clear();
		rakPeerInterface->PushBackPacket(packet, false);

		return RR_STOP_PROCESSING;
	}

	MafiaNet::BitStream bsIn(packetData, packetDataLength, false);
	bsIn.IgnoreBytes(packetDataOffset);
	connection->gotDownloadComplete = true;
	connection->DeserializeOnDownloadComplete(&bsIn);
	return RR_CONTINUE_PROCESSING;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Replica3 *ReplicaManager3::GetReplicaByNetworkID(NetworkID networkId, WorldId worldId)
{
	RM3World *world = worldsArray[worldId];

	unsigned int i;
	for (i = 0; i < world->userReplicaList.size(); i++)
	{
		if (world->userReplicaList[i]->GetNetworkID() == networkId)
			return world->userReplicaList[i];
	}
	return 0;
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------


void ReplicaManager3::BroadcastDestructionList(std::vector<Replica3 *> &replicaListSource, const SystemAddress &exclusionAddress, WorldId worldId)
{
	MafiaNet::BitStream bsOut;
	unsigned int i, j;

	std::lock_guard<std::recursive_mutex> worldLock(worldMutex);
	RakAssert(worldsArray[worldId] != 0 && "World not in use");
	RM3World *world = worldsArray[worldId];

	std::vector<Replica3 *> replicaList;

	for (i = 0; i < replicaListSource.size(); i++)
	{
		if (replicaListSource[i] == currentlyDeallocatingReplica)
			continue;
		replicaList.push_back(replicaListSource[i]);
	}

	if (replicaList.size() == 0)
		return;

	for (i = 0; i < replicaList.size(); i++)
	{
		if (replicaList[i]->deletingSystemGUID == UNASSIGNED_RAKNET_GUID)
			replicaList[i]->deletingSystemGUID = GetRakPeerInterface()->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
	}

	for (j = 0; j < world->connectionList.size(); j++)
	{
		if (world->connectionList[j]->GetSystemAddress() == exclusionAddress)
			continue;

		bsOut.Reset();
		bsOut.Write((MessageID) ID_REPLICA_MANAGER_CONSTRUCTION);
		bsOut.Write(worldId);
		uint16_t cnt = 0;
		bsOut.Write(cnt); // No construction
		cnt = (uint16_t) replicaList.size();
		BitSize_t cntOffset = bsOut.GetWriteOffset();
		;
		bsOut.Write(cnt); // Overwritten at send call
		cnt = 0;

		for (i = 0; i < replicaList.size(); i++)
		{
			if (world->connectionList[j]->HasReplicaConstructed(replicaList[i]) == false)
				continue;
			cnt++;

			NetworkID networkId;
			networkId = replicaList[i]->GetNetworkID();
			bsOut.Write(networkId);
			BitSize_t offsetStart, offsetEnd;
			offsetStart = bsOut.GetWriteOffset();
			bsOut.Write(offsetStart);
			bsOut.Write(replicaList[i]->deletingSystemGUID);
			replicaList[i]->SerializeDestruction(&bsOut, world->connectionList[j]);
			bsOut.AlignWriteToByteBoundary();
			offsetEnd = bsOut.GetWriteOffset();
			bsOut.SetWriteOffset(offsetStart);
			bsOut.Write(offsetEnd);
			bsOut.SetWriteOffset(offsetEnd);
		}

		if (cnt > 0)
		{
			BitSize_t curOffset = bsOut.GetWriteOffset();
			bsOut.SetWriteOffset(cntOffset);
			bsOut.Write(cnt);
			bsOut.SetWriteOffset(curOffset);
			rakPeerInterface->Send(&bsOut, defaultSendParameters.priority, defaultSendParameters.reliability, defaultSendParameters.orderingChannel, world->connectionList[j]->GetSystemAddress(), false, defaultSendParameters.sendReceipt);
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------


void ReplicaManager3::BroadcastDestruction(Replica3 *replica, const SystemAddress &exclusionAddress)
{
	std::vector<Replica3 *> replicaList;
	replicaList.push_back(replica);
	BroadcastDestructionList(replicaList, exclusionAddress);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Connection_RM3::Connection_RM3(const SystemAddress &_systemAddress, RakNetGUID _guid)
    : systemAddress(_systemAddress), guid(_guid)
{
	isValidated = false;
	ownSerializationListPass = 0;
	listQueriedPass = 0;
	servedPass = 0;
	isFirstConstruction = true;
	groupConstructionAndSerialize = false;
	gotDownloadComplete = false;
	virtualWorld = VIRTUAL_WORLD_DEFAULT;
	lock = new RM3ConnectionLock;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Connection_RM3::~Connection_RM3()
{
	// Derived connections can supply heap records through the protected lists.
	// Records the library supplies are owned by this connection's store.
	for (auto *record : constructedReplicaList)
		if (record->allocationNumber == 0)
			delete record;
	for (auto *record : queryToConstructReplicaList)
		if (record->allocationNumber == 0)
			delete record;
	if (lock)
		DropConnectionLock(lock);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::GetConstructedReplicas(std::vector<Replica3 *> &objectsTheyDoHave)
{
	objectsTheyDoHave.clear();
	std::lock_guard<std::mutex> constructedLock(lock->constructed);
	for (unsigned int idx = 0; idx < constructedReplicaList.size(); idx++)
	{
		objectsTheyDoHave.push_back(constructedReplicaList[idx]->replica);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool Connection_RM3::HasReplicaConstructed(MafiaNet::Replica3 *replica)
{
	bool objectExists;
	std::lock_guard<std::mutex> constructedLock(lock->constructed);
	FindConstructed(constructedReplicaList, replica, &objectExists);
	return objectExists;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Connection_RM3::SendSerializeHeader(MafiaNet::Replica3 *replica, MafiaNet::Time timestamp, MafiaNet::BitStream *bs, WorldId worldId)
{
	bs->Reset();

	if (timestamp != 0)
	{
		bs->Write((MessageID) ID_TIMESTAMP);
		bs->Write(timestamp);
	}
	bs->Write((MessageID) ID_REPLICA_MANAGER_SERIALIZE);
	bs->Write(worldId);
	bs->Write(replica->GetNetworkID());
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Connection_RM3::ClearDownloadGroup(RakPeerInterface *rakPeerInterface)
{
	unsigned int i;
	for (i = 0; i < downloadGroup.size(); i++)
		rakPeerInterface->DeallocatePacket(downloadGroup[i]);
	downloadGroup.clear();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
SendSerializeIfChangedResult Connection_RM3::SendSerialize(MafiaNet::Replica3 *replica, bool indicesToSend[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS], MafiaNet::BitStream serializationData[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS], MafiaNet::Time timestamp, PRO sendParameters[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS], RakPeerInterface *rakPeer, unsigned char worldId, MafiaNet::Time curTime)
{
	// A channel goes out only when it is selected and holds data. The selection is what counts: a
	// replica's cached broadcast record keeps the bytes of channels it no longer selects.
	bool pending[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
	bool anyPending = false;
	for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
	{
		pending[z] = indicesToSend[z] && serializationData[z].GetNumberOfBitsUsed() > 0;
		anyPending = anyPending || pending[z];
	}

	MafiaNet::BitStream out;
	BitSize_t bitsPerChannel[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];

	if (anyPending == false)
	{
		memset(bitsPerChannel, 0, sizeof(bitsPerChannel));
		replica->OnSerializeTransmission(&out, this, bitsPerChannel, curTime);
		return SSICR_DID_NOT_SEND_DATA;
	}

	RakAssert(replica->GetNetworkID() != UNASSIGNED_NETWORK_ID);

	// One message per distinct set of send parameters, in channel order. Channels that share their
	// parameters share the message; a group with nothing to carry is never sent, so a state-only
	// change no longer costs an empty message on the channel before it.
	for (int first = 0; first < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; first++)
	{
		if (pending[first] == false)
			continue;

		const PRO pro = sendParameters[first];
		SendSerializeHeader(replica, timestamp, &out, worldId);
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			const bool channelHasData = pending[z] && sendParameters[z] == pro;
			out.Write(channelHasData);
			if (channelHasData)
			{
				bitsPerChannel[z] = serializationData[z].GetNumberOfBitsUsed();
				out.WriteCompressed(bitsPerChannel[z]);
				out.AlignWriteToByteBoundary();
				// Copied from its bytes, leaving the source untouched: a parallel serialize pass has several
				// connections send the same replica's bitstream at once.
				out.WriteBits(serializationData[z].GetData(), bitsPerChannel[z], false);
				pending[z] = false;
			}
			else
			{
				bitsPerChannel[z] = 0;
			}
		}
		replica->OnSerializeTransmission(&out, this, bitsPerChannel, curTime);
		rakPeer->Send(&out, pro.priority, pro.reliability, pro.orderingChannel, systemAddress, false, pro.sendReceipt);
	}
	return SSICR_SENT_DATA;
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

SendSerializeIfChangedResult Connection_RM3::SendSerializeIfChanged(LastSerializationResult *lsr, SerializeParameters *sp, MafiaNet::RakPeerInterface *rakPeer, unsigned char worldId, ReplicaManager3 *replicaManager, MafiaNet::Time curTime)
{
	MafiaNet::Replica3 *replica = lsr->replica;

	if (replica->GetNetworkID() == UNASSIGNED_NETWORK_ID)
		return SSICR_DID_NOT_SEND_DATA;

	// A game callback below may dereference this replica, which frees lsr, or drop this connection. Past each
	// one, neither is touched again unless both are still here; the caller sees SSICR_NEVER_SERIALIZE.
	const uint32_t generation = replicaManager->GetMembershipGeneration();
	const uint32_t allocation = lsr->allocationNumber;
	const auto gone = [&]()
	{ return replicaManager->GetMembershipGeneration() != generation && (!replicaManager->HoldsConnection(this, worldId) || !HoldsRecord(lsr) || lsr->allocationNumber != allocation); };

	RM3QuerySerializationResult rm3qsr = replica->QuerySerialization(this);
	if (gone())
		return SSICR_NEVER_SERIALIZE;
	if (rm3qsr == RM3QSR_NEVER_CALL_SERIALIZE)
	{
		// Never again for this connection and replica pair
		OnNeverSerialize(lsr, replicaManager);
		return SSICR_NEVER_SERIALIZE;
	}

	if (rm3qsr == RM3QSR_DO_NOT_CALL_SERIALIZE)
		return SSICR_DID_NOT_SEND_DATA;

	// A replica that broadcasts identically is serialized once per pass, by whichever connection gets
	// to it first, and every connection after is handed what that produced -- or nothing at all.
	if (replica->broadcastsIdentically && replicaManager->inSerializePass)
	{
		if (replica->preparedPass != replicaManager->serializePass)
		{
			replicaManager->PrepareBroadcast(replica, curTime);
			if (gone())
				return SSICR_NEVER_SERIALIZE;
		}
		if (replica->prepared == Replica3::PB_NOTHING_TO_SEND)
			return SSICR_DID_NOT_SEND_DATA;
		if (replica->prepared == Replica3::PB_SEND)
		{
			for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
			{
				if (replica->lastSentSerialization.indicesToSend[z])
					sp->bitsWrittenSoFar += replica->lastSentSerialization.bitStream[z].GetNumberOfBitsUsed();
			}
			return SendSerialize(replica, replica->lastSentSerialization.indicesToSend, replica->lastSentSerialization.bitStream, replica->preparedTimestamp, replica->preparedPro, rakPeer, worldId, curTime);
		}
	}

	if (replica->forceSendUntilNextUpdate)
	{
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			if (replica->lastSentSerialization.indicesToSend[z])
				sp->bitsWrittenSoFar += replica->lastSentSerialization.bitStream[z].GetNumberOfBitsUsed();
		}
		return SendSerialize(replica, replica->lastSentSerialization.indicesToSend, replica->lastSentSerialization.bitStream, sp->messageTimestamp, sp->pro, rakPeer, worldId, curTime);
	}

	for (int i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; i++)
	{
		sp->outputBitstream[i].Reset();
		if (lsr->lastSerializationResultBS)
			sp->lastSentBitstream[i] = &lsr->lastSerializationResultBS->bitStream[i];
		else
			sp->lastSentBitstream[i] = &replica->lastSentSerialization.bitStream[i];
	}

	RM3SerializationResult serializationResult = replica->Serialize(sp);
	if (gone())
		return SSICR_NEVER_SERIALIZE;

	if (serializationResult == RM3SR_NEVER_SERIALIZE_FOR_THIS_CONNECTION)
	{
		// Never again for this connection and replica pair
		OnNeverSerialize(lsr, replicaManager);
		return SSICR_NEVER_SERIALIZE;
	}

	if (serializationResult == RM3SR_DO_NOT_SERIALIZE)
	{
		// Don't serialize this tick only
		return SSICR_DID_NOT_SEND_DATA;
	}

	// This is necessary in case the user in the Serialize() function for some reason read the bitstream they also wrote
	// WIthout this code, the Write calls to another bitstream would not write the entire bitstream
	BitSize_t sum = 0;
	for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
	{
		sp->outputBitstream[z].ResetReadPointer();
		sum += sp->outputBitstream[z].GetNumberOfBitsUsed();
	}

	if (sum == 0)
	{
		// Don't serialize this tick only
		return SSICR_DID_NOT_SEND_DATA;
	}

	if (serializationResult == RM3SR_SERIALIZED_ALWAYS)
	{
		bool allIndices[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			sp->bitsWrittenSoFar += sp->outputBitstream[z].GetNumberOfBitsUsed();
			allIndices[z] = true;

			lsr->AllocBS();
			lsr->lastSerializationResultBS->bitStream[z].Reset();
			lsr->lastSerializationResultBS->bitStream[z].Write(&sp->outputBitstream[z]);
			sp->outputBitstream[z].ResetReadPointer();
		}
		return SendSerialize(replica, allIndices, sp->outputBitstream, sp->messageTimestamp, sp->pro, rakPeer, worldId, curTime);
	}

	if (serializationResult == RM3SR_SERIALIZED_ALWAYS_IDENTICALLY)
	{
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			replica->lastSentSerialization.indicesToSend[z] = sp->outputBitstream[z].GetNumberOfBitsUsed() > 0;
			sp->bitsWrittenSoFar += sp->outputBitstream[z].GetNumberOfBitsUsed();
			replica->lastSentSerialization.bitStream[z].Reset();
			replica->lastSentSerialization.bitStream[z].Write(&sp->outputBitstream[z]);
			sp->outputBitstream[z].ResetReadPointer();
			replica->forceSendUntilNextUpdate = true;
		}
		return SendSerialize(replica, replica->lastSentSerialization.indicesToSend, sp->outputBitstream, sp->messageTimestamp, sp->pro, rakPeer, worldId, curTime);
	}

	bool indicesToSend[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
	if (serializationResult == RM3SR_BROADCAST_IDENTICALLY || serializationResult == RM3SR_BROADCAST_IDENTICALLY_FORCE_SERIALIZATION)
	{
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			if (sp->outputBitstream[z].GetNumberOfBitsUsed() > 0 &&
			    (serializationResult == RM3SR_BROADCAST_IDENTICALLY_FORCE_SERIALIZATION ||
			     ((sp->outputBitstream[z].GetNumberOfBitsUsed() != replica->lastSentSerialization.bitStream[z].GetNumberOfBitsUsed() ||
			       memcmp(sp->outputBitstream[z].GetData(), replica->lastSentSerialization.bitStream[z].GetData(), sp->outputBitstream[z].GetNumberOfBytesUsed()) != 0))))
			{
				indicesToSend[z] = true;
				replica->lastSentSerialization.indicesToSend[z] = true;
				sp->bitsWrittenSoFar += sp->outputBitstream[z].GetNumberOfBitsUsed();
				replica->lastSentSerialization.bitStream[z].Reset();
				replica->lastSentSerialization.bitStream[z].Write(&sp->outputBitstream[z]);
				sp->outputBitstream[z].ResetReadPointer();
				replica->forceSendUntilNextUpdate = true;
			}
			else
			{
				indicesToSend[z] = false;
				replica->lastSentSerialization.indicesToSend[z] = false;
			}
		}
	}
	else
	{
		lsr->AllocBS();

		// RM3SR_SERIALIZED_UNIQUELY
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			if (sp->outputBitstream[z].GetNumberOfBitsUsed() > 0 &&
			    (sp->outputBitstream[z].GetNumberOfBitsUsed() != lsr->lastSerializationResultBS->bitStream[z].GetNumberOfBitsUsed() ||
			     memcmp(sp->outputBitstream[z].GetData(), lsr->lastSerializationResultBS->bitStream[z].GetData(), sp->outputBitstream[z].GetNumberOfBytesUsed()) != 0))
			{
				indicesToSend[z] = true;
				sp->bitsWrittenSoFar += sp->outputBitstream[z].GetNumberOfBitsUsed();
				lsr->lastSerializationResultBS->bitStream[z].Reset();
				lsr->lastSerializationResultBS->bitStream[z].Write(&sp->outputBitstream[z]);
				sp->outputBitstream[z].ResetReadPointer();
			}
			else
			{
				indicesToSend[z] = false;
			}
		}
	}


	if (serializationResult == RM3SR_BROADCAST_IDENTICALLY || serializationResult == RM3SR_BROADCAST_IDENTICALLY_FORCE_SERIALIZATION)
		replica->forceSendUntilNextUpdate = true;

	// Send out the data
	return SendSerialize(replica, indicesToSend, sp->outputBitstream, sp->messageTimestamp, sp->pro, rakPeer, worldId, curTime);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Connection_RM3::OnLocalReference(Replica3 *replica3, ReplicaManager3 *replicaManager)
{
	ConstructionMode curConstructionMode = QueryConstructionMode();
	RakAssert(curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION);
	RakAssert(replica3);
	(void) replicaManager;
	(void) curConstructionMode;

#ifdef _DEBUG
	for (unsigned int i = 0; i < queryToConstructReplicaList.size(); i++)
	{
		if (queryToConstructReplicaList[i]->replica == replica3)
		{
			RakAssert("replica added twice to queryToConstructReplicaList" && 0);
		}
	}

	bool alreadyConstructed;
	FindConstructed(constructedReplicaList, replica3, &alreadyConstructed);
	if (alreadyConstructed)
	{
		RakAssert("replica added to queryToConstructReplicaList when already in constructedReplicaList" && 0);
	}
#endif

	LastSerializationResult *lsr = lock->records.Acquire(replica3);
	queryToConstructReplicaList.push_back(lsr);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnDereference(Replica3 *replica3, ReplicaManager3 *replicaManager)
{
	ValidateLists(replicaManager);

	if (replica3->GetNetworkIDManager() == 0)
		return;

	LastSerializationResult *lsr = 0;
	unsigned int idx;

	bool objectExists;
	idx = FindConstructed(constructedReplicaList, replica3, &objectExists);
	if (objectExists)
	{
		lsr = constructedReplicaList[idx];
		{
			std::lock_guard<std::mutex> constructedLock(lock->constructed);
			constructedReplicaList.erase(constructedReplicaList.begin() + idx);
		}
	}

	for (idx = 0; idx < queryToConstructReplicaList.size(); idx++)
	{
		if (queryToConstructReplicaList[idx]->replica == replica3)
		{
			lsr = queryToConstructReplicaList[idx];
			queryToConstructReplicaList.erase(queryToConstructReplicaList.begin() + idx);
			break;
		}
	}

	for (idx = 0; idx < queryToSerializeReplicaList.size(); idx++)
	{
		if (queryToSerializeReplicaList[idx]->replica == replica3)
		{
			lsr = queryToSerializeReplicaList[idx];
			queryToSerializeReplicaList.erase(queryToSerializeReplicaList.begin() + idx);
			break;
		}
	}

	for (idx = 0; idx < queryToDestructReplicaList.size(); idx++)
	{
		if (queryToDestructReplicaList[idx]->replica == replica3)
		{
			lsr = queryToDestructReplicaList[idx];
			queryToDestructReplicaList.erase(queryToDestructReplicaList.begin() + idx);
			break;
		}
	}

	ValidateLists(replicaManager);

	if (lsr)
		lock->records.Release(lsr);

	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnDownloadFromThisSystem(Replica3 *replica3, ReplicaManager3 *replicaManager)
{
	RakAssert(replica3);

	ValidateLists(replicaManager);
	LastSerializationResult *lsr = lock->records.Acquire(replica3);

	ConstructionMode curConstructionMode = QueryConstructionMode();
	if (curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION)
	{
		unsigned int j;
		for (j = 0; j < queryToConstructReplicaList.size(); j++)
		{
			if (queryToConstructReplicaList[j]->replica == replica3)
			{
				lock->records.Release(queryToConstructReplicaList[j]);
				queryToConstructReplicaList.erase(queryToConstructReplicaList.begin() + j);
				break;
			}
		}
	}

	unsigned int insertedAt;
	{
		std::lock_guard<std::mutex> constructedLock(lock->constructed);
		insertedAt = InsertConstructed(constructedReplicaList, lsr->replica, lsr);
	}
	if (insertedAt != (unsigned) -1)
	{
		if (curConstructionMode != QUERY_CONNECTION_FOR_REPLICA_LIST)
			queryToDestructReplicaList.push_back(lsr);
		queryToSerializeReplicaList.push_back(lsr);
	}
	else
		lock->records.Release(lsr);

	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnDownloadFromOtherSystem(Replica3 *replica3, ReplicaManager3 *replicaManager)
{
	ConstructionMode curConstructionMode = QueryConstructionMode();
	if (curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION)
	{
		unsigned int j;
		for (j = 0; j < queryToConstructReplicaList.size(); j++)
		{
			if (queryToConstructReplicaList[j]->replica == replica3)
			{
				return;
			}
		}

		OnLocalReference(replica3, replicaManager);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnNeverConstruct(unsigned int queryToConstructIdx, ReplicaManager3 *replicaManager)
{
	ConstructionMode curConstructionMode = QueryConstructionMode();
	RakAssert(curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION);
	(void) curConstructionMode;

	ValidateLists(replicaManager);
	LastSerializationResult *lsr = queryToConstructReplicaList[queryToConstructIdx];
	queryToConstructReplicaList.erase(queryToConstructReplicaList.begin() + queryToConstructIdx);
	lock->records.Release(lsr);
	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnConstructToThisConnection(unsigned int queryToConstructIdx, ReplicaManager3 *replicaManager)
{
	ConstructionMode curConstructionMode = QueryConstructionMode();
	RakAssert(curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION);
	(void) curConstructionMode;

	ValidateLists(replicaManager);
	LastSerializationResult *lsr = queryToConstructReplicaList[queryToConstructIdx];
	queryToConstructReplicaList.erase(queryToConstructReplicaList.begin() + queryToConstructIdx);
	//assert(constructedReplicaList.GetIndexOf(lsr->replica)==(unsigned int)-1);
	{
		std::lock_guard<std::mutex> constructedLock(lock->constructed);
		InsertConstructed(constructedReplicaList, lsr->replica, lsr);
	}
	//assert(queryToDestructReplicaList.GetIndexOf(lsr->replica)==(unsigned int)-1);
	queryToDestructReplicaList.push_back(lsr);
	//assert(queryToSerializeReplicaList.GetIndexOf(lsr->replica)==(unsigned int)-1);
	queryToSerializeReplicaList.push_back(lsr);
	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnConstructToThisConnection(Replica3 *replica, ReplicaManager3 *replicaManager)
{
	RakAssert(replica);
	RakAssert(QueryConstructionMode() == QUERY_CONNECTION_FOR_REPLICA_LIST);
	(void) replicaManager;

	LastSerializationResult *lsr = lock->records.Acquire(replica);
	{
		std::lock_guard<std::mutex> constructedLock(lock->constructed);
		InsertConstructed(constructedReplicaList, replica, lsr);
	}
	queryToSerializeReplicaList.push_back(lsr);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnNeverSerialize(LastSerializationResult *lsr, ReplicaManager3 *replicaManager)
{
	ValidateLists(replicaManager);

	unsigned int j;
	for (j = 0; j < queryToSerializeReplicaList.size(); j++)
	{
		if (queryToSerializeReplicaList[j] == lsr)
		{
			queryToSerializeReplicaList.erase(queryToSerializeReplicaList.begin() + j);
			break;
		}
	}

	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnReplicaAlreadyExists(unsigned int queryToConstructIdx, ReplicaManager3 *replicaManager)
{
	ConstructionMode curConstructionMode = QueryConstructionMode();
	RakAssert(curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION);
	(void) curConstructionMode;

	ValidateLists(replicaManager);
	LastSerializationResult *lsr = queryToConstructReplicaList[queryToConstructIdx];
	queryToConstructReplicaList.erase(queryToConstructReplicaList.begin() + queryToConstructIdx);
	//assert(constructedReplicaList.GetIndexOf(lsr->replica)==(unsigned int)-1);
	{
		std::lock_guard<std::mutex> constructedLock(lock->constructed);
		InsertConstructed(constructedReplicaList, lsr->replica, lsr);
	}
	//assert(queryToDestructReplicaList.GetIndexOf(lsr->replica)==(unsigned int)-1);
	queryToDestructReplicaList.push_back(lsr);
	//assert(queryToSerializeReplicaList.GetIndexOf(lsr->replica)==(unsigned int)-1);
	queryToSerializeReplicaList.push_back(lsr);
	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnDownloadExisting(Replica3 *replica3, ReplicaManager3 *replicaManager)
{
	ValidateLists(replicaManager);

	ConstructionMode curConstructionMode = QueryConstructionMode();
	if (curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION)
	{
		unsigned int idx;
		for (idx = 0; idx < queryToConstructReplicaList.size(); idx++)
		{
			if (queryToConstructReplicaList[idx]->replica == replica3)
			{
				OnConstructToThisConnection(idx, replicaManager);
				return;
			}
		}
	}
	else
	{
		OnConstructToThisConnection(replica3, replicaManager);
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnSendDestructionFromQuery(unsigned int queryToDestructIdx, ReplicaManager3 *replicaManager)
{
	ConstructionMode curConstructionMode = QueryConstructionMode();
	RakAssert(curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION || curConstructionMode == QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION);
	(void) curConstructionMode;

	ValidateLists(replicaManager);
	LastSerializationResult *lsr = queryToDestructReplicaList[queryToDestructIdx];
	queryToDestructReplicaList.erase(queryToDestructReplicaList.begin() + queryToDestructIdx);
	unsigned int j;
	for (j = 0; j < queryToSerializeReplicaList.size(); j++)
	{
		if (queryToSerializeReplicaList[j]->replica == lsr->replica)
		{
			queryToSerializeReplicaList.erase(queryToSerializeReplicaList.begin() + j);
			break;
		}
	}
	for (j = 0; j < constructedReplicaList.size(); j++)
	{
		if (constructedReplicaList[j]->replica == lsr->replica)
		{
			{
				std::lock_guard<std::mutex> constructedLock(lock->constructed);
				constructedReplicaList.erase(constructedReplicaList.begin() + j);
			}
			break;
		}
	}
	//assert(queryToConstructReplicaList.GetIndexOf(lsr->replica)==(unsigned int)-1);
	queryToConstructReplicaList.push_back(lsr);
	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::OnDoNotQueryDestruction(unsigned int queryToDestructIdx, ReplicaManager3 *replicaManager)
{
	ValidateLists(replicaManager);
	queryToDestructReplicaList.erase(queryToDestructReplicaList.begin() + queryToDestructIdx);
	ValidateLists(replicaManager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::ValidateLists(ReplicaManager3 *replicaManager) const
{
	(void) replicaManager;
	/*
#ifdef _DEBUG
	// Each object should exist only once in either constructedReplicaList or queryToConstructReplicaList
	// replicaPointer from LastSerializationResult should be same among all lists
	unsigned int idx, idx2;
	for (idx=0; idx < constructedReplicaList.size(); idx++)
	{
		idx2=queryToConstructReplicaList.GetIndexOf(constructedReplicaList[idx]->replica);
		if (idx2!=(unsigned int)-1)
		{
			int a=5;
			assert(a==0);
			int *b=0;
			*b=5;
		}
	}

	for (idx=0; idx < queryToConstructReplicaList.size(); idx++)
	{
		idx2=constructedReplicaList.GetIndexOf(queryToConstructReplicaList[idx]->replica);
		if (idx2!=(unsigned int)-1)
		{
			int a=5;
			assert(a==0);
			int *b=0;
			*b=5;
		}
	}

	LastSerializationResult *lsr, *lsr2;
	for (idx=0; idx < constructedReplicaList.size(); idx++)
	{
		lsr=constructedReplicaList[idx];

		idx2=queryToSerializeReplicaList.GetIndexOf(lsr->replica);
		if (idx2!=(unsigned int)-1)
		{
			lsr2=queryToSerializeReplicaList[idx2];
			if (lsr2!=lsr)
			{
				int a=5;
				assert(a==0);
				int *b=0;
				*b=5;
			}
		}

		idx2=queryToDestructReplicaList.GetIndexOf(lsr->replica);
		if (idx2!=(unsigned int)-1)
		{
			lsr2=queryToDestructReplicaList[idx2];
			if (lsr2!=lsr)
			{
				int a=5;
				assert(a==0);
				int *b=0;
				*b=5;
			}
		}
	}
	for (idx=0; idx < queryToConstructReplicaList.size(); idx++)
	{
		lsr=queryToConstructReplicaList[idx];

		idx2=queryToSerializeReplicaList.GetIndexOf(lsr->replica);
		if (idx2!=(unsigned int)-1)
		{
			lsr2=queryToSerializeReplicaList[idx2];
			if (lsr2!=lsr)
			{
				int a=5;
				assert(a==0);
				int *b=0;
				*b=5;
			}
		}

		idx2=queryToDestructReplicaList.GetIndexOf(lsr->replica);
		if (idx2!=(unsigned int)-1)
		{
			lsr2=queryToDestructReplicaList[idx2];
			if (lsr2!=lsr)
			{
				int a=5;
				assert(a==0);
				int *b=0;
				*b=5;
			}
		}
	}

	// Verify pointer integrity
	for (idx=0; idx < constructedReplicaList.size(); idx++)
	{
		if (constructedReplicaList[idx]->replica->replicaManager!=replicaManager)
		{
			int a=5;
			assert(a==0);
			int *b=0;
			*b=5;
		}
	}

	// Verify pointer integrity
	for (idx=0; idx < queryToConstructReplicaList.size(); idx++)
	{
		if (queryToConstructReplicaList[idx]->replica->replicaManager!=replicaManager)
		{
			int a=5;
			assert(a==0);
			int *b=0;
			*b=5;
		}
	}
#endif
	*/
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::SendConstruction(std::vector<Replica3 *> &newObjects, std::vector<Replica3 *> &deletedObjects, PRO sendParameters, MafiaNet::RakPeerInterface *rakPeer, unsigned char worldId, ReplicaManager3 *replicaManager3)
{
	if (newObjects.size() == 0 && deletedObjects.size() == 0)
		return;

	// All construction and destruction takes place in the same network message
	// Otherwise, if objects rely on each other being created the same tick to be valid, this won't always be true
	//	std::vector<LastSerializationResult* > serializedObjects;
	BitSize_t offsetStart, offsetStart2, offsetEnd;
	unsigned int newListIndex, oldListIndex;
	MafiaNet::BitStream bsOut;
	NetworkID networkId;
	if (isFirstConstruction)
	{
		bsOut.Write((MessageID) ID_REPLICA_MANAGER_DOWNLOAD_STARTED);
		bsOut.Write(worldId);
		SerializeOnDownloadStarted(&bsOut);
		rakPeer->Send(&bsOut, sendParameters.priority, MafiaNet::Reliability::ReliableOrdered, sendParameters.orderingChannel, systemAddress, false, sendParameters.sendReceipt);
	}

	//	LastSerializationResult* lsr;
	bsOut.Reset();
	bsOut.Write((MessageID) ID_REPLICA_MANAGER_CONSTRUCTION);
	bsOut.Write(worldId);
	uint16_t objectSize = (uint16_t) newObjects.size();
	bsOut.Write(objectSize);

	// Construction
	for (newListIndex = 0; newListIndex < newObjects.size(); newListIndex++)
	{
		offsetStart = bsOut.GetWriteOffset();
		bsOut.Write(offsetStart); // overwritten to point to the end of the stream
		networkId = newObjects[newListIndex]->GetNetworkID();
		bsOut.Write(networkId);

		RM3ConstructionState cs = newObjects[newListIndex]->QueryConstruction(this, replicaManager3);
		bool actuallyCreateObject = cs == RM3CS_SEND_CONSTRUCTION;
		bsOut.Write(actuallyCreateObject);
		bsOut.AlignWriteToByteBoundary();

		if (actuallyCreateObject)
		{
			// Actually create the object
			bsOut.Write(newObjects[newListIndex]->creatingSystemGUID);
			offsetStart2 = bsOut.GetWriteOffset();
			bsOut.Write(offsetStart2);        // overwritten to point to after the call to WriteAllocationID
			bsOut.AlignWriteToByteBoundary(); // Give the user an aligned bitStream in case they use memcpy
			newObjects[newListIndex]->WriteAllocationID(this, &bsOut);
			bsOut.AlignWriteToByteBoundary(); // Give the user an aligned bitStream in case they use memcpy
			offsetEnd = bsOut.GetWriteOffset();
			bsOut.SetWriteOffset(offsetStart2);
			bsOut.Write(offsetEnd);
			bsOut.SetWriteOffset(offsetEnd);
			newObjects[newListIndex]->SerializeConstruction(&bsOut, this);
		}
		else
		{
			newObjects[newListIndex]->SerializeConstructionExisting(&bsOut, this);
		}

		bsOut.AlignWriteToByteBoundary();
		offsetEnd = bsOut.GetWriteOffset();
		bsOut.SetWriteOffset(offsetStart);
		bsOut.Write(offsetEnd);
		bsOut.SetWriteOffset(offsetEnd);
	}

	MafiaNet::BitStream bsOut2;
	for (newListIndex = 0; newListIndex < newObjects.size(); newListIndex++)
	{
		bsOut2.Reset();
		RM3ConstructionState cs = newObjects[newListIndex]->QueryConstruction(this, replicaManager3);
		if (cs == RM3CS_SEND_CONSTRUCTION)
		{
			newObjects[newListIndex]->PostSerializeConstruction(&bsOut2, this);
		}
		else
		{
			RakAssert(cs == RM3CS_ALREADY_EXISTS_REMOTELY);
			newObjects[newListIndex]->PostSerializeConstructionExisting(&bsOut2, this);
		}
		if (bsOut2.GetNumberOfBitsUsed() > 0)
		{
			bsOut.Write(true);
			bsOut.AlignWriteToByteBoundary();
			offsetStart = bsOut.GetWriteOffset();
			bsOut.Write(offsetStart); // overwritten to point to the end of the stream
			networkId = newObjects[newListIndex]->GetNetworkID();
			bsOut.Write(networkId);
			bsOut.AlignWriteToByteBoundary(); // Give the user an aligned bitStream in case they use memcpy
			bsOut.Write(&bsOut2);
			bsOut.AlignWriteToByteBoundary(); // Give the user an aligned bitStream in case they use memcpy
			offsetEnd = bsOut.GetWriteOffset();
			bsOut.SetWriteOffset(offsetStart);
			bsOut.Write(offsetEnd);
			bsOut.SetWriteOffset(offsetEnd);
		}
		else
			bsOut.Write(false);
	}
	bsOut.AlignWriteToByteBoundary();

	// Destruction
	objectSize = (uint16_t) deletedObjects.size();
	bsOut.Write(objectSize);
	for (oldListIndex = 0; oldListIndex < deletedObjects.size(); oldListIndex++)
	{
		networkId = deletedObjects[oldListIndex]->GetNetworkID();
		bsOut.Write(networkId);
		offsetStart = bsOut.GetWriteOffset();
		bsOut.Write(offsetStart);
		deletedObjects[oldListIndex]->deletingSystemGUID = rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
		bsOut.Write(deletedObjects[oldListIndex]->deletingSystemGUID);
		deletedObjects[oldListIndex]->SerializeDestruction(&bsOut, this);
		bsOut.AlignWriteToByteBoundary();
		offsetEnd = bsOut.GetWriteOffset();
		bsOut.SetWriteOffset(offsetStart);
		bsOut.Write(offsetEnd);
		bsOut.SetWriteOffset(offsetEnd);
	}
	rakPeer->Send(&bsOut, sendParameters.priority, MafiaNet::Reliability::ReliableOrdered, sendParameters.orderingChannel, systemAddress, false, sendParameters.sendReceipt);

	// TODO - shouldn't this be part of construction?

	// Initial Download serialize to a new system
	// Immediately send serialize after construction if the replica object already has saved data
	// If the object was serialized identically, and does not change later on, then the new connection never gets the data
	SerializeParameters sp;
	sp.whenLastSerialized = 0;
	MafiaNet::BitStream emptyBs;
	for (int index = 0; index < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; index++)
	{
		sp.lastSentBitstream[index] = &emptyBs;
		sp.pro[index] = sendParameters;
		sp.pro[index].reliability = MafiaNet::Reliability::ReliableOrdered;
	}

	sp.bitsWrittenSoFar = 0;
	//	MafiaNet::Time t = MafiaNet::GetTimeMS();
	for (newListIndex = 0; newListIndex < newObjects.size(); newListIndex++)
	{
		sp.destinationConnection = this;
		sp.messageTimestamp = 0;
		MafiaNet::Replica3 *replica = newObjects[newListIndex];
		// 8/22/09 Forgot ResetWritePointer
		for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
		{
			sp.outputBitstream[z].ResetWritePointer();
		}

		RM3SerializationResult res = replica->Serialize(&sp);
		if (res != RM3SR_NEVER_SERIALIZE_FOR_THIS_CONNECTION &&
		    res != RM3SR_DO_NOT_SERIALIZE &&
		    res != RM3SR_SERIALIZED_UNIQUELY)
		{
			bool allIndices[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
			for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
			{
				sp.bitsWrittenSoFar += sp.outputBitstream[z].GetNumberOfBitsUsed();
				allIndices[z] = true;
			}
			SendSerialize(replica, allIndices, sp.outputBitstream, sp.messageTimestamp, sp.pro, rakPeer, worldId, GetTime());
			///			newObjects[newListIndex]->whenLastSerialized=t;
		}
		// else wait for construction request accepted before serializing
	}

	if (isFirstConstruction)
	{
		bsOut.Reset();
		bsOut.Write((MessageID) ID_REPLICA_MANAGER_DOWNLOAD_COMPLETE);
		bsOut.Write(worldId);
		SerializeOnDownloadComplete(&bsOut);
		rakPeer->Send(&bsOut, sendParameters.priority, MafiaNet::Reliability::ReliableOrdered, sendParameters.orderingChannel, systemAddress, false, sendParameters.sendReceipt);
	}

	isFirstConstruction = false;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Connection_RM3::SendValidation(MafiaNet::RakPeerInterface *rakPeer, WorldId worldId)
{
	// Hijack to mean sendValidation
	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_REPLICA_MANAGER_SCOPE_CHANGE);
	bsOut.Write(worldId);
	rakPeer->Send(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, systemAddress, false);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Replica3::Replica3()
{
	creatingSystemGUID = UNASSIGNED_RAKNET_GUID;
	deletingSystemGUID = UNASSIGNED_RAKNET_GUID;
	replicaManager = 0;
	forceSendUntilNextUpdate = false;
	lsr = 0;
	referenceIndex = (uint32_t) -1;
	referencingWorlds = 0;
	broadcastsIdentically = false;
	preparedPass = 0;
	parallelPass = 0;
	neededPass = 0;
	prepared = PB_SERIALIZE_PER_CONNECTION;
	preparedTimestamp = 0;
	lastBroadcast = 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Replica3::~Replica3()
{
	if (replicaManager)
	{
		ReplicaManager3 *manager = replicaManager;
		std::lock_guard<std::recursive_mutex> worldLock(manager->worldMutex);
		for (const auto *world : manager->worldsList)
		{
			if (referencingWorlds == 0)
				break;
			manager->Dereference(this, world->worldId);
		}
		// Out of the table here, under the manager's locks: ~NetworkIDObject() would do it under none.
		Replica3::SetNetworkIDManager(0);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Replica3::SetNetworkIDManager(NetworkIDManager *manager)
{
	if (replicaManager == 0)
	{
		NetworkIDObject::SetNetworkIDManager(manager);
		return;
	}
	// The table is read under the world, or under the replica set by the thread delivering state.
	std::lock_guard<std::recursive_mutex> worldLock(replicaManager->worldMutex);
	ReplicaManager3::ReplicaSetChange change(replicaManager);
	NetworkIDObject::SetNetworkIDManager(manager);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Replica3::SetNetworkID(NetworkID id)
{
	if (replicaManager == 0)
	{
		NetworkIDObject::SetNetworkID(id);
		return;
	}
	std::lock_guard<std::recursive_mutex> worldLock(replicaManager->worldMutex);
	ReplicaManager3::ReplicaSetChange change(replicaManager);
	NetworkIDObject::SetNetworkID(id);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Replica3::BroadcastDestruction(void)
{
	replicaManager->BroadcastDestruction(this, UNASSIGNED_SYSTEM_ADDRESS);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RakNetGUID Replica3::GetCreatingSystemGUID(void) const
{
	return creatingSystemGUID;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3ConstructionState Replica3::QueryConstruction_ClientConstruction(MafiaNet::Connection_RM3 *destinationConnection, bool isThisTheServer)
{
	(void) destinationConnection;
	if (creatingSystemGUID == replicaManager->GetRakPeerInterface()->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS))
		return RM3CS_SEND_CONSTRUCTION;
	// Send back to the owner client too, because they couldn't assign the network ID
	if (isThisTheServer)
		return RM3CS_SEND_CONSTRUCTION;
	return RM3CS_NEVER_CONSTRUCT;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool Replica3::QueryRemoteConstruction_ClientConstruction(MafiaNet::Connection_RM3 *sourceConnection, bool isThisTheServer)
{
	(void) sourceConnection;
	(void) isThisTheServer;

	// OK to create
	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3ConstructionState Replica3::QueryConstruction_ServerConstruction(MafiaNet::Connection_RM3 *destinationConnection, bool isThisTheServer)
{
	(void) destinationConnection;

	if (isThisTheServer)
		return RM3CS_SEND_CONSTRUCTION;
	return RM3CS_NEVER_CONSTRUCT;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool Replica3::QueryRemoteConstruction_ServerConstruction(MafiaNet::Connection_RM3 *sourceConnection, bool isThisTheServer)
{
	(void) sourceConnection;
	if (isThisTheServer)
		return false;
	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3ConstructionState Replica3::QueryConstruction_PeerToPeer(MafiaNet::Connection_RM3 *destinationConnection, Replica3P2PMode p2pMode)
{
	(void) destinationConnection;

	if (p2pMode == R3P2PM_SINGLE_OWNER)
	{
		// We send to all, others do nothing
		if (creatingSystemGUID == replicaManager->GetRakPeerInterface()->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS))
			return RM3CS_SEND_CONSTRUCTION;

		// RM3CS_NEVER_CONSTRUCT will not send the object, and will not Serialize() it
		return RM3CS_NEVER_CONSTRUCT;
	}
	else if (p2pMode == R3P2PM_MULTI_OWNER_CURRENTLY_AUTHORITATIVE)
	{
		return RM3CS_SEND_CONSTRUCTION;
	}
	else if (p2pMode == R3P2PM_STATIC_OBJECT_CURRENTLY_AUTHORITATIVE)
	{
		return RM3CS_ALREADY_EXISTS_REMOTELY;
	}
	else if (p2pMode == R3P2PM_STATIC_OBJECT_NOT_CURRENTLY_AUTHORITATIVE)
	{
		return RM3CS_ALREADY_EXISTS_REMOTELY_DO_NOT_CONSTRUCT;
	}
	else
	{
		RakAssert(p2pMode == R3P2PM_MULTI_OWNER_NOT_CURRENTLY_AUTHORITATIVE);

		// RM3CS_ALREADY_EXISTS_REMOTELY will not send the object, but WILL call QuerySerialization() and Serialize() on it.
		return RM3CS_ALREADY_EXISTS_REMOTELY;
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool Replica3::QueryRemoteConstruction_PeerToPeer(MafiaNet::Connection_RM3 *sourceConnection)
{
	(void) sourceConnection;

	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3QuerySerializationResult Replica3::QuerySerialization_ClientSerializable(MafiaNet::Connection_RM3 *destinationConnection, bool isThisTheServer)
{
	// Owner client sends to all
	if (creatingSystemGUID == replicaManager->GetRakPeerInterface()->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS))
		return RM3QSR_CALL_SERIALIZE;
	// Server sends to all but owner client
	if (isThisTheServer && destinationConnection->GetRakNetGUID() != creatingSystemGUID)
		return RM3QSR_CALL_SERIALIZE;
	// Remote clients do not send
	return RM3QSR_NEVER_CALL_SERIALIZE;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3QuerySerializationResult Replica3::QuerySerialization_ServerSerializable(MafiaNet::Connection_RM3 *destinationConnection, bool isThisTheServer)
{
	(void) destinationConnection;
	// Server sends to all
	if (isThisTheServer)
		return RM3QSR_CALL_SERIALIZE;

	// Clients do not send
	return RM3QSR_NEVER_CALL_SERIALIZE;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3QuerySerializationResult Replica3::QuerySerialization_PeerToPeer(MafiaNet::Connection_RM3 *destinationConnection, Replica3P2PMode p2pMode)
{
	(void) destinationConnection;

	if (p2pMode == R3P2PM_SINGLE_OWNER)
	{
		// Owner peer sends to all
		if (creatingSystemGUID == replicaManager->GetRakPeerInterface()->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS))
			return RM3QSR_CALL_SERIALIZE;

		// Remote peers do not send
		return RM3QSR_NEVER_CALL_SERIALIZE;
	}
	else if (p2pMode == R3P2PM_MULTI_OWNER_CURRENTLY_AUTHORITATIVE)
	{
		return RM3QSR_CALL_SERIALIZE;
	}
	else if (p2pMode == R3P2PM_STATIC_OBJECT_CURRENTLY_AUTHORITATIVE)
	{
		return RM3QSR_CALL_SERIALIZE;
	}
	else if (p2pMode == R3P2PM_STATIC_OBJECT_NOT_CURRENTLY_AUTHORITATIVE)
	{
		return RM3QSR_DO_NOT_CALL_SERIALIZE;
	}
	else
	{
		RakAssert(p2pMode == R3P2PM_MULTI_OWNER_NOT_CURRENTLY_AUTHORITATIVE);
		return RM3QSR_DO_NOT_CALL_SERIALIZE;
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3ActionOnPopConnection Replica3::QueryActionOnPopConnection_Client(MafiaNet::Connection_RM3 *droppedConnection) const
{
	(void) droppedConnection;
	return RM3AOPC_DELETE_REPLICA;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3ActionOnPopConnection Replica3::QueryActionOnPopConnection_Server(MafiaNet::Connection_RM3 *droppedConnection) const
{
	(void) droppedConnection;
	return RM3AOPC_DELETE_REPLICA_AND_BROADCAST_DESTRUCTION;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RM3ActionOnPopConnection Replica3::QueryActionOnPopConnection_PeerToPeer(MafiaNet::Connection_RM3 *droppedConnection) const
{
	(void) droppedConnection;
	return RM3AOPC_DELETE_REPLICA;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

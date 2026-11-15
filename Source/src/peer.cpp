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

// \file
//


#define CAT_NEUTER_EXPORT /* Neuter dllimport for libcat */

#include "mafianet/defines.h"
#include "mafianet/peer.h"
#include "mafianet/connect_candidates.h"
#include "mafianet/session_admission.h"
#include "mafianet/types.h"

#ifdef _WIN32

#else
#include <unistd.h>
#endif

// #if defined(new)
// #pragma push_macro("new")
// #undef new
// #define RMO_NEW_UNDEF_ALLOCATING_QUEUE
// #endif

#include <time.h>
#include <ctype.h> // toupper
#include <string.h>
#include "mafianet/get_time.h"
#include <atomic>
#include <chrono>
#include <random>
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_black_hole.h"
#include "mafianet/ds_huffman_encoding_tree.h"
#include "mafianet/rand.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/string_compressor.h"
#include "mafianet/string_table.h"
#include "mafianet/network_id_object.h"
#include "mafianet/types.h"
#include "mafianet/dr_sha1.h"
#include "mafianet/sleep.h"
#include "mafianet/assert.h"
#include "mafianet/version.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/get_time_of_day.h"
#include "mafianet/signaled_event.h"
#include "mafianet/super_fast_hash.h"
#include "mafianet/alloca.h"
#include "mafianet/wsa_startup_singleton.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#ifdef USE_THREADED_SEND
#include "mafianet/send_to_thread.h"
#endif

namespace MafiaNet
{
RAK_THREAD_DECLARATION(UpdateNetworkLoop);
RAK_THREAD_DECLARATION(RecvFromLoop);
RAK_THREAD_DECLARATION(UDTConnect);
}
#define REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE 8

#if !defined(__APPLE__) && !defined(__APPLE_CC__)
#include <stdlib.h> // malloc
#endif

#include <sodium.h>


#if defined(_WIN32)
//
#else
/*
#include <alloca.h> // Console 2
#include <stdlib.h>
extern bool _extern_Console2LoadModules(void);
extern int _extern_Console2GetConnectionStatus(void);
extern int _extern_Console2GetLobbyStatus(void);
//extern bool Console2StartupFluff(unsigned int *);
extern void Console2ShutdownFluff(void);
//extern unsigned int Console2ActivateConnection(unsigned int, void *);
//extern bool Console2BlockOnEstablished(void);
extern void Console2GetIPAndPort(unsigned int, char *, unsigned short *, unsigned int );
//extern void Console2DeactivateConnection(unsigned int, unsigned int);
*/
#endif


// Probed high to low while connecting: the connecting peer pads
// ID_OPEN_CONNECTION_REQUEST_1 to a rung and steps down when nothing comes back,
// so the negotiated MTU is the largest rung that survived the path. Each rung is
// a whole IP datagram, MAXIMUM_MTU_SIZE first.
//
// 1280 is both the IPv6 minimum MTU and what WireGuard-derived tunnels commonly
// settle on, which makes it the single most valuable intermediate rung; 1024
// covers heavier or stacked encapsulation; 576 is the dial-up floor. The gap
// from MAXIMUM_MTU_SIZE straight to 1200 that used to sit here meant a peer one
// byte over the top rung gave up ~20% of its payload capacity to find that out.
//
// The ladder itself lives in mtu_black_hole.h: the in-session black-hole
// step-down (ReliabilityLayer) walks the same rungs this handshake probes.
static const int NUM_MTU_SIZES = MafiaNet::MTU_LADDER_SIZE;
static const int *const mtuSizes = MafiaNet::MTU_LADDER;

// How many connection attempts are spent on each rung of mtuSizes before
// stepping down.
//
// Never zero. Connect() takes sendConnectionAttemptCount from the caller and
// documents no lower bound, so a caller asking for fewer attempts than there are
// rungs used to divide by zero here -- a crash that was unreachable only because
// the default (12) happened to exceed NUM_MTU_SIZES. Such a caller now gets one
// attempt per rung and stops when its budget runs out, having tried the largest
// sizes first.
static unsigned GetConnectionAttemptsPerMTU(unsigned sendConnectionAttemptCount)
{
	const unsigned attemptsPerMTU = sendConnectionAttemptCount / (unsigned) NUM_MTU_SIZES;
	return attemptsPerMTU > 0 ? attemptsPerMTU : 1;
}


// Note to self - if I change this it might affect RECIPIENT_OFFLINE_MESSAGE_INTERVAL in Natpunchthrough.cpp
//static const int MAX_OPEN_CONNECTION_REQUESTS=8;
//static const int TIME_BETWEEN_OPEN_CONNECTION_REQUESTS=500;

using namespace MafiaNet;

static RakNetRandom rnr;

// The IP version to resolve a host name or literal for, to send on \a socket:
// either family on a dual-stack socket (the resolver's first answer; an IPv4
// result is mapped on send), otherwise the socket's own family.
static int ResolveIPVersion(const RakNetSocket2 *socket)
{
	return socket->IsDualStack() ? 0 : socket->GetBoundAddress().GetIPVersion();
}

// Swaps 127.0.0.1 and ::1 to match a single-family socket. A dual-stack socket
// reaches both loopbacks, so the address is left as the caller gave it.
static void FixForSocket(SystemAddress &address, const RakNetSocket2 *socket)
{
	if (!socket->IsDualStack())
		address.FixForIPVersion(socket->GetBoundAddress());
}

// Resolves \a host for a one-shot unconnected send (Ping, SendOutOfBand,
// SendTTL), which has no Happy Eyeballs race to fall back on. On a dual-stack
// socket a name with an IPv4 address goes to that address -- as it did when
// sockets were IPv4 by default -- and only a name without one goes to its first
// IPv6 address. Otherwise the socket's own family, as before. False when
// \a host does not resolve.
static bool ResolveOneShotTarget(const char *host, unsigned short port, const RakNetSocket2 *socket, SystemAddress *out)
{
	if (socket->IsDualStack())
	{
		static const int MAX_CANDIDATES = 16;
		SystemAddress candidates[MAX_CANDIDATES];
		const int count = socket->Resolve(host, port, candidates, MAX_CANDIDATES);
		if (count <= 0)
			return false;
		int chosen = 0;
		for (int i = 0; i < count; i++)
		{
			if (candidates[i].GetIPVersion() == 4)
			{
				chosen = i;
				break;
			}
		}
		*out = candidates[chosen];
		return true;
	}
	if (!out->FromStringExplicitPort(host, port, ResolveIPVersion(socket)))
		return false;
	FixForSocket(*out, socket);
	return true;
}

// Whether a message a connected remote sent, and that RakPeer did not consume itself, may be handed to
// the plugins and the application.
//
// Everything below ID_TIMESTAMP is RakPeer's own, and so is ID_SESSION_CONFIG_ABANDONED, which is only
// ever generated locally. The other library ids are told apart from a remote's by their first byte, by
// RakPeer and by every plugin, and every reader then treats the id behind an ID_TIMESTAMP prefix as the
// message. So a prefix may only carry what a conforming peer puts behind one: an application message, or
// one of the library messages that are sent timestamped. Anything else is a control message in disguise.
static bool MayReachApplicationFromRemote(const unsigned char *data, unsigned int byteSize)
{
	const MessageID id = data[0];
	// Indistinguishable here from the ones the reliability layer generates locally.
	if (id == ID_SND_RECEIPT_ACKED || id == ID_SND_RECEIPT_LOSS)
		return true;
	if (id < (MessageID) ID_TIMESTAMP || id == ID_SESSION_CONFIG_ABANDONED)
		return false;
	if (id != ID_TIMESTAMP)
		return true;

	const unsigned int innerIdOffset = sizeof(MessageID) + sizeof(MafiaNet::Time);
	if (byteSize <= innerIdOffset)
		return false; // Nothing behind the time, and readers index the id there.
	const MessageID innerId = data[innerIdOffset];
	if (innerId >= (MessageID) ID_USER_PACKET_ENUM)
		return true;
	switch (innerId)
	{
	// ReplicaManager3 accepts all of its messages behind a timestamp (Connection_RM3::SendSerializeHeader).
	case ID_REPLICA_MANAGER_CONSTRUCTION:
	case ID_REPLICA_MANAGER_SCOPE_CHANGE:
	case ID_REPLICA_MANAGER_SERIALIZE:
	case ID_REPLICA_MANAGER_DOWNLOAD_STARTED:
	case ID_REPLICA_MANAGER_DOWNLOAD_COMPLETE:
	// NatPunchthroughServer
	case ID_NAT_CONNECT_AT_TIME:
		return true;
	default:
		return false;
	}
}

/*
struct RakPeerAndIndex
{
	RakNetSocket2 *s;
	RakPeer *rakPeer;
};
*/

static const unsigned int MAX_OFFLINE_DATA_LENGTH = 400; // I set this because I limit ID_CONNECTION_REQUEST to 512 bytes, and the password is appended to that packet.

// Used to distinguish between offline messages with data, and messages from the reliability layer
// Should be different than any message that could result from messages from the reliability layer
// Make sure highest bit is 0, so isValid in DatagramHeaderFormat is false
static const unsigned char OFFLINE_MESSAGE_DATA_ID[16] = {0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78};
// The size of ID_OPEN_CONNECTION_REPLY_1: MessageID | OFFLINE_MESSAGE_DATA_ID | server RakNetGUID | challenge | MTU.
// ID_OPEN_CONNECTION_REQUEST_1 must be at least this long, so answering it never amplifies traffic. Clients pad
// REQUEST_1 to the MTU they probe (at least the smallest MTU_LADDER rung minus UDP_HEADER_SIZE), far above this.
static const int OPEN_CONNECTION_REPLY_1_BYTES = (int) (sizeof(MessageID) + sizeof(OFFLINE_MESSAGE_DATA_ID) + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint16_t));
static_assert(sizeof(RakNetGUID::g) == sizeof(uint64_t), "REPLY_1 size assumes an 8-byte RakNetGUID");

struct PacketFollowedByData
{
	Packet p;
	unsigned char data[1];
};

Packet *RakPeer::AllocPacket(unsigned dataSize, const char *file, unsigned int line)
{
	// Crashes when dataSize is 4 bytes - not sure why
	// 	unsigned char *data = (unsigned char *) rakMalloc_Ex(sizeof(PacketFollowedByData)+dataSize, file, line);
	// 	Packet *p = &((PacketFollowedByData *)data)->p;
	// 	p->data=((PacketFollowedByData *)data)->data;
	// 	p->length=dataSize;
	// 	p->bitSize=BYTES_TO_BITS(dataSize);
	// 	p->deleteData=false;
	// 	p->guid=UNASSIGNED_RAKNET_GUID;
	// 	return p;

	MafiaNet::Packet *p;
	packetAllocationPoolMutex.Lock();
	p = packetAllocationPool.Allocate(file, line);
	++packetsOutstanding;
	packetAllocationPoolMutex.Unlock();
	p = new ((void *) p) Packet;
	p->data = (unsigned char *) rakMalloc_Ex(dataSize, file, line);
	p->length = dataSize;
	p->bitSize = BYTES_TO_BITS(dataSize);
	p->deleteData = true;
	p->guid = UNASSIGNED_RAKNET_GUID;
	p->wasGeneratedLocally = false;
	return p;
}

Packet *RakPeer::AllocPacket(unsigned dataSize, unsigned char *data, const char *file, unsigned int line)
{
	// Packet *p = (Packet *)rakMalloc_Ex(sizeof(Packet), file, line);
	MafiaNet::Packet *p;
	packetAllocationPoolMutex.Lock();
	p = packetAllocationPool.Allocate(file, line);
	++packetsOutstanding;
	packetAllocationPoolMutex.Unlock();
	p = new ((void *) p) Packet;
	RakAssert(p);
	p->data = data;
	p->length = dataSize;
	p->bitSize = BYTES_TO_BITS(dataSize);
	p->deleteData = true;
	p->guid = UNASSIGNED_RAKNET_GUID;
	p->wasGeneratedLocally = false;
	return p;
}

STATIC_FACTORY_DEFINITIONS(RakPeerInterface, RakPeer)

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Constructor
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakPeer::RakPeer()
{
	StringCompressor::AddReference();
	MafiaNet::StringTable::AddReference();
	WSAStartupSingleton::AddRef();

	defaultMTUSize = mtuSizes[NUM_MTU_SIZES - 1];
	trackFrequencyTable = false;
	maximumIncomingConnections = 0;
	maximumNumberOfPeers = 0;
	//remoteSystemListSize=0;
	remoteSystemList = 0;
	activeSystemList = 0;
	activeSystemListSize = 0;
	remoteSystemLookup = 0;
	bytesSentPerSecond = bytesReceivedPerSecond = 0;
	endThreads = true;
	networkThreadReceiveStopped = false;
	isMainLoopThreadActive = false;
	socketFactory = nullptr;
	nextRaceGroup = 1;
	manualUpdate = false;
	incomingDatagramEventHandler = 0;
	sessionConfigInteractive = false;
	maximumPendingSessions = 0;
	maximumPendingSessionsPerAddress = 0;
	sessionTimeout = 0;


	// isRecvfromThreadActive=false;
#if defined(GET_TIME_SPIKE_LIMIT) && GET_TIME_SPIKE_LIMIT > 0
	occasionalPing = true;
#else
	occasionalPing = false;
#endif
	allowInternalRouting = false;
	for (unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++)
		ipList[i] = UNASSIGNED_SYSTEM_ADDRESS;
	allowConnectionResponseIPMigration = false;
	//incomingPasswordLength=outgoingPasswordLength=0;
	incomingPasswordLength = 0;
	splitMessageProgressInterval = 0;
	//unreliableTimeout=0;
	unreliableTimeout = 1000;
	maxOutgoingBPS = 0;
	firstExternalID = UNASSIGNED_SYSTEM_ADDRESS;
	myGuid = UNASSIGNED_RAKNET_GUID;
	userUpdateThreadPtr = 0;
	userUpdateThreadData = 0;

#ifdef _DEBUG
	// Wait longer to disconnect in debug so I don't get disconnected while tracing
	defaultTimeoutTime = 30000;
#else
	defaultTimeoutTime = 10000;
#endif

#ifdef _DEBUG
	_packetloss = 0.0;
	_minExtraPing = 0;
	_extraPingVariance = 0;
#endif

	bufferedCommands.SetPageSize(sizeof(BufferedCommandStruct) * 16);
	socketQueryOutput.SetPageSize(sizeof(SocketQueryOutput) * 8);

	packetAllocationPoolMutex.Lock();
	packetAllocationPool.SetPageSize(sizeof(DataStructures::MemoryPool<Packet>::MemoryWithPage) * 32);
	packetsOutstanding = 0;
	packetAllocationPoolMutex.Unlock();
	shutdownGeneration = 0;

	remoteSystemIndexPool.SetPageSize(sizeof(DataStructures::MemoryPool<RemoteSystemIndex>::MemoryWithPage) * 32);


	GenerateGUID();

	quitAndDataEvents.InitEvent();
	limitConnectionFrequencyFromTheSameIP = false;
	ResetSendReceipt();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Destructor
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakPeer::~RakPeer()
{
	Shutdown(0, 0);

	// Free the ban list.
	ClearBanList();

	StringCompressor::RemoveReference();
	MafiaNet::StringTable::RemoveReference();
	WSAStartupSingleton::Deref();

	quitAndDataEvents.CloseEvent();


	// 	for (unsigned int i=0; i < pluginListTS.Size(); i++)
	// 		pluginListTS[i]->SetRakPeerInterface(0);
	// 	for (unsigned int i=0; i < pluginListNTS.Size(); i++)
	// 		pluginListNTS[i]->SetRakPeerInterface(0);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// \brief Starts the network threads, opens the listen port.
// You must call this before calling Connect().
// Multiple calls while already active are ignored.  To call this function again with different settings, you must first call Shutdown().
// \note Call SetMaximumIncomingConnections if you want to accept incoming connections
// \param[in] maxConnections The maximum number of connections between this instance of RakPeer and another instance of RakPeer. Required so the network can preallocate and for thread safety. A pure client would set this to 1.  A pure server would set it to the number of allowed clients.- A hybrid would set it to the sum of both types of connections
// \param[in] localPort The port to listen for connections on.
// \param[in] _threadSleepTimer How many ms to Sleep each internal update cycle. With new congestion control, the best results will be obtained by passing 10.
// \param[in] socketDescriptors An array of SocketDescriptor structures to force RakNet to listen on a particular IP address or port (or both).  Each SocketDescriptor will represent one unique socket.  Do not pass redundant structures.  To listen on a specific port, you can pass &socketDescriptor, 1SocketDescriptor(myPort,0); such as for a server.  For a client, it is usually OK to just pass SocketDescriptor();
// \param[in] socketDescriptorCount The size of the \a socketDescriptors array.  Pass 1 if you are not sure what to pass.
// \return False on failure (can't create socket or thread), true on success.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
StartupResult RakPeer::Startup(unsigned int maxConnections, SocketDescriptor *socketDescriptors, unsigned socketDescriptorCount, int threadPriority)
{
	if (IsActive())
		return RAKNET_ALREADY_STARTED;

	// Safe to call repeatedly and from several threads; returns 1 when already initialised.
	if (sodium_init() < 0)
		return CRYPTO_INITIALIZATION_FAILED;

	// If getting the guid failed in the constructor, try again
	if (myGuid.g == 0)
	{
		GenerateGUID();
		if (myGuid.g == 0)
			return COULD_NOT_GENERATE_GUID;
	}

	if (threadPriority == -99999)
	{


#if defined(_WIN32)
		threadPriority = 0;


#else
		threadPriority = 1000;
#endif
	}


	FillIPList();

	if (myGuid == UNASSIGNED_RAKNET_GUID)
	{
		rnr.SeedMT(GenerateSeedFromGuid());
	}

	//RakPeerAndIndex rpai[32];
	//RakAssert(socketDescriptorCount<32);

	RakAssert(socketDescriptors && socketDescriptorCount >= 1);

	if (socketDescriptors == 0 || socketDescriptorCount < 1)
		return INVALID_SOCKET_DESCRIPTORS;

	//unsigned short localPort;
	//localPort=socketDescriptors[0].port;

	RakAssert(maxConnections > 0);

	if (maxConnections <= 0)
		return INVALID_MAX_CONNECTIONS;

	// Reject a family the socket layer cannot bind before touching any state,
	// so the peer can still be started afterwards with a valid descriptor.
	for (unsigned d = 0; d < socketDescriptorCount; d++)
	{
		const int family = socketDescriptors[d].socketFamily;
		if (family != AF_INET && family != AF_INET6 && family != AF_UNSPEC)
			return SOCKET_FAMILY_NOT_SUPPORTED;
	}

	DerefAllSockets();


	unsigned i;
	// Go through all socket descriptors and precreate sockets on the specified addresses
	for (i = 0; i < socketDescriptorCount; i++)
	{
		/*
		const char *addrToBind;
		if (socketDescriptors[i].hostAddress[0]==0)
			addrToBind=0;
		else
			addrToBind=socketDescriptors[i].hostAddress;
			*/

		RakNetSocket2 *r2 = socketFactory ? socketFactory->Create(socketDescriptors[i], i) : RakNetSocket2Allocator::AllocRNS2();
		if (r2 == nullptr)
		{
			DerefAllSockets();
			return SOCKET_PORT_ALREADY_IN_USE;
		}
		r2->SetUserConnectionSocketIndex(i);
		if (r2->IsBerkleySocket())
		{
			RNS2_BerkleyBindParameters bbp;
			bbp.port = socketDescriptors[i].port;
			bbp.hostAddress = (char *) socketDescriptors[i].hostAddress;
			bbp.addressFamily = socketDescriptors[i].socketFamily;
			bbp.requireDualStack = socketDescriptors[i].requireDualStack;
			bbp.type = SOCK_DGRAM;
			bbp.protocol = socketDescriptors[i].extraSocketOptions;
			bbp.nonBlockingSocket = false;
			bbp.setBroadcast = true;
			bbp.setIPHdrIncl = false;
			bbp.doNotFragment = false;
			bbp.pollingThreadPriority = threadPriority;
			bbp.eventHandler = this;
			RNS2BindResult br = ((RNS2_Berkley *) r2)->Bind(&bbp, _FILE_AND_LINE_);

			if (br == BR_FAMILY_NOT_SUPPORTED)
			{
				RakNetSocket2Allocator::DeallocRNS2(r2);
				DerefAllSockets();
				return SOCKET_FAMILY_NOT_SUPPORTED;
			}
			else if (br == BR_FAILED_TO_BIND_SOCKET)
			{
				RakNetSocket2Allocator::DeallocRNS2(r2);
				DerefAllSockets();
				return SOCKET_PORT_ALREADY_IN_USE;
			}
			else if (br == BR_FAILED_SEND_TEST)
			{
				RakNetSocket2Allocator::DeallocRNS2(r2);
				DerefAllSockets();
				return SOCKET_FAILED_TEST_SEND;
			}
			else
			{
				RakAssert(br == BR_SUCCESS);
			}
		}
		else
		{
			// Factory-supplied: already bound; it delivers datagrams to us.
			r2->SetRecvEventHandler(this);
		}

		socketList.Push(r2, _FILE_AND_LINE_);
	}

	for (i = 0; i < socketDescriptorCount; i++)
	{
		if (socketList[i]->IsBerkleySocket())
			((RNS2_Berkley *) socketList[i])->CreateRecvPollingThread(threadPriority);
	}

	for (i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++)
	{
		if (ipList[i] == UNASSIGNED_SYSTEM_ADDRESS)
			break;
		// #high - using the 1st socket here is flawed - in cases of having multiple sockets (f.e. different ports and different families (i.e. IPv4/IPv6) we must use the proper
		//         socket for each IP address in the list
		if (socketList[0]->IsBerkleySocket())
		{
			unsigned short port = ((RNS2_Berkley *) socketList[0])->GetBoundAddress().GetPort();
			ipList[i].SetPortHostOrder(port);
		}
		else
		{
			ipList[i].SetPortHostOrder(socketList[0]->GetBoundAddress().GetPort());
		}
	}

	if (maximumNumberOfPeers == 0)
	{
		// Don't allow more incoming connections than we have peers.
		if (maximumIncomingConnections > maxConnections)
			maximumIncomingConnections = maxConnections;

		maximumNumberOfPeers = maxConnections;
		// 04/19/2006 - Don't overallocate because I'm no longer allowing connected pings.
		// The disconnects are not consistently processed and the process was sloppy and complicated.
		// Allocate 10% extra to handle new connections from players trying to connect when the server is full
		//remoteSystemListSize = maxConnections;// * 11 / 10 + 1;

		// remoteSystemList in Single thread
		//remoteSystemList = MafiaNet::OP_NEW<RemoteSystemStruct[ remoteSystemListSize ]>( _FILE_AND_LINE_ );
		remoteSystemList = MafiaNet::OP_NEW_ARRAY<RemoteSystemStruct>(maximumNumberOfPeers, _FILE_AND_LINE_);

		remoteSystemLookup = MafiaNet::OP_NEW_ARRAY<RemoteSystemIndex *>((unsigned int) maximumNumberOfPeers * REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE, _FILE_AND_LINE_);

		activeSystemList = MafiaNet::OP_NEW_ARRAY<RemoteSystemStruct *>(maximumNumberOfPeers, _FILE_AND_LINE_);

		for (i = 0; i < maximumNumberOfPeers; i++)
		//for ( i = 0; i < remoteSystemListSize; i++ )
		{
			// remoteSystemList in Single thread
			remoteSystemList[i].isActive = false;
			remoteSystemList[i].systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
			remoteSystemList[i].raceFirstAddress = UNASSIGNED_SYSTEM_ADDRESS;
			remoteSystemList[i].guid = UNASSIGNED_RAKNET_GUID;
			remoteSystemList[i].myExternalSystemAddress = UNASSIGNED_SYSTEM_ADDRESS;
			remoteSystemList[i].connectMode = RemoteSystemStruct::NO_ACTION;
			remoteSystemList[i].MTUSize = defaultMTUSize;
			remoteSystemList[i].remoteSystemIndex = (SystemIndex) i;
			// One-time zero-init: the array is OP_NEW_ARRAY allocated with no member init, so prime the
			// reason-payload fields here before ClearDisconnectReason() can ever free them.
			remoteSystemList[i].disconnectReasonData = 0;
			remoteSystemList[i].disconnectReasonLength = 0;
			remoteSystemList[i].remoteSessionConfigData = 0;
			remoteSystemList[i].remoteSessionConfigLength = 0;
			remoteSystemList[i].withheldConnectionPacketData = 0;
			remoteSystemList[i].withheldConnectionPacketLength = 0;
			remoteSystemList[i].sessionConfigAwaitingLocalDecision = false;
			remoteSystemList[i].sessionConfigIsConnectingSide = false;
			remoteSystemList[i].connectionReportedToApplication = false;
			remoteSystemList[i].absorbedOurConnectAttempt = false;
			remoteSystemList[i].sessionConfigStartTime = 0;
#ifdef _DEBUG
			remoteSystemList[i].reliabilityLayer.ApplyNetworkSimulator(_packetloss, _minExtraPing, _extraPingVariance);
#endif

			// All entries in activeSystemList have valid pointers all the time.
			activeSystemList[i] = &remoteSystemList[i];
		}

		for (i = 0; i < (unsigned int) maximumNumberOfPeers * REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE; i++)
		{
			remoteSystemLookup[i] = 0;
		}
	}

	// For histogram statistics
	// nextReadBytesTime=0;
	// lastSentBytes=lastReceivedBytes=0;

	if (endThreads)
	{
		updateCycleIsRunning = false;
		endThreads = false;
		firstExternalID = UNASSIGNED_SYSTEM_ADDRESS;

		ClearBufferedCommands();
		ClearBufferedPackets();
		ClearSocketQueryOutput();

		if (isMainLoopThreadActive == false && !manualUpdate)
		{
#if RAKPEER_USER_THREADED != 1

			int errorCode;


			errorCode = MafiaNet::RakThread::Create(UpdateNetworkLoop, this, threadPriority);


			if (errorCode != 0)
			{
				Shutdown(0, 0);
				return FAILED_TO_CREATE_NETWORK_THREAD;
			}
//					RakAssert(isRecvFromLoopThreadActive.GetValue()==0);
#endif // RAKPEER_USER_THREADED!=1

			/*
			for (i=0; i<socketDescriptorCount; i++)
			{
				RakPeerAndIndex *rpai = MafiaNet::OP_NEW<RakPeerAndIndex>(_FILE_AND_LINE_);
				rpai->s=socketList[i];
				rpai->rakPeer=this;

#if RAKPEER_USER_THREADED!=1

				errorCode = MafiaNet::RakThread::Create(RecvFromLoop, rpai, threadPriority);

				if ( errorCode != 0 )
				{
					Shutdown( 0, 0 );
					return FAILED_TO_CREATE_NETWORK_THREAD;
				}
#endif // RAKPEER_USER_THREADED!=1
				}
				*/


			/*
#if RAKPEER_USER_THREADED!=1

			while (  isRecvFromLoopThreadActive.GetValue() < (uint32_t) socketDescriptorCount )
				RakSleep(10);
				#endif // RAKPEER_USER_THREADED!=1
				*/
		}

#if RAKPEER_USER_THREADED != 1
		// Wait for the threads to activate.  When they are active they will set these variables to true
		while (!manualUpdate && isMainLoopThreadActive == false)
			RakSleep(10);
#endif // RAKPEER_USER_THREADED!=1
	}

	for (i = 0; i < pluginListTS.Size(); i++)
	{
		pluginListTS[i]->OnRakPeerStartup();
	}

	for (i = 0; i < pluginListNTS.Size(); i++)
	{
		pluginListNTS[i]->OnRakPeerStartup();
	}

#ifdef USE_THREADED_SEND
	MafiaNet::SendToThread::AddRef();
#endif

	return RAKNET_STARTED;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::SetEncryptionMode(EncryptionMode mode)
{
	// Every connection of a started peer must agree on one mode, and the reliability layers read it when
	// a connection is allocated: it can only change while offline.
	if (IsActive())
		return false;
	encryptionMode = mode;
	return true;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
EncryptionMode RakPeer::GetEncryptionMode() const
{
	return encryptionMode;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
HandshakeChallenge &RakPeer::GetHandshakeChallengeForTesting()
{
	return handshakeChallenge;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const unsigned char *RakPeer::GetOfflineMessageDataIdForTesting()
{
	static_assert(sizeof(OFFLINE_MESSAGE_DATA_ID) == kOfflineMessageDataIdBytes, "kOfflineMessageDataIdBytes is stale");
	return OFFLINE_MESSAGE_DATA_ID;
}

bool RakPeer::GetPendingHandshakeNonceForTesting(const SystemAddress &remote, uint64_t *out)
{
	bool found = false;
	requestedConnectionQueueMutex.Lock();
	for (unsigned i = 0; i < requestedConnectionQueue.Size(); i++)
	{
		const RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (rcs->systemAddress == remote && !rcs->cancelled)
		{
			*out = rcs->handshakeNonce;
			found = true;
			break;
		}
	}
	requestedConnectionQueueMutex.Unlock();
	return found;
}

uint64_t RakPeer::GetServerKeyExchangeCountForTesting() const
{
	return serverKeyExchangeCount;
}

bool RakPeer::GetSessionKeysForTesting(const SystemAddress &remote, SessionKeys *out) const
{
	RemoteSystemStruct *remoteSystem = GetRemoteSystemFromSystemAddress(remote, true, true);
	if (remoteSystem == 0)
	{
		out->Wipe();
		return false;
	}
	return remoteSystem->reliabilityLayer.GetSessionKeysForTesting(out);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Sets how many incoming connections are allowed.  If this is less than the number of players currently connected, no
// more players will be allowed to connect.  If this is greater than the maximum number of peers allowed, it will be reduced
// to the maximum number of peers allowed.  Defaults to 0.
//
// Parameters:
// numberAllowed - Maximum number of incoming connections allowed.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetMaximumIncomingConnections(unsigned short numberAllowed)
{
	maximumIncomingConnections = numberAllowed;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns the maximum number of incoming connections, which is always <= maxConnections
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetMaximumIncomingConnections(void) const
{
	return maximumIncomingConnections;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Returns how many open connections there are at this time
// \return the number of open connections
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned short RakPeer::NumberOfConnections(void) const
{
	DataStructures::List<SystemAddress> addresses;
	DataStructures::List<RakNetGUID> guids;
	GetSystemList(addresses, guids);
	return (unsigned short) addresses.Size();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Sets the password incoming connections must match in the call to Connect (defaults to none)
// Pass 0 to passwordData to specify no password
//
// Parameters:
// passwordData: A data block that incoming connections must match.  This can be just a password, or can be a stream of data.
// - Specify 0 for no password data
// passwordDataLength: The length in bytes of passwordData
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetIncomingPassword(const char *passwordData, int passwordDataLength)
{
	//if (passwordDataLength > MAX_OFFLINE_DATA_LENGTH)
	//	passwordDataLength=MAX_OFFLINE_DATA_LENGTH;

	if (passwordDataLength > 255)
		passwordDataLength = 255;

	if (passwordData == 0)
		passwordDataLength = 0;

	// Not threadsafe but it's not important enough to lock.  Who is going to change the password a lot during runtime?
	// It won't overflow at least because incomingPasswordLength is an unsigned char
	if (passwordDataLength > 0)
		memcpy(incomingPassword, passwordData, passwordDataLength);
	incomingPasswordLength = (unsigned char) passwordDataLength;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetIncomingPassword(char *passwordData, int *passwordDataLength)
{
	if (passwordData == 0)
	{
		*passwordDataLength = incomingPasswordLength;
		return;
	}

	if (*passwordDataLength > incomingPasswordLength)
		*passwordDataLength = incomingPasswordLength;

	if (*passwordDataLength > 0)
		memcpy(passwordData, incomingPassword, *passwordDataLength);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Call this to connect to the specified host (ip or domain name) and server port.
// Calling Connect and not calling SetMaximumIncomingConnections acts as a dedicated client.  Calling both acts as a true peer.
// This is a non-blocking connection.  You know the connection is successful when IsConnected() returns true
// or receive gets a packet with the type identifier ID_CONNECTION_REQUEST_ACCEPTED.  If the connection is not
// successful, such as rejected connection or no response then neither of these things will happen.
// Requires that you first call Initialize
//
// Parameters:
// host: Either a dotted IP address or a domain name
// remotePort: Which port to connect to on the remote machine.
// passwordData: A data block that must match the data block on the server.  This can be just a password, or can be a stream of data
// passwordDataLength: The length in bytes of passwordData
//
// Returns:
// True on successful initiation. False on incorrect parameters, internal error, or too many existing peers
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
ConnectionAttemptResult RakPeer::Connect(const char *host, unsigned short remotePort, const char *passwordData, int passwordDataLength, PublicKey *publicKey, unsigned connectionSocketIndex, unsigned sendConnectionAttemptCount, unsigned timeBetweenSendConnectionAttemptsMS, MafiaNet::TimeMS timeoutTime)
{
	// If endThreads is true here you didn't call Startup() first.
	// publicKey is reserved for server identity keys; there are none yet.
	if (host == 0 || endThreads || connectionSocketIndex >= socketList.Size() || publicKey != 0)
		return INVALID_PARAMETER;

	RakAssert(remotePort != 0);

	connectionSocketIndex = GetRakNetSocketFromUserConnectionSocketIndex(connectionSocketIndex);

	if (passwordDataLength > 255)
		passwordDataLength = 255;

	if (passwordData == 0)
		passwordDataLength = 0;

	// Not threadsafe but it's not important enough to lock.  Who is going to change the password a lot during runtime?
	// It won't overflow at least because outgoingPasswordLength is an unsigned char
	//	if (passwordDataLength>0)
	//		memcpy(outgoingPassword, passwordData, passwordDataLength);
	//	outgoingPasswordLength=(unsigned char) passwordDataLength;

	// 04/02/09 - Can't remember why I disabled connecting to self, but it seems to work
	// Connecting to ourselves in the same instance of the program?
	//	if ( ( strcmp( host, "127.0.0.1" ) == 0 || strcmp( host, "0.0.0.0" ) == 0 ) && remotePort == mySystemAddress[0].port )
	//		return false;

	return SendConnectionRequest(host, remotePort, passwordData, passwordDataLength, connectionSocketIndex, 0, sendConnectionAttemptCount, timeBetweenSendConnectionAttemptsMS, timeoutTime);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ConnectionAttemptResult RakPeer::ConnectWithSocket(const char *host, unsigned short remotePort, const char *passwordData, int passwordDataLength, RakNetSocket2 *socket, PublicKey *publicKey, unsigned sendConnectionAttemptCount, unsigned timeBetweenSendConnectionAttemptsMS, MafiaNet::TimeMS timeoutTime)
{
	// publicKey is reserved for server identity keys; there are none yet.
	if (host == 0 || endThreads || socket == 0 || publicKey != 0)
		return INVALID_PARAMETER;

	if (passwordDataLength > 255)
		passwordDataLength = 255;

	if (passwordData == 0)
		passwordDataLength = 0;

	return SendConnectionRequest(host, remotePort, passwordData, passwordDataLength, 0, 0, sendConnectionAttemptCount, timeBetweenSendConnectionAttemptsMS, timeoutTime, socket);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Stops the network threads and close all connections.  Multiple calls are ok.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::Shutdown(unsigned int blockDuration, unsigned char orderingChannel, MafiaNet::Priority disconnectionNotificationPriority)
{
	unsigned i, j;
	bool anyActive;
	MafiaNet::TimeMS startWaitingTime;
	//	SystemAddress systemAddress;
	MafiaNet::TimeMS time;
	//unsigned short systemListSize = remoteSystemListSize; // This is done for threading reasons
	unsigned int systemListSize = maximumNumberOfPeers;

	++shutdownGeneration;

	if (blockDuration > 0)
	{
		for (i = 0; i < systemListSize; i++)
		{
			// remoteSystemList in user thread
			if (remoteSystemList[i].isActive)
				NotifyAndFlagForShutdown(remoteSystemList[i].systemAddress, false, orderingChannel, disconnectionNotificationPriority);
		}

		time = MafiaNet::GetTimeMS();
		startWaitingTime = time;
		// Nothing else advances a manually updated peer (or its clock, under a
		// test's virtual time): flush the notifications once and stop.
		if (manualUpdate)
			RunManualUpdate();
		while (!manualUpdate && time - startWaitingTime < blockDuration)
		{
			anyActive = false;
			for (j = 0; j < systemListSize; j++)
			{
				// remoteSystemList in user thread
				if (remoteSystemList[j].isActive)
				{
					anyActive = true;
					break;
				}
			}

			// If this system is out of packets to send, then stop waiting
			if (anyActive == false)
				break;

			// This will probably cause the update thread to run which will probably
			// send the disconnection notification

			RakSleep(15);
			time = MafiaNet::GetTimeMS();
		}
	}
	// Plugins tear down in OnRakPeerShutdown while the update thread may still be running; stop
	// offering them messages there first, so no hook is mid-call or starts after this point.
	SetNetworkThreadReceiveStopped(true);

	for (i = 0; i < pluginListTS.Size(); i++)
	{
		pluginListTS[i]->OnRakPeerShutdown();
	}
	for (i = 0; i < pluginListNTS.Size(); i++)
	{
		pluginListNTS[i]->OnRakPeerShutdown();
	}

	quitAndDataEvents.SetEvent();

	endThreads = true;

	// Under the mutex, so a WaitForPackets that has just seen the peer active is already waiting.
	packetReturnMutex.Lock();
	packetReturnCondition.notify_all();
	packetReturnMutex.Unlock();

//	MafiaNet::TimeMS timeout;
#if RAKPEER_USER_THREADED != 1

	for (i = 0; i < socketList.Size(); i++)
	{
		if (socketList[i]->IsBerkleySocket())
		{
			((RNS2_Berkley *) socketList[i])->SignalStopRecvPollingThread();
		}
	}

	/*
	// Get recvfrom to unblock
	for (i=0; i < socketList.Size(); i++)
	{
		if (SocketLayer::SendTo(socketList[i], (const char*) &i,1,socketList[i]->GetBoundAddress(), _FILE_AND_LINE_)!=0)
			break;
	}
	*/

	while (isMainLoopThreadActive)
	{
		RakSleep(15);
	}

	// The update thread has exited, so nothing can be in or enter the hook: re-arm it for the next
	// Startup(). Re-arming here rather than in Startup() means the constructor's value is the one
	// the first Startup() runs with.
	SetNetworkThreadReceiveStopped(false);

	activeSystemListSize = 0;

	/*
	timeout = MafiaNet::GetTimeMS()+1000;
	while ( isRecvFromLoopThreadActive.GetValue()>0 && MafiaNet::GetTimeMS()<timeout )
	{
		// Get recvfrom to unblock
		for (i=0; i < socketList.Size(); i++)
		{
			SocketLayer::SendTo(socketList[i], (const char*) &i,1,socketList[i]->GetBoundAddress(), _FILE_AND_LINE_);
		}

		RakSleep(30);
	}
	*/

	for (i = 0; i < socketList.Size(); i++)
	{
		if (socketList[i]->IsBerkleySocket())
		{
			((RNS2_Berkley *) socketList[i])->BlockOnStopRecvPollingThread();
		}
	}


#endif // RAKPEER_USER_THREADED!=1

	//	char c=0;
	//	unsigned int socketIndex;
	// remoteSystemList in Single thread
	for (i = 0; i < systemListSize; i++)
	{
		// Reserve this reliability layer for ourselves
		remoteSystemList[i].isActive = false;

		// Remove any remaining packets
		RakAssert(remoteSystemList[i].MTUSize <= MAXIMUM_MTU_SIZE);
		remoteSystemList[i].reliabilityLayer.Reset(false, remoteSystemList[i].MTUSize);
		remoteSystemList[i].rakNetSocket = 0;
		ClearDisconnectReason(&remoteSystemList[i]);
		ClearSessionConfig(&remoteSystemList[i]);
	}


	// Setting maximumNumberOfPeers to 0 allows remoteSystemList to be reallocated in Initialize.
	// Setting remoteSystemListSize prevents threads from accessing the reliability layer
	maximumNumberOfPeers = 0;
	//remoteSystemListSize = 0;

	// Free any packets the user didn't deallocate
	packetReturnMutex.Lock();
	for (i = 0; i < packetReturnQueue.Size(); i++)
		DeallocatePacket(packetReturnQueue[i]);
	packetReturnQueue.Clear(_FILE_AND_LINE_);
	packetReturnMutex.Unlock();
	// Packets still out live in the pool's pages: a Shutdown() from inside Receive's dispatch,
	// or from code handling a packet Receive returned, must not free the packet under it. The
	// pages are reused by the next session and freed by a later Shutdown() or the destructor.
	packetAllocationPoolMutex.Lock();
	if (packetsOutstanding == 0)
		packetAllocationPool.Clear(_FILE_AND_LINE_);
	packetAllocationPoolMutex.Unlock();

	/*
	if (isRecvFromLoopThreadActive.GetValue()>0)
	{
		timeout = MafiaNet::GetTimeMS()+1000;
		while ( isRecvFromLoopThreadActive.GetValue()>0 && MafiaNet::GetTimeMS()<timeout )
		{
			RakSleep(30);
		}
	}
	*/

	DerefAllSockets();

	ClearBufferedCommands();
	ClearBufferedPackets();
	ClearSocketQueryOutput();
	bytesSentPerSecond = bytesReceivedPerSecond = 0;

	ClearRequestedConnectionList();


	// Clear out the reliability layer list in case we want to reallocate it in a successive call to Init.
	RemoteSystemStruct *temp = remoteSystemList;
	remoteSystemList = 0;
	MafiaNet::OP_DELETE_ARRAY(temp, _FILE_AND_LINE_);
	MafiaNet::OP_DELETE_ARRAY(activeSystemList, _FILE_AND_LINE_);
	activeSystemList = 0;

	ClearRemoteSystemLookup();

#ifdef USE_THREADED_SEND
	MafiaNet::SendToThread::Deref();
#endif

	ResetSendReceipt();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns true if the network threads are running
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
inline bool RakPeer::IsActive(void) const
{
	return endThreads == false;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Fills the array remoteSystems with the systemAddress of all the systems we are connected to
//
// Parameters:
// remoteSystems (out): An array of SystemAddress structures to be filled with the SystemAddresss of the systems we are connected to
// - pass 0 to remoteSystems to only get the number of systems we are connected to
// numberOfSystems (int, out): As input, the size of remoteSystems array.  As output, the number of elements put into the array
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::GetConnectionList(SystemAddress *remoteSystems, unsigned short *numberOfSystems) const
{
	if (numberOfSystems == 0)
		return false;

	if (remoteSystemList == 0 || endThreads == true)
	{
		if (numberOfSystems)
			*numberOfSystems = 0;
		return false;
	}

	DataStructures::List<SystemAddress> addresses;
	DataStructures::List<RakNetGUID> guids;
	GetSystemList(addresses, guids);
	if (remoteSystems)
	{
		unsigned short i;
		for (i = 0; i < *numberOfSystems && i < addresses.Size(); i++)
			remoteSystems[i] = addresses[i];
		*numberOfSystems = i;
	}
	else
	{
		*numberOfSystems = (unsigned short) addresses.Size();
	}
	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t RakPeer::GetNextSendReceipt(void)
{
	sendReceiptSerialMutex.Lock();
	uint32_t retVal = sendReceiptSerial;
	sendReceiptSerialMutex.Unlock();
	return retVal;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t RakPeer::IncrementNextSendReceipt(void)
{
	sendReceiptSerialMutex.Lock();
	uint32_t returned = sendReceiptSerial;
	if (++sendReceiptSerial == 0)
		sendReceiptSerial = 1;
	sendReceiptSerialMutex.Unlock();
	return returned;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Sends a block of data to the specified system that you are connected to.
// This function only works while the client is connected (Use the Connect function).
// The first byte should be a message identifier starting at ID_USER_PACKET_ENUM
//
// Parameters:
// data: The block of data to send
// length: The size in bytes of the data to send
// bitStream: The bitstream to send
// priority: What priority level to send on.
// reliability: How reliability to send this data
// orderingChannel: When using ordered or sequenced packets, what channel to order these on.
// - Packets are only ordered relative to other packets on the same stream
// systemAddress: Who to send this packet to, or in the case of broadcasting who not to send it to. Use UNASSIGNED_SYSTEM_ADDRESS to specify none
// broadcast: True to send this packet to all connected systems.  If true, then systemAddress specifies who not to send the packet to.
// Returns:
// \return 0 on bad input. Otherwise a number that identifies this message. If \a reliability is a type that returns a receipt, on a later call to Receive() you will get ID_SND_RECEIPT_ACKED or ID_SND_RECEIPT_LOSS with bytes 1-4 inclusive containing this number
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A peer mid-session-handshake has not been reported to the application, so the application must not be
// able to address it. Checked here rather than in SendImmediate because the handshake's own messages go
// through SendImmediate directly and must still reach that peer.
bool RakPeer::IsExchangingSessionData(const AddressOrGUID systemIdentifier)
{
	if (remoteSystemList == 0)
		return false;

	int index;
	if (systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS)
		index = GetIndexFromSystemAddress(systemIdentifier.systemAddress, false);
	else
		index = GetIndexFromGuid(systemIdentifier.rakNetGuid);

	return index != -1 && remoteSystemList[index].isActive &&
	       remoteSystemList[index].connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t RakPeer::Send(const char *data, const int length, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, uint32_t forceReceiptNumber)
{
#ifdef _DEBUG
	RakAssert(data && length > 0);
#endif
	RakAssert(!((unsigned int) reliability >= MafiaNet::NUMBER_OF_RELIABILITIES || (int) reliability < 0));
	RakAssert(!((int) priority > (int) MafiaNet::NUMBER_OF_PRIORITIES || (int) priority < 0));
	RakAssert(!(orderingChannel >= NUMBER_OF_ORDERED_STREAMS));

	if (data == 0 || length < 0)
		return 0;

	if (remoteSystemList == 0 || endThreads == true)
		return 0;

	if (broadcast == false && systemIdentifier.IsUndefined())
		return 0;

	if (broadcast == false && IsExchangingSessionData(systemIdentifier))
		return 0;

	uint32_t usedSendReceipt;
	if (forceReceiptNumber != 0)
		usedSendReceipt = forceReceiptNumber;
	else
		usedSendReceipt = IncrementNextSendReceipt();

	if (broadcast == false && IsLoopbackAddress(systemIdentifier, true))
	{
		SendLoopback(data, length);

		if (reliability >= MafiaNet::Reliability::UnreliableWithAckReceipt)
		{
			char buff[5];
			buff[0] = ID_SND_RECEIPT_ACKED;
			sendReceiptSerialMutex.Lock();
			memcpy(buff + 1, &sendReceiptSerial, 4);
			sendReceiptSerialMutex.Unlock();
			SendLoopback(buff, 5);
		}

		return usedSendReceipt;
	}

	SendBuffered(data, length * 8, priority, reliability, orderingChannel, systemIdentifier, broadcast, RemoteSystemStruct::NO_ACTION, usedSendReceipt);

	return usedSendReceipt;
}

void RakPeer::SendLoopback(const char *data, const int length)
{
	if (data == 0 || length < 0)
		return;

	Packet *packet = AllocPacket(length, _FILE_AND_LINE_);
	memcpy(packet->data, data, length);
	packet->systemAddress = GetLoopbackAddress();
	packet->guid = myGuid;
	PushBackPacket(packet, false);
}

uint32_t RakPeer::Send(const MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, uint32_t forceReceiptNumber)
{
#ifdef _DEBUG
	RakAssert(bitStream->GetNumberOfBytesUsed() > 0);
#endif

	RakAssert(!((unsigned int) reliability >= MafiaNet::NUMBER_OF_RELIABILITIES || (int) reliability < 0));
	RakAssert(!((int) priority > (int) MafiaNet::NUMBER_OF_PRIORITIES || (int) priority < 0));
	RakAssert(!(orderingChannel >= NUMBER_OF_ORDERED_STREAMS));

	if (bitStream->GetNumberOfBytesUsed() == 0)
		return 0;

	if (remoteSystemList == 0 || endThreads == true)
		return 0;

	if (broadcast == false && systemIdentifier.IsUndefined())
		return 0;

	if (broadcast == false && IsExchangingSessionData(systemIdentifier))
		return 0;

	uint32_t usedSendReceipt;
	if (forceReceiptNumber != 0)
		usedSendReceipt = forceReceiptNumber;
	else
		usedSendReceipt = IncrementNextSendReceipt();

	if (broadcast == false && IsLoopbackAddress(systemIdentifier, true))
	{
		SendLoopback((const char *) bitStream->GetData(), bitStream->GetNumberOfBytesUsed());
		if (reliability >= MafiaNet::Reliability::UnreliableWithAckReceipt)
		{
			char buff[5];
			buff[0] = ID_SND_RECEIPT_ACKED;
			sendReceiptSerialMutex.Lock();
			memcpy(buff + 1, &sendReceiptSerial, 4);
			sendReceiptSerialMutex.Unlock();
			SendLoopback(buff, 5);
		}
		return usedSendReceipt;
	}

	// Sends need to be buffered and processed in the update thread because the systemAddress associated with the reliability layer can change,
	// from that thread, resulting in a send to the wrong player!  While I could mutex the systemAddress, that is much slower than doing this
	SendBuffered((const char *) bitStream->GetData(), bitStream->GetNumberOfBitsUsed(), priority, reliability, orderingChannel, systemIdentifier, broadcast, RemoteSystemStruct::NO_ACTION, usedSendReceipt);


	return usedSendReceipt;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Sends multiple blocks of data, concatenating them automatically.
//
// This is equivalent to:
// MafiaNet::BitStream bs;
// bs.WriteAlignedBytes(block1, blockLength1);
// bs.WriteAlignedBytes(block2, blockLength2);
// bs.WriteAlignedBytes(block3, blockLength3);
// Send(&bs, ...)
//
// This function only works while connected
// \param[in] data An array of pointers to blocks of data
// \param[in] lengths An array of integers indicating the length of each block of data
// \param[in] numParameters Length of the arrays data and lengths
// \param[in] priority What priority level to send on.  See packet_priority.h
// \param[in] reliability How reliability to send this data.  See packet_priority.h
// \param[in] orderingChannel When using ordered or sequenced messages, what channel to order these on. Messages are only ordered relative to other messages on the same stream
// \param[in] systemIdentifier Who to send this packet to, or in the case of broadcasting who not to send it to. Pass either a SystemAddress structure or a RakNetGUID structure. Use UNASSIGNED_SYSTEM_ADDRESS or to specify none
// \param[in] broadcast True to send this packet to all connected systems. If true, then systemAddress specifies who not to send the packet to.
// \return False if we are not connected to the specified recipient.  True otherwise
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t RakPeer::SendList(const char **data, const int *lengths, const int numParameters, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, uint32_t forceReceiptNumber)
{
#ifdef _DEBUG
	RakAssert(data);
#endif

	if (data == 0 || lengths == 0)
		return 0;

	if (remoteSystemList == 0 || endThreads == true)
		return 0;

	if (numParameters == 0)
		return 0;

	if (lengths == 0)
		return 0;

	if (broadcast == false && systemIdentifier.IsUndefined())
		return 0;

	if (broadcast == false && IsExchangingSessionData(systemIdentifier))
		return 0;

	uint32_t usedSendReceipt;
	if (forceReceiptNumber != 0)
		usedSendReceipt = forceReceiptNumber;
	else
		usedSendReceipt = IncrementNextSendReceipt();

	SendBufferedList(data, lengths, numParameters, priority, reliability, orderingChannel, systemIdentifier, broadcast, RemoteSystemStruct::NO_ACTION, usedSendReceipt);

	return usedSendReceipt;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Gets a packet from the incoming packet queue. Use DeallocatePacket to deallocate the packet after you are done with it.
// Check the Packet struct at the top of CoreNetworkStructures.h for the format of the struct
//
// Returns:
// 0 if no packets are waiting to be handled, otherwise an allocated packet
// If the client is not active this will also return 0, as all waiting packets are flushed when the client is Disconnected
// This also updates all memory blocks associated with synchronized memory and distributed objects
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
Packet *RakPeer::Receive(void)
{
	if (!(IsActive()))
		return 0;

	MafiaNet::Packet *packet;
	//	Packet **threadPacket;
	PluginReceiveResult pluginResult;

	int offset;
	unsigned int i;

	// User should call RunUpdateCycle and RunRecvFromOnce to do this commented code
	/*
#if RAKPEER_SINGLE_THREADED==1
	RakPeer::RecvFromStruct *recvFromStruct;
	for (i=0; i < socketList.Size(); i++)
	{
		for(;;)
		{
			recvFromStruct=bufferedPackets.Allocate( _FILE_AND_LINE_ );
			recvFromStruct->s=socketList[i]->s;
			recvFromStruct->extraSocketOptions=socketList[i]->extraSocketOptions;
			SocketLayer::RecvFromBlocking(
				recvFromStruct->s, this, recvFromStruct->extraSocketOptions, recvFromStruct->data, &recvFromStruct->bytesRead, &recvFromStruct->systemAddress, &recvFromStruct->timeRead);
			if (recvFromStruct->bytesRead<=0)
			{
				bufferedPackets.Deallocate(recvFromStruct, _FILE_AND_LINE_);
				break;
			}
			else
			{
				RakAssert(recvFromStruct->systemAddress.GetPort());
				bufferedPackets.Push(recvFromStruct);
			}
		}
	}

	BitStream updateBitStream( MAXIMUM_MTU_SIZE );
	RunUpdateCycle(0, 0, updateBitStream);
#endif
	*/

	for (i = 0; i < pluginListTS.Size(); i++)
	{
		pluginListTS[i]->Update();
	}
	for (i = 0; i < pluginListNTS.Size(); i++)
	{
		pluginListNTS[i]->Update();
	}

	do
	{
		packetReturnMutex.Lock();
		if (packetReturnQueue.IsEmpty())
			packet = 0;
		else
			packet = packetReturnQueue.Pop();
		packetReturnMutex.Unlock();
		if (packet == 0)
			return 0;

		//		unsigned char msgId;
		if ((packet->length >= sizeof(unsigned char) + sizeof(MafiaNet::Time)) &&
		    ((unsigned char) packet->data[0] == ID_TIMESTAMP))
		{
			offset = sizeof(unsigned char);
			ShiftIncomingTimestamp(packet->data + offset, packet->systemAddress);
			//			msgId=packet->data[sizeof(unsigned char) + sizeof( MafiaNet::Time )];
		}
		//		else
		//		msgId=packet->data[0];

		// Some locally generated packets need to be processed by plugins, for example ID_FCM2_NEW_HOST
		// The plugin itself should intercept these messages generated remotely
		// 		if (packet->wasGeneratedLocally)
		// 			return packet;


		// A callback may shut the peer down. The plugins have been reset by then and the packet
		// belongs to a closed session, so it goes no further and Receive returns nothing.
		const unsigned int generation = shutdownGeneration;

		CallPluginCallbacks(pluginListTS, packet);
		CallPluginCallbacks(pluginListNTS, packet);
		if (shutdownGeneration != generation)
		{
			DeallocatePacket(packet);
			return 0;
		}

		for (i = 0; i < pluginListTS.Size(); i++)
		{
			pluginResult = pluginListTS[i]->OnReceive(packet);
			if (shutdownGeneration != generation)
			{
				if (pluginResult != RR_STOP_PROCESSING)
					DeallocatePacket(packet);
				return 0;
			}
			if (pluginResult == RR_STOP_PROCESSING_AND_DEALLOCATE)
			{
				DeallocatePacket(packet);
				packet = 0; // Will do the loop again and get another packet
				break;      // break out of the enclosing for
			}
			else if (pluginResult == RR_STOP_PROCESSING)
			{
				packet = 0;
				break;
			}
		}

		for (i = 0; i < pluginListNTS.Size(); i++)
		{
			pluginResult = pluginListNTS[i]->OnReceive(packet);
			if (shutdownGeneration != generation)
			{
				if (pluginResult != RR_STOP_PROCESSING)
					DeallocatePacket(packet);
				return 0;
			}
			if (pluginResult == RR_STOP_PROCESSING_AND_DEALLOCATE)
			{
				DeallocatePacket(packet);
				packet = 0; // Will do the loop again and get another packet
				break;      // break out of the enclosing for
			}
			else if (pluginResult == RR_STOP_PROCESSING)
			{
				packet = 0;
				break;
			}
		}

	} while (packet == 0);

#ifdef _DEBUG
	RakAssert(packet->data);
#endif

	return packet;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Call this to deallocate a packet returned by Receive
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::DeallocatePacket(Packet *packet)
{
	if (packet == 0)
		return;

	if (packet->deleteData)
	{
		rakFree_Ex(packet->data, _FILE_AND_LINE_);
		packet->~Packet();
		packetAllocationPoolMutex.Lock();
		packetAllocationPool.Release(packet, _FILE_AND_LINE_);
		--packetsOutstanding;
		packetAllocationPoolMutex.Unlock();
	}
	else
	{
		rakFree_Ex(packet, _FILE_AND_LINE_);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Return the total number of connections we are allowed
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetMaximumNumberOfPeers(void) const
{
	return maximumNumberOfPeers;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Close the connection to another host (if we initiated the connection it will disconnect, if they did it will kick them out).
//
// Parameters:
// target: Which connection to close
// sendDisconnectionNotification: True to send ID_DISCONNECTION_NOTIFICATION to the recipient. False to close it silently.
// channel: If blockDuration > 0, the disconnect packet will be sent on this channel
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::CloseConnection(const AddressOrGUID target, bool sendDisconnectionNotification, unsigned char orderingChannel, MafiaNet::Priority disconnectionNotificationPriority, const MafiaNet::BitStream *reasonData)
{
	/*
	// This only be called from the user thread, for the user shutting down.
	// From the network thread, this should occur because of ID_DISCONNECTION_NOTIFICATION and ID_CONNECTION_LOST
	unsigned j;
	for (j=0; j < messageHandlerList.Size(); j++)
	{
		messageHandlerList[j]->OnClosedConnection(
			target.systemAddress==UNASSIGNED_SYSTEM_ADDRESS ? GetSystemAddressFromGuid(target.rakNetGuid) : target.systemAddress,
			target.rakNetGuid==UNASSIGNED_RAKNET_GUID ? GetGuidFromSystemAddress(target.systemAddress) : target.rakNetGuid,
			LCR_CLOSED_BY_USER);
	}
	*/

	const SystemAddress address = (target.systemAddress == UNASSIGNED_SYSTEM_ADDRESS) ? GetSystemAddressFromGuid(target.rakNetGuid) : target.systemAddress;
	int remoteSystemListIndex = GetIndexFromSystemAddress(address);

	// Resolve the socket to close on WITHOUT assuming a valid slot index.
	// GetIndexFromSystemAddress returns -1 when the target isn't in the list; never
	// coerce that to 0 — reading remoteSystemList[0] would crash if the list is
	// unallocated, or target an unrelated peer's slot. When the index is valid use
	// that slot's socket (it may itself be null during rapid connect/disconnect
	// churn). Either way, fall back to the primary socket — the same pattern used by
	// the BCS_CLOSE_CONNECTION path below. With no socket at all there is nothing to
	// close, so bail out.
	RakNetSocket2 *closeSocket = (remoteSystemListIndex != -1) ? remoteSystemList[remoteSystemListIndex].rakNetSocket : nullptr;
	if (closeSocket == nullptr && socketList.Size() > 0)
		closeSocket = socketList[0];
	if (closeSocket == nullptr)
		return;
	CloseConnectionInternal2(target, sendDisconnectionNotification, false, orderingChannel, disconnectionNotificationPriority, *closeSocket, reasonData);

	// 12/14/09 Return ID_CONNECTION_LOST when calling CloseConnection with sendDisconnectionNotification==false, elsewise it is never returned
	if (sendDisconnectionNotification == false && GetConnectionState(target) == IS_CONNECTED)
	{
		Packet *packet = AllocPacket(sizeof(char), _FILE_AND_LINE_);
		packet->data[0] = ID_CONNECTION_LOST; // DeadConnection
		packet->guid = target.rakNetGuid == UNASSIGNED_RAKNET_GUID ? GetGuidFromSystemAddress(target.systemAddress) : target.rakNetGuid;
		packet->systemAddress = address;
		packet->systemAddress.systemIndex = static_cast<SystemIndex>(remoteSystemListIndex == -1 ? 0 : remoteSystemListIndex);
		packet->guid.systemIndex = packet->systemAddress.systemIndex;
		packet->wasGeneratedLocally = true; // else processed twice
		AddPacketToProducer(packet);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Cancel a pending connection attempt
// If we are already connected, the connection stays open
// \param[in] target Which system to cancel
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::CancelConnectionAttempt(const SystemAddress target)
{
	unsigned int i;

	// Cancel pending connection attempt, if there is one. Any candidate of a
	// Happy Eyeballs race, or the race's first address, cancels the whole race.
	//
	// This may run on any thread while the network thread is using a request
	// with the queue unlocked, so nothing is freed here: the requests are only
	// flagged, and the network thread drops them (see RunUpdateCycle).
	requestedConnectionQueueMutex.Lock();
	for (i = 0; i < requestedConnectionQueue.Size(); i++)
	{
		RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (RequestMatches(rcs, target))
		{
			rcs->cancelled = true;
			if (rcs->raceGroup != 0)
			{
				for (unsigned int k = 0; k < requestedConnectionQueue.Size(); k++)
				{
					if (requestedConnectionQueue[k]->raceGroup == rcs->raceGroup)
						requestedConnectionQueue[k]->cancelled = true;
				}
			}
			break;
		}
	}
	requestedConnectionQueueMutex.Unlock();
}

bool RakPeer::RequestMatches(const RequestedConnectionStruct *rcs, const SystemAddress &address)
{
	if (rcs->cancelled)
		return false;
	return rcs->systemAddress == address || (rcs->raceGroup != 0 && rcs->raceFirstAddress == address);
}

void RakPeer::DeleteRequestedConnection(RequestedConnectionStruct *rcs)
{
	if (rcs->data)
	{
		rakFree_Ex(rcs->data, _FILE_AND_LINE_);
		rcs->data = 0;
	}
	if (rcs->ephemeral)
	{
		// The destructor wipes too; wiping first keeps the secret's lifetime obvious.
		rcs->ephemeral->Wipe();
		MafiaNet::OP_DELETE(rcs->ephemeral, _FILE_AND_LINE_);
		rcs->ephemeral = nullptr;
	}
	MafiaNet::OP_DELETE(rcs, _FILE_AND_LINE_);
}

void RakPeer::RemoveRequestedConnection_NoLock(const RequestedConnectionStruct *rcs)
{
	for (unsigned int i = 0; i < requestedConnectionQueue.Size(); i++)
	{
		if (requestedConnectionQueue[i] == rcs)
		{
			requestedConnectionQueue.RemoveAtIndex(i);
			return;
		}
	}
}

void RakPeer::CancelRaceSiblings_NoLock(const RequestedConnectionStruct *keep)
{
	if (keep->raceGroup == 0)
		return;
	unsigned int i = 0;
	while (i < requestedConnectionQueue.Size())
	{
		RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (rcs != keep && rcs->raceGroup == keep->raceGroup)
		{
			requestedConnectionQueue.RemoveAtIndex(i);
			DeleteRequestedConnection(rcs);
		}
		else
			i++;
	}
}

bool RakPeer::DropConnectionAttemptSupersededBy(const SystemAddress &address)
{
	RequestedConnectionStruct *superseded = nullptr;
	requestedConnectionQueueMutex.Lock();
	for (unsigned int i = 0; i < requestedConnectionQueue.Size(); i++)
	{
		RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (rcs->systemAddress == address && !rcs->cancelled && rcs->actionToTake == RequestedConnectionStruct::CONNECT)
		{
			superseded = rcs;
			break;
		}
	}
	if (superseded != nullptr)
	{
		CancelRaceSiblings_NoLock(superseded);
		RemoveRequestedConnection_NoLock(superseded);
	}
	requestedConnectionQueueMutex.Unlock();
	if (superseded == nullptr)
		return false;
	DeleteRequestedConnection(superseded); // wipes its ephemeral key pair
	return true;
}

bool RakPeer::AdvanceRace_NoLock(uint32_t raceGroup, MafiaNet::Time now)
{
	bool anyLeft = false;
	bool anyUnstarted = false;
	MafiaNet::Time earliest = 0;
	for (unsigned int i = 0; i < requestedConnectionQueue.Size(); i++)
	{
		const RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (rcs->raceGroup != raceGroup || rcs->cancelled)
			continue;
		anyLeft = true;
		if (rcs->requestsMade == 0 && (!anyUnstarted || rcs->nextCandidateTime < earliest))
		{
			earliest = rcs->nextCandidateTime;
			anyUnstarted = true;
		}
	}
	// Pull the next candidate forward to now; the ones after it keep their spacing.
	if (anyUnstarted && earliest > now)
	{
		const MafiaNet::Time shift = earliest - now;
		for (unsigned int i = 0; i < requestedConnectionQueue.Size(); i++)
		{
			RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
			if (rcs->raceGroup == raceGroup && rcs->requestsMade == 0)
				rcs->nextCandidateTime -= shift;
		}
	}
	return anyLeft;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ConnectionState RakPeer::GetConnectionState(const AddressOrGUID systemIdentifier)
{
	if (systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS)
	{
		unsigned int i = 0;
		requestedConnectionQueueMutex.Lock();
		for (; i < requestedConnectionQueue.Size(); i++)
		{
			if (RequestMatches(requestedConnectionQueue[i], systemIdentifier.systemAddress))
			{
				requestedConnectionQueueMutex.Unlock();
				return IS_PENDING;
			}
		}
		requestedConnectionQueueMutex.Unlock();
	}

	int index;
	if (systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS)
	{
		index = GetIndexFromSystemAddress(systemIdentifier.systemAddress, false);
		// Nothing, or only a closed slot that once held the address: the address may also be the first candidate's
		// of a Happy Eyeballs race a later candidate won (see Connect()), and that live connection comes first.
		if (index == -1 || !remoteSystemList[index].isActive)
		{
			for (unsigned int i = 0; i < maximumNumberOfPeers; i++)
			{
				if (remoteSystemList[i].isActive && remoteSystemList[i].raceFirstAddress == systemIdentifier.systemAddress)
				{
					index = (int) i;
					break;
				}
			}
		}
	}
	else
	{
		index = GetIndexFromGuid(systemIdentifier.rakNetGuid);
	}

	if (index == -1)
		return IS_NOT_CONNECTED;

	if (remoteSystemList[index].isActive == false)
		return IS_DISCONNECTED;

	switch (remoteSystemList[index].connectMode)
	{
	case RemoteSystemStruct::DISCONNECT_ASAP:
		return IS_DISCONNECTING;
	case RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY:
		return IS_SILENTLY_DISCONNECTING;
	case RemoteSystemStruct::DISCONNECT_ON_NO_ACK:
		return IS_DISCONNECTING;
	case RemoteSystemStruct::REQUESTED_CONNECTION:
		return IS_CONNECTING;
	case RemoteSystemStruct::HANDLING_CONNECTION_REQUEST:
		return IS_CONNECTING;
	case RemoteSystemStruct::UNVERIFIED_SENDER:
		return IS_CONNECTING;
	case RemoteSystemStruct::EXCHANGING_SESSION_DATA:
		// Transport is up but the session handshake has not completed, so the application has not been
		// told this connection exists yet.
		return IS_CONNECTING;
	case RemoteSystemStruct::CONNECTED:
		return IS_CONNECTED;
	default:
		return IS_NOT_CONNECTED;
	}
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Given a systemAddress, returns an index from 0 to the maximum number of players allowed - 1.
//
// Parameters
// systemAddress - The systemAddress to search for
//
// Returns
// An integer from 0 to the maximum number of peers -1, or -1 if that player is not found
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetIndexFromSystemAddress(const SystemAddress systemAddress) const
{
	return GetIndexFromSystemAddress(systemAddress, false);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// This function is only useful for looping through all players.
//
// Parameters
// index - an integer between 0 and the maximum number of players allowed - 1.
//
// Returns
// A valid systemAddress or UNASSIGNED_SYSTEM_ADDRESS if no such player at that index
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
SystemAddress RakPeer::GetSystemAddressFromIndex(unsigned int index)
{
	// remoteSystemList in user thread
	//if ( index >= 0 && index < remoteSystemListSize )
	if (index < maximumNumberOfPeers)
		if (remoteSystemList[index].isActive && remoteSystemList[index].connectMode == RakPeer::RemoteSystemStruct::CONNECTED) // Don't give the user players that aren't fully connected, since sends will fail
			return remoteSystemList[index].systemAddress;

	return UNASSIGNED_SYSTEM_ADDRESS;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Same as GetSystemAddressFromIndex but returns RakNetGUID
// \param[in] index Index should range between 0 and the maximum number of players allowed - 1.
// \return The RakNetGUID
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakNetGUID RakPeer::GetGUIDFromIndex(unsigned int index)
{
	// remoteSystemList in user thread
	//if ( index >= 0 && index < remoteSystemListSize )
	if (index < maximumNumberOfPeers)
		if (remoteSystemList[index].isActive && remoteSystemList[index].connectMode == RakPeer::RemoteSystemStruct::CONNECTED) // Don't give the user players that aren't fully connected, since sends will fail
			return remoteSystemList[index].guid;

	return UNASSIGNED_RAKNET_GUID;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Same as calling GetSystemAddressFromIndex and GetGUIDFromIndex for all systems, but more efficient
// Indices match each other, so \a addresses[0] and \a guids[0] refer to the same system
// \param[out] addresses All system addresses. Size of the list is the number of connections. Size of the list will match the size of the \a guids list.
// \param[out] guids All guids. Size of the list is the number of connections. Size of the list will match the size of the \a addresses list.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetSystemList(DataStructures::List<SystemAddress> &addresses, DataStructures::List<RakNetGUID> &guids) const
{
	addresses.Clear(false, _FILE_AND_LINE_);
	guids.Clear(false, _FILE_AND_LINE_);

	if (remoteSystemList == 0 || endThreads == true)
		return;

	unsigned int i;
	for (i = 0; i < activeSystemListSize; i++)
	{
		if ((activeSystemList[i])->isActive &&
		    (activeSystemList[i])->connectMode == RakPeer::RemoteSystemStruct::CONNECTED)
		{
			addresses.Push((activeSystemList[i])->systemAddress, _FILE_AND_LINE_);
			guids.Push((activeSystemList[i])->guid, _FILE_AND_LINE_);
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Bans an IP from connecting. Banned IPs persist between connections.
//
// Parameters
// IP - Dotted IP address.  Can use * as a wildcard, such as 128.0.0.* will ban
// All IP addresses starting with 128.0.0
// milliseconds - how many ms for a temporary ban.  Use 0 for a permanent ban
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::AddToBanList(const char *IP, MafiaNet::TimeMS milliseconds)
{
	unsigned index;
	MafiaNet::TimeMS time = MafiaNet::GetTimeMS();

	if (IP == 0 || IP[0] == 0 || strlen(IP) > 15)
		return;

	// If this guy is already in the ban list, do nothing
	index = 0;

	banListMutex.Lock();

	for (; index < banList.Size(); index++)
	{
		if (strcmp(IP, banList[index]->IP) == 0)
		{
			// Already in the ban list.  Just update the time
			if (milliseconds == 0)
				banList[index]->timeout = 0; // Infinite
			else
				banList[index]->timeout = time + milliseconds;
			banListMutex.Unlock();
			return;
		}
	}

	banListMutex.Unlock();

	BanStruct *banStruct = MafiaNet::OP_NEW<BanStruct>(_FILE_AND_LINE_);
	banStruct->IP = (char *) rakMalloc_Ex(16, _FILE_AND_LINE_);
	if (milliseconds == 0)
		banStruct->timeout = 0; // Infinite
	else
		banStruct->timeout = time + milliseconds;
	strcpy_s(banStruct->IP, 16, IP);
	banListMutex.Lock();
	banList.Insert(banStruct, _FILE_AND_LINE_);
	banListMutex.Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Allows a previously banned IP to connect.
//
// Parameters
// IP - Dotted IP address.  Can use * as a wildcard, such as 128.0.0.* will ban
// All IP addresses starting with 128.0.0
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::RemoveFromBanList(const char *IP)
{
	unsigned index;
	BanStruct *temp;

	if (IP == 0 || IP[0] == 0 || strlen(IP) > 15)
		return;

	index = 0;
	temp = 0;

	banListMutex.Lock();

	for (; index < banList.Size(); index++)
	{
		if (strcmp(IP, banList[index]->IP) == 0)
		{
			temp = banList[index];
			banList[index] = banList[banList.Size() - 1];
			banList.RemoveAtIndex(banList.Size() - 1);
			break;
		}
	}

	banListMutex.Unlock();

	if (temp)
	{
		rakFree_Ex(temp->IP, _FILE_AND_LINE_);
		MafiaNet::OP_DELETE(temp, _FILE_AND_LINE_);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Allows all previously banned IPs to connect.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearBanList(void)
{
	unsigned index;
	index = 0;
	banListMutex.Lock();

	for (; index < banList.Size(); index++)
	{
		rakFree_Ex(banList[index]->IP, _FILE_AND_LINE_);
		MafiaNet::OP_DELETE(banList[index], _FILE_AND_LINE_);
	}

	banList.Clear(false, _FILE_AND_LINE_);

	banListMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetLimitIPConnectionFrequency(bool b)
{
	limitConnectionFrequencyFromTheSameIP = b;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Determines if a particular IP is banned.
//
// Parameters
// IP - Complete dotted IP address
//
// Returns
// True if IP matches any IPs in the ban list, accounting for any wildcards.
// False otherwise.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::IsBanned(const char *IP)
{
	unsigned banListIndex, characterIndex;
	MafiaNet::TimeMS time;
	BanStruct *temp;

	if (IP == 0 || IP[0] == 0 || strlen(IP) > 15)
		return false;

	banListIndex = 0;

	if (banList.Size() == 0)
		return false; // Skip the mutex if possible

	time = MafiaNet::GetTimeMS();

	banListMutex.Lock();

	while (banListIndex < banList.Size())
	{
		if (banList[banListIndex]->timeout > 0 && banList[banListIndex]->timeout < time)
		{
			// Delete expired ban
			temp = banList[banListIndex];
			banList[banListIndex] = banList[banList.Size() - 1];
			banList.RemoveAtIndex(banList.Size() - 1);
			rakFree_Ex(temp->IP, _FILE_AND_LINE_);
			MafiaNet::OP_DELETE(temp, _FILE_AND_LINE_);
		}
		else
		{
			characterIndex = 0;

			for (;;)
			{
				if (banList[banListIndex]->IP[characterIndex] == IP[characterIndex])
				{
					// Equal characters

					if (IP[characterIndex] == 0)
					{
						banListMutex.Unlock();
						// End of the string and the strings match

						return true;
					}

					characterIndex++;
				}

				else
				{
					if (banList[banListIndex]->IP[characterIndex] == 0 || IP[characterIndex] == 0)
					{
						// End of one of the strings
						break;
					}

					// Characters do not match
					if (banList[banListIndex]->IP[characterIndex] == '*')
					{
						banListMutex.Unlock();

						// Domain is banned.
						return true;
					}

					// Characters do not match and it is not a *
					break;
				}
			}

			banListIndex++;
		}
	}

	banListMutex.Unlock();

	// No match found.
	return false;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Send a ping to the specified connected system.
//
// Parameters:
// target - who to ping
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::Ping(const SystemAddress target)
{
	PingInternal(target, false, MafiaNet::Reliability::Unreliable);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Send a ping to the specified unconnected system.
// The remote system, if it is Initialized, will respond with ID_UNCONNECTED_PONG.
// The final ping time will be encoded in the following sizeof(MafiaNet::TimeMS) bytes.  (Default is 4 bytes - See __GET_TIME_64BIT in types.h
//
// Parameters:
// host: Either a dotted IP address or a domain name.  Can be 255.255.255.255 for LAN broadcast.
// remotePort: Which port to connect to on the remote machine.
// onlyReplyOnAcceptingConnections: Only request a reply if the remote system has open connections
// connectionSocketIndex Index into the array of socket descriptors passed to socketDescriptors in RakPeer::Startup() to send on.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::Ping(const char *host, unsigned short remotePort, bool onlyReplyOnAcceptingConnections, unsigned connectionSocketIndex)
{
	if (host == 0)
		return false;

	// If this assert hits then Startup wasn't called or the call failed.
	RakAssert(connectionSocketIndex < socketList.Size());

	//	if ( IsActive() == false )
	//		return;

	MafiaNet::BitStream bitStream(sizeof(unsigned char) + sizeof(MafiaNet::Time));
	if (onlyReplyOnAcceptingConnections)
		bitStream.Write((MessageID) ID_UNCONNECTED_PING_OPEN_CONNECTIONS);
	else
		bitStream.Write((MessageID) ID_UNCONNECTED_PING);

	bitStream.Write(MafiaNet::GetTime());

	bitStream.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));

	bitStream.Write(GetMyGUID());

	// No timestamp for 255.255.255.255
	unsigned int realIndex = GetRakNetSocketFromUserConnectionSocketIndex(connectionSocketIndex);
	/*

	SystemAddress systemAddress;
	systemAddress.FromStringExplicitPort(host,remotePort, socketList[realIndex]->GetBoundAddress().GetIPVersion());
	systemAddress.FixForIPVersion(socketList[realIndex]->GetBoundAddress());

	unsigned i;
	for (i=0; i < pluginListNTS.Size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char*)bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), systemAddress);
	SocketLayer::SendTo( socketList[realIndex], (const char*)bitStream.GetData(), (int) bitStream.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );
	*/

	RNS2_SendParameters bsp;
	bsp.data = (char *) bitStream.GetData();
	bsp.length = bitStream.GetNumberOfBytesUsed();
	if (!ResolveOneShotTarget(host, remotePort, socketList[realIndex], &bsp.systemAddress) ||
	    bsp.systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
		return false;
	unsigned i;
	for (i = 0; i < pluginListNTS.Size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char *) bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), bsp.systemAddress);
	socketList[realIndex]->Send(&bsp, _FILE_AND_LINE_);

	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns the average of all ping times read for a specified target
//
// Parameters:
// target - whose time to read
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetAveragePing(const AddressOrGUID systemIdentifier)
{
	int sum, quantity;
	RemoteSystemStruct *remoteSystem = GetRemoteSystem(systemIdentifier, false, false);

	if (remoteSystem == 0)
		return -1;

	for (sum = 0, quantity = 0; quantity < PING_TIMES_ARRAY_SIZE; quantity++)
	{
		if (remoteSystem->pingAndClockDifferential[quantity].pingTime == 65535)
			break;
		else
			sum += remoteSystem->pingAndClockDifferential[quantity].pingTime;
	}

	if (quantity > 0)
		return sum / quantity;
	else
		return -1;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns the last ping time read for the specific player or -1 if none read yet
//
// Parameters:
// target - whose time to read
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetLastPing(const AddressOrGUID systemIdentifier) const
{
	RemoteSystemStruct *remoteSystem = GetRemoteSystem(systemIdentifier, false, false);

	if (remoteSystem == 0)
		return -1;

	//	return (int)(remoteSystem->reliabilityLayer.GetAckPing()/(MafiaNet::TimeUS)1000);

	if (remoteSystem->pingAndClockDifferentialWriteIndex == 0)
		return remoteSystem->pingAndClockDifferential[PING_TIMES_ARRAY_SIZE - 1].pingTime;
	else
		return remoteSystem->pingAndClockDifferential[remoteSystem->pingAndClockDifferentialWriteIndex - 1].pingTime;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns the lowest ping time read or -1 if none read yet
//
// Parameters:
// target - whose time to read
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetLowestPing(const AddressOrGUID systemIdentifier) const
{
	RemoteSystemStruct *remoteSystem = GetRemoteSystem(systemIdentifier, false, false);

	if (remoteSystem == 0)
		return -1;

	return remoteSystem->lowestPing;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Ping the remote systems every so often.  This is off by default
// This will work anytime
//
// Parameters:
// doPing - True to start occasional pings.  False to stop them.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetOccasionalPing(bool doPing)
{
	occasionalPing = doPing;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/// Return the clock difference between your system and the specified system
/// Subtract the time from a time returned by the remote system to get that time relative to your own system
/// Returns 0 if the system is unknown
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
MafiaNet::Time RakPeer::GetClockDifferential(const AddressOrGUID systemIdentifier)
{
	RemoteSystemStruct *remoteSystem = GetRemoteSystem(systemIdentifier, false, false);
	if (remoteSystem == 0)
		return 0;
	return GetClockDifferentialInt(remoteSystem);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

MafiaNet::Time RakPeer::GetClockDifferentialInt(RemoteSystemStruct *remoteSystem) const
{
	int counter, lowestPingSoFar;
	MafiaNet::Time clockDifferential;

	lowestPingSoFar = 65535;

	clockDifferential = 0;

	for (counter = 0; counter < PING_TIMES_ARRAY_SIZE; counter++)
	{
		if (remoteSystem->pingAndClockDifferential[counter].pingTime == 65535)
			break;

		if (remoteSystem->pingAndClockDifferential[counter].pingTime < lowestPingSoFar)
		{
			clockDifferential = remoteSystem->pingAndClockDifferential[counter].clockDifferential;
			lowestPingSoFar = remoteSystem->pingAndClockDifferential[counter].pingTime;
		}
	}

	return clockDifferential;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Length should be under 400 bytes, as a security measure against flood attacks
// Sets the data to send with an  (LAN server discovery) /(offline ping) response
// See the Ping sample project for how this is used.
// data: a block of data to store, or 0 for none
// length: The length of data in bytes, or 0 for none
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetOfflinePingResponse(const char *data, const unsigned int length)
{
	RakAssert(length < 400);

	rakPeerMutexes[offlinePingResponse_Mutex].Lock();
	offlinePingResponse.Reset();

	if (data && length > 0)
		offlinePingResponse.Write(data, length);

	rakPeerMutexes[offlinePingResponse_Mutex].Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Returns pointers to a copy of the data passed to SetOfflinePingResponse
// \param[out] data A pointer to a copy of the data passed to \a SetOfflinePingResponse()
// \param[out] length A pointer filled in with the length parameter passed to SetOfflinePingResponse()
// \sa SetOfflinePingResponse
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetOfflinePingResponse(char **data, unsigned int *length)
{
	rakPeerMutexes[offlinePingResponse_Mutex].Lock();
	*data = (char *) offlinePingResponse.GetData();
	*length = (int) offlinePingResponse.GetNumberOfBytesUsed();
	rakPeerMutexes[offlinePingResponse_Mutex].Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Sets the payload this peer offers during the session handshake. Opaque to MafiaNet.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetSessionConfig(const char *data, unsigned int length)
{
	RakAssert(length <= MAXIMUM_SESSION_CONFIG_SIZE);
	if (length > MAXIMUM_SESSION_CONFIG_SIZE)
		length = MAXIMUM_SESSION_CONFIG_SIZE;

	rakPeerMutexes[sessionConfig_Mutex].Lock();
	sessionConfig.Reset();

	if (data && length > 0)
		sessionConfig.Write(data, length);

	rakPeerMutexes[sessionConfig_Mutex].Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetSessionConfig(char **data, unsigned int *length)
{
	rakPeerMutexes[sessionConfig_Mutex].Lock();
	*data = (char *) sessionConfig.GetData();
	*length = (unsigned int) sessionConfig.GetNumberOfBytesUsed();
	rakPeerMutexes[sessionConfig_Mutex].Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char *RakPeer::GetRemoteSessionConfig(const AddressOrGUID systemIdentifier, unsigned int *length)
{
	if (length)
		*length = 0;

	int index;
	if (systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS)
		index = GetIndexFromSystemAddress(systemIdentifier.systemAddress, false);
	else
		index = GetIndexFromGuid(systemIdentifier.rakNetGuid);

	if (index == -1 || remoteSystemList == 0 || remoteSystemList[index].isActive == false)
		return 0;

	if (length)
		*length = remoteSystemList[index].remoteSessionConfigLength;
	return (const char *) remoteSystemList[index].remoteSessionConfigData;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetSessionConfigInteractive(bool interactive)
{
	sessionConfigInteractive = interactive;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::AcceptSession(const AddressOrGUID systemIdentifier, const char *data, unsigned int length)
{
	RakAssert(length <= MAXIMUM_SESSION_CONFIG_SIZE);
	if (length > MAXIMUM_SESSION_CONFIG_SIZE)
		length = MAXIMUM_SESSION_CONFIG_SIZE;
	QueueSessionDecision(systemIdentifier, true, data, length);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::RejectSession(const AddressOrGUID systemIdentifier, const char *reason)
{
	unsigned int reasonLength = (reason && reason[0]) ? (unsigned int) strlen(reason) : 0;
	// Bounded like every other payload that crosses the handshake. A reason is meant to be a short
	// string, so an oversized one is a caller mistake rather than something to transmit.
	RakAssert(reasonLength <= MAXIMUM_SESSION_CONFIG_SIZE);
	if (reasonLength > MAXIMUM_SESSION_CONFIG_SIZE)
		reasonLength = MAXIMUM_SESSION_CONFIG_SIZE;
	QueueSessionDecision(systemIdentifier, false, reason, reasonLength);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetMaximumPendingSessions(unsigned short total, unsigned short perAddress)
{
	maximumPendingSessions = total;
	maximumPendingSessionsPerAddress = total != 0 ? perAddress : 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetSessionTimeout(MafiaNet::TimeMS timeMS)
{
	sessionTimeout = timeMS;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SendSessionStatus(const AddressOrGUID systemIdentifier, const char *data, unsigned int length)
{
	RakAssert(length <= MAXIMUM_SESSION_CONFIG_SIZE);
	if (length > MAXIMUM_SESSION_CONFIG_SIZE)
		length = MAXIMUM_SESSION_CONFIG_SIZE;
	QueueSessionCommand(systemIdentifier, BufferedCommandStruct::BCS_SESSION_STATUS, data, length);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Hand an application's accept/reject over to the network thread. Applying it here would send on the
// connection and mutate connectMode from the user thread, which the rest of RakPeer never does.
void RakPeer::QueueSessionDecision(const AddressOrGUID systemIdentifier, bool accept, const char *data, unsigned int length)
{
	QueueSessionCommand(systemIdentifier, accept ? BufferedCommandStruct::BCS_SESSION_ACCEPT : BufferedCommandStruct::BCS_SESSION_REJECT, data, length);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::QueueSessionCommand(const AddressOrGUID systemIdentifier, int command, const char *data, unsigned int length)
{
	BufferedCommandStruct *bcs;
	bcs = bufferedCommands.Allocate(_FILE_AND_LINE_);
	bcs->command = (decltype(bcs->command)) command;
	bcs->systemIdentifier = systemIdentifier;
	bcs->numberOfBitsToSend = BYTES_TO_BITS(length);
	bcs->broadcast = false;
	bcs->connectionMode = RemoteSystemStruct::NO_ACTION;
	bcs->orderingChannel = 0;
	bcs->priority = MafiaNet::Priority::Immediate;
	bcs->reliability = MafiaNet::Reliability::ReliableOrdered;
	bcs->data = 0;
	if (data && length > 0)
	{
		bcs->data = (char *) rakMalloc_Ex(length, _FILE_AND_LINE_);
		if (bcs->data)
			memcpy(bcs->data, data, length);
		else
			bcs->numberOfBitsToSend = 0;
	}
	bufferedCommands.Push(bcs);
	quitAndDataEvents.SetEvent();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Return the unique SystemAddress that represents you on the the network
// Note that unlike in previous versions, this is a struct and is not sequential
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
SystemAddress RakPeer::GetInternalID(const SystemAddress systemAddress, const int index) const
{
	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
	{
		return ipList[index];
	}
	else
	{

		//		SystemAddress returnValue;
		RemoteSystemStruct *remoteSystem = GetRemoteSystemFromSystemAddress(systemAddress, false, true);
		if (remoteSystem == 0)
			return UNASSIGNED_SYSTEM_ADDRESS;

		return remoteSystem->theirInternalSystemAddress[index];
		/*
		sockaddr_in sa;
		socklen_t len = sizeof(sa);
		if (getsockname__(connectionSockets[remoteSystem->connectionSocketIndex], (sockaddr*)&sa, &len)!=0)
			return UNASSIGNED_SYSTEM_ADDRESS;
		returnValue.port=ntohs(sa.sin_port);
		returnValue.binaryAddress=sa.sin_addr.s_addr;
		return returnValue;
*/
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/// \brief Sets your internal IP address, for platforms that do not support reading it, or to override a value
/// \param[in] systemAddress. The address to set. Use SystemAddress::FromString() if you want to use a dotted string
/// \param[in] index When you have multiple internal IDs, which index to set?
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetInternalID(SystemAddress systemAddress, int index)
{
	RakAssert(index >= 0 && index < MAXIMUM_NUMBER_OF_INTERNAL_IDS);
	ipList[index] = systemAddress;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Return the unique address identifier that represents you on the the network and is based on your external
// IP / port (the IP / port the specified player uses to communicate with you)
// Note that unlike in previous versions, this is a struct and is not sequential
//
// Parameters:
// target: Which remote system you are referring to for your external ID
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
SystemAddress RakPeer::GetExternalID(const SystemAddress target) const
{
	unsigned i;
	SystemAddress inactiveExternalId;

	inactiveExternalId = UNASSIGNED_SYSTEM_ADDRESS;

	if (target == UNASSIGNED_SYSTEM_ADDRESS)
		return firstExternalID;

	// First check for active connection with this systemAddress
	for (i = 0; i < maximumNumberOfPeers; i++)
	{
		if (remoteSystemList[i].systemAddress == target)
		{
			if (remoteSystemList[i].isActive)
				return remoteSystemList[i].myExternalSystemAddress;
			else if (remoteSystemList[i].myExternalSystemAddress != UNASSIGNED_SYSTEM_ADDRESS)
				inactiveExternalId = remoteSystemList[i].myExternalSystemAddress;
		}
	}

	return inactiveExternalId;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

const RakNetGUID RakPeer::GetMyGUID(void) const
{
	return myGuid;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
SystemAddress RakPeer::GetMyBoundAddress(const int socketIndex)
{
	DataStructures::List<RakNetSocket2 *> sockets;
	GetSockets(sockets);
	if (sockets.Size() > 0)
		return sockets[socketIndex]->GetBoundAddress();
	else
		return UNASSIGNED_SYSTEM_ADDRESS;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

const RakNetGUID &RakPeer::GetGuidFromSystemAddress(const SystemAddress input) const
{
	if (input == UNASSIGNED_SYSTEM_ADDRESS)
		return myGuid;

	if (input.systemIndex != (SystemIndex) -1 && input.systemIndex < maximumNumberOfPeers && remoteSystemList[input.systemIndex].systemAddress == input)
		return remoteSystemList[input.systemIndex].guid;

	unsigned int i;
	for (i = 0; i < maximumNumberOfPeers; i++)
	{
		if (remoteSystemList[i].systemAddress == input)
		{
			// Set the systemIndex so future lookups will be fast
			remoteSystemList[i].guid.systemIndex = (SystemIndex) i;

			return remoteSystemList[i].guid;
		}
	}

	return UNASSIGNED_RAKNET_GUID;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

unsigned int RakPeer::GetSystemIndexFromGuid(const RakNetGUID input) const
{
	if (input == UNASSIGNED_RAKNET_GUID)
		return (unsigned int) -1;

	if (input == myGuid)
		return (unsigned int) -1;

	if (input.systemIndex != (SystemIndex) -1 && input.systemIndex < maximumNumberOfPeers && remoteSystemList[input.systemIndex].guid == input)
		return input.systemIndex;

	unsigned int i;
	for (i = 0; i < maximumNumberOfPeers; i++)
	{
		if (remoteSystemList[i].guid == input)
		{
			// Set the systemIndex so future lookups will be fast
			remoteSystemList[i].guid.systemIndex = (SystemIndex) i;

			return i;
		}
	}

	return (unsigned int) -1;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

SystemAddress RakPeer::GetSystemAddressFromGuid(const RakNetGUID input) const
{
	if (input == UNASSIGNED_RAKNET_GUID)
		return UNASSIGNED_SYSTEM_ADDRESS;

	if (input == myGuid)
		return GetInternalID(UNASSIGNED_SYSTEM_ADDRESS);

	if (input.systemIndex != (SystemIndex) -1 && input.systemIndex < maximumNumberOfPeers && remoteSystemList[input.systemIndex].guid == input)
		return remoteSystemList[input.systemIndex].systemAddress;

	unsigned int i;
	for (i = 0; i < maximumNumberOfPeers; i++)
	{
		if (remoteSystemList[i].guid == input)
		{
			// Set the systemIndex so future lookups will be fast
			remoteSystemList[i].guid.systemIndex = (SystemIndex) i;

			return remoteSystemList[i].systemAddress;
		}
	}

	return UNASSIGNED_SYSTEM_ADDRESS;
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Set the time, in MS, to use before considering ourselves disconnected after not being able to deliver a reliable packet
// \param[in] time Time, in MS
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetTimeoutTime(MafiaNet::TimeMS timeMS, const SystemAddress target)
{
	if (target == UNASSIGNED_SYSTEM_ADDRESS)
	{
		defaultTimeoutTime = timeMS;

		unsigned i;
		for (i = 0; i < maximumNumberOfPeers; i++)
		{
			if (remoteSystemList[i].isActive)
			{
				if (remoteSystemList[i].isActive)
					remoteSystemList[i].reliabilityLayer.SetTimeoutTime(timeMS);
			}
		}
	}
	else
	{
		RemoteSystemStruct *remoteSystem = GetRemoteSystemFromSystemAddress(target, false, true);

		if (remoteSystem != 0)
			remoteSystem->reliabilityLayer.SetTimeoutTime(timeMS);
	}
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

MafiaNet::TimeMS RakPeer::GetTimeoutTime(const SystemAddress target)
{
	if (target == UNASSIGNED_SYSTEM_ADDRESS)
	{
		return defaultTimeoutTime;
	}
	else
	{
		RemoteSystemStruct *remoteSystem = GetRemoteSystemFromSystemAddress(target, false, true);

		if (remoteSystem != 0)
			return remoteSystem->reliabilityLayer.GetTimeoutTime();
	}
	return defaultTimeoutTime;
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns the current MTU size
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetMTUSize(const SystemAddress target) const
{
	if (target != UNASSIGNED_SYSTEM_ADDRESS)
	{
		RemoteSystemStruct *rss = GetRemoteSystemFromSystemAddress(target, false, true);
		if (rss)
			return rss->MTUSize;
	}
	return defaultMTUSize;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns the number of IP addresses we have
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetNumberOfAddresses(void)
{
	if (IsActive() == false)
	{
		FillIPList();
	}

	unsigned int i = 0;
	while (i < MAXIMUM_NUMBER_OF_INTERNAL_IDS && ipList[i] != UNASSIGNED_SYSTEM_ADDRESS)
		i++; // count assigned entries until the first unassigned one (end of list)

	return i;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Returns an IP address at index 0 to GetNumberOfAddresses-1
// \param[in] index index into the list of IP addresses
// \return The local IP address at this index
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char *RakPeer::GetLocalIP(unsigned int index)
{
	if (IsActive() == false)
	{
		// Fill out ipList structure

		FillIPList();
	}


	static char str[128];
	ipList[index].ToString(false, str, static_cast<size_t>(128));
	return str;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Is this a local IP?
// \param[in] An IP address to check
// \return True if this is one of the IP addresses returned by GetLocalIP
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::IsLocalIP(const char *ip)
{
	if (ip == 0 || ip[0] == 0)
		return false;

	// #med - this should also check for "::1" here in IPv6 mode
	if (strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "localhost") == 0)
		return true;

	int num = GetNumberOfAddresses();
	int i;
	for (i = 0; i < num; i++)
	{
		if (strcmp(ip, GetLocalIP(i)) == 0)
			return true;
	}


	return false;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Allow or disallow connection responses from any IP. Normally this should be false, but may be necessary
// when connection to servers with multiple IP addresses
//
// Parameters:
// allow - True to allow this behavior, false to not allow.  Defaults to false.  Value persists between connections
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::AllowConnectionResponseIPMigration(bool allow)
{
	allowConnectionResponseIPMigration = allow;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Sends a message ID_ADVERTISE_SYSTEM to the remote unconnected system.
// This will tell the remote system our external IP outside the LAN, and can be used for NAT punch through
//
// Requires:
// The sender and recipient must already be started via a successful call to Initialize
//
// host: Either a dotted IP address or a domain name
// remotePort: Which port to connect to on the remote machine.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::AdvertiseSystem(const char *host, unsigned short remotePort, const char *data, int dataLength, unsigned connectionSocketIndex)
{
	MafiaNet::BitStream bs;
	bs.Write((MessageID) ID_ADVERTISE_SYSTEM);
	bs.WriteAlignedBytes((const unsigned char *) data, dataLength);
	return SendOutOfBand(host, remotePort, (const char *) bs.GetData(), bs.GetNumberOfBytesUsed(), connectionSocketIndex);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Controls how often to return ID_DOWNLOAD_PROGRESS for large message downloads.
// ID_DOWNLOAD_PROGRESS is returned to indicate a new partial message chunk, roughly the MTU size, has arrived
// As it can be slow or cumbersome to get this notification for every chunk, you can set the interval at which it is returned.
// Defaults to 0 (never return this notification)
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetSplitMessageProgressInterval(int interval)
{
	RakAssert(interval >= 0);
	splitMessageProgressInterval = interval;
	for (unsigned short i = 0; i < maximumNumberOfPeers; i++)
		remoteSystemList[i].reliabilityLayer.SetSplitMessageProgressInterval(splitMessageProgressInterval);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Returns what was passed to SetSplitMessageProgressInterval()
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetSplitMessageProgressInterval(void) const
{
	return splitMessageProgressInterval;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Set how long to wait before giving up on sending an unreliable message
// Useful if the network is clogged up.
// Set to 0 or less to never timeout.  Defaults to 0.
// timeoutMS How many ms to wait before simply not sending an unreliable message.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetUnreliableTimeout(MafiaNet::TimeMS timeoutMS)
{
	unreliableTimeout = timeoutMS;
	for (unsigned short i = 0; i < maximumNumberOfPeers; i++)
		remoteSystemList[i].reliabilityLayer.SetUnreliableTimeout(unreliableTimeout);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Send a message to host, with the IP socket option TTL set to 3
// This message will not reach the host, but will open the router.
// Used for NAT-Punchthrough
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SendTTL(const char *host, unsigned short remotePort, int ttl, unsigned connectionSocketIndex)
{
	char fakeData[2];
	fakeData[0] = 0;
	fakeData[1] = 1;
	unsigned int realIndex = GetRakNetSocketFromUserConnectionSocketIndex(connectionSocketIndex);
	if (socketList[realIndex]->IsBerkleySocket())
	{
		RNS2_SendParameters bsp;
		bsp.data = (char *) fakeData;
		bsp.length = 2;
		if (!ResolveOneShotTarget(host, remotePort, socketList[realIndex], &bsp.systemAddress))
			return;
		bsp.ttl = ttl;
		unsigned i;
		for (i = 0; i < pluginListNTS.Size(); i++)
			pluginListNTS[i]->OnDirectSocketSend((const char *) bsp.data, BYTES_TO_BITS(bsp.length), bsp.systemAddress);
		socketList[realIndex]->Send(&bsp, _FILE_AND_LINE_);
	}
}


// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Attatches a Plugin interface to run code automatically on message receipt in the Receive call
//
// \param messageHandler Pointer to a plugin to attach
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::AttachPlugin(PluginInterface2 *plugin)
{
	if (IsInsideNetworkThreadReceive())
		return;

	bool isNotThreadsafe = plugin->UsesReliabilityLayer();
	if (isNotThreadsafe)
	{
		if (pluginListNTS.GetIndexOf(plugin) == MAX_UNSIGNED_LONG)
		{
			plugin->SetRakPeerInterface(this);
			plugin->OnAttach();
			pluginListNTS.Insert(plugin, _FILE_AND_LINE_);
		}
	}
	else
	{
		if (pluginListTS.GetIndexOf(plugin) == MAX_UNSIGNED_LONG)
		{
			plugin->SetRakPeerInterface(this);
			plugin->OnAttach();
			pluginListTS.Insert(plugin, _FILE_AND_LINE_);
		}
	}

	pluginListNetworkThreadMutex.Lock();
	if (pluginListNetworkThread.GetIndexOf(plugin) == MAX_UNSIGNED_LONG)
		pluginListNetworkThread.Insert(plugin, _FILE_AND_LINE_);
	pluginListNetworkThreadMutex.Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Detaches a Plugin interface to run code automatically on message receipt
//
// \param messageHandler Pointer to a plugin to detach
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::DetachPlugin(PluginInterface2 *plugin)
{
	if (plugin == 0 || IsInsideNetworkThreadReceive())
		return;

	unsigned int index;

	bool isNotThreadsafe = plugin->UsesReliabilityLayer();
	if (isNotThreadsafe)
	{
		index = pluginListNTS.GetIndexOf(plugin);
		if (index != MAX_UNSIGNED_LONG)
		{
			// Unordered list so delete from end for speed
			pluginListNTS[index] = pluginListNTS[pluginListNTS.Size() - 1];
			pluginListNTS.RemoveFromEnd();
		}
	}
	else
	{
		index = pluginListTS.GetIndexOf(plugin);
		if (index != MAX_UNSIGNED_LONG)
		{
			// Unordered list so delete from end for speed
			pluginListTS[index] = pluginListTS[pluginListTS.Size() - 1];
			pluginListTS.RemoveFromEnd();
		}
	}
	// Removed under the lock the update thread dispatches under, so once this returns the plugin
	// is not inside OnReceiveNetworkThread and will not be called again.
	pluginListNetworkThreadMutex.Lock();
	index = pluginListNetworkThread.GetIndexOf(plugin);
	if (index != MAX_UNSIGNED_LONG)
	{
		pluginListNetworkThread[index] = pluginListNetworkThread[pluginListNetworkThread.Size() - 1];
		pluginListNetworkThread.RemoveFromEnd();
	}
	pluginListNetworkThreadMutex.Unlock();

	plugin->OnDetach();
	plugin->SetRakPeerInterface(0);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Put a packet back at the end of the receive queue in case you don't want to deal with it immediately
//
// packet The packet you want to push back.
// pushAtHead True to push the packet so that the next receive call returns it.  False to push it at the end of the queue (obviously pushing it at the end makes the packets out of order)
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::PushBackPacket(Packet *packet, bool pushAtHead)
{
	if (packet == 0)
		return;

	unsigned i;
	for (i = 0; i < pluginListTS.Size(); i++)
		pluginListTS[i]->OnPushBackPacket((const char *) packet->data, packet->bitSize, packet->systemAddress);
	for (i = 0; i < pluginListNTS.Size(); i++)
		pluginListNTS[i]->OnPushBackPacket((const char *) packet->data, packet->bitSize, packet->systemAddress);

	packetReturnMutex.Lock();
	if (pushAtHead)
		packetReturnQueue.PushAtHead(packet, 0, _FILE_AND_LINE_);
	else
		packetReturnQueue.Push(packet, _FILE_AND_LINE_);
	packetReturnCondition.notify_all();
	packetReturnMutex.Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ChangeSystemAddress(RakNetGUID guid, const SystemAddress &systemAddress)
{
	BufferedCommandStruct *bcs;

	bcs = bufferedCommands.Allocate(_FILE_AND_LINE_);
	bcs->data = 0;
	bcs->systemIdentifier.systemAddress = systemAddress;
	bcs->systemIdentifier.rakNetGuid = guid;
	bcs->command = BufferedCommandStruct::BCS_CHANGE_SYSTEM_ADDRESS;
	bufferedCommands.Push(bcs);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
Packet *RakPeer::AllocatePacket(unsigned dataSize)
{
	return AllocPacket(dataSize, _FILE_AND_LINE_);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakNetSocket2 *RakPeer::GetSocket(const SystemAddress target)
{
	// Send a query to the thread to get the socket, and return when we got it
	BufferedCommandStruct *bcs;
	bcs = bufferedCommands.Allocate(_FILE_AND_LINE_);
	bcs->command = BufferedCommandStruct::BCS_GET_SOCKET;
	bcs->systemIdentifier = target;
	bcs->data = 0;
	bufferedCommands.Push(bcs);

	// Block up to one second to get the socket, although it should actually take virtually no time
	SocketQueryOutput *sqo;
	MafiaNet::TimeMS stopWaiting = MafiaNet::GetTimeMS() + 1000;
	DataStructures::List<RakNetSocket2 *> output;
	while (manualUpdate || MafiaNet::GetTimeMS() < stopWaiting)
	{
		if (manualUpdate)
			RunManualUpdate(); // answers the query on this thread
		else if (isMainLoopThreadActive == false)
			return 0;
		else
			RakSleep(0);

		sqo = socketQueryOutput.Pop();
		if (sqo)
		{
			output = sqo->sockets;
			sqo->sockets.Clear(false, _FILE_AND_LINE_);
			socketQueryOutput.Deallocate(sqo, _FILE_AND_LINE_);
			if (output.Size())
				return output[0];
			break;
		}
	}
	return 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetSockets(DataStructures::List<RakNetSocket2 *> &sockets)
{
	sockets.Clear(false, _FILE_AND_LINE_);

	// Send a query to the thread to get the socket, and return when we got it
	BufferedCommandStruct *bcs;

	bcs = bufferedCommands.Allocate(_FILE_AND_LINE_);
	bcs->command = BufferedCommandStruct::BCS_GET_SOCKET;
	bcs->systemIdentifier = UNASSIGNED_SYSTEM_ADDRESS;
	bcs->data = 0;
	bufferedCommands.Push(bcs);

	// Block up to one second to get the socket, although it should actually take virtually no time
	SocketQueryOutput *sqo;
	//	RakNetSocket2* output;
	for (;;)
	{
		if (manualUpdate)
			RunManualUpdate(); // answers the query on this thread
		else if (isMainLoopThreadActive == false)
			return;
		else
			RakSleep(0);

		sqo = socketQueryOutput.Pop();
		if (sqo)
		{
			sockets = sqo->sockets;
			sqo->sockets.Clear(false, _FILE_AND_LINE_);
			socketQueryOutput.Deallocate(sqo, _FILE_AND_LINE_);
			return;
		}
	}
	return;
}
void RakPeer::ReleaseSockets(DataStructures::List<RakNetSocket2 *> &sockets)
{
	sockets.Clear(false, _FILE_AND_LINE_);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Adds simulated ping and packet loss to the outgoing data flow.
// To simulate bi-directional ping and packet loss, you should call this on both the sender and the recipient, with half the total ping and maxSendBPS value on each.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ApplyNetworkSimulator(float packetloss, unsigned short minExtraPing, unsigned short extraPingVariance)
{
#ifndef _DEBUG
	// unused parameters
	(void) packetloss;
	(void) minExtraPing;
	(void) extraPingVariance;
#endif

#ifdef _DEBUG
	if (remoteSystemList)
	{
		unsigned short i;
		for (i = 0; i < maximumNumberOfPeers; i++)
			//for (i=0; i < remoteSystemListSize; i++)
			remoteSystemList[i].reliabilityLayer.ApplyNetworkSimulator(packetloss, minExtraPing, extraPingVariance);
	}

	_packetloss = packetloss;
	_minExtraPing = minExtraPing;
	_extraPingVariance = extraPingVariance;
#endif
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void RakPeer::SetPerConnectionOutgoingBandwidthLimit(unsigned maxBitsPerSecond)
{
	maxOutgoingBPS = maxBitsPerSecond;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Returns if you previously called ApplyNetworkSimulator
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::IsNetworkSimulatorActive(void)
{
#ifdef _DEBUG
	return _packetloss > 0 || _minExtraPing > 0 || _extraPingVariance > 0;
#else
	return false;
#endif
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::WriteOutOfBandHeader(MafiaNet::BitStream *bitStream)
{
	bitStream->Write((MessageID) ID_OUT_OF_BAND_INTERNAL);
	bitStream->Write(myGuid);
	bitStream->WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetUserUpdateThread(void (*_userUpdateThreadPtr)(RakPeerInterface *, void *), void *_userUpdateThreadData)
{
	userUpdateThreadPtr = _userUpdateThreadPtr;
	userUpdateThreadData = _userUpdateThreadData;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetSocketFactory(RakNetSocket2Factory *factory)
{
	RakAssert(IsActive() == false);
	socketFactory = factory;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetManualUpdate(bool manual)
{
	RakAssert(IsActive() == false);
	manualUpdate = manual;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::RunManualUpdate()
{
	RakAssert(manualUpdate);
	// The same steps as one pass of UpdateNetworkLoop.
	if (userUpdateThreadPtr)
		userUpdateThreadPtr(this, userUpdateThreadData);
	BitStream updateBitStream(MAXIMUM_MTU_SIZE);
	RunUpdateCycle(updateBitStream);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetIncomingDatagramEventHandler(bool (*_incomingDatagramEventHandler)(RNS2RecvStruct *))
{
	incomingDatagramEventHandler = _incomingDatagramEventHandler;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::SendOutOfBand(const char *host, unsigned short remotePort, const char *data, BitSize_t dataLength, unsigned connectionSocketIndex)
{
	if (IsActive() == false)
		return false;

	if (host == 0 || host[0] == 0)
		return false;

	// If this assert hits then Startup wasn't called or the call failed.
	RakAssert(connectionSocketIndex < socketList.Size());

	// This is a security measure.  Don't send data longer than this value
	RakAssert(dataLength <= (MAX_OFFLINE_DATA_LENGTH + sizeof(unsigned char) + sizeof(MafiaNet::Time) + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID)));

	if (host == 0)
		return false;

	// 34 bytes
	MafiaNet::BitStream bitStream;
	WriteOutOfBandHeader(&bitStream);

	if (dataLength > 0)
	{
		bitStream.Write(data, dataLength);
	}

	unsigned int realIndex = GetRakNetSocketFromUserConnectionSocketIndex(connectionSocketIndex);

	/*
	SystemAddress systemAddress;
	systemAddress.FromStringExplicitPort(host,remotePort, socketList[realIndex]->GetBoundAddress().GetIPVersion());
	systemAddress.FixForIPVersion(socketList[realIndex]->GetBoundAddress());

	unsigned i;
	for (i=0; i < pluginListNTS.Size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char*)bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), systemAddress);
	
	SocketLayer::SendTo( socketList[realIndex], (const char*)bitStream.GetData(), (int) bitStream.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );
	*/

	RNS2_SendParameters bsp;
	bsp.data = (char *) bitStream.GetData();
	bsp.length = bitStream.GetNumberOfBytesUsed();
	if (!ResolveOneShotTarget(host, remotePort, socketList[realIndex], &bsp.systemAddress))
		return false;
	unsigned i;
	for (i = 0; i < pluginListNTS.Size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char *) bsp.data, BYTES_TO_BITS(bsp.length), bsp.systemAddress);
	socketList[realIndex]->Send(&bsp, _FILE_AND_LINE_);

	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakNetStatistics *RakPeer::GetStatistics(const SystemAddress systemAddress, RakNetStatistics *rns)
{
	static RakNetStatistics staticStatistics;
	RakNetStatistics *systemStats;
	if (rns == 0)
		systemStats = &staticStatistics;
	else
		systemStats = rns;

	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
	{
		bool firstWrite = false;
		// Return a crude sum
		for (unsigned short i = 0; i < maximumNumberOfPeers; i++)
		{
			if (remoteSystemList[i].isActive)
			{
				RakNetStatistics rnsTemp;
				remoteSystemList[i].reliabilityLayer.GetStatistics(&rnsTemp);

				if (firstWrite == false)
				{
					memcpy(systemStats, &rnsTemp, sizeof(RakNetStatistics));
					firstWrite = true;
				}
				else
					(*systemStats) += rnsTemp;
			}
		}
		return systemStats;
	}
	else
	{
		RemoteSystemStruct *rss;
		rss = GetRemoteSystemFromSystemAddress(systemAddress, false, false);
		if (rss && endThreads == false)
		{
			rss->reliabilityLayer.GetStatistics(systemStats);
			return systemStats;
		}
	}

	return 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetStatisticsList(DataStructures::List<SystemAddress> &addresses, DataStructures::List<RakNetGUID> &guids, DataStructures::List<RakNetStatistics> &statistics)
{
	addresses.Clear(false, _FILE_AND_LINE_);
	guids.Clear(false, _FILE_AND_LINE_);
	statistics.Clear(false, _FILE_AND_LINE_);

	if (remoteSystemList == 0 || endThreads == true)
		return;

	unsigned int i;
	for (i = 0; i < activeSystemListSize; i++)
	{
		if ((activeSystemList[i])->isActive &&
		    (activeSystemList[i])->connectMode == RakPeer::RemoteSystemStruct::CONNECTED)
		{
			addresses.Push((activeSystemList[i])->systemAddress, _FILE_AND_LINE_);
			guids.Push((activeSystemList[i])->guid, _FILE_AND_LINE_);
			RakNetStatistics rns;
			(activeSystemList[i])->reliabilityLayer.GetStatistics(&rns);
			statistics.Push(rns, _FILE_AND_LINE_);
		}
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::GetStatistics(const unsigned int index, RakNetStatistics *rns)
{
	if (index < maximumNumberOfPeers && remoteSystemList[index].isActive)
	{
		remoteSystemList[index].reliabilityLayer.GetStatistics(rns);
		return true;
	}
	return false;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::WaitForPackets(MafiaNet::TimeMS timeoutMs)
{
	// std::condition_variable_any needs lock()/unlock(); SimpleMutex spells them Lock()/Unlock().
	struct PacketReturnLock
	{
		SimpleMutex &mutex;
		void lock() { mutex.Lock(); }
		void unlock() { mutex.Unlock(); }
	} lock{packetReturnMutex};

	lock.lock();
	packetReturnCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]
	                               { return packetReturnQueue.IsEmpty() == false || IsActive() == false; });
	const bool packetsWaiting = packetReturnQueue.IsEmpty() == false;
	lock.unlock();
	return packetsWaiting;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetReceiveBufferSize(void)
{
	unsigned int size;
	packetReturnMutex.Lock();
	size = packetReturnQueue.Size();
	packetReturnMutex.Unlock();
	return size;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetIndexFromSystemAddress(const SystemAddress systemAddress, bool calledFromNetworkThread) const
{
	unsigned i;

	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
		return -1;

	if (systemAddress.systemIndex != (SystemIndex) -1 && systemAddress.systemIndex < maximumNumberOfPeers && remoteSystemList[systemAddress.systemIndex].systemAddress == systemAddress && remoteSystemList[systemAddress.systemIndex].isActive)
		return systemAddress.systemIndex;

	if (calledFromNetworkThread)
	{
		return GetRemoteSystemIndex(systemAddress);
	}
	else
	{
		// remoteSystemList in user and network thread
		for (i = 0; i < maximumNumberOfPeers; i++)
			if (remoteSystemList[i].isActive && remoteSystemList[i].systemAddress == systemAddress)
				return i;

		// If no active results found, try previously active results.
		for (i = 0; i < maximumNumberOfPeers; i++)
			if (remoteSystemList[i].systemAddress == systemAddress)
				return i;
	}

	return -1;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int RakPeer::GetIndexFromGuid(const RakNetGUID guid)
{
	unsigned i;

	if (guid == UNASSIGNED_RAKNET_GUID)
		return -1;

	if (guid.systemIndex != (SystemIndex) -1 && guid.systemIndex < maximumNumberOfPeers && remoteSystemList[guid.systemIndex].guid == guid && remoteSystemList[guid.systemIndex].isActive)
		return guid.systemIndex;

	// remoteSystemList in user and network thread
	for (i = 0; i < maximumNumberOfPeers; i++)
		if (remoteSystemList[i].isActive && remoteSystemList[i].guid == guid)
			return i;

	// If no active results found, try previously active results.
	for (i = 0; i < maximumNumberOfPeers; i++)
		if (remoteSystemList[i].guid == guid)
			return i;

	return -1;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
ConnectionAttemptResult RakPeer::SendConnectionRequest(const char *host, unsigned short remotePort, const char *passwordData, int passwordDataLength, unsigned connectionSocketIndex, unsigned int extraData, unsigned sendConnectionAttemptCount, unsigned timeBetweenSendConnectionAttemptsMS, MafiaNet::TimeMS timeoutTime)
{
	RakAssert(passwordDataLength <= 256);
	RakAssert(remotePort != 0);

	// A dual-stack socket resolves the host itself, in both families. More than
	// one answer starts a Happy Eyeballs race (RFC 8305): one request per
	// candidate, all sharing a raceGroup. Otherwise this is a single request,
	// exactly as on a single-family socket.
	static const int MAX_CONNECT_CANDIDATES = 16;
	SystemAddress candidates[MAX_CONNECT_CANDIDATES];
	int candidateCount;
	RakNetSocket2 *socket = socketList[connectionSocketIndex];
	if (socket->IsDualStack())
	{
		candidateCount = OrderConnectCandidates(candidates, socket->Resolve(host, remotePort, candidates, MAX_CONNECT_CANDIDATES));
		if (candidateCount <= 0)
			return CANNOT_RESOLVE_DOMAIN_NAME;
	}
	else
	{
		if (!candidates[0].FromStringExplicitPort(host, remotePort, ResolveIPVersion(socket)))
			return CANNOT_RESOLVE_DOMAIN_NAME;
		candidateCount = 1;
	}

	// Already connected?
	for (int c = 0; c < candidateCount; c++)
	{
		if (GetRemoteSystemFromSystemAddress(candidates[c], false, true))
			return ALREADY_CONNECTED_TO_ENDPOINT;
	}

	const MafiaNet::TimeMS now = MafiaNet::GetTimeMS();
	RequestedConnectionStruct *requests[MAX_CONNECT_CANDIDATES];
	for (int c = 0; c < candidateCount; c++)
	{
		//RequestedConnectionStruct *rcs = (RequestedConnectionStruct *) rakMalloc_Ex(sizeof(RequestedConnectionStruct), _FILE_AND_LINE_);
		RequestedConnectionStruct *rcs = MafiaNet::OP_NEW<RequestedConnectionStruct>(_FILE_AND_LINE_);

		rcs->systemAddress = candidates[c];
		rcs->nextRequestTime = now;
		rcs->requestsMade = 0;
		rcs->data = 0;
		rcs->socket = 0;
		rcs->extraData = extraData;
		rcs->socketIndex = connectionSocketIndex;
		rcs->actionToTake = RequestedConnectionStruct::CONNECT;
		rcs->sendConnectionAttemptCount = sendConnectionAttemptCount;
		rcs->timeBetweenSendConnectionAttemptsMS = timeBetweenSendConnectionAttemptsMS;
		// No password means passwordData is null. memcpy's source is declared nonnull,
		// so a null source is undefined behaviour even with a length of 0.
		if (passwordDataLength > 0)
			memcpy(rcs->outgoingPassword, passwordData, passwordDataLength);
		rcs->outgoingPasswordLength = (unsigned char) passwordDataLength;
		rcs->timeoutTime = timeoutTime;
		if (candidateCount > 1)
		{
			// raceGroup is assigned under the queue lock below.
			rcs->raceFirstAddress = candidates[0];
			rcs->nextCandidateTime = (MafiaNet::Time) now + (MafiaNet::Time) c * HAPPY_EYEBALLS_ATTEMPT_DELAY_MS;
		}
		requests[c] = rcs;
	}

	// Return false if already pending, else push on queue
	requestedConnectionQueueMutex.Lock();
	for (unsigned int i = 0; i < requestedConnectionQueue.Size(); i++)
	{
		for (int c = 0; c < candidateCount; c++)
		{
			if (RequestMatches(requestedConnectionQueue[i], candidates[c]))
			{
				requestedConnectionQueueMutex.Unlock();
				for (int k = 0; k < candidateCount; k++)
					DeleteRequestedConnection(requests[k]);
				return CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS;
			}
		}
	}
	// Checked again under the queue lock. The network thread answers a REQUEST_2 under this lock too, so a
	// connection it allocates for one of these addresses is either seen here or finds this request queued; the
	// cross-connection tie-break (the REQUEST_2 handler) relies on that.
	for (int c = 0; c < candidateCount; c++)
	{
		if (GetRemoteSystemFromSystemAddress(candidates[c], false, true))
		{
			requestedConnectionQueueMutex.Unlock();
			for (int k = 0; k < candidateCount; k++)
				DeleteRequestedConnection(requests[k]);
			return ALREADY_CONNECTED_TO_ENDPOINT;
		}
	}
	if (candidateCount > 1)
	{
		const uint32_t raceGroup = nextRaceGroup++;
		if (nextRaceGroup == 0)
			nextRaceGroup = 1;
		for (int c = 0; c < candidateCount; c++)
			requests[c]->raceGroup = raceGroup;
	}
	// One ephemeral key pair per candidate: each is a separate handshake with its own transcript.
	for (int c = 0; c < candidateCount; c++)
	{
		if (encryptionMode == EncryptionMode::Required)
		{
			requests[c]->ephemeral = MafiaNet::OP_NEW<EphemeralKeyPair>(_FILE_AND_LINE_);
			requests[c]->handshakeNonce = requests[c]->ephemeral->MakeCryptInfo().nonce;
		}
		else
			randombytes_buf(&requests[c]->handshakeNonce, sizeof(requests[c]->handshakeNonce)); // echoed by REPLY_2, so unpredictable here too
		requestedConnectionQueue.Push(requests[c], _FILE_AND_LINE_);
	}
	requestedConnectionQueueMutex.Unlock();

	return CONNECTION_ATTEMPT_STARTED;
}
ConnectionAttemptResult RakPeer::SendConnectionRequest(const char *host, unsigned short remotePort, const char *passwordData, int passwordDataLength, unsigned connectionSocketIndex, unsigned int extraData, unsigned sendConnectionAttemptCount, unsigned timeBetweenSendConnectionAttemptsMS, MafiaNet::TimeMS timeoutTime, RakNetSocket2 *socket)
{
	RakAssert(passwordDataLength <= 256);
	SystemAddress systemAddress;
	// Resolve in the family this socket can reach, as Connect() does.
	systemAddress.FromStringExplicitPort(host, remotePort, ResolveIPVersion(socket));

	// Already connected?
	if (GetRemoteSystemFromSystemAddress(systemAddress, false, true))
		return ALREADY_CONNECTED_TO_ENDPOINT;

	//RequestedConnectionStruct *rcs = (RequestedConnectionStruct *) rakMalloc_Ex(sizeof(RequestedConnectionStruct), _FILE_AND_LINE_);
	RequestedConnectionStruct *rcs = MafiaNet::OP_NEW<RequestedConnectionStruct>(_FILE_AND_LINE_);

	rcs->systemAddress = systemAddress;
	rcs->nextRequestTime = MafiaNet::GetTimeMS();
	rcs->requestsMade = 0;
	rcs->data = 0;
	rcs->socket = 0;
	rcs->extraData = extraData;
	rcs->socketIndex = connectionSocketIndex;
	rcs->actionToTake = RequestedConnectionStruct::CONNECT;
	rcs->sendConnectionAttemptCount = sendConnectionAttemptCount;
	rcs->timeBetweenSendConnectionAttemptsMS = timeBetweenSendConnectionAttemptsMS;
	// No password means passwordData is null. memcpy's source is declared nonnull,
	// so a null source is undefined behaviour even with a length of 0.
	if (passwordDataLength > 0)
		memcpy(rcs->outgoingPassword, passwordData, passwordDataLength);
	rcs->outgoingPasswordLength = (unsigned char) passwordDataLength;
	rcs->timeoutTime = timeoutTime;
	rcs->socket = socket;

	// Return false if already pending, else push on queue
	unsigned int i = 0;
	requestedConnectionQueueMutex.Lock();
	for (; i < requestedConnectionQueue.Size(); i++)
	{
		if (RequestMatches(requestedConnectionQueue[i], systemAddress))
		{
			requestedConnectionQueueMutex.Unlock();
			DeleteRequestedConnection(rcs);
			return CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS;
		}
	}
	// Checked again under the queue lock, as in the other overload (cross-connection tie-break).
	if (GetRemoteSystemFromSystemAddress(systemAddress, false, true))
	{
		requestedConnectionQueueMutex.Unlock();
		DeleteRequestedConnection(rcs);
		return ALREADY_CONNECTED_TO_ENDPOINT;
	}
	if (encryptionMode == EncryptionMode::Required)
	{
		rcs->ephemeral = MafiaNet::OP_NEW<EphemeralKeyPair>(_FILE_AND_LINE_);
		rcs->handshakeNonce = rcs->ephemeral->MakeCryptInfo().nonce;
	}
	else
		randombytes_buf(&rcs->handshakeNonce, sizeof(rcs->handshakeNonce)); // echoed by REPLY_2, so unpredictable here too
	requestedConnectionQueue.Push(rcs, _FILE_AND_LINE_);
	requestedConnectionQueueMutex.Unlock();

	return CONNECTION_ATTEMPT_STARTED;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ValidateRemoteSystemLookup(void) const
{
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakPeer::RemoteSystemStruct *RakPeer::GetRemoteSystem(const AddressOrGUID systemIdentifier, bool calledFromNetworkThread, bool onlyActive) const
{
	if (systemIdentifier.rakNetGuid != UNASSIGNED_RAKNET_GUID)
		return GetRemoteSystemFromGUID(systemIdentifier.rakNetGuid, onlyActive);
	else
		return GetRemoteSystemFromSystemAddress(systemIdentifier.systemAddress, calledFromNetworkThread, onlyActive);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakPeer::RemoteSystemStruct *RakPeer::GetRemoteSystemFromSystemAddress(const SystemAddress systemAddress, bool calledFromNetworkThread, bool onlyActive) const
{
	unsigned i;

	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
		return 0;

	if (calledFromNetworkThread)
	{
		unsigned int index = GetRemoteSystemIndex(systemAddress);
		if (index != (unsigned int) -1)
		{
			if (onlyActive == false || remoteSystemList[index].isActive == true)
			{
				RakAssert(remoteSystemList[index].systemAddress == systemAddress);
				return remoteSystemList + index;
			}
		}
	}
	else
	{
		int deadConnectionIndex = -1;

		// Active connections take priority.  But if there are no active connections, return the first systemAddress match found
		for (i = 0; i < maximumNumberOfPeers; i++)
		{
			if (remoteSystemList[i].systemAddress == systemAddress)
			{
				if (remoteSystemList[i].isActive)
					return remoteSystemList + i;
				else if (deadConnectionIndex == -1)
					deadConnectionIndex = i;
			}
		}

		if (deadConnectionIndex != -1 && onlyActive == false)
			return remoteSystemList + deadConnectionIndex;
	}

	return 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakPeer::RemoteSystemStruct *RakPeer::GetRemoteSystemFromGUID(const RakNetGUID guid, bool onlyActive) const
{
	if (guid == UNASSIGNED_RAKNET_GUID)
		return 0;

	unsigned i;
	for (i = 0; i < maximumNumberOfPeers; i++)
	{
		if (remoteSystemList[i].guid == guid && (onlyActive == false || remoteSystemList[i].isActive))
		{
			return remoteSystemList + i;
		}
	}
	return 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ParseConnectionRequestPacket(RakPeer::RemoteSystemStruct *remoteSystem, const SystemAddress &systemAddress, const char *data, int byteSize)
{
	MafiaNet::BitStream bs((unsigned char *) data, byteSize, false);
	bs.IgnoreBytes(sizeof(MessageID));
	RakNetGUID guid;
	bs.Read(guid);
	MafiaNet::Time incomingTimestamp;
	bs.Read(incomingTimestamp);
	// ID_CONNECTION_REQUEST: id | guid | timestamp | reserved byte (always 0; was libcat's HasSecurity) | password.
	// It travels inside the session cipher already, so a peer that derived different keys never gets here.
	unsigned char reserved;
	bs.Read(reserved);
	(void) reserved;

	unsigned char *password = bs.GetData() + BITS_TO_BYTES(bs.GetReadOffset());
	int passwordLength = byteSize - BITS_TO_BYTES(bs.GetReadOffset());
	if (incomingPasswordLength != passwordLength ||
	    memcmp(password, incomingPassword, incomingPasswordLength) != 0)
	{
		// This one we only send once since we don't care if it arrives.
		MafiaNet::BitStream bitStream;
		bitStream.Write((MessageID) ID_INVALID_PASSWORD);
		bitStream.Write(GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));
		SendImmediate((char *) bitStream.GetData(), bitStream.GetNumberOfBytesUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::Reliable, 0, systemAddress, false, false, MafiaNet::GetTimeUS(), 0);
		remoteSystem->connectMode = RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY;
		return;
	}

	// OK
	remoteSystem->connectMode = RemoteSystemStruct::HANDLING_CONNECTION_REQUEST;

	OnConnectionRequest(remoteSystem, incomingTimestamp);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::OnConnectionRequest(RakPeer::RemoteSystemStruct *remoteSystem, MafiaNet::Time incomingTimestamp)
{
	MafiaNet::BitStream bitStream;
	bitStream.Write((MessageID) ID_CONNECTION_REQUEST_ACCEPTED);
	bitStream.Write(remoteSystem->systemAddress);
	SystemIndex systemIndex = (SystemIndex) GetIndexFromSystemAddress(remoteSystem->systemAddress, true);
	RakAssert(systemIndex != 65535);
	bitStream.Write(systemIndex);
	for (unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++)
		bitStream.Write(ipList[i]);
	bitStream.Write(incomingTimestamp);
	bitStream.Write(MafiaNet::GetTime());

	SendImmediate((char *) bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, remoteSystem->systemAddress, false, false, MafiaNet::GetTimeUS(), 0);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::NotifyAndFlagForShutdown(const SystemAddress systemAddress, bool performImmediate, unsigned char orderingChannel, MafiaNet::Priority disconnectionNotificationPriority, const MafiaNet::BitStream *reasonData)
{
	MafiaNet::BitStream temp(sizeof(unsigned char));
	temp.Write((MessageID) ID_DISCONNECTION_NOTIFICATION);
	// Optionally append a caller-supplied reason payload right after the 1-byte ID. The ID occupies exactly 8 bits so
	// the payload stays byte-aligned; the remote reads it from packet->data+1 (length packet->length-1). Wire-backward
	// compatible: peers that only inspect data[0] ignore the extra bytes.
	if (reasonData != nullptr && reasonData->GetNumberOfBytesUsed() > 0)
		temp.Write((const char *) reasonData->GetData(), reasonData->GetNumberOfBytesUsed());
	if (performImmediate)
	{
		SendImmediate((char *) temp.GetData(), temp.GetNumberOfBitsUsed(), disconnectionNotificationPriority, MafiaNet::Reliability::ReliableOrdered, orderingChannel, systemAddress, false, false, MafiaNet::GetTimeUS(), 0);
		RemoteSystemStruct *rss = GetRemoteSystemFromSystemAddress(systemAddress, true, true);
		rss->connectMode = RemoteSystemStruct::DISCONNECT_ASAP;
	}
	else
	{
		SendBuffered((const char *) temp.GetData(), temp.GetNumberOfBitsUsed(), disconnectionNotificationPriority, MafiaNet::Reliability::ReliableOrdered, orderingChannel, systemAddress, false, RemoteSystemStruct::DISCONNECT_ASAP, 0);
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearDisconnectReason(RemoteSystemStruct *remoteSystem)
{
	if (remoteSystem == 0)
		return;
	if (remoteSystem->disconnectReasonData != 0)
	{
		rakFree_Ex(remoteSystem->disconnectReasonData, _FILE_AND_LINE_);
		remoteSystem->disconnectReasonData = 0;
	}
	remoteSystem->disconnectReasonLength = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearSessionConfig(RemoteSystemStruct *remoteSystem)
{
	if (remoteSystem == 0)
		return;
	if (remoteSystem->remoteSessionConfigData != 0)
	{
		rakFree_Ex(remoteSystem->remoteSessionConfigData, _FILE_AND_LINE_);
		remoteSystem->remoteSessionConfigData = 0;
	}
	remoteSystem->remoteSessionConfigLength = 0;
	if (remoteSystem->withheldConnectionPacketData != 0)
	{
		rakFree_Ex(remoteSystem->withheldConnectionPacketData, _FILE_AND_LINE_);
		remoteSystem->withheldConnectionPacketData = 0;
	}
	remoteSystem->withheldConnectionPacketLength = 0;
	remoteSystem->sessionConfigAwaitingLocalDecision = false;
	remoteSystem->sessionConfigIsConnectingSide = false;
	remoteSystem->connectionReportedToApplication = false;
	remoteSystem->sessionConfigStartTime = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::StoreRemoteSessionConfig(RemoteSystemStruct *remoteSystem, const unsigned char *data, unsigned int length)
{
	if (remoteSystem == 0)
		return false;

	// An oversized payload is a protocol violation, not something to truncate: the sender enforces the
	// same cap in SetSessionConfig, so anything larger did not come from a well-behaved peer.
	if (length > MAXIMUM_SESSION_CONFIG_SIZE)
		return false;

	if (remoteSystem->remoteSessionConfigData != 0)
	{
		rakFree_Ex(remoteSystem->remoteSessionConfigData, _FILE_AND_LINE_);
		remoteSystem->remoteSessionConfigData = 0;
	}
	remoteSystem->remoteSessionConfigLength = 0;

	if (data == 0 || length == 0)
		return true;

	// One extra byte, always zeroed and never counted in the reported length. The payload is
	// attacker-controlled and arbitrary bytes, so an application that reaches for strlen/printf on it
	// would otherwise run off the end of the buffer. Length remains the authoritative bound.
	remoteSystem->remoteSessionConfigData = (unsigned char *) rakMalloc_Ex(length + 1, _FILE_AND_LINE_);
	if (remoteSystem->remoteSessionConfigData == 0)
		return false;

	memcpy(remoteSystem->remoteSessionConfigData, data, length);
	remoteSystem->remoteSessionConfigData[length] = 0;
	remoteSystem->remoteSessionConfigLength = length;
	return true;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Replay the connection packet that was held back while the session handshake ran. This is the single
// point at which an application learns a connection exists, and by construction it runs only once the
// remote's session payload is already stored on the slot.
void RakPeer::ProduceWithheldConnectionPacket(RemoteSystemStruct *remoteSystem, MessageID messageId)
{
	if (remoteSystem == 0)
		return;

	Packet *packet;
	if (remoteSystem->withheldConnectionPacketData != 0 && remoteSystem->withheldConnectionPacketLength > 0)
	{
		packet = AllocPacket(remoteSystem->withheldConnectionPacketLength, _FILE_AND_LINE_);
		memcpy(packet->data, remoteSystem->withheldConnectionPacketData, remoteSystem->withheldConnectionPacketLength);
		packet->bitSize = remoteSystem->withheldConnectionPacketLength * 8;
		rakFree_Ex(remoteSystem->withheldConnectionPacketData, _FILE_AND_LINE_);
		remoteSystem->withheldConnectionPacketData = 0;
		remoteSystem->withheldConnectionPacketLength = 0;
	}
	else
	{
		// The original bytes could not be stashed (allocation failure); the message id alone still tells
		// the application a connection exists, which is what every caller keys off.
		packet = AllocPacket(sizeof(MessageID), _FILE_AND_LINE_);
		packet->data[0] = messageId;
		packet->bitSize = sizeof(MessageID) * 8;
	}

	packet->systemAddress = remoteSystem->systemAddress;
	packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
	packet->guid = remoteSystem->guid;
	packet->guid.systemIndex = packet->systemAddress.systemIndex;
	remoteSystem->connectionReportedToApplication = true;
	AddPacketToProducer(packet);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ProduceSessionAbandoned(RemoteSystemStruct *remoteSystem)
{
	if (remoteSystem == 0 || remoteSystem->weInitiatedTheConnection || remoteSystem->sessionConfigAwaitingLocalDecision == false)
		return;

	// Once only: the flag is what made the request the application's to answer.
	remoteSystem->sessionConfigAwaitingLocalDecision = false;

	Packet *packet = AllocPacket(sizeof(MessageID), _FILE_AND_LINE_);
	packet->data[0] = (unsigned char) ID_SESSION_CONFIG_ABANDONED;
	packet->bitSize = sizeof(MessageID) * 8;
	packet->systemAddress = remoteSystem->systemAddress;
	packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
	packet->guid = remoteSystem->guid;
	packet->guid.systemIndex = packet->systemAddress.systemIndex;
	AddPacketToProducer(packet);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Server side of the session handshake: answer the client's request, promote the connection to
// CONNECTED and release the ID_NEW_INCOMING_CONNECTION that was withheld.
void RakPeer::SendSessionConfigResponse(RemoteSystemStruct *remoteSystem, const char *data, unsigned int length)
{
	if (remoteSystem == 0)
		return;

	MafiaNet::BitStream bitStream;
	bitStream.Write((MessageID) ID_SESSION_CONFIG);
	if (data && length > 0)
		bitStream.Write(data, length);
	SendImmediate((char *) bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, remoteSystem->systemAddress, false, false, MafiaNet::GetTimeUS(), 0);

	remoteSystem->connectMode = RemoteSystemStruct::CONNECTED;
	remoteSystem->sessionConfigStartTime = 0;
	ProduceWithheldConnectionPacket(remoteSystem, (MessageID) ID_NEW_INCOMING_CONNECTION);

	PingInternal(remoteSystem->systemAddress, true, MafiaNet::Reliability::Unreliable);
	quitAndDataEvents.SetEvent();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SendSessionConfigRejection(RemoteSystemStruct *remoteSystem, const char *reason, unsigned int reasonLength)
{
	if (remoteSystem == 0)
		return;

	MafiaNet::BitStream bitStream;
	bitStream.Write((MessageID) ID_SESSION_CONFIG_REJECTED);
	if (reason && reasonLength > 0)
		bitStream.Write(reason, reasonLength);
	SendImmediate((char *) bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, remoteSystem->systemAddress, false, false, MafiaNet::GetTimeUS(), 0);

	// The refusal has to reach the client before the socket goes away, so drop on no-ack rather than
	// silently: the client turns ID_SESSION_CONFIG_REJECTED into ID_CONNECTION_ATTEMPT_FAILED itself.
	remoteSystem->connectMode = RemoteSystemStruct::DISCONNECT_ON_NO_ACK;
	remoteSystem->sessionConfigStartTime = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetNumberOfRemoteInitiatedConnections(void) const
{
	if (remoteSystemList == 0 || endThreads == true)
		return 0;

	unsigned int numberOfIncomingConnections;
	numberOfIncomingConnections = 0;
	unsigned int i;
	for (i = 0; i < activeSystemListSize; i++)
	{
		if ((activeSystemList[i])->isActive &&
		    (activeSystemList[i])->connectMode == RakPeer::RemoteSystemStruct::CONNECTED &&
		    (activeSystemList[i])->weInitiatedTheConnection == false)
		{
			numberOfIncomingConnections++;
		}
	}
	return numberOfIncomingConnections;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::IPConnectedRecently(const SystemAddress &systemAddress, MafiaNet::TimeMS time) const
{
	if (!limitConnectionFrequencyFromTheSameIP || IsLoopbackAddress(systemAddress, false))
		return false;
	for (unsigned i = 0; i < maximumNumberOfPeers; i++)
	{
		// 4/13/09 Attackers can flood ID_OPEN_CONNECTION_REQUEST and use up all available connection slots
		// Ignore connection attempts if this IP address connected within the last 100 milliseconds
		if (remoteSystemList[i].isActive == true &&
		    remoteSystemList[i].systemAddress.EqualsExcludingPort(systemAddress) &&
		    time >= remoteSystemList[i].connectionTime &&
		    time - remoteSystemList[i].connectionTime < 100)
			return true;
	}
	return false;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakPeer::RemoteSystemStruct *RakPeer::AssignSystemAddressToRemoteSystemList(const SystemAddress systemAddress, RemoteSystemStruct::ConnectMode connectionMode, RakNetSocket2 *incomingRakNetSocket, bool *thisIPConnectedRecently, SystemAddress bindingAddress, int incomingMTU, RakNetGUID guid, const SessionKeys *sessionKeys, const CryptInfo &localCryptInfo, const CryptInfo &remoteCryptInfo)
{
	RemoteSystemStruct *remoteSystem;
	unsigned j, assignedIndex;
	MafiaNet::TimeMS time = MafiaNet::GetTimeMS();
#ifdef _DEBUG
	RakAssert(systemAddress != UNASSIGNED_SYSTEM_ADDRESS);
#endif

	// Mode Required needs the keys the handshake just derived; Plaintext must not get any. Both callers
	// guarantee this; refuse rather than run a Required connection unencrypted.
	if ((encryptionMode == EncryptionMode::Required) != (sessionKeys != nullptr))
		return 0;

	// The ID_OPEN_CONNECTION_REQUEST_2 handler runs the same check before its key exchange; this one also
	// covers the connecting side.
	if (IPConnectedRecently(systemAddress, time))
	{
		*thisIPConnectedRecently = true;
		ValidateRemoteSystemLookup();
		return 0;
	}

	// Don't use a different port than what we received on
	bindingAddress.CopyPort(incomingRakNetSocket->GetBoundAddress());

	*thisIPConnectedRecently = false;
	for (assignedIndex = 0; assignedIndex < maximumNumberOfPeers; assignedIndex++)
	{
		if (remoteSystemList[assignedIndex].isActive == false)
		{
			// printf("--- Address %s has become active\n", systemAddress.ToString());

			remoteSystem = remoteSystemList + assignedIndex;

			// incomingMTU comes off the wire -- ID_OPEN_CONNECTION_REQUEST_2 on the accepting side,
			// ID_OPEN_CONNECTION_REPLY_2 on the connecting one -- so it is whatever the remote peer
			// said, and must be clamped rather than trusted. MTUSize sizes every datagram the
			// reliability layer builds into buffers declared MAXIMUM_MTU_SIZE bytes long, so a
			// larger value writes past the end of them. A peer built against a higher cap is enough
			// to produce one; a hostile peer can name any uint16. This used to be a bare RakAssert,
			// which is compiled out of exactly the builds that ship.
			if (incomingMTU > MAXIMUM_MTU_SIZE)
				incomingMTU = MAXIMUM_MTU_SIZE;
			const int mtuSize = incomingMTU > defaultMTUSize ? incomingMTU : defaultMTUSize;
			RakAssert(mtuSize <= MAXIMUM_MTU_SIZE);

			// Set up the reliability layer before the slot goes active, so a failure leaves it inactive.
			// Order matters: the mode decides whether Reset() reserves the cipher overhead, and Reset()
			// wipes keys, so the keys go in last. In mode Required the layer then seals every datagram it
			// sends and drops every one that does not open; a layer left without keys sends and accepts
			// nothing. There is no plaintext fallback.
			remoteSystem->reliabilityLayer.SetEncryptionMode(encryptionMode);
			remoteSystem->reliabilityLayer.Reset(true, mtuSize);
			if (sessionKeys != nullptr && !remoteSystem->reliabilityLayer.SetSessionKeys(*sessionKeys))
			{
				// Key installation failed: the slot stays inactive and nothing is allocated.
				remoteSystem->reliabilityLayer.Reset(false, mtuSize);
				ValidateRemoteSystemLookup();
				return 0;
			}

			ReferenceRemoteSystem(systemAddress, assignedIndex);
			// Stale reason payload from a prior occupant of this slot must never leak into a new connection.
			ClearDisconnectReason(remoteSystem);
			// Same for session state: peer.h documents it as cleared on slot reuse, and only the teardown
			// paths do it today. Without this, any future path that deactivates a slot without going
			// through CloseConnectionInternal2 would hand the previous occupant payload to the new one.
			ClearSessionConfig(remoteSystem);
			remoteSystem->MTUSize = mtuSize;
			remoteSystem->guid = guid;
			remoteSystem->raceFirstAddress = UNASSIGNED_SYSTEM_ADDRESS;
			remoteSystem->isActive = true; // This one line causes future incoming packets to go through the reliability layer
			remoteSystem->handshakeLocalCryptInfo = localCryptInfo;
			remoteSystem->handshakeRemoteCryptInfo = remoteCryptInfo;
			remoteSystem->reliabilityLayer.SetSplitMessageProgressInterval(splitMessageProgressInterval);
			remoteSystem->reliabilityLayer.SetUnreliableTimeout(unreliableTimeout);
			remoteSystem->reliabilityLayer.SetTimeoutTime(defaultTimeoutTime);
			AddToActiveSystemList(assignedIndex);
			if (incomingRakNetSocket->GetBoundAddress() == bindingAddress)
			{
				remoteSystem->rakNetSocket = incomingRakNetSocket;
			}
			else
			{
				// Originally this searched ipList for the internal address the datagram came in on and
				// forced the reply onto it, so a multi-homed machine answers from the same IP. That was
				// disabled upstream (06/26/09, unconfirmed report that the Vista firewall blocks the reply
				// if we force a binding), which left the search computing an index nobody read. Always
				// reply on the socket the datagram came in on.
				remoteSystem->rakNetSocket = incomingRakNetSocket;
			}

			for (j = 0; j < (unsigned) PING_TIMES_ARRAY_SIZE; j++)
			{
				remoteSystem->pingAndClockDifferential[j].pingTime = 65535;
				remoteSystem->pingAndClockDifferential[j].clockDifferential = 0;
			}

			remoteSystem->connectMode = connectionMode;
			remoteSystem->pingAndClockDifferentialWriteIndex = 0;
			remoteSystem->lowestPing = 65535;
			remoteSystem->nextPingTime = 0; // Ping immediately
			remoteSystem->weInitiatedTheConnection = false;
			remoteSystem->absorbedOurConnectAttempt = false;
			remoteSystem->connectionTime = time;
			remoteSystem->myExternalSystemAddress = UNASSIGNED_SYSTEM_ADDRESS;
			remoteSystem->lastReliableSend = time;

#ifdef _DEBUG
			int indexLoopupCheck = GetIndexFromSystemAddress(systemAddress, true);
			if ((int) indexLoopupCheck != (int) assignedIndex)
			{
				RakAssert((int) indexLoopupCheck == (int) assignedIndex);
			}
#endif

			return remoteSystem;
		}
	}

	return 0;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Adjust the first four bytes (treated as unsigned int) of the pointer
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ShiftIncomingTimestamp(unsigned char *data, const SystemAddress &systemAddress) const
{
#ifdef _DEBUG
	RakAssert(IsActive());
	RakAssert(data);
#endif

	MafiaNet::BitStream timeBS(data, sizeof(MafiaNet::Time), false);
	MafiaNet::Time encodedTimestamp;
	timeBS.Read(encodedTimestamp);

	encodedTimestamp = encodedTimestamp - GetBestClockDifferential(systemAddress);
	timeBS.SetWriteOffset(0);
	timeBS.Write(encodedTimestamp);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Thanks to Chris Taylor (cat02e@fsu.edu) for the improved timestamping algorithm
MafiaNet::Time RakPeer::GetBestClockDifferential(const SystemAddress systemAddress) const
{
	RemoteSystemStruct *remoteSystem = GetRemoteSystemFromSystemAddress(systemAddress, true, true);

	if (remoteSystem == 0)
		return 0;

	return GetClockDifferentialInt(remoteSystem);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::RemoteSystemLookupHashIndex(const SystemAddress &sa) const
{
	return SystemAddress::ToInteger(sa) % ((unsigned int) maximumNumberOfPeers * REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ReferenceRemoteSystem(const SystemAddress &sa, unsigned int remoteSystemListIndex)
{
	// #ifdef _DEBUG
	// 	for ( unsigned int remoteSystemIndex = 0; remoteSystemIndex < maximumNumberOfPeers; ++remoteSystemIndex )
	// 	{
	// 		if (remoteSystemList[remoteSystemIndex].isActive )
	// 		{
	// 			unsigned int debugHashIndex = GetRemoteSystemIndex(remoteSystemList[remoteSystemIndex].systemAddress);
	// 			RakAssert(debugHashIndex==remoteSystemIndex);
	// 		}
	// 	}
	// #endif


	SystemAddress oldAddress = remoteSystemList[remoteSystemListIndex].systemAddress;
	if (oldAddress != UNASSIGNED_SYSTEM_ADDRESS)
	{
		// The system might be active if rerouting
		//		RakAssert(remoteSystemList[remoteSystemListIndex].isActive==false);

		// Remove the reference if the reference is pointing to this inactive system
		if (GetRemoteSystem(oldAddress) == &remoteSystemList[remoteSystemListIndex])
			DereferenceRemoteSystem(oldAddress);
	}
	DereferenceRemoteSystem(sa);

	// #ifdef _DEBUG
	// 	for ( unsigned int remoteSystemIndex = 0; remoteSystemIndex < maximumNumberOfPeers; ++remoteSystemIndex )
	// 	{
	// 		if (remoteSystemList[remoteSystemIndex].isActive )
	// 		{
	// 			unsigned int debugHashIndex = GetRemoteSystemIndex(remoteSystemList[remoteSystemIndex].systemAddress);
	// 			if (debugHashIndex!=remoteSystemIndex)
	// 			{
	// 				RakAssert(debugHashIndex==remoteSystemIndex);
	// 			}
	// 		}
	// 	}
	// #endif


	remoteSystemList[remoteSystemListIndex].systemAddress = sa;

	unsigned int hashIndex = RemoteSystemLookupHashIndex(sa);
	// One entry per reference. (A second Allocate on a bucket collision used to overwrite the first, which
	// was then never released: one pool entry lost per colliding connection for the life of the peer.)
	RemoteSystemIndex *rsi = remoteSystemIndexPool.Allocate(_FILE_AND_LINE_);
	rsi->next = 0;
	rsi->index = remoteSystemListIndex;
	if (remoteSystemLookup[hashIndex] == 0)
	{
		remoteSystemLookup[hashIndex] = rsi;
	}
	else
	{
		RemoteSystemIndex *cur = remoteSystemLookup[hashIndex];
		while (cur->next != 0)
		{
			cur = cur->next;
		}
		cur->next = rsi;
	}

	// #ifdef _DEBUG
	// 	for ( unsigned int remoteSystemIndex = 0; remoteSystemIndex < maximumNumberOfPeers; ++remoteSystemIndex )
	// 	{
	// 		if (remoteSystemList[remoteSystemIndex].isActive )
	// 		{
	// 			unsigned int debugHashIndex = GetRemoteSystemIndex(remoteSystemList[remoteSystemIndex].systemAddress);
	// 			RakAssert(debugHashIndex==remoteSystemIndex);
	// 		}
	// 	}
	// #endif


	RakAssert(GetRemoteSystemIndex(sa) == remoteSystemListIndex);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::DereferenceRemoteSystem(const SystemAddress &sa)
{
	unsigned int hashIndex = RemoteSystemLookupHashIndex(sa);
	RemoteSystemIndex *cur = remoteSystemLookup[hashIndex];
	RemoteSystemIndex *last = 0;
	while (cur != 0)
	{
		if (remoteSystemList[cur->index].systemAddress == sa)
		{
			if (last == 0)
			{
				remoteSystemLookup[hashIndex] = cur->next;
			}
			else
			{
				last->next = cur->next;
			}
			remoteSystemIndexPool.Release(cur, _FILE_AND_LINE_);
			break;
		}
		last = cur;
		cur = cur->next;
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetRemoteSystemIndex(const SystemAddress &sa) const
{
	unsigned int hashIndex = RemoteSystemLookupHashIndex(sa);
	RemoteSystemIndex *cur = remoteSystemLookup[hashIndex];
	while (cur != 0)
	{
		if (remoteSystemList[cur->index].systemAddress == sa)
			return cur->index;
		cur = cur->next;
	}
	return (unsigned int) -1;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakPeer::RemoteSystemStruct *RakPeer::GetRemoteSystem(const SystemAddress &sa) const
{
	unsigned int remoteSystemIndex = GetRemoteSystemIndex(sa);
	if (remoteSystemIndex == (unsigned int) -1)
		return 0;
	return remoteSystemList + remoteSystemIndex;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearRemoteSystemLookup(void)
{
	remoteSystemIndexPool.Clear(_FILE_AND_LINE_);
	MafiaNet::OP_DELETE_ARRAY(remoteSystemLookup, _FILE_AND_LINE_);
	remoteSystemLookup = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::AddToActiveSystemList(unsigned int remoteSystemListIndex)
{
	activeSystemList[activeSystemListSize++] = remoteSystemList + remoteSystemListIndex;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::RemoveFromActiveSystemList(const SystemAddress &sa)
{
	unsigned int i;
	for (i = 0; i < activeSystemListSize; i++)
	{
		RemoteSystemStruct *rss = activeSystemList[i];
		if (rss->systemAddress == sa)
		{
			activeSystemList[i] = activeSystemList[activeSystemListSize - 1];
			activeSystemListSize--;
			return;
		}
	}
	RakAssert("activeSystemList invalid, entry not found in RemoveFromActiveSystemList. Ensure that AddToActiveSystemList and RemoveFromActiveSystemList are called by the same thread." && 0);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/*
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::LookupIndexUsingHashIndex(const SystemAddress &sa) const
{
	unsigned int scanCount=0;
	unsigned int index = RemoteSystemLookupHashIndex(sa);
	if (remoteSystemLookup[index].index==(unsigned int)-1)
		return (unsigned int) -1;
	while (remoteSystemList[remoteSystemLookup[index].index].systemAddress!=sa)
	{
		if (++index==(unsigned int) maximumNumberOfPeers*REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE)
			index=0;
		if (++scanCount>(unsigned int) maximumNumberOfPeers*REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE)
			return (unsigned int) -1;
		if (remoteSystemLookup[index].index==-1)
			return (unsigned int) -1;
	}
	return index;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::RemoteSystemListIndexUsingHashIndex(const SystemAddress &sa) const
{
	unsigned int index = LookupIndexUsingHashIndex(sa);
	if (index!=(unsigned int) -1)
	{
		return remoteSystemLookup[index].index;
	}
	return (unsigned int) -1;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::FirstFreeRemoteSystemLookupIndex(const SystemAddress &sa) const
{
//	unsigned int collisionCount=0;
	unsigned int index = RemoteSystemLookupHashIndex(sa);
	while (remoteSystemLookup[index].index!=(unsigned int)-1)
	{
		if (++index==(unsigned int) maximumNumberOfPeers*REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE)
			index=0;
//		collisionCount++;
	}
//	printf("%i collisions. Using index %i\n", collisionCount, index);
	return index;
}
*/
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::IsLoopbackAddress(const AddressOrGUID &systemIdentifier, bool matchPort) const
{
	if (systemIdentifier.rakNetGuid != UNASSIGNED_RAKNET_GUID)
		return systemIdentifier.rakNetGuid == myGuid;

	for (int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS && ipList[i] != UNASSIGNED_SYSTEM_ADDRESS; i++)
	{
		if (matchPort)
		{
			if (ipList[i] == systemIdentifier.systemAddress)
				return true;
		}
		else
		{
			if (ipList[i].EqualsExcludingPort(systemIdentifier.systemAddress))
				return true;
		}
	}

	return (matchPort == true && systemIdentifier.systemAddress == firstExternalID) ||
	       (matchPort == false && systemIdentifier.systemAddress.EqualsExcludingPort(firstExternalID));
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
SystemAddress RakPeer::GetLoopbackAddress(void) const
{

	return ipList[0];
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::AllowIncomingConnections(void) const
{
	// Counts peers still running the session handshake as well as fully established ones.
	// GetNumberOfRemoteInitiatedConnections() deliberately reports only CONNECTED peers -- that is what
	// an application means by "players" -- but admission control must not share that view: a peer parked
	// in EXCHANGING_SESSION_DATA already owns a slot, and counting only CONNECTED would let peers that
	// stall the handshake push the real total past SetMaximumIncomingConnections().
	if (remoteSystemList == 0 || endThreads == true)
		return false;

	return AllowIncomingConnection(UNASSIGNED_SYSTEM_ADDRESS);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::AllowIncomingConnection(const SystemAddress &address) const
{
	if (remoteSystemList == 0 || endThreads == true)
		return false;

	SessionAdmission::Limits limits;
	limits.maxIncoming = GetMaximumIncomingConnections();
	limits.maxPending = maximumPendingSessions;
	limits.maxPendingPerAddress = maximumPendingSessionsPerAddress;

	SessionAdmission::Counts counts;
	CountRemoteInitiatedPeers(address, counts.connected, counts.exchanging, counts.pending, counts.pendingFromAddress);
	return SessionAdmission::MayStartConnecting(limits, counts);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A pending peer is one that asked to connect and is not CONNECTED yet: still proving the transport
// (UNVERIFIED_SENDER, HANDLING_CONNECTION_REQUEST) or in the session handshake. A peer being torn down is
// in none of these, so a refusal frees its share of the pool at once rather than after the ack.
void RakPeer::CountRemoteInitiatedPeers(const SystemAddress &address, unsigned int &connected, unsigned int &exchanging, unsigned int &pending, unsigned int &pendingFromAddress) const
{
	connected = exchanging = pending = pendingFromAddress = 0;
	for (unsigned int i = 0; i < activeSystemListSize; i++)
	{
		const RemoteSystemStruct *system = activeSystemList[i];
		if (!system->isActive || system->weInitiatedTheConnection)
			continue;

		switch (system->connectMode)
		{
		case RemoteSystemStruct::CONNECTED:
			connected++;
			break;
		case RemoteSystemStruct::EXCHANGING_SESSION_DATA:
			exchanging++;
			// A peer in the handshake is pending too.
			[[fallthrough]];
		case RemoteSystemStruct::UNVERIFIED_SENDER:
		case RemoteSystemStruct::HANDLING_CONNECTION_REQUEST:
			pending++;
			if (address != UNASSIGNED_SYSTEM_ADDRESS && system->systemAddress.EqualsExcludingPort(address))
				pendingFromAddress++;
			break;
		default:
			break;
		}
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
MafiaNet::TimeMS RakPeer::GetSessionTimeoutFor(RemoteSystemStruct *remoteSystem) const
{
	return sessionTimeout != 0 ? sessionTimeout : remoteSystem->reliabilityLayer.GetTimeoutTime();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::DeallocRNS2RecvStruct(RNS2RecvStruct *s, const char *file, unsigned int line)
{
	bufferedPacketsFreePoolMutex.Lock();
	bufferedPacketsFreePool.Push(s, file, line);
	bufferedPacketsFreePoolMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RNS2RecvStruct *RakPeer::AllocRNS2RecvStruct(const char *file, unsigned int line)
{
	bufferedPacketsFreePoolMutex.Lock();
	if (bufferedPacketsFreePool.Size() > 0)
	{
		RNS2RecvStruct *s = bufferedPacketsFreePool.Pop();
		bufferedPacketsFreePoolMutex.Unlock();
		return s;
	}
	else
	{
		bufferedPacketsFreePoolMutex.Unlock();
		return MafiaNet::OP_NEW<RNS2RecvStruct>(file, line);
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearBufferedPackets(void)
{
	bufferedPacketsFreePoolMutex.Lock();
	while (bufferedPacketsFreePool.Size() > 0)
		MafiaNet::OP_DELETE(bufferedPacketsFreePool.Pop(), _FILE_AND_LINE_);
	bufferedPacketsFreePoolMutex.Unlock();

	bufferedPacketsQueueMutex.Lock();
	while (bufferedPacketsQueue.Size() > 0)
		MafiaNet::OP_DELETE(bufferedPacketsQueue.Pop(), _FILE_AND_LINE_);
	bufferedPacketsQueueMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetupBufferedPackets(void)
{
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::PushBufferedPacket(RNS2RecvStruct *p)
{
	bufferedPacketsQueueMutex.Lock();
	bufferedPacketsQueue.Push(p, _FILE_AND_LINE_);
	bufferedPacketsQueueMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RNS2RecvStruct *RakPeer::PopBufferedPacket(void)
{
	bufferedPacketsQueueMutex.Lock();
	if (bufferedPacketsQueue.Size() > 0)
	{
		RNS2RecvStruct *s = bufferedPacketsQueue.Pop();
		bufferedPacketsQueueMutex.Unlock();
		return s;
	}
	bufferedPacketsQueueMutex.Unlock();
	return 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::PingInternal(const SystemAddress target, bool performImmediate, MafiaNet::Reliability reliability)
{
	if (IsActive() == false)
		return;

	MafiaNet::BitStream bitStream(sizeof(unsigned char) + sizeof(MafiaNet::Time));
	bitStream.Write((MessageID) ID_CONNECTED_PING);
	bitStream.Write(MafiaNet::GetTime());
	if (performImmediate)
		SendImmediate((char *) bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, reliability, 0, target, false, false, MafiaNet::GetTimeUS(), 0);
	else
		Send(&bitStream, MafiaNet::Priority::Immediate, reliability, 0, target, false);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::CloseConnectionInternal(const AddressOrGUID &systemIdentifier, bool sendDisconnectionNotification, bool performImmediate, unsigned char orderingChannel, MafiaNet::Priority disconnectionNotificationPriority)
{
	RakAssert(orderingChannel < 32);

	if (systemIdentifier.IsUndefined())
		return;

	if (remoteSystemList == 0 || endThreads == true)
		return;

	// #med - this is flawed and needs to be resolved properly - socketList[0] is not correct here
	CloseConnectionInternal2(systemIdentifier, sendDisconnectionNotification, performImmediate, orderingChannel, disconnectionNotificationPriority, *(socketList[0]));
}

// #med - better integrate directly in CloseConnectionInternal2()
void RakPeer::CloseConnectionInternal2(const AddressOrGUID &systemIdentifier, bool sendDisconnectionNotification, bool performImmediate, unsigned char orderingChannel, MafiaNet::Priority disconnectionNotificationPriority, RakNetSocket2 &socket, const MafiaNet::BitStream *reasonData)
{
	RakAssert(orderingChannel < 32);

	if (systemIdentifier.IsUndefined())
		return;

	if (remoteSystemList == 0 || endThreads == true)
		return;

	SystemAddress target;
	if (systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS)
	{
		target = systemIdentifier.systemAddress;
	}
	else
	{
		target = GetSystemAddressFromGuid(systemIdentifier.rakNetGuid);
	}

	if (target != UNASSIGNED_SYSTEM_ADDRESS && performImmediate)
	{
		FixForSocket(target, &socket);
	}

	if (sendDisconnectionNotification)
	{
		NotifyAndFlagForShutdown(target, performImmediate, orderingChannel, disconnectionNotificationPriority, reasonData);
	}
	else
	{
		if (performImmediate)
		{
			unsigned int index = GetRemoteSystemIndex(target);
			if (index != (unsigned int) -1)
			{
				if (remoteSystemList[index].isActive)
				{
					RemoveFromActiveSystemList(target);

					// Found the index to stop
					// printf("--- Address %s has become inactive\n", remoteSystemList[index].systemAddress.ToString());
					remoteSystemList[index].isActive = false;

					remoteSystemList[index].guid = UNASSIGNED_RAKNET_GUID;
					remoteSystemList[index].raceFirstAddress = UNASSIGNED_SYSTEM_ADDRESS;

					// Reserve this reliability layer for ourselves
					//remoteSystemList[ remoteSystemLookup[index].index ].systemAddress = UNASSIGNED_SYSTEM_ADDRESS;

					// Clear any remaining messages
					RakAssert(remoteSystemList[index].MTUSize <= MAXIMUM_MTU_SIZE);
					remoteSystemList[index].reliabilityLayer.Reset(false, remoteSystemList[index].MTUSize);

					// Free any stashed disconnect-reason payload (e.g. delivered already, or never consumed)
					ClearDisconnectReason(&remoteSystemList[index]);

					// Free any session-handshake state left over from an exchange that never completed
					ClearSessionConfig(&remoteSystemList[index]);

					// Not using this socket
					remoteSystemList[index].rakNetSocket = 0;
				}
			}
		}
		else
		{
			BufferedCommandStruct *bcs;
			bcs = bufferedCommands.Allocate(_FILE_AND_LINE_);
			bcs->command = BufferedCommandStruct::BCS_CLOSE_CONNECTION;
			bcs->systemIdentifier = target;
			// #med - review whether this is safe here - can't the socket be closed/released before its used?
			bcs->socket = &socket;
			bcs->data = 0;
			bcs->orderingChannel = orderingChannel;
			bcs->priority = disconnectionNotificationPriority;
			bufferedCommands.Push(bcs);
		}
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SendBuffered(const char *data, BitSize_t numberOfBitsToSend, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, RemoteSystemStruct::ConnectMode connectionMode, uint32_t receipt)
{
	BufferedCommandStruct *bcs;

	bcs = bufferedCommands.Allocate(_FILE_AND_LINE_);
	bcs->data = (char *) rakMalloc_Ex((size_t) BITS_TO_BYTES(numberOfBitsToSend), _FILE_AND_LINE_); // Making a copy doesn't lose efficiency because I tell the reliability layer to use this allocation for its own copy
	if (bcs->data == 0)
	{
		notifyOutOfMemory(_FILE_AND_LINE_);
		bufferedCommands.Deallocate(bcs, _FILE_AND_LINE_);
		return;
	}

	RakAssert(!((unsigned int) reliability >= MafiaNet::NUMBER_OF_RELIABILITIES || (int) reliability < 0));
	RakAssert(!((int) priority > (int) MafiaNet::NUMBER_OF_PRIORITIES || (int) priority < 0));
	RakAssert(!(orderingChannel >= NUMBER_OF_ORDERED_STREAMS));

	memcpy(bcs->data, data, (size_t) BITS_TO_BYTES(numberOfBitsToSend));
	bcs->numberOfBitsToSend = numberOfBitsToSend;
	bcs->priority = priority;
	bcs->reliability = reliability;
	bcs->orderingChannel = orderingChannel;
	bcs->systemIdentifier = systemIdentifier;
	bcs->broadcast = broadcast;
	bcs->connectionMode = connectionMode;
	bcs->receipt = receipt;
	bcs->command = BufferedCommandStruct::BCS_SEND;
	bufferedCommands.Push(bcs);

	if (priority == MafiaNet::Priority::Immediate)
	{
		// Forces pending sends to go out now, rather than waiting to the next update interval
		quitAndDataEvents.SetEvent();
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SendBufferedList(const char **data, const int *lengths, const int numParameters, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, RemoteSystemStruct::ConnectMode connectionMode, uint32_t receipt)
{
	BufferedCommandStruct *bcs;
	unsigned int totalLength = 0;
	unsigned int lengthOffset;
	int i;
	for (i = 0; i < numParameters; i++)
	{
		if (lengths[i] > 0)
			totalLength += lengths[i];
	}
	if (totalLength == 0)
		return;

	char *dataAggregate;
	dataAggregate = (char *) rakMalloc_Ex((size_t) totalLength, _FILE_AND_LINE_); // Making a copy doesn't lose efficiency because I tell the reliability layer to use this allocation for its own copy
	if (dataAggregate == 0)
	{
		notifyOutOfMemory(_FILE_AND_LINE_);
		return;
	}
	for (i = 0, lengthOffset = 0; i < numParameters; i++)
	{
		if (lengths[i] > 0)
		{
			memcpy(dataAggregate + lengthOffset, data[i], lengths[i]);
			lengthOffset += lengths[i];
		}
	}

	if (broadcast == false && IsLoopbackAddress(systemIdentifier, true))
	{
		SendLoopback(dataAggregate, totalLength);
		rakFree_Ex(dataAggregate, _FILE_AND_LINE_);
		return;
	}

	RakAssert(!((unsigned int) reliability >= MafiaNet::NUMBER_OF_RELIABILITIES || (int) reliability < 0));
	RakAssert(!((int) priority > (int) MafiaNet::NUMBER_OF_PRIORITIES || (int) priority < 0));
	RakAssert(!(orderingChannel >= NUMBER_OF_ORDERED_STREAMS));

	bcs = bufferedCommands.Allocate(_FILE_AND_LINE_);
	bcs->data = dataAggregate;
	bcs->numberOfBitsToSend = BYTES_TO_BITS(totalLength);
	bcs->priority = priority;
	bcs->reliability = reliability;
	bcs->orderingChannel = orderingChannel;
	bcs->systemIdentifier = systemIdentifier;
	bcs->broadcast = broadcast;
	bcs->connectionMode = connectionMode;
	bcs->receipt = receipt;
	bcs->command = BufferedCommandStruct::BCS_SEND;
	bufferedCommands.Push(bcs);

	if (priority == MafiaNet::Priority::Immediate)
	{
		// Forces pending sends to go out now, rather than waiting to the next update interval
		quitAndDataEvents.SetEvent();
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::SendImmediate(char *data, BitSize_t numberOfBitsToSend, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, bool useCallerDataAllocation, MafiaNet::TimeUS currentTime, uint32_t receipt)
{
	unsigned *sendList;
	unsigned sendListSize;
	bool callerDataAllocationUsed;
	unsigned int remoteSystemIndex, sendListIndex; // Iterates into the list of remote systems
	                                               //	unsigned numberOfBytesUsed = (unsigned) BITS_TO_BYTES(numberOfBitsToSend);
	callerDataAllocationUsed = false;

	sendListSize = 0;

	if (systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS)
		remoteSystemIndex = GetIndexFromSystemAddress(systemIdentifier.systemAddress, true);
	else if (systemIdentifier.rakNetGuid != UNASSIGNED_RAKNET_GUID)
		remoteSystemIndex = GetSystemIndexFromGuid(systemIdentifier.rakNetGuid);
	else
		remoteSystemIndex = (unsigned int) -1;

	// 03/06/06 - If broadcast is false, use the optimized version of GetIndexFromSystemAddress
	if (broadcast == false)
	{
		if (remoteSystemIndex == (unsigned int) -1)
		{
#ifdef _DEBUG
//			int debugIndex = GetRemoteSystemIndex(systemIdentifier.systemAddress);
#endif
			return false;
		}

#if USE_ALLOCA == 1
		sendList = (unsigned *) alloca(sizeof(unsigned));
#else
		sendList = (unsigned *) rakMalloc_Ex(sizeof(unsigned), _FILE_AND_LINE_);
#endif

		if (remoteSystemList[remoteSystemIndex].isActive &&
		    remoteSystemList[remoteSystemIndex].connectMode != RemoteSystemStruct::DISCONNECT_ASAP &&
		    remoteSystemList[remoteSystemIndex].connectMode != RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY &&
		    remoteSystemList[remoteSystemIndex].connectMode != RemoteSystemStruct::DISCONNECT_ON_NO_ACK)
		{
			sendList[0] = remoteSystemIndex;
			sendListSize = 1;
		}
	}
	else
	{
#if USE_ALLOCA == 1
		sendList = (unsigned *) alloca(sizeof(unsigned) * maximumNumberOfPeers);
#else
		sendList = (unsigned *) rakMalloc_Ex(sizeof(unsigned) * maximumNumberOfPeers, _FILE_AND_LINE_);
#endif

		// remoteSystemList in network thread
		unsigned int idx;
		for (idx = 0; idx < maximumNumberOfPeers; idx++)
		{
			if (remoteSystemIndex != (unsigned int) -1 && idx == remoteSystemIndex)
				continue;

			// Peers still running the session handshake are excluded: the application has not been told they
			// exist, so broadcasting to them would hand application data to a peer it never accepted -- and
			// possibly to one it is about to refuse with RejectSession(). Internal handshake traffic is always
			// directed, so it never reaches this branch.
			if (remoteSystemList[idx].isActive && remoteSystemList[idx].systemAddress != UNASSIGNED_SYSTEM_ADDRESS &&
			    remoteSystemList[idx].connectMode != RemoteSystemStruct::EXCHANGING_SESSION_DATA)
				sendList[sendListSize++] = idx;
		}
	}

	if (sendListSize == 0)
	{
#if !defined(USE_ALLOCA)
		rakFree_Ex(sendList, _FILE_AND_LINE_);
#endif

		return false;
	}

	for (sendListIndex = 0; sendListIndex < sendListSize; sendListIndex++)
	{
		// Send may split the packet and thus deallocate data.  Don't assume data is valid if we use the callerAllocationData
		bool useData = useCallerDataAllocation && callerDataAllocationUsed == false && sendListIndex + 1 == sendListSize;
		remoteSystemList[sendList[sendListIndex]].reliabilityLayer.Send(data, numberOfBitsToSend, priority, reliability, orderingChannel, useData == false, remoteSystemList[sendList[sendListIndex]].MTUSize, currentTime, receipt);
		if (useData)
			callerDataAllocationUsed = true;

		if (reliability == MafiaNet::Reliability::Reliable ||
		    reliability == MafiaNet::Reliability::ReliableOrdered ||
		    reliability == MafiaNet::Reliability::ReliableSequenced ||
		    reliability == MafiaNet::Reliability::ReliableWithAckReceipt ||
		    reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt
		    //			||
		    //			reliability==RELIABLE_SEQUENCED_WITH_ACK_RECEIPT
		)
			remoteSystemList[sendList[sendListIndex]].lastReliableSend = (MafiaNet::TimeMS)(currentTime / (MafiaNet::TimeUS) 1000);
	}

#if !defined(USE_ALLOCA)
	rakFree_Ex(sendList, _FILE_AND_LINE_);
#endif

	// Return value only meaningful if true was passed for useCallerDataAllocation.  Means the reliability layer used that data copy, so the caller should not deallocate it
	return callerDataAllocationUsed;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ResetSendReceipt(void)
{
	sendReceiptSerialMutex.Lock();
	sendReceiptSerial = 1;
	sendReceiptSerialMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::OnConnectedPong(MafiaNet::Time sendPingTime, MafiaNet::Time sendPongTime, RemoteSystemStruct *remoteSystem)
{
	MafiaNet::Time ping;
	//	MafiaNet::TimeMS lastPing;
	MafiaNet::Time time = MafiaNet::GetTime(); // Update the time value to be accurate
	if (time > sendPingTime)
		ping = time - sendPingTime;
	else
		ping = 0;

	//	lastPing = remoteSystem->pingAndClockDifferential[ remoteSystem->pingAndClockDifferentialWriteIndex ].pingTime;

	remoteSystem->pingAndClockDifferential[remoteSystem->pingAndClockDifferentialWriteIndex].pingTime = (unsigned short) ping;
	// Thanks to Chris Taylor (cat02e@fsu.edu) for the improved timestamping algorithm
	// Divide each integer by 2, rather than the sum by 2, to prevent overflow
	remoteSystem->pingAndClockDifferential[remoteSystem->pingAndClockDifferentialWriteIndex].clockDifferential = sendPongTime - (time / 2 + sendPingTime / 2);

	if (remoteSystem->lowestPing == (unsigned short) -1 || remoteSystem->lowestPing > (int) ping)
		remoteSystem->lowestPing = (unsigned short) ping;

	if (++(remoteSystem->pingAndClockDifferentialWriteIndex) == (MafiaNet::Time) PING_TIMES_ARRAY_SIZE)
		remoteSystem->pingAndClockDifferentialWriteIndex = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearBufferedCommands(void)
{
	BufferedCommandStruct *bcs;

	while ((bcs = bufferedCommands.Pop()) != 0)
	{
		if (bcs->data)
			rakFree_Ex(bcs->data, _FILE_AND_LINE_);

		bufferedCommands.Deallocate(bcs, _FILE_AND_LINE_);
	}
	bufferedCommands.Clear(_FILE_AND_LINE_);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearSocketQueryOutput(void)
{
	socketQueryOutput.Clear(_FILE_AND_LINE_);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearRequestedConnectionList(void)
{
	DataStructures::Queue<RequestedConnectionStruct *> freeQueue;
	requestedConnectionQueueMutex.Lock();
	while (requestedConnectionQueue.Size())
		freeQueue.Push(requestedConnectionQueue.Pop(), _FILE_AND_LINE_);
	requestedConnectionQueueMutex.Unlock();
	unsigned i;
	for (i = 0; i < freeQueue.Size(); i++)
		DeleteRequestedConnection(freeQueue[i]);
}
bool RakPeer::IsInsideNetworkThreadReceive(void) const
{
	return networkThreadDispatcher.load() == std::this_thread::get_id();
}

void RakPeer::SetNetworkThreadReceiveStopped(bool stopped)
{
	pluginListNetworkThreadMutex.Lock();
	networkThreadReceiveStopped = stopped;
	pluginListNetworkThreadMutex.Unlock();
}

void RakPeer::DispatchNetworkThreadUpdate(void)
{
	pluginListNetworkThreadMutex.Lock();
	if (networkThreadReceiveStopped)
	{
		pluginListNetworkThreadMutex.Unlock();
		return;
	}

	networkThreadDispatcher = std::this_thread::get_id();
	for (unsigned int i = 0; i < pluginListNetworkThread.Size(); i++)
		pluginListNetworkThread[i]->UpdateNetworkThread();
	networkThreadDispatcher = std::thread::id();
	pluginListNetworkThreadMutex.Unlock();
}

bool RakPeer::DispatchNetworkThreadReceive(Packet *packet)
{
	pluginListNetworkThreadMutex.Lock();
	if (networkThreadReceiveStopped)
	{
		pluginListNetworkThreadMutex.Unlock();
		return false;
	}

	networkThreadDispatcher = std::this_thread::get_id();
	PluginReceiveResult result = RR_CONTINUE_PROCESSING;
	for (unsigned int i = 0; i < pluginListNetworkThread.Size() && result == RR_CONTINUE_PROCESSING; i++)
		result = pluginListNetworkThread[i]->OnReceiveNetworkThread(packet);
	networkThreadDispatcher = std::thread::id();
	pluginListNetworkThreadMutex.Unlock();

	if (result == RR_STOP_PROCESSING_AND_DEALLOCATE)
		DeallocatePacket(packet);
	return result != RR_CONTINUE_PROCESSING;
}

inline void RakPeer::AddPacketToProducer(MafiaNet::Packet *p)
{
	packetReturnMutex.Lock();
	packetReturnQueue.Push(p, _FILE_AND_LINE_);
	packetReturnCondition.notify_all();
	packetReturnMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
union Buff6AndBuff8
{
	unsigned char buff6[6];
	uint64_t buff8;
};
uint64_t RakPeerInterface::Get64BitUniqueRandomNumber(void)
{
	// Mac address is a poor solution because you can't have multiple connections from the same system


#if defined(_WIN32)
	// No timer-jitter harvesting: it slept eight times, and a Windows Sleep(1) lasts a whole timer
	// tick, so every peer took ~125 ms to construct. The OS entropy source and the counter below
	// are what make the value unique, as on every other platform.
	uint64_t g = MafiaNet::GetTimeUS();
	static std::atomic<uint64_t> sequence{0};
	std::random_device entropy;
	g ^= ((uint64_t) entropy() << 32) ^ (uint64_t) entropy();
	g ^= (sequence.fetch_add(1, std::memory_order_relaxed) + 1) * 0x9E3779B97F4A7C15ull;
	if (g == 0 || g == (uint64_t) -1)
		g = 1;
	return g;

#else
	// The time alone is not unique: two peers constructed within the same microsecond -- a server
	// and a client created back to back in one process -- got the same GUID, and a peer then
	// resolved the other's GUID to itself, so CloseConnection() and friends silently did nothing.
	// Mix in the OS entropy source and a per-process counter.
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	uint64_t g = (uint64_t) tv.tv_usec + (uint64_t) tv.tv_sec * 1000000;

	static std::atomic<uint64_t> sequence{0};
	std::random_device entropy;
	g ^= ((uint64_t) entropy() << 32) ^ (uint64_t) entropy();
	g ^= (sequence.fetch_add(1, std::memory_order_relaxed) + 1) * 0x9E3779B97F4A7C15ull;

	// Neither value is a usable GUID: 0 means "not generated" and all-ones is UNASSIGNED_RAKNET_GUID.
	if (g == 0 || g == (uint64_t) -1)
		g = 1;
	return g;
#endif
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GenerateGUID(void)
{
	myGuid.g = Get64BitUniqueRandomNumber();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// void MafiaNet::ProcessPortUnreachable( SystemAddress systemAddress, RakPeer *rakPeer )
// {
// 	(void) binaryAddress;
// 	(void) port;
// 	(void) rakPeer;
//
// }
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace MafiaNet
{
bool ProcessOfflineNetworkPacket(SystemAddress systemAddress, const char *data, const int length, RakPeer *rakPeer, RakNetSocket2 *rakNetSocket, bool *isOfflineMessage, MafiaNet::TimeUS timeRead)
{
	(void) timeRead;
	RakPeer::RemoteSystemStruct *remoteSystem;
	MafiaNet::Packet *packet;
	unsigned i;


	char str1[64];
	systemAddress.ToString(false, str1, static_cast<size_t>(64));
	if (rakPeer->IsBanned(str1))
	{
		for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
			rakPeer->pluginListNTS[i]->OnDirectSocketReceive(data, length * 8, systemAddress);

		MafiaNet::BitStream bs;
		bs.Write((MessageID) ID_CONNECTION_BANNED);
		bs.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
		bs.Write(rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));


		RNS2_SendParameters bsp;
		bsp.data = (char *) bs.GetData();
		bsp.length = bs.GetNumberOfBytesUsed();
		bsp.systemAddress = systemAddress;
		for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
			rakPeer->pluginListNTS[i]->OnDirectSocketSend((char *) bs.GetData(), bs.GetNumberOfBitsUsed(), systemAddress);
		rakNetSocket->Send(&bsp, _FILE_AND_LINE_);

		/*
		unsigned i;
		for (i=0; i < rakPeer->pluginListNTS.Size(); i++)
			rakPeer->pluginListNTS[i]->OnDirectSocketSend((char*) bs.GetData(), bs.GetNumberOfBitsUsed(), systemAddress);
		SocketLayer::SendTo( rakNetSocket, (char*) bs.GetData(), bs.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );
		*/

		return true;
	}


	// The reason for all this is that the reliability layer has no way to tell between offline messages that arrived late for a player that is now connected,
	// and a regular encoding. So I insert OFFLINE_MESSAGE_DATA_ID into the stream, the encoding of which is essentially impossible to hit by chance
	if (length <= 2)
	{
		*isOfflineMessage = true;
	}
	else if (
	    ((unsigned char) data[0] == ID_UNCONNECTED_PING ||
	     (unsigned char) data[0] == ID_UNCONNECTED_PING_OPEN_CONNECTIONS) &&
	    (size_t) length >= sizeof(unsigned char) + sizeof(MafiaNet::Time) + sizeof(OFFLINE_MESSAGE_DATA_ID))
	{
		*isOfflineMessage = memcmp(data + sizeof(unsigned char) + sizeof(MafiaNet::Time), OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID)) == 0;
	}
	else if ((unsigned char) data[0] == ID_UNCONNECTED_PONG && (size_t) length >= sizeof(unsigned char) + sizeof(MafiaNet::TimeMS) + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID))
	{
		*isOfflineMessage = memcmp(data + sizeof(unsigned char) + sizeof(MafiaNet::Time) + RakNetGUID::size(), OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID)) == 0;
	}
	else if (
	    (unsigned char) data[0] == ID_OUT_OF_BAND_INTERNAL &&
	    (size_t) length >= sizeof(MessageID) + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID))
	{
		*isOfflineMessage = memcmp(data + sizeof(MessageID) + RakNetGUID::size(), OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID)) == 0;
	}
	else if (
	    (
	        (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_1 ||
	        (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_2 ||
	        (unsigned char) data[0] == ID_OPEN_CONNECTION_REQUEST_1 ||
	        (unsigned char) data[0] == ID_OPEN_CONNECTION_REQUEST_2 ||
	        (unsigned char) data[0] == ID_CONNECTION_ATTEMPT_FAILED ||
	        (unsigned char) data[0] == ID_NO_FREE_INCOMING_CONNECTIONS ||
	        (unsigned char) data[0] == ID_CONNECTION_BANNED ||
	        (unsigned char) data[0] == ID_ALREADY_CONNECTED ||
	        (unsigned char) data[0] == ID_IP_RECENTLY_CONNECTED ||
	        (unsigned char) data[0] == ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION ||
	        (unsigned char) data[0] == ID_OUR_SYSTEM_REQUIRES_SECURITY) &&
	    (size_t) length >= sizeof(MessageID) + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID))
	{
		*isOfflineMessage = memcmp(data + sizeof(MessageID), OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID)) == 0;
	}
	else if (((unsigned char) data[0] == ID_INCOMPATIBLE_PROTOCOL_VERSION &&
	          (size_t) length == sizeof(MessageID) * 2 + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID)))
	{
		*isOfflineMessage = memcmp(data + sizeof(MessageID) * 2, OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID)) == 0;
	}
	else
	{
		*isOfflineMessage = false;
	}

	if (*isOfflineMessage)
	{
		for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
			rakPeer->pluginListNTS[i]->OnDirectSocketReceive(data, length * 8, systemAddress);

		// These are all messages from unconnected systems.  Messages here can be any size, but are never processed from connected systems.
		if (((unsigned char) data[0] == ID_UNCONNECTED_PING_OPEN_CONNECTIONS || (unsigned char) (data)[0] == ID_UNCONNECTED_PING) && (size_t) length >= sizeof(unsigned char) + sizeof(MafiaNet::Time) + sizeof(OFFLINE_MESSAGE_DATA_ID))
		{
			if ((unsigned char) (data)[0] == ID_UNCONNECTED_PING ||
			    rakPeer->AllowIncomingConnections()) // Open connections with players
			{
				MafiaNet::BitStream inBitStream((unsigned char *) data, length, false);
				inBitStream.IgnoreBits(8);
				MafiaNet::Time sendPingTime;
				inBitStream.Read(sendPingTime);
				inBitStream.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID));
				RakNetGUID remoteGuid = UNASSIGNED_RAKNET_GUID;
				inBitStream.Read(remoteGuid);

				MafiaNet::BitStream outBitStream;
				outBitStream.Write((MessageID) ID_UNCONNECTED_PONG); // Should be named ID_UNCONNECTED_PONG eventually
				outBitStream.Write(sendPingTime);
				outBitStream.Write(rakPeer->myGuid);
				outBitStream.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));

				rakPeer->rakPeerMutexes[RakPeer::offlinePingResponse_Mutex].Lock();
				// They are connected, so append offline ping data
				outBitStream.Write((char *) rakPeer->offlinePingResponse.GetData(), rakPeer->offlinePingResponse.GetNumberOfBytesUsed());
				rakPeer->rakPeerMutexes[RakPeer::offlinePingResponse_Mutex].Unlock();

				for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) outBitStream.GetData(), outBitStream.GetNumberOfBytesUsed(), systemAddress);

				RNS2_SendParameters bsp;
				bsp.data = (char *) outBitStream.GetData();
				bsp.length = outBitStream.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp, _FILE_AND_LINE_);

				// SocketLayer::SendTo( rakNetSocket, (const char*)outBitStream.GetData(), (unsigned int) outBitStream.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );

				packet = rakPeer->AllocPacket(sizeof(MessageID), _FILE_AND_LINE_);
				packet->data[0] = data[0];
				packet->systemAddress = systemAddress;
				packet->guid = remoteGuid;
				packet->systemAddress.systemIndex = (SystemIndex) rakPeer->GetIndexFromSystemAddress(systemAddress, true);
				packet->guid.systemIndex = packet->systemAddress.systemIndex;
				rakPeer->AddPacketToProducer(packet);
			}
		}
		// UNCONNECTED MESSAGE Pong with no data.
		else if ((unsigned char) data[0] == ID_UNCONNECTED_PONG && (size_t) length >= sizeof(unsigned char) + sizeof(MafiaNet::Time) + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID) && (size_t) length < sizeof(unsigned char) + sizeof(MafiaNet::Time) + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID) + MAX_OFFLINE_DATA_LENGTH)
		{
			packet = rakPeer->AllocPacket((unsigned int) (length - sizeof(OFFLINE_MESSAGE_DATA_ID) - RakNetGUID::size() - sizeof(MafiaNet::Time) + sizeof(MafiaNet::TimeMS)), _FILE_AND_LINE_);
			MafiaNet::BitStream bsIn((unsigned char *) data, length, false);
			bsIn.IgnoreBytes(sizeof(unsigned char));
			MafiaNet::Time ping;
			bsIn.Read(ping);
			bsIn.Read(packet->guid);

			MafiaNet::BitStream bsOut((unsigned char *) packet->data, packet->length, false);
			bsOut.ResetWritePointer();
			bsOut.Write((unsigned char) ID_UNCONNECTED_PONG);
			MafiaNet::TimeMS pingMS = (MafiaNet::TimeMS) ping;
			bsOut.Write(pingMS);
			bsOut.WriteAlignedBytes(
			    (const unsigned char *) data + sizeof(unsigned char) + sizeof(MafiaNet::Time) + RakNetGUID::size() + sizeof(OFFLINE_MESSAGE_DATA_ID),
			    length - sizeof(unsigned char) - sizeof(MafiaNet::Time) - RakNetGUID::size() - sizeof(OFFLINE_MESSAGE_DATA_ID));

			packet->systemAddress = systemAddress;
			packet->systemAddress.systemIndex = (SystemIndex) rakPeer->GetIndexFromSystemAddress(systemAddress, true);
			packet->guid.systemIndex = packet->systemAddress.systemIndex;
			rakPeer->AddPacketToProducer(packet);
		}
		else if ((unsigned char) data[0] == ID_OUT_OF_BAND_INTERNAL &&
		         (size_t) length > sizeof(OFFLINE_MESSAGE_DATA_ID) + sizeof(MessageID) + RakNetGUID::size() &&
		         (size_t) length < MAX_OFFLINE_DATA_LENGTH + sizeof(OFFLINE_MESSAGE_DATA_ID) + sizeof(MessageID) + RakNetGUID::size())
		{
			unsigned int dataLength = (unsigned int) (length - sizeof(OFFLINE_MESSAGE_DATA_ID) - RakNetGUID::size() - sizeof(MessageID));
			RakAssert(dataLength < 1024);
			packet = rakPeer->AllocPacket(dataLength + 1, _FILE_AND_LINE_);
			RakAssert(packet->length < 1024);

			MafiaNet::BitStream bs2((unsigned char *) data, length, false);
			bs2.IgnoreBytes(sizeof(MessageID));
			bs2.Read(packet->guid);

			if (data[sizeof(OFFLINE_MESSAGE_DATA_ID) + sizeof(MessageID) + RakNetGUID::size()] == ID_ADVERTISE_SYSTEM)
			{
				packet->length--;
				packet->bitSize = BYTES_TO_BITS(packet->length);
				packet->data[0] = ID_ADVERTISE_SYSTEM;
				memcpy(packet->data + 1, data + sizeof(OFFLINE_MESSAGE_DATA_ID) + sizeof(MessageID) * 2 + RakNetGUID::size(), dataLength - 1);
			}
			else
			{
				packet->data[0] = ID_OUT_OF_BAND_INTERNAL;
				memcpy(packet->data + 1, data + sizeof(OFFLINE_MESSAGE_DATA_ID) + sizeof(MessageID) + RakNetGUID::size(), dataLength);
			}

			packet->systemAddress = systemAddress;
			packet->systemAddress.systemIndex = (SystemIndex) rakPeer->GetIndexFromSystemAddress(systemAddress, true);
			packet->guid.systemIndex = packet->systemAddress.systemIndex;
			rakPeer->AddPacketToProducer(packet);
		}
		else if ((unsigned char) (data)[0] == (MessageID) ID_OPEN_CONNECTION_REPLY_1)
		{
			for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketReceive(data, length * 8, systemAddress);

			// ID_OPEN_CONNECTION_REPLY_1 (S->C), byte-aligned:
			//   MessageID (1) | OFFLINE_MESSAGE_DATA_ID (16) | server RakNetGUID (8) | challenge (uint64, 8) | MTU (uint16, 2)
			// The challenge is the server's stateless HandshakeChallenge for our address; we echo it unchanged.
			MafiaNet::BitStream bsIn((unsigned char *) data, length, false);
			bsIn.IgnoreBytes(sizeof(MessageID));
			bsIn.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID));
			RakNetGUID serverGuid;
			uint64_t challenge;
			uint16_t mtu;
			if (!bsIn.Read(serverGuid) || !bsIn.Read(challenge) || !bsIn.Read(mtu))
				return true; // malformed: drop

			// ID_OPEN_CONNECTION_REQUEST_2 (C->S), byte-aligned:
			//   MessageID (1) | OFFLINE_MESSAGE_DATA_ID (16) | challenge (uint64, 8) | client CryptInfo (41)
			//   | binding address (SystemAddress, the server's address as we see it) | MTU (uint16, 2) | client RakNetGUID (8)
			// CryptInfo (CryptInfo::Serialize) = mode (1: 0 Required, 1 Plaintext) | X25519 public key (32) | nonce (uint64 LE, 8).
			// In mode Plaintext the public key is zero. The client's nonce is random in both modes: REPLY_2 echoes it.
			MafiaNet::BitStream bsOut;
			bsOut.Write((MessageID) ID_OPEN_CONNECTION_REQUEST_2);
			bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
			bsOut.Write(challenge);

			rakPeer->requestedConnectionQueueMutex.Lock();
			for (i = 0; i < rakPeer->requestedConnectionQueue.Size(); i++)
			{
				RakPeer::RequestedConnectionStruct *rcs;
				rcs = rakPeer->requestedConnectionQueue[i];
				if (rcs->systemAddress == systemAddress && !rcs->cancelled)
				{
					// Happy Eyeballs: the first candidate to answer wins the race.
					// Every path below returns, so the shifted indices do not matter.
					rakPeer->CancelRaceSiblings_NoLock(rcs);

					// Every REQUEST_2 of one request carries the same crypt info: the server answers a
					// retransmission with the crypt info it derived the session keys from.
					CryptInfo clientInfo;
					if (rcs->ephemeral != nullptr && !rcs->ephemeral->IsWiped())
						clientInfo = rcs->ephemeral->MakeCryptInfo();
					else if (rakPeer->encryptionMode == EncryptionMode::Plaintext)
					{
						clientInfo.mode = EncryptionMode::Plaintext;
						clientInfo.nonce = rcs->handshakeNonce;
					}
					else
					{
						// Required, but this request's key pair is gone: never fall back to plaintext.
						rakPeer->requestedConnectionQueueMutex.Unlock();
						return true;
					}
					clientInfo.Serialize(&bsOut);

					// Binding address
					bsOut.Write(rcs->systemAddress);
					rakPeer->requestedConnectionQueueMutex.Unlock();
					// MTU
					bsOut.Write(mtu);
					// Our guid
					bsOut.Write(rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));

					for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
						rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);

					RNS2_SendParameters bsp;
					bsp.data = (char *) bsOut.GetData();
					bsp.length = bsOut.GetNumberOfBytesUsed();
					bsp.systemAddress = systemAddress;
					rakNetSocket->Send(&bsp, _FILE_AND_LINE_);

					return true;
				}
			}
			rakPeer->requestedConnectionQueueMutex.Unlock();
		}
		else if ((unsigned char) (data)[0] == (MessageID) ID_OPEN_CONNECTION_REPLY_2)
		{
			for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketReceive(data, length * 8, systemAddress);

			// ID_OPEN_CONNECTION_REPLY_2 (S->C), byte-aligned:
			//   MessageID (1) | OFFLINE_MESSAGE_DATA_ID (16) | echo of our CryptInfo nonce (uint64, 8) | server RakNetGUID (8)
			//   | our address as the server sees it (SystemAddress) | MTU (uint16, 2) | server CryptInfo (41)
			MafiaNet::BitStream bs((unsigned char *) data, length, false);
			bs.IgnoreBytes(sizeof(MessageID));
			bs.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID));
			uint64_t nonceEcho;
			RakNetGUID guid;
			SystemAddress bindingAddress;
			uint16_t mtu;
			CryptInfo serverInfo;
			if (!bs.Read(nonceEcho) || !bs.Read(guid) || !bs.Read(bindingAddress) || !bs.Read(mtu) || !serverInfo.Deserialize(&bs))
				return true; // malformed: drop

			RakPeer::RequestedConnectionStruct *rcs;
			bool unlock = true;
			rakPeer->requestedConnectionQueueMutex.Lock();
			for (i = 0; i < rakPeer->requestedConnectionQueue.Size(); i++)
			{
				rcs = rakPeer->requestedConnectionQueue[i];


				if (rcs->systemAddress == systemAddress && !rcs->cancelled)
				{
					// Before anything else: a REPLY_2 that does not echo this attempt's nonce was not sent in answer to
					// it (an off-path forgery has to guess 64 bits). Ignore it; the attempt carries on.
					if (nonceEcho != rcs->handshakeNonce)
						break;

					RakAssert(rcs->actionToTake == RakPeer::RequestedConnectionStruct::CONNECT);
					// A failed attempt is reported once, under the race's first address, and its request dropped
					// so it is not retried.
					const SystemAddress reportedAddress = rcs->raceGroup != 0 ? rcs->raceFirstAddress : rcs->systemAddress;

					// Both sides must agree on the mode; there is no fallback (GNS refuses on BadCrypt). The server
					// refuses a mismatch itself, so this only catches a server that answered anyway.
					if (serverInfo.mode != rakPeer->encryptionMode)
					{
						rakPeer->RemoveRequestedConnection_NoLock(rcs);
						rakPeer->requestedConnectionQueueMutex.Unlock();
						rakPeer->DeleteRequestedConnection(rcs);

						packet = rakPeer->AllocPacket(sizeof(char), _FILE_AND_LINE_);
						packet->data[0] = rakPeer->encryptionMode == EncryptionMode::Plaintext ? (MessageID) ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION : (MessageID) ID_OUR_SYSTEM_REQUIRES_SECURITY;
						packet->bitSize = (sizeof(char) * 8);
						packet->systemAddress = reportedAddress;
						packet->guid = guid;
						rakPeer->AddPacketToProducer(packet);
						return true;
					}

					// You might get this when already connected because of cross-connections
					remoteSystem = rakPeer->GetRemoteSystemFromSystemAddress(systemAddress, true, true);
					if (remoteSystem != 0 && remoteSystem->reliabilityLayer.IsEncrypted())
					{
						// Session keys are already installed for this address while this attempt is still pending. The
						// cross-connection tie-break (the REQUEST_2 handler) only lets that happen when we answered the
						// other peer's attempt because its GUID is lower: those keys belong to the winning handshake.
						// Any REPLY_2 to our own attempt -- stale, duplicated or forged -- is ignored: nothing is derived
						// from it, the installed keys are never replaced (a second SetSessionKeys would restart the
						// sequence on one side only), and we do not send ID_CONNECTION_REQUEST. The attempt stays queued
						// and is dropped once the winner's ID_CONNECTION_REQUEST opens, or ends as an attempt normally does.
						rakPeer->requestedConnectionQueueMutex.Unlock();
						return true;
					}

					// Derive under the lock: rcs->ephemeral belongs to the queue. DeriveSessionKeys wipes it.
					SessionKeys keys;
					CryptInfo clientInfo;
					if (rakPeer->encryptionMode == EncryptionMode::Required)
					{
						const RakNetGUID myGuid = rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
						bool derived = false;
						if (rcs->ephemeral != nullptr && !rcs->ephemeral->IsWiped())
						{
							clientInfo = rcs->ephemeral->MakeCryptInfo();
							derived = DeriveSessionKeys(*rcs->ephemeral, /*localIsInitiator=*/true, clientInfo, serverInfo, myGuid, guid, &keys);
						}
						if (!derived)
						{
							// A low-order server key (or no key pair left): this attempt cannot be encrypted.
							rakPeer->RemoveRequestedConnection_NoLock(rcs);
							rakPeer->requestedConnectionQueueMutex.Unlock();
							rakPeer->DeleteRequestedConnection(rcs);

							packet = rakPeer->AllocPacket(sizeof(char), _FILE_AND_LINE_);
							packet->data[0] = ID_CONNECTION_ATTEMPT_FAILED;
							packet->bitSize = (sizeof(char) * 8);
							packet->systemAddress = reportedAddress;
							packet->guid = guid;
							rakPeer->AddPacketToProducer(packet);
							return true;
						}
					}
					else
					{
						clientInfo.mode = EncryptionMode::Plaintext;
						clientInfo.nonce = rcs->handshakeNonce;
					}
					const SessionKeys *sessionKeys = rakPeer->encryptionMode == EncryptionMode::Required ? &keys : nullptr;

					rakPeer->requestedConnectionQueueMutex.Unlock();
					unlock = false;

					bool thisIPConnectedRecently = false;
					if (remoteSystem == 0)
					{
						if (rcs->socket == 0)
						{
							remoteSystem = rakPeer->AssignSystemAddressToRemoteSystemList(systemAddress, RakPeer::RemoteSystemStruct::UNVERIFIED_SENDER, rakNetSocket, &thisIPConnectedRecently, bindingAddress, mtu, guid, sessionKeys, clientInfo, serverInfo);
						}
						else
						{
							remoteSystem = rakPeer->AssignSystemAddressToRemoteSystemList(systemAddress, RakPeer::RemoteSystemStruct::UNVERIFIED_SENDER, rcs->socket, &thisIPConnectedRecently, bindingAddress, mtu, guid, sessionKeys, clientInfo, serverInfo);
						}
					}
					keys.Wipe();

					// 4/13/09 Attackers can flood ID_OPEN_CONNECTION_REQUEST and use up all available connection slots
					// Ignore connection attempts if this IP address connected within the last 100 milliseconds
					if (thisIPConnectedRecently == false)
					{
						// Don't check GetRemoteSystemFromGUID, server will verify
						if (remoteSystem)
						{
							remoteSystem->weInitiatedTheConnection = true;
							remoteSystem->connectMode = RakPeer::RemoteSystemStruct::REQUESTED_CONNECTION;
							// The request, and with it the race's first address, is freed below: keep that address with
							// the connection, so GetConnectionState() still answers for it.
							if (rcs->raceGroup != 0 && rcs->raceFirstAddress != systemAddress)
								remoteSystem->raceFirstAddress = rcs->raceFirstAddress;
							if (rcs->timeoutTime != 0)
								remoteSystem->reliabilityLayer.SetTimeoutTime(rcs->timeoutTime);

							// ID_CONNECTION_REQUEST: id | our guid | timestamp | reserved (1, always 0) | password.
							// The first datagram sealed with the new keys: a server that derived different keys
							// cannot open it, so the connection never completes (implicit key confirmation, as in GNS).
							MafiaNet::BitStream temp;
							temp.Write((MessageID) ID_CONNECTION_REQUEST);
							temp.Write(rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));
							temp.Write(MafiaNet::GetTime());
							temp.Write((unsigned char) 0);

							if (rcs->outgoingPasswordLength > 0)
								temp.Write((char *) rcs->outgoingPassword, rcs->outgoingPasswordLength);

							rakPeer->SendImmediate((char *) temp.GetData(), temp.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::Reliable, 0, systemAddress, false, false, timeRead, 0);
						}
						else
						{
							// Failed, no connections available anymore
							packet = rakPeer->AllocPacket(sizeof(char), _FILE_AND_LINE_);
							packet->data[0] = ID_CONNECTION_ATTEMPT_FAILED; // Attempted a connection and couldn't
							packet->bitSize = (sizeof(char) * 8);
							packet->systemAddress = reportedAddress;
							packet->guid = guid;
							rakPeer->AddPacketToProducer(packet);
						}
					}

					rakPeer->requestedConnectionQueueMutex.Lock();
					rakPeer->RemoveRequestedConnection_NoLock(rcs);
					rakPeer->requestedConnectionQueueMutex.Unlock();

					rakPeer->DeleteRequestedConnection(rcs);

					break;
				}
			}

			if (unlock)
				rakPeer->requestedConnectionQueueMutex.Unlock();

			return true;
		}
		else if ((unsigned char) (data)[0] == (MessageID) ID_CONNECTION_ATTEMPT_FAILED ||
		         (unsigned char) (data)[0] == (MessageID) ID_NO_FREE_INCOMING_CONNECTIONS ||
		         (unsigned char) (data)[0] == (MessageID) ID_CONNECTION_BANNED ||
		         (unsigned char) (data)[0] == (MessageID) ID_ALREADY_CONNECTED ||
		         (unsigned char) (data)[0] == (MessageID) ID_INVALID_PASSWORD ||
		         (unsigned char) (data)[0] == (MessageID) ID_IP_RECENTLY_CONNECTED ||
		         (unsigned char) (data)[0] == (MessageID) ID_INCOMPATIBLE_PROTOCOL_VERSION ||
		         (unsigned char) (data)[0] == (MessageID) ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION ||
		         (unsigned char) (data)[0] == (MessageID) ID_OUR_SYSTEM_REQUIRES_SECURITY)
		{

			MafiaNet::BitStream bs((unsigned char *) data, length, false);
			bs.IgnoreBytes(sizeof(MessageID));
			bs.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID));
			if ((unsigned char) (data)[0] == (MessageID) ID_INCOMPATIBLE_PROTOCOL_VERSION)
				bs.IgnoreBytes(sizeof(unsigned char));

			RakNetGUID guid;
			bs.Read(guid);

			RakPeer::RequestedConnectionStruct *rcs;
			bool connectionAttemptCancelled = false;
			SystemAddress reportedAddress = systemAddress;
			rakPeer->requestedConnectionQueueMutex.Lock();
			for (i = 0; i < rakPeer->requestedConnectionQueue.Size(); i++)
			{
				rcs = rakPeer->requestedConnectionQueue[i];
				if (rcs->actionToTake == RakPeer::RequestedConnectionStruct::CONNECT && rcs->systemAddress == systemAddress && !rcs->cancelled)
				{
					connectionAttemptCancelled = true;
					// A rejection from the server ends a Happy Eyeballs race: report
					// it once, under the race's first address, and drop the rest.
					if (rcs->raceGroup != 0)
					{
						reportedAddress = rcs->raceFirstAddress;
						rakPeer->CancelRaceSiblings_NoLock(rcs);
						for (i = 0; i < rakPeer->requestedConnectionQueue.Size(); i++)
						{
							if (rakPeer->requestedConnectionQueue[i] == rcs)
								break;
						}
					}
					rakPeer->requestedConnectionQueue.RemoveAtIndex(i);
					rakPeer->DeleteRequestedConnection(rcs);
					break;
				}
			}

			rakPeer->requestedConnectionQueueMutex.Unlock();

			if (connectionAttemptCancelled)
			{
				// Tell user of connection attempt failed
				packet = rakPeer->AllocPacket(sizeof(char), _FILE_AND_LINE_);
				packet->data[0] = data[0]; // Attempted a connection and couldn't
				packet->bitSize = (sizeof(char) * 8);
				packet->systemAddress = reportedAddress;
				packet->guid = guid;
				rakPeer->AddPacketToProducer(packet);
			}
		}
		else if ((unsigned char) (data)[0] == ID_OPEN_CONNECTION_REQUEST_1 && length >= OPEN_CONNECTION_REPLY_1_BYTES)
		{ /*
			static int x = 0;
			++x;

			SystemAddress *addr = (SystemAddress*)&systemAddress;
			addr->binaryAddress += x;*/

			//RAKNET_DEBUG_PRINTF("%i:IOCR, ", __LINE__);
			char remoteProtocol = data[1 + sizeof(OFFLINE_MESSAGE_DATA_ID)];
			if (remoteProtocol != RAKNET_PROTOCOL_VERSION)
			{
				MafiaNet::BitStream bs;
				bs.Write((MessageID) ID_INCOMPATIBLE_PROTOCOL_VERSION);
				bs.Write((unsigned char) RAKNET_PROTOCOL_VERSION);
				bs.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
				bs.Write(rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));

				for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((char *) bs.GetData(), bs.GetNumberOfBitsUsed(), systemAddress);

				// SocketLayer::SendTo( rakNetSocket, (char*)bs.GetData(), bs.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );

				RNS2_SendParameters bsp;
				bsp.data = (char *) bs.GetData();
				bsp.length = bs.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;

				rakNetSocket->Send(&bsp, _FILE_AND_LINE_);
				return true;
			}

			for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketReceive(data, length * 8, systemAddress);

			// ID_OPEN_CONNECTION_REPLY_1 (S->C): see the receiving side above for the layout. The challenge is
			// stateless, so answering allocates nothing; REQUEST_2 must echo it within HandshakeChallenge's window.
			MafiaNet::BitStream bsOut;
			bsOut.Write((MessageID) ID_OPEN_CONNECTION_REPLY_1);
			bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
			bsOut.Write(rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));
			bsOut.Write(rakPeer->handshakeChallenge.Generate(MafiaNet::GetTimeUS(), systemAddress));

			// MTU. Lower MTU if it is exceeds our own limit
			if (length + UDP_HEADER_SIZE > MAXIMUM_MTU_SIZE)
				bsOut.WriteCasted<uint16_t>(MAXIMUM_MTU_SIZE);
			else
				bsOut.WriteCasted<uint16_t>(length + UDP_HEADER_SIZE);

			for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
			// SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );

			RNS2_SendParameters bsp;
			bsp.data = (char *) bsOut.GetData();
			bsp.length = bsOut.GetNumberOfBytesUsed();
			bsp.systemAddress = systemAddress;
			rakNetSocket->Send(&bsp, _FILE_AND_LINE_);
		}
		else if ((unsigned char) (data)[0] == ID_OPEN_CONNECTION_REQUEST_2)
		{
			// ID_OPEN_CONNECTION_REQUEST_2 (C->S): see the sending side (the REPLY_1 handler) for the layout.
			MafiaNet::BitStream bsOut;
			MafiaNet::BitStream bs((unsigned char *) data, length, false);
			bs.IgnoreBytes(sizeof(MessageID));
			bs.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID));

			// The challenge comes first and is checked before anything else: until it verifies, the sender has not
			// shown it can receive at the address it claims, and nothing may be allocated or derived for it.
			uint64_t challenge;
			if (!bs.Read(challenge) || !rakPeer->handshakeChallenge.Verify(challenge, MafiaNet::GetTimeUS(), systemAddress))
				return true;

			CryptInfo clientInfo;
			SystemAddress bindingAddress;
			uint16_t mtu;
			RakNetGUID guid;
			if (!clientInfo.Deserialize(&bs) || !bs.Read(bindingAddress) || !bs.Read(mtu) || !bs.Read(guid))
				return true; // malformed: drop

			// Both sides must use the same mode; there is no fallback. Refuse without allocating anything, and
			// tell the client from its own point of view, the way ID_ALREADY_CONNECTED is sent:
			//   MessageID (1) | OFFLINE_MESSAGE_DATA_ID (16) | server RakNetGUID (8)
			// We require encryption and the client does not: ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION.
			// We are plaintext and the client requires encryption: ID_OUR_SYSTEM_REQUIRES_SECURITY.
			if (clientInfo.mode != rakPeer->encryptionMode)
			{
				bsOut.Write(rakPeer->encryptionMode == EncryptionMode::Required ? (MessageID) ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION : (MessageID) ID_OUR_SYSTEM_REQUIRES_SECURITY);
				bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
				bsOut.Write(rakPeer->myGuid);
				for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp, _FILE_AND_LINE_);
				return true;
			}

			RakPeer::RemoteSystemStruct *rssFromSA = rakPeer->GetRemoteSystemFromSystemAddress(systemAddress, true, true);
			bool IPAddrInUse = rssFromSA != 0 && rssFromSA->isActive;
			RakPeer::RemoteSystemStruct *rssFromGuid = rakPeer->GetRemoteSystemFromGUID(guid, true);
			bool GUIDInUse = rssFromGuid != 0 && rssFromGuid->isActive;

			// IPAddrInUse, GuidInUse, outcome
			// TRUE,	  , TRUE	 , ID_OPEN_CONNECTION_REPLY if they are the same, else ID_ALREADY_CONNECTED
			// FALSE,     , TRUE     , ID_ALREADY_CONNECTED (someone else took this guid)
			// TRUE,	  , FALSE	 , ID_ALREADY_CONNECTED (silently disconnected, restarted rakNet)
			// FALSE	  , FALSE	 , Allow connection

			int outcome;
			if (IPAddrInUse & GUIDInUse)
			{
				if (rssFromSA == rssFromGuid && rssFromSA->connectMode == RakPeer::RemoteSystemStruct::UNVERIFIED_SENDER)
				{
					// ID_OPEN_CONNECTION_REPLY if they are the same
					outcome = 1;

					// Note to self: If REQUESTED_CONNECTION, this means two systems attempted to connect to each other at the same time, and one finished first.
					// Returns ID)_CONNECTION_REQUEST_ACCEPTED to one system, and ID_ALREADY_CONNECTED followed by ID_NEW_INCOMING_CONNECTION to another
				}
				else
				{
					// ID_ALREADY_CONNECTED (restarted raknet, connected again from same ip, plus someone else took this guid)
					outcome = 2;
				}
			}
			else if (IPAddrInUse == false && GUIDInUse == true)
			{
				// ID_ALREADY_CONNECTED (someone else took this guid)
				outcome = 3;
			}
			else if (IPAddrInUse == true && GUIDInUse == false)
			{
				// ID_ALREADY_CONNECTED (silently disconnected, restarted rakNet)
				outcome = 4;
			}
			else
			{
				// Allow connection
				outcome = 0;
			}

			// ID_OPEN_CONNECTION_REPLY_2 (S->C): see the receiving side (the REPLY_2 handler) for the layout.
			// The server's crypt info goes last, once it is known.
			MafiaNet::BitStream bsAnswer;
			bsAnswer.Write((MessageID) ID_OPEN_CONNECTION_REPLY_2);
			bsAnswer.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
			bsAnswer.Write(clientInfo.nonce); // the client drops a REPLY_2 that does not echo its nonce
			bsAnswer.Write(rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));
			bsAnswer.Write(systemAddress);
			bsAnswer.Write(mtu);

			if (outcome == 1)
			{
				// Duplicate connection request packet from packetloss: send back the same answer, with the crypt
				// info the session keys were derived from. A different client crypt info under the same address
				// and GUID is not a retransmission of this handshake: drop it.
				uint8_t received[CryptInfo::kSerializedBytes];
				uint8_t expected[CryptInfo::kSerializedBytes];
				clientInfo.SerializeTo(received);
				rssFromSA->handshakeRemoteCryptInfo.SerializeTo(expected);
				if (sodium_memcmp(received, expected, sizeof(received)) != 0)
					return true;
				rssFromSA->handshakeLocalCryptInfo.Serialize(&bsAnswer);

				for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsAnswer.GetData(), bsAnswer.GetNumberOfBitsUsed(), systemAddress);
				// SocketLayer::SendTo( rakNetSocket, (const char*) bsAnswer.GetData(), bsAnswer.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );

				RNS2_SendParameters bsp;
				bsp.data = (char *) bsAnswer.GetData();
				bsp.length = bsAnswer.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp, _FILE_AND_LINE_);

				return true;
			}
			else if (outcome != 0)
			{
				bsOut.Write((MessageID) ID_ALREADY_CONNECTED);
				bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
				bsOut.Write(rakPeer->myGuid);
				for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				// SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );
				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp, _FILE_AND_LINE_);

				return true;
			}

			if (rakPeer->AllowIncomingConnection(systemAddress) == false)
			{
				// Cross-connection: a peer with a lower GUID does not answer our REQUEST_2 while its own attempt to us
				// is pending (see the tie-break below). This refusal ends that attempt, so retry ours right away
				// instead of after a whole retry interval.
				if (rakPeer->encryptionMode == EncryptionMode::Required && guid.g < rakPeer->myGuid.g)
				{
					rakPeer->requestedConnectionQueueMutex.Lock();
					for (i = 0; i < rakPeer->requestedConnectionQueue.Size(); i++)
					{
						RakPeer::RequestedConnectionStruct *rcs = rakPeer->requestedConnectionQueue[i];
						if (rcs->systemAddress == systemAddress && !rcs->cancelled)
							rcs->nextRequestTime = 0;
					}
					rakPeer->requestedConnectionQueueMutex.Unlock();
				}
				bsOut.Write((MessageID) ID_NO_FREE_INCOMING_CONNECTIONS);
				bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
				bsOut.Write(rakPeer->myGuid);
				for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				//SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );
				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp, _FILE_AND_LINE_);

				return true;
			}

			// Cross-connection tie-break. When two peers connect to each other at once, each would answer the other's
			// REQUEST_2 and install keys from the other's handshake, and the two sides would end up holding keys
			// from different handshakes. The handshake initiated by the peer with the lower RakNetGUID wins on both
			// sides. So if our own attempt to this address is pending and our GUID is lower, do not answer: the
			// losing handshake then never derives keys and nothing can be sealed under them. The other peer retries;
			// once our handshake completes the retry gets ID_ALREADY_CONNECTED, and if ours fails it is answered
			// normally. If our GUID is higher, answer as usual: the keys installed below are the winner's, a REPLY_2
			// to our own attempt is ignored (the REPLY_2 handler), and the attempt is dropped once the winner's
			// ID_CONNECTION_REQUEST opens under these keys (RunUpdateCycle, UNVERIFIED_SENDER).
			// Plaintext has no keys to agree on and keeps converging as it always did.
			// The queue lock is held until the connection is allocated: Connect() looks for a connection under the
			// same lock, so an attempt it starts meanwhile is either refused there or seen here.
			rakPeer->requestedConnectionQueueMutex.Lock();
			if (rakPeer->encryptionMode == EncryptionMode::Required && rakPeer->myGuid.g < guid.g)
			{
				for (i = 0; i < rakPeer->requestedConnectionQueue.Size(); i++)
				{
					const RakPeer::RequestedConnectionStruct *rcs = rakPeer->requestedConnectionQueue[i];
					if (rcs->systemAddress == systemAddress && !rcs->cancelled && rcs->actionToTake == RakPeer::RequestedConnectionStruct::CONNECT)
					{
						rakPeer->requestedConnectionQueueMutex.Unlock();
						return true;
					}
				}
			}

			// The cheap refusals all come before the key exchange. The last one: SetLimitIPConnectionFrequency's
			// "connected recently from this IP", which AssignSystemAddressToRemoteSystemList also applies. Checked
			// here first, with the same rule and clock, so a flood from one IP costs no X25519.
			bool thisIPConnectedRecently = rakPeer->IPConnectedRecently(systemAddress, MafiaNet::GetTimeMS());

			// Our side of the handshake: a fresh ephemeral key pair, used once and wiped by DeriveSessionKeys
			// (and by its destructor) before this block ends. The client initiated, so it holds the initiator slot.
			SessionKeys keys;
			CryptInfo serverInfo;
			if (thisIPConnectedRecently)
				rssFromSA = 0;
			else if (rakPeer->encryptionMode == EncryptionMode::Required)
			{
				EphemeralKeyPair serverPair;
				serverInfo = serverPair.MakeCryptInfo();
				rakPeer->serverKeyExchangeCount++;
				if (!DeriveSessionKeys(serverPair, /*localIsInitiator=*/false, clientInfo, serverInfo, guid, rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS), &keys))
				{
					rakPeer->requestedConnectionQueueMutex.Unlock();
					return true; // low-order client key: drop, allocating nothing
				}
			}
			else
				serverInfo.mode = EncryptionMode::Plaintext;

			if (!thisIPConnectedRecently)
				rssFromSA = rakPeer->AssignSystemAddressToRemoteSystemList(systemAddress, RakPeer::RemoteSystemStruct::UNVERIFIED_SENDER, rakNetSocket, &thisIPConnectedRecently, bindingAddress, mtu, guid, rakPeer->encryptionMode == EncryptionMode::Required ? &keys : nullptr, serverInfo, clientInfo);
			rakPeer->requestedConnectionQueueMutex.Unlock();
			keys.Wipe();

			if (thisIPConnectedRecently == true)
			{
				bsOut.Write((MessageID) ID_IP_RECENTLY_CONNECTED);
				bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
				bsOut.Write(rakPeer->myGuid);
				for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				//SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );

				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp, _FILE_AND_LINE_);

				return true;
			}

			if (rssFromSA == 0)
				return true;

			serverInfo.Serialize(&bsAnswer);

			for (i = 0; i < rakPeer->pluginListNTS.Size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsAnswer.GetData(), bsAnswer.GetNumberOfBitsUsed(), systemAddress);
			// SocketLayer::SendTo( rakNetSocket, (const char*) bsAnswer.GetData(), bsAnswer.GetNumberOfBytesUsed(), systemAddress, _FILE_AND_LINE_ );
			RNS2_SendParameters bsp;
			bsp.data = (char *) bsAnswer.GetData();
			bsp.length = bsAnswer.GetNumberOfBytesUsed();
			bsp.systemAddress = systemAddress;
			rakNetSocket->Send(&bsp, _FILE_AND_LINE_);
		}
		return true;
	}

	return false;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// #med - deprecate this overload --- socketList[0] is not really ensured to be the correctly bound socket at all!
void ProcessNetworkPacket(SystemAddress systemAddress, const char *data, const int length, RakPeer *rakPeer, MafiaNet::TimeUS timeRead, BitStream &updateBitStream)
{
	ProcessNetworkPacket(systemAddress, data, length, rakPeer, rakPeer->socketList[0], timeRead, updateBitStream);
}
void ProcessNetworkPacket(SystemAddress systemAddress, const char *data, const int length, RakPeer *rakPeer, RakNetSocket2 *rakNetSocket, MafiaNet::TimeUS timeRead, BitStream &updateBitStream)
{
	RakAssert(systemAddress.GetPort());
	bool isOfflineMessage;
	if (ProcessOfflineNetworkPacket(systemAddress, data, length, rakPeer, rakNetSocket, &isOfflineMessage, timeRead))
	{
		return;
	}

	//	MafiaNet::Packet *packet;
	RakPeer::RemoteSystemStruct *remoteSystem;

	// See if this datagram came from a connected system
	remoteSystem = rakPeer->GetRemoteSystemFromSystemAddress(systemAddress, true, true);
	if (remoteSystem)
	{
		// Handle regular incoming data
		// HandleSocketReceiveFromConnectedPlayer is only safe to be called from the same thread as Update, which is this thread
		if (isOfflineMessage == false)
		{
			remoteSystem->reliabilityLayer.HandleSocketReceiveFromConnectedPlayer(
			    data, length, systemAddress, rakPeer->pluginListNTS, remoteSystem->MTUSize,
			    rakNetSocket, &rnr, timeRead, updateBitStream);
		}
	}
	else
	{
		// int a=5;
		// printf("--- Packet from unknown system %s\n", systemAddress.ToString());
	}
}

}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GenerateSeedFromGuid(void)
{
	/*
	// Construct a random seed based on the initial guid value, and the last digits of the difference to each subsequent number
	// This assumes that only the last 3 bits of each guidId integer has a meaningful amount of randomness between it and the prior number
	unsigned int t = guid.g[0];
	unsigned int i;
	for (i=1; i < sizeof(guid.g) / sizeof(guid.g[0]); i++)
	{
		unsigned int diff = guid.g[i]-guid.g[i-1];
		unsigned int diff3Bits = diff & 0x0007;
		diff3Bits <<= 29;
		diff3Bits >>= (i-1)*3;
		t ^= diff3Bits;
	}

	return t;
	*/
	return (unsigned int) ((myGuid.g >> 32) ^ myGuid.g);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::DerefAllSockets(void)
{
	unsigned int i;
	for (i = 0; i < socketList.Size(); i++)
	{
		if (socketFactory && !socketList[i]->IsBerkleySocket())
			socketFactory->Destroy(socketList[i]);
		else
			MafiaNet::OP_DELETE(socketList[i], _FILE_AND_LINE_);
	}
	socketList.Clear(false, _FILE_AND_LINE_);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetRakNetSocketFromUserConnectionSocketIndex(unsigned int userIndex) const
{
	unsigned int i;
	for (i = 0; i < socketList.Size(); i++)
	{
		if (socketList[i]->GetUserConnectionSocketIndex() == userIndex)
			return i;
	}
	RakAssert("GetRakNetSocketFromUserConnectionSocketIndex failed" && 0);
	return (unsigned int) -1;
}

/*
// DS_APR
void RakPeer::ProcessChromePacket(RakNetSocket2 *s, const char *buffer, int dataSize, const SystemAddress& recvFromAddress, MafiaNet::TimeUS timeRead)
{
	RakAssert(buffer);
	RakAssert(dataSize > 0);
	RakAssert(recvFromAddress.GetPort());

	RNS2RecvStruct *recvFromStruct;
	recvFromStruct=bufferedPackets.Allocate( _FILE_AND_LINE_ );
	RakAssert(dataSize <= (int)sizeof(recvFromStruct->data));
	memcpy(recvFromStruct->data, buffer, dataSize);
	recvFromStruct->bytesRead=dataSize;
	recvFromStruct->systemAddress=recvFromAddress;
	recvFromStruct->timeRead=timeRead;
	bufferedPackets.Push(recvFromStruct);
}
*/

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/*
bool RakPeer::RunRecvFromOnce( RakNetSocket *s )
{
	RakPeer::RecvFromStruct *recvFromStruct;

	recvFromStruct=bufferedPackets.Allocate( _FILE_AND_LINE_ );
	if (recvFromStruct != nullptr)
	{
		recvFromStruct->s=s;
		SocketLayer::RecvFromBlocking(s, this, recvFromStruct->data, &recvFromStruct->bytesRead, &recvFromStruct->systemAddress, &recvFromStruct->timeRead);

		if (recvFromStruct->bytesRead>0)
		{
			RakAssert(recvFromStruct->systemAddress.GetPort());
			bufferedPackets.Push(recvFromStruct);
			quitAndDataEvents.SetEvent();

			// Got data
			return true;
		}
		else
		{
			bufferedPackets.Deallocate(recvFromStruct, _FILE_AND_LINE_);
		}
	}
	// No data
	return false;
}
*/
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::RunUpdateCycle(BitStream &updateBitStream)
{
	RakPeer::RemoteSystemStruct *remoteSystem;
	unsigned int activeSystemListIndex;
	Packet *packet;
	// int currentSentBytes,currentReceivedBytes;
	//	unsigned numberOfBytesUsed;
	//	BitSize_t numberOfBitsUsed;
	//SystemAddress authoritativeClientSystemAddress;
	BitSize_t bitSize;
	unsigned int byteSize;
	unsigned char *data;
	SystemAddress systemAddress;
	BufferedCommandStruct *bcs;
	bool callerDataAllocationUsed;
	RakNetStatistics *rnss;
	MafiaNet::TimeUS timeNS = 0;
	MafiaNet::Time timeMS = 0;

	// This is here so RecvFromBlocking actually gets data from the same thread

#if defined(_WIN32)
	// #high - review this - this is hardly correct to only work with the first socket here...
	if (socketList[0]->GetSocketType() == RNS2T_WINDOWS && ((RNS2_Windows *) socketList[0])->GetSocketLayerOverride())
	{
		int len;
		SystemAddress sender;
		char dataOut[MAXIMUM_MTU_SIZE];
		do
		{
			len = ((RNS2_Windows *) socketList[0])->GetSocketLayerOverride()->RakNetRecvFrom(dataOut, &sender, true);
			if (len > 0)
				ProcessNetworkPacket(sender, dataOut, len, this, socketList[0], MafiaNet::GetTimeUS(), updateBitStream);
		} while (len > 0);
	}
#endif

	//	unsigned int socketListIndex;
	RNS2RecvStruct *recvFromStruct;
	while ((recvFromStruct = PopBufferedPacket()) != 0)
	{
		/*
		for (socketListIndex=0; socketListIndex < socketList.Size(); socketListIndex++)
		{
			if ((RakNetSocket*) socketList[socketListIndex]==recvFromStruct->s)
				break;
		}
		if (socketListIndex!=socketList.Size())
		*/
		ProcessNetworkPacket(recvFromStruct->systemAddress, recvFromStruct->data, recvFromStruct->bytesRead, this, recvFromStruct->socket, recvFromStruct->timeRead, updateBitStream);
		DeallocRNS2RecvStruct(recvFromStruct, _FILE_AND_LINE_);
	}

	// Before the send queue is drained, so whatever the plugins send here leaves this cycle.
	DispatchNetworkThreadUpdate();

	while ((bcs = bufferedCommands.PopInaccurate()) != 0)
	{
		if (bcs->command == BufferedCommandStruct::BCS_SEND)
		{
			// GetTime is a very slow call so do it once and as late as possible
			if (timeNS == 0)
			{
				timeNS = MafiaNet::GetTimeUS();
				timeMS = (MafiaNet::TimeMS)(timeNS / (MafiaNet::TimeUS) 1000);
			}

			callerDataAllocationUsed = SendImmediate((char *) bcs->data, bcs->numberOfBitsToSend, bcs->priority, bcs->reliability, bcs->orderingChannel, bcs->systemIdentifier, bcs->broadcast, true, timeNS, bcs->receipt);
			if (callerDataAllocationUsed == false)
				rakFree_Ex(bcs->data, _FILE_AND_LINE_);

			// Set the new connection state AFTER we call sendImmediate in case we are setting it to a disconnection state, which does not allow further sends
			if (bcs->connectionMode != RemoteSystemStruct::NO_ACTION)
			{
				remoteSystem = GetRemoteSystem(bcs->systemIdentifier, true, true);
				if (remoteSystem)
					remoteSystem->connectMode = bcs->connectionMode;
			}
		}
		else if (bcs->command == BufferedCommandStruct::BCS_SESSION_ACCEPT || bcs->command == BufferedCommandStruct::BCS_SESSION_REJECT)
		{
			// Validate here rather than in the user-thread API: only the network thread may read connectMode
			// and the awaiting flag without racing the receive path.
			RemoteSystemStruct *sessionSystem = GetRemoteSystem(bcs->systemIdentifier, true, true);
			// connectMode is checked as well as the awaiting flag: a peer that disconnects while the decision
			// is queued leaves the flag set (the teardown path does not clear session state), and answering
			// then would send on a dying connection and report a connection for a peer that has already gone.
			if (sessionSystem && sessionSystem->isActive &&
			    sessionSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA &&
			    sessionSystem->sessionConfigAwaitingLocalDecision)
			{
				sessionSystem->sessionConfigAwaitingLocalDecision = false;
				const unsigned int sessionPayloadLength = (unsigned int) BITS_TO_BYTES(bcs->numberOfBitsToSend);
				if (bcs->command == BufferedCommandStruct::BCS_SESSION_ACCEPT)
				{
					// With a pending pool the handshake may have started while the server was full, so the
					// incoming limit is enforced here. The application should check first; this is the backstop
					// that keeps two accepts racing one free slot from both getting it.
					SessionAdmission::Limits limits;
					limits.maxIncoming = GetMaximumIncomingConnections();
					limits.maxPending = maximumPendingSessions;
					limits.maxPendingPerAddress = maximumPendingSessionsPerAddress;
					SessionAdmission::Counts counts;
					CountRemoteInitiatedPeers(UNASSIGNED_SYSTEM_ADDRESS, counts.connected, counts.exchanging, counts.pending, counts.pendingFromAddress);
					if (SessionAdmission::MayAccept(limits, counts))
						SendSessionConfigResponse(sessionSystem, bcs->data, sessionPayloadLength);
					else
						SendSessionConfigRejection(sessionSystem, 0, 0);
				}
				else
					SendSessionConfigRejection(sessionSystem, bcs->data, sessionPayloadLength);
			}
			if (bcs->data)
				rakFree_Ex(bcs->data, _FILE_AND_LINE_);
		}
		else if (bcs->command == BufferedCommandStruct::BCS_SESSION_STATUS)
		{
			// Only a server peer still awaiting its application's decision may be told anything, and telling
			// it restarts the session timer here as well as on the client when it arrives.
			RemoteSystemStruct *sessionSystem = GetRemoteSystem(bcs->systemIdentifier, true, true);
			if (sessionSystem && sessionSystem->isActive &&
			    sessionSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA &&
			    sessionSystem->sessionConfigAwaitingLocalDecision &&
			    sessionSystem->sessionConfigIsConnectingSide == false)
			{
				MafiaNet::BitStream statusStream;
				statusStream.Write((MessageID) ID_SESSION_CONFIG_STATUS);
				const unsigned int statusLength = (unsigned int) BITS_TO_BYTES(bcs->numberOfBitsToSend);
				if (bcs->data && statusLength > 0)
					statusStream.Write(bcs->data, statusLength);
				SendImmediate((char *) statusStream.GetData(), statusStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, sessionSystem->systemAddress, false, false, MafiaNet::GetTimeUS(), 0);
				sessionSystem->sessionConfigStartTime = MafiaNet::GetTimeMS();
			}
			if (bcs->data)
				rakFree_Ex(bcs->data, _FILE_AND_LINE_);
		}
		else if (bcs->command == BufferedCommandStruct::BCS_CLOSE_CONNECTION)
		{
			// bcs->socket may be null: the socket can be released between queuing this
			// command and processing it here (e.g. during rapid connect/disconnect churn).
			// RakAssert is a no-op in release (NDEBUG), so guard explicitly and fall back
			// to the primary socket to avoid dereferencing null in CloseConnectionInternal2.
			RakNetSocket2 *closeSocket = bcs->socket;
			if (closeSocket == nullptr && socketList.Size() > 0)
				closeSocket = socketList[0];
			if (closeSocket != nullptr)
				CloseConnectionInternal2(bcs->systemIdentifier, false, true, bcs->orderingChannel, bcs->priority, *closeSocket);
		}
		else if (bcs->command == BufferedCommandStruct::BCS_CHANGE_SYSTEM_ADDRESS)
		{
			// Reroute
			RakPeer::RemoteSystemStruct *rssFromGuid = GetRemoteSystem(bcs->systemIdentifier.rakNetGuid, true, true);
			if (rssFromGuid != 0)
			{
				unsigned int existingSystemIndex = GetRemoteSystemIndex(rssFromGuid->systemAddress);
				ReferenceRemoteSystem(bcs->systemIdentifier.systemAddress, existingSystemIndex);
			}
		}
		else if (bcs->command == BufferedCommandStruct::BCS_GET_SOCKET)
		{
			SocketQueryOutput *sqo;
			if (bcs->systemIdentifier.IsUndefined())
			{
				sqo = socketQueryOutput.Allocate(_FILE_AND_LINE_);
				sqo->sockets = socketList;
				socketQueryOutput.Push(sqo);
			}
			else
			{
				remoteSystem = GetRemoteSystem(bcs->systemIdentifier, true, true);
				sqo = socketQueryOutput.Allocate(_FILE_AND_LINE_);

				sqo->sockets.Clear(false, _FILE_AND_LINE_);
				if (remoteSystem)
				{
					sqo->sockets.Push(remoteSystem->rakNetSocket, _FILE_AND_LINE_);
				}
				else
				{
					// Leave empty smart pointer
				}
				socketQueryOutput.Push(sqo);
			}
		}

#ifdef _DEBUG
		bcs->data = 0;
#endif

		bufferedCommands.Deallocate(bcs, _FILE_AND_LINE_);
	}

	if (requestedConnectionQueue.IsEmpty() == false)
	{
		if (timeNS == 0)
		{
			timeNS = MafiaNet::GetTimeUS();
			timeMS = (MafiaNet::TimeMS)(timeNS / (MafiaNet::TimeUS) 1000);
		}

		bool condition1, condition2;
		unsigned requestedConnectionQueueIndex = 0;
		requestedConnectionQueueMutex.Lock();
		while (requestedConnectionQueueIndex < requestedConnectionQueue.Size())
		{
			RequestedConnectionStruct *rcs;
			rcs = requestedConnectionQueue[requestedConnectionQueueIndex];
			if (rcs->cancelled)
			{
				// CancelConnectionAttempt flagged it; this thread is the one that frees requests.
				requestedConnectionQueue.RemoveAtIndex(requestedConnectionQueueIndex);
				DeleteRequestedConnection(rcs);
				continue;
			}
			requestedConnectionQueueMutex.Unlock();
			// A Happy Eyeballs candidate that has not started waits for its turn.
			const bool waitingForTurn = rcs->raceGroup != 0 && rcs->requestsMade == 0 && timeMS < rcs->nextCandidateTime;
			if (rcs->nextRequestTime < timeMS && !waitingForTurn)
			{
				condition1 = rcs->requestsMade == rcs->sendConnectionAttemptCount + 1;
				condition2 = (bool) ((rcs->systemAddress == UNASSIGNED_SYSTEM_ADDRESS) == 1);
				// If too many requests made or a hole then remove this if possible, otherwise invalidate it
				if (condition1 || condition2)
				{
					bool reportFailure = condition1 && !condition2 && rcs->actionToTake == RequestedConnectionStruct::CONNECT;
					SystemAddress failedAddress = rcs->systemAddress;

					requestedConnectionQueueMutex.Lock();
					for (unsigned int k = 0; k < requestedConnectionQueue.Size(); k++)
					{
						if (requestedConnectionQueue[k] == rcs)
						{
							requestedConnectionQueue.RemoveAtIndex(k);
							break;
						}
					}
					if (rcs->cancelled)
						reportFailure = false; // cancelled meanwhile: no message
					else if (rcs->raceGroup != 0)
					{
						// One candidate of a race gave up. The race only fails, once,
						// when its last candidate does; until then the next one starts now.
						if (AdvanceRace_NoLock(rcs->raceGroup, timeMS))
							reportFailure = false;
						else
							failedAddress = rcs->raceFirstAddress;
					}
					requestedConnectionQueueMutex.Unlock();

					if (reportFailure)
					{
						// Tell user of connection attempt failed
						packet = AllocPacket(sizeof(char), _FILE_AND_LINE_);
						packet->data[0] = ID_CONNECTION_ATTEMPT_FAILED; // Attempted a connection and couldn't
						packet->bitSize = (sizeof(char) * 8);
						packet->systemAddress = failedAddress;
						AddPacketToProducer(packet);
					}

					DeleteRequestedConnection(rcs);
				}
				else
				{

					const unsigned attemptsPerMTU = GetConnectionAttemptsPerMTU(rcs->sendConnectionAttemptCount);
					int MTUSizeIndex = rcs->requestsMade / attemptsPerMTU;
					if (MTUSizeIndex >= NUM_MTU_SIZES)
						MTUSizeIndex = NUM_MTU_SIZES - 1;
					rcs->requestsMade++;
					rcs->nextRequestTime = timeMS + rcs->timeBetweenSendConnectionAttemptsMS;

					MafiaNet::BitStream bitStream;
					//WriteOutOfBandHeader(&bitStream, ID_USER_PACKET_ENUM);
					bitStream.Write((MessageID) ID_OPEN_CONNECTION_REQUEST_1);
					bitStream.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
					bitStream.Write((MessageID) RAKNET_PROTOCOL_VERSION);
					bitStream.PadWithZeroToByteLength(mtuSizes[MTUSizeIndex] - UDP_HEADER_SIZE);

					char str[256];
					rcs->systemAddress.ToString(true, str, static_cast<size_t>(256));

					//RAKNET_DEBUG_PRINTF("%i:IOCR, ", __LINE__);

					unsigned i;
					for (i = 0; i < pluginListNTS.Size(); i++)
						pluginListNTS[i]->OnDirectSocketSend((const char *) bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), rcs->systemAddress);

					RakNetSocket2 *socketToUse;
					if (rcs->socket == 0)
						socketToUse = socketList[rcs->socketIndex];
					else
						socketToUse = rcs->socket;

					FixForSocket(rcs->systemAddress, socketToUse);
					if (socketToUse->IsBerkleySocket())
						((RNS2_Berkley *) socketToUse)->SetDoNotFragment(1);

					//					SocketLayer::SetDoNotFragment(socketToUse, 1);
					MafiaNet::Time sendToStart = MafiaNet::GetTime();

					RNS2_SendParameters bsp;
					bsp.data = (char *) bitStream.GetData();
					bsp.length = bitStream.GetNumberOfBytesUsed();
					bsp.systemAddress = rcs->systemAddress;
					const RNS2SendResult sendResult = socketToUse->Send(&bsp, _FILE_AND_LINE_);
					// Send() reports failure as sendto's own return value and drops the reason, so
					// the error has to be read back off the thread before anything else touches a
					// socket. The old test here compared that return value against 10040 directly,
					// which sendto never yields -- it yields SOCKET_ERROR -- so this branch was dead
					// and a locally-refused MTU burned its rung's entire attempt budget.
					if (sendResult < 0 && RNS2_IsDatagramTooLargeError(RNS2_GetLastSocketError()))
					{
						// The local interface refused the datagram outright. Every attempt at this
						// size fails the same way, so step down now.
						rcs->requestsMade = (unsigned char) ((MTUSizeIndex + 1) * attemptsPerMTU);
						rcs->nextRequestTime = timeMS;
					}
					else
					{
						MafiaNet::Time sendToEnd = MafiaNet::GetTime();
						if (sendToEnd - sendToStart > 100)
						{
							// A send that blocks this long means the outgoing interface is
							// struggling: a virtual VPN adapter renegotiating its tunnel, or a full
							// transmit queue. Smaller datagrams are likelier to get through, so drop
							// to the lowest MTU -- but keep trying. Giving up here (which is what
							// this did when already on the lowest rung) turned a transient stall
							// into ID_CONNECTION_ATTEMPT_FAILED with most of the budget unspent.
							const int lowestMtuIndex = (int) (attemptsPerMTU * (unsigned) (NUM_MTU_SIZES - 1));
							if (lowestMtuIndex > (int) rcs->requestsMade)
							{
								rcs->requestsMade = (unsigned char) lowestMtuIndex;
								rcs->nextRequestTime = timeMS;
							}
						}
					}
					// SocketLayer::SetDoNotFragment(socketToUse, 0);
					if (socketToUse->IsBerkleySocket())
						((RNS2_Berkley *) socketToUse)->SetDoNotFragment(0);

					requestedConnectionQueueIndex++;
				}
			}
			else
				requestedConnectionQueueIndex++;

			requestedConnectionQueueMutex.Lock();
		}
		requestedConnectionQueueMutex.Unlock();
	}

	// remoteSystemList in network thread
	for (activeSystemListIndex = 0; activeSystemListIndex < activeSystemListSize; ++activeSystemListIndex)
	//for ( remoteSystemIndex = 0; remoteSystemIndex < remoteSystemListSize; ++remoteSystemIndex )
	{
		// I'm using systemAddress from remoteSystemList but am not locking it because this loop is called very frequently and it doesn't
		// matter if we miss or do an extra update.  The reliability layers themselves never care which player they are associated with
		//systemAddress = remoteSystemList[ remoteSystemIndex ].systemAddress;
		// Allow the systemAddress for this remote system list to change.  We don't care if it changes now.
		//	remoteSystemList[ remoteSystemIndex ].allowSystemAddressAssigment=true;


		// Found an active remote system
		remoteSystem = activeSystemList[activeSystemListIndex];
		systemAddress = remoteSystem->systemAddress;
		RakAssert(systemAddress != UNASSIGNED_SYSTEM_ADDRESS);
		// Update is only safe to call from the same thread that calls HandleSocketReceiveFromConnectedPlayer,
		// which is this thread

		if (timeNS == 0)
		{
			timeNS = MafiaNet::GetTimeUS();
			timeMS = (MafiaNet::TimeMS)(timeNS / (MafiaNet::TimeUS) 1000);
			//RAKNET_DEBUG_PRINTF("timeNS = %I64i timeMS=%i\n", timeNS, timeMS);
		}


		if (timeMS > remoteSystem->lastReliableSend && timeMS - remoteSystem->lastReliableSend > remoteSystem->reliabilityLayer.GetTimeoutTime() / 2 && remoteSystem->connectMode == RemoteSystemStruct::CONNECTED)
		{
			// If no reliable packets are waiting for an ack, do a one byte reliable send so that disconnections are noticed
			RakNetStatistics rakNetStatistics;
			rnss = remoteSystem->reliabilityLayer.GetStatistics(&rakNetStatistics);
			if (rnss->messagesInResendBuffer == 0)
			{
				PingInternal(systemAddress, true, MafiaNet::Reliability::Reliable);

				//remoteSystem->lastReliableSend=timeMS+remoteSystem->reliabilityLayer.GetTimeoutTime();
				remoteSystem->lastReliableSend = timeMS;
			}
		}

		if (endThreads)
			// for the final call, make sure we send out any outstanding ACKs
			remoteSystem->reliabilityLayer.UpdateAndForceACKs(remoteSystem->rakNetSocket, systemAddress, remoteSystem->MTUSize, timeNS, maxOutgoingBPS, pluginListNTS, &rnr, updateBitStream); // systemAddress only used for the internet simulator test
		else
			remoteSystem->reliabilityLayer.Update(remoteSystem->rakNetSocket, systemAddress, remoteSystem->MTUSize, timeNS, maxOutgoingBPS, pluginListNTS, &rnr, updateBitStream); // systemAddress only used for the internet simulator test

		// The reliability layer steps the negotiated MTU down when it
		// detects an in-session path-MTU black hole; keep the value
		// GetMTUSize() reports in sync with what is actually on the wire.
		// Written only on an actual step-down: GetMTUSize() reads this
		// field from the user thread without synchronization (as it always
		// has for the connect-time write), so avoid turning it into a
		// continuously-written field.
		const int reliabilityMtu = remoteSystem->reliabilityLayer.GetCurrentMtuBytes();
		if (remoteSystem->MTUSize != reliabilityMtu)
			remoteSystem->MTUSize = reliabilityMtu;

		// Check for failure conditions
		if (remoteSystem->reliabilityLayer.IsDeadConnection() ||
		    ((remoteSystem->connectMode == RemoteSystemStruct::DISCONNECT_ASAP || remoteSystem->connectMode == RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY) && remoteSystem->reliabilityLayer.IsOutgoingDataWaiting() == false) ||
		    (remoteSystem->connectMode == RemoteSystemStruct::DISCONNECT_ON_NO_ACK && (remoteSystem->reliabilityLayer.AreAcksWaiting() == false || remoteSystem->reliabilityLayer.AckTimeout(timeMS) == true)) ||
		    ((
		        (remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION ||
		         remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST ||
		         remoteSystem->connectMode == RemoteSystemStruct::UNVERIFIED_SENDER) &&
		        timeMS > remoteSystem->connectionTime && timeMS - remoteSystem->connectionTime > 10000)))
		{
			//	RAKNET_DEBUG_PRINTF("timeMS=%i remoteSystem->connectionTime=%i\n", timeMS, remoteSystem->connectionTime );

			// Before the close notification logic: a held session request is not a reported connection, but
			// the application is holding a decision for it and has to learn it will never be needed.
			ProduceSessionAbandoned(remoteSystem);

			// Failed.  Inform the user?
			// TODO - RakNet 4.0 - Return a different message identifier for DISCONNECT_ASAP_SILENTLY and DISCONNECT_ASAP than for DISCONNECT_ON_NO_ACK
			// The first two mean we called CloseConnection(), the last means the other system sent us ID_DISCONNECTION_NOTIFICATION
			// A connection the application was never told about must not produce a close notification for
			// it. On the accepting side that covers a peer refused by RejectSession() (which parks the slot
			// in DISCONNECT_ON_NO_ACK so the refusal is delivered first) and one dropped mid-handshake.
			// The connecting side is exempt: it has an outstanding Connect() to resolve either way.
			const bool reportableConnection = remoteSystem->connectionReportedToApplication || remoteSystem->weInitiatedTheConnection;
			// Our Connect() was absorbed into this incoming connection (cross-connection tie-break) and the
			// connection dies before the application heard of it: that Connect() is still owed an answer.
			const bool absorbedAttemptUnanswered = remoteSystem->absorbedOurConnectAttempt && !remoteSystem->connectionReportedToApplication;
			if (absorbedAttemptUnanswered ||
			    (reportableConnection &&
			     (remoteSystem->connectMode == RemoteSystemStruct::CONNECTED || remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION || remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA || remoteSystem->connectMode == RemoteSystemStruct::DISCONNECT_ASAP || remoteSystem->connectMode == RemoteSystemStruct::DISCONNECT_ON_NO_ACK)))
			{

				//					MafiaNet::BitStream undeliveredMessages;
				//					remoteSystem->reliabilityLayer.GetUndeliveredMessages(&undeliveredMessages,remoteSystem->MTUSize);

				//					packet=AllocPacket(sizeof( char ) + undeliveredMessages.GetNumberOfBytesUsed());
				// A graceful remote disconnect (DISCONNECT_ON_NO_ACK) may carry a reason payload stashed when the
				// notification arrived. Only ID_DISCONNECTION_NOTIFICATION carries it; ID_CONNECTION_LOST and
				// ID_CONNECTION_ATTEMPT_FAILED are locally synthesized and stay payload-less.
				const bool attachReason = !absorbedAttemptUnanswered && remoteSystem->connectMode == RemoteSystemStruct::DISCONNECT_ON_NO_ACK && remoteSystem->disconnectReasonData != 0;
				const unsigned int reasonLength = attachReason ? remoteSystem->disconnectReasonLength : 0;
				packet = AllocPacket(sizeof(char) + reasonLength, _FILE_AND_LINE_);
				if (absorbedAttemptUnanswered || remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION)
					packet->data[0] = ID_CONNECTION_ATTEMPT_FAILED; // Attempted a connection and couldn't
				else if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA)
					// Dropped mid-session-handshake. The connecting side still has an attempt outstanding; the
					// accepting side never reported anything, so a lost connection is all it can report.
					packet->data[0] = remoteSystem->sessionConfigIsConnectingSide
					                      ? (unsigned char) ID_CONNECTION_ATTEMPT_FAILED
					                      : (unsigned char) ID_CONNECTION_LOST;
				else if (remoteSystem->connectMode == RemoteSystemStruct::CONNECTED)
					packet->data[0] = ID_CONNECTION_LOST; // DeadConnection
				else
					packet->data[0] = ID_DISCONNECTION_NOTIFICATION; // DeadConnection

				if (attachReason)
					memcpy(packet->data + sizeof(unsigned char), remoteSystem->disconnectReasonData, reasonLength);

				//					memcpy(packet->data+1, undeliveredMessages.GetData(), undeliveredMessages.GetNumberOfBytesUsed());

				packet->guid = remoteSystem->guid;
				packet->systemAddress = systemAddress;
				packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
				packet->guid.systemIndex = packet->systemAddress.systemIndex;

				AddPacketToProducer(packet);
			}
			// else connection shutting down, don't bother telling the user

#ifdef _DO_PRINTF
			RAKNET_DEBUG_PRINTF("Connection dropped for player %i:%i\n", systemAddress);
#endif
			// we are about to close the connection to the remote system, we'd still make sure to send any outstanding ACKs, so for the remote system not unnecessarily waiting for these until its timeout
			// (and trying to resend them unnecessarily)
			remoteSystem->reliabilityLayer.UpdateAndForceACKs(remoteSystem->rakNetSocket, systemAddress, remoteSystem->MTUSize, timeNS, maxOutgoingBPS, pluginListNTS, &rnr, updateBitStream);

			CloseConnectionInternal2(systemAddress, false, true, 0, MafiaNet::Priority::Low, *(remoteSystem->rakNetSocket));
			continue;
		}

		// A session handshake that never completes must not pin the slot. Acks keep flowing in
		// EXCHANGING_SESSION_DATA, so the ordinary dead-connection detection above would never fire; bound
		// it by the same timeout instead. Interactive servers put an application in the middle of this
		// exchange, so it is a reachable state, not a theoretical one.
		if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA &&
		    remoteSystem->sessionConfigStartTime != 0 &&
		    timeMS > remoteSystem->sessionConfigStartTime &&
		    timeMS - remoteSystem->sessionConfigStartTime > GetSessionTimeoutFor(remoteSystem))
		{
			// Only the connecting side reports a failure; a server just drops the half-open slot.
			if (remoteSystem->sessionConfigIsConnectingSide)
			{
				Packet *timeoutPacket = AllocPacket(sizeof(MessageID), _FILE_AND_LINE_);
				timeoutPacket->data[0] = (unsigned char) ID_CONNECTION_ATTEMPT_FAILED;
				timeoutPacket->bitSize = sizeof(MessageID) * 8;
				timeoutPacket->systemAddress = remoteSystem->systemAddress;
				timeoutPacket->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
				timeoutPacket->guid = remoteSystem->guid;
				timeoutPacket->guid.systemIndex = timeoutPacket->systemAddress.systemIndex;
				AddPacketToProducer(timeoutPacket);
			}
			else
			{
				ProduceSessionAbandoned(remoteSystem);
			}

			remoteSystem->reliabilityLayer.UpdateAndForceACKs(remoteSystem->rakNetSocket, systemAddress, remoteSystem->MTUSize, timeNS, maxOutgoingBPS, pluginListNTS, &rnr, updateBitStream);
			CloseConnectionInternal2(systemAddress, false, true, 0, MafiaNet::Priority::Low, *(remoteSystem->rakNetSocket));
			continue;
		}

		// Ping this guy if it is time to do so
		if (remoteSystem->connectMode == RemoteSystemStruct::CONNECTED && timeMS > remoteSystem->nextPingTime && (occasionalPing || remoteSystem->lowestPing == (unsigned short) -1))
		{
			remoteSystem->nextPingTime = timeMS + 5000;
			PingInternal(systemAddress, true, MafiaNet::Reliability::Unreliable);

			// Update again immediately after this tick so the ping goes out right away
			quitAndDataEvents.SetEvent();
		}

		// Find whoever has the lowest player ID
		//if (systemAddress < authoritativeClientSystemAddress)
		// authoritativeClientSystemAddress=systemAddress;

		// Does the reliability layer have any packets waiting for us?
		// To be thread safe, this has to be called in the same thread as HandleSocketReceiveFromConnectedPlayer
		bitSize = remoteSystem->reliabilityLayer.Receive(&data);

		while (bitSize > 0)
		{
			// These types are for internal use and should never arrive from a network packet.
			// A remote that sends one anyway gets it dropped: freed, and the rest of the
			// queue still processed (this used to leak it and stall the queue, and to
			// RakAssert in Debug).
			if (data[0] == ID_CONNECTION_ATTEMPT_FAILED)
			{
				rakFree_Ex(data, _FILE_AND_LINE_);
				bitSize = remoteSystem->reliabilityLayer.Receive(&data);
				continue;
			}

			// Fast and easy - just use the data that was returned
			byteSize = (unsigned int) BITS_TO_BYTES(bitSize);

			// For unknown senders we only accept a few specific packets
			if (remoteSystem->connectMode == RemoteSystemStruct::UNVERIFIED_SENDER)
			{
				if ((unsigned char) (data)[0] == ID_CONNECTION_REQUEST)
				{
					ParseConnectionRequestPacket(remoteSystem, systemAddress, (const char *) data, byteSize);
					rakFree_Ex(data, _FILE_AND_LINE_);
					// Cross-connection: if our own attempt to this peer is still pending, it lost the tie-break (see the
					// REQUEST_2 handler). The winner's ID_CONNECTION_REQUEST just opened under these keys, so this is the
					// connection: drop our attempt quietly, wiping its key pair.
					// Until the connection is reported, that Connect() is answered by the failure path if it dies.
					if (remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST && remoteSystem->reliabilityLayer.IsEncrypted() &&
					    DropConnectionAttemptSupersededBy(systemAddress))
						remoteSystem->absorbedOurConnectAttempt = true;
				}
				else
				{
					CloseConnectionInternal2(systemAddress, false, true, 0, MafiaNet::Priority::Low, *(remoteSystem->rakNetSocket));
#ifdef _DO_PRINTF
					RAKNET_DEBUG_PRINTF("Temporarily banning %i:%i for sending nonsense data\n", systemAddress);
#endif

					char str1[64];
					systemAddress.ToString(false, str1, static_cast<size_t>(64));
					AddToBanList(str1, remoteSystem->reliabilityLayer.GetTimeoutTime());


					rakFree_Ex(data, _FILE_AND_LINE_);
				}
			}
			else
			{
				// However, if we are connected we still take a connection request in case both systems are trying to connect to each other
				// at the same time
				if ((unsigned char) (data)[0] == ID_CONNECTION_REQUEST)
				{
					// 04/27/06 This is wrong.  With cross connections, we can both have initiated the connection are in state REQUESTED_CONNECTION
					// 04/28/06 Downgrading connections from connected will close the connection due to security at ((remoteSystem->connectMode!=RemoteSystemStruct::CONNECTED && time > remoteSystem->connectionTime && time - remoteSystem->connectionTime > 10000))
					if (remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION)
					{
						ParseConnectionRequestPacket(remoteSystem, systemAddress, (const char *) data, byteSize);
					}
					else
					{

						MafiaNet::BitStream bs((unsigned char *) data, byteSize, false);
						bs.IgnoreBytes(sizeof(MessageID));
						bs.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID));
						bs.IgnoreBytes(RakNetGUID::size());
						MafiaNet::Time incomingTimestamp;
						bs.Read(incomingTimestamp);

						// Got a connection request message from someone we are already connected to. Just reply normally.
						// This can happen due to race conditions with the fully connected mesh
						OnConnectionRequest(remoteSystem, incomingTimestamp);
					}
					rakFree_Ex(data, _FILE_AND_LINE_);
				}
				else if ((unsigned char) data[0] == ID_NEW_INCOMING_CONNECTION && byteSize > sizeof(unsigned char) + sizeof(unsigned int) + sizeof(unsigned short) + sizeof(MafiaNet::Time) * 2)
				{
					if (remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST)
					{
						// Transport is up, but the application is not told so until the client's session payload
						// arrives in ID_SESSION_CONFIG_REQUEST and we answer it.
						remoteSystem->connectMode = RemoteSystemStruct::EXCHANGING_SESSION_DATA;
						remoteSystem->sessionConfigIsConnectingSide = false;
						remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit
						PingInternal(systemAddress, true, MafiaNet::Reliability::Unreliable);

						// Update again immediately after this tick so the ping goes out right away
						quitAndDataEvents.SetEvent();

						MafiaNet::BitStream inBitStream((unsigned char *) data, byteSize, false);
						SystemAddress bsSystemAddress;

						inBitStream.IgnoreBits(8);
						inBitStream.Read(bsSystemAddress);
						for (unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++)
							inBitStream.Read(remoteSystem->theirInternalSystemAddress[i]);

						MafiaNet::Time sendPingTime, sendPongTime;
						inBitStream.Read(sendPingTime);
						inBitStream.Read(sendPongTime);
						OnConnectedPong(sendPingTime, sendPongTime, remoteSystem);

						// Overwrite the data in the packet
						//					NewIncomingConnectionStruct newIncomingConnectionStruct;
						//					MafiaNet::BitStream nICS_BS( data, NewIncomingConnectionStruct_Size, false );
						//					newIncomingConnectionStruct.Deserialize( nICS_BS );

						remoteSystem->myExternalSystemAddress = bsSystemAddress;

						// Bug: If A connects to B through R, A's firstExternalID is set to R. If A tries to send to R, sends to loopback because R==firstExternalID
						// Correct fix is to specify in Connect() if target is through a proxy.
						// However, in practice you have to connect to something else first anyway to know about the proxy. So setting once only is good enough
						if (firstExternalID == UNASSIGNED_SYSTEM_ADDRESS)
						{
							firstExternalID = bsSystemAddress;
							firstExternalID.debugPort = ntohs(firstExternalID.address.addr4.sin_port);
						}

						// Hold this until the session handshake completes; the bytes are copied because `data`
						// is freed as soon as this branch returns.
						ClearSessionConfig(remoteSystem);
						remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit
						remoteSystem->withheldConnectionPacketData = (unsigned char *) rakMalloc_Ex(byteSize, _FILE_AND_LINE_);
						if (remoteSystem->withheldConnectionPacketData != 0)
						{
							memcpy(remoteSystem->withheldConnectionPacketData, data, byteSize);
							remoteSystem->withheldConnectionPacketLength = byteSize;
						}
						// AllocPacket used to take ownership of `data` here; the copy above means this branch does.
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
					else
					{
						// Already connected (or not expecting it): ignore it. The buffer is ours to free; a remote that
						// repeats this message must not make us leak one per datagram.
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
				}
				else if ((unsigned char) data[0] == ID_CONNECTED_PONG && byteSize == sizeof(unsigned char) + sizeof(MafiaNet::Time) * 2)
				{
					MafiaNet::Time sendPingTime, sendPongTime;

					// Copy into the ping times array the current time - the value returned
					// First extract the sent ping
					MafiaNet::BitStream inBitStream((unsigned char *) data, byteSize, false);
					//PingStruct ps;
					//ps.Deserialize(psBS);
					inBitStream.IgnoreBits(8);
					inBitStream.Read(sendPingTime);
					inBitStream.Read(sendPongTime);

					OnConnectedPong(sendPingTime, sendPongTime, remoteSystem);

					rakFree_Ex(data, _FILE_AND_LINE_);
				}
				else if ((unsigned char) data[0] == ID_CONNECTED_PING && byteSize == sizeof(unsigned char) + sizeof(MafiaNet::Time))
				{
					MafiaNet::BitStream inBitStream((unsigned char *) data, byteSize, false);
					inBitStream.IgnoreBits(8);
					MafiaNet::Time sendPingTime;
					inBitStream.Read(sendPingTime);

					MafiaNet::BitStream outBitStream;
					outBitStream.Write((MessageID) ID_CONNECTED_PONG);
					outBitStream.Write(sendPingTime);
					outBitStream.Write(MafiaNet::GetTime());
					SendImmediate((char *) outBitStream.GetData(), outBitStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::Unreliable, 0, systemAddress, false, false, MafiaNet::GetTimeUS(), 0);

					// Update again immediately after this tick so the ping goes out right away
					quitAndDataEvents.SetEvent();

					rakFree_Ex(data, _FILE_AND_LINE_);
				}
				else if ((unsigned char) data[0] == ID_DISCONNECTION_NOTIFICATION)
				{
					// Stash any reason payload (everything after the 1-byte ID) so it can ride along with the
					// user-facing notification packet synthesized once outstanding ACKs are flushed. This raw
					// reliability-layer buffer is freed below, so the bytes have to be copied out now.
					ClearDisconnectReason(remoteSystem);
					if (byteSize > sizeof(unsigned char))
					{
						const unsigned int reasonLength = byteSize - (unsigned int) sizeof(unsigned char);
						remoteSystem->disconnectReasonData = (unsigned char *) rakMalloc_Ex(reasonLength, _FILE_AND_LINE_);
						if (remoteSystem->disconnectReasonData != 0)
						{
							memcpy(remoteSystem->disconnectReasonData, data + sizeof(unsigned char), reasonLength);
							remoteSystem->disconnectReasonLength = reasonLength;
						}
					}

					// We shouldn't close the connection immediately because we need to ack the ID_DISCONNECTION_NOTIFICATION
					remoteSystem->connectMode = RemoteSystemStruct::DISCONNECT_ON_NO_ACK;
					rakFree_Ex(data, _FILE_AND_LINE_);

					//	AddPacketToProducer(packet);
				}
				else if ((unsigned char) (data)[0] == ID_DETECT_LOST_CONNECTIONS && byteSize == sizeof(unsigned char))
				{
					// Do nothing
					rakFree_Ex(data, _FILE_AND_LINE_);
				}
				else if ((unsigned char) (data)[0] == ID_SESSION_CONFIG_REQUEST)
				{
					// Server side of the session handshake. The client's payload arrives before we answer, so a
					// server can inspect it and refuse without ever having disclosed its own.
					if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && remoteSystem->sessionConfigAwaitingLocalDecision == false)
					{
						if (StoreRemoteSessionConfig(remoteSystem, (const unsigned char *) data + sizeof(MessageID), byteSize - (unsigned int) sizeof(MessageID)) == false)
						{
							// Oversized payload is a protocol violation; drop without answering.
							remoteSystem->connectMode = RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY;
							rakFree_Ex(data, _FILE_AND_LINE_);
						}
						else if (sessionConfigInteractive)
						{
							// Let the application decide. The connection stays unreported until it calls
							// AcceptSession() or RejectSession(), or the handshake times out below.
							remoteSystem->sessionConfigAwaitingLocalDecision = true;
							packet = AllocPacket(byteSize, data, _FILE_AND_LINE_);
							packet->bitSize = bitSize;
							packet->systemAddress = systemAddress;
							packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
							packet->guid = remoteSystem->guid;
							packet->guid.systemIndex = packet->systemAddress.systemIndex;
							AddPacketToProducer(packet);
						}
						else
						{
							rakPeerMutexes[sessionConfig_Mutex].Lock();
							SendSessionConfigResponse(remoteSystem, (const char *) sessionConfig.GetData(), (unsigned int) sessionConfig.GetNumberOfBytesUsed());
							rakPeerMutexes[sessionConfig_Mutex].Unlock();
							rakFree_Ex(data, _FILE_AND_LINE_);
						}
					}
					else
					{
						// A repeat request, or one on an already-established connection: ignore it rather than
						// letting a peer re-run the handshake and re-produce the connection packet.
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
				}
				else if ((unsigned char) (data)[0] == ID_SESSION_CONFIG)
				{
					// Client side of the session handshake: the server's payload is the last thing we needed, so
					// the withheld ID_CONNECTION_REQUEST_ACCEPTED goes out now and the connection becomes real.
					// Only the peer that initiated the connection may be answered this way, so a client cannot
					// push a payload at a server by replying with the server's own message.
					if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && remoteSystem->sessionConfigIsConnectingSide)
					{
						if (StoreRemoteSessionConfig(remoteSystem, (const unsigned char *) data + sizeof(MessageID), byteSize - (unsigned int) sizeof(MessageID)) == false)
						{
							remoteSystem->connectMode = RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY;
						}
						else
						{
							remoteSystem->connectMode = RemoteSystemStruct::CONNECTED;
							remoteSystem->sessionConfigStartTime = 0;
							ProduceWithheldConnectionPacket(remoteSystem, (MessageID) ID_CONNECTION_REQUEST_ACCEPTED);
						}
					}
					rakFree_Ex(data, _FILE_AND_LINE_);
				}
				else if ((unsigned char) (data)[0] == ID_SESSION_CONFIG_STATUS)
				{
					// Client side, while the server's application decides: surface the status and restart our own
					// session timer, which is what lets a queued client wait longer than the timeout. Bound to the
					// connecting side like the other replies, so a client cannot push one at a server.
					if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && remoteSystem->sessionConfigIsConnectingSide)
					{
						remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit
						packet = AllocPacket(byteSize, data, _FILE_AND_LINE_);
						packet->bitSize = bitSize;
						packet->systemAddress = systemAddress;
						packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
						packet->guid = remoteSystem->guid;
						packet->guid.systemIndex = packet->systemAddress.systemIndex;
						AddPacketToProducer(packet);
					}
					else
					{
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
				}
				else if ((unsigned char) (data)[0] == ID_SESSION_CONFIG_REJECTED)
				{
					// Only meaningful to the peer that initiated the connection. Without this a malicious client
					// could inject ID_CONNECTION_ATTEMPT_FAILED -- a message the application only ever expects for
					// its own outbound connects -- into a listening server's queue.
					if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && remoteSystem->sessionConfigIsConnectingSide)
					{
						// Rewrite in place so the reason string the server supplied stays at data+1, matching the
						// ID_DISCONNECTION_NOTIFICATION reason convention. No connection is ever reported.
						packet = AllocPacket(byteSize, data, _FILE_AND_LINE_);
						packet->data[0] = (unsigned char) ID_CONNECTION_ATTEMPT_FAILED;
						packet->bitSize = bitSize;
						packet->systemAddress = systemAddress;
						packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
						packet->guid = remoteSystem->guid;
						packet->guid.systemIndex = packet->systemAddress.systemIndex;
						AddPacketToProducer(packet);

						remoteSystem->connectMode = RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY;
					}
					else
					{
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
				}
				else if ((unsigned char) (data)[0] == ID_INVALID_PASSWORD)
				{
					if (remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION)
					{
						packet = AllocPacket(byteSize, data, _FILE_AND_LINE_);
						packet->bitSize = bitSize;
						packet->systemAddress = systemAddress;
						packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
						packet->guid = remoteSystem->guid;
						packet->guid.systemIndex = packet->systemAddress.systemIndex;
						AddPacketToProducer(packet);

						remoteSystem->connectMode = RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY;
					}
					else
					{
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
				}
				else if ((unsigned char) (data)[0] == ID_CONNECTION_REQUEST_ACCEPTED)
				{
					if (byteSize > sizeof(MessageID) + sizeof(unsigned int) + sizeof(unsigned short) + sizeof(SystemIndex) + sizeof(MafiaNet::Time) * 2)
					{
						// Make sure this connection accept is from someone we wanted to connect to
						bool allowConnection, alreadyConnected;

						if (remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST ||
						    remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION ||
						    allowConnectionResponseIPMigration)
							allowConnection = true;
						else
							allowConnection = false;

						if (remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST)
							alreadyConnected = true;
						else
							alreadyConnected = false;

						if (allowConnection)
						{
							SystemAddress externalID;
							SystemIndex systemIndex;
							//								SystemAddress internalID;

							MafiaNet::BitStream inBitStream((unsigned char *) data, byteSize, false);
							inBitStream.IgnoreBits(8);
							//	inBitStream.Read(remotePort);
							inBitStream.Read(externalID);
							inBitStream.Read(systemIndex);
							for (unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++)
								inBitStream.Read(remoteSystem->theirInternalSystemAddress[i]);

							MafiaNet::Time sendPingTime, sendPongTime;
							inBitStream.Read(sendPingTime);
							inBitStream.Read(sendPongTime);
							OnConnectedPong(sendPingTime, sendPongTime, remoteSystem);

							// Find a free remote system struct to use
							//						MafiaNet::BitStream casBitS(data, byteSize, false);
							//						ConnectionAcceptStruct cas;
							//						cas.Deserialize(casBitS);
							//	systemAddress.GetPort() = remotePort;

							// The remote system told us our external IP, so save it
							remoteSystem->myExternalSystemAddress = externalID;
							// Transport is up, but the application is not told so until the session payloads have been
							// exchanged. ID_CONNECTION_REQUEST_ACCEPTED is stashed and replayed from ID_SESSION_CONFIG.
							remoteSystem->connectMode = RemoteSystemStruct::EXCHANGING_SESSION_DATA;
							remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit

							// Bug: If A connects to B through R, A's firstExternalID is set to R. If A tries to send to R, sends to loopback because R==firstExternalID
							// Correct fix is to specify in Connect() if target is through a proxy.
							// However, in practice you have to connect to something else first anyway to know about the proxy. So setting once only is good enough
							if (firstExternalID == UNASSIGNED_SYSTEM_ADDRESS)
							{
								firstExternalID = externalID;
								firstExternalID.debugPort = ntohs(firstExternalID.address.addr4.sin_port);
							}

							// Hold the connection-complete packet until the session handshake finishes. The bytes are
							// copied because `data` is freed as soon as this branch returns.
							ClearSessionConfig(remoteSystem);
							remoteSystem->sessionConfigIsConnectingSide = true;
							remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit
							remoteSystem->withheldConnectionPacketData = (unsigned char *) rakMalloc_Ex(byteSize, _FILE_AND_LINE_);
							if (remoteSystem->withheldConnectionPacketData != 0)
							{
								memcpy(remoteSystem->withheldConnectionPacketData, data, byteSize);
								remoteSystem->withheldConnectionPacketLength = byteSize;
							}
							// AllocPacket used to take ownership of `data` here; the copy above means this branch does.
							rakFree_Ex(data, _FILE_AND_LINE_);

							MafiaNet::BitStream outBitStream;
							outBitStream.Write((MessageID) ID_NEW_INCOMING_CONNECTION);
							outBitStream.Write(systemAddress);
							for (unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++)
								outBitStream.Write(ipList[i]);
							outBitStream.Write(sendPongTime);
							outBitStream.Write(MafiaNet::GetTime());

							SendImmediate((char *) outBitStream.GetData(), outBitStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, systemAddress, false, false, MafiaNet::GetTimeUS(), 0);

							// Our half of the session handshake. Sent on the same ordered channel right behind
							// ID_NEW_INCOMING_CONNECTION, so the server has the transport connection before this lands.
							{
								MafiaNet::BitStream sessionBitStream;
								sessionBitStream.Write((MessageID) ID_SESSION_CONFIG_REQUEST);
								rakPeerMutexes[sessionConfig_Mutex].Lock();
								if (sessionConfig.GetNumberOfBytesUsed() > 0)
									sessionBitStream.Write((const char *) sessionConfig.GetData(), sessionConfig.GetNumberOfBytesUsed());
								rakPeerMutexes[sessionConfig_Mutex].Unlock();
								SendImmediate((char *) sessionBitStream.GetData(), sessionBitStream.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, systemAddress, false, false, MafiaNet::GetTimeUS(), 0);
							}

							if (alreadyConnected == false)
							{
								PingInternal(systemAddress, true, MafiaNet::Reliability::Unreliable);
							}
						}
						else
						{
							// Ignore, already connected
							rakFree_Ex(data, _FILE_AND_LINE_);
						}
					}
					else
					{
						// Too short to be a connection accept: drop it. Not a RakAssert -- the
						// remote chooses this size, and a Debug build must not abort on it.
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
				}
				else
				{
					// What do I do if I get a message from a system, before I am fully connected?
					// I can either ignore it or give it to the user
					// It seems like giving it to the user is a better option
					//
					// Except while the session handshake is running: the application has not been told this
					// connection exists, so handing it application data from that peer would deliver a packet
					// for a guid it has never seen connect -- and from a peer it may still refuse. A peer that
					// respects the protocol cannot send here anyway: its own public send path is gated on the
					// same state, so anything arriving in this window is malformed or hostile.
					if (MayReachApplicationFromRemote(data, byteSize) &&
					    remoteSystem->isActive &&
					    remoteSystem->connectMode != RemoteSystemStruct::EXCHANGING_SESSION_DATA)
					{
						packet = AllocPacket(byteSize, data, _FILE_AND_LINE_);
						packet->bitSize = bitSize;
						packet->systemAddress = systemAddress;
						packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
						packet->guid = remoteSystem->guid;
						packet->guid.systemIndex = packet->systemAddress.systemIndex;
						if (!DispatchNetworkThreadReceive(packet))
							AddPacketToProducer(packet);
					}
					else
					{
						rakFree_Ex(data, _FILE_AND_LINE_);
					}
				}
			}

			// Does the reliability layer have any more packets waiting for us?
			// To be thread safe, this has to be called in the same thread as HandleSocketReceiveFromConnectedPlayer
			bitSize = remoteSystem->reliabilityLayer.Receive(&data);
		}
	}

	return true;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void RakPeer::OnRNS2Recv(RNS2RecvStruct *recvStruct)
{
	if (incomingDatagramEventHandler)
	{
		if (incomingDatagramEventHandler(recvStruct) != true)
			return;
	}

	PushBufferedPacket(recvStruct);
	quitAndDataEvents.SetEvent();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RAK_THREAD_DECLARATION(MafiaNet::UpdateNetworkLoop)
{


	RakPeer *rakPeer = (RakPeer *) arguments;


	/*
	// 11/15/05 - this is slower than Sleep()
#ifdef _WIN32
#if (_WIN32_WINNT >= 0x0400) || (_WIN32_WINDOWS > 0x0400)
	// Lets see if these timers give better performance than Sleep
	HANDLE timerHandle;
	LARGE_INTEGER dueTime;

	if ( rakPeer->threadSleepTimer <= 0 )
		rakPeer->threadSleepTimer = 1;

	// 2nd parameter of false means synchronization timer instead of manual-reset timer
	timerHandle = CreateWaitableTimer( nullptr, FALSE, 0 );

	RakAssert( timerHandle );

	dueTime.QuadPart = -10000 * rakPeer->threadSleepTimer; // 10000 is 1 ms?

	BOOL success = SetWaitableTimer( timerHandle, &dueTime, rakPeer->threadSleepTimer, nullptr, nullptr, FALSE );
    (void) success;
	RakAssert( success );

#endif
#endif
*/

	BitStream updateBitStream(MAXIMUM_MTU_SIZE);
	//
	rakPeer->isMainLoopThreadActive = true;

	bool running = true;
	while (running)
	{
		if (rakPeer->endThreads)
		{
			// allow just one more final update-run prior to shutting down this thread to make sure that outstanding acks are still sent and the peers don't unnecessary wait for already retrieved packets
			// note: this fixes part of SLNET-123
			running = false;
		}
		// #ifdef _DEBUG
		// 		// Sanity check, make sure RunUpdateCycle does not block or not otherwise get called for a long time
		// 		RakNetTime thisCall=MafiaNet::GetTime();
		// 		RakAssert(thisCall-lastCall<250);
		// 		lastCall=thisCall;
		// #endif
		if (rakPeer->userUpdateThreadPtr)
			rakPeer->userUpdateThreadPtr(rakPeer, rakPeer->userUpdateThreadData);

		rakPeer->RunUpdateCycle(updateBitStream);

		// Pending sends go out this often, unless quitAndDataEvents is set
		rakPeer->quitAndDataEvents.WaitOnEvent(10);

		/*

// #if ((_WIN32_WINNT >= 0x0400) || (_WIN32_WINDOWS > 0x0400)) &&
#if defined(USE_WAIT_FOR_MULTIPLE_EVENTS) && defined(_WIN32)

		if (rakPeer->threadSleepTimer>0)
		{
			WSAEVENT eventArray[256];
			unsigned int i, eventArrayIndex;
			for (i=0,eventArrayIndex=0; i < rakPeer->socketList.Size(); i++)
			{
				if (rakPeer->socketList[i]->recvEvent!=INVALID_HANDLE_VALUE)
				{
					eventArray[eventArrayIndex]=rakPeer->socketList[i]->recvEvent;
					eventArrayIndex++;
					if (eventArrayIndex==256)
						break;
				}
			}
			WSAWaitForMultipleEvents(eventArrayIndex,(const HANDLE*) &eventArray,FALSE,rakPeer->threadSleepTimer,FALSE);
		}
		else
		{
			RakSleep(0);
		}

#else // ((_WIN32_WINNT >= 0x0400) || (_WIN32_WINDOWS > 0x0400)) && defined(USE_WAIT_FOR_MULTIPLE_EVENTS)
		#pragma message("-- RakNet: Using Sleep(). Uncomment USE_WAIT_FOR_MULTIPLE_EVENTS in defines.h if you want to use WaitForSingleObject instead. --")

		RakSleep( rakPeer->threadSleepTimer );
#endif
		*/
	}

	rakPeer->isMainLoopThreadActive = false;

	/*
#ifdef _WIN32
#if (_WIN32_WINNT >= 0x0400) || (_WIN32_WINDOWS > 0x0400)
	CloseHandle(timerHandle);
#endif
#endif
	*/


	return 0;
}

void RakPeer::CallPluginCallbacks(DataStructures::List<PluginInterface2 *> &pluginList, Packet *packet)
{
	for (unsigned int i = 0; i < pluginList.Size(); i++)
	{
		switch (packet->data[0])
		{
		case ID_DISCONNECTION_NOTIFICATION:
			pluginList[i]->OnClosedConnection(packet->systemAddress, packet->guid, LCR_DISCONNECTION_NOTIFICATION);
			break;
		case ID_CONNECTION_LOST:
			pluginList[i]->OnClosedConnection(packet->systemAddress, packet->guid, LCR_CONNECTION_LOST);
			break;
		case ID_NEW_INCOMING_CONNECTION:
			pluginList[i]->OnNewConnection(packet->systemAddress, packet->guid, true);
			break;
		case ID_CONNECTION_REQUEST_ACCEPTED:
			pluginList[i]->OnNewConnection(packet->systemAddress, packet->guid, false);
			break;
		case ID_CONNECTION_ATTEMPT_FAILED:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_CONNECTION_ATTEMPT_FAILED);
			break;
		case ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY);
			break;
		case ID_OUR_SYSTEM_REQUIRES_SECURITY:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_OUR_SYSTEM_REQUIRES_SECURITY);
			break;
		case ID_PUBLIC_KEY_MISMATCH:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_PUBLIC_KEY_MISMATCH);
			break;
		case ID_ALREADY_CONNECTED:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_ALREADY_CONNECTED);
			break;
		case ID_NO_FREE_INCOMING_CONNECTIONS:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_NO_FREE_INCOMING_CONNECTIONS);
			break;
		case ID_CONNECTION_BANNED:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_CONNECTION_BANNED);
			break;
		case ID_INVALID_PASSWORD:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_INVALID_PASSWORD);
			break;
		case ID_INCOMPATIBLE_PROTOCOL_VERSION:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_INCOMPATIBLE_PROTOCOL);
			break;
		case ID_IP_RECENTLY_CONNECTED:
			pluginList[i]->OnFailedConnectionAttempt(packet, FCAR_IP_RECENTLY_CONNECTED);
			break;
		}
	}
}

void RakPeer::FillIPList(void)
{
	if (ipList[0] != UNASSIGNED_SYSTEM_ADDRESS)
		return;

	// Fill out ipList structure
	RakNetSocket2::GetMyIP(ipList);

	// Sort the addresses from lowest to highest
	int startingIdx = 0;
	while (startingIdx < MAXIMUM_NUMBER_OF_INTERNAL_IDS - 1 && ipList[startingIdx] != UNASSIGNED_SYSTEM_ADDRESS)
	{
		int lowestIdx = startingIdx;
		for (int curIdx = startingIdx + 1; curIdx < MAXIMUM_NUMBER_OF_INTERNAL_IDS - 1 && ipList[curIdx] != UNASSIGNED_SYSTEM_ADDRESS; curIdx++)
		{
			if (ipList[curIdx] < ipList[startingIdx])
			{
				lowestIdx = curIdx;
			}
		}
		if (startingIdx != lowestIdx)
		{
			SystemAddress temp = ipList[startingIdx];
			ipList[startingIdx] = ipList[lowestIdx];
			ipList[lowestIdx] = temp;
		}
		++startingIdx;
	}
}


// #if defined(RMO_NEW_UNDEF_ALLOCATING_QUEUE)
// #pragma pop_macro("new")
// #undef RMO_NEW_UNDEF_ALLOCATING_QUEUE
// #endif

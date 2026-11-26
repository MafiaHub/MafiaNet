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
#include <cstdlib>
#include "mafianet/peer.h"
#include "player_roster.h"
#include "replication_receive_scope.h"
#include "mafianet/connect_candidates.h"
#include "mafianet/session_admission.h"
#include "mafianet/types.h"

#ifndef _WIN32
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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_black_hole.h"
#include "mafianet/vector_util.h"
#include "mafianet/huffman_encoding_tree.h"
#include "mafianet/rand.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/string_compressor.h"
#include "mafianet/string_table.h"
#include "mafianet/network_id_object.h"
#include "mafianet/types.h"
#include "mafianet/sleep.h"
#include "mafianet/assert.h"
#include "mafianet/version.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/get_time_of_day.h"
#include "mafianet/signaled_event.h"
#include "mafianet/super_fast_hash.h"
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

#include <sodium.h>

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

// A message ReliabilityLayer::Receive handed over: a std::malloc'd buffer that RakPeer now owns. It is either
// passed on to a Packet (AllocPacket takes the same malloc'd buffer, freed by DeallocatePacket) or freed here.
namespace
{
struct ReceivedMessageFree
{
	void operator()(unsigned char *bytes) const { std::free(bytes); }
};
using ReceivedMessage = std::unique_ptr<unsigned char, ReceivedMessageFree>;
} // namespace

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

Packet *RakPeer::AllocPacket(unsigned dataSize)
{
	// Crashes when dataSize is 4 bytes - not sure why
	// 	unsigned char *data = (unsigned char *) std::malloc(sizeof(PacketFollowedByData)+dataSize);
	// 	Packet *p = &((PacketFollowedByData *)data)->p;
	// 	p->data=((PacketFollowedByData *)data)->data;
	// 	p->length=dataSize;
	// 	p->bitSize=BYTES_TO_BITS(dataSize);
	// 	p->deleteData=false;
	// 	p->guid=UNASSIGNED_RAKNET_GUID;
	// 	return p;

	MafiaNet::Packet *p = new Packet;
	++packetsOutstanding;
	p->data = (unsigned char *) std::malloc(dataSize);
	p->length = dataSize;
	p->bitSize = BYTES_TO_BITS(dataSize);
	p->deleteData = true;
	p->guid = UNASSIGNED_RAKNET_GUID;
	p->wasGeneratedLocally = false;
	return p;
}

Packet *RakPeer::AllocPacket(unsigned dataSize, unsigned char *data)
{
	MafiaNet::Packet *p = new Packet;
	++packetsOutstanding;
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
    : playerRoster(std::make_unique<PlayerRoster>())
{
	StringCompressor::AddReference();
	MafiaNet::StringTable::AddReference();
	WSAStartupSingleton::AddRef();

	defaultMTUSize = mtuSizes[NUM_MTU_SIZES - 1];
	trackFrequencyTable = false;
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

	_packetloss = 0.0;
	_minExtraPing = 0;
	_extraPingVariance = 0;

	packetsOutstanding = 0;
	shutdownGeneration = 0;
	remoteSystemLookupSize = 0;
	liveRemoteSystemIndexEntries = 0;


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
	// The identity key outlives Shutdown() (a restart keeps signing), but not the peer.
	ClearIdentityKey();

	// Free the ban list.
	ClearBanList();

	StringCompressor::RemoveReference();
	MafiaNet::StringTable::RemoveReference();
	WSAStartupSingleton::Deref();

	quitAndDataEvents.CloseEvent();


	// 	for (unsigned int i=0; i < pluginListTS.size(); i++)
	// 		pluginListTS[i]->SetRakPeerInterface(0);
	// 	for (unsigned int i=0; i < pluginListNTS.size(); i++)
	// 		pluginListNTS[i]->SetRakPeerInterface(0);
}

std::optional<PlayerId> RakPeer::AcceptPlayer(PeerGuid guid)
{
	return playerRoster->Accept(guid);
}

std::optional<PlayerId> RakPeer::GetPlayerId(PeerGuid guid) const
{
	return playerRoster->FindId(guid);
}

std::optional<PeerGuid> RakPeer::GetPlayerGuid(PlayerId id) const
{
	return playerRoster->FindPeer(id);
}

void RakPeer::GetPlayerList(std::vector<std::pair<PlayerId, PeerGuid>> &out) const
{
	playerRoster->GetPlayerList(out);
}

RPC &RakPeer::GetRPC()
{
	rpc.RegisterGlobals();
	return rpc;
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
			RNS2BindResult br = ((RNS2_Berkley *) r2)->Bind(&bbp);

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

		socketList.push_back(r2);
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
		const auto capacity = playerRoster->Capacity();
		if (capacity.connected > maxConnections)
			playerRoster->SetIncoming(maxConnections, capacity.reserved);

		maximumNumberOfPeers = maxConnections;
		// 04/19/2006 - Don't overallocate because I'm no longer allowing connected pings.
		// The disconnects are not consistently processed and the process was sloppy and complicated.
		// Allocate 10% extra to handle new connections from players trying to connect when the server is full
		//remoteSystemListSize = maxConnections;// * 11 / 10 + 1;

		// remoteSystemList in Single thread
		//remoteSystemList = new RemoteSystemStruct[ remoteSystemListSize ];
		remoteSystemList = new RemoteSystemStruct[maximumNumberOfPeers];

		remoteSystemLookupSize = (unsigned int) maximumNumberOfPeers * REMOTE_SYSTEM_LOOKUP_HASH_MULTIPLE;
		remoteSystemLookup = new RemoteSystemIndex *[remoteSystemLookupSize];

		activeSystemList = new RemoteSystemStruct *[maximumNumberOfPeers];

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
			// One-time zero-init: the array is new[] allocated with no member init, so prime the
			// reason-payload fields here before ClearDisconnectReason() can ever free them.
			remoteSystemList[i].disconnectReasonData = 0;
			remoteSystemList[i].disconnectReasonLength = 0;
			remoteSystemList[i].remoteSessionConfigData = 0;
			remoteSystemList[i].remoteSessionConfigLength = 0;
			remoteSystemList[i].withheldConnectionPacketData = 0;
			remoteSystemList[i].withheldConnectionPacketLength = 0;
			remoteSystemList[i].heldApplicationBytes = 0;
			remoteSystemList[i].sessionConfigAwaitingLocalDecision = false;
			remoteSystemList[i].sessionConfigIsConnectingSide = false;
			remoteSystemList[i].connectionReportedToApplication = false;
			remoteSystemList[i].absorbedOurConnectAttempt = false;
			remoteSystemList[i].sessionConfigStartTime = 0;
			remoteSystemList[i].reliabilityLayer.ApplyNetworkSimulator(_packetloss, _minExtraPing, _extraPingVariance);

			// All entries in activeSystemList have valid pointers all the time.
			activeSystemList[i] = &remoteSystemList[i];
		}

		for (i = 0; i < remoteSystemLookupSize; i++)
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
				RakPeerAndIndex *rpai = new RakPeerAndIndex;
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

	rpc.RegisterGlobals();
	for (i = 0; i < pluginListTS.size(); i++)
	{
		pluginListTS[i]->OnRakPeerStartup();
	}

	for (i = 0; i < pluginListNTS.size(); i++)
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
bool RakPeer::SetIdentityKey(const uint8_t secretKey[64])
{
	// The network thread reads the key while started (it signs every REPLY_2): it only changes while offline.
	if (IsActive())
		return false;
	if (secretKey == nullptr)
	{
		ClearIdentityKey();
		return true;
	}
	uint8_t publicKey[kIdentityPublicKeyBytes];
	if (!IdentityPublicKeyFromSecret(secretKey, publicKey))
		return false;
	ClearIdentityKey(); // wipe the key being replaced
	memcpy(identitySecretKey, secretKey, sizeof(identitySecretKey));
	memcpy(identityPublicKey, publicKey, sizeof(identityPublicKey)); // validated once, reused for every REPLY_2
	hasIdentityKey = true;
	return true;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeerInterface::GenerateIdentityKeyPair(uint8_t publicKey[32], uint8_t secretKey[64])
{
	MafiaNet::GenerateIdentityKeyPair(publicKey, secretKey);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::GetIdentityPublicKey(uint8_t out[32]) const
{
	if (!hasIdentityKey)
	{
		memset(out, 0, kIdentityPublicKeyBytes);
		return false;
	}
	memcpy(out, identityPublicKey, kIdentityPublicKeyBytes);
	return true;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::GetRemoteIdentityKey(const AddressOrGUID systemIdentifier, uint8_t out[32]) const
{
	const RemoteSystemStruct *remoteSystem = GetRemoteSystem(systemIdentifier, false, true);
	if (remoteSystem == 0 || !remoteSystem->hasRemoteIdentityKey)
	{
		memset(out, 0, kIdentityPublicKeyBytes);
		return false;
	}
	memcpy(out, remoteSystem->remoteIdentityKey, kIdentityPublicKeyBytes);
	return true;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearIdentityKey()
{
	sodium_memzero(identitySecretKey, sizeof(identitySecretKey));
	memset(identityPublicKey, 0, sizeof(identityPublicKey));
	hasIdentityKey = false;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearHandshakeIdentity(RemoteSystemStruct *remoteSystem)
{
	if (remoteSystem == 0)
		return;
	remoteSystem->handshakeLocalIdentity = IdentityBlock();
	remoteSystem->hasRemoteIdentityKey = false;
	memset(remoteSystem->remoteIdentityKey, 0, sizeof(remoteSystem->remoteIdentityKey));
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
	for (unsigned i = 0; i < requestedConnectionQueue.size(); i++)
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

unsigned int RakPeer::GetLiveRemoteSystemIndexEntriesForTesting() const
{
	return liveRemoteSystemIndexEntries;
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
void RakPeer::SetMaximumIncomingConnections(unsigned short numberAllowed, unsigned short reserved)
{
	playerRoster->SetIncoming(numberAllowed, reserved);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Returns the maximum number of incoming connections, which is always <= maxConnections
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetMaximumIncomingConnections(void) const
{
	return playerRoster->Capacity().connected;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Returns how many open connections there are at this time
// \return the number of open connections
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned short RakPeer::NumberOfConnections(void) const
{
	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	GetSystemList(addresses, guids);
	return (unsigned short) addresses.size();
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
	if (host == 0 || endThreads || connectionSocketIndex >= socketList.size())
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

	// A non-null publicKey pins the server identity (checked in the REPLY_2 handler).
	return SendConnectionRequest(host, remotePort, passwordData, passwordDataLength, connectionSocketIndex, 0, sendConnectionAttemptCount, timeBetweenSendConnectionAttemptsMS, timeoutTime, publicKey != 0 ? publicKey->serverIdentity : nullptr);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ConnectionAttemptResult RakPeer::ConnectWithSocket(const char *host, unsigned short remotePort, const char *passwordData, int passwordDataLength, RakNetSocket2 *socket, PublicKey *publicKey, unsigned sendConnectionAttemptCount, unsigned timeBetweenSendConnectionAttemptsMS, MafiaNet::TimeMS timeoutTime)
{
	if (host == 0 || endThreads || socket == 0)
		return INVALID_PARAMETER;

	if (passwordDataLength > 255)
		passwordDataLength = 255;

	if (passwordData == 0)
		passwordDataLength = 0;

	// A non-null publicKey pins the server identity, as in Connect().
	return SendConnectionRequest(host, remotePort, passwordData, passwordDataLength, 0, 0, sendConnectionAttemptCount, timeBetweenSendConnectionAttemptsMS, timeoutTime, socket, publicKey != 0 ? publicKey->serverIdentity : nullptr);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Description:
// Stops the network threads and close all connections.  Multiple calls are ok.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::Shutdown(unsigned int blockDuration, unsigned char orderingChannel, MafiaNet::Priority disconnectionNotificationPriority)
{
	if (ReplicationReceiveScope::DeferShutdown(*this, blockDuration, orderingChannel, disconnectionNotificationPriority))
		return;

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
	replication.Shutdown();

	for (i = 0; i < pluginListTS.size(); i++)
	{
		pluginListTS[i]->OnRakPeerShutdown();
	}
	for (i = 0; i < pluginListNTS.size(); i++)
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

	for (i = 0; i < socketList.size(); i++)
	{
		if (socketList[i]->IsBerkleySocket())
		{
			((RNS2_Berkley *) socketList[i])->SignalStopRecvPollingThread();
		}
	}

	/*
	// Get recvfrom to unblock
	for (i=0; i < socketList.size(); i++)
	{
		if (SocketLayer::SendTo(socketList[i], (const char*) &i,1,socketList[i]->GetBoundAddress())!=0)
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
		for (i=0; i < socketList.size(); i++)
		{
			SocketLayer::SendTo(socketList[i], (const char*) &i,1,socketList[i]->GetBoundAddress());
		}

		RakSleep(30);
	}
	*/

	for (i = 0; i < socketList.size(); i++)
	{
		if (socketList[i]->IsBerkleySocket())
		{
			((RNS2_Berkley *) socketList[i])->BlockOnStopRecvPollingThread();
		}
	}


#endif // RAKPEER_USER_THREADED!=1

	std::atomic_store(&connectedSystems, std::shared_ptr<const ConnectedSystems>{});
	playerRoster->Reset();

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
		ClearHandshakeIdentity(&remoteSystemList[i]);
	}


	// Setting maximumNumberOfPeers to 0 allows remoteSystemList to be reallocated in Initialize.
	// Setting remoteSystemListSize prevents threads from accessing the reliability layer
	maximumNumberOfPeers = 0;
	//remoteSystemListSize = 0;

	// Free any packets the user didn't deallocate
	packetReturnMutex.Lock();
	for (Packet *queued : packetReturnQueue)
		DeallocatePacket(queued);
	packetReturnQueue.clear();
	cancelledAttempts.clear();
	withdrawnConnections.clear();
	reportedRaceAttempts.clear();
	packetReturnMutex.Unlock();
	// Packets still out (a Shutdown() from inside Receive's dispatch, or from code handling a
	// packet Receive returned) are individually allocated, so they stay valid until
	// DeallocatePacket frees them.

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
	delete[] temp;
	delete[] activeSystemList;
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

	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	GetSystemList(addresses, guids);
	if (remoteSystems)
	{
		unsigned short i;
		for (i = 0; i < *numberOfSystems && i < addresses.size(); i++)
			remoteSystems[i] = addresses[i];
		*numberOfSystems = i;
	}
	else
	{
		*numberOfSystems = (unsigned short) addresses.size();
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

RakPeer::ConnectedSystem RakPeer::FindDatagramTarget(const AddressOrGUID target) const
{
	const auto systems = std::atomic_load(&connectedSystems);
	if (systems && !target.IsUndefined())
	{
		const auto matches = [&](const ConnectedSystem &entry)
		{ return target.rakNetGuid != UNASSIGNED_RAKNET_GUID ? entry.guid == target.rakNetGuid : entry.address == target.systemAddress; };
		// The dense list commonly retains slot order. Validate the hint after removals or reuse.
		const auto hint = target.GetSystemIndex();
		if (hint < systems->size() && matches((*systems)[hint]))
			return (*systems)[hint];
		const auto found = std::find_if(systems->begin(), systems->end(), matches);
		if (found != systems->end())
			return *found;
	}
	return {UNASSIGNED_SYSTEM_ADDRESS, UNASSIGNED_RAKNET_GUID, 0};
}

unsigned RakPeer::GetMaxUnfragmentedPayload(const AddressOrGUID target, Reliability reliability) const
{
	return ReliabilityLayer::MaxUnfragmentedPayload(FindDatagramTarget(target).mtu, encryptionMode, reliability);
}

DatagramSendResult RakPeer::SendDatagram(const char *data, int length, Priority priority, Reliability reliability, char orderingChannel, const AddressOrGUID target)
{
	if (!data || length <= 0)
		return {DatagramSendStatus::InvalidArgument};
	return QueueDatagram(data, BYTES_TO_BITS(static_cast<uint64_t>(length)), priority, reliability, orderingChannel, target);
}

DatagramSendResult RakPeer::SendDatagram(const BitStream &data, Priority priority, Reliability reliability, char orderingChannel, const AddressOrGUID target)
{
	return QueueDatagram(reinterpret_cast<const char *>(data.GetData()), data.GetNumberOfBitsUsed(), priority, reliability, orderingChannel, target);
}

DatagramSendResult RakPeer::QueueDatagram(const char *data, uint64_t bits, Priority priority, Reliability reliability, char orderingChannel, const AddressOrGUID target)
{
	if (!bits || static_cast<unsigned>(priority) >= NUMBER_OF_PRIORITIES || static_cast<unsigned char>(orderingChannel) >= NUMBER_OF_ORDERED_STREAMS ||
	    (reliability != Reliability::Unreliable && reliability != Reliability::UnreliableSequenced && reliability != Reliability::UnreliableWithAckReceipt))
		return {DatagramSendStatus::InvalidArgument};
	const auto remote = FindDatagramTarget(target);
	if (!remote.mtu)
		return {DatagramSendStatus::NotConnected};
	if (bits > BYTES_TO_BITS(ReliabilityLayer::MaxUnfragmentedPayload(remote.mtu, encryptionMode, reliability)))
		return {DatagramSendStatus::TooLarge};
	const auto receipt = IncrementNextSendReceipt();
	if (!SendBuffered(data, static_cast<BitSize_t>(bits), priority, reliability, orderingChannel, remote.guid, false, RemoteSystemStruct::NO_ACTION, receipt, false))
		return {DatagramSendStatus::OutOfMemory};
	return {DatagramSendStatus::Queued, receipt};
}

void RakPeer::SendLoopback(const char *data, const int length)
{
	if (data == 0 || length < 0)
		return;

	Packet *packet = AllocPacket(length);
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
	if (ReplicationReceiveScope::IsClosing(*this))
		return nullptr;

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
	for (i=0; i < socketList.size(); i++)
	{
		for(;;)
		{
			recvFromStruct=bufferedPackets.Allocate();
			recvFromStruct->s=socketList[i]->s;
			recvFromStruct->extraSocketOptions=socketList[i]->extraSocketOptions;
			SocketLayer::RecvFromBlocking(
				recvFromStruct->s, this, recvFromStruct->extraSocketOptions, recvFromStruct->data, &recvFromStruct->bytesRead, &recvFromStruct->systemAddress, &recvFromStruct->timeRead);
			if (recvFromStruct->bytesRead<=0)
			{
				bufferedPackets.Deallocate(recvFromStruct);
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

	const unsigned int tickGeneration = shutdownGeneration;
	replication.ReceiveTick();
	if (shutdownGeneration != tickGeneration)
		return nullptr;

	for (i = 0; i < pluginListTS.size(); i++)
	{
		pluginListTS[i]->Update();
	}
	for (i = 0; i < pluginListNTS.size(); i++)
	{
		pluginListNTS[i]->Update();
	}

	do
	{
		packetReturnMutex.Lock();
		if (packetReturnQueue.empty())
			packet = 0;
		else
		{
			packet = packetReturnQueue.front();
			packetReturnQueue.pop_front();
		}
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

		for (i = 0; i < pluginListTS.size(); i++)
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

		for (i = 0; packet && i < pluginListNTS.size(); i++)
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

		if (packet)
		{
			const bool consumed = replication.ProcessPacket(packet) || (shutdownGeneration == generation && rpc.ProcessPacket(packet));
			if (consumed || shutdownGeneration != generation)
			{
				DeallocatePacket(packet);
				packet = nullptr;
			}
			if (shutdownGeneration != generation)
				return nullptr;
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
		std::free(packet->data);
		delete packet;
		--packetsOutstanding;
	}
	else
	{
		std::free(packet);
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
	if (closeSocket == nullptr && socketList.size() > 0)
		closeSocket = socketList[0];
	if (closeSocket == nullptr)
		return;
	CloseConnectionInternal2(target, sendDisconnectionNotification, false, orderingChannel, disconnectionNotificationPriority, *closeSocket, reasonData);
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
	for (i = 0; i < requestedConnectionQueue.size(); i++)
	{
		RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (RequestMatches(rcs, target))
		{
			rcs->cancelled = true;
			if (rcs->raceGroup != 0)
			{
				for (unsigned int k = 0; k < requestedConnectionQueue.size(); k++)
				{
					if (requestedConnectionQueue[k]->raceGroup == rcs->raceGroup)
						requestedConnectionQueue[k]->cancelled = true;
				}
			}
			break;
		}
	}
	requestedConnectionQueueMutex.Unlock();

	// An attempt that already holds a slot (transport handshake, or waiting for the server's session decision)
	// is the network thread's to drop (ApplyCancelledAttempts). Its acceptance may also be produced already
	// without the application having read it: take it back, with whatever the connection queued behind it,
	// so the application never sees the connection. Under the queue lock, which the connecting side also holds
	// to check for a cancel and queue the acceptance, so either it is in the queue here or it is never queued.
	std::vector<Packet *> withdrawn;
	packetReturnMutex.Lock();
	cancelledAttempts.push_back(target);
	for (std::size_t q = 0; q < packetReturnQueue.size();)
	{
		const Packet *accepted = packetReturnQueue[q];
		bool matches = accepted->length > 0 && accepted->data[0] == ID_CONNECTION_REQUEST_ACCEPTED && accepted->systemAddress == target;
		for (const auto &race : reportedRaceAttempts)
			matches = matches || (accepted->length > 0 && accepted->data[0] == ID_CONNECTION_REQUEST_ACCEPTED && race.first == accepted->guid && race.second == target);
		if (!matches)
		{
			q++;
			continue;
		}
		// The acceptance and everything its connection queued after it; the next packet then sits at q.
		const RakNetGUID guid = accepted->guid;
		withdrawnConnections.push_back(guid);
		for (std::size_t later = q; later < packetReturnQueue.size();)
		{
			if (packetReturnQueue[later]->guid == guid)
			{
				withdrawn.push_back(packetReturnQueue[later]);
				packetReturnQueue.erase(packetReturnQueue.begin() + (std::ptrdiff_t) later);
			}
			else
				later++;
		}
	}
	packetReturnMutex.Unlock();
	for (Packet *packet : withdrawn)
		DeallocatePacket(packet);
	quitAndDataEvents.SetEvent();
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
		std::free(rcs->data);
		rcs->data = 0;
	}
	if (rcs->ephemeral)
	{
		// The destructor wipes too; wiping first keeps the secret's lifetime obvious.
		rcs->ephemeral->Wipe();
		delete rcs->ephemeral;
		rcs->ephemeral = nullptr;
	}
	delete rcs;
}

void RakPeer::RemoveRequestedConnection_NoLock(const RequestedConnectionStruct *rcs)
{
	for (unsigned int i = 0; i < requestedConnectionQueue.size(); i++)
	{
		if (requestedConnectionQueue[i] == rcs)
		{
			requestedConnectionQueue.erase(requestedConnectionQueue.begin() + i);
			return;
		}
	}
}

void RakPeer::CancelRaceSiblings_NoLock(const RequestedConnectionStruct *keep)
{
	if (keep->raceGroup == 0)
		return;
	unsigned int i = 0;
	while (i < requestedConnectionQueue.size())
	{
		RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (rcs != keep && rcs->raceGroup == keep->raceGroup)
		{
			requestedConnectionQueue.erase(requestedConnectionQueue.begin() + i);
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
	for (unsigned int i = 0; i < requestedConnectionQueue.size(); i++)
	{
		RequestedConnectionStruct *rcs = requestedConnectionQueue[i];
		if (rcs->systemAddress == address && !rcs->cancelled && rcs->actionToTake == RequestedConnectionStruct::CONNECT)
		{
			// A pinned attempt is never absorbed into an incoming connection: that connection's remote was never
			// asked to prove the pinned identity. The REQUEST_2 handler does not create one while a pinned attempt
			// is pending; if one exists anyway, the pinned attempt runs on and ends as an attempt normally does.
			if (!rcs->pinnedIdentity)
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
	for (unsigned int i = 0; i < requestedConnectionQueue.size(); i++)
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
		for (unsigned int i = 0; i < requestedConnectionQueue.size(); i++)
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
		for (; i < requestedConnectionQueue.size(); i++)
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
// Copies one published snapshot of connected addresses and GUIDs.
// Indices match each other, so \a addresses[0] and \a guids[0] refer to the same system
// \param[out] addresses All system addresses. Size of the list is the number of connections. Size of the list will match the size of the \a guids list.
// \param[out] guids All guids. Size of the list is the number of connections. Size of the list will match the size of the \a addresses list.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetSystemList(std::vector<SystemAddress> &addresses, std::vector<RakNetGUID> &guids) const
{
	addresses.clear();
	guids.clear();

	const auto snapshot = std::atomic_load(&connectedSystems);
	if (!snapshot)
		return;

	addresses.reserve(snapshot->size());
	guids.reserve(snapshot->size());
	for (const auto &entry : *snapshot)
	{
		addresses.push_back(entry.address);
		guids.push_back(entry.guid);
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

	for (; index < banList.size(); index++)
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

	BanStruct *banStruct = new BanStruct;
	banStruct->IP = (char *) std::malloc(16);
	if (milliseconds == 0)
		banStruct->timeout = 0; // Infinite
	else
		banStruct->timeout = time + milliseconds;
	strcpy_s(banStruct->IP, 16, IP);
	banListMutex.Lock();
	banList.push_back(banStruct);
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

	for (; index < banList.size(); index++)
	{
		if (strcmp(IP, banList[index]->IP) == 0)
		{
			temp = banList[index];
			SwapRemove(banList, index);
			break;
		}
	}

	banListMutex.Unlock();

	if (temp)
	{
		std::free(temp->IP);
		delete temp;
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

	for (; index < banList.size(); index++)
	{
		std::free(banList[index]->IP);
		delete banList[index];
	}

	banList.clear();

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

	if (banList.size() == 0)
		return false; // Skip the mutex if possible

	time = MafiaNet::GetTimeMS();

	banListMutex.Lock();

	while (banListIndex < banList.size())
	{
		if (banList[banListIndex]->timeout > 0 && banList[banListIndex]->timeout < time)
		{
			// Delete expired ban
			temp = banList[banListIndex];
			SwapRemove(banList, banListIndex);
			std::free(temp->IP);
			delete temp;
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
	RakAssert(connectionSocketIndex < socketList.size());

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
	for (i=0; i < pluginListNTS.size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char*)bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), systemAddress);
	SocketLayer::SendTo( socketList[realIndex], (const char*)bitStream.GetData(), (int) bitStream.GetNumberOfBytesUsed(), systemAddress);
	*/

	RNS2_SendParameters bsp;
	bsp.data = (char *) bitStream.GetData();
	bsp.length = bitStream.GetNumberOfBytesUsed();
	if (!ResolveOneShotTarget(host, remotePort, socketList[realIndex], &bsp.systemAddress) ||
	    bsp.systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
		return false;
	unsigned i;
	for (i = 0; i < pluginListNTS.size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char *) bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), bsp.systemAddress);
	socketList[realIndex]->Send(&bsp);

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
SessionAcceptResult RakPeer::AcceptSession(const AddressOrGUID systemIdentifier, const char *data, unsigned int length, ConnectionSlot slot)
{
	return QueueSessionCommand(systemIdentifier, detail::SessionDecision::Accept, data, length, 0, slot);
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
	QueueSessionCommand(systemIdentifier, detail::SessionDecision::Reject, reason, reasonLength);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetMaximumPendingSessions(unsigned short total, unsigned short perAddress)
{
	playerRoster->SetPending(total, perAddress);
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
	QueueSessionCommand(systemIdentifier, detail::SessionDecision::Status, data, length);
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Hand an application's accept/reject over to the network thread. Applying it here would send on the
// connection and mutate connectMode from the user thread, which the rest of RakPeer never does.
SessionAcceptResult RakPeer::QueueSessionCommand(const AddressOrGUID systemIdentifier, detail::SessionDecision decision, const char *data, unsigned int length, uint64_t generation, ConnectionSlot slot)
try
{
	if (length > MAXIMUM_SESSION_CONFIG_SIZE || (length && !data))
		return SessionAcceptResult::InvalidPayload;
	auto bcs = std::make_unique<BufferedCommandStruct>();
	switch (decision)
	{
	case detail::SessionDecision::Accept:
		bcs->command = BufferedCommandStruct::BCS_SESSION_ACCEPT;
		break;
	case detail::SessionDecision::Reject:
		bcs->command = BufferedCommandStruct::BCS_SESSION_REJECT;
		break;
	case detail::SessionDecision::Status:
		bcs->command = BufferedCommandStruct::BCS_SESSION_STATUS;
		break;
	}
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
		bcs->data = (char *) std::malloc(length);
		if (bcs->data)
			memcpy(bcs->data, data, length);
		else
			return SessionAcceptResult::OutOfMemory;
	}
	const auto result = playerRoster->Answer(systemIdentifier, generation, decision, slot, [&](uint64_t currentGeneration)
	                                         {
		bcs->generation = currentGeneration;
		bufferedCommands.Push(std::move(bcs)); });
	if (result == SessionAcceptResult::Accepted)
		quitAndDataEvents.SetEvent();
	return result;
}
catch (const std::bad_alloc &)
{
	return SessionAcceptResult::OutOfMemory;
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
	std::vector<RakNetSocket2 *> sockets;
	GetSockets(sockets);
	if (sockets.size() > 0)
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
		for (i = 0; i < pluginListNTS.size(); i++)
			pluginListNTS[i]->OnDirectSocketSend((const char *) bsp.data, BYTES_TO_BITS(bsp.length), bsp.systemAddress);
		socketList[realIndex]->Send(&bsp);
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
		if (std::find(pluginListNTS.begin(), pluginListNTS.end(), plugin) == pluginListNTS.end())
		{
			plugin->SetRakPeerInterface(this);
			plugin->OnAttach();
			pluginListNTS.push_back(plugin);
		}
	}
	else
	{
		if (std::find(pluginListTS.begin(), pluginListTS.end(), plugin) == pluginListTS.end())
		{
			plugin->SetRakPeerInterface(this);
			plugin->OnAttach();
			pluginListTS.push_back(plugin);
		}
	}

	pluginListNetworkThreadMutex.Lock();
	if (std::find(pluginListNetworkThread.begin(), pluginListNetworkThread.end(), plugin) == pluginListNetworkThread.end())
		pluginListNetworkThread.push_back(plugin);
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

	bool isNotThreadsafe = plugin->UsesReliabilityLayer();
	if (isNotThreadsafe)
	{
		const auto it = std::find(pluginListNTS.begin(), pluginListNTS.end(), plugin);
		if (it != pluginListNTS.end())
		{
			// Unordered list so delete from end for speed
			SwapRemove(pluginListNTS, (std::size_t)(it - pluginListNTS.begin()));
		}
	}
	else
	{
		const auto it = std::find(pluginListTS.begin(), pluginListTS.end(), plugin);
		if (it != pluginListTS.end())
		{
			// Unordered list so delete from end for speed
			SwapRemove(pluginListTS, (std::size_t)(it - pluginListTS.begin()));
		}
	}
	// Removed under the lock the update thread dispatches under, so once this returns the plugin
	// is not inside OnReceiveNetworkThread and will not be called again.
	pluginListNetworkThreadMutex.Lock();
	const auto networkThreadIt = std::find(pluginListNetworkThread.begin(), pluginListNetworkThread.end(), plugin);
	if (networkThreadIt != pluginListNetworkThread.end())
		SwapRemove(pluginListNetworkThread, (std::size_t)(networkThreadIt - pluginListNetworkThread.begin()));
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
	for (i = 0; i < pluginListTS.size(); i++)
		pluginListTS[i]->OnPushBackPacket((const char *) packet->data, packet->bitSize, packet->systemAddress);
	for (i = 0; i < pluginListNTS.size(); i++)
		pluginListNTS[i]->OnPushBackPacket((const char *) packet->data, packet->bitSize, packet->systemAddress);

	packetReturnMutex.Lock();
	if (pushAtHead)
		packetReturnQueue.push_front(packet);
	else
		packetReturnQueue.push_back(packet);
	packetReturnCondition.notify_all();
	packetReturnMutex.Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ChangeSystemAddress(RakNetGUID guid, const SystemAddress &systemAddress)
{
	auto bcs = std::make_unique<BufferedCommandStruct>();
	bcs->systemIdentifier.systemAddress = systemAddress;
	bcs->systemIdentifier.rakNetGuid = guid;
	bcs->command = BufferedCommandStruct::BCS_CHANGE_SYSTEM_ADDRESS;
	bufferedCommands.Push(std::move(bcs));
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
Packet *RakPeer::AllocatePacket(unsigned dataSize)
{
	return AllocPacket(dataSize);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RakNetSocket2 *RakPeer::GetSocket(const SystemAddress target)
{
	// Send a query to the thread to get the socket, and return when we got it
	auto bcs = std::make_unique<BufferedCommandStruct>();
	bcs->command = BufferedCommandStruct::BCS_GET_SOCKET;
	bcs->systemIdentifier = target;
	bufferedCommands.Push(std::move(bcs));

	// Block up to one second to get the socket, although it should actually take virtually no time
	MafiaNet::TimeMS stopWaiting = MafiaNet::GetTimeMS() + 1000;
	while (manualUpdate || MafiaNet::GetTimeMS() < stopWaiting)
	{
		if (manualUpdate)
			RunManualUpdate(); // answers the query on this thread
		else if (isMainLoopThreadActive == false)
			return 0;
		else
			RakSleep(0);

		if (auto sqo = socketQueryOutput.TryPop())
		{
			if ((*sqo)->sockets.empty() == false)
				return (*sqo)->sockets[0];
			break;
		}
	}
	return 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::GetSockets(std::vector<RakNetSocket2 *> &sockets)
{
	sockets.clear();

	// Send a query to the thread to get the socket, and return when we got it
	auto bcs = std::make_unique<BufferedCommandStruct>();
	bcs->command = BufferedCommandStruct::BCS_GET_SOCKET;
	bcs->systemIdentifier = UNASSIGNED_SYSTEM_ADDRESS;
	bufferedCommands.Push(std::move(bcs));

	// Block up to one second to get the socket, although it should actually take virtually no time
	for (;;)
	{
		if (manualUpdate)
			RunManualUpdate(); // answers the query on this thread
		else if (isMainLoopThreadActive == false)
			return;
		else
			RakSleep(0);

		if (auto sqo = socketQueryOutput.TryPop())
		{
			sockets = (*sqo)->sockets;
			return;
		}
	}
	return;
}
void RakPeer::ReleaseSockets(std::vector<RakNetSocket2 *> &sockets)
{
	sockets.clear();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Adds simulated ping and packet loss to the outgoing data flow.
// To simulate bi-directional ping and packet loss, you should call this on both the sender and the recipient, with half the total ping and maxSendBPS value on each.
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ApplyNetworkSimulator(float packetloss, unsigned short minExtraPing, unsigned short extraPingVariance)
{
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
	return _packetloss > 0 || _minExtraPing > 0 || _extraPingVariance > 0;
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
	RakAssert(connectionSocketIndex < socketList.size());

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
	for (i=0; i < pluginListNTS.size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char*)bitStream.GetData(), bitStream.GetNumberOfBitsUsed(), systemAddress);
	
	SocketLayer::SendTo( socketList[realIndex], (const char*)bitStream.GetData(), (int) bitStream.GetNumberOfBytesUsed(), systemAddress);
	*/

	RNS2_SendParameters bsp;
	bsp.data = (char *) bitStream.GetData();
	bsp.length = bitStream.GetNumberOfBytesUsed();
	if (!ResolveOneShotTarget(host, remotePort, socketList[realIndex], &bsp.systemAddress))
		return false;
	unsigned i;
	for (i = 0; i < pluginListNTS.size(); i++)
		pluginListNTS[i]->OnDirectSocketSend((const char *) bsp.data, BYTES_TO_BITS(bsp.length), bsp.systemAddress);
	socketList[realIndex]->Send(&bsp);

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
void RakPeer::GetStatisticsList(std::vector<SystemAddress> &addresses, std::vector<RakNetGUID> &guids, std::vector<RakNetStatistics> &statistics)
{
	addresses.clear();
	guids.clear();
	statistics.clear();

	if (remoteSystemList == 0 || endThreads == true)
		return;

	unsigned int i;
	for (i = 0; i < activeSystemListSize; i++)
	{
		if ((activeSystemList[i])->isActive &&
		    (activeSystemList[i])->connectMode == RakPeer::RemoteSystemStruct::CONNECTED)
		{
			addresses.push_back((activeSystemList[i])->systemAddress);
			guids.push_back((activeSystemList[i])->guid);
			RakNetStatistics rns;
			(activeSystemList[i])->reliabilityLayer.GetStatistics(&rns);
			statistics.push_back(rns);
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
	                               { return packetReturnQueue.empty() == false || IsActive() == false; });
	const bool packetsWaiting = packetReturnQueue.empty() == false;
	lock.unlock();
	return packetsWaiting;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetReceiveBufferSize(void)
{
	unsigned int size;
	packetReturnMutex.Lock();
	size = static_cast<unsigned int>(packetReturnQueue.size()); // the interface returns unsigned int; the queue never nears 2^32 packets
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
ConnectionAttemptResult RakPeer::SendConnectionRequest(const char *host, unsigned short remotePort, const char *passwordData, int passwordDataLength, unsigned connectionSocketIndex, unsigned int extraData, unsigned sendConnectionAttemptCount, unsigned timeBetweenSendConnectionAttemptsMS, MafiaNet::TimeMS timeoutTime, const uint8_t *pinnedServerIdentity)
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
		//RequestedConnectionStruct *rcs = (RequestedConnectionStruct *) std::malloc(sizeof(RequestedConnectionStruct));
		RequestedConnectionStruct *rcs = new RequestedConnectionStruct;

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
		if (pinnedServerIdentity != nullptr)
		{
			rcs->pinnedIdentity = true;
			memcpy(rcs->pinnedServerIdentity, pinnedServerIdentity, sizeof(rcs->pinnedServerIdentity));
		}
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
	for (unsigned int i = 0; i < requestedConnectionQueue.size(); i++)
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
			requests[c]->ephemeral = new EphemeralKeyPair;
			requests[c]->handshakeNonce = requests[c]->ephemeral->MakeCryptInfo().nonce;
		}
		else
			randombytes_buf(&requests[c]->handshakeNonce, sizeof(requests[c]->handshakeNonce)); // echoed by REPLY_2, so unpredictable here too
		requestedConnectionQueue.push_back(requests[c]);
	}
	requestedConnectionQueueMutex.Unlock();

	return CONNECTION_ATTEMPT_STARTED;
}
ConnectionAttemptResult RakPeer::SendConnectionRequest(const char *host, unsigned short remotePort, const char *passwordData, int passwordDataLength, unsigned connectionSocketIndex, unsigned int extraData, unsigned sendConnectionAttemptCount, unsigned timeBetweenSendConnectionAttemptsMS, MafiaNet::TimeMS timeoutTime, RakNetSocket2 *socket, const uint8_t *pinnedServerIdentity)
{
	RakAssert(passwordDataLength <= 256);
	SystemAddress systemAddress;
	// Resolve in the family this socket can reach, as Connect() does.
	systemAddress.FromStringExplicitPort(host, remotePort, ResolveIPVersion(socket));

	// Already connected?
	if (GetRemoteSystemFromSystemAddress(systemAddress, false, true))
		return ALREADY_CONNECTED_TO_ENDPOINT;

	//RequestedConnectionStruct *rcs = (RequestedConnectionStruct *) std::malloc(sizeof(RequestedConnectionStruct));
	RequestedConnectionStruct *rcs = new RequestedConnectionStruct;

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
	if (pinnedServerIdentity != nullptr)
	{
		rcs->pinnedIdentity = true;
		memcpy(rcs->pinnedServerIdentity, pinnedServerIdentity, sizeof(rcs->pinnedServerIdentity));
	}

	// Return false if already pending, else push on queue
	unsigned int i = 0;
	requestedConnectionQueueMutex.Lock();
	for (; i < requestedConnectionQueue.size(); i++)
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
		rcs->ephemeral = new EphemeralKeyPair;
		rcs->handshakeNonce = rcs->ephemeral->MakeCryptInfo().nonce;
	}
	else
		randombytes_buf(&rcs->handshakeNonce, sizeof(rcs->handshakeNonce)); // echoed by REPLY_2, so unpredictable here too
	requestedConnectionQueue.push_back(rcs);
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
		SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY);
		return;
	}

	// OK
	SetConnectMode(*remoteSystem, RemoteSystemStruct::HANDLING_CONNECTION_REQUEST);

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
		SetConnectMode(*rss, RemoteSystemStruct::DISCONNECT_ASAP);
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
		std::free(remoteSystem->disconnectReasonData);
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
		std::free(remoteSystem->remoteSessionConfigData);
		remoteSystem->remoteSessionConfigData = 0;
	}
	remoteSystem->remoteSessionConfigLength = 0;
	if (remoteSystem->withheldConnectionPacketData != 0)
	{
		std::free(remoteSystem->withheldConnectionPacketData);
		remoteSystem->withheldConnectionPacketData = 0;
	}
	remoteSystem->withheldConnectionPacketLength = 0;
	// Held for a connection that is ending (or restarting its handshake) without having been reported.
	FreeHeldApplicationMessages(remoteSystem);
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
		std::free(remoteSystem->remoteSessionConfigData);
		remoteSystem->remoteSessionConfigData = 0;
	}
	remoteSystem->remoteSessionConfigLength = 0;

	if (data == 0 || length == 0)
		return true;

	// One extra byte, always zeroed and never counted in the reported length. The payload is
	// attacker-controlled and arbitrary bytes, so an application that reaches for strlen/printf on it
	// would otherwise run off the end of the buffer. Length remains the authoritative bound.
	remoteSystem->remoteSessionConfigData = (unsigned char *) std::malloc(length + 1);
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
void RakPeer::ProduceWithheldConnectionPacket(RemoteSystemStruct *remoteSystem)
{
	if (remoteSystem == 0)
		return;

	const bool incoming = !remoteSystem->sessionConfigIsConnectingSide;
	Packet *packet;
	if (remoteSystem->withheldConnectionPacketData != 0 && remoteSystem->withheldConnectionPacketLength > 0)
	{
		packet = AllocPacket(remoteSystem->withheldConnectionPacketLength);
		memcpy(packet->data, remoteSystem->withheldConnectionPacketData, remoteSystem->withheldConnectionPacketLength);
		packet->bitSize = remoteSystem->withheldConnectionPacketLength * 8;
		std::free(remoteSystem->withheldConnectionPacketData);
		remoteSystem->withheldConnectionPacketData = 0;
		remoteSystem->withheldConnectionPacketLength = 0;
	}
	else
	{
		// The original bytes could not be stashed (allocation failure); the message id alone still tells
		// the application a connection exists, which is what every caller keys off.
		packet = AllocPacket(sizeof(MessageID));
		packet->data[0] = incoming ? MessageID{ID_NEW_INCOMING_CONNECTION} : MessageID{ID_CONNECTION_REQUEST_ACCEPTED};
		packet->bitSize = sizeof(MessageID) * 8;
	}

	packet->systemAddress = remoteSystem->systemAddress;
	packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
	packet->guid = remoteSystem->guid;
	packet->guid.systemIndex = packet->systemAddress.systemIndex;
	remoteSystem->connectionReportedToApplication = true;
	if (incoming)
	{
		packet->connectionGeneration = remoteSystem->generation;
		playerRoster->Connected(ToPeerGuid(remoteSystem->guid), remoteSystem->generation,
		                        remoteSystem->lowestPing == 65535 ? -1 : static_cast<int>(remoteSystem->lowestPing));
		AddPacketToProducer(packet);
	}
	else
	{
		// A cancel the network thread has not applied yet ends the attempt here instead. Checked where the
		// acceptance is queued: CancelConnectionAttempt() takes the same lock to look for it, so a cancel either
		// finds it in the queue or is seen here.
		packetReturnMutex.Lock();
		const bool cancelled = IsAttemptCancelled_NoLock(*remoteSystem);
		if (!cancelled)
		{
			if (remoteSystem->raceFirstAddress != UNASSIGNED_SYSTEM_ADDRESS)
				reportedRaceAttempts.emplace_back(remoteSystem->guid, remoteSystem->raceFirstAddress);
			packetReturnQueue.push_back(packet);
			packetReturnCondition.notify_all();
		}
		packetReturnMutex.Unlock();
		if (cancelled)
		{
			DeallocatePacket(packet);
			remoteSystem->connectionReportedToApplication = false;
			AbandonConnectionAttempt(remoteSystem);
			return;
		}
	}

	// Then what the remote sent while we were still finishing the handshake, in arrival order: right
	// behind the connection packet, never ahead of it. Delivered the way any later message is, so the
	// network-thread plugins see each one now, when it reaches the application, not when it arrived.
	while (!remoteSystem->heldApplicationMessages.empty())
	{
		RemoteSystemStruct::HeldApplicationMessage held = std::move(remoteSystem->heldApplicationMessages.front());
		remoteSystem->heldApplicationMessages.pop_front();
		DeliverApplicationMessage(remoteSystem, held.data.release(), held.bitSize);
	}
	// clear() hands the deque's blocks back; a large hold must not keep them for the life of the connection.
	remoteSystem->heldApplicationMessages.clear();
	remoteSystem->heldApplicationBytes = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::HoldApplicationMessage(RemoteSystemStruct *remoteSystem, unsigned char *data, BitSize_t bitSize)
{
	// Owns `data` from here on: freed when refused, by FreeHeldApplicationMessages(), or released to a Packet.
	RemoteSystemStruct::HeldApplicationMessage held;
	held.data.reset(data);
	held.bitSize = bitSize;
	const unsigned int byteSize = (unsigned int) BITS_TO_BYTES(bitSize);
	if (remoteSystem->heldApplicationMessages.size() >= (size_t) MAXIMUM_HELD_SESSION_MESSAGES ||
	    byteSize > (unsigned int) MAXIMUM_HELD_SESSION_BYTES - remoteSystem->heldApplicationBytes)
		return false;
	remoteSystem->heldApplicationMessages.push_back(std::move(held));
	remoteSystem->heldApplicationBytes += byteSize;
	return true;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::FreeHeldApplicationMessages(RemoteSystemStruct *remoteSystem)
{
	remoteSystem->heldApplicationMessages.clear();
	remoteSystem->heldApplicationBytes = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::FailSessionHandshakeAsConnectingSide(RemoteSystemStruct *remoteSystem)
{
	FreeHeldApplicationMessages(remoteSystem);

	// Our Connect() is answered here, once: the slot leaves EXCHANGING_SESSION_DATA for a silent close,
	// which the failure path does not report again.
	Packet *packet = AllocPacket(sizeof(MessageID));
	packet->data[0] = (unsigned char) ID_CONNECTION_ATTEMPT_FAILED;
	packet->bitSize = sizeof(MessageID) * 8;
	packet->systemAddress = remoteSystem->systemAddress;
	packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
	packet->guid = remoteSystem->guid;
	packet->guid.systemIndex = packet->systemAddress.systemIndex;
	AddPacketToProducer(packet);

	// The remote has already reported this connection to its application, so tell it the connection is
	// gone rather than leaving it to time out. Sent before the silent close so it goes out with the next
	// update; the slot closes once nothing is left to send.
	const MessageID notification = (MessageID) ID_DISCONNECTION_NOTIFICATION;
	SendImmediate((char *) &notification, sizeof(notification) * 8, MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, remoteSystem->systemAddress, false, false, MafiaNet::GetTimeUS(), 0);
	SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY);
	remoteSystem->sessionConfigStartTime = 0;
	quitAndDataEvents.SetEvent();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::AbandonConnectionAttempt(RemoteSystemStruct *remoteSystem)
{
	FreeHeldApplicationMessages(remoteSystem);

	// The application cancelled, so nothing is reported here or when the slot closes. The remote may already be
	// holding our session request, or have accepted it: tell it the connection is gone, so it reports the request
	// abandoned (or the connection closed) now instead of after a timeout.
	const MessageID notification = (MessageID) ID_DISCONNECTION_NOTIFICATION;
	SendImmediate((char *) &notification, sizeof(notification) * 8, MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, remoteSystem->systemAddress, false, false, MafiaNet::GetTimeUS(), 0);
	SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY);
	remoteSystem->sessionConfigStartTime = 0;
	quitAndDataEvents.SetEvent();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::IsAttemptCancelled_NoLock(const RemoteSystemStruct &remoteSystem) const
{
	for (const SystemAddress &target : cancelledAttempts)
	{
		if (remoteSystem.systemAddress == target || remoteSystem.raceFirstAddress == target)
			return true;
	}
	return false;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ApplyCancelledAttempts(void)
{
	std::vector<SystemAddress> targets;
	std::vector<RakNetGUID> withdrawn;
	packetReturnMutex.Lock();
	if (cancelledAttempts.empty())
	{
		packetReturnMutex.Unlock();
		return;
	}
	targets.swap(cancelledAttempts);
	withdrawn = withdrawnConnections;
	packetReturnMutex.Unlock();

	for (const SystemAddress &target : targets)
	{
		for (unsigned int i = 0; i < activeSystemListSize; i++)
		{
			RemoteSystemStruct *remoteSystem = activeSystemList[i];
			if (!remoteSystem->isActive || !remoteSystem->weInitiatedTheConnection || (remoteSystem->systemAddress != target && remoteSystem->raceFirstAddress != target))
				continue;
			const RemoteSystemStruct::ConnectMode mode = remoteSystem->connectMode;
			// Still an attempt: the application has not been told of the connection, or the cancel took the
			// acceptance back before it read it. A connection the application has seen stays open.
			const bool unreported = !remoteSystem->connectionReportedToApplication && (mode == RemoteSystemStruct::REQUESTED_CONNECTION || mode == RemoteSystemStruct::EXCHANGING_SESSION_DATA);
			const bool withdrawnAcceptance = mode == RemoteSystemStruct::CONNECTED && std::find(withdrawn.begin(), withdrawn.end(), remoteSystem->guid) != withdrawn.end();
			if (unreported || withdrawnAcceptance)
				AbandonConnectionAttempt(remoteSystem);
		}
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::DeliverApplicationMessage(RemoteSystemStruct *remoteSystem, unsigned char *data, BitSize_t bitSize)
{
	Packet *packet = AllocPacket((unsigned int) BITS_TO_BYTES(bitSize), data);
	packet->bitSize = bitSize;
	packet->systemAddress = remoteSystem->systemAddress;
	packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
	packet->guid = remoteSystem->guid;
	packet->guid.systemIndex = packet->systemAddress.systemIndex;
	if (data[0] >= ID_USER_PACKET_ENUM || data[0] == ID_TIMESTAMP)
		packet->connectionGeneration = remoteSystem->generation;
	if (!DispatchNetworkThreadReceive(packet))
		AddPacketToProducer(packet);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ProduceSessionAbandoned(RemoteSystemStruct *remoteSystem)
{
	if (remoteSystem == 0 || remoteSystem->weInitiatedTheConnection || remoteSystem->sessionConfigAwaitingLocalDecision == false)
		return;

	playerRoster->Disconnected(ToPeerGuid(remoteSystem->guid), remoteSystem->generation);
	// Once only: the flag is what made the request the application's to answer.
	remoteSystem->sessionConfigAwaitingLocalDecision = false;

	Packet *packet = AllocPacket(sizeof(MessageID));
	packet->data[0] = (unsigned char) ID_SESSION_CONFIG_ABANDONED;
	packet->connectionGeneration = remoteSystem->generation;
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

	SetConnectMode(*remoteSystem, RemoteSystemStruct::CONNECTED);
	remoteSystem->sessionConfigStartTime = 0;
	ProduceWithheldConnectionPacket(remoteSystem);

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
	SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ON_NO_ACK);
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
RakPeer::RemoteSystemStruct *RakPeer::AssignSystemAddressToRemoteSystemList(const SystemAddress systemAddress, RemoteSystemStruct::ConnectMode connectionMode, RakNetSocket2 *incomingRakNetSocket, bool *thisIPConnectedRecently, SystemAddress bindingAddress, int incomingMTU, RakNetGUID guid, const SessionKeys *sessionKeys, const CryptInfo &localCryptInfo, const CryptInfo &remoteCryptInfo, const IdentityBlock &localIdentity)
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
			// printf("--- Address %s has become active\n", MafiaNet::to_string(systemAddress).c_str());

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
			ClearHandshakeIdentity(remoteSystem);
			remoteSystem->MTUSize = mtuSize;
			remoteSystem->guid = guid;
			remoteSystem->departingPlayerId = {};
			if (++lastConnectionGeneration == 0)
				++lastConnectionGeneration;
			remoteSystem->generation = lastConnectionGeneration;
			remoteSystem->raceFirstAddress = UNASSIGNED_SYSTEM_ADDRESS;
			remoteSystem->isActive = true; // This one line causes future incoming packets to go through the reliability layer
			remoteSystem->handshakeLocalCryptInfo = localCryptInfo;
			remoteSystem->handshakeRemoteCryptInfo = remoteCryptInfo;
			// Cleared above; the REPLY_2 handler records the remote's identity key after this, when it proved one.
			remoteSystem->handshakeLocalIdentity = localIdentity;
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
	// One entry per reference. (A second allocation on a bucket collision used to overwrite the first, which
	// was then never freed: one entry lost per colliding connection for the life of the peer.)
	RemoteSystemIndex *rsi = new RemoteSystemIndex;
	++liveRemoteSystemIndexEntries;
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
			delete cur;
			--liveRemoteSystemIndexEntries;
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
	if (remoteSystemLookup != 0)
	{
		for (unsigned int bucket = 0; bucket < remoteSystemLookupSize; bucket++)
		{
			RemoteSystemIndex *cur = remoteSystemLookup[bucket];
			while (cur != 0)
			{
				RemoteSystemIndex *next = cur->next;
				delete cur;
				cur = next;
			}
		}
	}
	liveRemoteSystemIndexEntries = 0;
	delete[] remoteSystemLookup;
	remoteSystemLookup = 0;
	remoteSystemLookupSize = 0;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SetConnectMode(RemoteSystemStruct &system, RemoteSystemStruct::ConnectMode mode)
{
	if (system.connectMode == RemoteSystemStruct::CONNECTED && mode != RemoteSystemStruct::CONNECTED)
		system.departingPlayerId = RetirePlayer(system);
	if (system.connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && mode != RemoteSystemStruct::CONNECTED && mode != RemoteSystemStruct::EXCHANGING_SESSION_DATA)
		playerRoster->Disconnected(ToPeerGuid(system.guid), system.generation);
	const auto previous = system.connectMode.exchange(mode);
	if ((previous == RemoteSystemStruct::CONNECTED) != (mode == RemoteSystemStruct::CONNECTED))
		PublishConnectedSystems();
}

PlayerId RakPeer::RetirePlayer(const RemoteSystemStruct &system)
{
	// Graceful closes leave CONNECTED before the notification. Timeouts and silent closes
	// can go straight from CONNECTED to slot removal in the same update.
	if (system.connectMode == RemoteSystemStruct::CONNECTED && !system.sessionConfigIsConnectingSide)
		return playerRoster->Disconnected(ToPeerGuid(system.guid), system.generation);
	return system.departingPlayerId;
}

void RakPeer::PublishConnectedSystems()
{
	// Only the update thread walks the live slots. Readers keep their version alive while copying.
	auto snapshot = std::make_shared<ConnectedSystems>();
	snapshot->reserve(activeSystemListSize);
	for (unsigned int i = 0; i < activeSystemListSize; ++i)
	{
		const auto &system = *activeSystemList[i];
		if (system.isActive && system.connectMode == RemoteSystemStruct::CONNECTED)
		{
			// Other getters lazily set guid.systemIndex. Take the hint from the slot itself.
			RakNetGUID guid(system.guid.g);
			guid.systemIndex = system.remoteSystemIndex;
			snapshot->push_back({system.systemAddress, guid, system.MTUSize.load(std::memory_order_relaxed)});
		}
	}
	std::atomic_store(&connectedSystems, std::shared_ptr<const ConnectedSystems>(std::move(snapshot)));
}

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
			if (rss->connectMode == RemoteSystemStruct::CONNECTED)
				PublishConnectedSystems();
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
	return AllowIncomingConnection(UNASSIGNED_SYSTEM_ADDRESS);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::AllowIncomingConnection(const SystemAddress &address) const
{
	if (remoteSystemList == nullptr || endThreads || activeSystemListSize >= maximumNumberOfPeers)
		return false;

	const auto limits = playerRoster->Capacity();
	unsigned int pending, pendingFromAddress;
	CountRemoteInitiatedPeers(address, pending, pendingFromAddress);
	return limits.allows_pending(pending, pendingFromAddress);
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A pending peer is one that asked to connect and is not CONNECTED yet: still proving the transport
// (UNVERIFIED_SENDER, HANDLING_CONNECTION_REQUEST) or in the session handshake. A peer being torn down is
// in none of these, so a refusal frees its share of the pool at once rather than after the ack.
void RakPeer::CountRemoteInitiatedPeers(const SystemAddress &address, unsigned int &pending, unsigned int &pendingFromAddress) const
{
	pending = pendingFromAddress = 0;
	for (unsigned int i = 0; i < activeSystemListSize; i++)
	{
		const RemoteSystemStruct *system = activeSystemList[i];
		if (!system->isActive || system->weInitiatedTheConnection)
			continue;

		switch (system->connectMode)
		{
		case RemoteSystemStruct::EXCHANGING_SESSION_DATA:
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
void RakPeer::DeallocRNS2RecvStruct(RNS2RecvStruct *s)
{
	bufferedPacketsFreePoolMutex.Lock();
	bufferedPacketsFreePool.push_back(s);
	bufferedPacketsFreePoolMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RNS2RecvStruct *RakPeer::AllocRNS2RecvStruct()
{
	bufferedPacketsFreePoolMutex.Lock();
	if (bufferedPacketsFreePool.empty() == false)
	{
		RNS2RecvStruct *s = bufferedPacketsFreePool.back();
		bufferedPacketsFreePool.pop_back();
		bufferedPacketsFreePoolMutex.Unlock();
		return s;
	}
	else
	{
		bufferedPacketsFreePoolMutex.Unlock();
		return new RNS2RecvStruct;
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearBufferedPackets(void)
{
	bufferedPacketsFreePoolMutex.Lock();
	for (RNS2RecvStruct *s : bufferedPacketsFreePool)
		delete s;
	bufferedPacketsFreePool.clear();
	bufferedPacketsFreePoolMutex.Unlock();

	bufferedPacketsQueueMutex.Lock();
	for (RNS2RecvStruct *s : bufferedPacketsQueue)
		delete s;
	bufferedPacketsQueue.clear();
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
	bufferedPacketsQueue.push_back(p);
	bufferedPacketsQueueMutex.Unlock();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RNS2RecvStruct *RakPeer::PopBufferedPacket(void)
{
	bufferedPacketsQueueMutex.Lock();
	if (bufferedPacketsQueue.empty() == false)
	{
		RNS2RecvStruct *s = bufferedPacketsQueue.front();
		bufferedPacketsQueue.pop_front();
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
					playerRoster->Disconnected(ToPeerGuid(remoteSystemList[index].guid), remoteSystemList[index].generation);
					{
						// The slot is gone, so a cancel can no longer take its acceptance back nor drop its packets.
						const RakNetGUID closing = remoteSystemList[index].guid;
						packetReturnMutex.Lock();
						withdrawnConnections.erase(std::remove(withdrawnConnections.begin(), withdrawnConnections.end(), closing), withdrawnConnections.end());
						reportedRaceAttempts.erase(std::remove_if(reportedRaceAttempts.begin(), reportedRaceAttempts.end(), [&](const std::pair<RakNetGUID, SystemAddress> &race)
						                                          { return race.first == closing; }),
						                           reportedRaceAttempts.end());
						packetReturnMutex.Unlock();
					}
					RemoveFromActiveSystemList(target);

					// Found the index to stop
					// printf("--- Address %s has become inactive\n", MafiaNet::to_string(remoteSystemList[index].systemAddress).c_str());
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
					ClearHandshakeIdentity(&remoteSystemList[index]);

					// Not using this socket
					remoteSystemList[index].rakNetSocket = 0;
				}
			}
		}
		else
		{
			auto bcs = std::make_unique<BufferedCommandStruct>();
			bcs->command = BufferedCommandStruct::BCS_CLOSE_CONNECTION;
			bcs->systemIdentifier = target;
			// #med - review whether this is safe here - can't the socket be closed/released before its used?
			bcs->socket = &socket;
			bcs->orderingChannel = orderingChannel;
			bcs->priority = disconnectionNotificationPriority;
			bufferedCommands.Push(std::move(bcs));
		}
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::SendBuffered(const char *data, BitSize_t numberOfBitsToSend, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, RemoteSystemStruct::ConnectMode connectionMode, uint32_t receipt, bool allowFragmentation, uint64_t generation)
{
	auto bcs = std::make_unique<BufferedCommandStruct>();
	bcs->generation = generation;
	bcs->data = (char *) std::malloc((size_t) BITS_TO_BYTES(numberOfBitsToSend)); // Making a copy doesn't lose efficiency because I tell the reliability layer to use this allocation for its own copy
	if (bcs->data == 0)
		return false;

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
	bcs->allowFragmentation = allowFragmentation;
	bcs->connectionMode = connectionMode;
	bcs->receipt = receipt;
	bcs->command = BufferedCommandStruct::BCS_SEND;
	bufferedCommands.Push(std::move(bcs));

	if (priority == MafiaNet::Priority::Immediate)
	{
		// Forces pending sends to go out now, rather than waiting to the next update interval
		quitAndDataEvents.SetEvent();
	}
	return true;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::SendBufferedList(const char **data, const int *lengths, const int numParameters, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, RemoteSystemStruct::ConnectMode connectionMode, uint32_t receipt)
{
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
	dataAggregate = (char *) std::malloc((size_t) totalLength); // Making a copy doesn't lose efficiency because I tell the reliability layer to use this allocation for its own copy
	if (dataAggregate == 0)
	{
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
		std::free(dataAggregate);
		return;
	}

	RakAssert(!((unsigned int) reliability >= MafiaNet::NUMBER_OF_RELIABILITIES || (int) reliability < 0));
	RakAssert(!((int) priority > (int) MafiaNet::NUMBER_OF_PRIORITIES || (int) priority < 0));
	RakAssert(!(orderingChannel >= NUMBER_OF_ORDERED_STREAMS));

	auto bcs = std::make_unique<BufferedCommandStruct>();
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
	bufferedCommands.Push(std::move(bcs));

	if (priority == MafiaNet::Priority::Immediate)
	{
		// Forces pending sends to go out now, rather than waiting to the next update interval
		quitAndDataEvents.SetEvent();
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool RakPeer::SendImmediate(char *data, BitSize_t numberOfBitsToSend, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, bool useCallerDataAllocation, MafiaNet::TimeUS currentTime, uint32_t receipt, bool allowFragmentation)
{
	unsigned unicastTarget = 0;          // unicast sends must not allocate
	std::vector<unsigned> broadcastList; // only filled for broadcasts
	unsigned *sendList = &unicastTarget;
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


		if (remoteSystemList[remoteSystemIndex].isActive &&
		    (allowFragmentation || remoteSystemList[remoteSystemIndex].connectMode == RemoteSystemStruct::CONNECTED) &&
		    remoteSystemList[remoteSystemIndex].connectMode != RemoteSystemStruct::DISCONNECT_ASAP &&
		    remoteSystemList[remoteSystemIndex].connectMode != RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY &&
		    remoteSystemList[remoteSystemIndex].connectMode != RemoteSystemStruct::DISCONNECT_ON_NO_ACK)
		{
			unicastTarget = remoteSystemIndex;
			sendListSize = 1;
		}
	}
	else
	{
		broadcastList.resize(maximumNumberOfPeers);
		sendList = broadcastList.data();

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
		return false;
	}

	for (sendListIndex = 0; sendListIndex < sendListSize; sendListIndex++)
	{
		// Send may split the packet and thus deallocate data.  Don't assume data is valid if we use the callerAllocationData
		bool useData = useCallerDataAllocation && callerDataAllocationUsed == false && sendListIndex + 1 == sendListSize;
		const bool accepted = remoteSystemList[sendList[sendListIndex]].reliabilityLayer.Send(data, numberOfBitsToSend, priority, reliability, orderingChannel, useData == false, remoteSystemList[sendList[sendListIndex]].MTUSize, currentTime, receipt, allowFragmentation);
		if (accepted && useData)
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
	if (remoteSystem->connectionReportedToApplication && !remoteSystem->sessionConfigIsConnectingSide)
		playerRoster->UpdatePing(ToPeerGuid(remoteSystem->guid), remoteSystem->generation, static_cast<int>(remoteSystem->pingAndClockDifferential[remoteSystem->pingAndClockDifferentialWriteIndex].pingTime));
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
	// Each dropped command frees its own data (~BufferedCommandStruct).
	bufferedCommands.Clear();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearSocketQueryOutput(void)
{
	socketQueryOutput.Clear();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void RakPeer::ClearRequestedConnectionList(void)
{
	std::deque<RequestedConnectionStruct *> freeQueue;
	requestedConnectionQueueMutex.Lock();
	freeQueue.swap(requestedConnectionQueue);
	requestedConnectionQueueMutex.Unlock();
	for (RequestedConnectionStruct *rcs : freeQueue)
		DeleteRequestedConnection(rcs);
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
	for (unsigned int i = 0; i < pluginListNetworkThread.size(); i++)
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
	for (unsigned int i = 0; i < pluginListNetworkThread.size() && result == RR_CONTINUE_PROCESSING; i++)
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
	// A connection whose acceptance CancelConnectionAttempt() took back is closing: the application never saw
	// it, so nothing more of it reaches the application either.
	if (!withdrawnConnections.empty() && std::find(withdrawnConnections.begin(), withdrawnConnections.end(), p->guid) != withdrawnConnections.end())
	{
		packetReturnMutex.Unlock();
		DeallocatePacket(p);
		return;
	}
	packetReturnQueue.push_back(p);
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
		for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
			rakPeer->pluginListNTS[i]->OnDirectSocketReceive(data, length * 8, systemAddress);

		MafiaNet::BitStream bs;
		bs.Write((MessageID) ID_CONNECTION_BANNED);
		bs.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
		bs.Write(rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));


		RNS2_SendParameters bsp;
		bsp.data = (char *) bs.GetData();
		bsp.length = bs.GetNumberOfBytesUsed();
		bsp.systemAddress = systemAddress;
		for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
			rakPeer->pluginListNTS[i]->OnDirectSocketSend((char *) bs.GetData(), bs.GetNumberOfBitsUsed(), systemAddress);
		rakNetSocket->Send(&bsp);

		/*
		unsigned i;
		for (i=0; i < rakPeer->pluginListNTS.size(); i++)
			rakPeer->pluginListNTS[i]->OnDirectSocketSend((char*) bs.GetData(), bs.GetNumberOfBitsUsed(), systemAddress);
		SocketLayer::SendTo( rakNetSocket, (char*) bs.GetData(), bs.GetNumberOfBytesUsed(), systemAddress);
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
		for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
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

				for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) outBitStream.GetData(), outBitStream.GetNumberOfBytesUsed(), systemAddress);

				RNS2_SendParameters bsp;
				bsp.data = (char *) outBitStream.GetData();
				bsp.length = outBitStream.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp);

				// SocketLayer::SendTo( rakNetSocket, (const char*)outBitStream.GetData(), (unsigned int) outBitStream.GetNumberOfBytesUsed(), systemAddress);

				packet = rakPeer->AllocPacket(sizeof(MessageID));
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
			packet = rakPeer->AllocPacket((unsigned int) (length - sizeof(OFFLINE_MESSAGE_DATA_ID) - RakNetGUID::size() - sizeof(MafiaNet::Time) + sizeof(MafiaNet::TimeMS)));
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
			packet = rakPeer->AllocPacket(dataLength + 1);
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
			for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
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
			for (i = 0; i < rakPeer->requestedConnectionQueue.size(); i++)
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

					for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
						rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);

					RNS2_SendParameters bsp;
					bsp.data = (char *) bsOut.GetData();
					bsp.length = bsOut.GetNumberOfBytesUsed();
					bsp.systemAddress = systemAddress;
					rakNetSocket->Send(&bsp);

					return true;
				}
			}
			rakPeer->requestedConnectionQueueMutex.Unlock();
		}
		else if ((unsigned char) (data)[0] == (MessageID) ID_OPEN_CONNECTION_REPLY_2)
		{
			for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketReceive(data, length * 8, systemAddress);

			// ID_OPEN_CONNECTION_REPLY_2 (S->C), byte-aligned:
			//   MessageID (1) | OFFLINE_MESSAGE_DATA_ID (16) | echo of our CryptInfo nonce (uint64, 8) | server RakNetGUID (8)
			//   | our address as the server sees it (SystemAddress) | MTU (uint16, 2) | server CryptInfo (41)
			//   | identity block (IdentityBlock): flag (1: 0 none, 1 present) [| identity public key (32) | signature (64)]
			// The signature is Ed25519 (crypto_sign_ed25519_detached) by the server's identity key over the 107 bytes
			//   "MafiaNet server identity" (24 ASCII) | RAKNET_PROTOCOL_VERSION (1) | our CryptInfo (41) | server CryptInfo (41)
			// (BuildIdentityTranscript), and the HKDF info of DeriveSessionKeys includes the identity block.
			MafiaNet::BitStream bs((unsigned char *) data, length, false);
			bs.IgnoreBytes(sizeof(MessageID));
			bs.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID));
			uint64_t nonceEcho;
			RakNetGUID guid;
			SystemAddress bindingAddress;
			uint16_t mtu;
			CryptInfo serverInfo;
			IdentityBlock serverIdentity;
			if (!bs.Read(nonceEcho) || !bs.Read(guid) || !bs.Read(bindingAddress) || !bs.Read(mtu) || !serverInfo.Deserialize(&bs) || !serverIdentity.Deserialize(&bs))
				return true; // malformed: drop

			RakPeer::RequestedConnectionStruct *rcs;
			bool unlock = true;
			rakPeer->requestedConnectionQueueMutex.Lock();
			for (i = 0; i < rakPeer->requestedConnectionQueue.size(); i++)
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

						packet = rakPeer->AllocPacket(sizeof(char));
						packet->data[0] = rakPeer->encryptionMode == EncryptionMode::Plaintext ? (MessageID) ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION : (MessageID) ID_OUR_SYSTEM_REQUIRES_SECURITY;
						packet->bitSize = (sizeof(char) * 8);
						packet->systemAddress = reportedAddress;
						packet->guid = guid;
						rakPeer->AddPacketToProducer(packet);
						return true;
					}

					// Our crypt info exactly as this request sent it in REQUEST_2 (see the REPLY_1 handler): the server's
					// identity signature covers it, and the key schedule below uses it.
					CryptInfo clientInfo;
					bool haveClientInfo = true;
					if (rakPeer->encryptionMode == EncryptionMode::Required)
					{
						if (rcs->ephemeral != nullptr && !rcs->ephemeral->IsWiped())
							clientInfo = rcs->ephemeral->MakeCryptInfo();
						else
							haveClientInfo = false; // no key pair left: the derivation below fails the attempt
					}
					else
					{
						clientInfo.mode = EncryptionMode::Plaintext;
						clientInfo.nonce = rcs->handshakeNonce;
					}

					// Server identity, checked before any key is derived and before anything changes.
					// - Pinned (Connect() was given a PublicKey*): the server must present an identity block, with the
					//   pinned public key (constant-time compare), and a signature that verifies. Anything else ends the
					//   attempt with ID_PUBLIC_KEY_MISMATCH, reported once under the race's first address.
					// - Not pinned: a present block is still verified. A bad signature drops this reply silently (an
					//   off-path forger must not be able to make us report errors) and the attempt carries on; a good
					//   one records the key for GetRemoteIdentityKey().
					bool identityVerified = false;
					if (haveClientInfo && serverIdentity.present)
						identityVerified = VerifyServerIdentity(serverIdentity, clientInfo, serverInfo);
					if (haveClientInfo && rcs->pinnedIdentity)
					{
						const bool keyMatches = serverIdentity.present && sodium_memcmp(serverIdentity.publicKey, rcs->pinnedServerIdentity, sizeof(rcs->pinnedServerIdentity)) == 0;
						if (!keyMatches || !identityVerified)
						{
							rakPeer->CancelRaceSiblings_NoLock(rcs);
							rakPeer->RemoveRequestedConnection_NoLock(rcs);
							rakPeer->requestedConnectionQueueMutex.Unlock();
							rakPeer->DeleteRequestedConnection(rcs); // wipes the ephemeral key pair

							packet = rakPeer->AllocPacket(sizeof(char));
							packet->data[0] = ID_PUBLIC_KEY_MISMATCH;
							packet->bitSize = (sizeof(char) * 8);
							packet->systemAddress = reportedAddress;
							packet->guid = guid;
							rakPeer->AddPacketToProducer(packet);
							return true;
						}
					}
					else if (haveClientInfo && serverIdentity.present && !identityVerified)
						break; // not pinned, bad signature: ignore this reply

					// You might get this when already connected because of cross-connections
					remoteSystem = rakPeer->GetRemoteSystemFromSystemAddress(systemAddress, true, true);
					if (remoteSystem != 0)
					{
						// The winning incoming handshake owns this slot. Ignore replies to the absorbed attempt.
						rakPeer->requestedConnectionQueueMutex.Unlock();
						return true;
					}

					// Derive under the lock: rcs->ephemeral belongs to the queue. DeriveSessionKeys wipes it.
					SessionKeys keys;
					if (rakPeer->encryptionMode == EncryptionMode::Required)
					{
						const RakNetGUID myGuid = rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);
						bool derived = false;
						if (haveClientInfo)
							derived = DeriveSessionKeys(*rcs->ephemeral, /*localIsInitiator=*/true, clientInfo, serverInfo, myGuid, guid, serverIdentity, &keys);
						if (!derived)
						{
							// A low-order server key (or no key pair left): this attempt cannot be encrypted.
							rakPeer->RemoveRequestedConnection_NoLock(rcs);
							rakPeer->requestedConnectionQueueMutex.Unlock();
							rakPeer->DeleteRequestedConnection(rcs);

							packet = rakPeer->AllocPacket(sizeof(char));
							packet->data[0] = ID_CONNECTION_ATTEMPT_FAILED;
							packet->bitSize = (sizeof(char) * 8);
							packet->systemAddress = reportedAddress;
							packet->guid = guid;
							rakPeer->AddPacketToProducer(packet);
							return true;
						}
					}
					const SessionKeys *sessionKeys = rakPeer->encryptionMode == EncryptionMode::Required ? &keys : nullptr;

					rakPeer->requestedConnectionQueueMutex.Unlock();
					unlock = false;

					bool thisIPConnectedRecently = false;
					auto *socket = rcs->socket ? rcs->socket : rakNetSocket;
					remoteSystem = rakPeer->AssignSystemAddressToRemoteSystemList(systemAddress, RakPeer::RemoteSystemStruct::UNVERIFIED_SENDER, socket, &thisIPConnectedRecently, bindingAddress, mtu, guid, sessionKeys, clientInfo, serverInfo, IdentityBlock());
					keys.Wipe();
					if (remoteSystem != 0 && identityVerified)
					{
						memcpy(remoteSystem->remoteIdentityKey, serverIdentity.publicKey, sizeof(remoteSystem->remoteIdentityKey));
						remoteSystem->hasRemoteIdentityKey = true;
					}

					// 4/13/09 Attackers can flood ID_OPEN_CONNECTION_REQUEST and use up all available connection slots
					// Ignore connection attempts if this IP address connected within the last 100 milliseconds
					if (thisIPConnectedRecently == false)
					{
						// Don't check GetRemoteSystemFromGUID, server will verify
						if (remoteSystem)
						{
							remoteSystem->weInitiatedTheConnection = true;
							rakPeer->SetConnectMode(*remoteSystem, RakPeer::RemoteSystemStruct::REQUESTED_CONNECTION);
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
							packet = rakPeer->AllocPacket(sizeof(char));
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
			for (i = 0; i < rakPeer->requestedConnectionQueue.size(); i++)
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
						for (i = 0; i < rakPeer->requestedConnectionQueue.size(); i++)
						{
							if (rakPeer->requestedConnectionQueue[i] == rcs)
								break;
						}
					}
					rakPeer->requestedConnectionQueue.erase(rakPeer->requestedConnectionQueue.begin() + i);
					rakPeer->DeleteRequestedConnection(rcs);
					break;
				}
			}

			rakPeer->requestedConnectionQueueMutex.Unlock();

			if (connectionAttemptCancelled)
			{
				// Tell user of connection attempt failed
				packet = rakPeer->AllocPacket(sizeof(char));
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

				for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((char *) bs.GetData(), bs.GetNumberOfBitsUsed(), systemAddress);

				// SocketLayer::SendTo( rakNetSocket, (char*)bs.GetData(), bs.GetNumberOfBytesUsed(), systemAddress);

				RNS2_SendParameters bsp;
				bsp.data = (char *) bs.GetData();
				bsp.length = bs.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;

				rakNetSocket->Send(&bsp);
				return true;
			}

			for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
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

			for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
			// SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress);

			RNS2_SendParameters bsp;
			bsp.data = (char *) bsOut.GetData();
			bsp.length = bsOut.GetNumberOfBytesUsed();
			bsp.systemAddress = systemAddress;
			rakNetSocket->Send(&bsp);
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
				for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp);
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
				// The identity block the first REPLY_2 carried, and the one the session keys were derived from.
				rssFromSA->handshakeLocalIdentity.Serialize(&bsAnswer);

				for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsAnswer.GetData(), bsAnswer.GetNumberOfBitsUsed(), systemAddress);
				// SocketLayer::SendTo( rakNetSocket, (const char*) bsAnswer.GetData(), bsAnswer.GetNumberOfBytesUsed(), systemAddress);

				RNS2_SendParameters bsp;
				bsp.data = (char *) bsAnswer.GetData();
				bsp.length = bsAnswer.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp);

				return true;
			}
			else if (outcome != 0)
			{
				bsOut.Write((MessageID) ID_ALREADY_CONNECTED);
				bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
				bsOut.Write(rakPeer->myGuid);
				for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				// SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress);
				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp);

				return true;
			}

			if (rakPeer->AllowIncomingConnection(systemAddress) == false)
			{
				// Cross-connection: a peer with a lower GUID does not answer our REQUEST_2 while its own attempt to us
				// is pending (see the tie-break below). This refusal ends that attempt, so retry ours right away
				// instead of after a whole retry interval.
				if (guid.g < rakPeer->myGuid.g)
				{
					rakPeer->requestedConnectionQueueMutex.Lock();
					for (i = 0; i < rakPeer->requestedConnectionQueue.size(); i++)
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
				for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				//SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress);
				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp);

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
			// Plaintext uses the same tie-break so exactly one peer receives the session request.
			// The queue lock is held until the connection is allocated: Connect() looks for a connection under the
			// same lock, so an attempt it starts meanwhile is either refused there or seen here.
			// Server identity: a peer whose own attempt to this address pinned the remote's identity never lets that
			// attempt be absorbed into a connection the remote initiated, because the initiator proves nothing about
			// its identity. With the higher GUID it would answer and be absorbed, so it refuses instead, with
			// ID_ALREADY_CONNECTED (RakNet's "you lost the cross-connection"; it ends the remote's attempt), and
			// retries its own attempt at once: the remote, no longer connecting, answers it as responder and must
			// prove the pinned identity.
			rakPeer->requestedConnectionQueueMutex.Lock();
			{
				bool refuseForPin = false;
				for (i = 0; i < rakPeer->requestedConnectionQueue.size(); i++)
				{
					RakPeer::RequestedConnectionStruct *rcs = rakPeer->requestedConnectionQueue[i];
					if (rcs->systemAddress == systemAddress && !rcs->cancelled && rcs->actionToTake == RakPeer::RequestedConnectionStruct::CONNECT)
					{
						if (rakPeer->myGuid.g < guid.g)
						{
							rakPeer->requestedConnectionQueueMutex.Unlock();
							return true;
						}
						if (rcs->pinnedIdentity)
						{
							rcs->nextRequestTime = 0;
							refuseForPin = true;
						}
					}
				}
				if (refuseForPin)
				{
					rakPeer->requestedConnectionQueueMutex.Unlock();
					bsOut.Write((MessageID) ID_ALREADY_CONNECTED);
					bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
					bsOut.Write(rakPeer->myGuid);
					for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
						rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
					RNS2_SendParameters bsp;
					bsp.data = (char *) bsOut.GetData();
					bsp.length = bsOut.GetNumberOfBytesUsed();
					bsp.systemAddress = systemAddress;
					rakNetSocket->Send(&bsp);
					return true;
				}
			}

			// The cheap refusals all come before the key exchange. The last one: SetLimitIPConnectionFrequency's
			// "connected recently from this IP", which AssignSystemAddressToRemoteSystemList also applies. Checked
			// here first, with the same rule and clock, so a flood from one IP costs no X25519.
			bool thisIPConnectedRecently = rakPeer->IPConnectedRecently(systemAddress, MafiaNet::GetTimeMS());

			// Our side of the handshake: a fresh ephemeral key pair, used once and wiped by DeriveSessionKeys
			// (and by its destructor) before this block ends. The client initiated, so it holds the initiator slot.
			// With an identity key, the server signs the transcript (see the REPLY_2 handler for the exact bytes) in
			// both modes, and the identity block enters the key schedule.
			SessionKeys keys;
			CryptInfo serverInfo;
			IdentityBlock identity;
			if (thisIPConnectedRecently)
				rssFromSA = 0;
			else if (rakPeer->encryptionMode == EncryptionMode::Required)
			{
				EphemeralKeyPair serverPair;
				serverInfo = serverPair.MakeCryptInfo();
				if (rakPeer->hasIdentityKey && !SignServerIdentity(rakPeer->identitySecretKey, rakPeer->identityPublicKey, clientInfo, serverInfo, &identity))
				{
					rakPeer->requestedConnectionQueueMutex.Unlock();
					return true; // never answer with an unsigned reply instead
				}
				rakPeer->serverKeyExchangeCount++;
				if (!DeriveSessionKeys(serverPair, /*localIsInitiator=*/false, clientInfo, serverInfo, guid, rakPeer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS), identity, &keys))
				{
					rakPeer->requestedConnectionQueueMutex.Unlock();
					return true; // low-order client key: drop, allocating nothing
				}
			}
			else
			{
				serverInfo.mode = EncryptionMode::Plaintext;
				if (rakPeer->hasIdentityKey && !SignServerIdentity(rakPeer->identitySecretKey, rakPeer->identityPublicKey, clientInfo, serverInfo, &identity))
				{
					rakPeer->requestedConnectionQueueMutex.Unlock();
					return true;
				}
			}

			if (!thisIPConnectedRecently)
				rssFromSA = rakPeer->AssignSystemAddressToRemoteSystemList(systemAddress, RakPeer::RemoteSystemStruct::UNVERIFIED_SENDER, rakNetSocket, &thisIPConnectedRecently, bindingAddress, mtu, guid, rakPeer->encryptionMode == EncryptionMode::Required ? &keys : nullptr, serverInfo, clientInfo, identity);
			rakPeer->requestedConnectionQueueMutex.Unlock();
			keys.Wipe();

			if (thisIPConnectedRecently == true)
			{
				bsOut.Write((MessageID) ID_IP_RECENTLY_CONNECTED);
				bsOut.WriteAlignedBytes((const unsigned char *) OFFLINE_MESSAGE_DATA_ID, sizeof(OFFLINE_MESSAGE_DATA_ID));
				bsOut.Write(rakPeer->myGuid);
				for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
					rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsOut.GetData(), bsOut.GetNumberOfBitsUsed(), systemAddress);
				//SocketLayer::SendTo( rakNetSocket, (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), systemAddress);

				RNS2_SendParameters bsp;
				bsp.data = (char *) bsOut.GetData();
				bsp.length = bsOut.GetNumberOfBytesUsed();
				bsp.systemAddress = systemAddress;
				rakNetSocket->Send(&bsp);

				return true;
			}

			if (rssFromSA == 0)
				return true;

			serverInfo.Serialize(&bsAnswer);
			identity.Serialize(&bsAnswer);

			for (i = 0; i < rakPeer->pluginListNTS.size(); i++)
				rakPeer->pluginListNTS[i]->OnDirectSocketSend((const char *) bsAnswer.GetData(), bsAnswer.GetNumberOfBitsUsed(), systemAddress);
			// SocketLayer::SendTo( rakNetSocket, (const char*) bsAnswer.GetData(), bsAnswer.GetNumberOfBytesUsed(), systemAddress);
			RNS2_SendParameters bsp;
			bsp.data = (char *) bsAnswer.GetData();
			bsp.length = bsAnswer.GetNumberOfBytesUsed();
			bsp.systemAddress = systemAddress;
			rakNetSocket->Send(&bsp);
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
		// printf("--- Packet from unknown system %s\n", MafiaNet::to_string(systemAddress).c_str());
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
	for (i = 0; i < socketList.size(); i++)
	{
		if (socketFactory && !socketList[i]->IsBerkleySocket())
			socketFactory->Destroy(socketList[i]);
		else
			delete socketList[i];
	}
	socketList.clear();
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned int RakPeer::GetRakNetSocketFromUserConnectionSocketIndex(unsigned int userIndex) const
{
	unsigned int i;
	for (i = 0; i < socketList.size(); i++)
	{
		if (socketList[i]->GetUserConnectionSocketIndex() == userIndex)
			return i;
	}
	RakAssert("GetRakNetSocketFromUserConnectionSocketIndex failed" && 0);
	return (unsigned int) -1;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/*
bool RakPeer::RunRecvFromOnce( RakNetSocket *s )
{
	RakPeer::RecvFromStruct *recvFromStruct;

	recvFromStruct=bufferedPackets.Allocate();
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
			bufferedPackets.Deallocate(recvFromStruct);
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
	SystemAddress systemAddress;
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
		for (socketListIndex=0; socketListIndex < socketList.size(); socketListIndex++)
		{
			if ((RakNetSocket*) socketList[socketListIndex]==recvFromStruct->s)
				break;
		}
		if (socketListIndex!=socketList.size())
		*/
		ProcessNetworkPacket(recvFromStruct->systemAddress, recvFromStruct->data, recvFromStruct->bytesRead, this, recvFromStruct->socket, recvFromStruct->timeRead, updateBitStream);
		DeallocRNS2RecvStruct(recvFromStruct);
	}

	// Before the send queue is drained, so whatever the plugins send here leaves this cycle.
	DispatchNetworkThreadUpdate();

	ApplyCancelledAttempts();

	// Only the commands queued when this cycle reached here. A user thread that keeps pushing (e.g. CloseConnection
	// in a polling loop until the peer is gone) must not hold the network thread in this loop: the rest of the
	// cycle, which completes that very disconnect, would never run. Later commands are handled next cycle.
	for (std::size_t pendingCommands = bufferedCommands.Size(); pendingCommands > 0; pendingCommands--)
	{
		auto popped = bufferedCommands.TryPop();
		if (!popped)
			break;
		// Destroying the command at the end of the iteration frees its data, unless a branch handed it on.
		BufferedCommandStruct *bcs = popped->get();
		// A reconnect may have reused the GUID since the game queued this command.
		if (bcs->generation)
		{
			const auto *target = GetRemoteSystem(bcs->systemIdentifier, true, true);
			if (!target || target->generation != bcs->generation ||
			    (bcs->command == BufferedCommandStruct::BCS_SEND && target->connectMode != RemoteSystemStruct::CONNECTED))
				continue;
		}
		if (bcs->command == BufferedCommandStruct::BCS_SEND)
		{
			// GetTime is a very slow call so do it once and as late as possible
			if (timeNS == 0)
			{
				timeNS = MafiaNet::GetTimeUS();
				timeMS = (MafiaNet::TimeMS)(timeNS / (MafiaNet::TimeUS) 1000);
			}

			callerDataAllocationUsed = SendImmediate((char *) bcs->data, bcs->numberOfBitsToSend, bcs->priority, bcs->reliability, bcs->orderingChannel, bcs->systemIdentifier, bcs->broadcast, true, timeNS, bcs->receipt, bcs->allowFragmentation);
			if (callerDataAllocationUsed)
				bcs->data = nullptr; // The reliability layer took the allocation over and frees it

			// Set the new connection state AFTER we call sendImmediate in case we are setting it to a disconnection state, which does not allow further sends
			if (bcs->connectionMode != RemoteSystemStruct::NO_ACTION)
			{
				remoteSystem = GetRemoteSystem(bcs->systemIdentifier, true, true);
				if (remoteSystem)
					SetConnectMode(*remoteSystem, bcs->connectionMode);
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
					SendSessionConfigResponse(sessionSystem, bcs->data, sessionPayloadLength);
				else
					SendSessionConfigRejection(sessionSystem, bcs->data, sessionPayloadLength);
			}
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
		}
		else if (bcs->command == BufferedCommandStruct::BCS_CLOSE_CONNECTION)
		{
			// bcs->socket may be null: the socket can be released between queuing this
			// command and processing it here (e.g. during rapid connect/disconnect churn).
			// RakAssert is a no-op in release (NDEBUG), so guard explicitly and fall back
			// to the primary socket to avoid dereferencing null in CloseConnectionInternal2.
			RakNetSocket2 *closeSocket = bcs->socket;
			if (closeSocket == nullptr && socketList.size() > 0)
				closeSocket = socketList[0];
			remoteSystem = GetRemoteSystem(bcs->systemIdentifier, true, true);
			if (closeSocket != nullptr && remoteSystem != nullptr)
			{
				// Produce the local silent-close event on the thread that owns the slot.
				// A racing timeout or a duplicate close can then retire this join only once.
				if (remoteSystem->connectionReportedToApplication)
				{
					packet = AllocPacket(sizeof(MessageID));
					packet->data[0] = ID_CONNECTION_LOST;
					packet->guid = remoteSystem->guid;
					packet->systemAddress = remoteSystem->systemAddress;
					packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
					packet->guid.systemIndex = remoteSystem->remoteSystemIndex;
					packet->playerId = RetirePlayer(*remoteSystem);
					packet->connectionGeneration = remoteSystem->generation;
					packet->wasGeneratedLocally = true;
					AddPacketToProducer(packet);
				}
				CloseConnectionInternal2(bcs->systemIdentifier, false, true, bcs->orderingChannel, bcs->priority, *closeSocket);
			}
		}
		else if (bcs->command == BufferedCommandStruct::BCS_CHANGE_SYSTEM_ADDRESS)
		{
			// Reroute
			RakPeer::RemoteSystemStruct *rssFromGuid = GetRemoteSystem(bcs->systemIdentifier.rakNetGuid, true, true);
			if (rssFromGuid != 0)
			{
				unsigned int existingSystemIndex = GetRemoteSystemIndex(rssFromGuid->systemAddress);
				ReferenceRemoteSystem(bcs->systemIdentifier.systemAddress, existingSystemIndex);
				if (rssFromGuid->connectMode == RemoteSystemStruct::CONNECTED)
					PublishConnectedSystems();
			}
		}
		else if (bcs->command == BufferedCommandStruct::BCS_GET_SOCKET)
		{
			auto sqo = std::make_unique<SocketQueryOutput>();
			if (bcs->systemIdentifier.IsUndefined())
				sqo->sockets = socketList;
			else
			{
				remoteSystem = GetRemoteSystem(bcs->systemIdentifier, true, true);
				if (remoteSystem)
					sqo->sockets.push_back(remoteSystem->rakNetSocket);
				// else leave the list empty: no such system
			}
			socketQueryOutput.Push(std::move(sqo));
		}
	}

	// Connect() pushes from user threads, so even the emptiness check takes the lock.
	requestedConnectionQueueMutex.Lock();
	const bool haveRequestedConnections = requestedConnectionQueue.empty() == false;
	requestedConnectionQueueMutex.Unlock();
	if (haveRequestedConnections)
	{
		if (timeNS == 0)
		{
			timeNS = MafiaNet::GetTimeUS();
			timeMS = (MafiaNet::TimeMS)(timeNS / (MafiaNet::TimeUS) 1000);
		}

		bool condition1, condition2;
		unsigned requestedConnectionQueueIndex = 0;
		requestedConnectionQueueMutex.Lock();
		while (requestedConnectionQueueIndex < requestedConnectionQueue.size())
		{
			RequestedConnectionStruct *rcs;
			rcs = requestedConnectionQueue[requestedConnectionQueueIndex];
			if (rcs->cancelled)
			{
				// CancelConnectionAttempt flagged it; this thread is the one that frees requests.
				requestedConnectionQueue.erase(requestedConnectionQueue.begin() + requestedConnectionQueueIndex);
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
					for (unsigned int k = 0; k < requestedConnectionQueue.size(); k++)
					{
						if (requestedConnectionQueue[k] == rcs)
						{
							requestedConnectionQueue.erase(requestedConnectionQueue.begin() + k);
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
						packet = AllocPacket(sizeof(char));
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
					for (i = 0; i < pluginListNTS.size(); i++)
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
					const RNS2SendResult sendResult = socketToUse->Send(&bsp);
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
		// Publish changes for application-thread queries and replication batch sizing.
		const int reliabilityMtu = remoteSystem->reliabilityLayer.GetCurrentMtuBytes();
		if (remoteSystem->MTUSize != reliabilityMtu)
		{
			remoteSystem->MTUSize = reliabilityMtu;
			PublishConnectedSystems();
		}

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
				packet = AllocPacket(sizeof(char) + reasonLength);
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

				packet->playerId = RetirePlayer(*remoteSystem);
				packet->connectionGeneration = remoteSystem->generation;
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
				Packet *timeoutPacket = AllocPacket(sizeof(MessageID));
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
		for (;;)
		{
			unsigned char *received = nullptr;
			bitSize = remoteSystem->reliabilityLayer.Receive(&received);
			if (bitSize == 0)
				break;
			// Owns the message for this iteration: it is freed when the iteration ends, whichever branch
			// handled it, unless that branch hands it on to a Packet with message.release().
			ReceivedMessage message(received);
			unsigned char *const data = received;

			// These types are for internal use and should never arrive from a network packet.
			// A remote that sends one anyway gets it dropped: freed, and the rest of the
			// queue still processed (this used to leak it and stall the queue, and to
			// RakAssert in Debug).
			if (data[0] == ID_CONNECTION_ATTEMPT_FAILED)
			{
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
					// Cross-connection: if our own attempt to this peer is still pending, it lost the tie-break (see the
					// REQUEST_2 handler). The winner's ID_CONNECTION_REQUEST just opened under these keys, so this is the
					// connection: drop our attempt quietly, wiping its key pair.
					// Until the connection is reported, that Connect() is answered by the failure path if it dies.
					if (remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST &&
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
				}
				else if ((unsigned char) data[0] == ID_NEW_INCOMING_CONNECTION && byteSize > sizeof(unsigned char) + sizeof(unsigned int) + sizeof(unsigned short) + sizeof(MafiaNet::Time) * 2)
				{
					if (remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST)
					{
						// Transport is up, but the application is not told so until the client's session payload
						// arrives in ID_SESSION_CONFIG_REQUEST and we answer it.
						SetConnectMode(*remoteSystem, RemoteSystemStruct::EXCHANGING_SESSION_DATA);
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

						// Hold this until the session handshake completes; the bytes are copied because `message`
						// frees `data` at the end of this iteration.
						ClearSessionConfig(remoteSystem);
						remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit
						remoteSystem->withheldConnectionPacketData = (unsigned char *) std::malloc(byteSize);
						if (remoteSystem->withheldConnectionPacketData != 0)
						{
							memcpy(remoteSystem->withheldConnectionPacketData, data, byteSize);
							remoteSystem->withheldConnectionPacketLength = byteSize;
						}
					}
					else
					{
						// Already connected (or not expecting it): ignore it. Both sides of a cross connection send
						// this, so it arrives here on every simultaneous connect; a remote that repeats it must not make
						// us leak one per datagram (the receive loop frees it).
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
				}
				else if ((unsigned char) data[0] == ID_DISCONNECTION_NOTIFICATION && remoteSystem->connectMode == RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY)
				{
					// The slot is already closing silently, which is terminal: the application has either been told
					// how it ended (ID_CONNECTION_ATTEMPT_FAILED from FailSessionHandshakeAsConnectingSide, a
					// refused session, a wrong password) or was never told of it at all. Moving it to
					// DISCONNECT_ON_NO_ACK would report the close a second time, as ID_DISCONNECTION_NOTIFICATION
					// for a connection the application never saw. The reliability layer still acks the message, and
					// `message` frees it.
				}
				else if ((unsigned char) data[0] == ID_DISCONNECTION_NOTIFICATION)
				{
					// Stash any reason payload (everything after the 1-byte ID) so it can ride along with the
					// user-facing notification packet synthesized once outstanding ACKs are flushed. This raw
					// reliability-layer buffer is freed with `message`, so the bytes have to be copied out now.
					ClearDisconnectReason(remoteSystem);
					if (byteSize > sizeof(unsigned char))
					{
						const unsigned int reasonLength = byteSize - (unsigned int) sizeof(unsigned char);
						remoteSystem->disconnectReasonData = (unsigned char *) std::malloc(reasonLength);
						if (remoteSystem->disconnectReasonData != 0)
						{
							memcpy(remoteSystem->disconnectReasonData, data + sizeof(unsigned char), reasonLength);
							remoteSystem->disconnectReasonLength = reasonLength;
						}
					}

					// We shouldn't close the connection immediately because we need to ack the ID_DISCONNECTION_NOTIFICATION
					SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ON_NO_ACK);

					//	AddPacketToProducer(packet);
				}
				else if ((unsigned char) (data)[0] == ID_DETECT_LOST_CONNECTIONS && byteSize == sizeof(unsigned char))
				{
					// Do nothing
				}
				else if ((unsigned char) (data)[0] == ID_SESSION_CONFIG_REQUEST)
				{
					// Server side of the session handshake. The client's payload arrives before we answer, so a
					// server can inspect it and refuse without ever having disclosed its own.
					if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && !remoteSystem->sessionConfigIsConnectingSide && !remoteSystem->sessionConfigAwaitingLocalDecision)
					{
						if (StoreRemoteSessionConfig(remoteSystem, (const unsigned char *) data + sizeof(MessageID), byteSize - (unsigned int) sizeof(MessageID)) == false)
						{
							// Oversized payload is a protocol violation; drop without answering.
							SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY);
						}
						else
						{
							// Let the application decide. The connection stays unreported until it calls
							// AcceptSession() or RejectSession(), or the handshake times out below.
							remoteSystem->sessionConfigAwaitingLocalDecision = true;
							playerRoster->Pending(ToPeerGuid(remoteSystem->guid), remoteSystem->generation, systemAddress);
							packet = AllocPacket(byteSize, message.release());
							packet->connectionGeneration = remoteSystem->generation;
							packet->bitSize = bitSize;
							packet->systemAddress = systemAddress;
							packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
							packet->guid = remoteSystem->guid;
							packet->guid.systemIndex = packet->systemAddress.systemIndex;
							AddPacketToProducer(packet);
						}
					}
					else
					{
						// A repeat request, or one on an already-established connection: ignore it rather than
						// letting a peer re-run the handshake and re-produce the connection packet.
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
							// An oversized payload is a protocol violation (a server built with a larger
							// MAXIMUM_SESSION_CONFIG_SIZE than ours sends one: AcceptSession clamps to the sender's limit).
							// Our Connect() is still owed an answer, and the server has already reported the connection,
							// so fail the attempt once and tell the server, as on any other broken-off handshake.
							FailSessionHandshakeAsConnectingSide(remoteSystem);
						}
						else
						{
							SetConnectMode(*remoteSystem, RemoteSystemStruct::CONNECTED);
							remoteSystem->sessionConfigStartTime = 0;
							ProduceWithheldConnectionPacket(remoteSystem);
						}
					}
				}
				else if ((unsigned char) (data)[0] == ID_SESSION_CONFIG_STATUS)
				{
					// Client side, while the server's application decides: surface the status and restart our own
					// session timer, which is what lets a queued client wait longer than the timeout. Bound to the
					// connecting side like the other replies, so a client cannot push one at a server.
					if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && remoteSystem->sessionConfigIsConnectingSide)
					{
						remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit
						packet = AllocPacket(byteSize, message.release());
						packet->bitSize = bitSize;
						packet->systemAddress = systemAddress;
						packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
						packet->guid = remoteSystem->guid;
						packet->guid.systemIndex = packet->systemAddress.systemIndex;
						AddPacketToProducer(packet);
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
						packet = AllocPacket(byteSize, message.release());
						packet->data[0] = (unsigned char) ID_CONNECTION_ATTEMPT_FAILED;
						packet->bitSize = bitSize;
						packet->systemAddress = systemAddress;
						packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
						packet->guid = remoteSystem->guid;
						packet->guid.systemIndex = packet->systemAddress.systemIndex;
						AddPacketToProducer(packet);

						SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY);
					}
				}
				else if ((unsigned char) (data)[0] == ID_INVALID_PASSWORD)
				{
					if (remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION)
					{
						packet = AllocPacket(byteSize, message.release());
						packet->bitSize = bitSize;
						packet->systemAddress = systemAddress;
						packet->systemAddress.systemIndex = remoteSystem->remoteSystemIndex;
						packet->guid = remoteSystem->guid;
						packet->guid.systemIndex = packet->systemAddress.systemIndex;
						AddPacketToProducer(packet);

						SetConnectMode(*remoteSystem, RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY);
					}
				}
				else if ((unsigned char) (data)[0] == ID_CONNECTION_REQUEST_ACCEPTED)
				{
					if (byteSize > sizeof(MessageID) + sizeof(unsigned int) + sizeof(unsigned short) + sizeof(SystemIndex) + sizeof(MafiaNet::Time) * 2)
					{
						// Make sure this connection accept is from someone we wanted to connect to
						bool allowConnection, alreadyConnected;

						// It answers our ID_CONNECTION_REQUEST, which only the REPLY_2 handler sends, right after
						// setting weInitiatedTheConnection (never cleared while the slot lives). So it is only ever
						// legitimate on the initiating slot, including when simultaneous attempts crossed.
						// A closing slot (a cancelled or failed attempt) never restarts, not even with IP migration.
						if (remoteSystem->weInitiatedTheConnection &&
						    (remoteSystem->connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST ||
						     remoteSystem->connectMode == RemoteSystemStruct::REQUESTED_CONNECTION ||
						     (allowConnectionResponseIPMigration &&
						      remoteSystem->connectMode != RemoteSystemStruct::DISCONNECT_ASAP &&
						      remoteSystem->connectMode != RemoteSystemStruct::DISCONNECT_ASAP_SILENTLY &&
						      remoteSystem->connectMode != RemoteSystemStruct::DISCONNECT_ON_NO_ACK)))
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
							SetConnectMode(*remoteSystem, RemoteSystemStruct::EXCHANGING_SESSION_DATA);
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
							// copied because `message` frees `data` at the end of this iteration.
							//
							// With allowConnectionResponseIPMigration a repeated ID_CONNECTION_REQUEST_ACCEPTED on a
							// connection still in the handshake starts it over, and ClearSessionConfig() then frees the
							// application messages held so far. They were acked, so they are lost. Accepted: the option is
							// off by default, and a restarted handshake is already a restarted connection to the application.
							ClearSessionConfig(remoteSystem);
							remoteSystem->sessionConfigIsConnectingSide = true;
							remoteSystem->sessionConfigStartTime = timeMS != 0 ? (MafiaNet::TimeMS) timeMS : MafiaNet::GetTimeMS(); // compared with GetTimeMS(), 32-bit
							remoteSystem->withheldConnectionPacketData = (unsigned char *) std::malloc(byteSize);
							if (remoteSystem->withheldConnectionPacketData != 0)
							{
								memcpy(remoteSystem->withheldConnectionPacketData, data, byteSize);
								remoteSystem->withheldConnectionPacketLength = byteSize;
							}

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
						}
					}
					else
					{
						// Too short to be a connection accept: drop it. Not a RakAssert -- the
						// remote chooses this size, and a Debug build must not abort on it. The attempt
						// then times out normally.
					}
				}
				else
				{
					// Application data (or a plugin's message) from the remote. It reaches the application only on
					// a connection the application has been told about: never ahead of the connection packet, so
					// never for a guid it has not seen connect, and never from a peer it may still refuse.
					//
					// The connecting side is the one exception to dropping the rest. The accepting side reports the
					// connection as soon as it answers our session request and may send at once, so its first
					// messages can overtake its ID_SESSION_CONFIG -- always, when that datagram is lost. They have
					// been acked, so dropping them would lose them for good; they are held and delivered right
					// after ID_CONNECTION_REQUEST_ACCEPTED instead, in arrival order. Only messages ordered on
					// channel 0 would have queued behind the session reply on their own. A conforming accepting
					// side cannot send in its own handshake window (its application has not been told of us yet),
					// and data after a failed or refused handshake is for an attempt already answered, so those
					// are dropped.
					if (!MayReachApplicationFromRemote(data, byteSize) || !remoteSystem->isActive)
					{
						// Dropped: `message` frees it.
					}
					else if (remoteSystem->connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA && remoteSystem->sessionConfigIsConnectingSide &&
					         remoteSystem->weInitiatedTheConnection)
					{
						// Only on a connection we initiated: a peer we accepted never has a reply of ours to overtake.
						// sessionConfigIsConnectingSide implies it today, because the ID_CONNECTION_REQUEST_ACCEPTED
						// handler only flips a slot we initiated (#99); checked again here as defence in depth, so a
						// peer that connected to us can never make us hold its data.
						//
						// Bounded: exceeding the bound is a protocol violation, and failing the attempt beats
						// silently losing the one message that did not fit -- the very bug this hold exists for.
						if (!HoldApplicationMessage(remoteSystem, message.release(), bitSize))
							FailSessionHandshakeAsConnectingSide(remoteSystem);
					}
					else if (remoteSystem->connectionReportedToApplication)
					{
						DeliverApplicationMessage(remoteSystem, message.release(), bitSize);
					}
				}
			}
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

	BitStream updateBitStream(MAXIMUM_MTU_SIZE);
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
		if (rakPeer->userUpdateThreadPtr)
			rakPeer->userUpdateThreadPtr(rakPeer, rakPeer->userUpdateThreadData);

		rakPeer->RunUpdateCycle(updateBitStream);

		// Pending sends go out this often, unless quitAndDataEvents is set
		rakPeer->quitAndDataEvents.WaitOnEvent(10);
	}

	rakPeer->isMainLoopThreadActive = false;

	return 0;
}

void RakPeer::CallPluginCallbacks(std::vector<PluginInterface2 *> &pluginList, Packet *packet)
{
	for (unsigned int i = 0; i < pluginList.size(); i++)
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

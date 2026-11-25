/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2019, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "../sample_session.h"
#include <cstdio>
#include "../http_request.h"
#include "mafianet/format_string.h"
#include <cstring>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "mafianet/get_time.h"
#include "mafianet/rand.h"
#include "mafianet/peer_interface.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/fully_connected_mesh2.h"
#include "mafianet/kbhit.h"
#include "mafianet/sleep.h"
#include "mafianet/types.h"
#include "mafianet/bit_stream.h"
#include "mafianet/socket_layer.h"
#include "mafianet/replica_manager3.h"
#include "mafianet/gets.h"
#include "mafianet/itoa.h"
#include "mafianet/nat_punchthrough_client.h"
#include "mafianet/nat_type_detection_client.h"
#include "miniupnpc.h"
#include "upnpcommands.h"
#include "upnperrors.h"
#include "mafianet/tcp_interface.h"
#include "mafianet/ready_event.h"
#include "mafianet/packet_logger.h"
#include "mafianet/remote_procedure_call.h"
#include "mafianet/http_connection2.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"
#include "mafianet/linux_strings.h"
#include "mafianet/guid_util.h"

#include <algorithm>
#include <vector>

// See http://www.digip.org/jansson/doc/2.4/
// This is used to make it easier to parse the JSON returned from the master server
#include "jansson.h"

#define DEFAULT_SERVER_PORT "61111"
// Public test server
#define DEFAULT_SERVER_ADDRESS "natpunch.slikesoft.com"
#define NAT_TYPE_DETECTION_SERVER 0
#define USE_UPNP 1
#define MASTER_SERVER_ADDRESS "masterserver2.raknet.com"
//#define MASTER_SERVER_ADDRESS "localhost"
#define MASTER_SERVER_PORT 80
//#define MASTER_SERVER_PORT 8888

using namespace MafiaNet;

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Plugins demonstrated by the sample ComphrensivePCGame
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Purpose: UDP network communication
// Required?: Yes
RakPeerInterface *rakPeer;

// Purpose: Game object replication
// Required?: No, but manages object replication automatically. Some form of this is required for almost every game
ReplicationManager *gameReplicaManager3;

// Purpose: Lookup game objects given ID. Used by ReplicaManager3
// Required?: Required to use ReplicaManager3, and some form of this is required for almost every game

// Purpose: Connect to RakNet's hosted master server
// Required?: Steam and consoles provide server solutions, so usually not if you are releasing only on those platforms.
TCPInterface *tcp;

// Purpose: If UPNP fails, used to connect across routers.
// Required?: Steam and consoles provide server solutions that do this automatically. Otherwise, if UPNP fails you need this.
// Note: See the project NAT Punchthrough / NATCompleteServer for a sample server implementation
NatPunchthroughClient *natPunchthroughClient;

#ifdef NAT_TYPE_DETECTION_SERVER
// Purpose: Determine what type of router we are behind, if any
// Required?: No, but game sessions that are impossible to join can be filtered out from game listings
NatTypeDetectionClient *natTypeDetectionClient;
#endif

// Purpose: Used to call C functions on remote systems. Convenience function.
// Required?: No

// Purpose: Used for players to ready / unready in the lobby in a way that is properly synchronized under peer to peer
// Required?: No, but provides a more robust way to ready/unready in a peer to peer game
ReadyEvent *readyEvent;

// Purpose: Automatically determine and migrate the host of a peer to peer session. Avoid partial connection failures when joining peer-to-peer games.
// Required?: Yes, for peer to peer games that need a session host. Not necessary for client/server
FullyConnectedMesh2 *fullyConnectedMesh2;

// Purpose: Helper class to parse http connection events
// Required?: Technically no since you could parse the strings yourself. But it's a lot of work otherwise.
HTTPConnection2 *httpConnection2;

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Sample game classes using ReplicaManager3
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// Forward declarations
class User;
void PostRoomToMaster(void);

// Game contains the list of objects, and serializes if the game is locked, the name of the game, and if it is in gameplay or in the lobby
// Game is created on startup, rather than over the network, so is replicated as a static object.
// See "Object replication" in docs/guide/replication.rst.
class Game : public Replica3
{
public:
	enum Phase
	{
		CONNECTING_TO_SERVER,
		DETERMINE_NAT_TYPE,
		SEARCH_FOR_GAMES,
		NAT_PUNCH_TO_GAME_HOST,
		CONNECTING_TO_GAME_HOST,
		VERIFIED_JOIN,
		IN_LOBBY_WAITING_FOR_HOST,
		IN_LOBBY_WITH_HOST,
		IN_GAME,
		EXIT_SAMPLE,
	};
	Game()
	{
		myNatType = NAT_TYPE_UNKNOWN;
		masterServerRow = -1;
		Reset();
		whenToNextUpdateMasterServer = 0;
		masterServerQueryResult = 0;
	}
	virtual ~Game()
	{
		if (masterServerQueryResult)
			json_decref(masterServerQueryResult);
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void) replicaManager3;

		if (fullyConnectedMesh2->IsConnectedHost())
			return QueryConstruction_PeerToPeer(destinationConnection, R3P2PM_STATIC_OBJECT_CURRENTLY_AUTHORITATIVE);
		else
			return QueryConstruction_PeerToPeer(destinationConnection, R3P2PM_STATIC_OBJECT_NOT_CURRENTLY_AUTHORITATIVE);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		return sourceConnection->GetRakNetGUID() == fullyConnectedMesh2->GetConnectedHost();
	}
	virtual void SerializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// unused parameters
		(void) constructionBitstream;
		(void) destinationConnection;
	}
	virtual bool DeserializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) constructionBitstream;
		(void) sourceConnection;

		return true;
	}
	virtual void SerializeConstructionExisting(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// unused parameters
		(void) destinationConnection;

		constructionBitstream->Write(gameName);
		constructionBitstream->Write(lockGame);
		constructionBitstream->Write(gameInLobby);
		constructionBitstream->Write(masterServerRow);
	}
	virtual void DeserializeConstructionExisting(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) sourceConnection;

		constructionBitstream->Read(gameName);
		constructionBitstream->Read(lockGame);
		constructionBitstream->Read(gameInLobby);
		constructionBitstream->Read(masterServerRow);
		printf("Downloaded game. locked=%i. inLobby=%i\n", lockGame, gameInLobby);
		EnterPhase(gameInLobby ? IN_LOBBY_WITH_HOST : IN_GAME);
	}
	virtual void SerializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// unused parameters
		(void) destructionBitstream;
		(void) destinationConnection;
	}
	virtual bool DeserializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) destructionBitstream;
		(void) sourceConnection;

		return true;
	}
	virtual MafiaNet::RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		// unused parameters
		(void) droppedConnection;

		return RM3AOPC_DO_NOTHING;
	}
	virtual void DeallocReplica(MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) sourceConnection;

		delete this;
	}
	virtual MafiaNet::RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		if (fullyConnectedMesh2->IsConnectedHost())
			return QuerySerialization_PeerToPeer(destinationConnection, R3P2PM_STATIC_OBJECT_CURRENTLY_AUTHORITATIVE);
		else
			return QuerySerialization_PeerToPeer(destinationConnection, R3P2PM_STATIC_OBJECT_NOT_CURRENTLY_AUTHORITATIVE);
	}
	virtual RM3SerializationResult Serialize(MafiaNet::SerializeParameters *serializeParameters)
	{
		serializeParameters->outputBitstream[0].Write(lockGame);
		serializeParameters->outputBitstream[0].Write(gameInLobby);
		serializeParameters->outputBitstream[0].Write(masterServerRow);
		return RM3SR_BROADCAST_IDENTICALLY;
	}
	virtual void Deserialize(MafiaNet::DeserializeParameters *deserializeParameters)
	{
		if (deserializeParameters->bitstreamWrittenTo[0])
		{
			bool b;
			deserializeParameters->serializationBitstream[0].Read(b);
			if (b != lockGame)
			{
				lockGame = b;
				if (lockGame)
					printf("Game is no longer locked\n");
				else
					printf("Game is now locked\n");
			}
			deserializeParameters->serializationBitstream[0].Read(b);
			if (b != gameInLobby)
			{
				gameInLobby = b;
				if (gameInLobby)
				{
					readyEvent->DeleteEvent(0);
					EnterPhase(Game::IN_LOBBY_WITH_HOST);
					printf("Game is now in the lobby\n");
				}
				else
				{
					readyEvent->ForceCompletion(0);
					EnterPhase(Game::IN_GAME);
				}
			}
			deserializeParameters->serializationBitstream[0].Read(masterServerRow);
		}
	}
	void EnterPhase(Phase newPhase)
	{
		phase = newPhase;
		switch (newPhase)
		{
		case CONNECTING_TO_SERVER:
		{
			char port[256];
			printf("Enter address of server running the NATCompleteServer project.\nEnter for default: ");
			Gets(serverIPAddr, 256);
			if (serverIPAddr[0] == 0)
				strcpy_s(serverIPAddr, DEFAULT_SERVER_ADDRESS);
			printf("Enter server port, or enter for default: ");
			Gets(port, 256);
			if (port[0] == 0)
				strcpy_s(port, DEFAULT_SERVER_PORT);
			const int intServerPort = atoi(port);
			if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max()))
			{
				printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
				phase = EXIT_SAMPLE;
				break;
			}
			ConnectionAttemptResult car = rakPeer->Connect(serverIPAddr, static_cast<unsigned short>(intServerPort), 0, 0);
			if (car != MafiaNet::CONNECTION_ATTEMPT_STARTED)
			{
				printf("Failed connect call to %s. Code=%i\n", serverIPAddr, car);
				phase = EXIT_SAMPLE;
			}
		}
		break;
#ifdef NAT_TYPE_DETECTION_SERVER
		case DETERMINE_NAT_TYPE:
			printf("Determining NAT type...\n");
			natTypeDetectionClient->DetectNATType(natPunchServerAddress);
			break;
#endif
		case SEARCH_FOR_GAMES:
			SearchForGames();
			break;
		case NAT_PUNCH_TO_GAME_HOST:
			printf("Attempting NAT punch to host of game session...\n");
			break;
		case IN_LOBBY_WITH_HOST:
			printf("(R)eady to start\n");
			printf("(U)nready to start\n");
			break;
		case IN_GAME:
			printf("Game started.\n(C)hat in-game\n");
			break;
		}
	}

	void Reset(void)
	{
		lockGame = false;
		gameInLobby = true;
	}

	void SearchForGames(void)
	{
		printf("Downloading rooms...\n");

		std::string rsRequest = MakeHttpRequest("GET",
		                                        MASTER_SERVER_ADDRESS "/testServer?__gameId=comprehensivePCGame");
		httpConnection2->TransmitRequest(rsRequest.c_str(), MASTER_SERVER_ADDRESS, MASTER_SERVER_PORT);
	}

	// ---------------------------------------------------------------------------------
	// Serialized variables
	// ---------------------------------------------------------------------------------
	// Shows up in game listings
	std::string gameName;
	// If host locked the game, join queries are rejected
	bool lockGame;
	// Game is either in the lobby or in gameplay
	bool gameInLobby;
	// Which row of the master server our game was uploaded to
	// Returned from the POST request
	// This is serialized so if the host migrates, the new host takes over that row. Otherwise, a new row would be written for the same game.
	int masterServerRow;

	// ---------------------------------------------------------------------------------
	// Not serialized variables
	// ---------------------------------------------------------------------------------
	// Store what type of router I am behind
	MafiaNet::NATTypeDetectionResult myNatType;
	Phase phase;
	// NAT punchthrough server runs RakNet project NatCompleteServer with NAT_TYPE_DETECTION_SERVER, and NAT_PUNCHTHROUGH_SERVER
	RakNetGUID natPunchServerGuid;
	SystemAddress natPunchServerAddress;
	char serverIPAddr[256];
	// Just tracks what other objects have been created
	std::vector<User *> users;

	// Master server has to be refreshed periodically so it knows we didn't crash
	MafiaNet::Time whenToNextUpdateMasterServer;

	// Helper function to store and read the JSON from the GET request
	void SetMasterServerQueryResult(json_t *root)
	{
		if (masterServerQueryResult)
			json_decref(masterServerQueryResult);
		masterServerQueryResult = root;
	}

	json_t *GetMasterServerQueryResult(void)
	{
		if (masterServerQueryResult == 0)
			return 0;
		void *iter = json_object_iter(masterServerQueryResult);
		while (iter)
		{
			const char *firstKey = json_object_iter_key(iter);
			if (_stricmp(firstKey, "GET") == 0)
			{
				return json_object_iter_value(iter);
			}
			iter = json_object_iter_next(masterServerQueryResult, iter);
			RakAssert(iter != 0);
		}
		return 0;
	}

	// The GET request returns a string. I use http://www.digip.org/jansson/ to parse the string, and store the results.
	json_t *masterServerQueryResult;
	json_t *jsonArray;

} *game;


// User represents a player in the game
// Each system creates one user on startup. Other users are downloaded
class User : public Replica3
{
public:
	User()
	{
		game->users.push_back(this);
		natType = NAT_TYPE_UNKNOWN;
	}
	virtual ~User()
	{
		game->users.erase(std::find(game->users.begin(), game->users.end(), this));
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void) replicaManager3;

		// Whoever created the user replicates it.
		return QueryConstruction_PeerToPeer(destinationConnection);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) sourceConnection;

		return true;
	}
	virtual void SerializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// unused parameters
		(void) destinationConnection;

		constructionBitstream->Write(userName);
		constructionBitstream->WriteCasted<unsigned char>(natType);
		constructionBitstream->Write(playerGuid);
		constructionBitstream->Write(playerAddress);
	}
	virtual bool DeserializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) sourceConnection;

		constructionBitstream->Read(userName);
		constructionBitstream->ReadCasted<unsigned char>(natType);
		constructionBitstream->Read(playerGuid);
		constructionBitstream->Read(playerAddress);
		return true;
	}
	virtual void PostDeserializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) constructionBitstream;
		(void) sourceConnection;

		printf("Downloaded user. name=%s\n", userName.c_str());

		// Update the user count on the master server as new users join
		if (fullyConnectedMesh2->IsConnectedHost())
			PostRoomToMaster();
	}

	virtual void SerializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// unused parameters
		(void) destructionBitstream;
		(void) destinationConnection;
	}
	virtual bool DeserializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) destructionBitstream;
		(void) sourceConnection;

		return true;
	}
	virtual MafiaNet::RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		return QueryActionOnPopConnection_PeerToPeer(droppedConnection);
	}
	virtual void DeallocReplica(MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void) sourceConnection;

		delete this;
	}
	virtual MafiaNet::RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_PeerToPeer(destinationConnection);
	}
	virtual RM3SerializationResult Serialize(MafiaNet::SerializeParameters *serializeParameters)
	{
		// unused parameters
		(void) serializeParameters;

		return RM3SR_BROADCAST_IDENTICALLY;
	}
	virtual void Deserialize(MafiaNet::DeserializeParameters *deserializeParameters)
	{
		// unused parameters
		(void) deserializeParameters;
	}

	std::string userName;
	NATTypeDetectionResult natType;
	RakNetGUID playerGuid;
	SystemAddress playerAddress;
};


// Helper functions
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// Demonstrates how to use built-in RPC
void InGameChat(MafiaNet::BitStream *userData, Packet *packet, void *context)
{
	// unused parameters
	(void) packet;
	(void) context;

	std::string rs;
	userData->Read(rs);
	printf("%s\n", rs.c_str());
}
// Register the function where it is defined, which is easier than maintaining a bunch of RegisterSlot() calls in main()
// nullptr context: this handler is a free function with no owning instance to route back to.
RPCGlobalRegistration __InGameChat("InGameChat", InGameChat, nullptr, 0);

// Write roomName and a list of NATTypeDetectionResult to a bitStream
void SerializeToJSON(std::string &outputString, std::string &roomName, std::vector<NATTypeDetectionResult> &natTypes)
{
	outputString = MafiaNet::FormatText("'roomName': '%s', 'guid': '%s', 'natTypes' : [ ", roomName.c_str(), MafiaNet::to_string(rakPeer->GetMyGUID()).c_str());
	for (unsigned short i = 0; i < natTypes.size(); i++)
	{
		if (i != 0)
			outputString += ", ";
		std::string appendStr = MafiaNet::FormatText("{'type': %i}", natTypes[i]);
		outputString += appendStr;
	}
	outputString += " ] ";
}

// A system has connected and is ready to participate in the game
// Register this system with the plugins that need to know about new participants
// This operation happens after FullyConnectedMesh2 has told us about who the host is.
void RegisterGameParticipant(RakNetGUID guid)
{
	gameReplicaManager3->Admit(ToPeerGuid(guid));
	readyEvent->AddToWaitList(0, guid);
}

// Upload details about the current game state to the cloud
// This is the responsibility of the system that initially created that room.
// If that system disconnects, the new host, as determined by FullyConnectedMesh2 will reupload the room
void PostRoomToMaster(void)
{
	BitStream bsOut;
	std::string jsonSerializedRoom;
	std::vector<NATTypeDetectionResult> natTypes;
	for (unsigned int i = 0; i < game->users.size(); i++)
		natTypes.push_back(game->users[i]->natType);
	SerializeToJSON(jsonSerializedRoom, game->gameName, natTypes);

	std::string rowStr;
	if (game->masterServerRow != -1)
		rowStr = MafiaNet::FormatText("\"__rowId\": %i,", game->masterServerRow);

	// See http://masterserver2.raknet.com/
	std::string rsRequest = MakeHttpRequest("POST",
	                                        (const char *) MASTER_SERVER_ADDRESS "/testServer",
	                                        "text/plain; charset=UTF-8",
	                                        MafiaNet::FormatText("{'__gameId': 'comprehensivePCGame', '__clientReqId': '0', %s '__timeoutSec': '30', %s }", rowStr.c_str(), jsonSerializedRoom.c_str()));

	// Refresh the room again slightly less than every 30 seconds
	game->whenToNextUpdateMasterServer = MafiaNet::GetTime() + 30000 - 1000;

	httpConnection2->TransmitRequest(rsRequest.c_str(), MASTER_SERVER_ADDRESS, MASTER_SERVER_PORT);

	printf("Posted game session. In room.\n");
}
void ReleaseRoomFromCloud(void)
{
	std::string rsRequest = MakeHttpRequest("DELETE",
	                                        MafiaNet::FormatText(MASTER_SERVER_ADDRESS "/testServer?__gameId=comprehensivePCGame&__rowId=%i", game->masterServerRow));
	httpConnection2->TransmitRequest(rsRequest.c_str(), MASTER_SERVER_ADDRESS, MASTER_SERVER_PORT);
	game->masterServerRow = -1;
}

void CreateRoom(void)
{
	size_t arraySize;
	if (game->GetMasterServerQueryResult())
		arraySize = json_array_size(game->GetMasterServerQueryResult());
	else
		arraySize = 0;

	if (arraySize > 0)
	{
		printf("Enter room name: ");
		char rn[128];
		Gets(rn, 128);
		if (rn[0] == 0)
			strcpy_s(rn, "Unnamed");
		game->gameName = rn;
	}
	else
	{
		game->gameName = "Default room name";
	}

	// Upload the room to the server
	PostRoomToMaster();

	game->EnterPhase(Game::IN_LOBBY_WAITING_FOR_HOST);

	// So that time spent in single player does not count towards which system has been running the longest in multiplayer
	fullyConnectedMesh2->ResetHostCalculation();

	printf("(E)xit session\n");
}

#if USE_UPNP != 0
struct UPNPOpenWorkerArgs
{
	char buff[256];
	unsigned short portToOpen;
	unsigned int timeout;
	void *userData;
	void (*resultCallback)(bool success, unsigned short portToOpen, void *userData);
	void (*progressCallback)(const char *progressMsg, void *userData);
};
RAK_THREAD_DECLARATION(UPNPOpenWorker)
{
	UPNPOpenWorkerArgs *args = (UPNPOpenWorkerArgs *) arguments;
	bool success = false;

	// Behind a NAT. Try to open with UPNP to avoid doing NAT punchthrough
	struct UPNPDev *devlist = 0;
	devlist = upnpDiscover(args->timeout, 0, 0, 0, 0, 2, 0);
	if (devlist)
	{
		if (args->progressCallback)
			args->progressCallback("List of UPNP devices found on the network :\n", args->userData);
		struct UPNPDev *device;
		for (device = devlist; device; device = device->pNext)
		{
			sprintf_s(args->buff, " desc: %s\n st: %s\n\n", device->descURL, device->st);
			if (args->progressCallback)
				args->progressCallback(args->buff, args->userData);
		}

		char lanaddr[64]; /* my ip address on the LAN */
		char wanaddr[64]; /* my ip address on the WAN */
		struct UPNPUrls urls;
		struct IGDdatas data;
		if (UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr)) == 1)
		{
			char iport[32];
			Itoa(args->portToOpen, iport, 10);
			char eport[32];
			strcpy_s(eport, iport);

			// Version miniupnpc-2.2.8
			int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
			                            eport, iport, lanaddr, 0, "UDP", 0, "0");

			if (r != UPNPCOMMAND_SUCCESS)
				printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
				       eport, iport, lanaddr, r, strupnperror(r));

			char intPort[6];
			char intClient[16];

			// Version miniupnpc-2.2.8
			char desc[128];
			char enabled[128];
			char leaseDuration[128];
			r = UPNP_GetSpecificPortMappingEntry(urls.controlURL,
			                                     data.first.servicetype,
			                                     eport, "UDP", NULL,
			                                     intClient, intPort,
			                                     desc, enabled, leaseDuration);

			if (r != UPNPCOMMAND_SUCCESS)
			{
				sprintf_s(args->buff, "GetSpecificPortMappingEntry() failed with code %d (%s)\n",
				          r, strupnperror(r));
				if (args->progressCallback)
					args->progressCallback(args->buff, args->userData);
			}
			else
			{
				if (args->progressCallback)
					args->progressCallback("UPNP success.\n", args->userData);
				// game->myNatType=NAT_TYPE_SUPPORTS_UPNP;

				success = true;
			}
		}
	}

	if (args->resultCallback)
		args->resultCallback(success, args->portToOpen, args->userData);
	delete args;
	return 0;
}

void UPNPOpenAsynch(unsigned short portToOpen,
                    unsigned int timeout,
                    void (*progressCallback)(const char *progressMsg, void *userData),
                    void (*resultCallback)(bool success, unsigned short portToOpen, void *userData),
                    void *userData)
{
	UPNPOpenWorkerArgs *args = new UPNPOpenWorkerArgs;
	args->portToOpen = portToOpen;
	args->timeout = timeout;
	args->userData = userData;
	args->progressCallback = progressCallback;
	args->resultCallback = resultCallback;
	RakThread::Create(UPNPOpenWorker, args);
}

void UPNPProgressCallback(const char *progressMsg, void *userData)
{
	// unused parameters
	(void) userData;

	printf(progressMsg);
}
void UPNPResultCallback(bool success, unsigned short portToOpen, void *userData)
{
	// unused parameters
	(void) portToOpen;
	(void) userData;

	if (success)
		game->myNatType = NAT_TYPE_SUPPORTS_UPNP;
	game->EnterPhase(Game::SEARCH_FOR_GAMES);
}

void OpenUPNP(void)
{
	printf("Discovering UPNP...\n");

	std::vector<RakNetSocket2 *> sockets;
	rakPeer->GetSockets(sockets);
	UPNPOpenAsynch(sockets[0]->GetBoundAddress().GetPort(), 2000, UPNPProgressCallback, UPNPResultCallback, 0);
}

#endif

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

int main(void)
{
	printf("Demonstrates networking elements for a P2P game on the PC, self-released,\nwith player hosted game servers\n");
	printf("Difficulty: Advanced\n\n");

	/*
	json_t *jsonObject = json_object();
	json_object_set(jsonObject, "testKey", json_string("\"") );
	char *ds = json_dumps(jsonObject,0);
	*/

	// ---------------------------------------------------------------------------------------------------------------------
	// Allocate plugins. See declaration in this file for description of each
	// ---------------------------------------------------------------------------------------------------------------------
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	fullyConnectedMesh2 = FullyConnectedMesh2::GetInstance();
	tcp = TCPInterface::GetInstance();
	natPunchthroughClient = NatPunchthroughClient::GetInstance();
#ifdef NAT_TYPE_DETECTION_SERVER
	natTypeDetectionClient = NatTypeDetectionClient::GetInstance();
#endif
	readyEvent = ReadyEvent::GetInstance();
	gameReplicaManager3 = &rakPeer->GetReplication();
	gameReplicaManager3->SetRole(ReplicationRole::Peer);
	gameReplicaManager3->RegisterReplica<Game>(ReplicaTypeId{1}, 1);
	gameReplicaManager3->RegisterReplica<User>(ReplicaTypeId{2}, 1);
	httpConnection2 = HTTPConnection2::GetInstance();

	// ---------------------------------------------------------------------------------------------------------------------
	// Attach plugins
	// ---------------------------------------------------------------------------------------------------------------------
	rakPeer->AttachPlugin(fullyConnectedMesh2);
	rakPeer->AttachPlugin(natPunchthroughClient);
#ifdef NAT_TYPE_DETECTION_SERVER
	rakPeer->AttachPlugin(natTypeDetectionClient);
#endif
	rakPeer->AttachPlugin(readyEvent);
	/// TCPInterface supports plugins too
	tcp->AttachPlugin(httpConnection2);

	// ---------------------------------------------------------------------------------------------------------------------
	// Setup plugins: Disable automatically adding new connections. Allocate initial objects and register for replication
	// ---------------------------------------------------------------------------------------------------------------------
	// New connections do not count until after login.
	fullyConnectedMesh2->SetAutoparticipateConnections(false);

	// Do not automatically count new connections, but do drop lost connections automatically
	gameReplicaManager3->SetAutomaticAdmission(false);

	// Reference static game objects that always exist
	game = new Game;
	game->SetNetworkID(0);
	gameReplicaManager3->ReferenceStatic(game, 0);

	// Setup my own user
	User *user = new User;
	user->userName = MafiaNet::to_string(rakPeer->GetMyGUID()).c_str();

	// ---------------------------------------------------------------------------------------------------------------------
	// Startup RakNet on first available port
	// ---------------------------------------------------------------------------------------------------------------------
	MafiaNet::SocketDescriptor sd;
	sd.socketFamily = AF_INET; // Only IPV4 supports broadcast on 255.255.255.255
	sd.port = 0;
	SLNET_VERIFY(rakPeer->Startup(8, &sd, 1) == RAKNET_STARTED);
	rakPeer->SetMaximumIncomingConnections(8);
	rakPeer->SetTimeoutTime(30000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
	printf("Our guid is %s\n", MafiaNet::to_string(rakPeer->GetGuidFromSystemAddress(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)).c_str());
	printf("Started on %s\n", MafiaNet::to_string(rakPeer->GetMyBoundAddress()).c_str());

	// Start TCPInterface and begin connecting to the NAT punchthrough server
	tcp->Start(0, 0, 1);

	// Connect to hosting server
	game->EnterPhase(Game::CONNECTING_TO_SERVER);

	// ---------------------------------------------------------------------------------------------------------------------
	// Read packets loop
	// ---------------------------------------------------------------------------------------------------------------------
	int ch;
	Packet *packet;
	while (game->phase != Game::EXIT_SAMPLE)
	{
		for (packet = Sample::ReceiveAndAcceptRequests(rakPeer); packet; rakPeer->DeallocatePacket(packet), packet = Sample::ReceiveAndAcceptRequests(rakPeer))
		{
			switch (packet->data[0])
			{
			case ID_NEW_INCOMING_CONNECTION:
			{
				printf("ID_NEW_INCOMING_CONNECTION from %s. guid=%s.\n", MafiaNet::to_string(packet->systemAddress).c_str(), MafiaNet::to_string(packet->guid).c_str());
			}
			break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
			{
				printf("ID_CONNECTION_REQUEST_ACCEPTED from %s,guid=%s\n", MafiaNet::to_string(packet->systemAddress).c_str(), MafiaNet::to_string(packet->guid).c_str());

				if (game->phase == Game::CONNECTING_TO_SERVER)
				{
					game->natPunchServerAddress = packet->systemAddress;
					game->natPunchServerGuid = packet->guid;

					// ---------------------------------------------------------------------------------------------------------------------
					// PC self-hosted servers only: Use the NAT punch server to determine NAT type. Attempt to open router if needed.
					// ---------------------------------------------------------------------------------------------------------------------
					if (NAT_TYPE_DETECTION_SERVER)
					{
						game->EnterPhase(Game::DETERMINE_NAT_TYPE);
					}
					else
					{
						OpenUPNP();
					}
				}
				else if (game->phase == Game::CONNECTING_TO_GAME_HOST)
				{
					printf("Asking host to join session...\n");

					// So time in single player does not count towards which system has been running multiplayer the longest
					fullyConnectedMesh2->ResetHostCalculation();

					// Custom message to ask to join the game
					// We first connect to the game host, and the game host is responsible for calling StartVerifiedJoin() for us to join the session
					BitStream bsOut;
					bsOut.Write((MessageID) ID_USER_PACKET_ENUM);
					rakPeer->Send(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->guid, false);
				}
			}
			break;
			case ID_CONNECTION_LOST:
			case ID_DISCONNECTION_NOTIFICATION:
				if (game->phase == Game::DETERMINE_NAT_TYPE)
				{
					printf("Lost connection during NAT type detection. Reason %s. Retrying...\n", PacketLogger::BaseIDTOString(packet->data[0]));
					game->EnterPhase(Game::CONNECTING_TO_SERVER);
				}
				else if (game->phase == Game::NAT_PUNCH_TO_GAME_HOST)
				{
					printf("Lost connection during NAT punch to game host. Reason %s.\n", PacketLogger::BaseIDTOString(packet->data[0]));
					game->EnterPhase(Game::SEARCH_FOR_GAMES);
				}
				else
				{
					if (packet->guid == game->natPunchServerGuid)
					{
						printf("Server connection lost. Reason %s.\nGame session is no longer searchable.\n", PacketLogger::BaseIDTOString(packet->data[0]));
					}
					else
					{
						printf("Peer connection lost. Reason %s.\n", PacketLogger::BaseIDTOString(packet->data[0]));
					}
				}
				break;

			case ID_ALREADY_CONNECTED:
				printf("ID_ALREADY_CONNECTED with guid %" PRINTF_64_BIT_MODIFIER "u\n", packet->guid.g);
				break;

			case ID_INVALID_PASSWORD:
			case ID_NO_FREE_INCOMING_CONNECTIONS:
			case ID_CONNECTION_ATTEMPT_FAILED:
			case ID_CONNECTION_BANNED:
			case ID_IP_RECENTLY_CONNECTED:
			case ID_INCOMPATIBLE_PROTOCOL_VERSION:
				// Note: Failing to connect to another system does not automatically mean we cannot join a session, since that system may be disconnecting from the host simultaneously
				// FullyConnectedMesh2::StartVerifiedJoin() internally handles success or failure and notifies the client through ID_FCM2_VERIFIED_JOIN_FAILED if needed.
				printf("Failed to connect to %s. Reason %s\n", MafiaNet::to_string(packet->systemAddress).c_str(), PacketLogger::BaseIDTOString(packet->data[0]));

				if (game->phase == Game::CONNECTING_TO_SERVER)
					game->EnterPhase(Game::EXIT_SAMPLE);
				break;

			case ID_FCM2_NEW_HOST:
			{
				MafiaNet::BitStream bs(packet->data, packet->length, false);
				bs.IgnoreBytes(1);
				RakNetGUID oldHost;
				bs.Read(oldHost);

				if (packet->guid == rakPeer->GetMyGUID())
				{
					if (oldHost != UNASSIGNED_RAKNET_GUID)
					{
						if (game->phase == Game::IN_LOBBY_WAITING_FOR_HOST)
							game->phase = Game::IN_LOBBY_WITH_HOST;
						PostRoomToMaster();
						printf("ID_FCM2_NEW_HOST: Taking over as host from the old host.\nNew options:\n");
					}
					else
					{
						// Room not hosted if we become host the first time since this was done in CreateRoom() already
						printf("ID_FCM2_NEW_HOST: We have become host for the first time. New options:\n");
					}

					printf("(L)ock and unlock game\n");
				}
				else
				{
					if (oldHost != UNASSIGNED_RAKNET_GUID)
						printf("ID_FCM2_NEW_HOST: A new system %s has become host, GUID=%s\n", MafiaNet::to_string(packet->systemAddress).c_str(), MafiaNet::to_string(packet->guid).c_str());
					else
						printf("ID_FCM2_NEW_HOST: System %s is host, GUID=%s\n", MafiaNet::to_string(packet->systemAddress).c_str(), MafiaNet::to_string(packet->guid).c_str());
				}

				if (oldHost == UNASSIGNED_RAKNET_GUID)
				{
					// First time calculated host. Add existing connections to ReplicaManager3
					std::vector<RakNetGUID> participantList;
					fullyConnectedMesh2->GetParticipantList(participantList);
					for (unsigned int i = 0; i < participantList.size(); i++)
						RegisterGameParticipant(participantList[i]);

					// Reference the user we created (host or not)
					for (unsigned int i = 0; i < game->users.size(); i++)
						gameReplicaManager3->Reference(game->users[i]);
				}
			}
			break;
			case ID_NAT_TARGET_NOT_CONNECTED:
			case ID_NAT_TARGET_UNRESPONSIVE:
			case ID_NAT_CONNECTION_TO_TARGET_LOST:
			case ID_NAT_PUNCHTHROUGH_FAILED:
			{
				// As with connection failed, this does not automatically mean we cannot join the session
				// We only fail on ID_FCM2_VERIFIED_JOIN_FAILED
				printf("NAT punch to %s failed. Reason %s\n", MafiaNet::to_string(packet->guid).c_str(), PacketLogger::BaseIDTOString(packet->data[0]));

				if (game->phase == Game::NAT_PUNCH_TO_GAME_HOST)
					game->EnterPhase(Game::SEARCH_FOR_GAMES);
			}

			case ID_NAT_ALREADY_IN_PROGRESS:
				// Can ignore this
				break;

			case ID_NAT_PUNCHTHROUGH_SUCCEEDED:
			{
				if (game->phase == Game::NAT_PUNCH_TO_GAME_HOST || game->phase == Game::VERIFIED_JOIN)
				{
					// Connect to the session host
					ConnectionAttemptResult car = rakPeer->Connect(MafiaNet::to_string(packet->systemAddress, false).c_str(), packet->systemAddress.GetPort(), 0, 0);
					if (car != MafiaNet::CONNECTION_ATTEMPT_STARTED)
					{
						printf("Failed connect call to %s. Code=%i\n", MafiaNet::to_string(packet->systemAddress, false).c_str(), car);
						game->EnterPhase(Game::SEARCH_FOR_GAMES);
					}
					else
					{
						if (game->phase == Game::NAT_PUNCH_TO_GAME_HOST)
						{
							printf("NAT punch completed. Connecting to %s (game host)...\n", MafiaNet::to_string(packet->systemAddress).c_str());
							game->EnterPhase(Game::CONNECTING_TO_GAME_HOST);
						}
						else
						{
							printf("NAT punch completed. Connecting to %s (game client)...\n", MafiaNet::to_string(packet->systemAddress).c_str());
						}
					}
				}
			}
			break;


			case ID_NAT_TYPE_DETECTION_RESULT:
			{
				game->myNatType = (MafiaNet::NATTypeDetectionResult) packet->data[1];
				printf("NAT Type is %s (%s)\n", NATTypeDetectionResultToString(game->myNatType), NATTypeDetectionResultToStringFriendly(game->myNatType));

				if (game->myNatType != MafiaNet::NAT_TYPE_NONE)
				{
					OpenUPNP();
				}

				if (game->myNatType == MafiaNet::NAT_TYPE_PORT_RESTRICTED || game->myNatType == MafiaNet::NAT_TYPE_SYMMETRIC)
				{
					printf("Note: Your router must support UPNP or have the user manually forward ports.\n");
					printf("Otherwise NATPunchthrough may not always succeed.\n");
				}

				game->EnterPhase(Game::SEARCH_FOR_GAMES);
			}
			break;

			case ID_READY_EVENT_ALL_SET:
				printf("Got ID_READY_EVENT_ALL_SET from %s\n", MafiaNet::to_string(packet->systemAddress).c_str());
				printf("All users ready.\n");
				if (fullyConnectedMesh2->IsConnectedHost())
					printf("New options:\n(B)egin gameplay\n");
				break;

			case ID_READY_EVENT_SET:
				printf("Got ID_READY_EVENT_SET from %s\n", MafiaNet::to_string(packet->systemAddress).c_str());
				break;

			case ID_READY_EVENT_UNSET:
				printf("Got ID_READY_EVENT_UNSET from %s\n", MafiaNet::to_string(packet->systemAddress).c_str());
				break;

			// ID_USER_PACKET_ENUM is used by this sample as a custom message to ask to join a game
			case ID_USER_PACKET_ENUM:
				if (game->phase > Game::SEARCH_FOR_GAMES)
				{
					printf("Got request from client to join session.\nExecuting StartVerifiedJoin()\n");
					fullyConnectedMesh2->StartVerifiedJoin(packet->guid);
				}
				else
				{
					BitStream bsOut;
					bsOut.Write((MessageID) (ID_USER_PACKET_ENUM + 1));
					rakPeer->Send(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->guid, false);
				}
				break;
			// ID_USER_PACKET_ENUM+1 is used by this sample as a custom message to reject a join game request
			// Requests may also be later rejected through FullyConnectedMesh2::RespondOnVerifiedJoinCapable() to send ID_FCM2_VERIFIED_JOIN_REJECTED
			case (ID_USER_PACKET_ENUM + 1):
				printf("Join request denied\n");
				game->EnterPhase(Game::SEARCH_FOR_GAMES);
				break;
			case ID_FCM2_VERIFIED_JOIN_START:
			{
				game->EnterPhase(Game::VERIFIED_JOIN);

				// This message means the session host sent us a list of systems in the session
				// Once we connect to, or fail to connect to, each of these systems we will get ID_FCM2_VERIFIED_JOIN_FAILED, ID_FCM2_VERIFIED_JOIN_ACCEPTED, or ID_FCM2_VERIFIED_JOIN_REJECTED
				printf("Host sent us system list. Doing NAT punch to each system...\n");
				std::vector<SystemAddress> addresses;
				std::vector<RakNetGUID> guids;
				std::vector<BitStream *> userData;
				fullyConnectedMesh2->GetVerifiedJoinRequiredProcessingList(packet->guid, addresses, guids, userData);
				for (unsigned int i = 0; i < guids.size(); i++)
					natPunchthroughClient->OpenNAT(guids[i], game->natPunchServerAddress);
			}
			break;

			case ID_FCM2_VERIFIED_JOIN_CAPABLE:
				printf("Client is capable of joining FullyConnectedMesh2.\n");
				if (game->lockGame)
				{
					MafiaNet::BitStream bsOut;
					bsOut.Write("Game is locked");
					fullyConnectedMesh2->RespondOnVerifiedJoinCapable(packet, false, &bsOut);
				}
				else
					fullyConnectedMesh2->RespondOnVerifiedJoinCapable(packet, true, 0);
				break;

			case ID_FCM2_VERIFIED_JOIN_ACCEPTED:
			{
				std::vector<RakNetGUID> systemsAccepted;
				bool thisSystemAccepted;
				fullyConnectedMesh2->GetVerifiedJoinAcceptedAdditionalData(packet, &thisSystemAccepted, systemsAccepted, 0);
				if (thisSystemAccepted)
					printf("Game join request accepted\n");
				else
					printf("System %s joined the mesh\n", MafiaNet::to_string(systemsAccepted[0]).c_str());

				// Add the new participant to the game if we already know who the host is. Otherwise do this
				// once ID_FCM2_NEW_HOST arrives
				if (fullyConnectedMesh2->GetConnectedHost() != UNASSIGNED_RAKNET_GUID)
				{
					// FullyConnectedMesh2 already called AddParticipant() for each accepted system
					// Still need to add those systems to the other plugins though
					for (unsigned int i = 0; i < systemsAccepted.size(); i++)
						RegisterGameParticipant(systemsAccepted[i]);

					if (thisSystemAccepted)
						game->EnterPhase(Game::IN_LOBBY_WITH_HOST);
				}
				else
				{
					if (thisSystemAccepted)
						game->EnterPhase(Game::IN_LOBBY_WAITING_FOR_HOST);
				}

				printf("(E)xit room\n");
			}
			break;

			case ID_FCM2_VERIFIED_JOIN_REJECTED:
			{
				BitStream additionalData;
				fullyConnectedMesh2->GetVerifiedJoinRejectedAdditionalData(packet, &additionalData);
				std::string reason;
				additionalData.Read(reason);
				printf("Join rejected. Reason=%s\n", reason.c_str());
				rakPeer->CloseConnection(packet->guid, true);
				game->EnterPhase(Game::SEARCH_FOR_GAMES);
				break;
			}
			}
		}

		// The following code is TCP operations for talking to the master server, and parsing the reply
		SystemAddress sa;
		// This is kind of crappy, but for TCP plugins, always do HasCompletedConnectionAttempt, then Receive(), then HasFailedConnectionAttempt(),HasLostConnection()
		sa = tcp->HasCompletedConnectionAttempt();
		for (packet = tcp->Receive(); packet; tcp->DeallocatePacket(packet), packet = tcp->Receive())
			;
		sa = tcp->HasFailedConnectionAttempt();
		sa = tcp->HasLostConnection();

		std::string stringTransmitted;
		std::string hostTransmitted;
		std::string responseReceived;
		SystemAddress hostReceived;
		ptrdiff_t contentOffset;
		if (httpConnection2->GetResponse(stringTransmitted, hostTransmitted, responseReceived, hostReceived, contentOffset))
		{
			if (responseReceived.empty() == false)
			{
				if (contentOffset == -1)
				{
					// No content
					printf("%s", responseReceived.c_str());
				}
				else
				{
					json_error_t error;
					json_t *root = json_loads(responseReceived.c_str() + contentOffset, JSON_REJECT_DUPLICATES, &error);
					if (!root)
					{
						printf("Error parsing JSON\n");
					}
					else
					{
						void *iter = json_object_iter(root);
						while (iter)
						{
							const char *firstKey = json_object_iter_key(iter);
							if (_stricmp(firstKey, "GET") == 0)
							{
								game->SetMasterServerQueryResult(root);
								root = 0;

								json_t *jsonArray = json_object_iter_value(iter);
								size_t arraySize = json_array_size(jsonArray);
								for (unsigned int i = 0; i < arraySize; i++)
								{
									json_t *object = json_array_get(jsonArray, i);
									json_t *roomNameVal = json_object_get(object, "roomName");
									RakAssert(roomNameVal->type == JSON_STRING);
									json_t *natTypesVal = json_object_get(object, "natTypes");
									RakAssert(natTypesVal->type == JSON_ARRAY);
									size_t natTypesSize = json_array_size(natTypesVal);
									printf("Room name: %s. Players: %zu\n", json_string_value(roomNameVal), natTypesSize);
								}

								if (arraySize == 0)
									printf("No rooms.\n");


								printf("(J)oin room\n");
								printf("(C)reate room\n");
								printf("(S)earch rooms\n");
								break;
							}
							else if (_stricmp(firstKey, "POST") == 0)
							{
								RakAssert(_stricmp(firstKey, "POST") == 0);

								json_t *jsonObject = json_object_iter_value(iter);
								json_t *val1 = json_object_get(jsonObject, "__rowId");
								RakAssert(val1->type == JSON_INTEGER);
								game->masterServerRow = (int) json_integer_value(val1);

								printf("Session posted to row %i\n", game->masterServerRow);
								break;
							}
							else
							{
								iter = json_object_iter_next(root, iter);
								RakAssert(iter != 0);
							}
						}

						json_decref(root);
					}
				}
			}
		}

		if (_kbhit())
		{
			ch = _getch();

			if (game->phase == Game::SEARCH_FOR_GAMES)
			{
				if (ch == 'c' || ch == 'C')
				{
					CreateRoom();
				}
				if (ch == 's' || ch == 'S')
				{
					game->SearchForGames();
				}
				else if (ch == 'j' || ch == 'J')
				{
					size_t arraySize = 0;
					json_t *jsonArray = game->GetMasterServerQueryResult();
					if (jsonArray)
					{
						arraySize = json_array_size(jsonArray);
					}

					// Join room
					if (arraySize == 0)
					{
						printf("No rooms to join.\n");
					}
					else
					{
						int index;
						if (arraySize > 1)
						{
							printf("Enter index of room to join.\n");
							char indexstr[64];
							Gets(indexstr, 64);
							index = atoi(indexstr);
						}
						else
						{
							index = 0;
						}

						if (index < 0 || (unsigned int) index >= arraySize)
						{
							printf("Index out of range.\n");
						}
						else
						{
							json_t *object = json_array_get(jsonArray, index);
							json_t *guidVal = json_object_get(object, "guid");
							RakAssert(guidVal->type == JSON_STRING);
							RakNetGUID clientGUID;
							clientGUID.FromString(json_string_value(guidVal));
							if (clientGUID != rakPeer->GetMyGUID())
							{
								natPunchthroughClient->OpenNAT(clientGUID, game->natPunchServerAddress);
								game->EnterPhase(Game::NAT_PUNCH_TO_GAME_HOST);
							}
							else
							{
								printf("Cannot join your own room\n");
							}
						}
					}
				}
			}
			else
			{
				if (game->phase == Game::IN_GAME)
				{
					if (ch == 'c' || ch == 'C')
					{
						std::vector<RakNetGUID> participantList;
						fullyConnectedMesh2->GetParticipantList(participantList);

						if (participantList.size() > 0)
						{
							printf("Enter in-game chat message: ");
							char str[256];
							Gets(str, 256);
							BitStream bsOut;
							bsOut.Write(str);
							for (unsigned int i = 0; i < participantList.size(); i++)
								rakPeer->GetRPC().Signal("InGameChat", &bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, participantList[i], false, false);
						}
					}
				}

				if (ch == 'r' || ch == 'R')
				{
					if (readyEvent->SetEvent(0, true))
						printf("We are ready to start.\n");
				}
				else if (ch == 'u' || ch == 'U')
				{
					if (readyEvent->SetEvent(0, false))
						printf("We are no longer ready to start.\n");
				}
				else if (ch == 'l' || ch == 'L')
				{
					if (fullyConnectedMesh2->IsConnectedHost())
					{
						if (game->lockGame)
						{
							printf("Game is no longer locked\n");
							game->lockGame = false;
						}
						else
						{
							printf("Game is now locked\n");
							game->lockGame = true;
						}
					}
				}
				else if (ch == 'b' || ch == 'B')
				{
					if (fullyConnectedMesh2->IsConnectedHost())
					{
						if (game->gameInLobby)
						{
							readyEvent->ForceCompletion(0);
							game->gameInLobby = false;
							game->EnterPhase(Game::IN_GAME);
						}
						else
						{
							readyEvent->DeleteEvent(0);
							printf("Game ended, and now in lobby\n");
							game->gameInLobby = true;
							game->EnterPhase(Game::IN_LOBBY_WITH_HOST);
						}
					}
				}
				else if (ch == 'e' || ch == 'E')
				{
					// Disconnect from FullyConnectedMesh2 participants
					std::vector<RakNetGUID> participantList;
					fullyConnectedMesh2->GetParticipantList(participantList);
					for (unsigned int i = 0; i < participantList.size(); i++)
						rakPeer->CloseConnection(participantList[i], true);

					// If we were the host, no longer list this session
					// The new host will call PostRoomToCloud to reupload under a new IP address on ID_FCM2_NEW_HOST
					ReleaseRoomFromCloud();

					// Clear out state data from plugins
					fullyConnectedMesh2->Clear();
					readyEvent->DeleteEvent(0);
					gameReplicaManager3->Dereference(game);
					gameReplicaManager3->ReferenceStatic(game, 0);

					game->Reset();
					game->EnterPhase(Game::SEARCH_FOR_GAMES);
				}
				else if (ch == 'q' || ch == 'Q')
				{
					printf("Quitting.\n");

					std::string rspost = MakeHttpRequest("GET",
					                                     MafiaNet::FormatText(MASTER_SERVER_ADDRESS "/testServer?row=%i", game->masterServerRow));
					httpConnection2->TransmitRequest(rspost.c_str(), MASTER_SERVER_ADDRESS, MASTER_SERVER_PORT);

					game->EnterPhase(Game::EXIT_SAMPLE);
				}
			}
		}

		// The game host updates the master server
		MafiaNet::Time t = MafiaNet::GetTime();
		if ((fullyConnectedMesh2->IsConnectedHost() || game->users.size() == 1) &&
		    t > game->whenToNextUpdateMasterServer &&
		    (game->phase == Game::IN_LOBBY_WITH_HOST ||
		     game->phase == Game::IN_GAME ||
		     game->phase == Game::IN_LOBBY_WAITING_FOR_HOST))
		{
			PostRoomToMaster();
		}

		RakSleep(30);
	}

	rakPeer->Shutdown(100);

	while (game->users.size())
		delete game->users[game->users.size() - 1];
	delete game;

	RakPeerInterface::DestroyInstance(rakPeer);
	FullyConnectedMesh2::DestroyInstance(fullyConnectedMesh2);
	NatPunchthroughClient::DestroyInstance(natPunchthroughClient);
	NatTypeDetectionClient::DestroyInstance(natTypeDetectionClient);
	ReadyEvent::DestroyInstance(readyEvent);
	HTTPConnection2::DestroyInstance(httpConnection2);

	return 1;
}

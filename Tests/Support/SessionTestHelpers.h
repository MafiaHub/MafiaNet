// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"

namespace SessionTestHelpers
{
// This application's hosts accept each request with an empty response.
inline MafiaNet::Packet *ReceiveAndAcceptRequests(MafiaNet::RakPeerInterface *peer)
{
	while (auto *packet = peer->Receive())
	{
		if (packet->data[0] != ID_SESSION_CONFIG_REQUEST)
			return packet;
		peer->AcceptSession(packet->guid, nullptr, 0);
		peer->DeallocatePacket(packet);
	}
	return nullptr;
}

inline void DrainAndAcceptRequests(MafiaNet::RakPeerInterface *peer)
{
	while (auto *packet = ReceiveAndAcceptRequests(peer))
		peer->DeallocatePacket(packet);
}
}

/*
 *  Copyright (c) 2019, SLikeSoft UG (haftungsbeschraenkt)
 *
 *  This source code is  licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

/// \file mafianet.h
/// \brief Umbrella header aggregating the core public MafiaNet API.
///
/// Include this single header to pull in the common client/server path:
/// \code
/// #include "mafianet/mafianet.h"
/// \endcode
///
/// This is purely additive — the granular headers under "mafianet/" remain
/// available for advanced users who want fine-grained control over includes.
///
/// \note Encryption headers are intentionally omitted. Connection security is
/// opt-in via RakPeerInterface::InitializeSecurity() (gated behind the
/// LIBCAT_SECURITY build define) and is not part of the common path.

#pragma once

#include "mafianet/peer_interface.h"      // RakPeerInterface — main entry point
#include "mafianet/types.h"              // Packet, SystemAddress, RakNetGUID, enums
#include "mafianet/message_identifiers.h" // ID_* message IDs + ID_USER_PACKET_ENUM
#include "mafianet/packet_priority.h"     // MafiaNet::Priority / MafiaNet::Reliability
#include "mafianet/statistics.h"         // RakNetStatistics — return type of GetStatistics()
#include "mafianet/bit_stream.h"          // binary serialization
#include "mafianet/archive.h"            // serialize() convention over BitStream
#include "mafianet/get_time.h"            // MafiaNet::GetTime / TimeMS
#include "mafianet/guid_util.h"          // MafiaNet::to_string / connected_address
#include "mafianet/aliases.h"            // canonical aliases: PeerInterface, Guid, Statistics
#include "mafianet/peer_handle.h"         // RAII handles: Peer, PacketPtr
#include "mafianet/dispatcher.h"         // typed message dispatcher: Dispatcher, Sender

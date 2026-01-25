/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2024, MafiaHub
 *
 *  This source code was modified by MafiaHub. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief Message IDs for the MasterServer sample
///

#ifndef __MASTER_SERVER_MESSAGE_IDS_H
#define __MASTER_SERVER_MESSAGE_IDS_H

#include "mafianet/MessageIdentifiers.h"

/// Master Server message IDs - these were removed from core MafiaNet
/// and are now sample-specific. They start at ID_USER_PACKET_ENUM.
enum MasterServerMessageIDs
{
	/// Query the master server for a list of servers
	ID_QUERY_MASTER_SERVER = ID_USER_PACKET_ENUM,

	/// Register a server with the master server
	ID_MASTER_SERVER_SET_SERVER,

	/// Update server info on the master server
	ID_MASTER_SERVER_UPDATE_SERVER,

	/// Remove a server from the master server
	ID_MASTER_SERVER_DELIST_SERVER,

	/// Relay connection notification to a game server
	ID_RELAYED_CONNECTION_NOTIFICATION,

	/// Pong response (alias for ID_UNCONNECTED_PONG for legacy compatibility)
	ID_PONG = ID_UNCONNECTED_PONG
};

#endif

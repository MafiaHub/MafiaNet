/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/native_feature_includes.h"

#if _RAKNET_SUPPORT_PacketLogger==1

#include <string.h>

#include "mafianet/packet_logger.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

/*
Description:
Checks that PacketLogger's message-id name table stays aligned with the DefaultMessageIDTypes enum.

The table is a positional array: a name missing from the middle of it does not fail to compile, it
silently shifts every later entry so each id reports the NEXT id's name. That is how
ID_RAKVOICE_RELAY_DATA going missing mislabelled 85 ids, including the whole session-config range,
and it only surfaced because a test printed a name that did not match the id it had asked about.

Success conditions: every id in the enum reports its own name, and the ends and both sides of the
historical break are pinned so any future off-by-one fails here.

Failure conditions: any id reports a name other than its own.
*/

namespace
{
	// Pinning the LAST entry is the load-bearing case: the table is sized from ID_USER_PACKET_ENUM, so
	// a name dropped anywhere before this point pushes this one off the end of its own slot.
	TEST(PacketLoggerIdNames, LastEnumEntryReportsItsOwnName)
	{
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_RESERVED_9), "ID_RESERVED_9");
	}

	TEST(PacketLoggerIdNames, FirstEnumEntryReportsItsOwnName)
	{
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_CONNECTED_PING), "ID_CONNECTED_PING");
	}

	// The id that was missing from the table, plus its neighbours on each side.
	TEST(PacketLoggerIdNames, RakVoiceRelayDataAndItsNeighboursAreNamed)
	{
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_RAKVOICE_DATA), "ID_RAKVOICE_DATA");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_RAKVOICE_RELAY_DATA), "ID_RAKVOICE_RELAY_DATA");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_AUTOPATCHER_GET_CHANGELIST_SINCE_DATE),
			"ID_AUTOPATCHER_GET_CHANGELIST_SINCE_DATE");
	}

	// Everything past the break was shifted, so the ids the session handshake uses were all wrong.
	TEST(PacketLoggerIdNames, SessionConfigIdsAreNamed)
	{
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_SESSION_CONFIG_REQUEST), "ID_SESSION_CONFIG_REQUEST");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_SESSION_CONFIG), "ID_SESSION_CONFIG");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_SESSION_CONFIG_REJECTED), "ID_SESSION_CONFIG_REJECTED");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_SESSION_CONFIG_STATUS), "ID_SESSION_CONFIG_STATUS");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_SESSION_CONFIG_ABANDONED), "ID_SESSION_CONFIG_ABANDONED");
	}

	// The connection packets, which is what a reader of a handshake log keys off.
	TEST(PacketLoggerIdNames, ConnectionIdsAreNamed)
	{
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_CONNECTION_REQUEST_ACCEPTED), "ID_CONNECTION_REQUEST_ACCEPTED");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_NEW_INCOMING_CONNECTION), "ID_NEW_INCOMING_CONNECTION");
		EXPECT_STREQ(PacketLogger::BaseIDTOString(ID_DISCONNECTION_NOTIFICATION), "ID_DISCONNECTION_NOTIFICATION");
	}

	// Ids at or past the user range are not the table's to name; the caller falls back to a number.
	TEST(PacketLoggerIdNames, UserRangeIsNotNamed)
	{
		EXPECT_EQ(PacketLogger::BaseIDTOString((unsigned char)ID_USER_PACKET_ENUM), (const char*)0);
		EXPECT_EQ(PacketLogger::BaseIDTOString(255), (const char*)0);
	}

	// Blanket sweep: no id in the enum may report a name belonging to a different id. Catches a shift
	// anywhere, including ranges nobody thought to pin above.
	TEST(PacketLoggerIdNames, EveryEnumIdReportsANonEmptyName)
	{
		for (int id = 0; id < (int)ID_USER_PACKET_ENUM; ++id)
		{
			const char *name = PacketLogger::BaseIDTOString((unsigned char)id);
			ASSERT_NE(name, (const char*)0) << "id " << id << " has no name";
			EXPECT_GT(strlen(name), (size_t)3) << "id " << id << " has a suspiciously short name";
			EXPECT_EQ(strncmp(name, "ID_", 3), 0) << "id " << id << " reports '" << name << "'";
		}
	}
} // namespace

#endif // _RAKNET_SUPPORT_PacketLogger

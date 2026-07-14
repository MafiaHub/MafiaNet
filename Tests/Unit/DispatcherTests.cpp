/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/Dispatcher.h"
#include "mafianet/BitStream.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/GetTime.h"

#include <string>

using namespace MafiaNet;

namespace
{
	// Two typed messages. Registration order (ChatMessage then PlayerMove) fixes
	// their auto-assigned ids at ID_USER_PACKET_ENUM and ID_USER_PACKET_ENUM+1.
	struct ChatMessage
	{
		std::string text;
		int channel = 0;

		template <class Ar>
		void serialize(Ar &ar) { ar & text & channel; }

		bool operator==(const ChatMessage &o) const { return text == o.text && channel == o.channel; }
	};

	struct PlayerMove
	{
		float x = 0.f, y = 0.f;
		unsigned int tick = 0;

		template <class Ar>
		void serialize(Ar &ar) { ar & x & y & tick; }

		bool operator==(const PlayerMove &o) const { return x == o.x && y == o.y && tick == o.tick; }
	};

	// Synthesize a received Packet that borrows the bytes written into a BitStream,
	// so dispatch() can be exercised without a live connection.
	Packet MakePacket(BitStream &bs, const RakNetGUID &guid)
	{
		Packet p{};
		p.data = bs.GetData();
		p.length = bs.GetNumberOfBytesUsed();
		p.bitSize = bs.GetNumberOfBitsUsed();
		p.guid = guid;
		return p;
	}
}

// --- Auto-assigned id contract: registration order fixes the ids ---
TEST(Dispatcher, AutoAssignedIdsFollowRegistrationOrder)
{
	Dispatcher d;
	MessageID chatId = d.on<ChatMessage>([](const ChatMessage &, const Sender &) {});
	MessageID moveId = d.on<PlayerMove>([](const PlayerMove &, const Sender &) {});

	ASSERT_TRUE(chatId == (MessageID) ID_USER_PACKET_ENUM && moveId == (MessageID) (ID_USER_PACKET_ENUM + 1))
		<< "Auto-assigned ids did not follow registration order: chat=" << (unsigned) chatId << " move=" << (unsigned) moveId;
	ASSERT_TRUE(d.has<ChatMessage>() && d.has<PlayerMove>())
		<< "has<T>() false for a registered type";
	ASSERT_TRUE(d.id_of<ChatMessage>() == chatId && d.id_of<PlayerMove>() == moveId)
		<< "id_of<>() disagreed with assigned ids";
}

// --- Offline round-trip via encode()/dispatch(), no network ---
TEST(Dispatcher, OfflineEncodeDispatchRoundTrip)
{
	Dispatcher d;
	d.on<ChatMessage>([](const ChatMessage &, const Sender &) {});

	ChatMessage sent;
	sent.text = std::string("a\0b hidden null", 15); // binary-safe payload
	sent.channel = -42;

	ChatMessage got;
	bool fired = false;

	Dispatcher r;
	r.on<ChatMessage>([&](const ChatMessage &m, const Sender &from) {
		got = m;
		fired = true;
		(void) from;
	});

	BitStream bs;
	d.encode(bs, sent);

	RakNetGUID sender(0x1234567890abcdefULL);
	Packet packet = MakePacket(bs, sender);
	ASSERT_TRUE(r.dispatch(packet)) << "dispatch() returned false for a registered id";
	ASSERT_TRUE(fired && got == sent)
		<< "Offline typed round-trip mismatch: text='" << got.text << "' channel=" << got.channel;
}

// --- ID_TIMESTAMP-prefixed packet dispatches to the correct handler ---
TEST(Dispatcher, TimestampPrefixedPacketDispatchesToHandler)
{
	Dispatcher d;
	d.on<PlayerMove>([](const PlayerMove &, const Sender &) {});

	PlayerMove sent;
	sent.x = 3.5f; sent.y = -1.25f; sent.tick = 99;

	PlayerMove got;
	bool fired = false;
	Dispatcher r;
	// Same registration order -> same id as the encoder above.
	r.on<PlayerMove>([&](const PlayerMove &m, const Sender &) { got = m; fired = true; });

	// Wire: ID_TIMESTAMP, Time, then the encoded typed message (id + body).
	BitStream bs;
	bs.Write((MessageID) ID_TIMESTAMP);
	bs.Write(GetTime());
	d.encode(bs, sent);

	Packet packet = MakePacket(bs, UNASSIGNED_RAKNET_GUID);
	ASSERT_TRUE(r.dispatch(packet) && fired && got == sent)
		<< "ID_TIMESTAMP-prefixed dispatch failed (fired=" << (int) fired << ")";
}

// --- Unregistered id and truncated packets leave the switch path usable ---
TEST(Dispatcher, UnregisteredAndTruncatedPacketsAreNotHandled)
{
	Dispatcher d; // nothing registered
	BitStream bs;
	bs.Write((MessageID) (ID_USER_PACKET_ENUM + 50));
	Packet packet = MakePacket(bs, UNASSIGNED_RAKNET_GUID);
	ASSERT_FALSE(d.dispatch(packet)) << "dispatch() handled an unregistered id";

	// A lone ID_TIMESTAMP with no room for the Time prefix must not read OOB.
	BitStream truncated;
	truncated.Write((MessageID) ID_TIMESTAMP);
	Packet tpacket = MakePacket(truncated, UNASSIGNED_RAKNET_GUID);
	ASSERT_FALSE(d.dispatch(tpacket)) << "dispatch() handled a truncated timestamp packet";
}

// --- Explicit-id registration bypasses registration-order coupling ---
TEST(Dispatcher, ExplicitIdRegistrationBypassesOrderCoupling)
{
	Dispatcher a, b;
	const MessageID kExplicit = (MessageID) (ID_USER_PACKET_ENUM + 30);
	// a registers a decoy first, so its *auto* id would differ from b's.
	a.on<PlayerMove>([](const PlayerMove &, const Sender &) {});
	a.on<ChatMessage>(kExplicit, [](const ChatMessage &, const Sender &) {});

	bool fired = false;
	ChatMessage got;
	b.on<ChatMessage>(kExplicit, [&](const ChatMessage &m, const Sender &) { got = m; fired = true; });

	ChatMessage sent; sent.text = "explicit"; sent.channel = 5;
	BitStream bs;
	a.encode(bs, sent); // uses kExplicit despite different registration order
	Packet packet = MakePacket(bs, UNASSIGNED_RAKNET_GUID);
	ASSERT_TRUE(b.dispatch(packet) && fired && got == sent) << "Explicit-id round-trip failed";
}

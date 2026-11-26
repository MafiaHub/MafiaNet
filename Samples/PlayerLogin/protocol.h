// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/message_identifiers.h"
#include <cstdint>

namespace Login
{
constexpr unsigned short port = 60000;
constexpr uint32_t worldRevision = 1;
enum : unsigned char
{
	worldLoadedId = ID_USER_PACKET_ENUM,
	welcomeId
};

// Application-defined reason codes sent with a refusal or a leave. Zero means none.
enum Reason : uint32_t
{
	unknownTicket = 1,
	loginUnavailable,
	wrongRevision,
	finished
};

struct WorldLoaded
{
	uint32_t revision;
	template <class Archive>
	void serialize(Archive &archive) { archive & revision; }
};

struct Welcome
{
	uint32_t playerNumber;
	template <class Archive>
	void serialize(Archive &archive) { archive & playerNumber; }
};
}

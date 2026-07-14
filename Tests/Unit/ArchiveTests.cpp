/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/BitStream.h"
#include "mafianet/Archive.h"

#include <string>

using namespace MafiaNet;

// Verifies the serialization adapter over BitStream (issue #19):
// - A user struct describes its wire format once via
//   `template <class Ar> void serialize(Ar& ar) { ar & a & b; }` and
//   round-trips through a BitStream: writing with WriteArchive then reading
//   with ReadArchive yields an equal value.
// - std::string members round-trip (length-prefixed, not raw object copy).
// - Nested serializable types recurse through the archive rather than being
//   raw-copied by the BitStream catch-all Write().
// - A plain type that already has a BitStream specialization (e.g. an int, or
//   a type with its own operator<< / operator>>) still works through the
//   archive without a serialize() method.

namespace
{
	// A flat user type that describes its wire format once. The same serialize()
	// drives both the write and the read direction.
	struct ChatMessage
	{
		std::string text;
		int channel = 0;

		template <class Ar>
		void serialize(Ar &ar) { ar & text & channel; }

		bool operator==(const ChatMessage &o) const
		{
			return text == o.text && channel == o.channel;
		}
	};

	// A nested type: one of its members (lastMessage) is itself a serialize()-able
	// type. This must recurse through the archive, NOT fall through to the
	// BitStream catch-all Write() which would raw-copy the std::string inside.
	struct PlayerState
	{
		std::string name;
		unsigned int health = 0;
		ChatMessage lastMessage;

		template <class Ar>
		void serialize(Ar &ar) { ar & name & health & lastMessage; }

		bool operator==(const PlayerState &o) const
		{
			return name == o.name && health == o.health && lastMessage == o.lastMessage;
		}
	};

	// A type with no serialize() but with its own BitStream operator<< / operator>>.
	// The archive must fall through to those, not require a serialize() method.
	struct Color
	{
		unsigned char r = 0, g = 0, b = 0;
		bool operator==(const Color &o) const { return r == o.r && g == o.g && b == o.b; }
	};

	// Round-trip a serialize()-able value through a fresh pair of BitStreams.
	template <class T>
	void RoundTrip(const T &in, T &out)
	{
		BitStream bsOut;
		WriteArchive w(bsOut);
		w & const_cast<T &>(in);

		BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
		ReadArchive r(bsIn);
		r & out;
	}
}

// operator<< / operator>> for Color live in the MafiaNet namespace so the
// archive's `bs << v` / `bs >> v` find them via ADL alongside the BitStream
// catch-all template.
namespace MafiaNet
{
	static BitStream &operator<<(BitStream &out, const Color &c)
	{
		out.Write(c.r);
		out.Write(c.g);
		out.Write(c.b);
		return out;
	}
	static BitStream &operator>>(BitStream &in, Color &c)
	{
		in.Read(c.r);
		in.Read(c.g);
		in.Read(c.b);
		return in;
	}
}

// A flat struct with serialize() round-trips (write then read == original).
TEST(Archive, FlatSerializeStructRoundTrips)
{
	ChatMessage original;
	original.text = "hello, world";
	original.channel = 7;

	ChatMessage restored;
	RoundTrip(original, restored);
	EXPECT_TRUE(restored == original)
		<< "Flat serialize() struct did not round-trip: text='" << restored.text
		<< "' channel=" << restored.channel;
}

// std::string with an embedded null must survive (binary-safe, length-prefixed).
TEST(Archive, EmbeddedNullStringRoundTrips)
{
	ChatMessage original;
	original.text = std::string("a\0b\0c", 5);
	original.channel = -3;

	ChatMessage restored;
	RoundTrip(original, restored);
	EXPECT_TRUE(restored == original)
		<< "std::string with embedded null did not round-trip (len "
		<< (unsigned) restored.text.size() << ")";
}

// A nested serialize()-able type recurses through the archive.
TEST(Archive, NestedSerializeTypeRecurses)
{
	PlayerState original;
	original.name = "Tommy";
	original.health = 100;
	original.lastMessage.text = "on my way";
	original.lastMessage.channel = 2;

	PlayerState restored;
	RoundTrip(original, restored);
	EXPECT_TRUE(restored == original)
		<< "Nested serialize() type did not round-trip: name='" << restored.name
		<< "' health=" << restored.health
		<< " inner='" << restored.lastMessage.text << "'/" << restored.lastMessage.channel;
}

// A type without serialize() but with its own operator<< / operator>>
// still works through the archive (falls through to the BitStream ops).
TEST(Archive, OperatorStreamFallthroughRoundTrips)
{
	Color original{ 10, 20, 30 };
	Color restored;

	BitStream bsOut;
	WriteArchive w(bsOut);
	w & original;

	BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
	ReadArchive r(bsIn);
	r & restored;

	EXPECT_TRUE(restored == original)
		<< "operator<< / operator>> fallthrough did not round-trip: "
		<< (unsigned) restored.r << "," << (unsigned) restored.g << "," << (unsigned) restored.b;
}

// A bare primitive also works through the archive (no serialize needed).
TEST(Archive, BarePrimitiveRoundTrips)
{
	int original = 0x1234abcd;
	int restored = 0;

	BitStream bsOut;
	WriteArchive w(bsOut);
	w & original;

	BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
	ReadArchive r(bsIn);
	r & restored;

	EXPECT_EQ(restored, original) << "Bare primitive did not round-trip through the archive";
}

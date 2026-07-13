/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "ArchiveTest.h"

#include <string>

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
	bool RoundTrip(const T &in, T &out)
	{
		BitStream bsOut;
		WriteArchive w(bsOut);
		w & const_cast<T &>(in);

		BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
		ReadArchive r(bsIn);
		r & out;
		return true;
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

int ArchiveTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

	// 1. A flat struct with serialize() round-trips (write then read == original).
	{
		ChatMessage original;
		original.text = "hello, world";
		original.channel = 7;

		ChatMessage restored;
		RoundTrip(original, restored);
		if (!(restored == original))
		{
			if (isVerbose)
				printf("flat serialize() round-trip mismatch: text='%s' channel=%d\n",
				       restored.text.c_str(), restored.channel);
			return 1;
		}
	}

	// 2. std::string with an embedded null must survive (binary-safe, length-prefixed).
	{
		ChatMessage original;
		original.text = std::string("a\0b\0c", 5);
		original.channel = -3;

		ChatMessage restored;
		RoundTrip(original, restored);
		if (!(restored == original))
		{
			if (isVerbose)
				printf("embedded-null string did not round-trip (len %u)\n",
				       (unsigned) restored.text.size());
			return 2;
		}
	}

	// 3. A nested serialize()-able type recurses through the archive.
	{
		PlayerState original;
		original.name = "Tommy";
		original.health = 100;
		original.lastMessage.text = "on my way";
		original.lastMessage.channel = 2;

		PlayerState restored;
		RoundTrip(original, restored);
		if (!(restored == original))
		{
			if (isVerbose)
				printf("nested serialize() round-trip mismatch: name='%s' health=%u inner='%s'/%d\n",
				       restored.name.c_str(), restored.health,
				       restored.lastMessage.text.c_str(), restored.lastMessage.channel);
			return 3;
		}
	}

	// 4. A type without serialize() but with its own operator<< / operator>>
	//    still works through the archive (falls through to the BitStream ops).
	{
		Color original{ 10, 20, 30 };
		Color restored;

		BitStream bsOut;
		WriteArchive w(bsOut);
		w & original;

		BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
		ReadArchive r(bsIn);
		r & restored;

		if (!(restored == original))
		{
			if (isVerbose)
				printf("operator<</>> fallthrough failed: %u,%u,%u\n",
				       restored.r, restored.g, restored.b);
			return 4;
		}
	}

	// 5. A bare primitive also works through the archive (no serialize needed).
	{
		int original = 0x1234abcd;
		int restored = 0;

		BitStream bsOut;
		WriteArchive w(bsOut);
		w & original;

		BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
		ReadArchive r(bsIn);
		r & restored;

		if (restored != original)
		{
			if (isVerbose) printf("primitive round-trip mismatch: 0x%x\n", (unsigned) restored);
			return 5;
		}
	}

	printf("Archive serialize() round-trips correctly\n");
	return 0;
}

ArchiveTest::ArchiveTest(void)
{
}

ArchiveTest::~ArchiveTest(void)
{
}

RakString ArchiveTest::GetTestName(void)
{
	return "ArchiveTest";
}

RakString ArchiveTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "Flat serialize() struct did not round-trip";
	case 2:  return "std::string with embedded null did not round-trip";
	case 3:  return "Nested serialize() type did not round-trip";
	case 4:  return "operator<< / operator>> fallthrough did not round-trip";
	case 5:  return "Bare primitive did not round-trip through the archive";
	default: return "Undefined error";
	}
}

void ArchiveTest::DestroyPeers(void)
{
}

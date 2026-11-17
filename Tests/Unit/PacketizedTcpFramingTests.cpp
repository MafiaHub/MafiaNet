/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 *
 * PacketizedTCP frames each message with a 4-byte big-endian length prefix and reassembles the
 * TCP byte stream per connection. The prefix is peer input: these tests pin reassembly across
 * arbitrary read boundaries and the bound on the prefix (a prefix above the limit drops the
 * connection; nothing is allocated for it and nothing wraps).
 *
 * Hermetic: no sockets and no threads. The harness marks the interface started, registers one
 * connection and feeds "received" bytes through PushBackPacket, which is exactly where the TCP
 * update thread's packets surface in PacketizedTCP::Receive.
 */

#include <gtest/gtest.h>

#include "mafianet/message_identifiers.h"
#include "mafianet/packetized_tcp.h"
#include "mafianet/remote_procedure_call.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

static_assert(!std::is_copy_constructible_v<PacketizedTCP>, "PacketizedTCP owns connection streams and threads");
static_assert(!std::is_copy_assignable_v<PacketizedTCP>, "PacketizedTCP owns connection streams and threads");
static_assert(!std::is_copy_constructible_v<TCPInterface>, "TCPInterface owns sockets, threads and LockedQueues");
static_assert(!std::is_copy_assignable_v<TCPInterface>, "TCPInterface owns sockets, threads and LockedQueues");

namespace
{
using Bytes = std::vector<unsigned char>;

class FramingHarness : public PacketizedTCP
{
public:
	explicit FramingHarness(const SystemAddress &remote)
	    : peer(remote)
	{
		isStarted.Increment(); // Receive() returns nothing on a stopped interface
		AddToConnectionList(peer);
	}
	~FramingHarness() override
	{
		// Back to "never started" (unless a test called Stop()), so ~TCPInterface's Stop() has no
		// thread or socket to tear down.
		if (isStarted.GetValue() > 0)
			isStarted.Decrement();
	}

	// Bytes as the TCP thread would hand them over from one recv().
	void Deliver(const Bytes &bytes)
	{
		Packet *packet = AllocatePacket(static_cast<unsigned>(bytes.size()));
		if (!bytes.empty())
			std::memcpy(packet->data, bytes.data(), bytes.size());
		packet->systemAddress = peer;
		packet->deleteData = true; // came from the network
		PushBackPacket(packet, false);
	}

	// Every packet Receive() has ready now.
	std::vector<Bytes> DrainMessages()
	{
		std::vector<Bytes> out;
		while (Packet *packet = Receive())
		{
			EXPECT_EQ(packet->systemAddress, peer);
			out.emplace_back(packet->data, packet->data + packet->length);
			DeallocatePacket(packet);
		}
		return out;
	}

	bool IsTracked() const { return connections.count(peer) != 0; }

	SystemAddress peer;
};

SystemAddress Peer()
{
	SystemAddress address("127.0.0.1", 4242);
	address.systemIndex = 0;
	return address;
}

void AppendPrefix(Bytes &stream, uint32_t length)
{
	stream.push_back(static_cast<unsigned char>(length >> 24));
	stream.push_back(static_cast<unsigned char>(length >> 16));
	stream.push_back(static_cast<unsigned char>(length >> 8));
	stream.push_back(static_cast<unsigned char>(length));
}

Bytes Frame(const Bytes &payload)
{
	Bytes framed;
	AppendPrefix(framed, static_cast<uint32_t>(payload.size()));
	framed.insert(framed.end(), payload.begin(), payload.end());
	return framed;
}

Bytes Payload(size_t size, unsigned char seed)
{
	Bytes payload(size);
	for (size_t i = 0; i < size; ++i)
		payload[i] = static_cast<unsigned char>(seed + i * 7);
	return payload;
}

// Fixed-seed generator: the same inputs on every run and platform.
struct Lcg
{
	uint64_t state;
	uint32_t Next()
	{
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return static_cast<uint32_t>(state >> 33);
	}
	uint32_t Below(uint32_t bound) { return bound == 0 ? 0 : Next() % bound; }
};

// Reference model of the framing: the messages a correct parser returns for \a stream, and
// whether it must drop the connection (a prefix above \a limit, checked as soon as it is whole).
struct Expected
{
	std::vector<Bytes> messages;
	bool dropped = false;
};
Expected ReferenceParse(const Bytes &stream, uint32_t limit)
{
	Expected expected;
	size_t offset = 0;
	while (stream.size() - offset >= 4)
	{
		const uint32_t length = (uint32_t(stream[offset]) << 24) | (uint32_t(stream[offset + 1]) << 16) |
		                        (uint32_t(stream[offset + 2]) << 8) | uint32_t(stream[offset + 3]);
		if (length > limit)
		{
			expected.dropped = true;
			break;
		}
		if (stream.size() - offset - 4 < length)
			break;
		expected.messages.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(offset + 4),
		                               stream.begin() + static_cast<std::ptrdiff_t>(offset + 4 + length));
		offset += 4 + length;
	}
	return expected;
}
} // namespace

TEST(PacketizedTcpFraming, RpcDispatchesCompleteFramesOnlyOnEnabledEndpoints)
{
	FramingHarness plain(Peer());
	BitStream message;
	message.Write((MessageID) ID_RPC);
	message.Write((MessageID) 2); // Signal on the protocol 10 wire.
	message.WriteCompressed("test");
	const Bytes payload(message.GetData(), message.GetData() + message.GetNumberOfBytesUsed());
	const Bytes framed = Frame(payload);

	plain.Deliver(framed);
	EXPECT_EQ((std::vector<Bytes>{payload}), plain.DrainMessages());

	FramingHarness endpoint(Peer());
	unsigned calls = 0;
	auto handler = [](BitStream *, Packet *, void *context)
	{ ++*static_cast<unsigned *>(context); };
	endpoint.GetRPC().RegisterSlot("test", handler, &calls, 0);
	endpoint.Deliver(Bytes(framed.begin(), framed.end() - 1));
	EXPECT_TRUE(endpoint.DrainMessages().empty());
	EXPECT_EQ(0u, calls);
	endpoint.Deliver(Bytes{framed.back()});
	EXPECT_TRUE(endpoint.DrainMessages().empty());
	EXPECT_EQ(1u, calls);

	endpoint.GetRPC().RegisterFunction("local", handler, &calls);
	endpoint.GetRPC().CallLoopback("local", nullptr);
	EXPECT_TRUE(endpoint.DrainMessages().empty());
	EXPECT_EQ(2u, calls);
}

TEST(PacketizedTcpFraming, ReassemblesMessagesSplitAcrossReads)
{
	const std::vector<Bytes> messages = {Payload(1, 1), Payload(300, 2), Payload(5, 3), Payload(4096, 4)};
	Bytes stream;
	for (const Bytes &message : messages)
	{
		const Bytes framed = Frame(message);
		stream.insert(stream.end(), framed.begin(), framed.end());
	}

	// One byte per read: every prefix and payload is split at every position.
	{
		FramingHarness tcp(Peer());
		std::vector<Bytes> received;
		for (unsigned char byte : stream)
		{
			tcp.Deliver(Bytes{byte});
			for (Bytes &message : tcp.DrainMessages())
				received.push_back(std::move(message));
		}
		EXPECT_EQ(received, messages);
		EXPECT_TRUE(tcp.IsTracked());
	}

	// The whole stream in one read: several messages come out of one recv().
	{
		FramingHarness tcp(Peer());
		tcp.Deliver(stream);
		EXPECT_EQ(tcp.DrainMessages(), messages);
	}

	// Reads that end inside the second prefix, then inside the last payload.
	{
		FramingHarness tcp(Peer());
		const size_t cutInPrefix = 4 + 1 + 2;
		const size_t cutInPayload = stream.size() - 100;
		tcp.Deliver(Bytes(stream.begin(), stream.begin() + cutInPrefix));
		EXPECT_EQ(tcp.DrainMessages(), (std::vector<Bytes>{messages[0]}));
		tcp.Deliver(Bytes(stream.begin() + cutInPrefix, stream.begin() + cutInPayload));
		EXPECT_EQ(tcp.DrainMessages(), (std::vector<Bytes>{messages[1], messages[2]}));
		tcp.Deliver(Bytes(stream.begin() + cutInPayload, stream.end()));
		EXPECT_EQ(tcp.DrainMessages(), (std::vector<Bytes>{messages[3]}));
	}
}

TEST(PacketizedTcpFraming, EmptyMessageIsDelivered)
{
	FramingHarness tcp(Peer());
	Bytes stream = Frame(Bytes{});
	const Bytes next = Frame(Payload(3, 9));
	stream.insert(stream.end(), next.begin(), next.end());
	tcp.Deliver(stream);
	EXPECT_EQ(tcp.DrainMessages(), (std::vector<Bytes>{Bytes{}, Payload(3, 9)}));
}

TEST(PacketizedTcpFraming, RejectsLengthPrefixLargerThanLimit)
{
	FramingHarness tcp(Peer());
	tcp.SetMaxMessageLength(1000);
	EXPECT_EQ(tcp.GetMaxMessageLength(), 1000u);

	// Exactly at the limit is fine.
	tcp.Deliver(Frame(Payload(1000, 5)));
	EXPECT_EQ(tcp.DrainMessages(), (std::vector<Bytes>{Payload(1000, 5)}));
	EXPECT_EQ(tcp.HasLostConnection(), UNASSIGNED_SYSTEM_ADDRESS);

	// One byte over: rejected as soon as the prefix is whole, before any payload arrives.
	Bytes oversized;
	AppendPrefix(oversized, 1001);
	tcp.Deliver(Bytes(oversized.begin(), oversized.begin() + 3));
	EXPECT_TRUE(tcp.DrainMessages().empty());
	EXPECT_TRUE(tcp.IsTracked()); // three bytes are not a prefix yet
	tcp.Deliver(Bytes(oversized.begin() + 3, oversized.end()));
	EXPECT_TRUE(tcp.DrainMessages().empty());
	EXPECT_FALSE(tcp.IsTracked());
	EXPECT_EQ(tcp.HasLostConnection(), tcp.peer);
	EXPECT_EQ(tcp.HasLostConnection(), UNASSIGNED_SYSTEM_ADDRESS);

	// Whatever the dropped peer sends afterwards is discarded, not framed.
	tcp.Deliver(Frame(Payload(10, 6)));
	EXPECT_TRUE(tcp.DrainMessages().empty());
}

TEST(PacketizedTcpFraming, DefaultLimitRejectsAFourGigabytePrefix)
{
	FramingHarness tcp(Peer());
	EXPECT_EQ(tcp.GetMaxMessageLength(), PacketizedTCP::DEFAULT_MAX_MESSAGE_LENGTH);

	// A message before the bad prefix in the same read is still delivered.
	Bytes stream = Frame(Payload(8, 1));
	AppendPrefix(stream, 0xFFFFFFFFu); // would wrap a 32-bit "length + 4"
	stream.push_back(0x42);
	tcp.Deliver(stream);
	EXPECT_EQ(tcp.DrainMessages(), (std::vector<Bytes>{Payload(8, 1)}));
	EXPECT_FALSE(tcp.IsTracked());
	EXPECT_EQ(tcp.HasLostConnection(), tcp.peer);
}

TEST(PacketizedTcpFraming, LargeMessageReportsDownloadProgress)
{
	const Bytes payload = Payload(200000, 11);
	const Bytes framed = Frame(payload);
	FramingHarness tcp(Peer());

	std::vector<Bytes> received;
	for (size_t offset = 0; offset < framed.size(); offset += 10000)
	{
		const size_t end = std::min(framed.size(), offset + 10000);
		tcp.Deliver(Bytes(framed.begin() + static_cast<std::ptrdiff_t>(offset), framed.begin() + static_cast<std::ptrdiff_t>(end)));
		for (Bytes &message : tcp.DrainMessages())
			received.push_back(std::move(message));
	}

	// The buffer crosses 64 KiB, 128 KiB and 192 KiB before the message is whole.
	ASSERT_EQ(received.size(), 4u);
	for (size_t i = 0; i < 3; ++i)
	{
		const Bytes &progress = received[i];
		ASSERT_EQ(progress.size(), 1 + 3 * sizeof(unsigned int) + 65536);
		EXPECT_EQ(progress[0], ID_DOWNLOAD_PROGRESS);
		unsigned int partIndex, totalParts, chunkSize;
		std::memcpy(&partIndex, progress.data() + 1, sizeof(partIndex));
		std::memcpy(&totalParts, progress.data() + 1 + sizeof(unsigned int), sizeof(totalParts));
		std::memcpy(&chunkSize, progress.data() + 1 + 2 * sizeof(unsigned int), sizeof(chunkSize));
		EXPECT_EQ(partIndex, i + 1);
		EXPECT_EQ(totalParts, 200000u / 65536u);
		EXPECT_EQ(chunkSize, 65536u);
		EXPECT_TRUE(std::equal(payload.begin(), payload.begin() + 65536, progress.begin() + 1 + 3 * sizeof(unsigned int)));
	}
	EXPECT_EQ(received[3], payload);
}

TEST(PacketizedTcpFraming, RandomAndTruncatedPrefixesMatchTheReferenceParser)
{
	// Differential fuzz against ReferenceParse: random streams mixing well-formed frames, prefixes
	// at and around the limit, huge and truncated prefixes and raw noise, cut into random reads.
	// Streams stay below 64 KiB, so no download-progress packets are interleaved.
	Lcg rng{0x5eed1234u};
	for (int iteration = 0; iteration < 400; ++iteration)
	{
		const uint32_t limit = rng.Below(2048);
		Bytes stream;
		const uint32_t pieces = 1 + rng.Below(12);
		for (uint32_t piece = 0; piece < pieces; ++piece)
		{
			switch (rng.Below(6))
			{
			case 0: // a valid frame
			case 1:
			{
				const Bytes framed = Frame(Payload(rng.Below(limit + 1), static_cast<unsigned char>(rng.Next())));
				stream.insert(stream.end(), framed.begin(), framed.end());
				break;
			}
			case 2: // a prefix just around the limit, with or without its payload
			{
				const uint32_t length = limit + rng.Below(3) - (limit > 0 ? 1 : 0);
				AppendPrefix(stream, length);
				if (rng.Below(2))
				{
					const Bytes body = Payload(length, 0);
					stream.insert(stream.end(), body.begin(), body.end());
				}
				break;
			}
			case 3: // a huge prefix (4 GiB, 2 GiB, 2^31 - 1, a random large one)
			{
				const uint32_t huge[] = {0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFCu, 0x10000000u + rng.Next()};
				AppendPrefix(stream, huge[rng.Below(5)]);
				break;
			}
			case 4: // a truncated prefix
			{
				Bytes partial;
				AppendPrefix(partial, rng.Next());
				stream.insert(stream.end(), partial.begin(), partial.begin() + rng.Below(4));
				break;
			}
			default: // noise
			{
				const uint32_t count = rng.Below(16);
				for (uint32_t i = 0; i < count; ++i)
					stream.push_back(static_cast<unsigned char>(rng.Next()));
				break;
			}
			}
		}

		const Expected expected = ReferenceParse(stream, limit);

		FramingHarness tcp(Peer());
		tcp.SetMaxMessageLength(limit);
		std::vector<Bytes> received;
		size_t offset = 0;
		while (offset < stream.size())
		{
			const size_t end = std::min(stream.size(), offset + 1 + rng.Below(64));
			tcp.Deliver(Bytes(stream.begin() + static_cast<std::ptrdiff_t>(offset), stream.begin() + static_cast<std::ptrdiff_t>(end)));
			for (Bytes &message : tcp.DrainMessages())
			{
				ASSERT_LE(message.size(), limit) << "iteration " << iteration;
				received.push_back(std::move(message));
			}
			offset = end;
		}

		ASSERT_EQ(received, expected.messages) << "iteration " << iteration;
		EXPECT_EQ(tcp.IsTracked(), !expected.dropped) << "iteration " << iteration;
		EXPECT_EQ(tcp.HasLostConnection(), expected.dropped ? tcp.peer : UNASSIGNED_SYSTEM_ADDRESS) << "iteration " << iteration;
	}
}

TEST(PacketizedTcpFraming, StopDiscardsMessagesNotYetReceived)
{
	// One read completes two messages; the first is taken, the second still waits in
	// waitingPackets when Stop() runs. Stop() used to free it and leave the freed pointer queued.
	FramingHarness tcp(Peer());
	Bytes stream = Frame(Payload(4, 1));
	const Bytes second = Frame(Payload(4, 2));
	stream.insert(stream.end(), second.begin(), second.end());
	tcp.Deliver(stream);
	Packet *first = tcp.Receive();
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(Bytes(first->data, first->data + first->length), Payload(4, 1));
	tcp.DeallocatePacket(first);

	tcp.Stop();
	Packet *after = tcp.Receive();
	EXPECT_EQ(after, nullptr); // (not deallocated if non-null: it would be the freed packet)
}

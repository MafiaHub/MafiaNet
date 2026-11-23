// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include <cstring>
#include <set>
#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"

using namespace MafiaNet;
namespace
{
constexpr MessageID payloadId = ID_USER_PACKET_ENUM + 19;
class WireObserver final : public PluginInterface2
{
public:
	bool UsesReliabilityLayer() const override { return true; }
	void OnInternalPacket(InternalPacket *p, unsigned, SystemAddress, TimeMS, int send) override
	{
		if (!send)
			return;
		const auto *bytes = p->splitPacketCount ? p->refCountedData->sharedDataBlock : p->data;
		if (bytes[0] == payloadId)
		{
			reliability = p->reliability;
			fragments += p->splitPacketCount != 0;
			++packets;
		}
	}
	unsigned packets = 0, fragments = 0;
	Reliability reliability = Reliability::Unreliable;
};

TEST(UnfragmentedSend, UnreliablePayloadThatFitsKeepsItsDeliveryMode)
{
	WireObserver wire;
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	auto *sender = net.AddPeer(1), *receiver = net.AddPeer(1, 1);
	ASSERT_TRUE(net.Connect(sender, receiver));
	sender->AttachPlugin(&wire);
	const auto address = net.AddressOf(receiver);
	// Twenty bytes after UDP/IP are ample for the datagram and unsplit unreliable headers.
	std::vector<char> bytes(sender->GetMTUSize(address) - UDP_HEADER_SIZE - 20, 'x');
	bytes.front() = payloadId;
	ASSERT_NE(sender->Send(bytes.data(), static_cast<int>(bytes.size()), Priority::High, Reliability::Unreliable, 0, address, false), 0u);
	ASSERT_TRUE(net.RunUntilReceived(receiver, payloadId, 2000));
	EXPECT_EQ(wire.packets, 1u);
	EXPECT_EQ(wire.fragments, 0u);
	EXPECT_EQ(wire.reliability, Reliability::Unreliable);
}

class DatagramBoundary : public ::testing::TestWithParam<EncryptionMode>
{
};

TEST_P(DatagramBoundary, ChecksTheLastBitForEveryUnreliableMode)
{
	WireObserver wire;
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(GetParam());
	auto *sender = net.AddPeer(1), *receiver = net.AddPeer(1, 1);
	ASSERT_TRUE(net.Connect(sender, receiver));
	sender->AttachPlugin(&wire);
	const auto guid = receiver->GetMyGUID();
	for (const auto mode : {Reliability::Unreliable, Reliability::UnreliableSequenced, Reliability::UnreliableWithAckReceipt})
	{
		SCOPED_TRACE(static_cast<unsigned>(mode));
		const auto limit = sender->GetMaxUnfragmentedPayload(guid, mode);
		ASSERT_GT(limit, 0u);
		EXPECT_EQ(limit, sender->GetMaxUnfragmentedPayload(net.AddressOf(receiver), mode));
		std::vector<unsigned char> bytes(limit + 1, 'a');
		bytes.front() = payloadId;
		BitStream message(bytes.data(), limit + 1, false);
		message.SetWriteOffset(limit * 8 - 1);
		auto result = sender->SendDatagram(message, Priority::High, mode, 0, guid);
		ASSERT_EQ(result.status, DatagramSendStatus::Queued);
		ASSERT_NE(result.receipt, 0u);
		ASSERT_TRUE(net.RunUntilReceived(receiver, payloadId, 2000));
		net.Inbox(receiver).clear();
		const auto before = wire.packets;
		result = sender->SendDatagram(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(limit), Priority::High, mode, 0, guid);
		ASSERT_EQ(result.status, DatagramSendStatus::Queued);
		ASSERT_TRUE(net.RunUntilReceived(receiver, payloadId, 2000));
		EXPECT_EQ(wire.packets, before + 1);
		EXPECT_EQ(wire.fragments, 0u);
		EXPECT_EQ(wire.reliability, mode);
		net.Inbox(receiver).clear();
		message.SetWriteOffset(limit * 8 + 1);
		const auto rejected = sender->SendDatagram(message, Priority::High, mode, 0, guid);
		EXPECT_EQ(rejected.status, DatagramSendStatus::TooLarge);
		EXPECT_EQ(rejected.receipt, 0u);
		EXPECT_EQ(sender->SendDatagram(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(limit + 1), Priority::High, mode, 0, guid).status, DatagramSendStatus::TooLarge);
		net.Step(100);
		EXPECT_FALSE(net.Find(receiver, payloadId));
		EXPECT_EQ(wire.packets, before + 1);
	}
}
TEST_P(DatagramBoundary, OrdinarySendSplitsOnlyBeyondTheReportedLimitForEachDeliveryMode)
{
	WireObserver wire;
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(GetParam());
	auto *sender = net.AddPeer(1), *receiver = net.AddPeer(1, 1);
	ASSERT_TRUE(net.Connect(sender, receiver));
	sender->AttachPlugin(&wire);
	const auto target = receiver->GetMyGUID();
	for (unsigned index = 0; index < NUMBER_OF_RELIABILITIES; ++index)
	{
		const auto mode = static_cast<Reliability>(index);
		SCOPED_TRACE(index);
		const auto limit = sender->GetMaxUnfragmentedPayload(target, mode);
		ASSERT_GT(limit, 0u);
		std::vector<char> bytes(limit + 1, 'x');
		bytes.front() = payloadId;
		for (unsigned extra : {0u, 1u})
		{
			wire.packets = wire.fragments = 0;
			net.Inbox(receiver).clear();
			ASSERT_NE(sender->Send(bytes.data(), static_cast<int>(limit + extra), Priority::High, mode, 0, target, false), 0u);
			ASSERT_TRUE(net.RunUntilReceived(receiver, payloadId, 2000));
			EXPECT_EQ(net.Find(receiver, payloadId)->data.size(), limit + extra);
			EXPECT_EQ(wire.fragments != 0, extra != 0);
			if (!extra)
			{
				EXPECT_EQ(wire.packets, 1u);
				EXPECT_EQ(wire.reliability, mode);
			}
		}
	}
}
INSTANTIATE_TEST_SUITE_P(EncryptedAndPlaintext, DatagramBoundary, testing::Values(EncryptionMode::Required, EncryptionMode::Plaintext));

TEST(UnfragmentedSend, UnknownTargetAndUnsupportedArgumentsDoNotQueue)
{
	SimulatedNetwork net;
	auto *sender = net.AddPeer(1), *receiver = net.AddPeer(1, 1);
	const auto address = net.AddressOf(receiver);
	const char data = static_cast<char>(payloadId);
	EXPECT_EQ(sender->GetMaxUnfragmentedPayload(address, Reliability::Unreliable), 0u);
	EXPECT_EQ(sender->SendDatagram(&data, 1, Priority::High, Reliability::Unreliable, 0, address).status, DatagramSendStatus::NotConnected);
	ASSERT_TRUE(net.Connect(sender, receiver));
	for (auto mode : {Reliability::Reliable, Reliability::ReliableOrdered, Reliability::ReliableSequenced})
	{
		EXPECT_GT(sender->GetMaxUnfragmentedPayload(address, mode), 0u);
		EXPECT_EQ(sender->SendDatagram(&data, 1, Priority::High, mode, 0, address).status, DatagramSendStatus::InvalidArgument);
	}
	EXPECT_EQ(sender->SendDatagram(nullptr, 1, Priority::High, Reliability::Unreliable, 0, address).status, DatagramSendStatus::InvalidArgument);
	EXPECT_EQ(sender->SendDatagram(&data, 1, Priority::High, Reliability::UnreliableSequenced, 32, address).status, DatagramSendStatus::InvalidArgument);
	EXPECT_EQ(sender->GetMaxUnfragmentedPayload(address, static_cast<Reliability>(-1)), 0u);
}

TEST(UnfragmentedSend, AlreadyQueuedReceiptIsSettledWhenTheDatagramStopsFitting)
{
	SimulatedNetwork net;
	auto *sender = net.AddPeer(1), *receiver = net.AddPeer(1, 1);
	ASSERT_TRUE(net.Connect(sender, receiver));
	const auto address = net.AddressOf(receiver);
	const auto mode = Reliability::UnreliableWithAckReceipt;
	std::vector<char> data(sender->GetMaxUnfragmentedPayload(address, mode), 'x');
	ASSERT_FALSE(data.empty());
	data.front() = payloadId;
	sender->SetPerConnectionOutgoingBandwidthLimit(8);
	for (unsigned i = 0; i < 8; ++i)
		ASSERT_EQ(sender->SendDatagram(data.data(), static_cast<int>(data.size()), Priority::High, mode, 0, address).status, DatagramSendStatus::Queued);
	net.Step(1);
	std::set<uint32_t> pending;
	SimulatedNetwork::SendBudget budget;
	ASSERT_TRUE(net.ReadSendBudget(sender, address, budget, [&](const InternalPacket &p)
	                               {
		if (p.data[0] == payloadId)
			pending.insert(p.sendReceiptSerial); }));
	ASSERT_FALSE(pending.empty());
	ASSERT_TRUE(net.StepDownMtu(sender, address));
	net.Step(1);
	for (const auto &msg : net.Inbox(sender))
	{
		if (msg.id != ID_SND_RECEIPT_LOSS)
			continue;
		ASSERT_EQ(msg.data.size(), 5u);
		uint32_t receipt;
		std::memcpy(&receipt, msg.data.data() + 1, sizeof(receipt));
		EXPECT_EQ(pending.erase(receipt), 1u);
	}
	EXPECT_TRUE(pending.empty());
}

TEST(UnfragmentedSend, PathShrinkingBetweenAdmissionAndTheNetworkThreadReportsLoss)
{
	WireObserver wire;
	SimulatedNetwork net;
	auto *sender = net.AddPeer(1), *receiver = net.AddPeer(1, 1);
	ASSERT_TRUE(net.Connect(sender, receiver));
	sender->AttachPlugin(&wire);
	const auto address = net.AddressOf(receiver);
	const auto mode = Reliability::UnreliableWithAckReceipt;
	const auto oldLimit = sender->GetMaxUnfragmentedPayload(address, mode);
	ASSERT_GT(oldLimit, 0u);
	std::vector<char> data(oldLimit, 'x');
	data[0] = payloadId;
	auto send = [&]
	{ return sender->SendDatagram(data.data(), static_cast<int>(data.size()), Priority::High, mode, 0, address); };
	const auto first = send();
	ASSERT_EQ(first.status, DatagramSendStatus::Queued);
	ASSERT_TRUE(net.StepDownMtu(sender, address));
	ASSERT_TRUE(net.RunUntilReceived(sender, ID_SND_RECEIPT_LOSS, 2000));
	const auto *loss = net.Find(sender, ID_SND_RECEIPT_LOSS);
	ASSERT_EQ(loss->data.size(), 5u);
	uint32_t receipt;
	std::memcpy(&receipt, loss->data.data() + 1, sizeof(receipt));
	EXPECT_EQ(receipt, first.receipt);
	EXPECT_EQ(wire.packets, 0u);
	const auto newLimit = sender->GetMaxUnfragmentedPayload(address, mode);
	ASSERT_LT(newLimit, oldLimit);
	EXPECT_EQ(send().status, DatagramSendStatus::TooLarge);
	data.resize(newLimit);
	ASSERT_EQ(send().status, DatagramSendStatus::Queued);
	ASSERT_TRUE(net.RunUntilReceived(receiver, payloadId, 2000));
	EXPECT_EQ(wire.packets, 1u);
	EXPECT_EQ(wire.fragments, 0u);
}
}

/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/replica_manager3.h"
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/peer_interface.h"

#include <vector>

using namespace MafiaNet;

namespace
{
	// One message as Connection_RM3::SendSerialize hands it to RakPeer, decoded the way
	// ReplicaManager3::OnSerialize reads it.
	struct SentMessage
	{
		bool written[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS] = {};
		std::vector<unsigned char> payload[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
		NetworkID networkId = UNASSIGNED_NETWORK_ID;
	};

	class SendTestConnection : public Connection_RM3
	{
	public:
		SendTestConnection() : Connection_RM3(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(2)) {}
		Replica3 *AllocReplica(MafiaNet::BitStream *, ReplicaManager3 *) { return 0; }
	};

	// Records every message SendSerialize builds: OnSerializeTransmission sees each one just
	// before it goes to RakPeer::Send, which an idle peer then drops.
	class SendTestReplica : public Replica3
	{
	public:
		void WriteAllocationID(Connection_RM3 *, MafiaNet::BitStream *) const {}
		RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) { return RM3CS_NEVER_CONSTRUCT; }
		bool QueryRemoteConstruction(Connection_RM3 *) { return false; }
		void SerializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) {}
		bool DeserializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
		void SerializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) {}
		bool DeserializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
		RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const { return RM3AOPC_DO_NOTHING; }
		void DeallocReplica(Connection_RM3 *) {}
		RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) { return RM3QSR_CALL_SERIALIZE; }
		RM3SerializationResult Serialize(SerializeParameters *) { return RM3SR_DO_NOT_SERIALIZE; }
		void Deserialize(DeserializeParameters *) {}

		void OnSerializeTransmission(MafiaNet::BitStream *bitStream, Connection_RM3 *, BitSize_t bitsPerChannel[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS], Time)
		{
			++transmissions;
			if (bitStream->GetNumberOfBitsUsed() == 0)
				return;
			MafiaNet::BitStream in(bitStream->GetData(), bitStream->GetNumberOfBytesUsed(), false);
			SentMessage message;
			MessageID id;
			WorldId world;
			in.Read(id);
			EXPECT_EQ(id, ID_REPLICA_MANAGER_SERIALIZE);
			in.Read(world);
			in.Read(message.networkId);
			for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
			{
				in.Read(message.written[z]);
				if (!message.written[z])
				{
					EXPECT_EQ(bitsPerChannel[z], 0u);
					continue;
				}
				BitSize_t bits;
				in.ReadCompressed(bits);
				EXPECT_EQ(bits, bitsPerChannel[z]);
				in.AlignReadToByteBoundary();
				message.payload[z].resize(BITS_TO_BYTES(bits));
				in.ReadBits(message.payload[z].data(), bits);
			}
			sent.push_back(message);
		}

		std::vector<SentMessage> sent;
		int transmissions = 0;
	};

	class ReplicaManager3SendSerialize : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			peer = RakPeerInterface::GetInstance();
			replica.SetNetworkIDManager(&ids);
			replica.SetNetworkID(7);
			for (int z = 0; z < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; z++)
			{
				selected[z] = false;
				parameters[z].priority = Priority::High;
				parameters[z].reliability = Reliability::ReliableOrdered;
				parameters[z].orderingChannel = 1;
				parameters[z].sendReceipt = 0;
			}
			// The shape the Framework uses: an unreliable pose channel ahead of reliable state.
			parameters[0].reliability = Reliability::Unreliable;
			parameters[0].orderingChannel = 0;
		}
		void TearDown() override
		{
			RakPeerInterface::DestroyInstance(peer);
		}

		SendSerializeIfChangedResult Send()
		{
			return connection.SendSerialize(&replica, selected, data, 0, parameters, peer, 0, 0);
		}

		RakPeerInterface *peer = nullptr;
		NetworkIDManager ids;
		SendTestReplica replica;
		SendTestConnection connection;
		bool selected[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
		MafiaNet::BitStream data[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
		PRO parameters[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS];
	};
}

TEST_F(ReplicaManager3SendSerialize, NothingSelectedSendsNothing)
{
	data[0].Write(static_cast<uint32_t>(1));
	EXPECT_EQ(Send(), SSICR_DID_NOT_SEND_DATA);
	EXPECT_TRUE(replica.sent.empty());
	EXPECT_EQ(replica.transmissions, 1) << "OnSerializeTransmission still reports the empty pass";
}

TEST_F(ReplicaManager3SendSerialize, StateOnlyChangeSendsOneMessage)
{
	// Channel 0 keeps stale bytes from an earlier broadcast but is not selected.
	data[0].Write(static_cast<uint32_t>(0xDEAD));
	data[1].Write(static_cast<uint32_t>(0xBEEF));
	selected[1] = true;

	EXPECT_EQ(Send(), SSICR_SENT_DATA);
	ASSERT_EQ(replica.sent.size(), 1u) << "No empty message for the unselected channel ahead of it";
	EXPECT_FALSE(replica.sent[0].written[0]);
	EXPECT_TRUE(replica.sent[0].written[1]);
	EXPECT_EQ(replica.sent[0].networkId, static_cast<NetworkID>(7));
}

TEST_F(ReplicaManager3SendSerialize, SelectedButEmptyChannelIsNotSent)
{
	selected[0] = true;
	data[1].Write(static_cast<uint32_t>(0xBEEF));
	selected[1] = true;

	EXPECT_EQ(Send(), SSICR_SENT_DATA);
	ASSERT_EQ(replica.sent.size(), 1u);
	EXPECT_TRUE(replica.sent[0].written[1]);
}

TEST_F(ReplicaManager3SendSerialize, DifferentParametersSendOneMessageEach)
{
	data[0].Write(static_cast<uint32_t>(0x11111111));
	data[1].Write(static_cast<uint32_t>(0x22222222));
	selected[0] = selected[1] = true;

	EXPECT_EQ(Send(), SSICR_SENT_DATA);
	ASSERT_EQ(replica.sent.size(), 2u);
	EXPECT_TRUE(replica.sent[0].written[0]);
	EXPECT_FALSE(replica.sent[0].written[1]);
	EXPECT_FALSE(replica.sent[1].written[0]);
	EXPECT_TRUE(replica.sent[1].written[1]);
}

TEST_F(ReplicaManager3SendSerialize, SharedParametersShareOneMessage)
{
	// Channels 1 and 3 share parameters even though channel 2 sits between them.
	parameters[2].orderingChannel = 5;
	data[1].Write(static_cast<uint32_t>(0x11111111));
	data[2].Write(static_cast<uint32_t>(0x22222222));
	data[3].Write(static_cast<uint32_t>(0x33333333));
	selected[1] = selected[2] = selected[3] = true;

	EXPECT_EQ(Send(), SSICR_SENT_DATA);
	ASSERT_EQ(replica.sent.size(), 2u);
	EXPECT_TRUE(replica.sent[0].written[1]);
	EXPECT_FALSE(replica.sent[0].written[2]);
	EXPECT_TRUE(replica.sent[0].written[3]);
	EXPECT_TRUE(replica.sent[1].written[2]);
}

TEST_F(ReplicaManager3SendSerialize, PayloadRoundTripsAndCanBeSentAgain)
{
	data[1].Write(static_cast<uint32_t>(0xCAFEF00D));
	selected[1] = true;

	ASSERT_EQ(Send(), SSICR_SENT_DATA);
	ASSERT_EQ(Send(), SSICR_SENT_DATA) << "The same bitstreams go to the next connection";
	ASSERT_EQ(replica.sent.size(), 2u);
	for (const SentMessage &message : replica.sent)
	{
		ASSERT_EQ(message.payload[1].size(), 4u);
		MafiaNet::BitStream in(const_cast<unsigned char *>(message.payload[1].data()), 4, false);
		uint32_t value = 0;
		in.Read(value);
		EXPECT_EQ(value, 0xCAFEF00Du);
	}
}

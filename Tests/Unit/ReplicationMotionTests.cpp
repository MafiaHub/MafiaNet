// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "SimulatedNetwork.h"
#include "mafianet/snapshot_replica.h"
#include <array>

using namespace MafiaNet;
namespace
{
struct Position
{
	int value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct Car : SnapshotReplica<Position, 4>
{
	unsigned encodes = 0;
	RM3SerializationResult Serialize(SerializeParameters *params) override
	{
		++encodes;
		return SnapshotReplica<Position, 4>::Serialize(params);
	}
	bool Drive(PeerGuid, const Position &step)
	{
		if (step.value < 0 || step.value > 10)
			return false;
		state.value += step.value;
		return true;
	}
};
constexpr ReplicaTypeId carType{22};
auto blend = [](Position a, Position b, double t)
{ return Position{static_cast<int>(a.value + (b.value - a.value) * t)}; };

class ReplicationMotion : public ::testing::Test
{
protected:
	std::array<Car, 3> cars;
	OwnerInputHistory<Position, 2> inputs;
	Position predicted;
	std::vector<OwnerInputStamp> processed;
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(2, 2), *driver = net.AddPeer(1), *spectator = net.AddPeer(1);
	ReplicationManager &host = server->GetReplication(), &view = driver->GetReplication();
	Time tick = 0;
	Car *Copy(RakPeer *peer, unsigned i = 0)
	{
		return static_cast<Car *>(peer->GetReplication().FindReplica(cars[i].GetNetworkID()));
	}
	void Step()
	{
		ASSERT_TRUE(host.Tick(++tick));
		net.Step(16);
	}
	void SetUp() override
	{
		SimulatedNetwork::LinkRule link;
		link.delayMs = 25;
		link.jitterMs = 5;
		net.SetDefaultLink(link);
		for (auto *peer : {server, driver, spectator})
		{
			auto &r = peer->GetReplication();
			ASSERT_TRUE(r.SetRole(peer == server ? ReplicationRole::Server : ReplicationRole::Client));
			ASSERT_TRUE(r.RegisterReplica<Car>(carType, 1));
			ASSERT_TRUE((r.RegisterOwnerUpdate<Car, Position>(1, &Car::Drive)));
			ASSERT_TRUE(r.SetSnapshotPolicy(carType, ChangeDetection::Explicit, SnapshotDelivery::Replaceable));
		}
		host.SetTickMode(ReplicationTickMode::Manual);
		host.SetSerializeInterval(0);
		for (auto &car : cars)
			ASSERT_TRUE(host.Reference(&car));
		ASSERT_TRUE(net.Connect(driver, server));
		ASSERT_TRUE(net.Connect(spectator, server));
		for (unsigned i = 0; i < 100 && (!Copy(driver, 2) || !Copy(spectator, 2)); ++i)
			Step();
		ASSERT_NE(Copy(driver, 2), nullptr);
		ASSERT_NE(Copy(spectator, 2), nullptr);
	}
};

TEST(ReplicationMotionSchema, HistoryEnabledAndPlainCodecsCannotShareATypeId)
{
	struct Plain : SnapshotReplica<Position>
	{
	};
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &remote = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(remote.SetRole(ReplicationRole::Client));
	ASSERT_TRUE(host.RegisterReplica<Car>(carType, 1));
	ASSERT_TRUE(remote.RegisterReplica<Plain>(carType, 1));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote.GetPeerStatus(ToPeerGuid(server->GetMyGUID())) == ReplicationPeerStatus::SchemaMismatch; }, 3000));
}

TEST_F(ReplicationMotion, BudgetAndCadencePreserveADeferredFinalChangeForEachObserver)
{
	const auto slow = ToPeerGuid(spectator->GetMyGUID());
	ASSERT_TRUE(host.SetObserverStateBudget(slow, 1));
	ASSERT_TRUE(host.SetUpdateInterval(slow, cars[0], 4));
	for (auto &car : cars)
	{
		car.state.value = 15;
		ASSERT_TRUE(host.MarkDirty(car));
	}
	Step();
	EXPECT_GT(host.GetStatistics(slow).deferredState, 0u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Copy(driver, 2)->state.value == 15; }, 3000));
	EXPECT_EQ(Copy(spectator, 2)->state.value, 0);
	// One indivisible snapshot can exceed the byte budget. Its debt delays
	// subsequent sends; all three must catch up after mutations have stopped.
	for (unsigned i = 0; i < 300 && Copy(spectator, 2)->state.value != 15; ++i)
		Step();
	for (unsigned i = 0; i < cars.size(); ++i)
		EXPECT_EQ(Copy(spectator, i)->state.value, 15);
	ASSERT_TRUE(host.SetObserverStateBudget(slow, 0));
	for (unsigned i = 0; i < 10; ++i)
	{
		cars[0].state.value++;
		host.MarkDirty(cars[0]);
		Step();
	}
	for (unsigned i = 0; i < 15; ++i)
		Step();
	EXPECT_EQ(Copy(spectator)->state.value, cars[0].state.value);
	EXPECT_EQ(Copy(driver)->state.value, cars[0].state.value);
}

TEST_F(ReplicationMotion, ObserversShareEncodingAndALaterTurnGetsTheFinalValue)
{
	auto observer = ToPeerGuid(spectator->GetMyGUID());
	ASSERT_TRUE(host.SetUpdateInterval(observer, cars[0], 4));
	cars[0].state.value = 10;
	ASSERT_TRUE(host.MarkDirty(cars[0]));
	auto before = cars[0].encodes;
	Step();
	EXPECT_EQ(cars[0].encodes, before + 1);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Copy(driver)->state.value == 10 && Copy(spectator)->state.value == 10; }, 3000));
	cars[0].state.value = 11;
	ASSERT_TRUE(host.MarkDirty(cars[0]));
	Step();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Copy(driver)->state.value == 11; }, 3000));
	EXPECT_EQ(Copy(spectator)->state.value, 10);
	Step();
	Step();
	net.Step(200);
	EXPECT_EQ(Copy(spectator)->state.value, 10);
	Step();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Copy(spectator)->state.value == 11; }, 3000));
}

TEST_F(ReplicationMotion, ReenteringTheGridStartsANewHistory)
{
	ASSERT_TRUE(host.GetInterestGrid().Configure({{0, 0}, {2, 1}, 1}));
	const auto observer = ToPeerGuid(spectator->GetMyGUID());
	Car extra;
	ASSERT_TRUE(host.GetInterestGrid().SetObserverCells(observer, {{0, 0}}));
	ASSERT_TRUE(host.Reference(&extra, SpatialScope{{0.5f, 0.5f}}));
	const auto id = extra.GetNetworkID();
	auto remote = [&]
	{ return static_cast<Car *>(spectator->GetReplication().FindReplica(id)); };
	Step();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote() != nullptr; }, 3000));
	ASSERT_TRUE(host.GetInterestGrid().SetObserverCells(observer, {{1, 0}}));
	Step();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote() == nullptr; }, 3000));
	extra.state.value = 125;
	ASSERT_TRUE(host.GetInterestGrid().SetObserverCells(observer, {{0, 0}}));
	Step();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote() != nullptr; }, 3000));
	EXPECT_EQ(remote()->SampleAt(0, blend).value, 125);
	EXPECT_EQ(remote()->LatestSnapshotTick(), tick);
	ASSERT_TRUE(host.Destroy(&extra));
}

TEST_F(ReplicationMotion, RenderSamplesUseSimulationTicksAndSnapAcrossTeleport)
{
	tick = 100;
	ASSERT_TRUE(host.Tick(tick));
	EXPECT_FALSE(host.Tick(100));
	for (int x : {20, 40})
	{
		cars[0].state.value = x;
		ASSERT_TRUE(host.MarkDirty(cars[0]));
		tick += 10;
		ASSERT_TRUE(host.Tick(tick));
		ASSERT_TRUE(net.RunUntil([&]
		                         { return Copy(spectator)->state.value == x; }, 3000));
	}
	EXPECT_EQ(Copy(spectator)->SampleAt(115, blend).value, 30);
	EXPECT_EQ(Copy(spectator)->SampleAt(150, blend).value, 40);
	cars[0].MarkTeleport();
	cars[0].state.value = 500;
	host.MarkDirty(cars[0]);
	Step();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Copy(spectator)->state.value == 500; }, 3000));
	EXPECT_EQ(Copy(spectator)->SampleAt(115, blend).value, 500);
}

TEST_F(ReplicationMotion, PredictionReplaysPendingInputAndHandoffIsolatesGrants)
{
	view.SetReplicaCallback([&](const ReplicaEvent &event)
	                        {
		if(event.kind==ReplicaEventKind::InputProcessed)
		{
			processed.push_back(event.input);
			predicted=inputs.Reconcile(event.input,Copy(driver)->state,[](Position &p,const Position &step){p.value+=step.value;});
		}
		if(event.kind==ReplicaEventKind::AuthorityLost)
			inputs.Clear(); });
	ASSERT_TRUE(host.SetAuthority(cars[0], ToPeerGuid(driver->GetMyGUID())));
	for (unsigned i = 0; i < 50 && !view.HasLocalAuthority(*Copy(driver)); ++i)
		Step();
	ASSERT_TRUE(view.HasLocalAuthority(*Copy(driver)));
	ASSERT_TRUE(inputs.Send(view, *Copy(driver), Position{4}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return cars[0].state.value == 4; }, 3000));
	Step(); // Publish correction 1 before submitting input 2.
	ASSERT_TRUE(inputs.Send(view, *Copy(driver), Position{7}));
	EXPECT_FALSE(inputs.Send(view, *Copy(driver), Position{1}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !processed.empty(); }, 3000));
	EXPECT_EQ(predicted.value, 11);
	EXPECT_EQ(inputs.Size(), 1u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return cars[0].state.value == 11; }, 3000));
	Step();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return inputs.Size() == 0 && Copy(spectator)->state.value == 11; }, 3000));
	ASSERT_TRUE(inputs.Send(view, *Copy(driver), Position{99}));
	for (unsigned i = 0; i < 50 && inputs.Size(); ++i)
		Step();
	EXPECT_EQ(inputs.Size(), 0u);
	EXPECT_EQ(predicted.value, 11);
	ASSERT_TRUE(inputs.Send(view, *Copy(driver), Position{9}));
	ASSERT_TRUE(host.SetAuthority(cars[0], ToPeerGuid(spectator->GetMyGUID())));
	for (unsigned i = 0; i < 50 && (!spectator->GetReplication().HasLocalAuthority(*Copy(spectator)) || inputs.Size()); ++i)
		Step();
	EXPECT_EQ(inputs.Size(), 0u);
	EXPECT_EQ(cars[0].state.value, 11);
	EXPECT_FALSE(inputs.Send(view, *Copy(driver), Position{1}));
	ASSERT_TRUE(inputs.Send(spectator->GetReplication(), *Copy(spectator), Position{2}));
	EXPECT_NE(spectator->GetReplication().GetLocalInputStamp(*Copy(spectator)).epoch, processed.front().epoch);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return cars[0].state.value == 13; }, 3000));
}
}

TEST(ReplicationMotionCodec, InvalidSampleLeavesTheReceiveChannelUnapplied)
{
	Car car;
	DeserializeParameters params{};
	params.bitstreamWrittenTo[0] = true;
	params.serializationBitstream[0].Write(uint32_t{1});
	car.Deserialize(&params);
	EXPECT_FALSE(params.bitstreamWrittenTo[0]);
	EXPECT_EQ(car.state.value, 0);
}

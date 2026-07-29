/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Wire-layout and parsing tests for the RakVoice relay frame.
//
// These pin the two things a relay host cannot afford to get wrong and that no
// higher layer can check for it: the byte offsets the writer and both readers
// share, and ReadRelayOrigin's handling of hostile input. ReadRelayOrigin is
// static precisely so it can be exercised without a live RakPeerInterface, which
// is what makes this suite possible at all.

#include <gtest/gtest.h>

#include "mafianet/MessageIdentifiers.h"
#include "mafianet/RakVoice.h"
#include "mafianet/types.h"

#include <cstring>
#include <vector>

using namespace MafiaNet;

namespace {
    // Builds a well-formed relay frame with the given origin, then lets a test
    // corrupt one field. Mirrors the writer in RakVoice::Update byte for byte —
    // if the two ever disagree, the offset assertions below fail first.
    std::vector<unsigned char> MakeRelayFrame(uint64_t origin, unsigned payloadBytes = 8,
                                              unsigned char version = RAKVOICE_RELAY_FORMAT_VERSION) {
        std::vector<unsigned char> f(RAKVOICE_RELAY_HEADER_SIZE + payloadBytes, 0);
        f[0] = ID_RAKVOICE_RELAY_DATA;
        f[RAKVOICE_RELAY_OFFSET_VERSION] = version;
        memcpy(f.data() + RAKVOICE_RELAY_OFFSET_ORIGIN, &origin, sizeof(uint64_t));

        const uint16_t channelId = 0;
        memcpy(f.data() + RAKVOICE_RELAY_OFFSET_CHANNEL_ID, &channelId, sizeof(uint16_t));

        const unsigned short sequence = 0;
        memcpy(f.data() + RAKVOICE_RELAY_OFFSET_SEQUENCE, &sequence, sizeof(unsigned short));
        return f;
    }

    // ReadRelayOrigin only reads data/length, so a stack Packet is sufficient.
    Packet MakePacket(std::vector<unsigned char> &bytes) {
        Packet p {};
        p.data = bytes.data();
        p.length = static_cast<unsigned int>(bytes.size());
        return p;
    }
} // namespace

// --- Layout -----------------------------------------------------------------

// The writer and both readers derive every offset from these. A change here is a
// wire-format break, so it must be a deliberate edit that also bumps the format
// version, not an accident.
TEST(RakVoiceRelayLayout, OffsetsAreContiguousAndHeaderIsFourteenBytes) {
    EXPECT_EQ(RAKVOICE_RELAY_OFFSET_VERSION, 1u);
    EXPECT_EQ(RAKVOICE_RELAY_OFFSET_ORIGIN, 2u);
    EXPECT_EQ(RAKVOICE_RELAY_OFFSET_CHANNEL_ID, 10u);
    EXPECT_EQ(RAKVOICE_RELAY_OFFSET_SEQUENCE, 12u);
    EXPECT_EQ(RAKVOICE_RELAY_HEADER_SIZE, 14u);
}

TEST(RakVoiceRelayLayout, FieldsDoNotOverlap) {
    EXPECT_EQ(RAKVOICE_RELAY_OFFSET_ORIGIN, RAKVOICE_RELAY_OFFSET_VERSION + sizeof(uint8_t));
    EXPECT_EQ(RAKVOICE_RELAY_OFFSET_CHANNEL_ID, RAKVOICE_RELAY_OFFSET_ORIGIN + sizeof(uint64_t));
    EXPECT_EQ(RAKVOICE_RELAY_OFFSET_SEQUENCE, RAKVOICE_RELAY_OFFSET_CHANNEL_ID + sizeof(uint16_t));
    EXPECT_EQ(RAKVOICE_RELAY_HEADER_SIZE, RAKVOICE_RELAY_OFFSET_SEQUENCE + sizeof(unsigned short));
}

// --- ReadRelayOrigin: happy path --------------------------------------------

TEST(RakVoiceRelayOrigin, RoundTripsTheOriginGuid) {
    auto bytes = MakeRelayFrame(0x0123456789ABCDEFull);
    Packet p = MakePacket(bytes);

    EXPECT_EQ(RakVoice::ReadRelayOrigin(&p).g, 0x0123456789ABCDEFull);
}

TEST(RakVoiceRelayOrigin, ReadsOriginIndependentlyOfPayloadLength) {
    for (unsigned payload : {0u, 1u, 8u, 512u, RAKVOICE_MAX_OPUS_PACKET_SIZE}) {
        auto bytes = MakeRelayFrame(0xFEEDFACECAFEBEEFull, payload);
        Packet p = MakePacket(bytes);
        EXPECT_EQ(RakVoice::ReadRelayOrigin(&p).g, 0xFEEDFACECAFEBEEFull) << "payload=" << payload;
    }
}

// --- ReadRelayOrigin: hostile input -----------------------------------------
//
// Every rejection must return UNASSIGNED_RAKNET_GUID, because that is the value
// callers treat as "drop this frame". Failing open here would let a malformed
// packet reach the decoder or, on a relay host, be forwarded to other players.

TEST(RakVoiceRelayOrigin, RejectsNullPacket) {
    EXPECT_EQ(RakVoice::ReadRelayOrigin(nullptr), UNASSIGNED_RAKNET_GUID);
}

TEST(RakVoiceRelayOrigin, RejectsPacketTruncatedInsideTheOriginField) {
    // One byte short of a complete origin: reading it would overrun the buffer.
    auto bytes = MakeRelayFrame(0x1122334455667788ull);
    bytes.resize(RAKVOICE_RELAY_OFFSET_ORIGIN + sizeof(uint64_t) - 1);
    Packet p = MakePacket(bytes);

    EXPECT_EQ(RakVoice::ReadRelayOrigin(&p), UNASSIGNED_RAKNET_GUID);
}

TEST(RakVoiceRelayOrigin, RejectsPacketTooShortToHoldTheVersionByte) {
    auto bytes = MakeRelayFrame(0x1122334455667788ull);
    bytes.resize(1); // id only
    Packet p = MakePacket(bytes);

    EXPECT_EQ(RakVoice::ReadRelayOrigin(&p), UNASSIGNED_RAKNET_GUID);
}

TEST(RakVoiceRelayOrigin, RejectsEmptyPacket) {
    std::vector<unsigned char> empty;
    Packet p {};
    p.data = nullptr;
    p.length = 0;

    EXPECT_EQ(RakVoice::ReadRelayOrigin(&p), UNASSIGNED_RAKNET_GUID);
}

// The version byte is the forward-compatibility escape hatch: a future format
// must be rejected by today's build rather than misparsed as version 1.
TEST(RakVoiceRelayOrigin, RejectsUnknownFormatVersion) {
    for (unsigned char v : {0, 2, 7, 255}) {
        auto bytes = MakeRelayFrame(0xAABBCCDDEEFF0011ull, 8, v);
        Packet p = MakePacket(bytes);
        EXPECT_EQ(RakVoice::ReadRelayOrigin(&p), UNASSIGNED_RAKNET_GUID) << "version=" << int(v);
    }
}

TEST(RakVoiceRelayOrigin, AcceptsExactlyTheCurrentFormatVersion) {
    auto bytes = MakeRelayFrame(0xAABBCCDDEEFF0011ull, 8, RAKVOICE_RELAY_FORMAT_VERSION);
    Packet p = MakePacket(bytes);

    EXPECT_NE(RakVoice::ReadRelayOrigin(&p), UNASSIGNED_RAKNET_GUID);
}

// An all-ones origin is indistinguishable from the sentinel, so it must be
// treated as a rejection rather than as a valid speaker.
TEST(RakVoiceRelayOrigin, TreatsSentinelOriginAsRejected) {
    auto bytes = MakeRelayFrame(static_cast<uint64_t>(-1));
    Packet p = MakePacket(bytes);

    EXPECT_EQ(RakVoice::ReadRelayOrigin(&p), UNASSIGNED_RAKNET_GUID);
}

// --- Impersonation ----------------------------------------------------------
//
// A relay host must compare the stamped origin against the sender's own GUID and
// drop any mismatch; otherwise a modified client stamps someone else's GUID and
// speaks as them. The comparison is the host's to make, but it is only sound if
// ReadRelayOrigin reports the stamped value faithfully rather than the sender's —
// which is what this pins.
TEST(RakVoiceRelayOrigin, ReportsTheStampedOriginNotTheSender) {
    const uint64_t sender = 0x1111111111111111ull;
    const uint64_t claimed = 0x2222222222222222ull;

    auto bytes = MakeRelayFrame(claimed);
    Packet p = MakePacket(bytes);
    p.guid = RakNetGUID(sender); // as the transport would set it

    const RakNetGUID parsed = RakVoice::ReadRelayOrigin(&p);
    EXPECT_EQ(parsed.g, claimed);
    EXPECT_NE(parsed, p.guid) << "a host comparing these must see a mismatch";
}

TEST(RakVoiceRelayOrigin, MatchesWhenAClientStampsItsOwnGuid) {
    const uint64_t self = 0x3333333333333333ull;

    auto bytes = MakeRelayFrame(self);
    Packet p = MakePacket(bytes);
    p.guid = RakNetGUID(self);

    EXPECT_EQ(RakVoice::ReadRelayOrigin(&p), p.guid);
}

// --- Frame sizing -----------------------------------------------------------

// A relay host bounds inbound frames with these two constants. If the header
// grew without the bound moving, oversized frames would be forwarded.
TEST(RakVoiceRelaySizing, MaxFrameIsHeaderPlusMaxOpusPacket) {
    EXPECT_EQ(RAKVOICE_RELAY_HEADER_SIZE + RAKVOICE_MAX_OPUS_PACKET_SIZE, 14u + 4000u);
}

TEST(RakVoiceRelaySizing, HeaderOnlyFrameCarriesNoPayload) {
    auto bytes = MakeRelayFrame(0x4444444444444444ull, 0);
    EXPECT_EQ(bytes.size(), RAKVOICE_RELAY_HEADER_SIZE);
}

// --- Concurrent relay speaker cap -------------------------------------------
//
// Each relay speaker costs a decoder plus two ring buffers, and origins are
// attacker-influenced, so channel creation must be bounded. GetOrCreateChannel
// is protected and needs only Init() (not a live peer), so a test-only subclass
// can drive it directly.

namespace {
    class RelayChannelProbe : public RakVoice {
      public:
        using RakVoice::GetOrCreateChannel;
        using RakVoice::OnOpenChannelReply;
        using RakVoice::OnReceive;
        unsigned OpenChannelCount() const {
            return voiceChannels.Size();
        }
    };
} // namespace

TEST(RakVoiceRelayCap, StopsAllocatingPastTheSpeakerCeiling) {
    RelayChannelProbe voice;
    voice.Init(48000, 960 * sizeof(short));

    unsigned created = 0;
    for (unsigned i = 0; i < RAKVOICE_MAX_RELAY_SPEAKERS + 16; i++) {
        // Distinct fabricated origins, as a spoofing sender would produce.
        if (voice.GetOrCreateChannel(RakNetGUID(0x1000ull + i)) != nullptr) {
            created++;
        }
    }

    EXPECT_EQ(voice.OpenChannelCount(), RAKVOICE_MAX_RELAY_SPEAKERS);
    EXPECT_EQ(created, RAKVOICE_MAX_RELAY_SPEAKERS);

    voice.Deinit();
}

TEST(RakVoiceRelayCap, KeepsServingSpeakersAlreadyOpenOnceFull) {
    RelayChannelProbe voice;
    voice.Init(48000, 960 * sizeof(short));

    const RakNetGUID first(0x2000ull);
    VoiceChannel *original = voice.GetOrCreateChannel(first);
    ASSERT_NE(original, nullptr);

    for (unsigned i = 1; i < RAKVOICE_MAX_RELAY_SPEAKERS + 8; i++) {
        voice.GetOrCreateChannel(RakNetGUID(0x2000ull + i));
    }

    // An established speaker must not be starved by a flood of fabricated ones.
    EXPECT_EQ(voice.GetOrCreateChannel(first), original);

    voice.Deinit();
}

TEST(RakVoiceRelayCap, RefusesChannelsBeforeInit) {
    RelayChannelProbe voice;
    EXPECT_EQ(voice.GetOrCreateChannel(RakNetGUID(0x3000ull)), nullptr);
}

// --- Hostile channel-open input ---------------------------------------------
//
// OnOpenChannelRequest/Reply parse a sample rate straight out of a remote
// packet. These paths predate relay mode and are reachable by any connected
// peer, so they are pinned here alongside it.

namespace {
    // A channel-open packet body: [id][int32 sample rate], as RequestVoiceChannel writes it.
    std::vector<unsigned char> MakeOpenChannelPacket(int32_t sampleRate, bool truncated = false) {
        std::vector<unsigned char> f(1 + sizeof(int32_t), 0);
        f[0] = ID_RAKVOICE_OPEN_CHANNEL_REPLY;
        // BitStream writes integers big-endian on little-endian hosts.
        const unsigned char *r = reinterpret_cast<const unsigned char *>(&sampleRate);
        for (size_t i = 0; i < sizeof(int32_t); i++) {
            f[1 + i] = r[sizeof(int32_t) - 1 - i];
        }
        if (truncated) {
            f.resize(2); // id + one stray byte: not enough for the rate
        }
        return f;
    }
} // namespace

// RakAssert is a real assert() in debug builds, so asserting on a remotely
// supplied sample rate would hand any peer a way to abort a debug server.
TEST(RakVoiceOpenChannel, RejectsOutOfRangeSampleRateWithoutAsserting) {
    RelayChannelProbe voice;
    voice.Init(48000, 960 * sizeof(short));

    for (int32_t bogus : {0, 1, -1, 44100, 96000, 2147483647}) {
        auto bytes = MakeOpenChannelPacket(bogus);
        Packet p = MakePacket(bytes);
        p.guid = RakNetGUID(0x9000ull + static_cast<uint64_t>(bogus & 0xFF));

        voice.OnOpenChannelReply(&p);
    }

    EXPECT_EQ(voice.OpenChannelCount(), 0u) << "no channel may be opened at an invalid rate";
    voice.Deinit();
}

// A packet too short to hold the rate leaves the read incomplete; the value must
// not be consumed.
TEST(RakVoiceOpenChannel, RejectsTruncatedSampleRate) {
    RelayChannelProbe voice;
    voice.Init(48000, 960 * sizeof(short));

    auto bytes = MakeOpenChannelPacket(48000, /*truncated=*/true);
    Packet p = MakePacket(bytes);
    p.guid = RakNetGUID(0x9100ull);

    voice.OnOpenChannelReply(&p);

    EXPECT_EQ(voice.OpenChannelCount(), 0u);
    voice.Deinit();
}

TEST(RakVoiceOpenChannel, AcceptsEachSupportedSampleRate) {
    for (int32_t rate : {8000, 16000, 24000, 48000}) {
        RelayChannelProbe voice;
        voice.Init(48000, 960 * sizeof(short));

        auto bytes = MakeOpenChannelPacket(rate);
        Packet p = MakePacket(bytes);
        p.guid = RakNetGUID(0x9200ull);

        voice.OnOpenChannelReply(&p);

        EXPECT_EQ(voice.OpenChannelCount(), 1u) << "rate=" << rate;
        voice.Deinit();
    }
}

// An uninitialised instance has bufferSizeBytes == 0, so opening a channel would
// allocate empty rings. OnOpenChannelRequest already guarded this; the reply path
// did not, and a decode-only relay channel skips the encoder create that would
// otherwise have failed first.
TEST(RakVoiceOpenChannel, RefusesUnsolicitedReplyBeforeInit) {
    RelayChannelProbe voice;

    auto bytes = MakeOpenChannelPacket(48000);
    Packet p = MakePacket(bytes);
    p.guid = RakNetGUID(0x9300ull);

    voice.OnOpenChannelReply(&p);

    EXPECT_EQ(voice.OpenChannelCount(), 0u);
}

// OnReceive dispatches on data[0] before any handler can validate.
TEST(RakVoiceOpenChannel, EmptyPacketDoesNotReadPastTheBuffer) {
    RelayChannelProbe voice;
    voice.Init(48000, 960 * sizeof(short));

    Packet p {};
    p.data = nullptr;
    p.length = 0;
    p.guid = RakNetGUID(0x9400ull);

    EXPECT_EQ(voice.OnReceive(&p), RR_CONTINUE_PROCESSING);
    voice.Deinit();
}

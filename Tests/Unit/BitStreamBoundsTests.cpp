/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Reads whose length comes from the data being read. Found by the
 *  bit_stream fuzz harness (Tests/Fuzz).
 */

#include <gtest/gtest.h>

#include <cstring>

#include "mafianet/bit_stream.h"

#ifdef _WIN32
#include <ws2tcpip.h> // inet_pton, htons
#else
#include <arpa/inet.h> // inet_pton, htons
#endif

using namespace MafiaNet;

namespace
{
// 0x20000001 << 3 wraps to 8 in 32 bits: the old bounds checks compared
// against 8 bits, passed, and copied 512 MB.
const unsigned int kWrappingLength = 0x20000001u;

void FillSixteenBytes(BitStream &bs)
{
	for (int i = 0; i < 16; ++i)
		bs.Write((unsigned char) i);
}
}

TEST(BitStreamBounds, AlignedReadLongerThanTheStreamWhoseBitCountWrapsIsRejected)
{
	BitStream bs;
	FillSixteenBytes(bs);
	char out[16] = {};
	EXPECT_FALSE(bs.Read(out, kWrappingLength));
	EXPECT_EQ(0u, bs.GetReadOffset());
}

TEST(BitStreamBounds, UnalignedReadLongerThanTheStreamWhoseBitCountWrapsIsRejected)
{
	BitStream bs;
	FillSixteenBytes(bs);
	bs.IgnoreBits(1);
	char out[16] = {};
	EXPECT_FALSE(bs.Read(out, kWrappingLength));
	EXPECT_EQ(1u, bs.GetReadOffset());
}

TEST(BitStreamBounds, ReadAlignedBytesWhoseBitCountWrapsIsRejected)
{
	BitStream bs;
	FillSixteenBytes(bs);
	unsigned char out[16] = {};
	EXPECT_FALSE(bs.ReadAlignedBytes(out, kWrappingLength));
}

TEST(BitStreamBounds, ReadExactlyTheRemainingBytesSucceeds)
{
	BitStream bs;
	FillSixteenBytes(bs);
	char out[16] = {};
	ASSERT_TRUE(bs.Read(out, 16));
	EXPECT_EQ(15, out[15]);
	EXPECT_FALSE(bs.Read(out, 1));
}

TEST(BitStreamBounds, SafeAllocReadThatRunsShortReturnsNoBuffer)
{
	// Claims 100 bytes, carries 4: the buffer used to be returned allocated
	// with nothing read into it, leaking for callers that free on success.
	BitStream bs;
	bs.WriteCompressed((unsigned int) 100);
	bs.Write((uint32_t) 0xDEADBEEF);

	std::unique_ptr<unsigned char[]> buffer;
	unsigned int length = 0;
	EXPECT_FALSE(bs.ReadAlignedBytesSafeAlloc(buffer, length, 4096));
	EXPECT_EQ(nullptr, buffer.get());
}

TEST(BitStreamBounds, ReadDeltaAtTheEndOfTheStreamFailsAndLeavesTheValue)
{
	// The "was written" flag was read into an uninitialized bool and then
	// branched on even when that read had failed.
	BitStream bs;
	uint32_t value = 1234;
	EXPECT_FALSE(bs.ReadDelta(value));
	EXPECT_EQ(1234u, value);
	EXPECT_FALSE(bs.ReadCompressedDelta(value));
	EXPECT_EQ(1234u, value);
}

namespace
{
SystemAddress ParseAddress(const char *text)
{
	SystemAddress address;
	EXPECT_TRUE(address.FromString(text));
	return address;
}

// The documented wire form of an IPv6 address: bitwise-inverted bytes.
void WriteV6(BitStream &bs, const unsigned char (&bytes)[16], unsigned char portHi, unsigned char portLo)
{
	bs.Write((unsigned char) 6);
	for (unsigned char b : bytes)
		bs.Write((unsigned char) ~b);
	bs.Write(portHi);
	bs.Write(portLo);
}
}

TEST(BitStreamSystemAddress, Ipv6EncodesNineteenBytes)
{
	const SystemAddress address = ParseAddress("2001:db8::1|258");
	BitStream bs;
	bs.Write(address);
	ASSERT_EQ(19u, bs.GetNumberOfBytesUsed());
	EXPECT_EQ(19, SystemAddress::size());

	const unsigned char *data = bs.GetData();
	EXPECT_EQ(6, data[0]);
	for (int i = 0; i < 16; ++i)
		EXPECT_EQ((unsigned char) ~address.address.addr6.sin6_addr.s6_addr[i], data[1 + i]) << i;
	EXPECT_EQ(0x01, data[17]);
	EXPECT_EQ(0x02, data[18]);
}

TEST(BitStreamSystemAddress, Ipv6RoundTripDropsScope)
{
	const SystemAddress address = ParseAddress("fe80::1%1|5");
	BitStream bs;
	bs.Write(address);

	SystemAddress out;
	ASSERT_TRUE(bs.Read(out));
	EXPECT_EQ(6, out.GetIPVersion());
	EXPECT_EQ(0, memcmp(address.address.addr6.sin6_addr.s6_addr, out.address.addr6.sin6_addr.s6_addr, 16));
	EXPECT_EQ(5, out.GetPort());
	EXPECT_EQ(0u, out.address.addr6.sin6_scope_id);
}

TEST(BitStreamSystemAddress, TruncatedIpv6ReadFails)
{
	BitStream bs;
	bs.Write((unsigned char) 6);
	for (int i = 0; i < 10; ++i)
		bs.Write((unsigned char) 0xAA);
	const BitSize_t before = bs.GetNumberOfUnreadBits();

	SystemAddress out;
	EXPECT_FALSE(bs.Read(out));
	// Only the version byte may have been consumed.
	EXPECT_EQ(before - 8, bs.GetNumberOfUnreadBits());
}

TEST(BitStreamSystemAddress, TruncatedIpv4ReadFails)
{
	BitStream bs;
	bs.Write((unsigned char) 4);
	for (int i = 0; i < 3; ++i)
		bs.Write((unsigned char) 0xAA);
	SystemAddress out;
	EXPECT_FALSE(bs.Read(out));
}

TEST(BitStreamSystemAddress, UnknownVersionReadFails)
{
	BitStream bs;
	bs.Write((unsigned char) 5);
	for (int i = 0; i < 18; ++i)
		bs.Write((unsigned char) 0);
	SystemAddress out;
	EXPECT_FALSE(bs.Read(out));
}

TEST(BitStreamSystemAddress, MappedIpv6ReadBecomesIpv4)
{
	const unsigned char mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 7};
	BitStream bs;
	WriteV6(bs, mapped, 0x01, 0x02);

	SystemAddress out;
	ASSERT_TRUE(bs.Read(out));
	EXPECT_EQ(4, out.GetIPVersion());
	char text[64];
	out.ToString(false, text, sizeof(text));
	EXPECT_STREQ("192.0.2.7", text);
	EXPECT_EQ(258, out.GetPort());
}

TEST(BitStreamSystemAddress, MappedAddressIsWrittenInTheIpv4Form)
{
	// Built directly: FromString canonicalizes a mapped literal to AF_INET.
	SystemAddress mapped;
	sockaddr_in6 raw;
	memset(&raw, 0, sizeof(raw));
	raw.sin6_family = AF_INET6;
	raw.sin6_port = htons(258);
	ASSERT_EQ(inet_pton(AF_INET6, "::ffff:192.0.2.7", &raw.sin6_addr), 1);
	memcpy(&mapped.address.addr6, &raw, sizeof(raw));
	ASSERT_TRUE(mapped.IsIPv4Mapped());
	BitStream bs;
	bs.Write(mapped);
	ASSERT_EQ(7u, bs.GetNumberOfBytesUsed());
	EXPECT_EQ(4, bs.GetData()[0]);

	SystemAddress out;
	ASSERT_TRUE(bs.Read(out));
	EXPECT_EQ(4, out.GetIPVersion());
	EXPECT_EQ(258, out.GetPort());
}

TEST(BitStreamSystemAddress, Ipv4LayoutUnchanged)
{
	const SystemAddress address = ParseAddress("1.2.3.4|258");
	BitStream bs;
	bs.Write(address);
	ASSERT_EQ(7u, bs.GetNumberOfBytesUsed());
	const unsigned char expected[7] = {4, 0xFE, 0xFD, 0xFC, 0xFB, 0x01, 0x02};
	EXPECT_EQ(0, memcmp(expected, bs.GetData(), 7));

	SystemAddress out;
	ASSERT_TRUE(bs.Read(out));
	EXPECT_TRUE(out == address);
}

/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Fuzzes the datagram parser of a connected ReliabilityLayer: datagram
 *  headers, ack/nak ranges, split-packet reassembly and ordering channels --
 *  everything a connected remote controls. Input is a sequence of datagrams,
 *  each prefixed by a 16-bit little-endian length. Time advances 10ms per
 *  datagram so resend, ack and split timeouts run too.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "mafianet/bit_stream.h"
#include "mafianet/ds_list.h"
#include "mafianet/get_time.h"
#include "mafianet/mtu_size.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/rand.h"
#include "mafianet/reliability_layer.h"
#include "mafianet/socket2.h"

using namespace MafiaNet;

namespace
{
// Acks and resends the layer emits are discarded.
struct NullSocket : public RakNetSocket2
{
	RNS2SendResult Send(RNS2_SendParameters *sendParameters, const char *, unsigned int) override
	{
		return sendParameters->length;
	}
};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static NullSocket socket;
	static SystemAddress remote("127.0.0.1", 40001);
	static RakNetRandom rnr;
	DataStructures::List<PluginInterface2 *> handlers;
	BitStream updateBitStream(MAXIMUM_MTU_SIZE);

	ReliabilityLayer layer;
	layer.Reset(true, MAXIMUM_MTU_SIZE, false);
	layer.SetTimeoutTime(1000000);
	CCTimeType now = GetTimeUS();

	size_t offset = 0;
	while (offset + 2 <= size)
	{
		size_t length = (size_t) data[offset] | ((size_t) data[offset + 1] << 8);
		offset += 2;
		if (length > size - offset)
			length = size - offset;
		if (length > MAXIMUM_MTU_SIZE)
			length = MAXIMUM_MTU_SIZE;

		// The layer's parser takes a mutable buffer; give it its own exactly
		// sized copy so a read or write past the datagram is caught by ASan.
		if (length == 0)
			continue;
		std::unique_ptr<char[]> datagram(new char[length]);
		std::memcpy(datagram.get(), data + offset, length);
		offset += length;

		layer.HandleSocketReceiveFromConnectedPlayer(datagram.get(), (unsigned int) length,
		                                             remote, handlers, MAXIMUM_MTU_SIZE, &socket, &rnr, now, updateBitStream);

		now += 10000;
		layer.Update(&socket, remote, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream);

		unsigned char *message;
		while (layer.Receive(&message) > 0)
			rakFree_Ex(message, _FILE_AND_LINE_);
	}
	return 0;
}

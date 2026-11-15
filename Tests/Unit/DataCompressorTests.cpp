// DataCompressor::DecompressAndAllocate parses data from remote peers: it must
// reject malformed input and never size an allocation from an unchecked field.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "mafianet/bit_stream.h"
#include "mafianet/data_compressor.h"

using namespace MafiaNet;

namespace
{
std::vector<unsigned char> MakeInput()
{
	std::vector<unsigned char> input(4096); // Compress() requires more than 2048 bytes
	for (size_t i = 0; i < input.size(); i++)
		input[i] = (unsigned char) ((i * 31) % 11);
	return input;
}
} // namespace

TEST(DataCompressor, CompressThenDecompressRoundTrips)
{
	std::vector<unsigned char> input = MakeInput();
	BitStream compressed;
	DataCompressor::Compress(input.data(), (unsigned) input.size(), &compressed);

	std::unique_ptr<unsigned char[]> output;
	ASSERT_EQ(input.size(), DataCompressor::DecompressAndAllocate(&compressed, output));
	ASSERT_NE(nullptr, output.get());
	EXPECT_EQ(0, std::memcmp(input.data(), output.get(), input.size()));
}

TEST(DataCompressor, DecompressRejectsTruncatedStream)
{
	std::vector<unsigned char> input = MakeInput();
	BitStream compressed;
	DataCompressor::Compress(input.data(), (unsigned) input.size(), &compressed);

	// Cut the payload short; the declared bit count then exceeds what is present.
	const unsigned int keep = compressed.GetNumberOfBytesUsed() / 2;
	BitStream truncated(compressed.GetData(), keep, true);

	std::unique_ptr<unsigned char[]> output(new unsigned char[1]);
	EXPECT_EQ(0u, DataCompressor::DecompressAndAllocate(&truncated, output));
	EXPECT_EQ(nullptr, output.get());

	BitStream empty;
	EXPECT_EQ(0u, DataCompressor::DecompressAndAllocate(&empty, output));
	EXPECT_EQ(nullptr, output.get());
}

TEST(DataCompressor, DecompressRejectsOversizedDeclaredLength)
{
	BitStream bs;
	bs.WriteCompressed((unsigned int) 0xFFFFFFF0u); // declared output size
	for (int i = 0; i < 256; i++)
		bs.WriteCompressed((unsigned int) 1);
	bs.AlignWriteToByteBoundary();
	bs.Write((unsigned int) 8); // eight payload bits ...
	bs.Write((unsigned char) 0xA5); // ... cannot expand to four billion bytes

	std::unique_ptr<unsigned char[]> output;
	unsigned result = 1;
	EXPECT_NO_THROW(result = DataCompressor::DecompressAndAllocate(&bs, output));
	EXPECT_EQ(0u, result);
	EXPECT_EQ(nullptr, output.get());
}

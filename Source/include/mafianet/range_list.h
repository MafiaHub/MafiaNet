/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file range_list.h
/// \internal
/// \brief Sorted list of inclusive index ranges; the ACK/NAK wire codec.
///

#ifndef __RANGE_LIST_H
#define __RANGE_LIST_H

#include "mafianet/assert.h"
#include "mafianet/bit_stream.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace MafiaNet
{
template <class range_type>
struct RangeNode
{
	RangeNode() {}
	RangeNode(range_type min, range_type max)
	    : minIndex(min), maxIndex(max) {}
	range_type minIndex;
	range_type maxIndex;
};

/// Stores indices as sorted, non-overlapping, inclusive ranges. Serialize and
/// Deserialize define the ACK/NAK wire format and must stay bit-exact.
template <class range_type>
class RangeList
{
public:
	/// Insert an index; adjacent ranges are merged. Duplicates are ignored.
	void Insert(range_type index)
	{
		// First range whose maxIndex >= index: either it contains index or it
		// is the first range after it.
		const size_t ins = FirstNotBelow(index);
		if (ins < ranges.size() && !(index < ranges[ins].minIndex))
		{
			return; // already covered by a range
		}

		// index > maxIndex of the entire list
		if (ins == ranges.size())
		{
			if (!ranges.empty() && index == ranges[ins - 1].maxIndex + (range_type) 1)
			{
				ranges[ins - 1].maxIndex++;
			}
			else
			{
				ranges.push_back(RangeNode<range_type>(index, index));
			}
			return;
		}

		if (index < ranges[ins].minIndex - (range_type) 1)
		{
			ranges.insert(ranges.begin() + (std::ptrdiff_t) ins, RangeNode<range_type>(index, index));
			return;
		}

		// index == minIndex - 1: extend downwards and join left
		ranges[ins].minIndex--;
		if (ins > 0 && ranges[ins - 1].maxIndex + (range_type) 1 == ranges[ins].minIndex)
		{
			ranges[ins - 1].maxIndex = ranges[ins].maxIndex;
			ranges.erase(ranges.begin() + (std::ptrdiff_t) ins);
		}
	}

	void Clear() { ranges.clear(); }

	bool IsWithinRange(range_type value) const
	{
		const size_t i = FirstNotBelow(value);
		return i < ranges.size() && !(value < ranges[i].minIndex);
	}

	unsigned Size() const { return (unsigned) ranges.size(); }

	unsigned RangeSum() const
	{
		unsigned sum = 0;
		for (const RangeNode<range_type> &r : ranges)
		{
			sum += r.maxIndex - r.minIndex + 1;
		}
		return sum;
	}

	/// Write as many ranges as fit in maxBits. With clearSerialized the written
	/// ranges (always a prefix) are removed. Returns the bits written.
	BitSize_t Serialize(BitStream *out, BitSize_t maxBits, bool clearSerialized)
	{
		RakAssert(ranges.size() < (unsigned short) -1);
		BitStream tempBS;
		BitSize_t bitsWritten = 0;
		unsigned short countWritten = 0;
		for (size_t i = 0; i < ranges.size(); i++)
		{
			if ((int) sizeof(unsigned short) * 8 + bitsWritten + (int) sizeof(range_type) * 8 * 2 + 1 > maxBits)
			{
				break;
			}
			unsigned char minEqualsMax = ranges[i].minIndex == ranges[i].maxIndex ? 1 : 0;
			tempBS.Write(minEqualsMax); // one byte instead of one bit, for speed
			tempBS.Write(ranges[i].minIndex);
			bitsWritten += sizeof(range_type) * 8 + 8;
			if (ranges[i].minIndex != ranges[i].maxIndex)
			{
				tempBS.Write(ranges[i].maxIndex);
				bitsWritten += sizeof(range_type) * 8;
			}
			countWritten++;
		}

		out->AlignWriteToByteBoundary();
		BitSize_t before = out->GetWriteOffset();
		out->Write(countWritten);
		bitsWritten += out->GetWriteOffset() - before;
		out->Write(&tempBS, tempBS.GetNumberOfBitsUsed());

		if (clearSerialized && countWritten)
		{
			ranges.erase(ranges.begin(), ranges.begin() + countWritten);
		}
		return bitsWritten;
	}

	/// Replace the contents with ranges read from the stream. Returns false on
	/// truncated or non-increasing input (the list may then be partial).
	bool Deserialize(BitStream *in)
	{
		ranges.clear();
		unsigned short count;
		in->AlignReadToByteBoundary();
		if (!in->Read(count))
		{
			return false;
		}
		// Grow as ranges are actually read; never trust the declared count.
		range_type min{}, max{};
		for (unsigned short i = 0; i < count; i++)
		{
			if (!DeserializeSingleRange(in, min, max))
			{
				return false;
			}
			if (!ranges.empty() && min <= ranges.back().maxIndex)
			{
				return false;
			}
			ranges.push_back(RangeNode<range_type>(min, max));
		}
		return true;
	}

	std::vector<RangeNode<range_type>> ranges;

private:
	size_t FirstNotBelow(const range_type &value) const
	{
		auto it = std::lower_bound(ranges.begin(), ranges.end(), value, [](const RangeNode<range_type> &n, const range_type &v)
		                           { return n.maxIndex < v; });
		return (size_t) (it - ranges.begin());
	}

	static bool DeserializeSingleRange(BitStream *in, range_type &min, range_type &max)
	{
		unsigned char maxEqualToMin;
		if (!in->Read(maxEqualToMin) || !in->Read(min))
		{
			return false;
		}
		if (maxEqualToMin == 0)
		{
			if (!in->Read(max) || max <= min)
			{
				return false;
			}
		}
		else
		{
			max = min;
		}
		return true;
	}
};

} // namespace MafiaNet

#endif

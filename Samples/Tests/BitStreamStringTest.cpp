/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#include "BitStreamStringTest.h"

#include <string>

/*
Description:
Verifies that BitStream serializes std::string by value (length-prefixed
characters), not by raw-copying the object representation. Covers:
  1. round-trip equality of a std::string,
  2. the on-wire size matches the RakString format (2-byte length + bytes),
  3. wire-compatibility in both directions with RakString,
  4. binary safety for strings containing embedded null bytes.

Success conditions: RunTest returns 0.
Failure conditions: a non-zero error code identifying which check failed.
*/
int BitStreamStringTest::RunTest(DataStructures::List<RakString> params,bool isVerbose,bool noPauses)
{
    (void) params; (void) noPauses;

    const std::string original = "Hello, MafiaNet!";

    // 1. Round-trip a std::string through a BitStream.
    BitStream bsOut;
    bsOut.Write(original);

    // 2. On-wire size must be the length-prefixed string form (unsigned short
    //    length + the characters), NOT sizeof(std::string). This is the check
    //    that fails when the catch-all template raw-copies the object.
    const BitSize_t expectedBytes = (BitSize_t)(sizeof(unsigned short) + original.length());
    if (bsOut.GetNumberOfBytesUsed() != expectedBytes)
    {
        if (isVerbose)
            printf("std::string wrote %u bytes, expected %u (raw object copy?)\n",
                   (unsigned) bsOut.GetNumberOfBytesUsed(), (unsigned) expectedBytes);
        return 1;
    }

    BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
    std::string roundTripped;
    if (!bsIn.Read(roundTripped))
    {
        if (isVerbose) printf("Failed to read std::string back\n");
        return 2;
    }
    if (roundTripped != original)
    {
        if (isVerbose)
            printf("std::string round-trip mismatch: got '%s'\n", roundTripped.c_str());
        return 3;
    }

    // 3a. std::string written -> RakString read.
    {
        BitStream bs;
        bs.Write(original);
        RakString rs;
        if (!bs.Read(rs) || strcmp(rs.C_String(), original.c_str()) != 0)
        {
            if (isVerbose) printf("std::string -> RakString interop failed: '%s'\n", rs.C_String());
            return 4;
        }
    }

    // 3b. RakString written -> std::string read.
    {
        RakString rs(original.c_str());
        BitStream bs;
        bs.Write(rs);
        std::string out;
        if (!bs.Read(out) || out != original)
        {
            if (isVerbose) printf("RakString -> std::string interop failed: '%s'\n", out.c_str());
            return 5;
        }
    }

    // 4. Binary safety: a string with an embedded null must round-trip in full.
    {
        std::string binary("a\0b\0c", 5);
        BitStream bs;
        bs.Write(binary);
        std::string out;
        if (!bs.Read(out) || out != binary)
        {
            if (isVerbose) printf("embedded-null round-trip failed (len %u)\n", (unsigned) out.size());
            return 6;
        }
    }

    printf("std::string serialization round-trips correctly\n");
    return 0;
}

BitStreamStringTest::BitStreamStringTest(void)
{
}

BitStreamStringTest::~BitStreamStringTest(void)
{
}

RakString BitStreamStringTest::GetTestName(void)
{
    return "BitStreamStringTest";
}

RakString BitStreamStringTest::ErrorCodeToString(int errorCode)
{
    switch (errorCode)
    {
    case 0:  return "No error";
    case 1:  return "std::string serialized as a raw object copy (wrong byte count)";
    case 2:  return "Failed to read std::string back from the bitstream";
    case 3:  return "std::string round-trip produced a different value";
    case 4:  return "std::string -> RakString wire interop failed";
    case 5:  return "RakString -> std::string wire interop failed";
    case 6:  return "std::string with embedded null did not round-trip";
    default: return "Undefined Error";
    }
}

void BitStreamStringTest::DestroyPeers(void)
{
}

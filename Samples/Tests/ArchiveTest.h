/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#pragma once


#include "TestInterface.h"

#include "mafianet/string.h"
#include "mafianet/DS_List.h"
#include "mafianet/BitStream.h"
#include "mafianet/Archive.h"

using namespace MafiaNet;

/// Verifies the serialization adapter over BitStream (issue #19):
/// - A user struct describes its wire format once via
///   `template <class Ar> void serialize(Ar& ar) { ar & a & b; }` and
///   round-trips through a BitStream: writing with WriteArchive then reading
///   with ReadArchive yields an equal value.
/// - std::string members round-trip (length-prefixed, not raw object copy).
/// - Nested serializable types recurse through the archive rather than being
///   raw-copied by the BitStream catch-all Write().
/// - A plain type that already has a BitStream specialization (e.g. an int, or
///   a type with its own operator<< / operator>>) still works through the
///   archive without a serialize() method.
class ArchiveTest : public TestInterface
{
public:
	ArchiveTest(void);
	~ArchiveTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
};

/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 *
 * Directory walks, progress notifications and file data use a local temporary tree.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/file_list.h"
#include "mafianet/file_operations.h"
#include "mafianet/super_fast_hash.h"
#include "mafianet/file_list_transfer.h"
#include "mafianet/file_list_node_context.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/string_compressor.h"
#include "mafianet/osx_adapter.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
struct TempTree
{
	std::filesystem::path root;
	TempTree()
	{
		// One directory per test: ctest may run these in parallel, and a
		// shared path lets one test delete another's tree mid-walk.
		root = std::filesystem::temp_directory_path() /
		       (std::string("mafianet_filelist_unit_") +
		        ::testing::UnitTest::GetInstance()->current_test_info()->name());
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root / "sub" / "deeper");
		Write("a.bin", 10);
		Write("sub/b.bin", 20);
		Write("sub/deeper/c.bin", 30);
	}
	~TempTree() { std::filesystem::remove_all(root); }
	void Write(const char *rel, size_t size)
	{
		std::ofstream out(root / rel, std::ios::binary);
		std::string bytes(size, 'x');
		out.write(bytes.data(), (std::streamsize) bytes.size());
	}
};

std::set<std::string> Names(const FileList &list)
{
	std::set<std::string> names;
	for (unsigned i = 0; i < list.fileList.size(); ++i)
		names.insert(list.fileList[i].filename.C_String());
	return names;
}
} // namespace

TEST(FileListDirectory, RecursiveWalkFindsEveryFileAndTerminates)
{
	TempTree tree;
	const std::string dir = tree.root.string() + "/";

	FileList list;
	list.AddFilesFromDirectory(dir.c_str(), nullptr, true, false, true, FileListNodeContext(0, 0, 0, 0));

	EXPECT_EQ(Names(list), (std::set<std::string>{"a.bin", "sub/b.bin", "sub/deeper/c.bin"}));
}

TEST(FileListDirectory, NonRecursiveWalkStaysInTheTopDirectory)
{
	TempTree tree;
	const std::string dir = tree.root.string() + "/";

	FileList list;
	list.AddFilesFromDirectory(dir.c_str(), nullptr, false, false, false, FileListNodeContext(0, 0, 0, 0));

	EXPECT_EQ(Names(list), (std::set<std::string>{"a.bin"}));
	ASSERT_EQ(list.fileList.size(), 1u);
	EXPECT_EQ(list.fileList[0].fileLengthBytes, 10u);
}

TEST(FileListDirectory, WriteDataReadsContentsIncludingAnEmptyFile)
{
	TempTree tree;
	tree.Write("empty.bin", 0);
	const std::string dir = tree.root.string() + "/";

	FileList list;
	list.AddFilesFromDirectory(dir.c_str(), nullptr, false, true, false, FileListNodeContext(0, 0, 0, 0));

	// The read checks must not mistake a zero-length file for a failed read.
	ASSERT_EQ(Names(list), (std::set<std::string>{"a.bin", "empty.bin"}));
	for (unsigned i = 0; i < list.fileList.size(); ++i)
	{
		const FileListNode &node = list.fileList[i];
		if (node.filename == "a.bin")
		{
			ASSERT_EQ(node.dataLengthBytes, 10u);
			EXPECT_EQ(std::string(node.data, node.dataLengthBytes), std::string(10, 'x'));
		}
		else
		{
			EXPECT_EQ(node.dataLengthBytes, 0u);
		}
	}
}

TEST(FileListDirectory, SubdirectoryKeepsItsPrefixAndStoresHashBeforeData)
{
	TempTree tree;
	FileList list;
	list.AddFilesFromDirectory(tree.root.string().c_str(), "sub/", true, true, false, FileListNodeContext(0, 0, 0, 0));

	ASSERT_EQ(list.fileList.size(), 1u);
	const auto &node = list.fileList.front();
	EXPECT_EQ(node.filename, "sub/b.bin");
	EXPECT_EQ(std::filesystem::path(node.fullPathToFile.C_String()), tree.root / "sub" / "b.bin");
	EXPECT_EQ(node.fileLengthBytes, 20u);
	ASSERT_EQ(node.dataLengthBytes, 24u);
	ASSERT_NE(node.data, nullptr);
	unsigned int hash = SuperFastHashFile((tree.root / "sub" / "b.bin").string().c_str());
	if (BitStream::DoEndianSwap())
		BitStream::ReverseBytesInPlace(reinterpret_cast<unsigned char *>(&hash), sizeof(hash));
	EXPECT_EQ(std::memcmp(node.data, &hash, sizeof(hash)), 0);
	EXPECT_EQ(std::string(node.data + sizeof(hash), node.fileLengthBytes), std::string(20, 'x'));
}

TEST(FileListDirectory, DirectoryExistsRecognizesEmptyDirectories)
{
	TempTree tree;
	const auto empty = tree.root / "empty";
	std::filesystem::create_directory(empty);
	EXPECT_TRUE(DirectoryExists(empty.string().c_str()));
	EXPECT_FALSE(DirectoryExists((tree.root / "a.bin").string().c_str()));
	EXPECT_FALSE(DirectoryExists((tree.root / "missing").string().c_str()));
}

TEST(FileListDirectory, PopulateDataFromDiskReadsEveryFile)
{
	TempTree tree;
	const std::string dir = tree.root.string() + "/";

	FileList list;
	list.AddFilesFromDirectory(dir.c_str(), nullptr, false, false, true, FileListNodeContext(0, 0, 0, 0));
	ASSERT_EQ(list.fileList.size(), 3u);

	list.PopulateDataFromDisk(dir.c_str(), true, false, false);

	for (unsigned i = 0; i < list.fileList.size(); ++i)
	{
		const FileListNode &node = list.fileList[i];
		ASSERT_NE(node.data, nullptr) << node.filename.C_String();
		EXPECT_EQ(node.dataLengthBytes, node.fileLengthBytes);
		EXPECT_EQ(std::string(node.data, node.dataLengthBytes), std::string(node.fileLengthBytes, 'x'));
	}
}

TEST(FileListDirectory, PopulateDataFromDiskWithoutDataOrHashTerminates)
{
	// With neither data nor hash requested, the loop used to skip i++ (and fclose) for every
	// file that exists, re-processing the first entry forever.
	TempTree tree;
	const std::string dir = tree.root.string() + "/";

	FileList list;
	list.AddFilesFromDirectory(dir.c_str(), nullptr, false, true, true, FileListNodeContext(0, 0, 0, 0));
	ASSERT_EQ(list.fileList.size(), 3u);

	list.PopulateDataFromDisk(dir.c_str(), false, false, false);

	ASSERT_EQ(list.fileList.size(), 3u);
	for (unsigned i = 0; i < list.fileList.size(); ++i)
	{
		EXPECT_EQ(list.fileList[i].data, nullptr);
		EXPECT_EQ(list.fileList[i].dataLengthBytes, 0u);
	}
}

namespace
{
struct RecordingProgress : FileListProgress
{
	std::vector<std::string> directories;
	std::vector<unsigned int> remaining;
	void OnDirectory(FileList *, char *dir, unsigned int directoriesRemaining) override
	{
		directories.push_back(dir);
		remaining.push_back(directoriesRemaining);
	}
};
} // namespace

TEST(FileListDirectory, GetCallbacksFillsAStdVectorWithoutDuplicates)
{
	FileList list;
	FileListProgress first, second;
	list.AddCallback(&first);
	list.AddCallback(&second);
	list.AddCallback(&first); // already registered
	list.AddCallback(nullptr);

	std::vector<FileListProgress *> callbacks{&second}; // replaced, not appended to
	list.GetCallbacks(callbacks);
	EXPECT_EQ(callbacks, (std::vector<FileListProgress *>{&first, &second}));

	list.RemoveCallback(&first);
	list.RemoveCallback(&first); // no longer there: no-op
	list.GetCallbacks(callbacks);
	EXPECT_EQ(callbacks, (std::vector<FileListProgress *>{&second}));

	list.ClearCallbacks();
	list.GetCallbacks(callbacks);
	EXPECT_TRUE(callbacks.empty());
}

TEST(FileListDirectory, FileListTransferGetCallbacksFillsAStdVector)
{
	FileListTransfer transfer;
	FileListProgress first, second;
	transfer.AddCallback(&first);
	transfer.AddCallback(&second);
	transfer.AddCallback(&first);

	std::vector<FileListProgress *> callbacks;
	transfer.GetCallbacks(callbacks);
	EXPECT_EQ(callbacks, (std::vector<FileListProgress *>{&first, &second}));

	transfer.RemoveCallback(&first);
	transfer.GetCallbacks(callbacks);
	EXPECT_EQ(callbacks, (std::vector<FileListProgress *>{&second}));
}

TEST(FileListDirectory, MissingDirectoryProducesNoDirectoryNotification)
{
	TempTree tree;
	FileList list;
	RecordingProgress progress;
	list.AddCallback(&progress);
	list.AddFilesFromDirectory(tree.root.string().c_str(), "missing", false, false, true, FileListNodeContext(0, 0, 0, 0));

	EXPECT_TRUE(progress.directories.empty());
	EXPECT_TRUE(list.fileList.empty());
}

TEST(FileListDirectory, WalkReportsEachDirectoryWithTheQueueLength)
{
	TempTree tree;
	std::filesystem::create_directories(tree.root / "other" / "child");
	const std::string dir = tree.root.string() + "/";

	FileList list;
	RecordingProgress progress;
	list.AddCallback(&progress);
	list.AddFilesFromDirectory(dir.c_str(), nullptr, false, false, true, FileListNodeContext(0, 0, 0, 0));

	// Siblings may be enumerated in either order. Both must be reported before their children.
	ASSERT_EQ(progress.directories.size(), 5u);
	EXPECT_EQ(progress.directories[0], dir + "*");
	EXPECT_EQ((std::set<std::string>{progress.directories[1], progress.directories[2]}),
	          (std::set<std::string>{dir + "sub/*", dir + "other/*"}));
	EXPECT_EQ((std::set<std::string>{progress.directories[3], progress.directories[4]}),
	          (std::set<std::string>{dir + "sub/deeper/*", dir + "other/child/*"}));
	EXPECT_EQ(progress.remaining, (std::vector<unsigned int>{0, 1, 1, 1, 0}));
}

TEST(FileListDirectory, AddFileKeepsInsertionOrderAndMovesAReplacedFileToTheEnd)
{
	FileList list;
	list.AddFile("one", "one", "1", 1, 1, FileListNodeContext(0, 0, 0, 0));
	list.AddFile("two", "two", "2", 1, 1, FileListNodeContext(0, 0, 0, 0));
	list.AddFile("three", "three", "3", 1, 1, FileListNodeContext(0, 0, 0, 0));
	list.AddFile("two", "two", "2", 1, 1, FileListNodeContext(0, 0, 0, 0)); // identical: kept in place
	ASSERT_EQ(list.fileList.size(), 3u);
	EXPECT_EQ(list.fileList[1].filename, "two");

	list.AddFile("one", "one", "X", 1, 1, FileListNodeContext(0, 0, 0, 0)); // different data: replaced
	ASSERT_EQ(list.fileList.size(), 3u);
	EXPECT_EQ(list.fileList[0].filename, "two");
	EXPECT_EQ(list.fileList[1].filename, "three");
	EXPECT_EQ(list.fileList[2].filename, "one");
	EXPECT_EQ(list.fileList[2].data[0], 'X');
}

TEST(FileListDirectory, SerializeRoundTripPreservesOrderAndContents)
{
	// Serialize encodes names through the shared StringCompressor, which RakPeer normally owns.
	StringCompressor::AddReference();
	struct Release
	{
		~Release() { StringCompressor::RemoveReference(); }
	} release;

	FileList source;
	source.AddFile("b.bin", "b.bin", "bb", 2, 2, FileListNodeContext(1, 2, 3, 0));
	source.AddFile("a.bin", "a.bin", nullptr, 0, 7, FileListNodeContext(4, 5, 6, 0));
	source.AddFile("c.bin", "c.bin", "ccc", 3, 9, FileListNodeContext(7, 8, 9, 0));

	BitStream bs;
	source.Serialize(&bs);

	// Deserialize reads the count as a 32-bit compressed integer, so this also pins that
	// Serialize still writes 32 bits rather than a size_t.
	FileList copy;
	ASSERT_TRUE(copy.Deserialize(&bs));
	ASSERT_EQ(copy.fileList.size(), 3u);
	const char *names[] = {"b.bin", "a.bin", "c.bin"};
	for (size_t i = 0; i < 3; ++i)
	{
		EXPECT_EQ(copy.fileList[i].filename, names[i]);
		EXPECT_EQ(copy.fileList[i].context.op, source.fileList[i].context.op);
		EXPECT_EQ(copy.fileList[i].fileLengthBytes, source.fileList[i].fileLengthBytes);
		// Not on the wire, so a received file is never a reference (and the flag is never left unset).
		EXPECT_FALSE(copy.fileList[i].isAReference);
		ASSERT_EQ(copy.fileList[i].dataLengthBytes, source.fileList[i].dataLengthBytes);
		if (copy.fileList[i].dataLengthBytes)
		{
			EXPECT_EQ(0, std::memcmp(copy.fileList[i].data, source.fileList[i].data, copy.fileList[i].dataLengthBytes));
		}
	}
}

#ifndef _WIN32
// The shim is only compiled on Linux and macOS; MSVC provides the real function.

TEST(StrncpySShim, CountEqualToBufferSizeCopiesAShortSource)
{
	char dest[8];
	std::memset(dest, 'Z', sizeof(dest));
	EXPECT_EQ(strncpy_s(dest, sizeof(dest), "abc", sizeof(dest)), 0);
	EXPECT_STREQ(dest, "abc");
}

TEST(StrncpySShim, SourceThatDoesNotFitIsRejectedWithoutOverflow)
{
	// A guard byte after the destination catches a write of the terminator at dest[size].
	char block[8 + 1];
	block[8] = 'G';
	EXPECT_EQ(strncpy_s(block, 8, "abcdefgh", 8), 34); // ERANGE: 8 chars need 9 bytes
	EXPECT_STREQ(block, "");
	EXPECT_EQ(block[8], 'G');
}

TEST(StrncpySShim, OnlyCountCharactersOfTheSourceAreRead)
{
	// The source is not terminated within count bytes; the shim must not read past count.
	const char source[4] = {'a', 'b', 'c', 'd'};
	char dest[8];
	EXPECT_EQ(strncpy_s(dest, sizeof(dest), source, 4), 0);
	EXPECT_STREQ(dest, "abcd");
}

TEST(StrncpySShim, TruncateCopiesWhatFits)
{
	char dest[4];
	EXPECT_EQ(strncpy_s(dest, sizeof(dest), "abcdefgh", _TRUNCATE), 80); // STRUNCATE
	EXPECT_STREQ(dest, "abc");
}
#endif

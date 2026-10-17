/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 *
 * FileList::AddFilesFromDirectory walks a directory tree through the _findfirst/_findnext
 * emulation. On Linux that walk never terminated: the emulation copied each entry name with a
 * count equal to the destination size, which the strncpy_s shim rejected after reading past the
 * end of dirent::d_name, so every name came back empty, the "." and ".." filter never matched,
 * and the walk recursed into "dir//" forever until the process ran out of memory. These tests
 * pin the shim's bounds and the walk's termination. Hermetic: a temp directory, no networking.
 */

#include <gtest/gtest.h>

#include "mafianet/file_list.h"
#include "mafianet/file_list_node_context.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

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
			out.write(bytes.data(), (std::streamsize)bytes.size());
		}
	};

	std::set<std::string> Names(const FileList &list)
	{
		std::set<std::string> names;
		for (unsigned i = 0; i < list.fileList.Size(); ++i)
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
	ASSERT_EQ(list.fileList.Size(), 1u);
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
	for (unsigned i = 0; i < list.fileList.Size(); ++i)
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

TEST(FileListDirectory, PopulateDataFromDiskReadsEveryFile)
{
	TempTree tree;
	const std::string dir = tree.root.string() + "/";

	FileList list;
	list.AddFilesFromDirectory(dir.c_str(), nullptr, false, false, true, FileListNodeContext(0, 0, 0, 0));
	ASSERT_EQ(list.fileList.Size(), 3u);

	list.PopulateDataFromDisk(dir.c_str(), true, false, false);

	for (unsigned i = 0; i < list.fileList.Size(); ++i)
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
	ASSERT_EQ(list.fileList.Size(), 3u);

	list.PopulateDataFromDisk(dir.c_str(), false, false, false);

	ASSERT_EQ(list.fileList.Size(), 3u);
	for (unsigned i = 0; i < list.fileList.Size(); ++i)
	{
		EXPECT_EQ(list.fileList[i].data, nullptr);
		EXPECT_EQ(list.fileList[i].dataLengthBytes, 0u);
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

/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 */

#include <gtest/gtest.h>

#include "mafianet/file_list.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <random>
#include <set>
#include <string>

using namespace MafiaNet;

namespace
{
struct PauseAfterOpen : FileListProgress
{
	std::promise<void> opened;
	std::promise<void> resume;

	void OnDirectory(FileList *, char *, unsigned int) override
	{
		opened.set_value();
		EXPECT_EQ(resume.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
	}
};
} // namespace

TEST(FileListDirectoryLive, ClosingTheFirstWalkLeavesTheSecondWalkUsable)
{
	const auto root = std::filesystem::temp_directory_path() / ("mafianet_overlapping_walks_" + std::to_string(std::random_device{}()));
	ASSERT_TRUE(std::filesystem::create_directory(root));
	struct Cleanup
	{
		std::filesystem::path path;
		~Cleanup() { std::filesystem::remove_all(path); }
	} cleanup{root};
	const std::set<std::string> expected{"one", "two", "three"};
	for (const auto &name : expected)
		ASSERT_TRUE(std::ofstream(root / name).good());

	auto walk = [&](PauseAfterOpen &pause)
	{
		FileList list;
		list.AddCallback(&pause);
		list.AddFilesFromDirectory(root.string().c_str(), nullptr, false, false, false, FileListNodeContext(0, 0, 0, 0));
		std::set<std::string> names;
		for (const auto &node : list.fileList)
			names.insert(node.filename.c_str());
		return names;
	};

	// Close the older search while the newer search still has entries to read.
	PauseAfterOpen first, second;
	auto firstWalk = std::async(std::launch::async, walk, std::ref(first));
	ASSERT_EQ(first.opened.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
	auto secondWalk = std::async(std::launch::async, walk, std::ref(second));
	ASSERT_EQ(second.opened.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
	first.resume.set_value();
	EXPECT_EQ(firstWalk.get(), expected);
	second.resume.set_value();
	EXPECT_EQ(secondWalk.get(), expected);
}

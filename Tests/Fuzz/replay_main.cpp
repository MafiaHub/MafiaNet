/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Runs a fuzz harness over files, without libFuzzer. Every harness is built
 *  this way alongside the test suites, so the checked-in corpus (seeds and
 *  minimized crash reproducers) is replayed on every compiler and platform,
 *  not just in the Clang fuzzing job.
 *
 *  Usage: <harness>_replay <file-or-directory>...
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace
{
	int RunFile(const std::filesystem::path &path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
			return 1;
		}
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		LLVMFuzzerTestOneInput(bytes.empty() ? nullptr : bytes.data(), bytes.size());
		return 0;
	}
}

int main(int argc, char **argv)
{
	int failures = 0;
	int ran = 0;
	for (int i = 1; i < argc; ++i)
	{
		const std::filesystem::path arg(argv[i]);
		if (std::filesystem::is_directory(arg))
		{
			for (const auto &entry : std::filesystem::directory_iterator(arg))
			{
				if (!entry.is_regular_file())
					continue;
				failures += RunFile(entry.path());
				++ran;
			}
		}
		else
		{
			failures += RunFile(arg);
			++ran;
		}
	}
	std::printf("replayed %d input(s)\n", ran);
	return failures == 0 ? 0 : 1;
}

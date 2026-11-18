/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/file_operations.h"
#if _RAKNET_SUPPORT_FileOperations == 1
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <string.h>
#include <string>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

static bool EnsureDirectories(const std::filesystem::path &directory)
{
	if (directory.empty())
		return true;

	std::error_code ec;
#ifdef _WIN32
	std::filesystem::create_directories(directory, ec);
	return !ec;
#else
	if (std::filesystem::is_directory(directory, ec))
		return true;

	// create_directories uses 0777. Apply the historical 0744 (subject to umask)
	// when creating each directory, without changing permissions on existing ones.
	auto prefix = directory.root_path();
	for (const auto &component : directory.relative_path())
	{
		prefix /= component;
		if (::mkdir(prefix.c_str(), 0744) != 0 && !std::filesystem::is_directory(prefix, ec))
			return false;
	}
	return true;
#endif
}

bool WriteFileWithDirectories(const char *path, char *data, unsigned dataLength)
{
	if (!path || !*path)
		return false;

	try
	{
		if (!data)
		{
#ifndef _WIN32
			// Directory-only calls historically accept either separator on POSIX.
			std::string directory(path);
			std::replace(directory.begin(), directory.end(), '\\', '/');
			return EnsureDirectories(directory);
#else
			return EnsureDirectories(path);
#endif
		}

		const std::filesystem::path file(path);
		if (!EnsureDirectories(file.parent_path()))
			return false;

		std::ofstream output(file, std::ios::binary);
		output.write(data, dataLength);
		output.close();
		return output.good();
	}
	catch (const std::filesystem::filesystem_error &)
	{
		// Converting the caller's narrow path to a native path can fail on Windows.
		return false;
	}
}
bool IsSlash(unsigned char c)
{
	return c == '/' || c == '\\';
}

void AddSlash(char *input)
{
	if (input == 0 || input[0] == 0)
		return;

	int lastCharIndex = (int) strlen(input) - 1;
	if (input[lastCharIndex] == '\\')
		input[lastCharIndex] = '/';
	else if (input[lastCharIndex] != '/')
	{
		input[lastCharIndex + 1] = '/';
		input[lastCharIndex + 2] = 0;
	}
}
bool DirectoryExists(const char *directory)
{
	std::error_code ec;
	return directory && std::filesystem::is_directory(*directory ? directory : ".", ec);
}
void QuoteIfSpaces(char *str)
{
	unsigned i;
	bool hasSpace = false;
	for (i = 0; str[i]; i++)
	{
		if (str[i] == ' ')
		{
			hasSpace = true;
			break;
		}
	}
	if (hasSpace)
	{
		int len = (int) strlen(str);
		memmove(str + 1, str, len);
		str[0] = '\"';
		str[len] = '\"';
		str[len + 1] = 0;
	}
}
unsigned int GetFileLength(const char *path)
{
	FILE *fp;
	if (fopen_s(&fp, path, "rb") != 0)
		return 0;
	fseek(fp, 0, SEEK_END);
	unsigned int fileLength = ftell(fp);
	fclose(fp);
	return fileLength;
}

#endif // _RAKNET_SUPPORT_FileOperations

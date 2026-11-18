/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/file_list.h"
#include <cstring>
#include <cstdlib>

#if _RAKNET_SUPPORT_FileOperations == 1

#include <stdio.h> // RAKNET_DEBUG_PRINTF
#include <vector>
#ifdef _WIN32
#include "mafianet/windows_includes.h"
#include <io.h>
#else
#include <unistd.h>
#endif

#include "mafianet/string_compressor.h"
#include "mafianet/bit_stream.h"
#include "mafianet/file_operations.h"
#include "mafianet/super_fast_hash.h"
#include "mafianet/assert.h"
#include "mafianet/linux_strings.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

#define MAX_FILENAME_LENGTH 512
static const unsigned HASH_LENGTH = 4;

using namespace MafiaNet;

//int RAK_DLL_EXPORT FileListNodeComp( char * const &key, const FileListNode &data )
//{
//	return strcmp(key, data.filename);
//}


STATIC_FACTORY_DEFINITIONS(FileListProgress, FileListProgress)
STATIC_FACTORY_DEFINITIONS(FLP_Printf, FLP_Printf)
STATIC_FACTORY_DEFINITIONS(FileList, FileList)

/// First callback called when FileList::AddFilesFromDirectory() starts
void FLP_Printf::OnAddFilesFromDirectoryStarted(FileList *fileList, char *dir)
{
	(void) fileList;
	RAKNET_DEBUG_PRINTF("Adding files from directory %s\n", dir);
}

/// Called for each directory, when that directory begins processing
void FLP_Printf::OnDirectory(FileList *fileList, char *dir, unsigned int directoriesRemaining)
{
	(void) fileList;
	RAKNET_DEBUG_PRINTF("Adding %s. %i remaining.\n", dir, directoriesRemaining);
}
void FLP_Printf::OnFilePushesComplete(SystemAddress systemAddress, unsigned short setID)
{
	(void) setID;

	char str[32];
	systemAddress.ToString(true, (char *) str, static_cast<size_t>(32));
	RAKNET_DEBUG_PRINTF("File pushes complete to %s\n", str);
}
void FLP_Printf::OnSendAborted(SystemAddress systemAddress)
{
	char str[32];
	systemAddress.ToString(true, (char *) str, static_cast<size_t>(32));
	RAKNET_DEBUG_PRINTF("Send aborted to %s\n", str);
}
FileList::FileList()
{
}
FileList::~FileList()
{
	Clear();
}
void FileList::AddFile(const char *filepath, const char *filename, FileListNodeContext context)
{
	if (filepath == 0 || filename == 0)
		return;

	char *data;
	//std::fstream file;
	//file.open(filename, std::ios::in | std::ios::binary);

	FILE *fp;
	if (fopen_s(&fp, filepath, "rb") != 0)
		return;
	fseek(fp, 0, SEEK_END);
	int length = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (length > (int) ((unsigned int) -1 / 8))
	{
		// If this assert hits, split up your file. You could also change BitSize_t in types.h to unsigned long long but this is not recommended for performance reasons
		RakAssert("Cannot add files over 536 MB" && 0);
		fclose(fp);
		return;
	}


	std::vector<char> fileData(length);
	data = fileData.data();

	// A short read means the file shrank or failed under us; skip it like an unopenable one.
	const bool readOk = fread(data, 1, length, fp) == (size_t) length;
	if (readOk)
		AddFile(filename, filepath, data, length, length, context);
	fclose(fp);
}
void FileList::AddFile(const char *filename, const char *fullPathToFile, const char *data, const unsigned dataLength, const unsigned fileLength, FileListNodeContext context, bool isAReference, bool takeDataPointer)
{
	if (filename == 0)
		return;
	if (strlen(filename) > MAX_FILENAME_LENGTH)
	{
		// Should be enough for anyone
		RakAssert(0);
		return;
	}
	// If adding a reference, do not send data
	RakAssert(isAReference == false || data == 0);
	// Avoid duplicate insertions unless the data is different, in which case overwrite the old data
	unsigned i;
	for (i = 0; i < fileList.size(); i++)
	{
		if (fileList[i].filename == filename)
		{
			if (fileList[i].fileLengthBytes == fileLength && fileList[i].dataLengthBytes == dataLength &&
			    (dataLength == 0 || fileList[i].data == 0 ||
			     memcmp(fileList[i].data, data, dataLength) == 0))
				// Exact same file already here
				return;

			// File of the same name, but different contents, so overwrite
			std::free(fileList[i].data);
			fileList.erase(fileList.begin() + i);
			break;
		}
	}

	FileListNode n;
	//	size_t fileNameLen = strlen(filename);
	if (dataLength && data)
	{
		if (takeDataPointer)
		{
			n.data = (char *) data;
		}
		else
		{
			n.data = (char *) std::malloc(dataLength);
			RakAssert(n.data);
			memcpy(n.data, data, dataLength);
		}
	}
	else
		n.data = 0;
	n.dataLengthBytes = dataLength;
	n.fileLengthBytes = fileLength;
	n.isAReference = isAReference;
	n.context = context;
	if (n.context.dataPtr == 0)
		n.context.dataPtr = n.data;
	if (n.context.dataLength == 0)
		n.context.dataLength = dataLength;
	n.filename = filename;
	n.fullPathToFile = fullPathToFile ? fullPathToFile : "";

	fileList.push_back(n);
}
// Keep the caller's path spelling in callbacks and in the names stored by AddFile.
static void EnsureDirectorySeparator(std::string &path)
{
	if (!path.empty() && !IsSlash(path.back()))
		path += static_cast<char>(std::filesystem::path::preferred_separator);
}

void FileList::AddFilesFromDirectory(const char *applicationDirectory, const char *subDirectory, bool writeHash, bool writeData, bool recursive, FileListNodeContext context)
{
	namespace fs = std::filesystem;
	std::string start = applicationDirectory ? applicationDirectory : "";
	EnsureDirectorySeparator(start);
	const size_t rootLength = start.size();
	if (subDirectory)
	{
		start += subDirectory;
		EnsureDirectorySeparator(start);
	}
	for (size_t i = 0; i < fileListProgressCallbacks.size(); ++i)
		fileListProgressCallbacks[i]->OnAddFilesFromDirectoryStarted(this, start.data());

	std::deque<std::string> dirList;
	dirList.push_back(std::move(start));
	while (!dirList.empty())
	{
		std::string directory = std::move(dirList.front());
		dirList.pop_front();
		std::error_code ec;
		fs::directory_iterator entry(directory.empty() ? "." : directory, ec);
		if (ec)
			return;

		std::string pattern = directory + "*";
		for (size_t i = 0; i < fileListProgressCallbacks.size(); ++i)
			fileListProgressCallbacks[i]->OnDirectory(this, pattern.data(), static_cast<unsigned int>(dirList.size()));

		for (; entry != fs::directory_iterator(); entry.increment(ec))
		{
#ifdef _WIN32
			// std::filesystem does not expose the hidden and system attributes.
			const DWORD attributes = GetFileAttributesW(entry->path().c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
				continue;
#endif
			const fs::file_status status = entry->status(ec);
			if (ec)
				continue;
			std::string name = entry->path().filename().string();
			const std::string fullPath = directory + name;
			if (fs::is_directory(status))
			{
				if (recursive)
					dirList.push_back(fullPath + "/");
				continue;
			}
			if (!fs::is_regular_file(status) || fullPath.size() - rootLength > MAX_FILENAME_LENGTH)
				continue;

			const auto size = entry->file_size(ec);
			// FileList lengths are unsigned int and SuperFastHash accepts an int.
			if (ec || size > std::numeric_limits<unsigned int>::max() || (writeHash && size > std::numeric_limits<int>::max()))
				continue;
			const auto fileSize = static_cast<unsigned int>(size);
			const char *relativeName = fullPath.c_str() + rootLength;
			for (size_t i = 0; i < fileListProgressCallbacks.size(); ++i)
				fileListProgressCallbacks[i]->OnFile(this, directory.data(), name.data(), fileSize);

			if (writeData)
			{
				std::ifstream input(entry->path(), std::ios::binary);
				if (!input)
					continue;
				const unsigned int prefix = writeHash ? HASH_LENGTH : 0;
				std::vector<char> data(prefix + fileSize);
				if (fileSize && !input.read(data.data() + prefix, fileSize))
					continue;

				if (writeHash)
				{
					unsigned int hash = SuperFastHash(data.data() + prefix, static_cast<int>(fileSize));
					if (BitStream::DoEndianSwap())
						BitStream::ReverseBytesInPlace((unsigned char *) &hash, sizeof(hash));
					memcpy(data.data(), &hash, HASH_LENGTH);
				}
				AddFile(relativeName, fullPath.c_str(), data.data(), prefix + fileSize, fileSize, context);
			}
			else if (writeHash)
			{
				unsigned int hash = SuperFastHashFile(fullPath.c_str());
				if (BitStream::DoEndianSwap())
					BitStream::ReverseBytesInPlace((unsigned char *) &hash, sizeof(hash));
				AddFile(relativeName, fullPath.c_str(), (const char *) &hash, HASH_LENGTH, fileSize, context);
			}
			else
			{
				AddFile(relativeName, fullPath.c_str(), nullptr, 0, fileSize, context);
			}
		}
	}
}
void FileList::Clear(void)
{
	unsigned i;
	for (i = 0; i < fileList.size(); i++)
	{
		std::free(fileList[i].data);
	}
	fileList.clear();
}
void FileList::Serialize(MafiaNet::BitStream *outBitStream)
{
	outBitStream->WriteCompressed(static_cast<unsigned int>(fileList.size()));
	unsigned i;
	for (i = 0; i < fileList.size(); i++)
	{
		outBitStream->WriteCompressed(fileList[i].context.op);
		outBitStream->WriteCompressed(fileList[i].context.flnc_extraData1);
		outBitStream->WriteCompressed(fileList[i].context.flnc_extraData2);
		StringCompressor::Instance()->EncodeString(fileList[i].filename.c_str(), MAX_FILENAME_LENGTH, outBitStream);

		bool writeFileData = (fileList[i].dataLengthBytes > 0) == true;
		outBitStream->Write(writeFileData);
		if (writeFileData)
		{
			outBitStream->WriteCompressed(fileList[i].dataLengthBytes);
			outBitStream->Write(fileList[i].data, fileList[i].dataLengthBytes);
		}

		outBitStream->Write((bool) (fileList[i].fileLengthBytes == fileList[i].dataLengthBytes));
		if (fileList[i].fileLengthBytes != fileList[i].dataLengthBytes)
			outBitStream->WriteCompressed(fileList[i].fileLengthBytes);
	}
}
bool FileList::Deserialize(MafiaNet::BitStream *inBitStream)
{
	bool b, dataLenNonZero = false, fileLenMatchesDataLen = false;
	char filename[512];
	uint32_t fileListSize;
	FileListNode n;
	b = inBitStream->ReadCompressed(fileListSize);
#ifdef _DEBUG
	RakAssert(b);
	RakAssert(fileListSize < 10000);
#endif
	if (b == false || fileListSize > 10000)
		return false; // Sanity check
	Clear();
	unsigned i;
	for (i = 0; i < fileListSize; i++)
	{
		inBitStream->ReadCompressed(n.context.op);
		inBitStream->ReadCompressed(n.context.flnc_extraData1);
		inBitStream->ReadCompressed(n.context.flnc_extraData2);
		StringCompressor::Instance()->DecodeString((char *) filename, MAX_FILENAME_LENGTH, inBitStream);
		inBitStream->Read(dataLenNonZero);
		if (dataLenNonZero)
		{
			inBitStream->ReadCompressed(n.dataLengthBytes);
			// sanity check
			if (n.dataLengthBytes > 2000000000)
			{
#ifdef _DEBUG
				RakAssert(n.dataLengthBytes <= 2000000000);
#endif
				return false;
			}
			n.data = (char *) std::malloc((size_t) n.dataLengthBytes);
			RakAssert(n.data);
			inBitStream->Read(n.data, n.dataLengthBytes);
		}
		else
		{
			n.dataLengthBytes = 0;
			n.data = 0;
		}

		b = inBitStream->Read(fileLenMatchesDataLen);
		if (fileLenMatchesDataLen)
			n.fileLengthBytes = (unsigned) n.dataLengthBytes;
		else
			b = inBitStream->ReadCompressed(n.fileLengthBytes);
#ifdef _DEBUG
		RakAssert(b);
#endif
		if (b == 0)
		{
			Clear();
			return false;
		}
		n.filename = filename;
		n.fullPathToFile = filename;
		fileList.push_back(n);
	}

	return true;
}
void FileList::GetDeltaToCurrent(FileList *input, FileList *output, const char *dirSubset, const char *remoteSubdir)
{
	// For all files in this list that do not match the input list, write them to the output list.
	// dirSubset allows checking only a portion of the files in this list.
	unsigned thisIndex, inputIndex;
	unsigned dirSubsetLen, localPathLen, remoteSubdirLen;
	bool match;
	if (dirSubset)
		dirSubsetLen = (unsigned int) strlen(dirSubset);
	else
		dirSubsetLen = 0;
	if (remoteSubdir && remoteSubdir[0])
	{
		remoteSubdirLen = (unsigned int) strlen(remoteSubdir);
		if (IsSlash(remoteSubdir[remoteSubdirLen - 1]))
			remoteSubdirLen--;
	}
	else
		remoteSubdirLen = 0;

	for (thisIndex = 0; thisIndex < fileList.size(); thisIndex++)
	{
		localPathLen = (unsigned int) fileList[thisIndex].filename.size();
		while (localPathLen > 0)
		{
			if (IsSlash(fileList[thisIndex].filename[localPathLen - 1]))
			{
				localPathLen--;
				break;
			}
			localPathLen--;
		}

		// fileList[thisIndex].filename has to match dirSubset and be shorter or equal to it in length.
		if (dirSubsetLen > 0 &&
		    (localPathLen < dirSubsetLen ||
		     _strnicmp(fileList[thisIndex].filename.c_str(), dirSubset, dirSubsetLen) != 0 ||
		     (localPathLen > dirSubsetLen && IsSlash(fileList[thisIndex].filename[dirSubsetLen]) == false)))
			continue;

		match = false;
		for (inputIndex = 0; inputIndex < input->fileList.size(); inputIndex++)
		{
			// If the filenames, hashes, and lengths match then skip this element in fileList.  Otherwise write it to output
			if (_stricmp(input->fileList[inputIndex].filename.c_str() + remoteSubdirLen, fileList[thisIndex].filename.c_str() + dirSubsetLen) == 0)
			{
				match = true;
				if (input->fileList[inputIndex].fileLengthBytes == fileList[thisIndex].fileLengthBytes &&
				    input->fileList[inputIndex].dataLengthBytes == fileList[thisIndex].dataLengthBytes &&
				    memcmp(input->fileList[inputIndex].data, fileList[thisIndex].data, (size_t) fileList[thisIndex].dataLengthBytes) == 0)
				{
					// File exists on both machines and is the same.
					break;
				}
				else
				{
					// File exists on both machines and is not the same.
					output->AddFile(fileList[thisIndex].filename.c_str(), fileList[thisIndex].fullPathToFile.c_str(), 0, 0, fileList[thisIndex].fileLengthBytes, FileListNodeContext(0, 0, 0, 0), false);
					break;
				}
			}
		}
		if (match == false)
		{
			// Other system does not have the file at all
			output->AddFile(fileList[thisIndex].filename.c_str(), fileList[thisIndex].fullPathToFile.c_str(), 0, 0, fileList[thisIndex].fileLengthBytes, FileListNodeContext(0, 0, 0, 0), false);
		}
	}
}
void FileList::ListMissingOrChangedFiles(const char *applicationDirectory, FileList *missingOrChangedFiles, bool alwaysWriteHash, bool neverWriteHash)
{
	unsigned fileLength;
	FILE *fp;
	char fullPath[512];
	unsigned i;

	for (i = 0; i < fileList.size(); i++)
	{
		strcpy_s(fullPath, applicationDirectory);
		FixEndingSlash(fullPath, 512);
		strcat_s(fullPath, fileList[i].filename.c_str());
		if (fopen_s(&fp, fullPath, "rb") != 0)
		{
			missingOrChangedFiles->AddFile(fileList[i].filename.c_str(), fileList[i].fullPathToFile.c_str(), 0, 0, 0, FileListNodeContext(0, 0, 0, 0), false);
		}
		else
		{
			fseek(fp, 0, SEEK_END);
			fileLength = ftell(fp);
			fseek(fp, 0, SEEK_SET);

			if (fileLength != fileList[i].fileLengthBytes && alwaysWriteHash == false)
			{
				missingOrChangedFiles->AddFile(fileList[i].filename.c_str(), fileList[i].fullPathToFile.c_str(), 0, 0, fileLength, FileListNodeContext(0, 0, 0, 0), false);
			}
			else
			{
				unsigned int hash = SuperFastHashFilePtr(fp);
				if (MafiaNet::BitStream::DoEndianSwap())
					MafiaNet::BitStream::ReverseBytesInPlace((unsigned char *) &hash, sizeof(hash));

				if (fileLength != fileList[i].fileLengthBytes || memcmp(&hash, fileList[i].data, HASH_LENGTH) != 0)
				{
					if (neverWriteHash == false)
						missingOrChangedFiles->AddFile(fileList[i].filename.c_str(), fileList[i].fullPathToFile.c_str(), (const char *) &hash, HASH_LENGTH, fileLength, FileListNodeContext(0, 0, 0, 0), false);
					else
						missingOrChangedFiles->AddFile(fileList[i].filename.c_str(), fileList[i].fullPathToFile.c_str(), 0, 0, fileLength, FileListNodeContext(0, 0, 0, 0), false);
				}
			}
			fclose(fp);
		}
	}
}
void FileList::PopulateDataFromDisk(const char *applicationDirectory, bool writeFileData, bool writeFileHash, bool removeUnknownFiles)
{
	FILE *fp;
	char fullPath[512];
	unsigned i;

	i = 0;
	while (i < fileList.size())
	{
		std::free(fileList[i].data);
		strcpy_s(fullPath, applicationDirectory);
		FixEndingSlash(fullPath, 512);
		strcat_s(fullPath, fileList[i].filename.c_str());
		if (fopen_s(&fp, fullPath, "rb") == 0)
		{
			if (writeFileHash || writeFileData)
			{
				bool readOk = true;
				fseek(fp, 0, SEEK_END);
				fileList[i].fileLengthBytes = ftell(fp);
				fseek(fp, 0, SEEK_SET);
				if (writeFileHash)
				{
					if (writeFileData)
					{
						// Hash + data so offset the data by HASH_LENGTH
						fileList[i].data = (char *) std::malloc(fileList[i].fileLengthBytes + HASH_LENGTH);
						RakAssert(fileList[i].data);
						readOk = fileList[i].fileLengthBytes == 0 || fread(fileList[i].data + HASH_LENGTH, fileList[i].fileLengthBytes, 1, fp) == 1;
						unsigned int hash = SuperFastHash(fileList[i].data + HASH_LENGTH, fileList[i].fileLengthBytes);
						if (MafiaNet::BitStream::DoEndianSwap())
							MafiaNet::BitStream::ReverseBytesInPlace((unsigned char *) &hash, sizeof(hash));
						memcpy(fileList[i].data, &hash, HASH_LENGTH);
					}
					else
					{
						// Hash only
						fileList[i].dataLengthBytes = HASH_LENGTH;
						if (fileList[i].fileLengthBytes < HASH_LENGTH)
							fileList[i].data = (char *) std::malloc(HASH_LENGTH);
						else
							fileList[i].data = (char *) std::malloc(fileList[i].fileLengthBytes);
						RakAssert(fileList[i].data);
						readOk = fileList[i].fileLengthBytes == 0 || fread(fileList[i].data, fileList[i].fileLengthBytes, 1, fp) == 1;
						unsigned int hash = SuperFastHash(fileList[i].data, fileList[i].fileLengthBytes);
						if (MafiaNet::BitStream::DoEndianSwap())
							MafiaNet::BitStream::ReverseBytesInPlace((unsigned char *) &hash, sizeof(hash));
						memcpy(fileList[i].data, &hash, HASH_LENGTH);
					}
				}
				else
				{
					// Data only
					fileList[i].dataLengthBytes = fileList[i].fileLengthBytes;
					fileList[i].data = (char *) std::malloc(fileList[i].fileLengthBytes);
					RakAssert(fileList[i].data);
					readOk = fileList[i].fileLengthBytes == 0 || fread(fileList[i].data, fileList[i].fileLengthBytes, 1, fp) == 1;
				}

				if (!readOk)
				{
					// Short read: drop the partial contents rather than keep uninitialized bytes
					// (and a hash computed over them).
					std::free(fileList[i].data);
					fileList[i].data = 0;
					fileList[i].dataLengthBytes = 0;
				}
			}
			else
			{
				fileList[i].data = 0;
				fileList[i].dataLengthBytes = 0;
			}

			// Both branches are done with this entry. The no-data/no-hash branch used to skip
			// these, leaking fp and re-processing entry i forever.
			fclose(fp);
			i++;
		}
		else
		{
			if (removeUnknownFiles)
			{
				fileList.erase(fileList.begin() + i);
			}
			else
				i++;
		}
	}
}
void FileList::FlagFilesAsReferences(void)
{
	for (unsigned int i = 0; i < fileList.size(); i++)
	{
		fileList[i].isAReference = true;
		fileList[i].dataLengthBytes = fileList[i].fileLengthBytes;
	}
}
void FileList::WriteDataToDisk(const char *applicationDirectory)
{
	char fullPath[512];
	unsigned i, j;

	for (i = 0; i < fileList.size(); i++)
	{
		strcpy_s(fullPath, applicationDirectory);
		FixEndingSlash(fullPath, 512);
		strcat_s(fullPath, fileList[i].filename.c_str());

		// Security - Don't allow .. in the filename anywhere so you can't write outside of the root directory
		for (j = 1; j < fileList[i].filename.size(); j++)
		{
			if (fileList[i].filename[j] == '.' && fileList[i].filename[j - 1] == '.')
			{
#ifdef _DEBUG
				RakAssert(0);
#endif
				// Just cancel the write entirely
				return;
			}
		}

		WriteFileWithDirectories(fullPath, fileList[i].data, (unsigned int) fileList[i].dataLengthBytes);
	}
}

void FileList::DeleteFiles(const char *applicationDirectory)
{


	char fullPath[512];
	unsigned i, j;

	for (i = 0; i < fileList.size(); i++)
	{
		// The filename should not have .. in the path - if it does ignore it
		for (j = 1; j < fileList[i].filename.size(); j++)
		{
			if (fileList[i].filename[j] == '.' && fileList[i].filename[j - 1] == '.')
			{
#ifdef _DEBUG
				RakAssert(0);
#endif
				// Just cancel the deletion entirely
				return;
			}
		}

		strcpy_s(fullPath, applicationDirectory);
		FixEndingSlash(fullPath, 512);
		strcat_s(fullPath, fileList[i].filename.c_str());

		// Do not rename to _unlink as linux uses unlink
#if defined(_WIN32)
		int result = _unlink(fullPath);
#else
		int result = unlink(fullPath);
#endif
		if (result != 0)
		{
			RAKNET_DEBUG_PRINTF("FileList::DeleteFiles: unlink (%s) failed.\n", fullPath);
		}
	}
}

void FileList::AddCallback(FileListProgress *cb)
{
	if (cb == 0)
		return;

	if (std::find(fileListProgressCallbacks.begin(), fileListProgressCallbacks.end(), cb) == fileListProgressCallbacks.end())
		fileListProgressCallbacks.push_back(cb);
}
void FileList::RemoveCallback(FileListProgress *cb)
{
	const auto it = std::find(fileListProgressCallbacks.begin(), fileListProgressCallbacks.end(), cb);
	if (it != fileListProgressCallbacks.end())
		fileListProgressCallbacks.erase(it);
}
void FileList::ClearCallbacks(void)
{
	fileListProgressCallbacks.clear();
}
void FileList::GetCallbacks(std::vector<FileListProgress *> &callbacks)
{
	callbacks = fileListProgressCallbacks;
}

bool FileList::FixEndingSlash(char *str)
{
#ifdef _WIN32
	if (str[strlen(str) - 1] != '/' && str[strlen(str) - 1] != '\\')
	{
		strcat(str, "\\"); // Only \ works with system commands
		return true;
	}
#else
	if (str[strlen(str) - 1] != '\\' && str[strlen(str) - 1] != '/')
	{
		strcat(str, "/"); // Only / works with Linux
		return true;
	}
#endif

	return false;
}

bool FileList::FixEndingSlash(char *str, size_t strLength)
{
#ifdef _WIN32
	if (str[strlen(str) - 1] != '/' && str[strlen(str) - 1] != '\\')
	{
		strcat_s(str, strLength, "\\"); // Only \ works with system commands
		return true;
	}
#else
	if (str[strlen(str) - 1] != '\\' && str[strlen(str) - 1] != '/')
	{
		strcat_s(str, strLength, "/"); // Only / works with Linux
		return true;
	}
#endif

	return false;
}

#endif // _RAKNET_SUPPORT_FileOperations

/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "mafianet/peer.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/FileListTransfer.h"
#include "mafianet/FileListTransferCBInterface.h"
#include "mafianet/DirectoryDeltaTransfer.h"
#include "mafianet/IncrementalReadInterface.h"
#include "mafianet/sleep.h"
#include "mafianet/GetTime.h"

using namespace MafiaNet;

/*
Description:
DirectoryDeltaTransfer writes a file pushed through an IncrementalReadInterface to disk chunk by
chunk, at each chunk's offset, instead of holding the whole file in memory until its last chunk. The
property to hold is that the downloaded directory is byte-identical to the uploaded one -- for a file
many chunks long, one smaller than a chunk and one that ends mid-chunk, with the server's send window
open -- and that a streamed file reaches the application's OnFile with no buffered copy of its data.

Success conditions:
- Every file on disk below the output directory matches its source byte for byte.
- OnFile sees no fileData for the multi-chunk files: FileListTransfer kept none.
- OnDownloadComplete fires once.
- A second download of the unchanged directory sends no file at all, and after one file changes
  and the uploads are registered again, only that file is sent. AddFile records each upload's hash
  rather than its contents, so the comparison with the downloader's hashes can match.

Failure conditions: any byte differs, a file is missing, a streamed file arrives buffered, an
unchanged file is sent again, or a download does not complete in time.
*/

namespace
{
	const int kConnectTimeoutMs  = 15000;
	const int kTransferTimeoutMs = 60000;
	const unsigned int kChunkSize = 64 * 1024;

	class Observer : public FileListTransferCBInterface
	{
	public:
		std::map<std::string, bool> buffered;
		bool complete = false;
		int completions = 0;

		bool OnFile(OnFileStruct *onFileStruct) override
		{
			buffered[onFileStruct->fileName] = onFileStruct->fileData != 0;
			return true;
		}

		void OnFileProgress(FileProgressStruct *) override {}

		bool OnDownloadComplete(DownloadCompleteStruct *) override
		{
			complete = true;
			++completions;
			return false;
		}
	};

	std::vector<char> Pattern(size_t size, unsigned seed)
	{
		std::vector<char> bytes(size);
		unsigned value = seed * 2654435761u + 1;
		for (size_t i = 0; i < size; ++i)
		{
			value = value * 1103515245u + 12345u;
			bytes[i] = (char)(value >> 16);
		}
		return bytes;
	}

	std::vector<char> ReadAll(const std::filesystem::path &path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
} // namespace

TEST(DirectoryDeltaTransferStreaming, LargeFilesAreWrittenChunkByChunk)
{
	const unsigned short port = 61211;
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "mafianet_ddt_streaming";
	const std::filesystem::path source = root / "source";
	const std::filesystem::path output = root / "output";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(source);
	std::filesystem::create_directories(output);

	// Many chunks; less than one chunk; ends mid-chunk.
	const std::map<std::string, std::vector<char>> expected = {
		{"large.bin", Pattern(kChunkSize * 37 + 1234, 1)},
		{"small.bin", Pattern(900, 2)},
		{"ragged.bin", Pattern(kChunkSize * 3 + 77, 3)},
	};
	for (const auto &entry : expected)
	{
		std::ofstream out(source / entry.first, std::ios::binary);
		out.write(entry.second.data(), (std::streamsize)entry.second.size());
	}

	RakPeerInterface *server = RakPeerInterface::GetInstance();
	RakPeerInterface *client = RakPeerInterface::GetInstance();
	FileListTransfer serverTransfer;
	FileListTransfer clientTransfer;
	DirectoryDeltaTransfer serverDelta;
	DirectoryDeltaTransfer clientDelta;
	IncrementalReadInterface reader;
	server->AttachPlugin(&serverTransfer);
	server->AttachPlugin(&serverDelta);
	client->AttachPlugin(&clientTransfer);
	client->AttachPlugin(&clientDelta);
	serverDelta.SetFileListTransferPlugin(&serverTransfer);
	clientDelta.SetFileListTransferPlugin(&clientTransfer);
	serverTransfer.SetReferencePushWindow(8);
	serverDelta.SetDownloadRequestIncrementalReadInterface(&reader, kChunkSize);

	const std::string sourceDir = source.string() + "/";
	serverDelta.SetApplicationDirectory(sourceDir.c_str());
	for (const auto &entry : expected)
		serverDelta.AddFile((source / entry.first).string().c_str(), entry.first.c_str());
	const std::string outputDir = output.string() + "/";
	clientDelta.SetApplicationDirectory(outputDir.c_str());

	SocketDescriptor serverSocket(port, "127.0.0.1");
	SocketDescriptor clientSocket(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(4, &serverSocket, 1), RAKNET_STARTED);
	ASSERT_EQ(client->Startup(1, &clientSocket, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(4);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	SystemAddress serverAddress = UNASSIGNED_SYSTEM_ADDRESS;
	bool serverSawClient = false;
	TimeMS entry = GetTimeMS();
	while ((serverAddress == UNASSIGNED_SYSTEM_ADDRESS || !serverSawClient) && GetTimeMS() - entry < (TimeMS)kConnectTimeoutMs)
	{
		for (Packet *p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
			if (p->data[0] == ID_NEW_INCOMING_CONNECTION)
				serverSawClient = true;
		for (Packet *p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
			if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
				serverAddress = p->systemAddress;
		RakSleep(5);
	}
	ASSERT_NE(serverAddress, UNASSIGNED_SYSTEM_ADDRESS);
	ASSERT_TRUE(serverSawClient);

	const auto download = [&](Observer &observer) {
		clientDelta.DownloadFromSubdirectory(nullptr, nullptr, true, serverAddress, &observer, Priority::Medium, 0, nullptr);
		const TimeMS began = GetTimeMS();
		while (!observer.complete && GetTimeMS() - began < (TimeMS)kTransferTimeoutMs)
		{
			for (Packet *p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
				;
			for (Packet *p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
				;
			RakSleep(1);
		}
	};

	Observer observer;
	download(observer);

	EXPECT_TRUE(observer.complete);
	EXPECT_EQ(observer.completions, 1);
	for (const auto &file : expected)
	{
		EXPECT_TRUE(ReadAll(output / file.first) == file.second) << file.first << " differs on disk";
	}
	EXPECT_FALSE(observer.buffered["large.bin"]) << "a multi-chunk file was held in memory";
	EXPECT_FALSE(observer.buffered["ragged.bin"]) << "a multi-chunk file was held in memory";

	// Nothing changed: nothing is sent.
	Observer unchanged;
	download(unchanged);
	EXPECT_TRUE(unchanged.complete);
	EXPECT_TRUE(unchanged.buffered.empty()) << unchanged.buffered.size() << " unchanged file(s) were sent again";

	// One file changed and the uploads are registered again: only it is sent.
	const std::vector<char> changedBytes = Pattern(1500, 9);
	{
		std::ofstream out(source / "small.bin", std::ios::binary | std::ios::trunc);
		out.write(changedBytes.data(), (std::streamsize)changedBytes.size());
	}
	serverDelta.ClearUploads();
	for (const auto &entry : expected)
		serverDelta.AddFile((source / entry.first).string().c_str(), entry.first.c_str());
	Observer changed;
	download(changed);
	EXPECT_TRUE(changed.complete);
	EXPECT_EQ(changed.buffered.size(), 1u);
	EXPECT_EQ(changed.buffered.count("small.bin"), 1u);
	EXPECT_TRUE(ReadAll(output / "small.bin") == changedBytes);

	client->Shutdown(100);
	server->Shutdown(100);
	RakPeerInterface::DestroyInstance(client);
	RakPeerInterface::DestroyInstance(server);
	std::filesystem::remove_all(root);
}

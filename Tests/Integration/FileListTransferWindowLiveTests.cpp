/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mafianet/peer.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/FileList.h"
#include "mafianet/FileListTransfer.h"
#include "mafianet/FileListTransferCBInterface.h"
#include "mafianet/IncrementalReadInterface.h"
#include "mafianet/sleep.h"
#include "mafianet/GetTime.h"

using namespace MafiaNet;

/*
Description:
FileListTransfer::SetReferencePushWindow() keeps several IncrementalReadInterface chunks in flight
instead of one per round trip. The recipient is unchanged, so the property to hold is that a windowed
transfer delivers exactly the same bytes, in order, as an unwindowed one -- across a file many chunks
long, a file smaller than one chunk, and a file that ends mid-chunk -- with the recipient writing each
chunk at its offset and letting none of them be buffered for it.

Success conditions:
- With windows 1, 4 and 16, every file arrives byte-identical, through chunks written at their
  offsets (allocateIrIDataChunkAutomatically = false) or, for a file below one chunk, through OnFile.
- OnDownloadComplete fires once per transfer.
- The window setter clamps 0 to 1.

Failure conditions: any byte differs, a file is missing, or the transfer does not complete in time.
*/

namespace
{
	const int kConnectTimeoutMs  = 15000;
	const int kTransferTimeoutMs = 60000;
	const unsigned int kChunkSize = 64 * 1024;

	// Writes every chunk at its offset, as a recipient that streams to disk does.
	class Receiver : public FileListTransferCBInterface
	{
	public:
		std::map<std::string, std::vector<char>> files;
		bool complete = false;
		int completions = 0;

		bool OnFile(OnFileStruct *onFileStruct) override
		{
			std::vector<char> &file = files[onFileStruct->fileName];
			file.resize(onFileStruct->byteLengthOfThisFile);
			// Below one chunk the whole file arrives here instead of through OnFileProgress.
			if (onFileStruct->fileData != 0 && onFileStruct->byteLengthOfThisFile > 0)
				memcpy(file.data(), onFileStruct->fileData, onFileStruct->byteLengthOfThisFile);
			return true;
		}

		void OnFileProgress(FileProgressStruct *fps) override
		{
			fps->allocateIrIDataChunkAutomatically = false;
			if (fps->iriDataChunk == 0)
				return;
			std::vector<char> &file = files[fps->onFileStruct->fileName];
			file.resize(fps->onFileStruct->byteLengthOfThisFile);
			ASSERT_LE((size_t)fps->iriWriteOffset + fps->dataChunkLength, file.size());
			memcpy(file.data() + fps->iriWriteOffset, fps->iriDataChunk, fps->dataChunkLength);
		}

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

	void RunTransfer(unsigned int window, unsigned short port)
	{
		const std::filesystem::path root = std::filesystem::temp_directory_path() / ("mafianet_flt_window_" + std::to_string(window));
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);

		// Many chunks; less than one chunk; ends mid-chunk.
		const std::map<std::string, std::vector<char>> expected = {
			{"large.bin", Pattern(kChunkSize * 37 + 1234, 1)},
			{"small.bin", Pattern(900, 2)},
			{"ragged.bin", Pattern(kChunkSize * 3 + 77, 3)},
		};
		for (const auto &entry : expected)
		{
			std::ofstream out(root / entry.first, std::ios::binary);
			out.write(entry.second.data(), (std::streamsize)entry.second.size());
		}

		RakPeerInterface *server = RakPeerInterface::GetInstance();
		RakPeerInterface *client = RakPeerInterface::GetInstance();
		FileListTransfer serverTransfer;
		FileListTransfer clientTransfer;
		IncrementalReadInterface reader;
		server->AttachPlugin(&serverTransfer);
		client->AttachPlugin(&clientTransfer);
		serverTransfer.SetReferencePushWindow(window);
		EXPECT_EQ(serverTransfer.GetReferencePushWindow(), window);

		SocketDescriptor serverSocket(port, "127.0.0.1");
		SocketDescriptor clientSocket(0, "127.0.0.1");
		ASSERT_EQ(server->Startup(4, &serverSocket, 1), RAKNET_STARTED);
		ASSERT_EQ(client->Startup(1, &clientSocket, 1), RAKNET_STARTED);
		server->SetMaximumIncomingConnections(4);
		ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

		SystemAddress clientAddress = UNASSIGNED_SYSTEM_ADDRESS;
		SystemAddress serverAddress = UNASSIGNED_SYSTEM_ADDRESS;
		TimeMS entry = GetTimeMS();
		while ((clientAddress == UNASSIGNED_SYSTEM_ADDRESS || serverAddress == UNASSIGNED_SYSTEM_ADDRESS) && GetTimeMS() - entry < (TimeMS)kConnectTimeoutMs)
		{
			for (Packet *p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
				if (p->data[0] == ID_NEW_INCOMING_CONNECTION)
					clientAddress = p->systemAddress;
			for (Packet *p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
				if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
					serverAddress = p->systemAddress;
			RakSleep(5);
		}
		ASSERT_NE(clientAddress, UNASSIGNED_SYSTEM_ADDRESS);
		ASSERT_NE(serverAddress, UNASSIGNED_SYSTEM_ADDRESS);

		Receiver receiver;
		const unsigned short setId = clientTransfer.SetupReceive(&receiver, false, serverAddress);

		FileList list;
		for (const auto &file : expected)
		{
			const std::string full = (root / file.first).string();
			list.AddFile(file.first.c_str(), full.c_str(), 0, (unsigned)file.second.size(), (unsigned)file.second.size(), FileListNodeContext(0, 0, 0, 0), true);
		}
		serverTransfer.Send(&list, server, clientAddress, setId, Priority::Medium, 0, &reader, kChunkSize);

		entry = GetTimeMS();
		while (!receiver.complete && GetTimeMS() - entry < (TimeMS)kTransferTimeoutMs)
		{
			for (Packet *p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
				;
			for (Packet *p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
				;
			RakSleep(1);
		}

		EXPECT_TRUE(receiver.complete) << "window " << window;
		EXPECT_EQ(receiver.completions, 1);
		for (const auto &file : expected)
		{
			const auto received = receiver.files.find(file.first);
			ASSERT_NE(received, receiver.files.end()) << file.first << " (window " << window << ")";
			EXPECT_TRUE(received->second == file.second) << file.first << " differs (window " << window << ")";
		}

		client->Shutdown(100);
		server->Shutdown(100);
		RakPeerInterface::DestroyInstance(client);
		RakPeerInterface::DestroyInstance(server);
		std::filesystem::remove_all(root);
	}
} // namespace

TEST(FileListTransferWindow, UnwindowedTransferIsIntact)
{
	RunTransfer(1, 61201);
}

TEST(FileListTransferWindow, WindowOfFourDeliversTheSameBytes)
{
	RunTransfer(4, 61202);
}

TEST(FileListTransferWindow, WindowOfSixteenDeliversTheSameBytes)
{
	RunTransfer(16, 61203);
}

TEST(FileListTransferWindow, ZeroIsClampedToOne)
{
	FileListTransfer transfer;
	transfer.SetReferencePushWindow(0);
	EXPECT_EQ(transfer.GetReferencePushWindow(), 1u);
}

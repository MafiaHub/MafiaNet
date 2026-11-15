/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/send_to_thread.h"
#ifdef USE_THREADED_SEND
#include "mafianet/thread.h"
#include "mafianet/internal_packet.h"
#include "mafianet/get_time.h"

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL != 1
#include "mafianet/cc_udt.h"
#else
#include "mafianet/cc_sliding_window.h"
#endif

using namespace MafiaNet;

int SendToThread::refCount = 0;
LockedQueue<std::unique_ptr<SendToThread::SendToThreadBlock>> SendToThread::objectQueue;
ThreadPool<SendToThread::SendToThreadBlock *, SendToThread::SendToThreadBlock *> SendToThread::threadPool;

SendToThread::SendToThreadBlock *SendToWorkerThread(SendToThread::SendToThreadBlock *input, bool *returnOutput, void *perThreadData)
{
	(void) perThreadData;
	*returnOutput = false;
	//	MafiaNet::TimeUS *mostRecentTime=(MafiaNet::TimeUS *)input->data;
	//	*mostRecentTime=MafiaNet::GetTimeUS();
	SocketLayer::SendTo(input->s, input->data, input->dataWriteOffset, input->systemAddress, _FILE_AND_LINE_);
	SendToThread::objectQueue.Push(std::unique_ptr<SendToThread::SendToThreadBlock>(input));
	return 0;
}
SendToThread::SendToThread()
{
}
SendToThread::~SendToThread()
{
}
void SendToThread::AddRef(void)
{
	if (++refCount == 1)
	{
		threadPool.StartThreads(1, 0);
	}
}
void SendToThread::Deref(void)
{
	if (refCount > 0)
	{
		if (--refCount == 0)
		{
			threadPool.StopThreads();
			RakAssert(threadPool.NumThreadsWorking() == 0);

			// Blocks still waiting to be sent are dropped; free them together with the reusable ones.
			for (unsigned i = 0; i < threadPool.InputSize(); i++)
				objectQueue.Push(std::unique_ptr<SendToThreadBlock>(threadPool.GetInputAtIndex(i)));
			threadPool.ClearInput();
			objectQueue.Clear();
		}
	}
}
SendToThread::SendToThreadBlock *SendToThread::AllocateBlock(void)
{
	// The caller hands the block to ProcessBlock; the worker thread returns it to objectQueue once sent.
	if (auto recycled = objectQueue.TryPop())
		return recycled->release();
	return new SendToThreadBlock;
}
void SendToThread::ProcessBlock(SendToThread::SendToThreadBlock *threadedSend)
{
	RakAssert(threadedSend->dataWriteOffset > 0 && threadedSend->dataWriteOffset <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
	threadPool.AddInput(SendToWorkerThread, threadedSend);
}
#endif

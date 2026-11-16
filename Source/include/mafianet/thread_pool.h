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

#ifndef __THREAD_POOL_H
#define __THREAD_POOL_H

#include "mafianet/simple_mutex.h"
#include "mafianet/export.h"
#include "mafianet/thread.h"
#include "mafianet/signaled_event.h"

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

class ThreadDataInterface
{
public:
	ThreadDataInterface() {}
	virtual ~ThreadDataInterface() {}

	virtual void *PerThreadFactory(void *context) = 0;
	virtual void PerThreadDestructor(void *factoryResult, void *context) = 0;
};
/// A simple class to create worker threads that processes a queue of functions with data.
/// This class does not allocate or deallocate memory.  It is up to the user to handle memory management.
/// InputType and OutputType are stored directly in a queue.  For large structures, if you plan to delete from the middle of the queue,
/// you might wish to store pointers rather than the structures themselves so the array can shift efficiently.
template <class InputType, class OutputType>
struct RAK_DLL_EXPORT ThreadPool
{
	ThreadPool();
	~ThreadPool();
	// Owns mutexes, atomics and running threads that point back at it.
	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;

	/// Start the specified number of threads.
	/// \param[in] numThreads The number of threads to start
	/// \param[in] stackSize 0 for default (except on consoles).
	/// \param[in] _perThreadInit User callback to return data stored per thread.  Pass 0 if not needed.
	/// \param[in] _perThreadDeinit User callback to destroy data stored per thread, created by _perThreadInit.  Pass 0 if not needed.
	/// \return True on success, false on failure.
	bool StartThreads(int numThreads, int stackSize, void *(*_perThreadInit)() = 0, void (*_perThreadDeinit)(void *) = 0);

	// Alternate form of _perThreadDataFactory, _perThreadDataDestructor
	void SetThreadDataInterface(ThreadDataInterface *tdi, void *context);

	/// Stops all threads
	void StopThreads(void);

	/// Adds a function to a queue with data to pass to that function.  This function will be called from the thread
	/// Memory management is your responsibility!  This class does not allocate or deallocate memory.
	/// The best way to deallocate \a inputData is in userCallback.  If you call EndThreads such that callbacks were not called, you
	/// can iterate through the inputQueue and deallocate all pending input data there
	/// The best way to deallocate output is as it is returned to you from GetOutput.  Similarly, if you end the threads such that
	/// not all output was returned, you can iterate through outputQueue and deallocate it there.
	/// \param[in] workerThreadCallback The function to call from the thread
	/// \param[in] inputData The parameter to pass to \a userCallback
	void AddInput(OutputType (*workerThreadCallback)(InputType, bool *returnOutput, void *perThreadData), InputType inputData);

	/// Adds to the output queue
	/// Use it if you want to inject output into the same queue that the system uses. Normally you would not use this. Consider it a convenience function.
	/// \param[in] outputData The output to inject
	void AddOutput(OutputType outputData);

	/// Returns true if output from GetOutput is waiting.
	/// \return true if output is waiting, false otherwise
	bool HasOutput(void);

	/// Inaccurate but fast version of HasOutput (no lock: reads a count published under the lock).  If this returns true, you should still check HasOutput for the real value.
	/// \return true if output is probably waiting, false otherwise
	bool HasOutputFast(void);

	/// Returns true if input from GetInput is waiting.
	/// \return true if input is waiting, false otherwise
	bool HasInput(void);

	/// Inaccurate but fast version of HasInput (no lock: reads a count published under the lock).  If this returns true, you should still check HasInput for the real value.
	/// \return true if input is probably waiting, false otherwise
	bool HasInputFast(void);

	/// Gets the output of a call to \a userCallback
	/// HasOutput must return true before you call this function.  Otherwise it returns a value-initialized OutputType.
	/// \return The output of \a userCallback.  If you have different output signatures, it is up to you to encode the data to indicate this
	OutputType GetOutput(void);

	/// Clears internal buffers
	void Clear(void);

	/// Lock the input buffer before calling the functions InputSize, InputAtIndex, and RemoveInputAtIndex
	/// It is only necessary to lock the input or output while the threads are running
	void LockInput(void);

	/// Unlock the input buffer after you are done with the functions InputSize, GetInputAtIndex, and RemoveInputAtIndex
	void UnlockInput(void);

	/// Length of the input queue
	unsigned InputSize(void);

	/// Get the input at a specified index
	InputType GetInputAtIndex(unsigned index);

	/// Remove input from a specific index.  This does NOT do memory deallocation - it only removes the item from the queue
	void RemoveInputAtIndex(unsigned index);

	/// Lock the output buffer before calling the functions OutputSize, OutputAtIndex, and RemoveOutputAtIndex
	/// It is only necessary to lock the input or output while the threads are running
	void LockOutput(void);

	/// Unlock the output buffer after you are done with the functions OutputSize, GetOutputAtIndex, and RemoveOutputAtIndex
	void UnlockOutput(void);

	/// Length of the output queue
	unsigned OutputSize(void);

	/// Get the output at a specified index
	OutputType GetOutputAtIndex(unsigned index);

	/// Remove output from a specific index.  This does NOT do memory deallocation - it only removes the item from the queue
	void RemoveOutputAtIndex(unsigned index);

	/// Removes all items from the input queue
	void ClearInput(void);

	/// Removes all items from the output queue
	void ClearOutput(void);

	/// Are any of the threads working, or is input or output available?
	bool IsWorking(void);

	/// The number of currently active threads.
	int NumThreadsWorking(void);

	/// Did we call Start?
	bool WasStarted(void);

	// Block until all threads are stopped.
	bool Pause(void);

	// Continue running
	void Resume(void);

protected:
	// It is valid to cancel input before it is processed.  To do so, lock the inputQueue with inputQueueMutex,
	// Scan the list, and remove the item you don't want.
	std::mutex inputQueueMutex, outputQueueMutex;
	MafiaNet::SimpleMutex workingThreadCountMutex, runThreadsMutex;

	void *(*perThreadDataFactory)();
	void (*perThreadDataDestructor)(void *);

	using WorkerCallback = OutputType (*)(InputType, bool *, void *);

	// Guarded by inputQueueMutex: each input with the function to call on it.
	std::deque<std::pair<WorkerCallback, InputType>> inputQueue;
	// Guarded by outputQueueMutex.
	std::deque<OutputType> outputQueue;
	// inputQueue.size() / outputQueue.size(), stored under the matching mutex after every change, so
	// HasInputFast / HasOutputFast can poll without the lock and without a data race.
	std::atomic<std::size_t> inputCount, outputCount;

	ThreadDataInterface *threadDataInterface;
	void *tdiContext;


	template <class ThreadInputType, class ThreadOutputType>
	friend RAK_THREAD_DECLARATION(WorkerThread);

	/*
#ifdef _WIN32
	friend unsigned __stdcall WorkerThread( LPVOID arguments );
#else
	friend void* WorkerThread( void* arguments );
#endif
	*/

	/// \internal
	bool runThreads;
	/// \internal
	int numThreadsRunning;
	/// \internal
	int numThreadsWorking;
	/// \internal
	MafiaNet::SimpleMutex numThreadsRunningMutex;

	MafiaNet::SignaledEvent quitAndIncomingDataEvents;
};

#include "mafianet/thread_pool.h"
#include "mafianet/sleep.h"
#ifdef _WIN32

#else
#include <unistd.h>
#endif

// #med - consider simplifying this and use a simple macro?
// disable false-positive warnings 4701/4703 about inputData not being initialized (which it isn't in the case it's used)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4701) // potentially uninitialized local variable
#pragma warning(disable : 4703) // potentially uninitialized local pointer
#endif
template <class ThreadInputType, class ThreadOutputType>
RAK_THREAD_DECLARATION(WorkerThread)
/*
#ifdef _WIN32
unsigned __stdcall WorkerThread( LPVOID arguments )
#else
void* WorkerThread( void* arguments )
#endif
*/
{


	ThreadPool<ThreadInputType, ThreadOutputType> *threadPool = (ThreadPool<ThreadInputType, ThreadOutputType> *) arguments;


	bool returnOutput;
	ThreadOutputType (*userCallback)(ThreadInputType, bool *, void *);
	ThreadInputType inputData;
	ThreadOutputType callbackOutput;

	userCallback = 0;

	void *perThreadData;
	if (threadPool->perThreadDataFactory)
		perThreadData = threadPool->perThreadDataFactory();
	else if (threadPool->threadDataInterface)
		perThreadData = threadPool->threadDataInterface->PerThreadFactory(threadPool->tdiContext);
	else
		perThreadData = 0;

	// Increase numThreadsRunning
	threadPool->numThreadsRunningMutex.Lock();
	++threadPool->numThreadsRunning;
	threadPool->numThreadsRunningMutex.Unlock();

	for (;;)
	{
		//#ifdef _WIN32
		if (userCallback == 0)
		{
			threadPool->quitAndIncomingDataEvents.WaitOnEvent(1000);
		}
		// #else
		// 		if (userCallback==0)
		// 			RakSleep(30);
		// #endif

		threadPool->runThreadsMutex.Lock();
		if (threadPool->runThreads == false)
		{
			threadPool->runThreadsMutex.Unlock();
			break;
		}
		threadPool->runThreadsMutex.Unlock();

		threadPool->workingThreadCountMutex.Lock();
		++threadPool->numThreadsWorking;
		threadPool->workingThreadCountMutex.Unlock();

		// Read input data
		userCallback = 0;
		threadPool->inputQueueMutex.lock();
		if (!threadPool->inputQueue.empty())
		{
			userCallback = threadPool->inputQueue.front().first;
			inputData = threadPool->inputQueue.front().second;
			threadPool->inputQueue.pop_front();
			threadPool->inputCount.store(threadPool->inputQueue.size(), std::memory_order_relaxed);
		}
		threadPool->inputQueueMutex.unlock();

		if (userCallback)
		{
			callbackOutput = userCallback(inputData, &returnOutput, perThreadData);
			if (returnOutput)
			{
				threadPool->outputQueueMutex.lock();
				threadPool->outputQueue.push_back(callbackOutput);
				threadPool->outputCount.store(threadPool->outputQueue.size(), std::memory_order_relaxed);
				threadPool->outputQueueMutex.unlock();
			}
		}

		threadPool->workingThreadCountMutex.Lock();
		--threadPool->numThreadsWorking;
		threadPool->workingThreadCountMutex.Unlock();
	}

	if (threadPool->perThreadDataDestructor)
		threadPool->perThreadDataDestructor(perThreadData);
	else if (threadPool->threadDataInterface)
		threadPool->threadDataInterface->PerThreadDestructor(perThreadData, threadPool->tdiContext);

	// Decrease numThreadsRunning. Last touch of the pool: StopThreads() returns once the count reaches
	// 0, and the owner may then destroy the pool while this thread is still exiting.
	threadPool->numThreadsRunningMutex.Lock();
	--threadPool->numThreadsRunning;
	threadPool->numThreadsRunningMutex.Unlock();

	return 0;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

template <class InputType, class OutputType>
ThreadPool<InputType, OutputType>::ThreadPool()
    : inputCount(0), outputCount(0)
{
	runThreads = false;
	numThreadsRunning = 0;
	threadDataInterface = 0;
	tdiContext = 0;
	numThreadsWorking = 0;
}
template <class InputType, class OutputType>
ThreadPool<InputType, OutputType>::~ThreadPool()
{
	StopThreads();
	Clear();
}
template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::StartThreads(int numThreads, int stackSize, void *(*_perThreadDataFactory)(), void (*_perThreadDataDestructor)(void *))
{
	(void) stackSize;


	runThreadsMutex.Lock();
	if (runThreads == true)
	{
		// Already running
		runThreadsMutex.Unlock();
		return false;
	}
	runThreadsMutex.Unlock();

	quitAndIncomingDataEvents.InitEvent();

	perThreadDataFactory = _perThreadDataFactory;
	perThreadDataDestructor = _perThreadDataDestructor;

	runThreadsMutex.Lock();
	runThreads = true;
	runThreadsMutex.Unlock();

	numThreadsWorking = 0;
	unsigned threadId = 0;
	(void) threadId;
	int i;
	for (i = 0; i < numThreads; i++)
	{
		int errorCode;


		errorCode = MafiaNet::RakThread::Create(WorkerThread<InputType, OutputType>, this);

		if (errorCode != 0)
		{
			StopThreads();
			return false;
		}
	}
	// Wait for number of threads running to increase to numThreads
	bool done = false;
	while (done == false)
	{
		RakSleep(50);
		numThreadsRunningMutex.Lock();
		if (numThreadsRunning == numThreads)
			done = true;
		numThreadsRunningMutex.Unlock();
	}

	return true;
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::SetThreadDataInterface(ThreadDataInterface *tdi, void *context)
{
	threadDataInterface = tdi;
	tdiContext = context;
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::StopThreads(void)
{
	runThreadsMutex.Lock();
	if (runThreads == false)
	{
		runThreadsMutex.Unlock();
		return;
	}

	runThreads = false;
	runThreadsMutex.Unlock();

	// Wait for number of threads running to decrease to 0
	bool done = false;
	while (done == false)
	{
		quitAndIncomingDataEvents.SetEvent();

		RakSleep(50);
		numThreadsRunningMutex.Lock();
		if (numThreadsRunning == 0)
			done = true;
		numThreadsRunningMutex.Unlock();
	}

	quitAndIncomingDataEvents.CloseEvent();
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::AddInput(OutputType (*workerThreadCallback)(InputType, bool *returnOutput, void *perThreadData), InputType inputData)
{
	inputQueueMutex.lock();
	inputQueue.emplace_back(workerThreadCallback, inputData);
	inputCount.store(inputQueue.size(), std::memory_order_relaxed);
	inputQueueMutex.unlock();

	// The event is initialized only while the threads run (StartThreads to StopThreads); signalling it
	// before that used an uninitialized pthread condition (a hang on glibc). Input queued earlier is
	// picked up when the workers start.
	if (WasStarted())
		quitAndIncomingDataEvents.SetEvent();
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::AddOutput(OutputType outputData)
{
	outputQueueMutex.lock();
	outputQueue.push_back(outputData);
	outputCount.store(outputQueue.size(), std::memory_order_relaxed);
	outputQueueMutex.unlock();
}
template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::HasOutputFast(void)
{
	return outputCount.load(std::memory_order_relaxed) != 0;
}
template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::HasOutput(void)
{
	bool res;
	outputQueueMutex.lock();
	res = !outputQueue.empty();
	outputQueueMutex.unlock();
	return res;
}
template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::HasInputFast(void)
{
	return inputCount.load(std::memory_order_relaxed) != 0;
}
template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::HasInput(void)
{
	bool res;
	inputQueueMutex.lock();
	res = !inputQueue.empty();
	inputQueueMutex.unlock();
	return res;
}
template <class InputType, class OutputType>
OutputType ThreadPool<InputType, OutputType>::GetOutput(void)
{
	// Real output check
	OutputType output{};
	outputQueueMutex.lock();
	if (!outputQueue.empty())
	{
		output = outputQueue.front();
		outputQueue.pop_front();
		outputCount.store(outputQueue.size(), std::memory_order_relaxed);
	}
	outputQueueMutex.unlock();
	return output;
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::Clear(void)
{
	// Always under the queue locks: a stopped pool has no worker left, but taking them is cheap.
	inputQueueMutex.lock();
	ClearInput();
	inputQueueMutex.unlock();

	outputQueueMutex.lock();
	ClearOutput();
	outputQueueMutex.unlock();
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::LockInput(void)
{
	inputQueueMutex.lock();
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::UnlockInput(void)
{
	inputQueueMutex.unlock();
}
template <class InputType, class OutputType>
unsigned ThreadPool<InputType, OutputType>::InputSize(void)
{
	return static_cast<unsigned>(inputQueue.size());
}
template <class InputType, class OutputType>
InputType ThreadPool<InputType, OutputType>::GetInputAtIndex(unsigned index)
{
	return inputQueue[index].second;
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::RemoveInputAtIndex(unsigned index)
{
	inputQueue.erase(inputQueue.begin() + index);
	inputCount.store(inputQueue.size(), std::memory_order_relaxed);
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::LockOutput(void)
{
	outputQueueMutex.lock();
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::UnlockOutput(void)
{
	outputQueueMutex.unlock();
}
template <class InputType, class OutputType>
unsigned ThreadPool<InputType, OutputType>::OutputSize(void)
{
	return static_cast<unsigned>(outputQueue.size());
}
template <class InputType, class OutputType>
OutputType ThreadPool<InputType, OutputType>::GetOutputAtIndex(unsigned index)
{
	return outputQueue[index];
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::RemoveOutputAtIndex(unsigned index)
{
	outputQueue.erase(outputQueue.begin() + index);
	outputCount.store(outputQueue.size(), std::memory_order_relaxed);
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::ClearInput(void)
{
	inputQueue.clear();
	inputCount.store(0, std::memory_order_relaxed);
}

template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::ClearOutput(void)
{
	outputQueue.clear();
	outputCount.store(0, std::memory_order_relaxed);
}
template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::IsWorking(void)
{
	bool isWorking;
	//	workingThreadCountMutex.Lock();
	//	isWorking=numThreadsWorking!=0;
	//	workingThreadCountMutex.Unlock();

	//	if (isWorking)
	//		return true;

	// Bug fix: Originally the order of these two was reversed.
	// It's possible with the thread timing that working could have been false, then it picks up the data in the other thread, then it checks
	// here and sees there is no data.  So it thinks the thread is not working when it was.
	if (HasOutputFast() && HasOutput())
		return true;

	if (HasInputFast() && HasInput())
		return true;

	// Need to check is working again, in case the thread was between the first and second checks
	workingThreadCountMutex.Lock();
	isWorking = numThreadsWorking != 0;
	workingThreadCountMutex.Unlock();

	return isWorking;
}

template <class InputType, class OutputType>
int ThreadPool<InputType, OutputType>::NumThreadsWorking(void)
{
	return numThreadsWorking;
}

template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::WasStarted(void)
{
	bool b;
	runThreadsMutex.Lock();
	b = runThreads;
	runThreadsMutex.Unlock();
	return b;
}
template <class InputType, class OutputType>
bool ThreadPool<InputType, OutputType>::Pause(void)
{
	if (WasStarted() == false)
		return false;

	workingThreadCountMutex.Lock();
	while (numThreadsWorking > 0)
	{
		RakSleep(30);
	}
	return true;
}
template <class InputType, class OutputType>
void ThreadPool<InputType, OutputType>::Resume(void)
{
	workingThreadCountMutex.Unlock();
}

#endif

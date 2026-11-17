/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/signaled_event.h"

#include <chrono>

using namespace MafiaNet;

SignaledEvent::SignaledEvent() = default;
SignaledEvent::~SignaledEvent() = default;

void SignaledEvent::InitEvent(void)
{
	std::lock_guard<std::mutex> lock(mutex);
	isSignaled = false;
}

void SignaledEvent::CloseEvent(void)
{
	// The condition variable lives with the object. InitEvent clears any signal on reuse.
}

void SignaledEvent::SetEvent(void)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		isSignaled = true;
	}
	event.notify_one();
}

void SignaledEvent::WaitOnEvent(int timeoutMs)
{
	std::unique_lock<std::mutex> lock(mutex);
	const auto signaled = [this]
	{ return isSignaled; };
	if (timeoutMs == -1)
		event.wait(lock, signaled);
	else
		event.wait_for(lock, std::chrono::milliseconds(timeoutMs), signaled);
	isSignaled = false;
}

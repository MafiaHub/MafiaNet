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

#ifndef __SIGNALED_EVENT_H
#define __SIGNALED_EVENT_H

#include "mafianet/export.h"

#include <condition_variable>
#include <mutex>

namespace MafiaNet
{

/// An auto-reset event. Repeated signals coalesce until a waiter consumes one.
class RAK_DLL_EXPORT SignaledEvent
{
public:
	SignaledEvent();
	~SignaledEvent();

	void InitEvent(void);
	void CloseEvent(void);
	void SetEvent(void);
	/// Consume a pending signal or wait up to timeoutMs milliseconds. Use -1 to wait indefinitely.
	void WaitOnEvent(int timeoutMs);

private:
	std::mutex mutex;
	std::condition_variable event;
	bool isSignaled = false;
};

} // namespace MafiaNet

#endif

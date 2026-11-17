/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// The AddressSanitizer build itself: a call whose locals ASan puts in its use-after-return fake stack
// must hand that frame back when it returns. GCC on AArch64 does not, so after some 16k calls every
// call pays a scan of a full fake stack, which slowed the network thread enough to stall
// Rm3ReceiveThread. The top-level CMakeLists.txt turns the fake stack off there; this fails if a
// toolchain leaks fake frames and nothing turned it off.

#include <gtest/gtest.h>

#if defined(__SANITIZE_ADDRESS__)
#define MAFIANET_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MAFIANET_TEST_ASAN 1
#endif
#endif

#if defined(MAFIANET_TEST_ASAN) && (defined(__GNUC__) || defined(__clang__))
#include <sanitizer/asan_interface.h>

#include <thread>

namespace
{
// Whether this call's local was given a frame in the fake stack, rather than on the thread's stack.
__attribute__((noinline)) bool LocalIsInFakeStack()
{
	volatile int local = 0;
	void *fakeStack = __asan_get_current_fake_stack();
	return fakeStack != nullptr && __asan_addr_is_in_fake_stack(fakeStack, const_cast<int *>(&local), nullptr, nullptr) != nullptr;
}
} // namespace

TEST(AsanFakeStack, FramesAreReleasedOrNotUsed)
{
	// On a thread of its own, whose fake stack starts empty: this one may already have filled its own.
	bool usesFakeStack = false;
	bool stillUsesFakeStack = false;
	std::thread([&]()
	            {
		            usesFakeStack = LocalIsInFakeStack();
		            // Far more calls than a size class of a thread's fake stack has frames (2^14 for the smallest).
		            for (int i = 0; i < (1 << 17); i++)
			            LocalIsInFakeStack();
		            stillUsesFakeStack = LocalIsInFakeStack(); })
	    .join();
	if (usesFakeStack)
		EXPECT_TRUE(stillUsesFakeStack) << "the fake stack filled up: returning calls do not release their fake frames";
}
#else
TEST(AsanFakeStack, FramesAreReleasedOrNotUsed)
{
	GTEST_SKIP() << "not a GCC or Clang AddressSanitizer build";
}
#endif

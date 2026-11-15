/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Compile-time checks on HTTPConnection2::TransmitRequest. Its `bool useSSL`
 *  parameter was removed with HTTPS; without a guard a call written for the old
 *  signature would still compile, with the bool bound to `int ipVersion`.
 */

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "mafianet/http_connection2.h"

#if _RAKNET_SUPPORT_HTTPConnection2 == 1 && _RAKNET_SUPPORT_TCPInterface == 1

namespace
{

// Detection idiom: whether TransmitRequest(const char *, const char *, unsigned short, Args...) is well-formed.
template <typename Void, typename... Args>
struct CanTransmitImpl : std::false_type
{
};

template <typename... Args>
struct CanTransmitImpl<decltype((void) std::declval<MafiaNet::HTTPConnection2 &>().TransmitRequest(
                           std::declval<const char *>(), std::declval<const char *>(), std::declval<Args>()...)),
                       Args...> : std::true_type
{
};

template <typename... Args>
constexpr bool CanTransmit = CanTransmitImpl<void, Args...>::value;

// The calls that keep working.
static_assert(CanTransmit<>, "TransmitRequest(request, host) must compile");
static_assert(CanTransmit<unsigned short>, "TransmitRequest(request, host, port) must compile");
static_assert(CanTransmit<int>, "TransmitRequest(request, host, 80) must compile");
static_assert(CanTransmit<unsigned short, int>, "TransmitRequest(request, host, port, ipVersion) must compile");
static_assert(CanTransmit<unsigned short, int, MafiaNet::SystemAddress, void *>,
              "TransmitRequest with every argument must compile");

// The old useSSL form must not.
static_assert(!CanTransmit<unsigned short, bool>, "TransmitRequest(request, host, port, true) must not compile");
static_assert(!CanTransmit<int, bool>, "TransmitRequest(request, host, 443, true) must not compile");
static_assert(!CanTransmit<unsigned short, bool, MafiaNet::SystemAddress>,
              "TransmitRequest(request, host, port, useSSL, address) must not compile");

} // namespace

// The checks above run at compile time; this case only reports them under ctest.
TEST(HttpConnection2Api, BoolUseSslFormIsRejectedAtCompileTime)
{
	SUCCEED();
}

#endif

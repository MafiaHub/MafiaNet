// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace MafiaNet
{
namespace detail
{
// The body of a session rejection or a disconnect notification (the bytes after the message ID).
// With a zero code it is the detail alone, which is what raw RejectSession() and CloseConnection()
// callers send and read. A nonzero code, a detail that starts with a zero byte, or a rejection that
// must not look like an unanswered attempt is framed: a zero byte, the code as four bytes in network
// byte order, then the detail. A RejectSession() reason is a C string and never starts with a zero byte.
constexpr std::size_t kFramedReasonBytes = 5;

inline std::string EncodeReason(uint32_t code, std::string_view detail, bool framed = false)
{
	if (code == 0 && !framed && (detail.empty() || detail.front() != '\0'))
		return std::string(detail);
	std::string body(kFramedReasonBytes, '\0');
	for (std::size_t i = 0; i < 4; ++i)
		body[1 + i] = static_cast<char>((code >> (24 - 8 * i)) & 0xFF);
	body.append(detail.data(), detail.size());
	return body;
}

struct Reason
{
	uint32_t code = 0;
	std::string detail;
};

inline Reason DecodeReason(const unsigned char *body, std::size_t length)
{
	if (length < kFramedReasonBytes || body[0] != 0)
		return {0, std::string(reinterpret_cast<const char *>(body), length)};
	uint32_t code = 0;
	for (std::size_t i = 1; i < kFramedReasonBytes; ++i)
		code = (code << 8) | body[i];
	return {code, std::string(reinterpret_cast<const char *>(body) + kFramedReasonBytes, length - kFramedReasonBytes)};
}
}
}

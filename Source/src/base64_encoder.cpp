/*
 *  Copyright (c) 2014, Oculus VR, Inc.
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

#include "mafianet/base64_encoder.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <sodium.h>

namespace
{
using MafiaNet::Base64Format;
constexpr int variant = sodium_base64_VARIANT_ORIGINAL;
constexpr size_t mimeLineBytes = 57; // Each three input bytes produce four columns.

std::optional<size_t> EncodedSize(size_t byteCount, Base64Format format, size_t limit)
{
	if (byteCount > limit / 4 * 3)
		return std::nullopt;
	const size_t characters = sodium_base64_encoded_len(byteCount, variant) - 1;
	if (format != Base64Format::Mime)
		return characters;
	const size_t lines = byteCount == 0 ? 1 : 1 + (byteCount - 1) / mimeLineBytes;
	if (lines > (limit - characters) / 2)
		return std::nullopt;
	return characters + 2 * lines;
}

// The destination has EncodedSize + 1 bytes. libsodium supplies the final NUL for standard output.
void EncodeInto(const unsigned char *bytes, size_t byteCount, Base64Format format, char *output)
{
	for (;;)
	{
		const size_t count = format == Base64Format::Mime ? std::min(byteCount, mimeLineBytes) : byteCount;
		const size_t capacity = sodium_base64_encoded_len(count, variant);
		sodium_bin2base64(output, capacity, bytes, count, variant);
		if (format != Base64Format::Mime)
			return;
		output += capacity - 1;
		*output++ = '\r';
		*output++ = '\n';
		if (count == byteCount)
		{
			*output = '\0';
			return;
		}
		bytes += count;
		byteCount -= count;
	}
}
} // namespace

std::string MafiaNet::EncodeBase64(std::string_view bytes, Base64Format format)
{
	if (sodium_init() < 0)
		throw std::runtime_error("Could not initialize libsodium");
	std::string result;
	const auto size = EncodedSize(bytes.size(), format, result.max_size());
	if (!size)
		throw std::length_error("Base64 output exceeds maximum string length");
	result.resize(*size);
	EncodeInto(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), format, result.data());
	return result;
}

std::optional<std::string> MafiaNet::DecodeBase64(std::string_view text, Base64Format format)
{
	if (sodium_init() < 0)
		throw std::runtime_error("Could not initialize libsodium");
	if (text.empty())
		return std::string{};
	// libsodium also matches the terminator of a non-null ignore string.
	if (format == Base64Format::Mime && text.find('\0') != std::string_view::npos)
		return std::nullopt;
	std::string bytes(text.size() / 4 * 3, '\0');
	size_t written;
	const char *ignore = format == Base64Format::Mime ? " \t\r\n" : nullptr;
	if (sodium_base642bin(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size(), text.data(), text.size(), ignore, &written, nullptr, variant) != 0)
		return std::nullopt;
	bytes.resize(written);
	return bytes;
}

const char *Base64Map(void)
{
	return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

int Base64Encoding(const unsigned char *inputData, int dataLength, char *outputData)
{
	if (dataLength < 0 || (!inputData && dataLength != 0) || !outputData || sodium_init() < 0)
		return -1;
	const auto size = EncodedSize(static_cast<size_t>(dataLength), Base64Format::Mime, std::numeric_limits<int>::max());
	if (!size)
		return -1;
	EncodeInto(inputData, static_cast<size_t>(dataLength), Base64Format::Mime, outputData);
	return static_cast<int>(*size);
}

int Base64Encoding(const unsigned char *inputData, int dataLength, char **outputData)
{
	if (!outputData)
		return -1;
	*outputData = nullptr;
	if (dataLength < 0 || (!inputData && dataLength != 0) || sodium_init() < 0)
		return -1;
	const auto size = EncodedSize(static_cast<size_t>(dataLength), Base64Format::Mime, std::numeric_limits<int>::max());
	if (!size)
		return -1;
	*outputData = static_cast<char *>(std::malloc(*size + 1));
	if (!*outputData)
		return -1;
	EncodeInto(inputData, static_cast<size_t>(dataLength), Base64Format::Mime, *outputData);
	return static_cast<int>(*size);
}

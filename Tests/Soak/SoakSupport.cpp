/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "SoakSupport.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

// Under a sanitizer, operator new stays the runtime's (see below).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SOAK_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SOAK_SANITIZER 1
#endif
#endif

#ifndef SOAK_SANITIZER
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h> // malloc_usable_size, or _msize on Windows
#endif
#endif

#include "mafianet/memory_override.h"

namespace
{
std::atomic<int64_t> g_newBlocks{0};
std::atomic<int64_t> g_newBytes{0};
std::atomic<int64_t> g_rakBlocks{0};

#ifndef SOAK_SANITIZER
size_t BlockSize(void *p)
{
#if defined(__APPLE__)
	return malloc_size(p);
#elif defined(_WIN32)
	return _msize(p);
#else
	return malloc_usable_size(p);
#endif
}
#endif

void *CountedMallocEx(size_t size, const char *, unsigned int)
{
	void *p = std::malloc(size);
	if (p != nullptr)
		g_rakBlocks.fetch_add(1, std::memory_order_relaxed);
	return p;
}

void *CountedReallocEx(void *p, size_t size, const char *, unsigned int)
{
	void *q = std::realloc(p, size);
	if (p == nullptr && q != nullptr)
		g_rakBlocks.fetch_add(1, std::memory_order_relaxed);
	else if (p != nullptr && size == 0 && q == nullptr)
		g_rakBlocks.fetch_sub(1, std::memory_order_relaxed);
	return q;
}

void CountedFreeEx(void *p, const char *, unsigned int)
{
	if (p != nullptr)
		g_rakBlocks.fetch_sub(1, std::memory_order_relaxed);
	std::free(p);
}
} // namespace

// Replaces the global allocation functions for the whole SoakTests binary: every non-aligned form (the
// aligned forms keep the library's, and pair among themselves). Not under a sanitizer: its runtime defines
// operator new itself (TSan's cannot be replaced at all), and it tracks every allocation anyway; only the
// rakMalloc hooks count there.
#ifndef SOAK_SANITIZER
namespace
{
void *CountedNew(size_t size) noexcept
{
	void *p = std::malloc(size ? size : 1);
	if (p != nullptr)
	{
		g_newBlocks.fetch_add(1, std::memory_order_relaxed);
		g_newBytes.fetch_add((int64_t) BlockSize(p), std::memory_order_relaxed);
	}
	return p;
}

void CountedDelete(void *p) noexcept
{
	if (p == nullptr)
		return;
	g_newBlocks.fetch_sub(1, std::memory_order_relaxed);
	g_newBytes.fetch_sub((int64_t) BlockSize(p), std::memory_order_relaxed);
	std::free(p);
}

void *CountedNewOrThrow(size_t size)
{
	void *p = CountedNew(size);
	if (p == nullptr)
		throw std::bad_alloc();
	return p;
}
} // namespace

void *operator new(size_t size)
{
	return CountedNewOrThrow(size);
}
void *operator new[](size_t size)
{
	return CountedNewOrThrow(size);
}
void *operator new(size_t size, const std::nothrow_t &) noexcept
{
	return CountedNew(size);
}
void *operator new[](size_t size, const std::nothrow_t &) noexcept
{
	return CountedNew(size);
}
void operator delete(void *p) noexcept
{
	CountedDelete(p);
}
void operator delete[](void *p) noexcept
{
	CountedDelete(p);
}
void operator delete(void *p, size_t) noexcept
{
	CountedDelete(p);
}
void operator delete[](void *p, size_t) noexcept
{
	CountedDelete(p);
}
void operator delete(void *p, const std::nothrow_t &) noexcept
{
	CountedDelete(p);
}
void operator delete[](void *p, const std::nothrow_t &) noexcept
{
	CountedDelete(p);
}
#endif

namespace Soak
{
void WriteOrdered(MafiaNet::BitStream &out, uint64_t session, uint32_t sequence, uint16_t extraBytes)
{
	out.Write(ID_SOAK_ORDERED);
	out.Write(session);
	out.Write(sequence);
	out.Write(extraBytes);
	for (uint32_t i = 0; i < extraBytes; ++i)
		out.Write(ExtraByte(session, sequence, i));
}

void OrderChecker::Violation(const std::string &what)
{
	++violations;
	if (examples.size() < 20)
		examples.push_back(what);
}

void OrderChecker::OnOrdered(int receiver, int sender, const unsigned char *data, size_t length)
{
	MafiaNet::BitStream in(const_cast<unsigned char *>(data), (unsigned int) length, false);
	in.IgnoreBytes(1);
	uint64_t session = 0;
	uint32_t sequence = 0;
	uint16_t extraBytes = 0;
	char where[96];
	std::snprintf(where, sizeof(where), "%d<-%d ", receiver, sender);
	if (!in.Read(session) || !in.Read(sequence) || !in.Read(extraBytes) || in.GetNumberOfUnreadBits() != extraBytes * 8u)
	{
		Violation(std::string(where) + "malformed ordered message");
		return;
	}
	for (uint32_t i = 0; i < extraBytes; ++i)
	{
		unsigned char b = 0;
		in.Read(b);
		if (b != ExtraByte(session, sequence, i))
		{
			Violation(std::string(where) + "corrupt payload at byte " + std::to_string(i) + " of sequence " + std::to_string(sequence));
			return;
		}
	}
	++delivered;
	RecvStream &stream = streams[{receiver, sender}];
	if (session != stream.session)
	{
		// A new connection: its first ordered message must be the first one sent on it.
		if (sequence != 0)
			Violation(std::string(where) + "new session starts at " + std::to_string(sequence));
		stream.session = session;
		stream.expected = sequence + 1;
		return;
	}
	if (sequence != stream.expected)
		Violation(std::string(where) + "expected " + std::to_string(stream.expected) + " got " + std::to_string(sequence));
	stream.expected = sequence + 1;
}

const RecvStream *OrderChecker::Stream(int receiver, int sender) const
{
	auto it = streams.find({receiver, sender});
	return it == streams.end() ? nullptr : &it->second;
}

bool CountsOperatorNew()
{
#ifdef SOAK_SANITIZER
	return false;
#else
	return true;
#endif
}

void InstallRakAllocatorHooks()
{
	SetMalloc_Ex(CountedMallocEx);
	SetRealloc_Ex(CountedReallocEx);
	SetFree_Ex(CountedFreeEx);
}

AllocationSnapshot Allocations()
{
	AllocationSnapshot s;
	s.newBlocks = g_newBlocks.load(std::memory_order_relaxed);
	s.newBytes = g_newBytes.load(std::memory_order_relaxed);
	s.rakBlocks = g_rakBlocks.load(std::memory_order_relaxed);
	return s;
}

uint32_t EnvUInt(const char *name, uint32_t fallback)
{
	const char *value = std::getenv(name);
	if (value == nullptr || *value == '\0')
		return fallback;
	char *end = nullptr;
	const unsigned long parsed = std::strtoul(value, &end, 0);
	return (end != nullptr && *end == '\0') ? (uint32_t) parsed : fallback;
}
} // namespace Soak

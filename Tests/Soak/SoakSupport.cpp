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

// The hooks are installed before main(), so before any network thread exists to read them: swapping
// rakMalloc_Ex and friends while a peer's thread runs would be a data race.
struct RakAllocatorHooks
{
	RakAllocatorHooks()
	{
		SetMalloc_Ex(CountedMallocEx);
		SetRealloc_Ex(CountedReallocEx);
		SetFree_Ex(CountedFreeEx);
	}
} g_rakAllocatorHooks;
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
namespace
{
void WriteNumbered(MafiaNet::BitStream &out, MafiaNet::MessageID id, uint64_t session, uint32_t sequence, uint16_t extraBytes)
{
	out.Write(id);
	out.Write(session);
	out.Write(sequence);
	out.Write(extraBytes);
	for (uint32_t i = 0; i < extraBytes; ++i)
		out.Write(ExtraByte(session, sequence, i));
}

// Reads a message WriteNumbered wrote and checks its extra bytes. Returns an empty string, or what is wrong.
std::string ReadNumbered(const unsigned char *data, size_t length, uint64_t *session, uint32_t *sequence)
{
	MafiaNet::BitStream in(const_cast<unsigned char *>(data), (unsigned int) length, false);
	in.IgnoreBytes(1);
	uint16_t extraBytes = 0;
	if (!in.Read(*session) || !in.Read(*sequence) || !in.Read(extraBytes) || in.GetNumberOfUnreadBits() != extraBytes * 8u)
		return "malformed message";
	for (uint32_t i = 0; i < extraBytes; ++i)
	{
		unsigned char b = 0;
		in.Read(b);
		if (b != ExtraByte(*session, *sequence, i))
			return "corrupt payload at byte " + std::to_string(i) + " of sequence " + std::to_string(*sequence);
	}
	return std::string();
}
} // namespace

void WriteOrdered(MafiaNet::BitStream &out, uint64_t session, uint32_t sequence, uint16_t extraBytes)
{
	WriteNumbered(out, ID_SOAK_ORDERED, session, sequence, extraBytes);
}

void WriteReliable(MafiaNet::BitStream &out, uint64_t session, uint32_t sequence, uint16_t extraBytes)
{
	WriteNumbered(out, ID_SOAK_RELIABLE, session, sequence, extraBytes);
}

void OrderChecker::Violation(const std::string &what)
{
	++violations;
	if (examples.size() < 20)
		examples.push_back(what);
}

void OrderChecker::OnOrdered(int receiver, int sender, const unsigned char *data, size_t length)
{
	uint64_t session = 0;
	uint32_t sequence = 0;
	char where[96];
	std::snprintf(where, sizeof(where), "%d<-%d ", receiver, sender);
	const std::string wrong = ReadNumbered(data, length, &session, &sequence);
	if (!wrong.empty())
	{
		Violation(std::string(where) + "ordered: " + wrong);
		return;
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

void ExactlyOnceChecker::Violation(const std::string &what)
{
	++violations;
	if (examples.size() < 20)
		examples.push_back(what);
}

void ExactlyOnceChecker::OnReliable(int receiver, int sender, const unsigned char *data, size_t length)
{
	uint64_t session = 0;
	uint32_t sequence = 0;
	char where[96];
	std::snprintf(where, sizeof(where), "%d<-%d ", receiver, sender);
	const std::string wrong = ReadNumbered(data, length, &session, &sequence);
	if (!wrong.empty())
	{
		Violation(std::string(where) + "reliable: " + wrong);
		return;
	}
	++delivered;
	Stream &stream = streams[{receiver, sender}];
	if (session != stream.session)
	{
		// A new connection: the old one's messages can no longer arrive.
		stream.session = session;
		stream.received = 0;
		stream.arrived.clear();
	}
	if (sequence >= stream.arrived.size())
		stream.arrived.resize((size_t) sequence + 1, false);
	if (stream.arrived[sequence])
	{
		Violation(std::string(where) + "reliable sequence " + std::to_string(sequence) + " delivered twice");
		return;
	}
	stream.arrived[sequence] = true;
	++stream.received;
}

const ExactlyOnceChecker::Stream *ExactlyOnceChecker::Find(int receiver, int sender) const
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

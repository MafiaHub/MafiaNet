/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file Archive.h
/// \brief A single serialization convention over BitStream (issue #19).
///
/// \details A user type describes its wire format once with a member template:
/// \code
/// struct ChatMessage {
///     std::string text;
///     int channel;
///     template <class Ar> void serialize(Ar& ar) { ar & text & channel; }
/// };
/// \endcode
///
/// and reads/writes it through an archive adapter, without duplicating the
/// field list for each direction:
/// \code
/// MafiaNet::BitStream bs;
/// ChatMessage msg{ "hi", 3 };
/// MafiaNet::WriteArchive(bs) & msg;          // write
///
/// MafiaNet::BitStream in(bs.GetData(), bs.GetNumberOfBytesUsed(), false);
/// ChatMessage got;
/// MafiaNet::ReadArchive(in) & got;           // read; got == msg
/// \endcode
///
/// This header is *only* the wire convention. Message-ID assignment and
/// dispatch are out of scope (issue #20).
///
/// The archives reuse BitStream's existing operator<< / operator>> and its
/// per-type specializations (std::string, SystemAddress, RakNetGUID, ...), so
/// primitives are never reimplemented here. A field whose type has its own
/// serialize() recurses through the archive; any other field falls through to
/// `bs << field` / `bs >> field`, which also picks up a user-provided
/// operator<< / operator>> for that type.

#ifndef __MAFIANET_ARCHIVE_H
#define __MAFIANET_ARCHIVE_H

#include "BitStream.h"

#include <type_traits>
#include <utility>

namespace MafiaNet
{
	/// Detects whether \a T has a member `serialize(Ar&)` callable with archive
	/// type \a Ar. When true the archive recurses into that type; otherwise it
	/// defers to BitStream's operator<< / operator>>.
	template <class T, class Ar, class = void>
	struct HasSerialize : std::false_type {};

	template <class T, class Ar>
	struct HasSerialize<T, Ar,
		std::void_t<decltype(std::declval<T &>().serialize(std::declval<Ar &>()))> >
		: std::true_type {};

	/// Writes user types to a BitStream. `ar & field` serializes \a field: if the
	/// field's type has a serialize() it recurses, otherwise it is written via
	/// `bs << field`.
	class WriteArchive
	{
		BitStream &bs;

	public:
		/// True on the writing archive; lets a serialize() branch on direction.
		static constexpr bool IsWriting = true;
		static constexpr bool IsReading = false;

		explicit WriteArchive(BitStream &stream) : bs(stream) {}

		template <class T>
		WriteArchive &operator&(const T &value)
		{
			if constexpr (HasSerialize<T, WriteArchive>::value)
			{
				// One serialize() serves both directions; the write path only
				// reads the members, so calling the non-const serialize() on a
				// const value is safe.
				const_cast<T &>(value).serialize(*this);
			}
			else
			{
				bs << value;
			}
			return *this;
		}

		/// Access the underlying stream (e.g. to write a header before the body).
		BitStream &stream() { return bs; }
	};

	/// Reads user types from a BitStream. `ar & field` deserializes \a field: if
	/// the field's type has a serialize() it recurses, otherwise it is read via
	/// `bs >> field`.
	class ReadArchive
	{
		BitStream &bs;

	public:
		static constexpr bool IsWriting = false;
		static constexpr bool IsReading = true;

		explicit ReadArchive(BitStream &stream) : bs(stream) {}

		template <class T>
		ReadArchive &operator&(T &value)
		{
			if constexpr (HasSerialize<T, ReadArchive>::value)
			{
				value.serialize(*this);
			}
			else
			{
				bs >> value;
			}
			return *this;
		}

		/// Access the underlying stream (e.g. to read a header before the body).
		BitStream &stream() { return bs; }
	};
}

#endif // __MAFIANET_ARCHIVE_H

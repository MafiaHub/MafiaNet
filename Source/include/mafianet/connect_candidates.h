/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file connect_candidates.h
/// \brief Ordering of the addresses a host name resolved to, for racing them
/// in Connect() on a dual-stack socket (RFC 8305, "Happy Eyeballs").
///
/// Portable and compiled everywhere, so it is unit tested on every platform.

#ifndef __MAFIANET_CONNECT_CANDIDATES_H
#define __MAFIANET_CONNECT_CANDIDATES_H

#include "mafianet/export.h"
#include "mafianet/types.h"

namespace MafiaNet
{

/// Reorders \a addresses in place into the order Connect() tries them
/// (RFC 8305 section 4, with a "First Address Family Count" of 1): the first
/// address keeps its place, then the families alternate, starting with the
/// other family. Addresses of the same family keep their relative (resolver)
/// order; once one family runs out, the rest of the other follows. Exact
/// duplicates (same address and port) are dropped, keeping the first.
///
/// Returns the new count, at most \a count. \a addresses may be null when
/// \a count is 0.
RAK_DLL_EXPORT int OrderConnectCandidates(SystemAddress *addresses, int count);

} // namespace MafiaNet

#endif // __MAFIANET_CONNECT_CANDIDATES_H

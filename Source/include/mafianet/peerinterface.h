/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 *
 * Deprecated include path. "mafianet/peerinterface.h" was renamed to "mafianet/RakPeerInterface.h" so
 * every header follows the PascalCase name of the class it declares. This
 * forwarding header is kept for one release cycle and will then be removed.
 * Define MAFIANET_SILENCE_DEPRECATED_INCLUDES to suppress the message.
 */
#pragma once
#ifndef MAFIANET_SILENCE_DEPRECATED_INCLUDES
#pragma message("mafianet/peerinterface.h is deprecated; include mafianet/RakPeerInterface.h instead")
#endif
#include "RakPeerInterface.h"

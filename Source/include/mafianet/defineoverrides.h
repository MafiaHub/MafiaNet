/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 *
 * Deprecated include path. "mafianet/defineoverrides.h" was renamed to "mafianet/RakNetDefinesOverrides.h" so
 * every header follows the PascalCase name of the class it declares. This
 * forwarding header is kept for one release cycle and will then be removed.
 * Define MAFIANET_SILENCE_DEPRECATED_INCLUDES to suppress the message.
 */
#pragma once
#ifndef MAFIANET_SILENCE_DEPRECATED_INCLUDES
#pragma message("mafianet/defineoverrides.h is deprecated; include mafianet/RakNetDefinesOverrides.h instead")
#endif
#include "RakNetDefinesOverrides.h"

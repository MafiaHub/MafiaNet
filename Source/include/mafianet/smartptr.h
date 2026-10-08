/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 *
 * Deprecated include path. "mafianet/smartptr.h" was renamed to "mafianet/RakNetSmartPtr.h" so
 * every header follows the PascalCase name of the class it declares. This
 * forwarding header is kept for one release cycle and will then be removed.
 * Define MAFIANET_SILENCE_DEPRECATED_INCLUDES to suppress the message.
 */
#pragma once
#ifndef MAFIANET_SILENCE_DEPRECATED_INCLUDES
#pragma message("mafianet/smartptr.h is deprecated; include mafianet/RakNetSmartPtr.h instead")
#endif
#include "RakNetSmartPtr.h"

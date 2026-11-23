// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/time.h"

namespace MafiaNet
{
// Monotonic microseconds from an unspecified origin, or the installed simulation clock.
TimeUS GetSteadyTimeUS();
}

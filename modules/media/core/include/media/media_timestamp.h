#pragma once
#include "media/media_packet.h"
// Nearest rounding, ties away from zero. Invalid/overflow returns kNoTimestamp.
std::int64_t RescaleMediaTimestamp(std::int64_t, Rational source, Rational target) noexcept;

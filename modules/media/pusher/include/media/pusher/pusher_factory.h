#pragma once
#include <memory>
#include "media/pusher/i_pusher.h"
std::unique_ptr<IPusher> CreateMediaPusher();

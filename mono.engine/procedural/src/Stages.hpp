#pragma once

#include <cstdint>

namespace engine::procedural::detail {
	enum class Stage : uint8_t { Seed, VillageSources, Entrance, Road, VillageLots, Forest };
}

#include <engine/scene/Audio.hpp>

#include <array>
#include <string_view>

namespace engine::scene {

	namespace {
		// The names, in ordinal order, from one list.
		//
		// **One table rather than a switch**, which is `Input.cpp`'s argument: a
		// name is registered into `ecs::EnumTable` from here and compared against
		// by a script, so the table and the enum have to stay in step and a
		// `static_assert` is what makes that a checked claim.
		constexpr std::array<std::string_view, static_cast<size_t>(ListenerMode::Count)> MODE_NAMES{{
			"Camera",
			"ObjectPosition",
		}};

		static_assert(MODE_NAMES.size() == static_cast<size_t>(ListenerMode::Count));
	}

	const char *Describe(ListenerMode mode) {
		const auto index = static_cast<size_t>(mode);
		return index < MODE_NAMES.size() ? MODE_NAMES[index].data() : MODE_NAMES[0].data();
	}
}

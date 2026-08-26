// The parts of the datatype vocabulary that are not a VM.
//
// **Three functions and one table, and the reason they are here rather than
// beside the Luau constructors is the module split.** They were in
// `LuauDatatypes.cpp` because that is where the table was first written, and the
// filename check that used to guard this directory was happy with that: the file
// meets `lua.h`, so it is named for the VM. `scriptjs` calls all three anyway,
// and once the two adapters became two libraries neither could reach the other's
// object file.
//
// So the neutral half moved down to `script`, which is where the header that
// declares it has always lived. `mono.tools/bindings` is the third caller and
// opens no VM at all.
//
// @tier L9 · shared

#include <engine/core/types/TweenInfo.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/scene/Components.hpp>
#include <engine/script/Datatypes.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace engine::script {

	namespace {
		// One member of `Enum.Axis`: the name a script spells and the direction
		// it means.
		struct AxisEntry {
			std::string_view Name;
			core::Vector3 Direction;
		};

		// The members of `Enum.Axis`, in Roblox's ordinal order.
		//
		// **One array rather than a member list here and a switch over there.**
		// The registration below and `DirectionOfAxis` are two readings of the
		// same three facts, and the second copy is always the one that goes
		// stale. Function-local, so it is built on first use and no other
		// translation unit's start-up can reach it before `Vector3::XAxis` has
		// a value.
		const std::array<AxisEntry, 3> &Axes() {
			static const std::array<AxisEntry, 3> AXES{{
				{"X", core::Vector3::XAxis},
				{"Y", core::Vector3::YAxis},
				{"Z", core::Vector3::ZAxis},
			}};
			return AXES;
		}
	}

	// The three enums this vocabulary needs.
	//
	// **Here rather than in each VM's open, because there are three callers.**
	// Both surfaces consume them through `TweenInfo`, and `mono.tools/bindings`
	// needs them without opening a VM at all - see `script/Datatypes.hpp` for
	// what having written the list twice actually cost.
	//
	// `Axis` is here for the same reason and one more: `Vector3.FromAxis` is the
	// only thing that names it, and no world registers it - so a script in a
	// process that never built a scene would otherwise be told `Axis` is not an
	// enum this engine registers.
	void RegisterDatatypeEnums() {
		// **Walked off the enum rather than typed out**, which is the rule the
		// comment above is about and which this pair broke: `gui` registers the
		// same two sets for `UIPageLayout`, and two literal lists would be two
		// orderings that agree until somebody adds a curve to one of them. The
		// ordinal is the storage, so the order is not a detail.
		std::array<std::string_view, core::EASING_STYLE_COUNT> styles{};
		for (size_t index = 0; index < styles.size(); index++) {
			styles[index] = core::Describe(static_cast<core::EasingStyle>(index));
		}

		std::array<std::string_view, core::EASING_DIRECTION_COUNT> directions{};
		for (size_t index = 0; index < directions.size(); index++) {
			directions[index] = core::Describe(static_cast<core::EasingDirection>(index));
		}

		std::array<std::string_view, 3> axes{};
		for (size_t index = 0; index < axes.size(); index++) {
			axes[index] = Axes()[index].Name;
		}

		ecs::EnumTable::Register("EasingStyle", styles);
		ecs::EnumTable::Register("EasingDirection", directions);
		ecs::EnumTable::Register("Axis", axes);
	}

	bool DirectionOfNormalId(core::Name member, core::Vector3 &out) {
		size_t ordinal = 0;
		if (!ecs::EnumTable::OrdinalOf(core::Name("NormalId"), member, ordinal)) {
			return false;
		}

		// A game may register a member of its own onto any enum, and
		// `scene::NormalOf` has six faces and a fallback rather than an answer
		// for a seventh. Refusing here says so; falling through would hand back
		// `Front` for a name that means nothing.
		if (ordinal > static_cast<size_t>(scene::NormalId::Front)) {
			return false;
		}

		out = scene::NormalOf(static_cast<scene::NormalId>(ordinal));
		return true;
	}

	bool DirectionOfAxis(core::Name member, core::Vector3 &out) {
		for (const AxisEntry &axis : Axes()) {
			// The text rather than a `core::Name` built from it: interning takes
			// the registry's mutex, and three of them per call to answer a
			// question a string comparison already answers.
			if (member.Text() == axis.Name) {
				out = axis.Direction;
				return true;
			}
		}
		return false;
	}

}

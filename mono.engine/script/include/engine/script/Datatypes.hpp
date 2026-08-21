#pragma once

// The datatype vocabulary's enums, registered once for both VMs and for the
// tool that describes them.
//
// **Two VMs and a generator need the same two enums, and each used to register
// its own copy.** `EasingStyle` and `EasingDirection` are consumed by
// `TweenInfo` in Luau and in JavaScript, so both surfaces registered the member
// list at VM-open time - the same eleven strings written out twice. Registering
// twice is agreement rather than conflict, which is exactly why the second copy
// went unnoticed: nothing failed, and one list could quietly gain a member the
// other did not.
//
// The third consumer is what made it worth fixing. `mono.tools/bindings`
// generates the declaration files from `ecs::EnumTable`, and it opens no VM -
// so `Enum.EasingStyle` was absent from the manifest and from both declaration
// files, and a script naming it had no completion for a value the run time
// accepts.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Vector3.hpp>

namespace engine::script {

	// Registers `EasingStyle`, `EasingDirection` and `Axis` with `ecs::EnumTable`.
	//
	// Process-wide and idempotent, like every other registration in this engine.
	// Call it before anything reads the enum table: both VMs do it while opening
	// their datatypes, and the bindings generator does it before writing the
	// manifest.
	//
	// @since v0.7
	void RegisterDatatypeEnums();

	// The outward direction an `Enum.NormalId` member names.
	//
	// What `Vector3.FromNormalId` is, in the one place both VMs can call. The
	// face order is `ecs::EnumTable`'s and the direction is `scene::NormalOf`'s,
	// so this adds no third copy of either - which matters because `Front` is
	// **-Z** and a hand-written table is where that gets flipped.
	//
	// @param member The member's name, `Top` or `Front` and so on.
	// @param out    Filled with the outward unit normal when this returns true.
	// @return `false` when nothing has registered `NormalId` or it holds no such
	//         member, leaving `out` untouched.
	bool DirectionOfNormalId(core::Name member, core::Vector3 &out);

	// The unit vector an `Enum.Axis` member names.
	//
	// What `Vector3.FromAxis` is. `Axis` is registered by
	// `RegisterDatatypeEnums` rather than by a world, so this answers in a
	// process that never built a scene.
	//
	// @param member The member's name: `X`, `Y` or `Z`.
	// @param out    Filled with the axis when this returns true.
	// @return `false` when the member names no axis, leaving `out` untouched.
	bool DirectionOfAxis(core::Name member, core::Vector3 &out);
}

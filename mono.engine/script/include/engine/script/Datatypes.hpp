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

namespace engine::script {

	// Registers `EasingStyle` and `EasingDirection` with `ecs::EnumTable`.
	//
	// Process-wide and idempotent, like every other registration in this engine.
	// Call it before anything reads the enum table: both VMs do it while opening
	// their datatypes, and the bindings generator does it before writing the
	// manifest.
	//
	// @since v0.7
	void RegisterDatatypeEnums();
}

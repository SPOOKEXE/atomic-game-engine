#pragma once

// Names the closed set of property value types, for headers that switch on or
// store one without needing the class registry.
//
// `Classes.hpp` brings in class registration, component sets and the component
// table. A header that only takes or keeps a `PropertyType` includes this
// instead, and forward-declares `PropertyDescriptor` when it passes one by
// reference.
//
// @tier L3 · shared

#include <cstdint>

namespace engine::ecs {

	// What a property is, for a binding or an editor that has to type it.
	//
	// Deliberately small and closed. A property type that needs a case here is
	// a decision about what userland can hold, not an implementation detail -
	// so adding one is a visible change rather than a template specialisation
	// somebody slips in.
	//
	// @since v0.2
	enum class PropertyType : uint8_t {
		// Anything with no better description. Readable as bytes, not as a value.
		Opaque,

		Bool,
		Int32,
		Int64,
		Float,
		Double,

		// `core::Name` - written as text, never as its process-local id.
		//
		// **For text drawn from a bounded set**, which is what interning is for:
		// a material, an asset id, a class name. Every distinct string ever
		// assigned to one of these is kept for the life of the process, so a
		// property whose value a game *computes* is the wrong shape for it - see
		// `String` below, which exists because that was found the hard way.
		Name,

		// A `core::Name` that must be one of a registered set.
		//
		// **The storage is identical to `Name` and the contract is not.** A
		// `Name` property takes any string at all, so `part.Material =
		// "Plsatic"` lands in the component and surfaces as a part rendering
		// with the default material for reasons nobody can see. An `Enum`
		// property is checked against `EnumTable`, so the typo is refused where
		// it was made, and a binding can offer completion because the set is
		// readable at run time.
		//
		// Which set is named by `PropertyDescriptor::EnumName`. Adding this was
		// a decision about what userland can hold rather than an implementation
		// detail, which is exactly why `PropertyType` is a closed list.
		Enum,

		// An owned string, stored in the component rather than interned.
		//
		// **Added at v0.8 because `Name` is a leak for text that changes**, which
		// is `D00020` and is the reason this member is not simply "the same as
		// `Name` with a different spelling". `core::Name` is a process-wide
		// registry that never releases: `label.Text = tostring(score)` at sixty
		// hertz interns a new string every frame, forever, and takes that
		// registry's mutex inside the frame loop to do it. A score counter is not
		// an exotic case - it is the first thing anybody writes.
		//
		// So the two are a real choice rather than a stylistic one, and the rule
		// is short: **a value the game picks from a set is a `Name`; a value the
		// game computes is a `String`.** A material, an asset id and a face are
		// the first; a score, a timer and a chat line are the second.
		//
		// The cost is paid where it belongs. A `String` property makes its
		// component non-trivial - an allocation per row rather than a shared id -
		// so a component holding one needs a written serialiser and does not
		// memcpy. `ecs::Column` has carried that path since v0.2 and this is the
		// first component set to use it.
		String,

		// `Entity` - a handle within this world, and meaningless outside it.
		Reference,

		// The `core/types` values a script actually holds. These describe what
		// **userland** sees, not what a component stores - a property whose
		// getter doubles a half-extent is a `Vector3` here and the storage is
		// whatever `Bounds` says. That is why this list stays short no matter
		// what the components grow: `spatial::LayerMask` will never need a case,
		// because no property is one.
		Vector3,
		CFrame,
		Color3,

		// The four a 2D tree is authored in, added at v0.8 for `gui`.
		//
		// **They are here because a `UDim2` has to be a property, not because
		// `core/types` gained four headers.** All four have existed since v0.6
		// and none of them was a `PropertyType`, so a component holding one
		// could not be saved, could not appear in a properties panel and could
		// not be set from a script - which makes `Frame.Size` unauthorable, and
		// an unauthorable size is a widget set nobody can use.
		//
		// The list stays closed and this is what growing it looks like: four
		// cases in `game::Values`, four widgets in the properties panel, four
		// in each binding, and this comment saying why.
		Vector2,
		UDim,
		UDim2,
		Rect,

		// The three a curve is authored in, added at v0.10 for `effects`.
		//
		// **Here for the reason the four above are: a value that cannot be a
		// property is a value nothing can author.** `core::NumberSequence` and
		// `core::ColorSequence` have existed since v0.6 and neither was a
		// `PropertyType`, which was fine while the only thing holding one was a
		// local in a script. A particle emitter holds eight of them, and an
		// emitter whose `Size` could not be saved, could not appear in a
		// properties panel and could not be set from a script would be an emitter
		// nobody can author - the same gap `UDim2` had at v0.8, in the same words.
		//
		// **They are big, and that is the one thing about them worth checking
		// against the closed-list rule.** A `NumberSequence` is 328 bytes and a
		// `ColorSequence` is 328; every other member of this enum is 32 or fewer.
		// `PropertyDescriptor::Size` already carries the width and every path
		// checks a caller's buffer against it, so nothing here assumes a property
		// value fits in a register - but a marshaller that copied by value into a
		// stack temporary per read is a marshaller that just grew its frame by a
		// third of a kilobyte. `control/src/Tools.cpp` reads through a shared
		// buffer for exactly this reason and is where the size is felt.
		//
		// `NumberRange` joins them because an emitter's `Lifetime` and `Speed` are
		// ranges rather than curves, and a range spelled as two float properties
		// is two writes with a frame between them where the minimum exceeds the
		// maximum.
		NumberRange,
		NumberSequence,
		ColorSequence,
	};

	struct PropertyDescriptor;
}

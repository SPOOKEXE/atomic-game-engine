#pragma once

// Luau's userdata tag numbers, and nothing else.
//
// **Split out of `LuauBindings.hpp` because a tag is not a VM.** The block below
// is a plain enum of ints, but it used to sit inside the umbrella header that
// includes `<lua.h>` - so a *neutral* service surface naming its own tag, which
// `UserInputService` and `SoundService` each have to, pulled the whole Luau API
// into a translation unit that does not otherwise mention it. This header is
// what those two include instead, and it is why they compile in a build that
// never opens a VM.
//
// **A tag stays a Luau fact even though the number is neutral.**
// `ServiceSurface::Tag` is what a *Luau* installer passes to
// `lua_newuserdatatagged`; JavaScript reads none of it. So the name says which
// VM's tag space this is rather than pretending the numbers are shared.
//
// @tier L9 · shared

namespace engine::script {

	// Userdata tags. Luau checks these on every access, so a `Color3` handed to
	// something expecting a `Vector3` is caught by the VM rather than by a
	// reinterpret_cast that happens to line up - the two are three floats each.
	//
	// **Values are explicit and must not be reordered.** Nothing serialises one,
	// so this is not rule 4 - it is that a tag is compared against a userdata
	// created earlier in the same process, and renumbering mid-edit is the kind
	// of change that produces a type confusion nothing reports.
	enum : int {
		TAG_VECTOR3 = 1,
		TAG_COLOR3 = 2,
		TAG_CFRAME = 3,
		TAG_INSTANCE = 4,

		// **Retired at v0.7, and the number is held down rather than reused.**
		// This tagged `workspace`, back when `workspace` was the world itself
		// rather than an instance in it. It is a `TAG_INSTANCE` now - see
		// `OpenWorkspace` for why the two notions were collapsed.
		//
		// Not deleted, because the paragraph above this enum gives the reason:
		// a tag is compared against userdata created earlier in the same
		// process, and handing 5 to a new type is how a type confusion nothing
		// reports gets introduced.
		TAG_WORLD_RETIRED = 5,

		// v0.6's datatype vocabulary.
		TAG_VECTOR2 = 6,
		TAG_UDIM = 7,
		TAG_UDIM2 = 8,
		TAG_RECT = 9,
		TAG_REGION3 = 10,
		TAG_NUMBER_RANGE = 11,
		TAG_NUMBER_SEQUENCE = 12,
		TAG_COLOR_SEQUENCE = 13,
		TAG_TWEEN_INFO = 14,
		TAG_RAY = 15,
		TAG_RANDOM = 16,

		// The signal surface. A signal is a handle onto `SignalTable`; a
		// connection is a handle onto one entry in it.
		TAG_SIGNAL = 17,
		TAG_CONNECTION = 18,

		// One member of one enum, which is a pair of interned names.
		TAG_ENUM_ITEM = 19,

		// What a raycast is told to ignore.
		TAG_RAYCAST_PARAMS = 20,

		// One stop in a sequence.
		//
		// **Added at v0.10 because a particle emitter made the table form
		// insufficient**, which is a reversal of the note above
		// `ReadNumberKeypoint` and worth recording rather than quietly editing
		// out. That note said two more userdata types for a value an author
		// writes inline once was surface nobody asked for, and while a sequence
		// was only ever constructed from a literal it was right.
		//
		// What changed is that a sequence is now a *property*: an emitter's
		// `Transparency` is read back, and `emitter.Transparency.Keypoints[1]`
		// handed back a bare `{time, value, envelope}` table - three anonymous
		// numbers with no `typeof`, no way to tell one from a `ColorSequence`'s
		// stop, and nothing to compare against. Reading a value back in a shape
		// its own constructor accepts is the round trip a property surface owes,
		// and the table form does not give it a name.
		//
		// The table form still works everywhere it did. Both constructors take
		// either.
		TAG_NUMBER_KEYPOINT = 21,
		TAG_COLOR_KEYPOINT = 22,

		// `UserInputService`, which is a userdata rather than a table.
		//
		// **Because a table cannot hold a property that changes.** `luaL_sandbox`
		// freezes the globals and enables `safeenv`, and Luau then compiles
		// `Service.Field` on a constant global table into a `GETIMPORT` resolved
		// once per closure - so `UserInputService.MouseBehavior` would read
		// whatever it was the first time a script asked, forever. A userdata's
		// field access always goes through `__index`.
		//
		// It carries no payload. What the object is, is its metatable.
		TAG_INPUT_SERVICE = 23,

		// `SoundService`, a userdata for `TAG_INPUT_SERVICE`'s reason: it has a
		// live `Volume`, and a property on a frozen global table is a
		// `GETIMPORT` resolved once. Carries no payload either.
		TAG_SOUND_SERVICE = 24,

		// What an input signal hands its listener.
		//
		// **Roblox's `InputObject`, and the reason it is a userdata rather than
		// a table is not `safeenv` this time** - it is that a table would be a
		// value a handler could write into and hand on, and an input report is
		// a fact about a frame rather than a document. The tag is also what
		// makes `typeof` answer `"InputObject"`.
		TAG_INPUT_OBJECT = 25,

		// What `TweenService:Create` hands back.
		//
		// **A userdata of its own rather than the ordinary instance handle**,
		// even though a tween *is* an entity - see `Tweens.hpp`. The neutral
		// instance methods are installed flat on every instance, so a `Play`
		// there would claim the name for every part and folder in the engine,
		// and `Play` is a name Roblox puts on three classes.
		//
		// Carries the tween's entity, which is the key to `TweenTable` and the
		// subject of its `Completed`.
		TAG_TWEEN = 26,
	};
}

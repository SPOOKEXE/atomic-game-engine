#pragma once

// Roblox's binary model container, `.rbxm`, read into a tree of classes, names
// and values.
//
// **The one importer here whose output is not a mesh or a picture.** Every other
// reader in this module turns somebody else's file into an `assets::MeshData` or
// an `assets::TextureData`; an `.rbxm` is an *instance tree*, so what comes back
// is a tree of what the file called each thing and what it said about it. Who
// turns that into rows in a store is the caller's business — `bake` is L9 and
// knows nothing about `ecs`.
//
// **A referent is a within-file encoding and never leaves as identity.** The
// format numbers its instances so that its `PRNT` chunk can say which is inside
// which, and those numbers are chosen by whatever wrote the file. So a referent
// becomes exactly one thing here — **the shape of the tree** — and is discarded
// with the parse. Nothing in what comes back is numbered, and a `Ref`-typed
// property is refused rather than turned into an index, because a number that
// named a row of one file is not a name anything outside it can resolve. That is
// `AGENTS.md` rule 4 read from the parser's side.
//
// ## The subset, said once
//
// This reads a deliberately small part of a large format, and everything outside
// it is **reported by name rather than approximated**, for the reason
// `bake/AGENTS.md` gives about interlaced PNG: a half-read file that produces a
// recognisable, wrong result reads as a setting rather than as a bug.
//
// **Read:**
//
// - the `INST`, `PROP`, `PRNT` and `SSTR` chunks, with any other skipped by its
//   own length;
// - a payload stored raw, LZ4-framed or Zstandard-framed, which is every
//   framing Roblox writes;
// - `String`, `ProtectedString`, `Bool`, `Int32`, `Int64`, `Float32` and
//   `Float64`;
// - `UDim`, `UDim2`, `Vector2`, `Vector3`, `Color3`, `Color3uint8`, `Rect` and
//   `NumberRange`;
// - `CFrame`, whole — position *and* rotation;
// - `SharedString`, resolved out of the file's own table.
//
// **Refused by name:** `Enum` and `Ref`, which are numbers naming somebody
// else's table; `NumberSequence`, `ColorSequence`, `PhysicalProperties`, `Font`
// and everything else the format carries.
//
// **A refused property costs its property and not its file.** Every `PROP` chunk
// carries one property of one class and nothing after it depends on its bytes, so
// a type this does not decode is a chunk skipped whole. That is the structural
// fact that makes a partial reader of this format safe rather than lucky, and it
// is why the list above can grow one row at a time.
//
// **An `Enum` is refused rather than guessed** and it is the row most likely to
// be argued with. The file stores an enum as a number, and that number indexes
// Roblox's table — `Material` 256 is `Plastic` because Roblox says so. This
// engine names enum members through `ecs::EnumTable` and has no such table, so
// the only ways to honour one are to ship Roblox's numbering or to use this
// engine's declaration order. The second is precisely the id-derived-from-order
// that rule 4 exists to forbid.
//
// @tier L9 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::bake {

	// Which of a `RobloxValue`'s fields carries the value.
	//
	// @since v0.15
	enum class RobloxValueKind : uint8_t {
		// `Bool`.
		Bool,

		// `Integer` — the format's `Int32` and `Int64` both arrive here.
		Integer,

		// `Number` — the format's `Float32` and `Float64` both arrive here.
		Number,

		// `Text` — the format's `String`, `ProtectedString` and `SharedString`
		// all arrive here, the last already resolved out of the file's table.
		//
		// `ProtectedString` is how a script's source is written and is a
		// separate type number carrying the same bytes; see `RobloxModel.cpp`
		// for why leaving it out is the mistake that costs every script.
		Text,

		// `Vector3`.
		Vector3,

		// `Vector2`.
		Vector2,

		// `Color3` — the format's `Color3` and `Color3uint8` both arrive here.
		Color3,

		// `CFrame`.
		CFrame,

		// `UDim`.
		UDim,

		// `UDim2`.
		UDim2,

		// `Rect`.
		Rect,

		// `NumberRange`.
		NumberRange,
	};

	// One property's value as the file spelled it.
	//
	// **A struct with every field rather than a variant**, which is
	// `game::PropertyValue`'s argument and holds here for the same reason: this
	// crosses once per property of an import and is never stored in bulk.
	//
	// **It is not `game::PropertyValue` and cannot be.** That type lives in
	// `game`, which is above this module and links `ecs`, `scene` and `world`; an
	// importer that named it would put half the engine underneath a foreign-format
	// parser. So this carries the kinds a `.rbxm` can actually produce — the left
	// column of the table in this file's header, and nothing else — and the
	// caller converts, keyed on the type its own class table declares rather than
	// on what the file happened to store.
	//
	// @since v0.15
	struct RobloxValue {
		// Which field below is meaningful.
		RobloxValueKind Kind = RobloxValueKind::Bool;

		// A `RobloxValueKind::Bool` value.
		bool Bool = false;

		// A `RobloxValueKind::Integer` value.
		int64_t Integer = 0;

		// A `RobloxValueKind::Number` value.
		double Number = 0.0;

		// A `RobloxValueKind::Text` value. Bytes as the file held them, which is
		// UTF-8 for anything Studio wrote.
		std::string Text;

		// A `RobloxValueKind::Vector3` value.
		core::Vector3 Vector3;

		// A `RobloxValueKind::Vector2` value.
		core::Vector2 Vector2;

		// A `RobloxValueKind::Color3` value, on 0..1 whichever type it arrived
		// as.
		core::Color3 Color3;

		// A `RobloxValueKind::CFrame` value.
		core::CFrame CFrame;

		// A `RobloxValueKind::UDim` value.
		core::UDim UDim;

		// A `RobloxValueKind::UDim2` value.
		core::UDim2 UDim2;

		// A `RobloxValueKind::Rect` value.
		core::Rect Rect;

		// A `RobloxValueKind::NumberRange` value.
		core::NumberRange NumberRange;
	};

	// One property, as the file named it.
	//
	// @since v0.15
	struct RobloxProperty {
		// What the file called it — `Anchored`, `Size`. Matched against the
		// caller's own property list by spelling, because a name is the only
		// thing about a property that crosses a file.
		std::string Name;

		// What it said.
		RobloxValue Value;
	};

	// One instance out of the file.
	//
	// @since v0.15
	struct RobloxInstance {
		// The class the file named, which the caller resolves in its own class
		// table. Not checked here: `bake` has no idea what classes exist.
		std::string ClassName;

		// Its `Name` property, or the class name when it carried none — which is
		// Roblox's own default for an instance nobody renamed.
		//
		// **Lifted out of `Properties` rather than left in it**, so that there is
		// one place a name is read from rather than two that can disagree.
		std::string Name;

		// Everything else the file said about it, in the order the properties
		// were read. `Name` is not among them.
		std::vector<RobloxProperty> Properties;

		// What was inside it, in the order the file's parent table listed them.
		//
		// **A nested tree rather than indices into a flat list**, so that no
		// number derived from this file's own ordering appears in what comes
		// back. See this file's header on referents.
		std::vector<RobloxInstance> Children;
	};

	// What a `.rbxm` held.
	//
	// @since v0.15
	struct RobloxModel {
		// The instances with nothing above them, in file order.
		//
		// **A list rather than one**, because the format allows any number and
		// Rojo's file table allows one — so which count is acceptable is a
		// question for whoever asked, not for the reader.
		std::vector<RobloxInstance> Roots;

		// What the parse decided rather than read: a property type it does not
		// decode, a chunk it skipped, a parent link it refused. One line each, in
		// words somebody can act on.
		//
		// **Non-empty is not failure.** A file where every class is understood and
		// three properties are not is still an import; the notes are how it says
		// which three.
		std::vector<std::string> Notes;
	};

	// The ceiling on how many instances one file may declare.
	//
	// **A count off the wire, so it is checked before it is believed.** A real
	// place exported whole is around a hundred and forty thousand instances, so
	// this is an order of magnitude above anything a model file has cause to
	// hold and still refuses the four bytes an attacker chose. Matches
	// `Importers.hpp`'s argument for `MAXIMUM_IMPORTED_VERTICES`.
	constexpr uint32_t MAXIMUM_ROBLOX_INSTANCES = 1024u * 1024u;

	// How deep the tree may nest.
	//
	// The bound `studio::RojoSync` puts on a JSON document, for the same reason:
	// the tree is walked recursively and a file is free to claim a million levels.
	// It doubles as the cycle check — a parent chain longer than this is either
	// too deep or a loop, and both answers are the same one.
	constexpr uint32_t MAXIMUM_ROBLOX_DEPTH = 64;

	// Reads a `.rbxm`.
	//
	// Every count and length in the file is treated as hostile, for `Image.hpp`'s
	// reason: this runs over a file somebody supplied.
	//
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure, so a caller can name the file *and*
	//                the reason. Untouched on success.
	// @return `false` on a file this cannot read or will not trust. A file it can
	//         read but only partly understands succeeds with `RobloxModel::Notes`
	//         saying what was left out.
	// @since v0.15
	bool ReadRobloxModel(std::span<const std::byte> bytes, RobloxModel &out, std::string &failure);
}

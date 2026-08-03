#pragma once

// What a script sends across a bus, as bytes.
//
// **The one piece of this surface with no prior art anywhere in the tree**, and
// `docs/SCRIPT_CONCURRENCY.md` §3 states its three requirements in priority
// order. Each of them decided something about the format rather than about the
// implementation, which is the point — a property of the *format* is one both
// bindings get for free, and a property of an implementation is one each would
// have to be trusted to add.
//
// ## 1. Deterministic
//
// The bytes go into a recording that has to replay identically, so **table
// iteration order is the trap**. A codec walking a hash map in memory order
// serialises differently on two runs of one script, and `just determinism` then
// fails somewhere a long way from the codec.
//
// So keys are sorted, **by bytes**, and the sort is part of the format rather
// than something a caller should remember to do. Byte order and not the
// language's own string comparison: Lua compares with `strcoll` and JavaScript
// compares UTF-16 code units, so `"é" < "z"` is answerable two ways and neither
// VM's answer may be allowed to reach the wire. `Encode` sorts, always, whatever
// order the entries arrived in.
//
// ## 2. Identical across both VMs
//
// A world scripted in Luau and one scripted in JavaScript must produce the same
// bytes for equivalent values, or the two disagree on the wire while each looks
// internally consistent. **That is one shared test, not two per-VM ones**, and
// it is why this file holds a value tree rather than a pair of streaming
// writers: both bindings build the same `ScriptValue` and hand it here, so
// there is exactly one encoder and nothing for the two to differ about.
//
// Numbers are one case worth naming. Lua and JavaScript both hold a double, so
// a number crosses as **eight bytes always** — never narrowed to an integer
// when it happens to be whole. A format that shortened `1.0` to an int and left
// `1.5` a double would make the bytes depend on the value's history rather than
// on the value.
//
// ## 3. Bounded
//
// A payload has a maximum size and a maximum depth, both **refused rather than
// truncated**, and a cyclic table is an error rather than a hang. Truncation
// produces a message the far side can decode and that means something else,
// which is worse than no message.
//
// ## What crosses, and what does not
//
// Booleans, numbers, strings, the three value types, and arrays and maps of
// those. Not functions, not instances — an `Entity` is *"meaningless outside
// this world"*, so a reference has to cross as whatever the game uses to name
// things — and nothing holding a pointer. Rule 3, expressed as a type list.
//
// @tier L9 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {

	// How far a value may nest before it is refused.
	//
	// Deep enough for anything an author writes by hand, shallow enough that
	// the recursive walk cannot exhaust a stack. A refusal names the limit.
	inline constexpr uint32_t CODEC_MAX_DEPTH = 16;

	// The largest payload one publish may produce, in bytes.
	//
	// Sixty-four kilobytes, which is well inside a bus datagram and far past
	// what a message ought to be carrying. A script sending more than this is
	// using the wrong tool, and saying so early is kinder than fragmenting.
	inline constexpr size_t CODEC_MAX_BYTES = 64u * 1024u;

	// The tag byte in front of every encoded value.
	//
	// **Explicit values, and they are part of the wire format.** Reordering this
	// enum changes what old bytes mean, which is rule 4's concern arriving
	// through a C++ default nobody looked at.
	//
	// @since v0.6
	enum class ValueTag : uint8_t {
		Nil = 0x00,
		False = 0x01,
		True = 0x02,
		Number = 0x03,
		String = 0x04,
		Array = 0x05,
		Map = 0x06,
		Vector3 = 0x07,
		Color3 = 0x08,
		CFrame = 0x09,
	};

	// Why an encode or a decode did not produce a value.
	//
	// Named rather than a bare `false`, because §5's rule is that a script
	// author must be able to tell which refusal they hit.
	//
	// @since v0.6
	enum class CodecStatus : uint8_t {
		// It worked.
		Ok,

		// Nesting past `CODEC_MAX_DEPTH`.
		TooDeep,

		// The encoding would exceed `CODEC_MAX_BYTES`.
		TooLarge,

		// A table reachable from itself.
		Cyclic,

		// A function, an instance, or anything else with no representation.
		Unsupported,

		// Decoding hit a tag it does not know, or ran out of bytes.
		Malformed,
	};

	// A stable, human-readable name for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(CodecStatus status);

	// One value on its way across a bus.
	//
	// A tree rather than a stream, so map keys can be sorted before anything is
	// written and so both bindings share one encoder. Built by a VM's own
	// walker, consumed by `Encode`, produced by `Decode`.
	//
	// @since v0.6
	struct ScriptValue {
		// Which of the fields below carries the value.
		ValueTag Tag = ValueTag::Nil;

		// Set for `True` and `False`.
		bool Boolean = false;

		// Set for `Number`. A double, always: both languages hold one.
		double Number = 0.0;

		// Set for `String`. Bytes rather than characters — nothing here decodes
		// an encoding.
		std::string Text;

		// Set for `Array`, in order.
		std::vector<ScriptValue> Items;

		// Set for `Map`. **Sorted by `Encode`, not by the caller**: a binding
		// that walked its table in whatever order the VM offered is exactly the
		// case the sort exists for, so requiring the caller to sort would put
		// the determinism guarantee back in two places.
		std::vector<std::pair<std::string, ScriptValue>> Entries;

		// Set for `Vector3`.
		core::Vector3 Vector;

		// Set for `Color3`.
		core::Color3 Colour;

		// Set for `CFrame`.
		core::CFrame Frame;

		// Constructs nil.
		ScriptValue() = default;

		// Constructs a value of one tag with everything else defaulted.
		//
		// @param tag Which kind of value.
		explicit ScriptValue(ValueTag tag) : Tag(tag) {}

		// Reports whether two values are equal, comparing only what the tag
		// says is meaningful.
		//
		// **Map entries compare in order**, because after `Encode` they are
		// sorted and two maps that encode identically must compare equal. A
		// caller comparing two hand-built maps in different orders is comparing
		// something the wire never sees.
		bool operator==(const ScriptValue &other) const;
	};

	// Writes a value as bytes.
	//
	// Sorts every map's entries by key bytes on the way, in place, so the
	// tree handed in comes back in the order it was written.
	//
	// @param value The value to encode.
	// @param out   Filled in with the bytes. Cleared first.
	// @return `Ok`, or why not.
	CodecStatus Encode(ScriptValue &value, std::vector<std::byte> &out);

	// Reads back what `Encode` produced.
	//
	// **Never trusts what it reads.** A length field is checked against what is
	// left before anything is reserved, so a corrupt payload claiming four
	// billion entries fails rather than allocating.
	//
	// @param bytes The payload.
	// @param out   Filled in with the value.
	// @return `Ok`, or why not.
	CodecStatus Decode(std::span<const std::byte> bytes, ScriptValue &out);
}

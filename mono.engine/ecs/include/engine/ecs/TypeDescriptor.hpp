#pragma once

// What the storage knows about a component type whose C++ type it has lost.
//
// An archetype holds columns of bytes. It has to construct, destroy, move and
// serialise those bytes without being a template, because four different things
// need the same type information for four different reasons and only one of
// them is compile-time:
//
// - **systems** iterate rows and want the concrete type back,
// - **Luau** reads and writes properties *by name at runtime*, so there is no
//   `T` at the call site at all,
// - **snapshots** serialise a column for a restart or a process migration,
// - **the bindings manifest** is generated from the same metadata.
//
// One descriptor per registered type serves all four. Without it each would
// grow its own table, and the three that are not the compiler's would disagree.
//
// **Ids are process-local; names cross.** `ComponentId` is a dense counter
// assigned in registration order, so it is fast to compare and meaningless in a
// file. Anything leaving this process carries `TypeDescriptor::Name`.
//
// @tier L3 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Enums.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace engine::ecs {

	// A dense process-local handle for one registered component type.
	//
	// Assigned in registration order, so two runs of the same binary that
	// register the same types in the same order agree — which is what lets an
	// archetype be identified by a sorted list of these rather than by names.
	// Registration order is fixed by doing it at startup; see Components.
	//
	// @since v0.2
	struct ComponentId {
		// The value no registration produces.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// The registration index, or INVALID.
		uint32_t Index = INVALID;

		// Creates an invalid id.
		constexpr ComponentId() = default;

		// Creates an id from a registration index.
		//
		// @param index The registration index to wrap.
		constexpr explicit ComponentId(uint32_t index) : Index(index) {}

		// Reports whether this id names a registered type.
		//
		// @return `true` when the id came from a registration.
		constexpr bool IsValid() const {
			return Index != INVALID;
		}

		// Compares registration indices for equality.
		//
		// @param other The id to compare.
		// @return `true` when both name the same type.
		constexpr bool operator==(const ComponentId &other) const {
			return Index == other.Index;
		}

		// Compares registration indices for inequality.
		//
		// @param other The id to compare.
		// @return `true` when the ids name different types.
		constexpr bool operator!=(const ComponentId &other) const {
			return Index != other.Index;
		}

		// Orders by registration index, which is what keeps a component set
		// canonical: one sorted list per set of types, whatever order the
		// caller named them in.
		//
		// @param other The id to compare.
		// @return `true` when this id was registered first.
		constexpr bool operator<(const ComponentId &other) const {
			return Index < other.Index;
		}
	};

	// A component's second serialisation: the compact, lossy form it crosses a
	// replication wire in.
	//
	// **Separate from `Write` and `Read` on purpose, and that separation is the
	// whole reason this type exists rather than a codec installed over them.**
	// `Store::Save` and `Store::Load` are what a recording is made of, and
	// `just replay-check` requires a replay to reproduce the run it replayed
	// byte for byte. A lossy codec fitted over `Write` would make every
	// recording lossy and the check would go on passing, because it would be
	// comparing one lossy file against another. So this pair is named
	// differently and the storage never reaches it: **nothing in `ecs` calls
	// these**, and the only caller is `replication`.
	//
	// It is also why a wire codec belongs here rather than in a table
	// `replication` keeps by component name. The registration that installs it
	// is the same one that names the type, so a server and a client cannot
	// disagree about a component's wire form without disagreeing about the
	// component — and a receiver decoding ten bytes as twenty-eight is not a
	// failure worth making a caller's discipline.
	//
	// @since v0.5
	struct WireFormat {
		// Appends `count` values in the compact form.
		void (*Write)(core::ByteWriter &writer, const void *source, size_t count) = nullptr;

		// Reads `count` already-constructed values back out of it.
		//
		// **Must be total over its input.** Every bit pattern reaches this from
		// a peer, so a decode has to produce a usable value for all of them
		// rather than only for the ones an encoder would have produced.
		void (*Read)(core::ByteReader &reader, void *destination, size_t count) = nullptr;

		// Bytes one value occupies in that form. Zero when there is none.
		uint32_t Size = 0;

		// Whether this format is usable.
		//
		// @return `true` when a caller may encode and decode through it.
		bool Present() const {
			return Write != nullptr && Read != nullptr && Size > 0;
		}
	};

	// Everything the storage needs to handle a component it cannot name.
	//
	// The function pointers operate on *ranges* rather than single values so
	// that a trivially copyable type collapses to one memcpy for a whole column
	// instead of a call per row. `Trivial` says when the caller may skip them
	// entirely and move the bytes itself.
	//
	// @since v0.2
	struct TypeDescriptor {
		// The stable identity. This is what a snapshot, a manifest or a wire
		// format carries; the id is not.
		core::Name Name;

		// sizeof(T). Zero for a tag — a type with no data, which costs a column
		// of nothing and exists only to be matched by a query.
		uint32_t Size = 0;

		// alignof(T). Never zero, so a column can always compute an allocation.
		uint32_t Alignment = 1;

		// Whether this component holds bytes at all.
		//
		// Derived rather than declared — a type with no members is a tag — but
		// named, so a caller asks what a component is instead of inferring it
		// from a size of zero and getting the inference wrong somewhere.
		ComponentKind Kind = ComponentKind::Data;

		// Whether the bytes may be copied, moved and abandoned directly.
		//
		// True for almost every component — a transform, a velocity, an id.
		// When it is true the four lifetime hooks below still work but nothing
		// has to call them, and the storage takes the memcpy path.
		bool Trivial = false;

		// Whether Write and Read are usable for this type.
		//
		// False means the type has no serialisation and a snapshot containing
		// it will refuse rather than write bytes that cannot be read back.
		bool Serialisable = false;

		// Default-constructs `count` values at `destination`.
		void (*DefaultConstruct)(void *destination, size_t count) = nullptr;

		// Destroys `count` values at `destination`.
		void (*Destruct)(void *destination, size_t count) = nullptr;

		// Copy-constructs `count` values at `destination` from `source`.
		void (*CopyConstruct)(void *destination, const void *source, size_t count) = nullptr;

		// Move-constructs `count` values at `destination` from `source`,
		// leaving the source values valid and destructible.
		void (*MoveConstruct)(void *destination, void *source, size_t count) = nullptr;

		// Appends `count` values to `writer`. Null unless Serialisable.
		void (*Write)(core::ByteWriter &writer, const void *source, size_t count) = nullptr;

		// Reads `count` already-constructed values from `reader`. Null unless
		// Serialisable. A short or corrupt buffer leaves the reader failed and
		// the values unspecified but valid — never a partial object.
		void (*Read)(core::ByteReader &reader, void *destination, size_t count) = nullptr;

		// The compact form this type crosses a replication wire in, if it has
		// one. Empty means the wire carries `Write`'s bytes unchanged.
		//
		// **Lossy, so `Save` and `Load` must never touch it.** See `WireFormat`
		// for what that would cost, and `ecs/AGENTS.md` for the convention,
		// which the build cannot check.
		WireFormat Wire;
	};

	// The name a type is registered under when nobody supplies one.
	//
	// Derived from the compiler's own spelling of the signature, which gives
	// `server::Position` rather than a mangled string, and needs no macro at
	// every declaration. Two consequences worth knowing:
	//
	// - It is **stable within one build** and may differ between compilers, so
	//   it satisfies same-binary determinism and nothing wider.
	// - Anything whose name has to survive a compiler change — a component in a
	//   save file or on a wire — should be registered explicitly instead.
	//
	// @return The spelled-out type name, without a trailing null.
	template <class T> constexpr std::string_view TypeNameOf() {
#if defined(__clang__) || defined(__GNUC__)
		constexpr std::string_view signature = __PRETTY_FUNCTION__;
		constexpr std::string_view opening = "T = ";
		constexpr std::string_view closing = "]";
#elif defined(_MSC_VER)
		constexpr std::string_view signature = __FUNCSIG__;
		constexpr std::string_view opening = "TypeNameOf<";
		constexpr std::string_view closing = ">(void)";
#else
		constexpr std::string_view signature = "unknown";
		constexpr std::string_view opening = "";
		constexpr std::string_view closing = "";
#endif
		const size_t start = signature.find(opening);
		if (start == std::string_view::npos) {
			return signature;
		}

		const size_t from = start + opening.size();

		// GCC spells the whole substitution list — `[with T = X; std::string_view
		// = ...]` — so the name ends at the first semicolon, not at the closing
		// bracket. Taking the bracket produced names like
		// `Position; std::string_view = std::basic_string_view<char>`, which are
		// still unique per type and so still *worked*, but are unreadable in a
		// log and would have gone into a bindings manifest.
		size_t to = signature.find(';', from);
		if (to == std::string_view::npos) {
			to = signature.rfind(closing);
		}

		if (to == std::string_view::npos || to <= from) {
			return signature.substr(from);
		}

		return signature.substr(from, to - from);
	}

	// Builds the descriptor for a concrete type.
	//
	// Every hook is generated here, where `T` is still known. Serialisation is
	// the raw object representation, which is only offered for a trivially
	// copyable type.
	//
	// @warning A trivially copyable component containing a `core::Name` gets
	//          raw serialisation that writes the name's **id** — a
	//          process-local counter — which `Name.hpp` says never to
	//          serialize. Such a type must be registered with an explicit
	//          writer and reader instead.
	//
	// @warning **A component that is snapshotted must have no padding.** Raw
	//          serialisation writes the object representation, padding
	//          included, and padding is never initialised — so two runs of one
	//          scene produce different bytes and every comparison of two worlds
	//          becomes unreliable. Order the members widest-first, or add an
	//          explicit `Reserved` field, as `WorldTime` does. `just
	//          determinism` is what catches this.
	//
	// @return The descriptor for `T`, named `name`.
	template <class T> TypeDescriptor DescribeType(core::Name name) {
		TypeDescriptor descriptor;
		descriptor.Name = name;
		descriptor.Size = static_cast<uint32_t>(std::is_empty_v<T> ? 0 : sizeof(T));
		descriptor.Kind = std::is_empty_v<T> ? ComponentKind::Tag : ComponentKind::Data;
		descriptor.Alignment = static_cast<uint32_t>(alignof(T));
		descriptor.Trivial = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;
		descriptor.Serialisable = std::is_trivially_copyable_v<T> && !std::is_empty_v<T>;

		descriptor.DefaultConstruct = [](void *destination, size_t count) {
			auto *values = static_cast<T *>(destination);
			for (size_t index = 0; index < count; index++) {
				new (values + index) T();
			}
		};

		descriptor.Destruct = [](void *destination, size_t count) {
			auto *values = static_cast<T *>(destination);
			for (size_t index = 0; index < count; index++) {
				values[index].~T();
			}
		};

		if constexpr (std::is_copy_constructible_v<T>) {
			descriptor.CopyConstruct = [](void *destination, const void *source, size_t count) {
				auto *values = static_cast<T *>(destination);
				const auto *from = static_cast<const T *>(source);
				for (size_t index = 0; index < count; index++) {
					new (values + index) T(from[index]);
				}
			};
		}

		if constexpr (std::is_move_constructible_v<T>) {
			descriptor.MoveConstruct = [](void *destination, void *source, size_t count) {
				auto *values = static_cast<T *>(destination);
				auto *from = static_cast<T *>(source);
				for (size_t index = 0; index < count; index++) {
					new (values + index) T(std::move(from[index]));
				}
			};
		}

		if constexpr (std::is_trivially_copyable_v<T> && !std::is_empty_v<T>) {
			descriptor.Write = [](core::ByteWriter &writer, const void *source, size_t count) {
				writer.WriteRaw(source, sizeof(T) * count);
			};
			descriptor.Read = [](core::ByteReader &reader, void *destination, size_t count) {
				reader.ReadRaw(destination, sizeof(T) * count);
			};
		}

		return descriptor;
	}
}

#include <engine/core/Log.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Schema.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace engine::ecs {

	namespace {

		// The largest number of components a process may describe.
		//
		// **A cap because the lifetime hooks are function pointers with nowhere
		// to put a schema.** `TypeDescriptor::DefaultConstruct` is
		// `void (*)(void *, size_t)` - no context parameter, and a captureless
		// function cannot look up which schema it belongs to. So one hook set is
		// generated per slot at compile time, `Thunks<N>` closes over the index
		// as a template argument, and the table below is what a registration
		// hands out.
		//
		// The alternative was threading a context through the six hooks, which
		// is a public signature change reaching all hundred-odd
		// `Components::Register<T>` calls plus every hand-written serialiser in
		// the engine - a very large edit so that a game could describe one more
		// component.
		//
		// **The number is measured rather than guessed, and here is the table.**
		// `.text` of the release `server` binary, which is what a player would
		// download:
		//
		//     slots    .text        over 256     configure+build
		//       256    6 056 426           -          -
		//      1024    6 203 882    +147 KB       5.0 s
		//      2048    6 400 490    +344 KB       8.1 s
		//      4096    6 793 706    +737 KB      14.4 s
		//
		// About **192 bytes of code a slot** - six thunks of roughly thirty-two
		// bytes each - so the curve is flat and the choice is nearly free at any
		// of these. 2048 is taken because it is eight times what a game has ever
		// needed and still costs a third of a megabyte; going to 4096 doubles
		// the bill to buy headroom above a number nothing is near.
		//
		// **Those figures depend on `SCHEMA_OUT_OF_LINE` and are meaningless
		// without it.** With the hook bodies inlinable the compiler put a copy of
		// each switch into every thunk: the same file compiled to 113 MB of
		// object at 4096 slots and took ninety seconds. See the macro.
		//
		// The refusal is reported rather than silent: `Status::Exhausted` names
		// the limit and this comment says what moving it costs.
		constexpr size_t MAX_SCHEMAS = 2048;

		// What one field type occupies.
		struct ValueLayout {
			uint32_t Size = 0;
			uint32_t Align = 1;
		};

		ValueLayout LayoutOf(PropertyType type) {
			switch (type) {
			case PropertyType::Bool:
				return {sizeof(bool), alignof(bool)};
			case PropertyType::Int32:
				return {sizeof(int32_t), alignof(int32_t)};
			case PropertyType::Int64:
				return {sizeof(int64_t), alignof(int64_t)};
			case PropertyType::Float:
				return {sizeof(float), alignof(float)};
			case PropertyType::Double:
				return {sizeof(double), alignof(double)};
			case PropertyType::Name:
			case PropertyType::Enum:
				return {sizeof(core::Name), alignof(core::Name)};
			case PropertyType::String:
				return {sizeof(std::string), alignof(std::string)};
			case PropertyType::Reference:
				return {sizeof(Entity), alignof(Entity)};
			case PropertyType::Vector3:
				return {sizeof(core::Vector3), alignof(core::Vector3)};
			case PropertyType::CFrame:
				return {sizeof(core::CFrame), alignof(core::CFrame)};
			case PropertyType::Color3:
				return {sizeof(core::Color3), alignof(core::Color3)};
			case PropertyType::Vector2:
				return {sizeof(core::Vector2), alignof(core::Vector2)};
			case PropertyType::UDim:
				return {sizeof(core::UDim), alignof(core::UDim)};
			case PropertyType::UDim2:
				return {sizeof(core::UDim2), alignof(core::UDim2)};
			case PropertyType::Rect:
				return {sizeof(core::Rect), alignof(core::Rect)};
			case PropertyType::NumberRange:
				return {sizeof(core::NumberRange), alignof(core::NumberRange)};
			case PropertyType::NumberSequence:
				return {sizeof(core::NumberSequence), alignof(core::NumberSequence)};
			case PropertyType::ColorSequence:
				return {sizeof(core::ColorSequence), alignof(core::ColorSequence)};
			case PropertyType::Opaque:
				break;
			}
			return {};
		}

		uint32_t BitsOf(FieldPacking packing, PropertyType type) {
			switch (packing) {
			case FieldPacking::Float16:
			case FieldPacking::UFloat16:
			case FieldPacking::Int16:
			case FieldPacking::UInt16:
				return 16;
			case FieldPacking::Float8:
			case FieldPacking::UFloat8:
			case FieldPacking::Int8:
			case FieldPacking::UInt8:
				return 8;
			case FieldPacking::Int4:
			case FieldPacking::UInt4:
				return 4;
			case FieldPacking::Bool:
				return 1;
			case FieldPacking::Native:
				return LayoutOf(type).Size * 8;
			}
			return 0;
		}

		bool PackingFits(FieldPacking packing, PropertyType type) {
			switch (packing) {
			case FieldPacking::Native:
				return true;
			case FieldPacking::Float16:
			case FieldPacking::UFloat16:
			case FieldPacking::Float8:
			case FieldPacking::UFloat8:
				return type == PropertyType::Float;
			case FieldPacking::Int16:
			case FieldPacking::UInt16:
			case FieldPacking::Int8:
			case FieldPacking::UInt8:
			case FieldPacking::Int4:
			case FieldPacking::UInt4:
				return type == PropertyType::Int32;
			case FieldPacking::Bool:
				return type == PropertyType::Bool;
			}
			return false;
		}

		uint32_t ReadBits(const void *blob, const FieldDescriptor &field) {
			const auto *bytes = static_cast<const uint8_t *>(blob) + field.Offset;
			uint32_t value = 0;
			for (uint8_t bit = 0; bit < field.StorageBits; bit++) {
				const uint8_t source = static_cast<uint8_t>(field.BitOffset + bit);
				value |= static_cast<uint32_t>((bytes[source / 8] >> (source % 8)) & 1u) << bit;
			}
			return value;
		}

		void WriteBits(void *blob, const FieldDescriptor &field, uint32_t value) {
			auto *bytes = static_cast<uint8_t *>(blob) + field.Offset;
			for (uint8_t bit = 0; bit < field.StorageBits; bit++) {
				const uint8_t target = static_cast<uint8_t>(field.BitOffset + bit);
				const uint8_t mask = static_cast<uint8_t>(1u << (target % 8));
				bytes[target / 8] = static_cast<uint8_t>(
					(bytes[target / 8] & ~mask) | (((value >> bit) & 1u) != 0 ? mask : 0u)
				);
			}
		}

		uint16_t PackFloat16(float value) {
			const uint32_t bits = std::bit_cast<uint32_t>(value);
			const uint32_t sign = (bits >> 16) & 0x8000u;
			const uint32_t exponent = (bits >> 23) & 0xffu;
			uint32_t mantissa = bits & 0x7fffffu;

			if (exponent == 0xffu) {
				return static_cast<uint16_t>(sign | (mantissa == 0 ? 0x7c00u : 0x7e00u));
			}

			const int32_t halfExponent = static_cast<int32_t>(exponent) - 127 + 15;
			if (halfExponent >= 31) {
				return static_cast<uint16_t>(sign | 0x7c00u);
			}
			if (halfExponent <= 0) {
				if (halfExponent < -10) {
					return static_cast<uint16_t>(sign);
				}
				mantissa |= 0x800000u;
				const uint32_t shift = static_cast<uint32_t>(14 - halfExponent);
				uint32_t rounded = mantissa >> shift;
				const uint32_t remainder = mantissa & ((1u << shift) - 1u);
				const uint32_t halfway = 1u << (shift - 1u);
				if (remainder > halfway || (remainder == halfway && (rounded & 1u) != 0)) {
					rounded++;
				}
				return static_cast<uint16_t>(sign | rounded);
			}

			uint32_t rounded = sign | (static_cast<uint32_t>(halfExponent) << 10) | (mantissa >> 13);
			const uint32_t remainder = mantissa & 0x1fffu;
			if (remainder > 0x1000u || (remainder == 0x1000u && (rounded & 1u) != 0)) {
				rounded++;
			}
			return static_cast<uint16_t>(rounded);
		}

		float UnpackFloat16(uint16_t half) {
			const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
			uint32_t exponent = (half >> 10) & 0x1fu;
			uint32_t mantissa = half & 0x3ffu;
			uint32_t bits = 0;

			if (exponent == 0) {
				if (mantissa == 0) {
					bits = sign;
				} else {
					int32_t normalExponent = -14;
					while ((mantissa & 0x400u) == 0) {
						mantissa <<= 1;
						normalExponent--;
					}
					mantissa &= 0x3ffu;
					bits = sign | (static_cast<uint32_t>(normalExponent + 127) << 23) | (mantissa << 13);
				}
			} else if (exponent == 0x1fu) {
				bits = sign | 0x7f800000u | (mantissa << 13);
			} else {
				bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
			}
			return std::bit_cast<float>(bits);
		}

		template <typename Integer> Integer Saturate(int32_t value) {
			const int64_t lowest = static_cast<int64_t>(std::numeric_limits<Integer>::lowest());
			const int64_t highest = static_cast<int64_t>(std::numeric_limits<Integer>::max());
			return static_cast<Integer>(std::clamp<int64_t>(value, lowest, highest));
		}

		// Whether a field owns anything the bytes alone do not carry.
		//
		// One type does, and the whole non-trivial path exists for it: a
		// `std::string` field holds an allocation, so the component cannot take
		// the memcpy path and needs its four lifetime hooks called.
		bool Owns(PropertyType type) {
			return type == PropertyType::String;
		}

		std::string *StringAt(void *blob, uint32_t offset) {
			return reinterpret_cast<std::string *>(static_cast<std::byte *>(blob) + offset);
		}

		const std::string *StringAt(const void *blob, uint32_t offset) {
			return reinterpret_cast<const std::string *>(static_cast<const std::byte *>(blob) + offset);
		}

		void *FieldAt(void *blob, uint32_t offset) {
			return static_cast<std::byte *>(blob) + offset;
		}

		const void *FieldAt(const void *blob, uint32_t offset) {
			return static_cast<const std::byte *>(blob) + offset;
		}

		// --- the per-field halves of the six hooks ---------------------------
		//
		// Every one of these is a loop over `Schema::Fields()`, and the blob is
		// **zeroed first** in each of the three that produce a fresh value. That
		// is the promise `Schema.hpp` makes in place of "there is no padding": a
		// derived layout may pad, so the padding is defined instead of absent.

		void ConstructOne(const Schema &schema, void *blob) {
			std::memset(blob, 0, schema.Size());
			for (const FieldDescriptor &field : schema.Fields()) {
				if (Owns(field.Type)) {
					new (StringAt(blob, field.Offset)) std::string();
				}
			}
		}

		void DestructOne(const Schema &schema, void *blob) {
			for (const FieldDescriptor &field : schema.Fields()) {
				if (Owns(field.Type)) {
					std::destroy_at(StringAt(blob, field.Offset));
				}
			}
		}

		void CopyOne(const Schema &schema, void *blob, const void *source) {
			std::memset(blob, 0, schema.Size());
			for (const FieldDescriptor &field : schema.Fields()) {
				if (Owns(field.Type)) {
					new (StringAt(blob, field.Offset)) std::string(*StringAt(source, field.Offset));
				} else {
					std::memcpy(FieldAt(blob, field.Offset), FieldAt(source, field.Offset), field.Size);
				}
			}
		}

		void MoveOne(const Schema &schema, void *blob, void *source) {
			std::memset(blob, 0, schema.Size());
			for (const FieldDescriptor &field : schema.Fields()) {
				if (Owns(field.Type)) {
					new (StringAt(blob, field.Offset))
						std::string(std::move(*StringAt(source, field.Offset)));
				} else {
					std::memcpy(FieldAt(blob, field.Offset), FieldAt(source, field.Offset), field.Size);
				}
			}
		}

		// **Field by field, never the object representation**, and the reason is
		// a `Name` field: its value is a process-local id, and `Name.hpp` says
		// never to serialize one. Raw bytes would write that id into a save file
		// and read it back in a process where it means a different string.
		void WriteOne(const Schema &schema, core::ByteWriter &writer, const void *blob) {
			for (const FieldDescriptor &field : schema.Fields()) {
				alignas(8) std::array<std::byte, 8> scratch{};
				const void *value = Schemas::ReadField(blob, field, scratch.data());
				switch (field.Type) {
				case PropertyType::Bool:
					writer.WriteBool(*static_cast<const bool *>(value));
					break;
				case PropertyType::Int32:
					writer.WriteInt32(*static_cast<const int32_t *>(value));
					break;
				case PropertyType::Int64:
					writer.WriteInt64(*static_cast<const int64_t *>(value));
					break;
				case PropertyType::Float:
					writer.WriteFloat(*static_cast<const float *>(value));
					break;
				case PropertyType::Double:
					writer.WriteDouble(*static_cast<const double *>(value));
					break;
				case PropertyType::Name:
				case PropertyType::Enum:
					writer.WriteName(*static_cast<const core::Name *>(value));
					break;
				case PropertyType::String:
					writer.WriteString(*static_cast<const std::string *>(value));
					break;
				case PropertyType::Reference:
					writer.WriteUInt64(static_cast<const Entity *>(value)->Id);
					break;
				default:
					// The value types, which are floats and counts with no
					// interned id anywhere in them. `core::Sequence` zeroes its
					// unused tail for exactly this path.
					writer.WriteRaw(value, field.Size);
					break;
				}
			}
		}

		void ReadOne(const Schema &schema, core::ByteReader &reader, void *blob) {
			for (const FieldDescriptor &field : schema.Fields()) {
				alignas(8) std::array<std::byte, 8> scratch{};
				void *value =
					field.Packing == FieldPacking::Native ? FieldAt(blob, field.Offset) : scratch.data();
				switch (field.Type) {
				case PropertyType::Bool:
					*static_cast<bool *>(value) = reader.ReadBool();
					break;
				case PropertyType::Int32:
					*static_cast<int32_t *>(value) = reader.ReadInt32();
					break;
				case PropertyType::Int64:
					*static_cast<int64_t *>(value) = reader.ReadInt64();
					break;
				case PropertyType::Float:
					*static_cast<float *>(value) = reader.ReadFloat();
					break;
				case PropertyType::Double:
					*static_cast<double *>(value) = reader.ReadDouble();
					break;
				case PropertyType::Name:
				case PropertyType::Enum:
					*static_cast<core::Name *>(value) = reader.ReadName();
					break;
				case PropertyType::String:
					// The value is already constructed - `Read`'s contract is
					// that it reads back over live values - so this assigns.
					*static_cast<std::string *>(value) = reader.ReadString();
					break;
				case PropertyType::Reference:
					static_cast<Entity *>(value)->Id = reader.ReadUInt64();
					break;
				default:
					reader.ReadRaw(value, field.Size);
					break;
				}
				if (field.Packing != FieldPacking::Native) {
					Schemas::WriteField(blob, field, value);
				}
			}
		}

		// --- the registry ----------------------------------------------------

		struct SchemaRegistry {
			std::mutex Guard;

			// A deque because `Of` hands back a pointer and registration
			// continues afterwards - `Components`' own descriptor table is a
			// deque for the same reason, and a vector would turn every schema
			// anybody was holding into a dangling pointer. It is also what makes
			// `Schema::Slot` a stable address, which is the identity the
			// component table compares.
			std::deque<Schema> Entries;
			std::unordered_map<uint32_t, size_t> ByName;
			std::unordered_map<uint32_t, size_t> ByComponent;
		};

		// Never destroyed, for the reason `Components`' registry is not: a
		// column reaches its schema to destroy its rows, and a store held in a
		// static outlives this table under reverse destruction order.
		SchemaRegistry &SchemaRegistryOf() {
			static SchemaRegistry *registry = new SchemaRegistry();
			return *registry;
		}

		// What a generated hook reads, without taking the registry's lock.
		//
		// **A lock in `Destruct` would be a lock per row.** The pointers are
		// written once under `SchemaRegistry::Guard` and read from every tick
		// afterwards, so the array is atomic rather than plain - a plain read
		// racing the registering write is a data race by the letter of the
		// standard even where it happens to be benign.
		std::array<std::atomic<const Schema *>, MAX_SCHEMAS> &Live() {
			static auto *live = new std::array<std::atomic<const Schema *>, MAX_SCHEMAS>{};
			return *live;
		}

		const Schema &LiveAt(size_t index) {
			return *Live()[index].load(std::memory_order_acquire);
		}

		// --- the generated hook sets -----------------------------------------

		// Never inlined, and that is the whole reason the table is affordable.
		//
		// **Measured, because the first version was not.** `Thunks<N>` is meant
		// to be a two-instruction trampoline - load the index, jump - and with
		// these six bodies inlinable the compiler put a copy of each *switch*
		// into every one of them instead. `Schema.cpp` compiled to **7.9 MB of
		// object at 256 slots and 113 MB at 4096**, growing at about 28 KB a
		// slot, which is what made the cap look like a hard constraint rather
		// than a number somebody picked.
		//
		// With the bodies held out of line each thunk is a handful of bytes and
		// the table costs what it looks like it should. The numbers are in
		// `MAX_SCHEMAS` above.
		//
		// MSVC spells it differently and has no attribute syntax for it, which
		// is why this is a macro rather than `[[gnu::noinline]]` at each
		// definition.
#if defined(_MSC_VER)
#define SCHEMA_OUT_OF_LINE __declspec(noinline)
#else
#define SCHEMA_OUT_OF_LINE __attribute__((noinline))
#endif

		SCHEMA_OUT_OF_LINE void ConstructAt(size_t slot, void *destination, size_t count) {
			const Schema &schema = LiveAt(slot);
			auto *bytes = static_cast<std::byte *>(destination);
			for (size_t value = 0; value < count; value++) {
				ConstructOne(schema, bytes + value * schema.Size());
			}
		}

		SCHEMA_OUT_OF_LINE void DestructAt(size_t slot, void *destination, size_t count) {
			const Schema &schema = LiveAt(slot);
			auto *bytes = static_cast<std::byte *>(destination);
			for (size_t value = 0; value < count; value++) {
				DestructOne(schema, bytes + value * schema.Size());
			}
		}

		SCHEMA_OUT_OF_LINE void CopyAt(size_t slot, void *destination, const void *source, size_t count) {
			const Schema &schema = LiveAt(slot);
			auto *bytes = static_cast<std::byte *>(destination);
			const auto *from = static_cast<const std::byte *>(source);
			for (size_t value = 0; value < count; value++) {
				CopyOne(schema, bytes + value * schema.Size(), from + value * schema.Size());
			}
		}

		SCHEMA_OUT_OF_LINE void MoveAt(size_t slot, void *destination, void *source, size_t count) {
			const Schema &schema = LiveAt(slot);
			auto *bytes = static_cast<std::byte *>(destination);
			auto *from = static_cast<std::byte *>(source);
			for (size_t value = 0; value < count; value++) {
				MoveOne(schema, bytes + value * schema.Size(), from + value * schema.Size());
			}
		}

		SCHEMA_OUT_OF_LINE void
		WriteAt(size_t slot, core::ByteWriter &writer, const void *source, size_t count) {
			const Schema &schema = LiveAt(slot);
			const auto *from = static_cast<const std::byte *>(source);
			for (size_t value = 0; value < count; value++) {
				WriteOne(schema, writer, from + value * schema.Size());
			}
		}

		SCHEMA_OUT_OF_LINE void
		ReadAt(size_t slot, core::ByteReader &reader, void *destination, size_t count) {
			const Schema &schema = LiveAt(slot);
			auto *bytes = static_cast<std::byte *>(destination);
			for (size_t value = 0; value < count; value++) {
				ReadOne(schema, reader, bytes + value * schema.Size());
			}
		}

		// One slot's six hooks. Nothing here but the index.
		template <size_t Index> struct Thunks {
			static void DefaultConstruct(void *destination, size_t count) {
				ConstructAt(Index, destination, count);
			}

			static void Destruct(void *destination, size_t count) {
				DestructAt(Index, destination, count);
			}

			static void CopyConstruct(void *destination, const void *source, size_t count) {
				CopyAt(Index, destination, source, count);
			}

			static void MoveConstruct(void *destination, void *source, size_t count) {
				MoveAt(Index, destination, source, count);
			}

			static void Write(core::ByteWriter &writer, const void *source, size_t count) {
				WriteAt(Index, writer, source, count);
			}

			static void Read(core::ByteReader &reader, void *destination, size_t count) {
				ReadAt(Index, reader, destination, count);
			}
		};

		// The six pointers one slot's hooks resolve to.
		struct HookSet {
			void (*DefaultConstruct)(void *, size_t);
			void (*Destruct)(void *, size_t);
			void (*CopyConstruct)(void *, const void *, size_t);
			void (*MoveConstruct)(void *, void *, size_t);
			void (*Write)(core::ByteWriter &, const void *, size_t);
			void (*Read)(core::ByteReader &, void *, size_t);
		};

		template <size_t... Indices>
		constexpr std::array<HookSet, sizeof...(Indices)> BuildHooks(std::index_sequence<Indices...>) {
			return {HookSet{
				&Thunks<Indices>::DefaultConstruct,
				&Thunks<Indices>::Destruct,
				&Thunks<Indices>::CopyConstruct,
				&Thunks<Indices>::MoveConstruct,
				&Thunks<Indices>::Write,
				&Thunks<Indices>::Read,
			}...};
		}

		const std::array<HookSet, MAX_SCHEMAS> &Hooks() {
			static const auto hooks = BuildHooks(std::make_index_sequence<MAX_SCHEMAS>{});
			return hooks;
		}

		// Whether two field sets say the same thing.
		//
		// Order is deliberately not part of it: the layout is derived, so two
		// callers naming the same fields in different orders have declared the
		// same component rather than two.
		bool Same(const Schema &schema, std::span<const FieldDescriptor> fields) {
			if (schema.Fields().size() != fields.size()) {
				return false;
			}
			for (const FieldDescriptor &wanted : fields) {
				const FieldDescriptor *held = schema.Find(wanted.Name);
				if (held == nullptr || held->Type != wanted.Type || held->Enum != wanted.Enum ||
					held->Packing != wanted.Packing) {
					return false;
				}
			}
			return true;
		}
	}

	const char *Describe(FieldPacking packing) {
		switch (packing) {
		case FieldPacking::Native:
			return "native";
		case FieldPacking::Float16:
			return "float16";
		case FieldPacking::UFloat16:
			return "ufloat16";
		case FieldPacking::Float8:
			return "float8";
		case FieldPacking::UFloat8:
			return "ufloat8";
		case FieldPacking::Int16:
			return "int16";
		case FieldPacking::UInt16:
			return "uint16";
		case FieldPacking::Int8:
			return "int8";
		case FieldPacking::UInt8:
			return "uint8";
		case FieldPacking::Int4:
			return "int4";
		case FieldPacking::UInt4:
			return "uint4";
		case FieldPacking::Bool:
			return "bool";
		}
		return "native";
	}

	// --- Schema ------------------------------------------------------------

	const FieldDescriptor *Schema::Find(core::Name field) const {
		for (const FieldDescriptor &held : Layout) {
			if (held.Name == field) {
				return &held;
			}
		}
		return nullptr;
	}

	const FieldDescriptor *Schema::Find(std::string_view field) const {
		for (const FieldDescriptor &held : Layout) {
			if (held.Spelling == field) {
				return &held;
			}
		}
		return nullptr;
	}

	std::vector<std::string> Schema::Tags() const {
		return TagNames;
	}

	// --- Schemas -----------------------------------------------------------

	uint32_t Schemas::SizeOf(PropertyType type) {
		return LayoutOf(type).Size;
	}

	bool Schemas::TypeNamed(std::string_view spelling, PropertyType &out) {
		// **The engine's own spellings plus the three a script reaches for.**
		// `Describe(PropertyType)` is what the manifest prints, so accepting
		// those means a field type read out of a manifest can be written back
		// into a declaration without a translation table in between.
		static const struct {
			std::string_view Spelling;
			PropertyType Type;
		} NAMES[] = {
			{"bool", PropertyType::Bool},
			{"boolean", PropertyType::Bool},
			{"int32", PropertyType::Int32},
			{"int", PropertyType::Int32},
			{"int64", PropertyType::Int64},
			{"float", PropertyType::Float},
			{"double", PropertyType::Double},

			// A script's `number` is a double in both runtimes. An author who
			// wants four bytes per row says `float` and means it.
			{"number", PropertyType::Double},

			// Both spellings of the two that `Describe(PropertyType)` prints in
			// lower case, so a field list read back out of a schema or a
			// manifest can be handed straight back to `Register` without a
			// translation table sitting between them.
			{"Name", PropertyType::Name},
			{"name", PropertyType::Name},
			{"String", PropertyType::String},
			{"string", PropertyType::String},
			{"Instance", PropertyType::Reference},
			{"reference", PropertyType::Reference},
			{"Vector3", PropertyType::Vector3},
			{"CFrame", PropertyType::CFrame},
			{"Color3", PropertyType::Color3},
			{"Vector2", PropertyType::Vector2},
			{"UDim", PropertyType::UDim},
			{"UDim2", PropertyType::UDim2},
			{"Rect", PropertyType::Rect},
			{"NumberRange", PropertyType::NumberRange},
			{"NumberSequence", PropertyType::NumberSequence},
			{"ColorSequence", PropertyType::ColorSequence},
		};

		for (const auto &named : NAMES) {
			if (named.Spelling == spelling) {
				out = named.Type;
				return true;
			}
		}
		return false;
	}

	bool Schemas::FieldTypeNamed(std::string_view spelling, PropertyType &type, FieldPacking &packing) {
		static const struct {
			std::string_view Spelling;
			PropertyType Type;
			FieldPacking Packing;
		} PACKED[] = {
			{"float16", PropertyType::Float, FieldPacking::Float16},
			{"ufloat16", PropertyType::Float, FieldPacking::UFloat16},
			{"float8", PropertyType::Float, FieldPacking::Float8},
			{"ufloat8", PropertyType::Float, FieldPacking::UFloat8},
			{"int16", PropertyType::Int32, FieldPacking::Int16},
			{"uint16", PropertyType::Int32, FieldPacking::UInt16},
			{"int8", PropertyType::Int32, FieldPacking::Int8},
			{"uint8", PropertyType::Int32, FieldPacking::UInt8},
			{"int4", PropertyType::Int32, FieldPacking::Int4},
			{"uint4", PropertyType::Int32, FieldPacking::UInt4},
		};

		for (const auto &named : PACKED) {
			if (named.Spelling == spelling) {
				type = named.Type;
				packing = named.Packing;
				return true;
			}
		}

		if (!TypeNamed(spelling, type)) {
			return false;
		}
		packing = type == PropertyType::Bool ? FieldPacking::Bool : FieldPacking::Native;
		return true;
	}

	const void *Schemas::ReadField(const void *component, const FieldDescriptor &field, void *scratch) {
		if (component == nullptr) {
			return nullptr;
		}
		if (field.Packing == FieldPacking::Native) {
			return FieldAt(component, field.Offset);
		}
		if (scratch == nullptr) {
			return nullptr;
		}

		const uint32_t bits = ReadBits(component, field);
		switch (field.Packing) {
		case FieldPacking::Float16:
			*static_cast<float *>(scratch) = UnpackFloat16(static_cast<uint16_t>(bits));
			break;
		case FieldPacking::UFloat16:
			*static_cast<float *>(scratch) = static_cast<float>(bits) / 65535.0f;
			break;
		case FieldPacking::Float8:
			*static_cast<float *>(scratch) = std::max(
				-1.0f,
				static_cast<float>(
					(bits & 0x80u) != 0 ? static_cast<int32_t>(bits) - 256 : static_cast<int32_t>(bits)
				) / 127.0f
			);
			break;
		case FieldPacking::UFloat8:
			*static_cast<float *>(scratch) = static_cast<float>(bits) / 255.0f;
			break;
		case FieldPacking::Int16:
			*static_cast<int32_t *>(scratch) =
				(bits & 0x8000u) != 0 ? static_cast<int32_t>(bits) - 65536 : static_cast<int32_t>(bits);
			break;
		case FieldPacking::UInt16:
			*static_cast<int32_t *>(scratch) = static_cast<uint16_t>(bits);
			break;
		case FieldPacking::Int8:
			*static_cast<int32_t *>(scratch) =
				(bits & 0x80u) != 0 ? static_cast<int32_t>(bits) - 256 : static_cast<int32_t>(bits);
			break;
		case FieldPacking::UInt8:
			*static_cast<int32_t *>(scratch) = static_cast<uint8_t>(bits);
			break;
		case FieldPacking::Int4:
			*static_cast<int32_t *>(scratch) =
				(bits & 0x8u) != 0 ? static_cast<int32_t>(bits) - 16 : static_cast<int32_t>(bits);
			break;
		case FieldPacking::UInt4:
			*static_cast<int32_t *>(scratch) = static_cast<int32_t>(bits);
			break;
		case FieldPacking::Bool:
			*static_cast<bool *>(scratch) = bits != 0;
			break;
		case FieldPacking::Native:
			break;
		}
		return scratch;
	}

	bool Schemas::WriteField(void *component, const FieldDescriptor &field, const void *value) {
		if (component == nullptr || value == nullptr) {
			return false;
		}
		if (field.Packing == FieldPacking::Native) {
			if (Owns(field.Type)) {
				*StringAt(component, field.Offset) = *static_cast<const std::string *>(value);
			} else {
				std::memcpy(FieldAt(component, field.Offset), value, LayoutOf(field.Type).Size);
			}
			return true;
		}

		uint32_t bits = 0;
		switch (field.Packing) {
		case FieldPacking::Float16:
			bits = PackFloat16(*static_cast<const float *>(value));
			break;
		case FieldPacking::UFloat16: {
			const float source = *static_cast<const float *>(value);
			const float finite = std::isnan(source) ? 0.0f : source;
			bits = static_cast<uint32_t>(std::lround(std::clamp(finite, 0.0f, 1.0f) * 65535.0f));
			break;
		}
		case FieldPacking::Float8: {
			const float source = *static_cast<const float *>(value);
			const float finite = std::isnan(source) ? 0.0f : source;
			bits = static_cast<uint8_t>(std::lround(std::clamp(finite, -1.0f, 1.0f) * 127.0f));
			break;
		}
		case FieldPacking::UFloat8: {
			const float source = *static_cast<const float *>(value);
			const float finite = std::isnan(source) ? 0.0f : source;
			bits = static_cast<uint32_t>(std::lround(std::clamp(finite, 0.0f, 1.0f) * 255.0f));
			break;
		}
		case FieldPacking::Int16:
			bits = static_cast<uint16_t>(Saturate<int16_t>(*static_cast<const int32_t *>(value)));
			break;
		case FieldPacking::UInt16:
			bits = Saturate<uint16_t>(*static_cast<const int32_t *>(value));
			break;
		case FieldPacking::Int8:
			bits = static_cast<uint8_t>(Saturate<int8_t>(*static_cast<const int32_t *>(value)));
			break;
		case FieldPacking::UInt8:
			bits = Saturate<uint8_t>(*static_cast<const int32_t *>(value));
			break;
		case FieldPacking::Int4:
			bits = static_cast<uint32_t>(std::clamp(*static_cast<const int32_t *>(value), -8, 7)) & 0xfu;
			break;
		case FieldPacking::UInt4:
			bits = static_cast<uint32_t>(std::clamp(*static_cast<const int32_t *>(value), 0, 15));
			break;
		case FieldPacking::Bool:
			bits = *static_cast<const bool *>(value) ? 1u : 0u;
			break;
		case FieldPacking::Native:
			break;
		}
		WriteBits(component, field, bits);
		return true;
	}

	Schemas::Result Schemas::Register(std::string_view name, std::span<const FieldSpec> fields) {
		if (name.empty()) {
			return {{}, Status::Unnamed, false};
		}

		// Built before the lock is taken, because none of it touches the table
		// and a refusal should not have queued behind a registration.
		std::vector<FieldDescriptor> layout;
		layout.reserve(fields.size());

		for (const FieldSpec &spec : fields) {
			const ValueLayout value = LayoutOf(spec.Type);
			const FieldPacking packing =
				spec.Type == PropertyType::Bool && spec.Packing == FieldPacking::Native ? FieldPacking::Bool
																						: spec.Packing;
			if (spec.Name.empty() || value.Size == 0 || !PackingFits(packing, spec.Type)) {
				return {{}, Status::BadField, false};
			}

			FieldDescriptor field;
			field.Name = core::Name(spec.Name);
			field.Spelling = field.Name.Text();
			field.Type = spec.Type;
			field.Enum = spec.Enum.empty() ? core::Name{} : core::Name(spec.Enum);
			field.Packing = packing;
			field.StorageBits = BitsOf(packing, spec.Type);
			field.Size = static_cast<uint32_t>((field.StorageBits + 7) / 8);

			for (const FieldDescriptor &held : layout) {
				if (held.Name == field.Name) {
					return {{}, Status::DuplicateField, false};
				}
			}

			layout.push_back(field);
		}

		// **Native alignment first, then by name, and never the caller's order.** A Luau
		// table iterates in hash order, so a layout that followed the caller
		// would differ between two runs of one script - and a snapshot written
		// by one of them would not be readable by the other. Sorting by
		// alignment also removes most of the padding a naive order would leave.
		// Packed values are byte-accessed, so they need no artificial alignment;
		// sub-byte values sort last and consume one shared bit stream.
		std::sort(layout.begin(), layout.end(), [](const FieldDescriptor &a, const FieldDescriptor &b) {
			const uint32_t left = a.Packing == FieldPacking::Native ? LayoutOf(a.Type).Align : 1;
			const uint32_t right = b.Packing == FieldPacking::Native ? LayoutOf(b.Type).Align : 1;
			if (left != right) {
				return left > right;
			}
			if ((a.StorageBits < 8) != (b.StorageBits < 8)) {
				return a.StorageBits >= 8;
			}
			return a.Spelling < b.Spelling;
		});

		uint32_t offset = 0;
		uint32_t alignment = 1;
		bool trivial = true;
		for (FieldDescriptor &field : layout) {
			if (field.StorageBits < 8) {
				continue;
			}
			const ValueLayout value =
				field.Packing == FieldPacking::Native ? LayoutOf(field.Type) : ValueLayout{field.Size, 1};
			offset = (offset + value.Align - 1) / value.Align * value.Align;
			field.Offset = offset;
			offset += value.Size;
			alignment = std::max(alignment, value.Align);
			trivial = trivial && !Owns(field.Type);
		}

		uint32_t bit = offset * 8;
		for (FieldDescriptor &field : layout) {
			if (field.StorageBits >= 8) {
				continue;
			}
			field.Offset = bit / 8;
			field.BitOffset = static_cast<uint8_t>(bit % 8);
			field.Size = static_cast<uint32_t>((field.BitOffset + field.StorageBits + 7) / 8);
			bit += field.StorageBits;
		}
		offset = (bit + 7) / 8;

		// Rounded up so that an array of these values keeps every field aligned.
		// A component with no fields is a tag, and `Column` is asked for no
		// bytes at all.
		const uint32_t width = layout.empty() ? 0 : (offset + alignment - 1) / alignment * alignment;

		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);

		const core::Name key(name);
		if (const auto found = registry.ByName.find(key.Id()); found != registry.ByName.end()) {
			Schema &held = registry.Entries[found->second];
			const bool same = Same(held, layout);
			return {same ? held.Slot : ComponentId{}, same ? Status::Ok : Status::Conflict, false};
		}

		// A name a C++ type already claimed is the same collision as a schema
		// claiming it twice, and it has to be caught here: `Components::Adopt`
		// answers it by aborting, which is right for two C++ types and wrong for
		// a script that mistyped a name.
		if (Components::Find(key).IsValid()) {
			return {{}, Status::Conflict, false};
		}

		if (Components::Sealed()) {
			return {{}, Status::Sealed, false};
		}

		if (registry.Entries.size() >= MAX_SCHEMAS) {
			return {{}, Status::Exhausted, false};
		}

		// **Read before the move below empties `layout`.** The descriptor's
		// `Kind` was being decided from `layout.empty()` afterwards, which is
		// always true once the vector has been moved from - so every component
		// a script declared was registered as a tag while holding bytes.
		// Nothing read `Kind` yet, which is the only reason it never showed;
		// `engine.ecs.invariants` is what says so now.
		const bool fieldless = layout.empty();

		const size_t index = registry.Entries.size();
		Schema &schema = registry.Entries.emplace_back();
		schema.TypeName = key;
		schema.Layout = std::move(layout);
		schema.Width = width;
		schema.Align = alignment;

		// Published before the descriptor is built, because the hooks the
		// descriptor names read this array and `Components::RegisterDescribed`
		// is free to be called on a thread that is already ticking.
		Live()[index].store(&schema, std::memory_order_release);

		const HookSet &hooks = Hooks()[index];

		TypeDescriptor descriptor;
		descriptor.Name = key;
		descriptor.Size = width;
		descriptor.Alignment = alignment;
		descriptor.Kind = fieldless ? ComponentKind::Tag : ComponentKind::Data;

		// **Trivial says the caller *may* skip the hooks, not that they do not
		// exist.** A blob of value types is a memcpy for the storage, which is
		// the fast path every declared component takes; a blob holding a string
		// is not, and `Column` reads this flag to decide.
		descriptor.Trivial = trivial;
		descriptor.Serialisable = true;
		descriptor.DefaultConstruct = hooks.DefaultConstruct;
		descriptor.Destruct = hooks.Destruct;
		descriptor.CopyConstruct = hooks.CopyConstruct;
		descriptor.MoveConstruct = hooks.MoveConstruct;
		descriptor.Write = hooks.Write;
		descriptor.Read = hooks.Read;

		const ComponentId id = Components::RegisterDescribed(name, descriptor, schema.Slot);

		registry.ByName.emplace(key.Id(), index);
		registry.ByComponent.emplace(id.Index, index);

		return {id, Status::Ok, true};
	}

	const Schema *Schemas::Of(ComponentId component) {
		if (!component.IsValid()) {
			return nullptr;
		}

		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);

		const auto found = registry.ByComponent.find(component.Index);
		return found == registry.ByComponent.end() ? nullptr : &registry.Entries[found->second];
	}

	const Schema *Schemas::Find(core::Name name) {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);

		const auto found = registry.ByName.find(name.Id());
		return found == registry.ByName.end() ? nullptr : &registry.Entries[found->second];
	}

	bool Schemas::SetTags(ComponentId component, std::span<const std::string_view> tags) {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);
		const auto found = registry.ByComponent.find(component.Index);
		if (found == registry.ByComponent.end()) {
			return false;
		}

		Schema &schema = registry.Entries[found->second];
		schema.TagNames.clear();
		schema.TagNames.reserve(tags.size());
		for (const std::string_view tag : tags) {
			if (!tag.empty()) {
				schema.TagNames.emplace_back(tag);
			}
		}
		return true;
	}

	bool
	Schemas::SetFieldTags(ComponentId component, core::Name field, std::span<const std::string_view> tags) {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);
		const auto found = registry.ByComponent.find(component.Index);
		if (found == registry.ByComponent.end()) {
			return false;
		}

		Schema &schema = registry.Entries[found->second];
		FieldDescriptor *descriptor = nullptr;
		for (FieldDescriptor &candidate : schema.Layout) {
			if (candidate.Name == field) {
				descriptor = &candidate;
				break;
			}
		}
		if (descriptor == nullptr) {
			return false;
		}

		descriptor->Tags.clear();
		descriptor->Tags.reserve(tags.size());
		for (const std::string_view tag : tags) {
			if (!tag.empty()) {
				descriptor->Tags.emplace_back(tag);
			}
		}
		return true;
	}

	bool Schemas::SetFieldExposed(ComponentId component, core::Name field, bool exposed) {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);
		const auto found = registry.ByComponent.find(component.Index);
		if (found == registry.ByComponent.end()) {
			return false;
		}
		FieldDescriptor *descriptor = nullptr;
		for (FieldDescriptor &candidate : registry.Entries[found->second].Layout) {
			if (candidate.Name == field) {
				descriptor = &candidate;
				break;
			}
		}
		if (descriptor == nullptr) {
			return false;
		}
		descriptor->Exposed = exposed;
		return true;
	}

	std::vector<std::string> Schemas::Tags(ComponentId component) {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);
		const auto found = registry.ByComponent.find(component.Index);
		return found == registry.ByComponent.end() ? std::vector<std::string>{}
												   : registry.Entries[found->second].Tags();
	}

	std::vector<std::string> Schemas::FieldTags(ComponentId component, core::Name field) {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);
		const auto found = registry.ByComponent.find(component.Index);
		if (found == registry.ByComponent.end()) {
			return {};
		}
		const FieldDescriptor *descriptor = registry.Entries[found->second].Find(field);
		return descriptor == nullptr ? std::vector<std::string>{} : descriptor->Tags;
	}

	std::vector<ComponentId> Schemas::All() {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);

		std::vector<ComponentId> ids;
		ids.reserve(registry.Entries.size());
		for (const Schema &schema : registry.Entries) {
			ids.push_back(schema.Slot);
		}
		return ids;
	}

	void Schemas::Clear() {
		auto &registry = SchemaRegistryOf();
		std::lock_guard lock(registry.Guard);

		for (size_t index = 0; index < registry.Entries.size(); index++) {
			Live()[index].store(nullptr, std::memory_order_release);
		}

		registry.Entries.clear();
		registry.ByName.clear();
		registry.ByComponent.clear();
	}
}

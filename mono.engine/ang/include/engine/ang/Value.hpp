#pragma once

// Values that travel along node graph links.
//
// Scalars are stored inline. Anything else is a `Blob`: a host object behind a
// shared pointer, so moving a result into the cache and handing it to several
// readers never copies the payload. A copy of a Value is a pointer copy.
// @tier L3 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace engine::ang {

	// Durable text name of a data type. Saved and compared as text, never as an ordinal.
	using TypeId = std::string;

	// The one wildcard type. `data.ANY` from older documents reads as this.
	inline constexpr std::string_view ANY_TYPE = "any";

	// Four floats, for colours with alpha and homogeneous vectors.
	struct Vector4 {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		float W = 0.0f;

		friend bool operator==(const Vector4 &, const Vector4 &) = default;
	};

	// A host-owned payload. `Identity` stands in for the content in cache keys:
	// the host sets it to a content hash or a version that changes when the
	// payload does. Zero means "unknown", and a node fed one is never cached.
	struct Blob {
		std::shared_ptr<const void> Data{};
		size_t Bytes = 0;
		uint64_t Identity = 0;

		// Same payload object and identity. Contents are never compared.
		friend bool operator==(const Blob &left, const Blob &right) {
			return left.Data == right.Data && left.Identity == right.Identity;
		}
	};

	// Tagged value with its type name. Empty means "no value".
	class Value {
	  public:
		using Storage = std::variant<
			std::monostate,
			bool,
			int64_t,
			double,
			core::Vector2,
			core::Vector3,
			Vector4,
			core::Color3,
			std::string,
			core::Name,
			Blob>;

		Value() = default;
		Value(bool value) : Type("bool"), Data(value) {}
		Value(int64_t value) : Type("int"), Data(value) {}
		Value(int value) : Type("int"), Data(int64_t{value}) {}
		Value(double value) : Type("number"), Data(value) {}
		Value(core::Vector2 value) : Type("vector2"), Data(value) {}
		Value(core::Vector3 value) : Type("vector3"), Data(value) {}
		Value(Vector4 value) : Type("vector4"), Data(value) {}
		Value(core::Color3 value) : Type("color3"), Data(value) {}
		Value(std::string value) : Type("string"), Data(std::move(value)) {}
		Value(const char *value) : Type("string"), Data(std::string(value)) {}
		Value(core::Name value) : Type("name"), Data(value) {}

		// Wraps a host object. The type is the host's registered TypeId.
		static Value FromBlob(TypeId type, Blob blob);

		// Retags a scalar, e.g. a double the host registered as "meters".
		Value WithType(TypeId type) const;

		bool Empty() const {
			return std::holds_alternative<std::monostate>(Data);
		}
		const TypeId &TypeName() const {
			return Type;
		}
		const Storage &Raw() const {
			return Data;
		}

		// Typed access; null when the stored kind differs.
		template <typename T> const T *Get() const {
			return std::get_if<T>(&Data);
		}

		// Numeric read that accepts int, double and bool. Returns `fallback` otherwise.
		double AsNumber(double fallback = 0.0) const;

		// Approximate resident size: inline size for scalars, string capacity,
		// or the blob's declared Bytes.
		size_t Bytes() const;

		// Stable 64-bit content hash. Blobs contribute their Identity, never their
		// address. Returns zero for a blob with unknown identity.
		uint64_t Hash() const;

		// True for a blob whose identity is unknown, which makes it uncacheable.
		bool Opaque() const;

		friend bool operator==(const Value &left, const Value &right);

	  private:
		TypeId Type{};
		Storage Data{};
	};

	// FNV-1a over bytes, used for every ANG hash so results match across runs.
	uint64_t HashBytes(uint64_t seed, const void *data, size_t size);

	// Mixes text with its length so "ab"+"c" and "a"+"bc" differ.
	uint64_t HashText(uint64_t seed, std::string_view text);

	inline constexpr uint64_t HASH_SEED = 0xcbf29ce484222325ull;

	// Folds one 64-bit value into a running hash.
	inline uint64_t HashMix(uint64_t hash, uint64_t value) {
		return HashBytes(hash, &value, sizeof(value));
	}
}

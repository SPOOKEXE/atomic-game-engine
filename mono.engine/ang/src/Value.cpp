#include <engine/ang/Value.hpp>

#include <cstring>

namespace engine::ang {

	uint64_t HashBytes(uint64_t seed, const void *data, size_t size) {
		const auto *bytes = static_cast<const unsigned char *>(data);
		uint64_t hash = seed;
		for (size_t index = 0; index < size; ++index) {
			hash ^= bytes[index];
			hash *= 0x100000001b3ull;
		}
		return hash;
	}

	uint64_t HashText(uint64_t seed, std::string_view text) {
		const uint64_t length = text.size();
		return HashBytes(HashBytes(seed, &length, sizeof(length)), text.data(), text.size());
	}

	Value Value::FromBlob(TypeId type, Blob blob) {
		Value value;
		value.Type = std::move(type);
		value.Data = std::move(blob);
		return value;
	}

	Value Value::WithType(TypeId type) const {
		Value value = *this;
		value.Type = std::move(type);
		return value;
	}

	double Value::AsNumber(double fallback) const {
		if (const auto *number = Get<double>()) {
			return *number;
		}
		if (const auto *integer = Get<int64_t>()) {
			return static_cast<double>(*integer);
		}
		if (const auto *flag = Get<bool>()) {
			return *flag ? 1.0 : 0.0;
		}
		return fallback;
	}

	size_t Value::Bytes() const {
		if (const auto *text = Get<std::string>()) {
			return sizeof(Value) + text->capacity();
		}
		if (const auto *blob = Get<Blob>()) {
			return sizeof(Value) + blob->Bytes;
		}
		return Empty() ? 0 : sizeof(Value);
	}

	bool Value::Opaque() const {
		const auto *blob = Get<Blob>();
		return blob != nullptr && blob->Identity == 0;
	}

	uint64_t Value::Hash() const {
		uint64_t hash = HashText(HASH_SEED, Type);
		const uint64_t kind = Data.index();
		hash = HashBytes(hash, &kind, sizeof(kind));
		// Each alternative hashes its fields explicitly; padding bytes never enter.
		return std::visit(
			[hash](const auto &held) -> uint64_t {
				using Held = std::decay_t<decltype(held)>;
				if constexpr (std::is_same_v<Held, std::monostate>) {
					return hash;
				} else if constexpr (std::is_same_v<Held, bool>) {
					const unsigned char flag = held ? 1 : 0;
					return HashBytes(hash, &flag, 1);
				} else if constexpr (std::is_same_v<Held, int64_t> || std::is_same_v<Held, double>) {
					return HashBytes(hash, &held, sizeof(held));
				} else if constexpr (std::is_same_v<Held, core::Vector2>) {
					const float parts[2]{held.X, held.Y};
					return HashBytes(hash, parts, sizeof(parts));
				} else if constexpr (std::is_same_v<Held, core::Vector3>) {
					const float parts[3]{held.X, held.Y, held.Z};
					return HashBytes(hash, parts, sizeof(parts));
				} else if constexpr (std::is_same_v<Held, Vector4>) {
					const float parts[4]{held.X, held.Y, held.Z, held.W};
					return HashBytes(hash, parts, sizeof(parts));
				} else if constexpr (std::is_same_v<Held, core::Color3>) {
					const float parts[3]{held.R, held.G, held.B};
					return HashBytes(hash, parts, sizeof(parts));
				} else if constexpr (std::is_same_v<Held, std::string>) {
					return HashText(hash, held);
				} else if constexpr (std::is_same_v<Held, core::Name>) {
					// Text, not Id(): the interned id depends on interning order.
					return HashText(hash, held.Text());
				} else {
					if (held.Identity == 0) {
						return 0;
					}
					return HashBytes(hash, &held.Identity, sizeof(held.Identity));
				}
			},
			Data
		);
	}

	bool operator==(const Value &left, const Value &right) {
		if (left.Type != right.Type || left.Data.index() != right.Data.index()) {
			return false;
		}
		return left.Data == right.Data;
	}
}

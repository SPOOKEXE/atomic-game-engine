#include "Encoding.hpp"

#include <engine/procedural/Canonical.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <span>
#include <utility>

namespace engine::procedural {

	namespace {
		struct Reader {
			std::span<const uint8_t> Bytes{};
			size_t Offset = 0;

			bool Raw(size_t size, std::span<const uint8_t> &out) {
				if (size > Bytes.size() - Offset) return false;
				out = Bytes.subspan(Offset, size);
				Offset += size;
				return true;
			}

			bool U8(uint8_t &value) {
				if (Offset == Bytes.size()) return false;
				value = Bytes[Offset++];
				return true;
			}

			bool U16(uint16_t &value) {
				uint8_t low;
				uint8_t high;
				if (!U8(low) || !U8(high)) return false;
				value = static_cast<uint16_t>(low | static_cast<uint16_t>(high) << 8);
				return true;
			}

			bool U32(uint32_t &value) {
				value = 0;
				for (unsigned shift = 0; shift < 32; shift += 8) {
					uint8_t byte;
					if (!U8(byte)) return false;
					value |= static_cast<uint32_t>(byte) << shift;
				}
				return true;
			}

			bool U64(uint64_t &value) {
				value = 0;
				for (unsigned shift = 0; shift < 64; shift += 8) {
					uint8_t byte;
					if (!U8(byte)) return false;
					value |= static_cast<uint64_t>(byte) << shift;
				}
				return true;
			}

			bool Text(std::string &value, size_t maximum) {
				uint16_t size;
				std::span<const uint8_t> raw;
				if (!U16(size) || size > maximum || !Raw(size, raw)) return false;
				if (raw.empty()) {
					value.clear();
					return true;
				}
				value.assign(reinterpret_cast<const char *>(raw.data()), raw.size());
				return true;
			}

			bool Real(double &value) {
				uint64_t bits;
				if (!U64(bits)) return false;
				value = std::bit_cast<double>(bits);
				return std::isfinite(value) && !(value == 0.0 && std::signbit(value));
			}
		};

		bool ReadValue(Reader &reader, ParameterData &out, const CanonicalLimits &limits) {
			std::string tag;
			if (!reader.Text(tag, 7)) return false;
			if (tag == "bool") {
				uint8_t value;
				if (!reader.U8(value) || value > 1) return false;
				out = value == 1;
			} else if (tag == "i64") {
				uint64_t value;
				if (!reader.U64(value)) return false;
				out = std::bit_cast<int64_t>(value);
			} else if (tag == "u64") {
				uint64_t value;
				if (!reader.U64(value)) return false;
				out = value;
			} else if (tag == "f64") {
				double value;
				if (!reader.Real(value)) return false;
				out = value;
			} else if (tag == "vector2") {
				Vector2 value;
				for (double &number : value.Values)
					if (!reader.Real(number)) return false;
				out = value;
			} else if (tag == "vector3") {
				Vector3 value;
				for (double &number : value.Values)
					if (!reader.Real(number)) return false;
				out = value;
			} else if (tag == "vector4") {
				Vector4 value;
				for (double &number : value.Values)
					if (!reader.Real(number)) return false;
				out = value;
			} else if (tag == "text") {
				StableText value;
				if (!reader.Text(value.Value, limits.MaximumNameBytes)) return false;
				out = std::move(value);
			} else if (tag == "bytes") {
				uint32_t size;
				std::span<const uint8_t> raw;
				if (!reader.U32(size) || size > limits.MaximumValueBytes || !reader.Raw(size, raw))
					return false;
				ByteBuffer value;
				value.Value.assign(raw.begin(), raw.end());
				out = std::move(value);
			} else {
				return false;
			}
			return true;
		}

		bool ReadParameters(
			Reader &reader, std::vector<Parameter> &out, size_t &total, const CanonicalLimits &limits
		) {
			uint16_t count;
			if (!reader.U16(count) || count > limits.MaximumParameters - total) return false;
			total += count;
			out.reserve(count);
			for (uint16_t index = 0; index < count; ++index) {
				Parameter parameter;
				if (!reader.Text(parameter.Name, limits.MaximumNameBytes) ||
					!ReadValue(reader, parameter.Value, limits))
					return false;
				out.push_back(std::move(parameter));
			}
			return true;
		}
	}

	Status DecodeRecipe(
		std::span<const uint8_t> bytes, Recipe &out, Diagnostic &diagnostic, const CanonicalLimits &limits
	) {
		if (limits.MaximumParameters > 64 || limits.MaximumNodes > 8 || limits.MaximumLinks > 16 ||
			limits.MaximumNameBytes > 128 || limits.MaximumValueBytes > 4096 || limits.MaximumBytes > 32768 ||
			bytes.size() > limits.MaximumBytes)
			return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.bytes");
		try {
			Reader reader{bytes};
			std::span<const uint8_t> magic;
			if (!reader.Raw(4, magic) || !std::equal(magic.begin(), magic.end(), "REC1"))
				return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.magic");
			Recipe candidate;
			if (!reader.Text(candidate.Name, limits.MaximumNameBytes) ||
				!reader.Text(candidate.Generator, limits.MaximumNameBytes) ||
				!reader.U32(candidate.SchemaVersion) || !reader.U64(candidate.RootSeed))
				return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.identity");
			size_t parameterCount = 0;
			if (!ReadParameters(reader, candidate.Parameters, parameterCount, limits))
				return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.parameters");
			uint16_t nodeCount;
			if (!reader.U16(nodeCount) || nodeCount > limits.MaximumNodes)
				return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.nodes");
			candidate.Nodes.reserve(nodeCount);
			for (uint16_t index = 0; index < nodeCount; ++index) {
				OperationNode node;
				if (!reader.Text(node.Id, limits.MaximumNameBytes) ||
					!reader.Text(node.Kind, limits.MaximumNameBytes) ||
					!ReadParameters(reader, node.Parameters, parameterCount, limits))
					return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.node");
				candidate.Nodes.push_back(std::move(node));
			}
			uint16_t linkCount;
			if (!reader.U16(linkCount) || linkCount > limits.MaximumLinks)
				return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.links");
			candidate.Links.reserve(linkCount);
			for (uint16_t index = 0; index < linkCount; ++index) {
				OperationLink link;
				if (!reader.Text(link.FromNode, limits.MaximumNameBytes) ||
					!reader.Text(link.FromPort, limits.MaximumNameBytes) ||
					!reader.Text(link.ToNode, limits.MaximumNameBytes) ||
					!reader.Text(link.ToPort, limits.MaximumNameBytes))
					return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.link");
				candidate.Links.push_back(std::move(link));
			}
			if (reader.Offset != bytes.size())
				return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.trailing");
			std::vector<uint8_t> canonical;
			const Status status = EncodeRecipe(candidate, canonical, diagnostic, limits);
			if (status != Status::Ok) return status;
			if (!std::equal(bytes.begin(), bytes.end(), canonical.begin(), canonical.end()))
				return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.canonical");
			out = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}
}

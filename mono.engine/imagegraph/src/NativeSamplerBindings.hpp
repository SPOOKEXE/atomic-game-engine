#pragma once
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <optional>
#include <string_view>

namespace engine::imagegraph::detail {
	struct NativeSamplerFault {
		Status Code;
		std::string_view Message, Argument;
	};
	// No cooked-program dependency goes upward from imagegraph. Actual Sampler2D name/type matching occurs at
	// capture.
	inline std::optional<NativeSamplerFault>
	ValidateNativeSamplerBindings(const Node &node, uint32_t version = 9) {
		if (node.NativeSamplerBindings.empty()) return {};
		if (version < 9)
			return NativeSamplerFault{
				Status::UnsupportedVersion, "native sampler bindings require imagegraph v9", {}
			};
		if (node.Type != "pc.hlsl")
			return NativeSamplerFault{Status::InvalidValue, "native sampler bindings require pc.hlsl", {}};
		if (node.NativeSamplerBindings.size() > Limits::MaximumNativeSamplerBindingsPerNode)
			return NativeSamplerFault{
				Status::LimitExceeded, "native sampler binding count exceeds its limit", {}
			};
		for (size_t index = 0; index < node.NativeSamplerBindings.size(); ++index) {
			const auto &binding = node.NativeSamplerBindings[index];
			if (binding.Argument.size() > Limits::MaximumNativeSamplerArgumentBytes ||
				binding.Texture.size() > Limits::MaximumNativeSamplerTextureBytes)
				return NativeSamplerFault{
					Status::LimitExceeded, "native sampler binding name exceeds its limit", binding.Argument
				};
			if (binding.Argument.empty() || binding.Texture.empty() ||
				binding.Argument.find('\0') != std::string::npos ||
				binding.Texture.find('\0') != std::string::npos)
				return NativeSamplerFault{
					Status::InvalidValue,
					"native sampler binding needs nonempty names without NUL",
					binding.Argument
				};
			for (size_t i = 0; i < binding.Argument.size(); ++i) {
				const char c = binding.Argument[i];
				if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					  (i && c >= '0' && c <= '9')))
					return NativeSamplerFault{
						Status::InvalidValue, "native sampler argument is not an identifier", binding.Argument
					};
			}
			for (size_t prior = 0; prior < index; ++prior)
				if (node.NativeSamplerBindings[prior].Argument == binding.Argument)
					return NativeSamplerFault{
						Status::DuplicateId, "native sampler argument binding repeats", binding.Argument
					};
		}
		return {};
	}
	inline std::optional<uint64_t> NativeSamplerBindingsPayloadBytes(const Node &node, bool retained) {
		if (ValidateNativeSamplerBindings(node)) return std::nullopt;
		const size_t count =
			retained ? node.NativeSamplerBindings.capacity() : node.NativeSamplerBindings.size();
		if (count > UINT64_MAX / sizeof(NativeSamplerBinding)) return std::nullopt;
		uint64_t bytes = count * sizeof(NativeSamplerBinding);
		for (const auto &binding : node.NativeSamplerBindings) {
			for (const auto *text : {&binding.Argument, &binding.Texture}) {
				const uint64_t amount =
					retained ? text->capacity() : std::max(text->size(), std::string{}.capacity());
				if (amount > UINT64_MAX - bytes) return std::nullopt;
				bytes += amount;
			}
		}
		return bytes;
	}

}

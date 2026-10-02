#pragma once
#include <engine/assets/Shader.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
namespace engine::render {
	// A borrowed view is valid only until its cooked owner mutates or retires.
	struct CookedShaderSelection {
		const assets::ShaderVariant *Variant = nullptr;
		const assets::ShaderPayload *Payload = nullptr;
	};
	inline std::optional<std::string> SelectCookedPayload(
		const assets::ShaderData &data,
		std::string_view abi,
		std::string_view name,
		std::string_view stage,
		std::string_view backend,
		CookedShaderSelection &output
	) {
		if (abi.empty() || data.ShaderAbi != abi) return "cooked shader ABI mismatch";
		if (!data.IsValid()) return "invalid cooked shader container";
		const auto variant = std::find_if(data.Variants.begin(), data.Variants.end(), [&](const auto &value) {
			return value.Name == name;
		});
		if (variant == data.Variants.end()) return "explicit cooked shader variant missing";
		if (variant->Stage != stage || !variant->Features.empty() || !variant->Specializations.empty())
			return "cooked shader selection requires exact unspecialized stage";
		const auto payload =
			std::find_if(variant->Payloads.begin(), variant->Payloads.end(), [&](const auto &value) {
				return value.Backend == backend;
			});
		if (payload == variant->Payloads.end()) return "requested cooked shader backend missing";
		output = {&*variant, &*payload};
		return {};
	}
	// Full value equality includes backend, entry point, target and bytes. Hashes
	// are optional accelerators.
	inline bool SameCookedPayload(const assets::ShaderPayload &first, const assets::ShaderPayload &second) {
		return first == second;
	}
} // namespace engine::render

namespace engine::render {
	struct OwnedCookedStage {
		assets::ShaderPayload SpirV;
		std::optional<assets::ShaderPayload> Msl;
		bool operator==(const OwnedCookedStage &) const = default;
	};
	// remainingBackingBytes is owner-local admission state, never a metrics
	// reading. Preflight both payload forms before any owned payload copy.
	inline std::optional<std::string> RetainCookedStage(
		const assets::ShaderData &data,
		std::string_view abi,
		std::string_view variantName,
		std::string_view stage,
		uint64_t remainingBackingBytes,
		OwnedCookedStage &output
	) {
		ENGINE_PROFILE_CAT("cooked shader payload retention", core::ProfileCategory::Assets);
		CookedShaderSelection selected;
		if (const auto failure = SelectCookedPayload(data, abi, variantName, stage, "spirv", selected))
			return failure;
		const auto &payloads = selected.Variant->Payloads;
		const auto msl = std::find_if(payloads.begin(), payloads.end(), [](const auto &payload) {
			return payload.Backend == "msl";
		});
		uint64_t bytes = selected.Payload->Bytes.size();
		if (bytes > remainingBackingBytes) return "cooked owner backing byte budget exceeded";
		if (msl != payloads.end() && msl->Bytes.size() > remainingBackingBytes - bytes)
			return "cooked owner backing byte budget exceeded";
		OwnedCookedStage candidate;
		candidate.SpirV = *selected.Payload;
		if (msl != payloads.end()) candidate.Msl = *msl;
		core::Metrics::Count(
			"shader.cooked.retained_payload_bytes", bytes + (msl == payloads.end() ? 0 : msl->Bytes.size())
		);
		core::Metrics::Count("shader.cooked.payload_retentions", 1);
		output = std::move(candidate);
		return {};
	}
} // namespace engine::render

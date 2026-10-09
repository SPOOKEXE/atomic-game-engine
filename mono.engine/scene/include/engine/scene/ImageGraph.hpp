#pragma once

// Authored live image graph references and primitive input overrides. Evaluation
// belongs above scene; this component never holds graph programs or device data.
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	inline constexpr size_t MAXIMUM_IMAGE_GRAPH_INPUTS = 64;
	inline constexpr size_t MAXIMUM_IMAGE_GRAPH_INPUT_BYTES = 64 * 1024;
	inline constexpr size_t MAXIMUM_IMAGE_GRAPH_STRING_BYTES = 4096;

	enum class ImageGraphInputKind : uint8_t { Number, Boolean, Colour, String };
	struct ImageGraphInput {
		core::Name Name;
		ImageGraphInputKind Kind = ImageGraphInputKind::Number;
		double Number = 0;
		bool Boolean = false;
		std::array<uint8_t, 4> Colour{0, 0, 0, 255};
		std::string String;
		bool operator==(const ImageGraphInput &) const = default;
	};
	struct ImageGraph {
		// Explicit durable identity, unique within a world. Clones must be rekeyed.
		core::Name InstanceKey;
		core::Name Graph;
		core::Name Output = core::Name("image");
		std::vector<ImageGraphInput> Inputs;
		// Local write counter; snapshots and replication start it at zero.
		uint32_t Revision = 0;
	};

	ecs::ClassId ImageGraphClass();
	bool ImageGraphTokenValid(std::string_view text);
	bool SetImageGraphInstanceKey(ecs::Store &store, ecs::Entity instance, core::Name key);
	bool SetImageGraphAsset(ecs::Store &store, ecs::Entity instance, core::Name graph);
	bool SetImageGraphOutput(ecs::Store &store, ecs::Entity instance, core::Name output);
	// Finite typed overrides, sorted by their durable names. Refusal preserves the row.
	// Repeating a value does not change Revision; reset restores the graph's default.
	bool SetImageGraphInput(ecs::Store &store, ecs::Entity instance, const ImageGraphInput &input);
	bool
	SetImageGraphInputs(ecs::Store &store, ecs::Entity instance, std::span<const ImageGraphInput> inputs);
	bool ResetImageGraphInput(ecs::Store &store, ecs::Entity instance, core::Name name);
	// Returns the explicit override, or false when the graph's default is in use.
	bool
	GetImageGraphInput(const ecs::Store &store, ecs::Entity instance, core::Name name, ImageGraphInput &out);
	// Stable imagegraph-instance://key#output reference for any ordinary image slot.
	// Missing assets, invalid tokens or duplicate instance keys return an invalid name.
	core::Name ImageGraphContentName(const ecs::Store &store, ecs::Entity instance, core::Name output = {});
}

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
	// Per-world limits keep live graph overrides bounded on save and replication.
	// Maximum number of named input overrides on one image graph.
	inline constexpr size_t MAXIMUM_IMAGE_GRAPH_INPUTS = 64;
	// Maximum encoded size of all input overrides on one image graph.
	inline constexpr size_t MAXIMUM_IMAGE_GRAPH_INPUT_BYTES = 64 * 1024;
	// Maximum UTF-8 byte length of one string override.
	inline constexpr size_t MAXIMUM_IMAGE_GRAPH_STRING_BYTES = 4096;

	// Value types supported by live image graph input overrides.
	enum class ImageGraphInputKind : uint8_t { Number, Boolean, Colour, String };
	// A typed value override identified by its durable input name.
	struct ImageGraphInput {
		// Name of the graph input this value overrides.
		core::Name Name;
		// Selects which value member is active.
		ImageGraphInputKind Kind = ImageGraphInputKind::Number;
		// Numeric value when Kind is Number.
		double Number = 0;
		// Boolean value when Kind is Boolean.
		bool Boolean = false;
		// RGBA8 value when Kind is Colour.
		std::array<uint8_t, 4> Colour{0, 0, 0, 255};
		// Text value when Kind is String.
		std::string String;
		// Compares the input name, kind and stored values.
		bool operator==(const ImageGraphInput &) const = default;
	};
	// Durable graph reference and user overrides attached to a scene instance.
	// Replica hosts may edit only predicted viewer-local instances.
	struct ImageGraph {
		// Explicit durable identity, unique within a world. Clones must be rekeyed.
		core::Name InstanceKey;
		// Asset name of the image graph document to evaluate.
		core::Name Graph;
		// Output name selected from the referenced graph.
		core::Name Output = core::Name("image");
		// Sorted named values that override graph defaults.
		std::vector<ImageGraphInput> Inputs;
		// Local write counter; snapshots and replication start it at zero.
		uint32_t Revision = 0;
	};

	// Registers the image graph component with the ECS class table.
	ecs::ClassId ImageGraphClass();
	// Checks that text is a valid token for an image graph reference.
	bool ImageGraphTokenValid(std::string_view text);
	// Assigns a world-unique durable key to an instance.
	bool SetImageGraphInstanceKey(ecs::Store &store, ecs::Entity instance, core::Name key);
	// Selects the graph asset referenced by an instance.
	bool SetImageGraphAsset(ecs::Store &store, ecs::Entity instance, core::Name graph);
	// Selects an output from the graph referenced by an instance.
	bool SetImageGraphOutput(ecs::Store &store, ecs::Entity instance, core::Name output);
	// Finite typed overrides, sorted by their durable names. Refusal preserves the row.
	// Repeating a value does not change Revision; reset restores the graph's default.
	bool SetImageGraphInput(ecs::Store &store, ecs::Entity instance, const ImageGraphInput &input);
	// Replaces all input overrides if the complete bounded set is valid.
	bool
	SetImageGraphInputs(ecs::Store &store, ecs::Entity instance, std::span<const ImageGraphInput> inputs);
	// Removes one named override so the graph's default becomes active again.
	bool ResetImageGraphInput(ecs::Store &store, ecs::Entity instance, core::Name name);
	// Returns the explicit override, or false when the graph's default is in use.
	bool
	GetImageGraphInput(const ecs::Store &store, ecs::Entity instance, core::Name name, ImageGraphInput &out);
	// Stable `imagegraph-instance://key#output` reference for any ordinary image slot.
	// Missing assets, invalid tokens or duplicate instance keys return an invalid name.
	core::Name ImageGraphContentName(const ecs::Store &store, ecs::Entity instance, core::Name output = {});
}

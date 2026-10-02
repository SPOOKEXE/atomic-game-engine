#pragma once

#include <engine/imagegraph/SourceRigid.hpp>

namespace engine::imagegraph {
	// The journal owns source wrapper properties. RigidValue stores only the
	// durable alias so a later Override is visible through every previously linked
	// object list.
	struct RigidVisualState {
		std::string BodyId;
		std::optional<SurfaceValue> Texture;
		double XOffset = 0, YOffset = 0, XScale = 1, YScale = 1;
		Colour BlendColour{255, 255, 255, 255};
		double Alpha = 1;
		bool operator==(const RigidVisualState &) const = default;
	};
	struct RigidVisualMutation {
		SourceRigidEventPosition Position;
		RigidVisualState Data;
		bool operator==(const RigidVisualMutation &) const = default;
	};
	struct RigidVisualFrame {
		std::vector<RigidVisualMutation> Mutations;
		bool operator==(const RigidVisualFrame &) const = default;
	};
	struct RigidCollisionPairState {
		std::string A, B;
		uint64_t LastObservedTick = 0;
		bool operator==(const RigidCollisionPairState &) const = default;
	};
	struct RigidNodeReplayState {
		std::string NodeId;
		uint32_t ProcessorRow = 0;
		uint64_t SpawnIndex = 0;
		std::vector<std::string> OutputBodyIds;
		std::vector<RigidCollisionPairState> CollisionPairs;
		bool operator==(const RigidNodeReplayState &) const = default;
	};
	struct RigidSpawnRecipe {
		std::string NodeId;
		uint32_t ProcessorRow = 0;
		uint64_t Tick = 0;
		std::vector<SourceRigidBody> Prototypes;
		std::vector<RigidVisualState> Visuals;
		bool operator==(const RigidSpawnRecipe &) const = default;
	};
	struct RigidNodeFrame {
		std::vector<RigidNodeReplayState> Nodes;
		std::vector<RigidSpawnRecipe> Recipes;
		bool operator==(const RigidNodeFrame &) const = default;
	};
	struct RigidOwnerReplayState {
		uint64_t AuthoringRevision = 0;
		SourceRigidHistory History;
		std::vector<RigidVisualFrame> VisualFrames;
		std::vector<RigidNodeFrame> NodeFrames;
		bool operator==(const RigidOwnerReplayState &) const = default;
	};
	struct RigidReplayState {
		std::vector<RigidOwnerReplayState> Owners;
		bool operator==(const RigidReplayState &) const = default;
	};
	uint64_t RetainedRigidReplayBytes(const RigidReplayState &state);
	[[nodiscard]] Status
	ValidateRigidReplay(const RigidReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic);
} // namespace engine::imagegraph

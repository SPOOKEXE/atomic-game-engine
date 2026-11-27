#pragma once

// A NodeCanvas seen as an ang graph, for link checks and for pointer gestures.
//
// The ECS entities stay the saved and replicated form. Everything here is a
// transient copy built when it is needed and dropped straight after: a link
// check builds one and throws it away, and a gesture keeps one only from press
// to release. Edits the angview interaction makes are written back to the
// entities, so the store is still the one place the graph lives.

#include <engine/ang/Compile.hpp>
#include <engine/angview/View.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>

#include <memory>
#include <unordered_map>

namespace engine::gui::detail {

	// A canvas mirrored into ang: one node definition per canvas node, built
	// from its port children, and one data type per distinct port value type.
	struct CanvasGraph {
		ang::Registry Types;
		ang::Graph Document;
		// Node id text to its entity.
		std::unordered_map<std::string, ecs::Entity> Nodes;
	};

	// Mirrors the canvas's nodes (at any depth) and links. Ports with an
	// unusable connection limit are mirrored as single-link inputs.
	CanvasGraph MirrorCanvas(const ecs::Store &store, ecs::Entity canvas);

	// The link a pair of port entities describes, in graph terms.
	//
	// Kept with no caller: wire pulling is broken and will be rebuilt on top
	// of this. `just dead-code` lists it until then.
	std::optional<ang::Link> LinkBetween(const ecs::Store &store, ecs::Entity output, ecs::Entity input);

	// Whether two port value types may be wired, under ang's rule: exact, or `any` at either end.
	bool PortTypesConnect(std::string_view output, std::string_view input);

	// One press-to-release pointer gesture on a NodeCanvas.
	//
	// A press only records a candidate. Past a few pixels it becomes a node
	// move, a pan or a wire pull and claims the pointer; a press released in
	// place stays an ordinary click, so a script's click handlers keep working.
	class NodeCanvasGesture {
	  public:
		// `point` is the logical pointer where the press landed on `canvas`.
		NodeCanvasGesture(ecs::Store &store, ecs::Entity canvas, core::Vector2 point);

		ecs::Entity Canvas() const {
			return Target;
		}
		bool Active() const {
			return Claimed;
		}

		// Follows a held pointer. Returns true once the gesture owns it.
		bool Move(ecs::Store &store, core::Vector2 point);

		// Finishes: a pulled wire dropped on a compatible port becomes a link.
		void Release(ecs::Store &store, core::Vector2 point);

		// The wire being pulled, for the compile, in canvas layout space.
		NodeCanvasWire Wire() const;

		// Stable summary of the gesture for router continuity checks.
		uint64_t Digest() const;

	  private:
		void WriteBack(ecs::Store &store);

		ecs::Entity Target;
		CanvasGraph Mirror;
		std::unique_ptr<angview::View> Interaction;
		core::Vector2 Origin{};
		core::Vector2 Start{};
		core::Vector2 Pointer{};
		bool Claimed = false;
		// Node positions at the press, written back with the drag offset.
		std::unordered_map<std::string, core::UDim2> StartPositions;
	};
}

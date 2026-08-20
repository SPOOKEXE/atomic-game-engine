#pragma once

// What sorts of node a render pipeline can hold, and which of their slots may
// be joined to which.
//
// **Two things a diagram does not need and an editor cannot work without.** The
// Render Pipeline widget could draw the standard frame and nothing else,
// because a panel that offers "add a node" has to have a list to offer and a
// panel that lets somebody drag a wire has to be able to refuse one. Neither
// question has an answer in `RenderGraph`: a node there carries a `Kind` that
// an executor switches on, and nothing says what kinds exist or what they take.
//
// ## Ports are resource slots, not a second graph
//
// A `RenderGraph` node names the resources it reads and writes; it has no
// wires. That is how a frame actually works - a shadow map is written once and
// sampled by everything - and it is deliberately kept.
//
// So a **port is a named slot that a resource is bound into**, and a **link is
// two nodes agreeing about one resource**: the writer's output slot and the
// reader's input slot both name it. Dragging a wire in the editor records
// `EditKind::Reads`; there is no new runtime concept and no second description
// of the frame to keep in step. `docs/DEFERRED.md` D00016 is the entry about
// having had three of those at once, and this is written not to add a fourth.
//
// ## What "typed" refuses
//
// A slot declares the `ResourceKind` it accepts. A depth buffer cannot be
// dropped into a slot that wants a colour attachment, and the editor can say so
// while the wire is still being dragged rather than after a compile.
//
// **One narrowing rule, and it is the one the hardware has.** A `Texture` input
// accepts a `Colour` or a `Depth` output, because sampling something that was
// rendered into is exactly what a shadow map and a mirror are. The reverse is
// refused: a pass cannot render *into* something declared as a sampled texture.
//
// ## Registered by hand
//
// `RegisterRenderNodeKinds` lists them, the way `scene::RegisterSceneClasses`
// lists classes. There is no scan and no discovery, for that function's reason:
// what a pipeline can contain must not depend on which translation units
// happened to be linked.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::graph {

	// Whether a slot is fed or feeds.
	//
	// @since v0.11
	enum class PortDirection : uint8_t {
		// A resource the node reads. Drawn on the left, and the end a wire
		// lands on.
		Input,

		// A resource the node writes. Drawn on the right, and the end a wire
		// leaves from.
		Output,
	};

	// A stable, human-readable name for a direction.
	//
	// @param direction The direction to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(PortDirection direction);

	// One named, typed slot on a node kind.
	//
	// @since v0.11
	struct PortSpec {
		// What the slot is called - `colour`, `depth`, `shadow`.
		//
		// **The slot's name and not the resource's.** Two frames may bind
		// different resources into the same slot, and an editor labels the slot
		// so that an unbound one still reads as something rather than as a gap.
		core::Name Name{};

		// What may be bound into it.
		ResourceKind Kind = ResourceKind::Colour;

		// What this slot produces, or expects to be handed.
		//
		// **A preference and not a requirement.** A pass that samples an
		// `RGBA16F` where it declared `RGBA8` works perfectly well; a pass that
		// *writes* fewer bits than the reader wanted loses them. So a format
		// mismatch is a mark on the wire rather than a refusal - see
		// `IsLossy`, and `docs/PIPELINE_NODES.md` §1.5 fault 6 for the frame
		// that spent four times the bits it needed on its normals.
		ResourceFormat Format = ResourceFormat::RGBA8;

		// Whether the node is incomplete without it.
		//
		// **A warning and not a refusal.** Somebody building a pipeline works in
		// whatever order suits them, and a panel that refused to hold a
		// half-wired node would be one nobody could use - `PipelineDocument`'s
		// `Record` makes the same argument about validation.
		bool Required = true;

		// One line for a tooltip. Empty is allowed and reads as no tooltip.
		std::string Summary{};
	};

	// Where a kind belongs in the add menu.
	//
	// **Presentational, and the executor never branches on it.** The split is
	// the vocabulary a frame is described in: something produces geometry,
	// something composites what was produced, something puts the result on a
	// screen. A menu that listed thirty kinds in one flat list would be one
	// nobody could find anything in.
	//
	// @since v0.11
	enum class NodeCategory : uint8_t {
		// Draws world geometry - shadow, surface, opaque, transparent.
		Draw,

		// Reads one image and writes another - tone mapping, bloom, a blur.
		Composite,

		// Draws over the frame rather than into the world - overlay, interface.
		Interface,

		// Puts a frame somewhere - the swapchain, a capture.
		Output,
	};

	// A stable, human-readable name for a category.
	//
	// @param category The category to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(NodeCategory category);

	// One sort of node, with its slots.
	//
	// @since v0.11
	struct NodeKindSpec {
		// What an executor switches on, and what an `AddNode` edit records.
		core::Name Kind;

		// What to call it in a menu. Falls back to `Kind` when empty.
		std::string Label;

		// One line under the label in the add menu.
		std::string Summary;

		// Where it sits in the menu.
		NodeCategory Category = NodeCategory::Draw;

		// Its slots, in the order they are drawn down each side.
		//@{
		std::vector<PortSpec> Inputs;
		std::vector<PortSpec> Outputs;
		//@}

		// Whether this kind produces something from nothing.
		//
		// **A fact about the pass, and the term the "must have an input" rule
		// turns on.** A shadow pass renders the world from a light and reads
		// nothing from the frame; a clear writes a constant; a velocity pass
		// reads the scene rather than a target. Everything else that declares no
		// inputs is a box no wire can reach - which is what `overlay` and
		// `interface` were before v0.11, and the reason the rule exists.
		//
		// **Not derived from `Category`.** It was, and `clear` broke it: a pass
		// can be a compositing kind and still be a source, and the category is
		// about where a menu puts it rather than about what it reads.
		bool Source = false;

		// What scope a node of this kind gets when one is added.
		//
		// **A default and not a rule.** How often a pass runs is a property of
		// what the pass does, so the catalogue is where the right answer lives -
		// but `RenderGraph` lets a node say otherwise and an author may have a
		// reason.
		NodeScope Scope = NodeScope::View;

		// The shader this kind runs when a node does not name one.
		//
		// **What makes a catalogue entry a working pass rather than a word.**
		// `raster` runs whatever a node's `shader` parameter names and does
		// nothing without one - which is right for a kind that *is* "somebody's
		// shader". An effect like `ssao` is not that: it has one correct
		// implementation, the engine ships it, and asking every author to type
		// its filename would be a setting with one right answer.
		//
		// **Still overridable.** A node naming its own shader beats this, so a
		// kind with a default is a starting point rather than a fixed pass -
		// which is the difference between shipping an ambient-occlusion pass and
		// shipping *the* ambient-occlusion pass.
		//
		// Empty for kinds whose behaviour is not a fullscreen shader at all -
		// `opaque` rasterises geometry and `entities` produces a list.
		//
		// @since v0.11
		std::string DefaultShader;
	};

	// Whether a wire from an output of kind `from` may land in an input of kind
	// `to`.
	//
	// **Not symmetric, deliberately.** See the header: a rendered target may be
	// sampled, and a sampled texture may not be rendered into.
	//
	// @param from What the writing slot produces.
	// @param to   What the reading slot accepts.
	// @return Whether the editor should allow the connection.
	bool PortsCompatible(ResourceKind from, ResourceKind to);

	// One line saying why a connection was refused, for a panel to show.
	//
	// **Written here rather than in the widget**, because the rule is here: a
	// panel that phrased its own explanation would drift from what the check
	// actually does the first time the rule gained a case.
	//
	// @param from What the writing slot produces.
	// @param to   What the reading slot accepts.
	// @return The reason, or an empty view when the two are compatible.
	std::string_view WhyIncompatible(ResourceKind from, ResourceKind to);

	// Whether feeding `from` into `to` throws information away.
	//
	// **Separate from `PortsCompatible` because it is a different answer.** That
	// one says whether the connection is *legal*; this says whether it is
	// *lossy*. An editor refuses the first and marks the second, which is the
	// only way somebody notices they are writing an HDR frame into eight bits
	// per channel before it looks wrong on screen.
	//
	// Two ways to lose: fewer bits per pixel, or fewer channels. Block
	// compressed formats are always lossy as a destination and never as a
	// source, because nothing in a frame renders into one.
	//
	// @param from What the writing slot produces.
	// @param to   What the reading slot expects.
	// @return Whether information is lost.
	bool IsLossy(ResourceFormat from, ResourceFormat to);

	// One line saying what a lossy wire costs, for a panel to show.
	//
	// @param from What the writing slot produces.
	// @param to   What the reading slot expects.
	// @return The description, or an empty view when nothing is lost.
	std::string_view WhatIsLost(ResourceFormat from, ResourceFormat to);

	// Every node kind this process knows.
	//
	// **Process-wide and registered once**, which is `ecs::Classes`' shape and
	// for its reason: a kind is named in save files, in scripts and in a menu,
	// and a per-world table would let two worlds disagree about what `opaque`
	// means.
	//
	// @since v0.11
	class NodeCatalogue {
	  public:
		// Adds a kind, or replaces the one of that name.
		//
		// **Replaces rather than refuses**, so a game may correct a built-in
		// rather than having to work around it. `RegisterRenderNodeKinds` is
		// idempotent for the same reason.
		//
		// @param spec The kind. Copied.
		// @return `false` for a spec with no `Kind`.
		static bool Register(NodeKindSpec spec);

		// One kind.
		//
		// @param kind Which.
		// @return The spec, or null when nothing registered that name.
		static const NodeKindSpec *Find(core::Name kind);

		// Every kind, sorted by `Kind`'s text.
		//
		// **Sorted rather than in registration order**, so a menu reads the same
		// way whatever order the registrations happened to run in - the same
		// argument `PipelineSet::Names` makes about a save file.
		//
		// @return The specs. Valid until the next `Register`.
		static std::span<const NodeKindSpec> All();

		// Forgets everything. For tests, which need a table they control.
		static void Reset();
	};

	// One slot of one node, resolved.
	//
	// **What a hit-test hands back and what a link is made of**, so an editor
	// never has to carry a node and an index in two variables that can disagree.
	//
	// @since v0.11
	struct PortRef {
		// Which node.
		core::Name Node;

		// Which side.
		PortDirection Direction = PortDirection::Input;

		// Which slot, by position in the kind's list.
		//
		// **An index and not a name, and this is the one place that is right.**
		// A `PortRef` lives for the length of a drag; it is never saved and
		// never crosses a boundary, which is exactly the condition rule 4
		// names. The document records resource *names*.
		uint32_t Slot = 0;

		// Whether it refers to anything.
		bool IsValid() const {
			return Node.IsValid();
		}

		// Whether two references name the same slot.
		bool operator==(const PortRef &other) const = default;
	};

	// The built-in node kinds available to authored render graphs.
	//
	// Idempotent.
	void RegisterRenderNodeKinds();
}

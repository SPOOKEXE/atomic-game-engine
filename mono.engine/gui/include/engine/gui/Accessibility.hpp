#pragma once

// A headless semantic snapshot of the compiled interface. Platform adapters
// consume these values and send actions back through the ordinary input route.
//
// @tier L7 · shared

#include <engine/core/types/Rect.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {
	struct DrawList;

	// The supported semantic behavior of a visible GUI instance.
	enum class SemanticRole : uint8_t {
		// Static text.
		Text,
		// An activatable control.
		Button,
		// An editable text control.
		TextField,
		// An image with a readable name.
		Image,
		// An interactive container.
		Group,
	};

	// Actions the existing GUI input route can perform for a semantic node.
	// Platform adapters use this to avoid offering controls the route cannot
	// honor.
	//
	// @since v0.23
	enum class SemanticActionSet : uint8_t {
		None = 0,
		Activate = 1 << 0,
		Focus = 1 << 1,
		Edit = 1 << 2,
	};

	// The text direction observed in the compiled shaped text.
	//
	// @since v0.23
	enum class SemanticTextDirection : uint8_t {
		Unknown,
		LeftToRight,
		RightToLeft,
	};

	// One node in the headless accessibility snapshot.
	struct SemanticNode {
		// The concrete source and its canvas owner.
		//@{
		ecs::Entity Instance;
		ecs::Entity Collector;
		//@}

		// Nearest semantic ancestor, or null at the collector boundary.
		ecs::Entity Parent;

		// The behavior a platform adapter exposes.
		SemanticRole Role = SemanticRole::Group;

		// The name read by an assistive client.
		std::string Name;

		// Current editable content for a text field.
		std::string Value;

		// Supplemental text such as a text field's placeholder.
		std::string Description;

		// The viewer locale that resolved this node's text, when supplied by the
		// caller. It stays viewer-local with the catalogue.
		std::string Language;

		// Visible bounds in collector canvas pixels.
		core::Rect Bounds;

		// Position among this snapshot's nodes in compiled reading order.
		uint32_t ReadingOrder = 0;

		// The actions which the ordinary GUI route can honor for this node.
		SemanticActionSet Actions = SemanticActionSet::None;

		// The direction observed in its first shaped text run.
		SemanticTextDirection Direction = SemanticTextDirection::Unknown;

		// Local interaction, selection, and text focus state.
		//@{
		bool Enabled = true;
		bool Selected = false;
		bool Focused = false;
		bool Editable = false;
		bool Password = false;
		//@}
	};

	// Bounded output and an explicit report when a hostile tree exceeds it.
	struct SemanticSnapshot {
		// Nodes in compiled reading order.
		std::vector<SemanticNode> Nodes;

		// True when the source ceiling stopped enumeration.
		bool Truncated = false;
	};

	// Evidence for one accessibility problem in the compiled, viewer-local tree.
	enum class SemanticIssueKind : uint8_t {
		MissingName,
		SmallTarget,
		DuplicateControlName,
	};

	// One accessibility problem detected in a compiled snapshot.
	struct SemanticIssue {
		// Category of accessibility problem.
		SemanticIssueKind Kind = SemanticIssueKind::MissingName;
		// Instance with the problem.
		ecs::Entity Instance;
		// Collector that owns the instance canvas.
		ecs::Entity Collector;
	};

	// Bounded accessibility audit output.
	struct SemanticAudit {
		// Detected problems in snapshot order.
		std::vector<SemanticIssue> Issues;
		// Whether the audit stopped at its output limit.
		bool Truncated = false;
	};

	// Audits compiled controls without changing authored properties. The target
	// threshold is expressed in canvas pixels after the viewer's UI scale.
	SemanticAudit AuditSemantics(const SemanticSnapshot &snapshot, float minimumTargetPixels = 44.0f);

	// Emits each visible meaningful instance once, in compiled reading order.
	// Bounds are clipped canvas pixels and retain their collector identity.
	// `language` is the viewer locale from the same compile request that made
	// `list`. It is bounded before copying so an adapter snapshot cannot become
	// an unbounded side channel.
	SemanticSnapshot
	CompileSemantics(const ecs::Store &store, const DrawList &list, std::string_view language = {});
}

#pragma once

// Headless node graph view: pan, zoom, selection, hover, drags, wires, the
// add-node palette and the draw list, driven by synthetic input.
//
// The interaction state machine lives here, not in a painter, so Studio's
// ImGui canvas, the in-game gui canvas and tests all drag the same way. A
// painter forwards input in view-space pixels and draws Paint(). Every graph
// change goes through Graph::Apply; the records queue for the host's undo.
//
// Default bindings: left-drag a node moves it (selection moves together),
// left-drag a port pulls a wire (from a linked input it picks the wire up),
// dropping on a compatible port links, dropping on empty space opens the
// palette filtered by type, left-drag on empty space box-selects, middle-drag
// or Alt+left or Space+left pans, the wheel zooms about the cursor, F frames
// the selection, Delete removes it, Ctrl+D duplicates it, Ctrl+A selects
// all, Escape cancels.
// Touch: one finger acts as left, two fingers pan and pinch-zoom.
// @tier L4 · shared

#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>
#include <engine/angview/DrawList.hpp>
#include <engine/angview/Layout.hpp>

#include <functional>
#include <optional>
#include <unordered_map>

namespace engine::angview {

	enum class PointerButton : uint8_t { Left, Middle, Right };

	enum class Key : uint8_t {
		Delete,
		Backspace,
		Escape,
		Enter,
		Up,
		Down,
		F,
		A,
		Space,
		Tab,
		D,
	};

	struct Modifiers {
		bool Shift = false;
		bool Control = false;
		bool Alt = false;
	};

	enum class HitKind : uint8_t { None, Node, Port, Link, Group };

	struct Hit {
		HitKind Kind = HitKind::None;
		std::string Node{};
		std::string Port{};
		ang::Direction Dir = ang::Direction::Input;
		// Index into Graph::Links() for a Link hit.
		size_t Link = 0;
		std::string Group{};
	};

	enum class DragKind : uint8_t { None, Nodes, Wire, BoxSelect, Pan, Group, Pinch };

	// The wire being pulled, for painters and tests.
	struct PendingWire {
		std::string Node{};
		std::string Port{};
		// Direction of the port the wire starts from.
		ang::Direction Dir = ang::Direction::Output;
		ang::TypeId Type{};
		core::Vector2 Pointer{};
		// Port under the pointer, and whether it accepts.
		std::optional<Hit> Target{};
		bool TargetAccepts = false;
		std::string Reason{};
	};

	// The add-node palette opened by dropping a wire on empty space or Tab.
	struct Palette {
		bool Open = false;
		// Where the new node goes, graph space.
		core::Vector2 Position{};
		std::string Filter{};
		// Definitions matching Filter and, when a wire is pending, its type.
		std::vector<std::string> Matches{};
		size_t Highlighted = 0;
		// Wire to complete once a node is chosen.
		std::optional<PendingWire> Wire{};
	};

	// Per-node run results shown on the canvas.
	struct NodeStatusView {
		ang::NodeStatus Status = ang::NodeStatus::Idle;
		double Milliseconds = 0.0;
		size_t Bytes = 0;
		std::string Note{};
	};

	class View {
	  public:
		View(ang::Graph &graph, const ang::Registry &registry, Theme theme = Theme::Dark());

		// --- Setup ---------------------------------------------------------------------------
		void SetViewport(core::Vector2 size);
		core::Vector2 Viewport() const {
			return ViewportSize;
		}
		void SetTheme(Theme theme);
		const Theme &CurrentTheme() const {
			return Style;
		}
		// The graph was replaced or edited outside the view.
		void GraphChanged();
		// NodeTitle or NodeImages would now answer differently; lays out again.
		void VisualsChanged();

		// --- Input, view-space pixels ---------------------------------------------------------
		// Each returns true when the view consumed the event.
		bool PointerDown(PointerButton button, core::Vector2 position, Modifiers modifiers = {});
		bool PointerMove(core::Vector2 position, Modifiers modifiers = {});
		bool PointerUp(PointerButton button, core::Vector2 position, Modifiers modifiers = {});
		bool Wheel(float steps, core::Vector2 position);
		bool KeyDown(Key key, Modifiers modifiers = {});
		bool KeyUp(Key key);
		// Typed text, for the palette filter.
		bool TextInput(std::string_view text);
		bool TouchDown(uint64_t id, core::Vector2 position);
		bool TouchMove(uint64_t id, core::Vector2 position);
		bool TouchUp(uint64_t id, core::Vector2 position);

		// --- State ----------------------------------------------------------------------------
		core::Vector2 Pan() const {
			return PanOffset;
		}
		float Zoom() const {
			return ZoomScale;
		}
		void SetPanZoom(core::Vector2 pan, float zoom);
		core::Vector2 ToGraph(core::Vector2 view) const;
		core::Vector2 ToView(core::Vector2 graph) const;

		const std::vector<std::string> &Selection() const {
			return Selected;
		}
		bool IsSelected(std::string_view node) const;
		void Select(std::vector<std::string> nodes);
		const Hit &Hovered() const {
			return HoverHit;
		}
		DragKind Dragging() const {
			return Drag;
		}
		// Nodes a node drag is moving and its graph-space offset so far. The
		// offset becomes MoveNode edits on release; a host that draws nodes
		// itself can follow it live.
		const std::vector<std::string> &DraggedNodes() const {
			return DragNodes;
		}
		core::Vector2 DragDelta() const {
			return DragOffset;
		}
		const std::optional<PendingWire> &Wire() const {
			return WireState;
		}
		std::optional<Rect> BoxSelection() const;
		const Palette &AddPalette() const {
			return PaletteState;
		}
		const Layout &Geometry();

		// Hit test in view space.
		Hit HitTest(core::Vector2 view);

		// Fits the selection, or every node, in the viewport.
		void Frame();

		// Puts the selected nodes in a new group, as one edit. The group has no
		// size, so it frames its members wherever they move. Returns its id, or
		// empty when nothing is selected.
		std::string GroupSelection(std::string label);
		// Graph-space frame of a group: its own Position and Size, or, when Size
		// is zero, its members' bounds with room for the label above them.
		Rect GroupFrame(const ang::Group &group);

		// Opens the palette at a view position, unfiltered by type.
		void OpenPalette(core::Vector2 view);
		void ClosePalette();
		// Adds the highlighted or named palette entry and completes a pending wire.
		bool AcceptPalette(std::string_view def = {});

		// --- Edits ----------------------------------------------------------------------------
		// Applies an edit through the graph and queues its record. False on refusal.
		bool Apply(const ang::Edit &change);
		bool Apply(std::span<const ang::Edit> changes);
		// Records of applied edits since the last call, oldest first.
		std::vector<ang::EditRecord> TakeEdits();
		// Why the last refused edit or link was refused.
		const std::string &LastError() const {
			return Error;
		}

		// --- Run feedback ---------------------------------------------------------------------
		// Status, time and bytes per node from the last run of `plan`.
		void SetRunReport(const ang::Plan &plan, const ang::RunReport &report);
		// Draws the latest run as a strip along the bottom of the canvas.
		void SetTimeline(const ang::RunTimeline &timeline);
		void ClearRunFeedback();
		const NodeStatusView *StatusOf(std::string_view node) const;

		// --- Output ---------------------------------------------------------------------------
		// True when something visible changed since the last Paint().
		bool NeedsRedraw() const;
		// Rebuilds the draw list only when dirty.
		const DrawList &Paint();

		// Replaces painting: the function receives the default list and may edit or
		// replace it. Scripts use this to restyle or draw their own.
		std::function<void(DrawList &, const View &)> CustomPaint{};

		// Host title per node; empty keeps the definition label.
		std::function<std::string(const ang::NodeInstance &)> NodeTitle{};
		// Host picture per node, drawn below its ports at its aspect. The painter
		// resolves the handle. Call VisualsChanged() when the answer changes.
		std::function<NodeImage(const ang::NodeInstance &)> NodeImages{};

		// Host geometry. When set, the view never lays nodes out from the theme:
		// the source fills the layout, in graph space, every time the view needs
		// it, and the view only offsets the nodes it is dragging.
		std::function<void(Layout &)> GeometrySource{};

		// A left drag on empty space pans instead of box-selecting. Touch-first
		// game canvases want this; editors keep box select.
		bool EmptyDragPans = false;

	  private:
		void Dirty() {
			Changed = true;
		}
		void EnsureLayout();
		void BeginDrag(core::Vector2 position, Modifiers modifiers);
		void UpdateWireTarget();
		void FinishWire();
		void RefreshPalette();
		void ZoomAbout(float factor, core::Vector2 view);
		void DeleteSelection();
		void DuplicateSelection();
		void PaintInto(DrawList &list);
		void PaintTimeline(DrawList &list);
		Rgba WireColour(const ang::TypeId &type) const;

		ang::Graph &Document;
		const ang::Registry &Types;
		Theme Style;
		Layout Shapes{};
		uint64_t LayoutRevision = ~uint64_t{0};
		bool LayoutStale = true;

		core::Vector2 ViewportSize{800.0f, 600.0f};
		core::Vector2 PanOffset{};
		float ZoomScale = 1.0f;

		std::vector<std::string> Selected{};
		Hit HoverHit{};
		DragKind Drag = DragKind::None;
		PointerButton DragButton = PointerButton::Left;
		core::Vector2 DragStart{};
		core::Vector2 DragPointer{};
		core::Vector2 DragPanStart{};
		bool DragMoved = false;
		bool DragAdditive = false;
		// Node drag preview: nodes and the graph-space offset not yet applied.
		std::vector<std::string> DragNodes{};
		core::Vector2 DragOffset{};
		std::string DragGroup{};
		// Link removed when a wire was picked up off an input; restored on cancel.
		std::optional<ang::Link> PickedUp{};
		std::optional<PendingWire> WireState{};
		bool SpaceHeld = false;

		// Touches by id, and the pinch baseline.
		std::unordered_map<uint64_t, core::Vector2> Touches{};
		uint64_t PrimaryTouch = 0;
		bool PrimaryTouchActive = false;
		float PinchDistance = 0.0f;
		core::Vector2 PinchCentre{};

		Palette PaletteState{};

		std::vector<ang::EditRecord> Records{};
		std::string Error{};

		std::unordered_map<std::string, NodeStatusView> Statuses{};
		std::vector<ang::TimelineSample> TimelineSamples{};
		double TimelineMilliseconds = 0.0;

		DrawList Commands{};
		bool Changed = true;
	};
}

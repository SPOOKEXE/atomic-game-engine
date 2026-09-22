#pragma once

// Turning what an author wrote into where it actually is.
//
// One pass, parent before child, writing `Resolved` per node. Everything after
// this - the draw list, the hit test, a script asking `AbsoluteSize` - reads
// that component with a query and nothing walks the tree a second time.
//
// **Why the tree is walked here and nowhere else.** A `UDim2` means nothing
// without a parent rectangle, so resolving one is inherently top-down; that is
// the single place the shape of the tree is unavoidable. Every consumer
// downstream wants a flat list in paint order, which is what `Compile.hpp`
// produces once and keeps.
//
// ## Text measurement
//
// A viewer may supply a validated font package. Layout then shapes the visible
// text for automatic sizing, fitted text size, and `Resolved::TextBounds`.
// Without a package, headless callers retain the constant advance fallback.
// The chosen package is also a compile input, so a package change invalidates
// the draw list. Backends consume the compiled glyph positions when available.
//
// `AutomaticSize` measures children and the visible string of a labelled
// element. Markup and the visible grapheme limit are applied before measuring.
//
// @tier L7 · shared

#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/gui/TextResolution.hpp>

#include <cstdint>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// Physical insets a host reserves at the edge of a display.
	//
	// They are values rather than a platform handle, so the same profile can
	// describe a phone, a television overscan region, or Studio's preview pane.
	// @since v0.25
	struct DisplayInsets {
		// Left reserved display edge in logical pixels.
		float Left = 0.0f;
		// Top reserved display edge in logical pixels.
		float Top = 0.0f;
		// Right reserved display edge in logical pixels.
		float Right = 0.0f;
		// Bottom reserved display edge in logical pixels.
		float Bottom = 0.0f;
	};

	// How the display is oriented. The dimensions remain authoritative because a
	// resizable desktop window can be neither conventional orientation.
	// @since v0.25
	enum class DisplayOrientation : uint8_t {
		Landscape,
		Portrait,
	};

	// The display facts a host resolves before calling the shared layout pass.
	//
	// `Width` and `Height` are the host's logical display units. A screen
	// collector derives its own logical canvas by applying `InterfaceScale` once;
	// authored descendants never see or multiply that transform. `FramebufferScale`
	// and `DevicePixelRatio` preserve physical facts for painters and diagnostics.
	// A screen collector uses the merged safe and transient insets; a spatial
	// collector owns its canvas through `SpatialCanvas` and does not consume
	// display reservations.
	//
	// @since v0.25
	struct DisplayProfile {
		// The host logical display dimensions before the collector scale.
		float Width = 1600.0f;
		// Host logical display height before collector scale.
		float Height = 900.0f;

		// Physical pixels represented by one logical pixel, and the platform's
		// reported device ratio. A host may render to a framebuffer whose scale
		// differs from the display's ratio, so the two remain distinct.
		float FramebufferScale = 1.0f;
		// Platform physical-pixel ratio per logical pixel.
		float DevicePixelRatio = 1.0f;

		// Host-reported display orientation.
		DisplayOrientation Orientation = DisplayOrientation::Landscape;

		// Permanent display cutouts and transient keyboard or system overlays.
		DisplayInsets SafeArea;
		// Transient occluded display edges such as keyboards.
		DisplayInsets Occluded;

		// The host's accessibility choices. They are inputs to the canonical
		// layout call and cache key; authored components remain unchanged.
		float TextScale = 1.0f;
		// Viewer scale applied to the screen collector canvas.
		float InterfaceScale = 1.0f;

		// The historic top-bar reservation. It remains so existing callers keep
		// their source contract while hosts migrate to `SafeArea.Top`.
		float TopInset = 0.0f;
	};

	// The screen a `ScreenGui` collects onto.
	//
	// Passed in rather than read from a resource, because the two callers have
	// different answers and both are right: a game fills the window, and the
	// studio's viewport panel fills a rectangle inside it. A module that read
	// "the window size" from somewhere global could not serve the second.
	//
	// **`Screen` and not `Viewport`**, because `gui::Viewport` is already the
	// component behind `ViewportFrame` - a 3D view drawn *inside* the tree,
	// which is very nearly the opposite of this.
	//
	// @since v0.8
	using Screen = DisplayProfile;

	// The logical canvas owned by a screen collector. Insets in a display
	// profile are expressed in host logical units, so they cross the collector
	// transform with the dimensions. Spatial collectors do not use this helper.
	//
	// @since v0.25
	Screen ScreenCollectorProfile(const Screen &screen);

	// The extent a screen draw list uses. It is the same transform
	// `ScreenCollectorProfile` applies during layout, so a backend and a hit test
	// receive coordinates from one space.
	//
	// @since v0.25
	core::Vector2 ScreenCanvasSize(const Screen &screen);

	// The host presentation extent before collector transforms are applied.
	core::Vector2 ScreenPresentationSize(const Screen &screen);

	// Fallback glyph-width multiple without a validated font package.
	constexpr float AVERAGE_ADVANCE = 0.52f;
	// Fallback line-height multiple without shaped text metrics.
	constexpr float LINE_SPACING = 1.2f;

	// The containers a `LayerCollector` may draw from, by name.
	//
	// **Roblox's containment rule, and it is a rule rather than a style
	// choice.** A `ScreenGui` parented to a `Part` draws nothing - not because
	// it is invisible but because nothing is looking at that part of the tree -
	// and an engine that drew it anyway would let an author ship a game whose
	// interface appears in the studio and not in the client, which is the worst
	// direction for a difference like that to run.
	//
	//   - a `ScreenGui` draws from `STARTER_GUI` or from a player's
	//     `PLAYER_GUI`. The studio shows the first; a client shows the second.
	//   - a `SurfaceGui` or a `BillboardGui` draws from those *and* from
	//     `WORKSPACE`, because each is attached to something in the world and
	//     the world is where that something lives.
	//
	// **These are `scene`'s service names, spelled again here**, because
	// `gui/AGENTS.md` refuses an edge to `scene` - the same refusal that made
	// `SurfaceGui::Face` re-declare `NormalId`'s six members. They are exposed
	// rather than kept in the source file so a test can pin them against the
	// service table they are copied from, which is the arrangement that turns a
	// rename into a failing test instead of an interface that quietly stops
	// drawing.
	//
	// @since v0.8
	//@{
	inline constexpr std::string_view WORKSPACE = "Workspace";
	inline constexpr std::string_view STARTER_GUI = "StarterGui";
	inline constexpr std::string_view PLAYER_GUI = "PlayerGui";
	//@}

	// Resolves every `LayerCollector` in the store and everything under it.
	//
	// Writes `Resolved` on each node reached and clears `Resolved::Rendered` on
	// each node that is not - a disabled collector, an invisible ancestor, an
	// element parented outside any collector. Nothing is destroyed and nothing
	// is zeroed: an element scrolled out of view keeps the rectangle it had, so
	// the hit test does not have to tell "off screen" from "never laid out".
	//
	// @param store   The world.
	// @param screen  The screen a `ScreenGui` collects onto.
	// @param seconds The caller's monotonic clock. Only differences are read,
	//        so the origin does not matter; see `CompileRequest::Seconds` for
	//        why it is an argument rather than something this module reads.
	//        Defaulted so every caller that lays out a still interface - which
	//        is most tests - says nothing about time and gets a settled one.
	// @param text      Viewer-local inputs for resolving and scaling text.
	// @return How many nodes were reached and marked rendered.
	size_t Layout(
		ecs::Store &store, const Screen &screen, double seconds = 0.0, const TextResolutionRequest &text = {}
	);

	// Resolves one host-provided collector against its own pixel canvas.
	//
	// `PluginGui` has no world or screen canvas by itself. A host such as Studio
	// supplies the dock content size here, which keeps host geometry out of the
	// ECS while the retained descendants use the ordinary layout path.
	//
	// Only this subtree has its rendered flags cleared. Other collectors may be
	// compiled by other views of the same world in the same frame.
	//
	// @param store     The world.
	// @param collector The `LayerCollector` whose descendants are laid out.
	// @param screen    The host-owned pixel rectangle, starting at `(0, 0)`.
	// @param seconds   The caller's monotonic clock.
	// @param text      Viewer-local inputs for resolving and scaling text.
	// @return How many descendant nodes were placed.
	// @since v0.22
	size_t LayoutCollector(
		ecs::Store &store,
		ecs::Entity collector,
		const Screen &screen,
		double seconds = 0.0,
		const TextResolutionRequest &text = {}
	);

	// Preserves the first visible keyed row's screen offset after its source
	// publishes new measured extents. Called before a compile signature is read.
	void ReconcileVirtualCollectionAnchors(ecs::Store &store, ecs::Entity collector);
}

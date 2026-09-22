#pragma once

// The compiled draw list, kept until the tree it came from moves.
//
// **The expensive half is not reading the components.** It is what comes
// after: laying out, sorting siblings by `ZIndex`, intersecting clip
// rectangles, flattening the tree into a list and building a command per
// visible element. A UI is a thing somebody looks at all day, and on almost
// every one of those frames the answer is the same answer as last frame.
//
// So the scan computes a **signature** - a rolling hash of every field the
// compile reads - and when it matches the last one, the compiled list is still
// correct and is kept. This is `studio::HierarchyView`'s pattern, one layer
// over, and it is the same fallback for the same reason: `ecs::Hierarchy`, `gui::Element` and the rest are
// not observed components, so `Store::ChangeVersion` does not move when an element is reparented, resized or
// renamed - and it *does* move when a physics tick writes a transform, which would rebuild the UI sixty times
// a second for nothing.
//
// ## What the signature covers, which is the whole correctness argument
//
// | Folded in | Why it has to be |
// |---|---|
// | The store's address | Two worlds built the same way hash identically |
// | The display profile | Canvas geometry and display-scale cache inputs resolve from it |
// | Entity id, per row | A destroyed row and a new one may otherwise match |
// | `Hierarchy` parent, first child, next sibling | The order the flatten descends in |
// | `InstanceName` | `SortOrder::Name` reads it |
// | Every field of every component this module declares | All of it reaches a rectangle or a command |
// | The hovered and pressed instances | `AutoButtonColor` shifts a fill |
// | The selected `ScreenGui` source | Edit templates and live player copies must not share a list |
//
// A field added to a component has to be added to the fold in `Compile.cpp`,
// and the failure if it is not is a UI one edit stale. `gui/tests/Compile.cpp`
// is what turns that from a rule into a check: it walks every property the
// class tree declares, writes each one, and asserts the signature moved.
//
// **The direction matters and only one way round is safe.** A signature that
// *collides* keeps a list the world has moved on from, which is a UI showing
// what is no longer there; a signature that changes when nothing really did
// costs one rebuild nobody sees. Every choice here leans the second way - an
// archetype that reshuffles its rows without changing a value still
// re-compiles.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Localization.hpp>
#include <engine/gui/ShapedText.hpp>
#include <engine/gui/Style.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// Selects which authored ScreenGui roots enter a compile.
	enum class ScreenGuiSource : uint8_t {
		All,
		StarterGui,
		PlayerGui,
	};

	// What the compile needs to know that is not in the store.
	//
	// @since v0.8
	struct CompileRequest {
		// The screen a `ScreenGui` collects onto.
		Screen Display;

		// Viewer-local catalogue and locale. The catalogue remains outside ECS so
		// a client can select a language without mutating replicated UI state.
		const LocalizationCatalogue *Catalogue = nullptr;
		// Locale used to resolve localized label text.
		std::string Locale;
		// Whether Studio marks unresolved localized text.
		bool StudioMissingLocalizationMarker = false;
		// Font catalogue used to shape viewer-local text.
		const FontPackage *Fonts = nullptr;

		// The element the pointer is over, or null.
		//
		// **An input, not a result.** It shifts an `AutoButtonColor` fill, so
		// it changes the compiled list and therefore belongs in the signature.
		// Writing the hover into the component instead would make
		// `BackgroundColor3` read back differently depending on where the mouse
		// is, which is a script bug nobody could see.
		ecs::Entity Hovered;

		// The element the pointer is pressed on, or null.
		ecs::Entity Pressed;

		// The `Player` this list is being compiled for, or null.
		//
		// **Only `BillboardGui.PlayerToHideFrom` reads it, and that is enough to
		// justify it being here rather than in the renderer.** The alternative is
		// a backend testing a handle while it records - which would mean the
		// compiled list said one thing and the frame drew another, and a headless
		// test could not assert what a player actually sees. Deciding it here
		// also folds it into the signature, so switching viewers rebuilds.
		//
		// Null means "no particular viewer", which is the studio's answer and a
		// test's: nothing is hidden.
		//
		// @since v0.18
		ecs::Entity Viewer;

		// Which screen-interface root this viewer is allowed to see. `PlayerGui`
		// also requires `Viewer` and admits only that player's subtree. Generic
		// tools default to both; shipped clients select `PlayerGui`, and Studio
		// selects the template only while editing.
		ScreenGuiSource ScreenGuis = ScreenGuiSource::All;

		// The caller's monotonic clock, in seconds.
		//
		// **Passed in, never read**, which is the standing rule
		// `render::FlipbookFrameAt` and `assets::Grant::HasExpired` both keep
		// and the one this module was said to be unable to satisfy. A module
		// that read the time would hold a non-deterministic input in the
		// subsystem whose failures are hardest to reproduce, and would make a
		// suite *wait* for an animation rather than state where it is.
		//
		// The origin does not matter and only differences are used, so any
		// steady clock works. Left at zero a page slide finishes instantly on
		// the second frame, which is the honest answer for a caller that has
		// not started passing one: it still lands on the right page.
		//
		// @since v0.17
		double Seconds = 0.0;
	};

	// A compiled draw list and the signature that says whether it is still
	// good.
	//
	// Long-lived: one per surface being drawn, kept across frames. Holding one
	// per frame would compute a signature, find nothing to compare it against
	// and rebuild every time, which is every cost of this design and none of
	// its benefit.
	//
	// @since v0.8
	class Compiled {
	  public:
		// Work performed while bringing this retained list up to date. These are
		// deliberately cumulative so a host and a test can prove an unchanged
		// frame did not run a derived-state pass behind a compile cache hit.
		struct WorkCounters {
			// Binding evaluations performed by the latest rebuild.
			size_t BindingEvaluations = 0;
			// Animation samples advanced by the latest rebuild.
			size_t PresentationAdvances = 0;
			// Virtual scroll anchors reconciled by the latest rebuild.
			size_t VirtualAnchorReconciliations = 0;
			// Layout passes performed by the latest rebuild.
			size_t Layouts = 0;
		};

		// One conservative changed rectangle for a collector. It contains both
		// the old and new visible pixels, so a retained target can repaint it
		// without leaving a removed command behind.
		//
		// @since v0.22
		struct DamageRegion {
			// Collector whose cached target needs repainting.
			ecs::Entity Collector;
			// Conservative changed canvas rectangle.
			core::Rect Bounds;
			// Whether Bounds belongs to a spatial collector.
			bool Spatial = false;
		};

		// Brings the list up to date, rebuilding only if it has to.
		//
		// @param store   The world.
		// @param request The screen and the pointer state.
		// @return `true` when the list was rebuilt, `false` when the previous
		//         one was still correct. A caller uploading vertices can skip
		//         the upload on `false`.
		bool Rebuild(ecs::Store &store, const CompileRequest &request);

		// Brings one host-provided collector up to date.
		//
		// Unlike `Rebuild`, this does not discover screen or spatial collectors.
		// The named collector owns the whole draw list and its canvas is exactly
		// `request.Display`, which is the bridge a `DockWidgetPluginGui` needs.
		//
		// @param store     The world.
		// @param collector The collector hosted by an external surface.
		// @param request   The surface size, pointer state, and clock.
		// @return `true` when the list was rebuilt.
		// @since v0.22
		bool RebuildCollector(ecs::Store &store, ecs::Entity collector, const CompileRequest &request);

		// The list, whether or not this frame rebuilt it.
		const DrawList &Commands() const {
			return List;
		}

		// Regions changed by the most recent successful rebuild. They stay empty
		// on a cache hit. A false return from `DamageValid` tells an adapter to
		// repaint its whole collector and leaves the last usable baseline intact.
		const std::vector<DamageRegion> &Damage() const {
			return DamageRegions;
		}

		// Whether Damage() can be used for partial repainting.
		bool DamageValid() const {
			return DamageReady;
		}

		// The signature of what the list was built from.
		//
		// Exposed for tests and for a panel reporting why a rebuild happened.
		// Zero before the first `Rebuild`, and zero is not a reserved value -
		// what makes a comparison meaningful is that both sides came out of the
		// same function, which is also what `studio::HierarchyView` says of its
		// own stamp.
		uint64_t Signature() const {
			return Stamp;
		}

		// How many times this has rebuilt, and how many times it has been
		// asked. The ratio is the whole point of the class, so it is readable
		// rather than inferred from a profiler.
		size_t Rebuilds() const {
			return Built;
		}

		// How many times the compiled list was asked for.
		//
		// Read beside `Rebuilds`: the two being equal means the cache never hit,
		// which is the shape of a tree whose hash moves every frame.
		//
		// @return The count since construction.
		size_t Requests() const {
			return Asked;
		}

		// Cumulative derived-state work performed by this retained compiler.
		const WorkCounters &Work() const {
			return WorkDone;
		}

		// Forgets the signature so the next `Rebuild` rebuilds.
		//
		// For a caller whose *backend* state was lost - a device reset, a
		// resized target - where the list is correct and the thing that
		// consumed it is not.
		void Invalidate() {
			Stamp = 0;
			Fresh = false;
			NoCollectors = false;
			DamageRegions.clear();
			DamagePrevious.clear();
			DamageReady = true;
		}

	  private:
		struct DamageBaseline {
			ecs::Entity Collector;
			core::Rect Bounds;
			bool Spatial = false;
			uint64_t Signature = 0;
		};

		DrawList List;
		std::vector<DamageRegion> DamageRegions;
		std::vector<DamageBaseline> DamagePrevious;
		bool DamageReady = true;
		uint64_t Stamp = 0;
		uint64_t LayoutStamp = 0;
		size_t Built = 0;
		size_t Asked = 0;
		WorkCounters WorkDone;
		uint64_t DynamicEpoch = 0;
		bool DynamicFresh = false;

		// Whether `Stamp` came from a real scan rather than from the initial
		// zero. Without it, a world whose scan genuinely hashes to zero would
		// be treated as already compiled and would draw nothing, forever.
		bool Fresh = false;
		// Whether the last request proved the store had no layer collectors.
		bool NoCollectors = false;
	};

	// Collects the visual properties directly authored on an instance. Style
	// inspection uses this rather than guessing whether an engine-default value
	// was explicitly assigned.
	void CollectDirectStyleValues(const ecs::Store &store, ecs::Entity instance, StyleSet &out);

	// Every shader an `ImageLabel` or `ImageButton` in this world names,
	// without duplicates.
	//
	// **`scene::DemandedShaders`'s exact shape, one indirection flatter.**
	// That function walks `MaterialRef` because a part's shader is authored
	// on a child instance; a `Picture` carries its own name directly, so this
	// walks `Picture` rather than anything standing in for it. Both feed the
	// same `render::ShaderLibrary`, which resolves a name against the same
	// `scene::ShaderScript` tree and the same built-ins regardless of which
	// module asked.
	//
	// A `const` walk, so this may be called from a read-only consumer.
	//
	// @param store The world.
	// @param out   Filled with the names, sorted by id. Cleared first.
	// @return How many distinct shaders are named.
	// @since v0.18
	size_t DemandedShaders(ecs::Store &store, std::vector<core::Name> &out);
}

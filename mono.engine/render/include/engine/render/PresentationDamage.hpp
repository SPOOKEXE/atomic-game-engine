#pragma once

// Independent invalidation for the images composed into one presentation.
//
// A scene texture, the game's interface, and the host interface do not share a
// lifetime. Treating their signatures as one makes a moving scene rebuild UI
// geometry and makes a blinking button redraw the world. This tracker states
// the separation before a renderer or editor decides how to satisfy it.
//
// @tier L12 · client

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine::render {

	// Independent source signatures feeding the retained scene image.
	//
	// These are causes, not separate copies of the scene. The renderer keeps one
	// combined scene image, while this split says which resident input invalidated
	// it and keeps cache diagnostics from reporting every change as "scene".
	struct ScenePresentationSignatures {
		uint64_t Objects = 0;
		uint64_t Particles = 0;
		uint64_t Environment = 0;
		uint64_t Portals = 0;

		bool operator==(const ScenePresentationSignatures &) const = default;
	};

	// Persistent signatures for the independently retained presentation layers.
	struct PresentationSignatures {
		ScenePresentationSignatures Scene;
		uint64_t GameInterface = 0;
		uint64_t HostInterface = 0;
		uint64_t Viewport = 0;
	};

	// Which retained layers need new pixels or geometry.
	struct PresentationDamage {
		bool Scene = false;
		bool GameInterface = false;
		bool HostInterface = false;
		bool Viewport = false;
		bool Overlay = false;

		// Which resident scene source caused `Scene`. A forced diagnostic render
		// may set `Scene` with all four false, which is reported honestly as a
		// combined-image write rather than invented source churn.
		//@{
		bool Objects = false;
		bool Particles = false;
		bool Environment = false;
		bool Portals = false;
		//@}

		// The scene target changes when either the world or its overlaid game UI
		// changes. Host chrome is composed later and does not invalidate it.
		bool SceneImage() const {
			return Scene || GameInterface || Viewport || Overlay;
		}

		bool Any() const {
			return Scene || GameInterface || HostInterface || Viewport || Overlay;
		}
	};

	// The retained layers in dependency order. Depth is a display property only;
	// the write cascade is defined by `PresentationCacheProfile::Record`.
	enum class PresentationCacheLayer : uint8_t {
		Objects,
		Particles,
		Environment,
		PortalInputs,
		PortalHistory,
		SceneImage,
		GameInterface,
		HostInterface,
		ViewportGeometry,
		ViewportOverlay,
		GameComposition,
		StudioComposition,
		FinalImage,
		Count,
	};

	struct PresentationCacheLayerInfo {
		std::string_view Name;
		uint8_t Depth = 0;
	};

	inline constexpr std::array PRESENTATION_CACHE_LAYERS{
		PresentationCacheLayerInfo{"objects", 0},
		PresentationCacheLayerInfo{"particles", 0},
		PresentationCacheLayerInfo{"environment", 0},
		PresentationCacheLayerInfo{"portal inputs", 0},
		PresentationCacheLayerInfo{"portal history", 1},
		PresentationCacheLayerInfo{"scene image", 1},
		PresentationCacheLayerInfo{"game interface", 1},
		PresentationCacheLayerInfo{"Studio interface", 1},
		PresentationCacheLayerInfo{"viewport geometry", 1},
		PresentationCacheLayerInfo{"viewport overlay", 1},
		PresentationCacheLayerInfo{"game composition", 2},
		PresentationCacheLayerInfo{"Studio composition", 2},
		PresentationCacheLayerInfo{"final image", 3},
	};

	static_assert(PRESENTATION_CACHE_LAYERS.size() == static_cast<size_t>(PresentationCacheLayer::Count));

	// One layer's cumulative decisions and its most recent one.
	struct PresentationCacheActivity {
		uint64_t Hits = 0;
		uint64_t Writes = 0;
		bool Wrote = false;
	};

	// Cheap per-viewport cache accounting for the diagnostics panel.
	//
	// A hit is a decision, not elapsed work, so this is deliberately a counter
	// tree rather than a fake-duration span in the timing flame graph.
	class PresentationCacheProfile {
	  public:
		void Record(const PresentationDamage &damage, bool studio, bool portalHistoryWrite = false) {
			const bool sceneImage = damage.Scene || damage.Objects || damage.Particles ||
									damage.Environment || damage.Portals || portalHistoryWrite;
			const bool gameComposition =
				sceneImage || damage.GameInterface || damage.Viewport || damage.Overlay;
			const bool studioComposition = studio && (gameComposition || damage.HostInterface);
			const bool finalImage = studio ? studioComposition : gameComposition;
			const std::array writes{
				damage.Objects,
				damage.Particles,
				damage.Environment,
				damage.Portals,
				portalHistoryWrite,
				sceneImage,
				damage.GameInterface,
				damage.HostInterface,
				damage.Viewport,
				damage.Overlay,
				gameComposition,
				studioComposition,
				finalImage,
			};

			for (size_t index = 0; index < Rows.size(); index++) {
				PresentationCacheActivity &row = Rows[index];
				row.Wrote = writes[index];
				if (row.Wrote) {
					row.Writes++;
				} else {
					row.Hits++;
				}
			}
		}

		std::span<const PresentationCacheActivity> Activities() const {
			return Rows;
		}

		void Reset() {
			Rows = {};
		}

	  private:
		std::array<PresentationCacheActivity, static_cast<size_t>(PresentationCacheLayer::Count)> Rows;
	};

	// Compares a candidate presentation with the last one actually completed.
	//
	// `Inspect` does not advance the baseline. A failed swapchain acquisition may
	// retry the same damage on the next opportunity; only `Commit` says the
	// retained images now represent the candidate.
	class PresentationDamageTracker {
	  public:
		PresentationDamage Inspect(const PresentationSignatures &candidate) const {
			if (!Valid) {
				return {
					.Scene = true,
					.GameInterface = true,
					.HostInterface = true,
					.Viewport = true,
					.Objects = true,
					.Particles = true,
					.Environment = true,
					.Portals = true,
				};
			}

			const bool viewport = candidate.Viewport != Presented.Viewport;
			const bool objects = candidate.Scene.Objects != Presented.Scene.Objects;
			const bool particles = candidate.Scene.Particles != Presented.Scene.Particles;
			const bool environment = candidate.Scene.Environment != Presented.Scene.Environment;
			const bool portals = candidate.Scene.Portals != Presented.Scene.Portals;
			return {
				.Scene = viewport || objects || particles || environment || portals,
				.GameInterface = viewport || candidate.GameInterface != Presented.GameInterface,
				.HostInterface = viewport || candidate.HostInterface != Presented.HostInterface,
				.Viewport = viewport,
				.Objects = objects,
				.Particles = particles,
				.Environment = environment,
				.Portals = portals,
			};
		}

		void Commit(const PresentationSignatures &presented) {
			Presented = presented;
			Valid = true;
		}

		void Reset() {
			Presented = {};
			Valid = false;
		}

		bool HasPresentation() const {
			return Valid;
		}

		PresentationCacheProfile &CacheProfile() {
			return Cache;
		}

		const PresentationCacheProfile &CacheProfile() const {
			return Cache;
		}

	  private:
		PresentationSignatures Presented;
		PresentationCacheProfile Cache;
		bool Valid = false;
	};
}

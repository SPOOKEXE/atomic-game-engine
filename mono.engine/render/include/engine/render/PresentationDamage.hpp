#pragma once

// Independent invalidation for the images composed into one presentation.
//
// A scene texture, the game's interface, and the host interface do not share a
// lifetime. Treating their signatures as one makes a moving scene rebuild UI
// geometry and makes a blinking button redraw the world. This tracker states
// the separation before a renderer or editor decides how to satisfy it.
//
// @tier L12 · client

#include <cstdint>

namespace engine::render {

	// Persistent signatures for the independently retained presentation layers.
	struct PresentationSignatures {
		uint64_t Scene = 0;
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

		// The scene target changes when either the world or its overlaid game UI
		// changes. Host chrome is composed later and does not invalidate it.
		bool SceneImage() const {
			return Scene || GameInterface || Viewport || Overlay;
		}

		bool Any() const {
			return Scene || GameInterface || HostInterface || Viewport || Overlay;
		}
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
				return {true, true, true, true};
			}

			const bool viewport = candidate.Viewport != Presented.Viewport;
			return {
				viewport || candidate.Scene != Presented.Scene,
				viewport || candidate.GameInterface != Presented.GameInterface,
				viewport || candidate.HostInterface != Presented.HostInterface,
				viewport,
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

	  private:
		PresentationSignatures Presented;
		bool Valid = false;
	};
}

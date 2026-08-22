#pragma once

// Which textures a world knows are flipbooks, and how they play.
//
// **`MeshCatalogue`'s shape for `MeshCatalogue`'s reason, one kind over.** A
// particle emitter names a texture - rule 4, so the name survives a save file
// and a wire - and the pixels behind that name live wherever the bytes were
// read: the renderer's `TextureTable` on a client, and nowhere at all on a
// headless server. "How many frames does that sheet have and how fast was it
// drawn" is a question about the *texture*, and this is the one place in the
// shared tier that can answer it.
//
// **The alternative was making every scene state the numbers by hand.** A 4x4
// flipbook and a 4x4 tile atlas are the same pixels, so an emitter pointed at a
// GIF has to be told the grid, the frame count and the rate - and a script that
// hardcodes `FlipbookFrames = 24` is a script that is wrong the moment somebody
// re-exports the GIF with a frame added. The numbers are in the baked texture
// already; this is how they reach the thing that plays it.
//
// **Nothing here reads a texture.** `scene` depends on `core`, `ecs` and
// `spatial` and that list is not growing - AGENTS.md - so this takes three
// numbers from whoever *did* read the texture rather than taking
// `assets::TextureData` and pulling them out. The client's content pump is that
// caller today, the same one that calls `RecordMesh` two lines above.
//
// **A frame rate is not device data**, which is the test this module applies to
// everything in it: a server with no graphics stack could produce this number
// and mean it, because it is a field of a file on disk.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <cstdint>
#include <unordered_map>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// How a texture's cells are laid out and how fast they were drawn.
	//
	// @since v0.10
	struct FlipbookFacts {
		// The grid's side. Zero means the texture is a still image, or that
		// this world has not been told about it - see `TextureCatalogue::Find`.
		uint8_t Side = 0;

		// How many of the grid's cells hold a frame.
		uint8_t Frames = 0;

		// Frames a second the source was authored at, or zero when the source
		// did not say. A hand-drawn sheet says nothing; a GIF states a delay per
		// frame and this is what those average to.
		float FrameRate = 0.0f;

		// Whether this describes an animation at all.
		bool IsFlipbook() const {
			return Side > 0 && Frames > 0;
		}
	};

	// What a world knows about the textures named on its emitters.
	//
	// **Derived rather than authored, so it is not saved.** Its registration
	// writes nothing and reads back empty, exactly as `MeshCatalogue`'s does:
	// the contents come from whatever registered the textures this run, and a
	// save file carrying last run's frame counts would be numbers that agree
	// with nothing on disk.
	//
	// @since v0.10
	struct TextureCatalogue {
		// Advances whenever recorded texture facts may have changed.
		//
		// Consumers cache derived playback values for thousands of emitters, so
		// one catalogue revision lets a steady frame skip one hash lookup per
		// emitter without missing a texture that arrived after the world did.
		uint64_t Revision = 0;

		// Flipbook facts per texture, keyed by `core::Name::Id`.
		//
		// The id rather than the `Name`, matching `MeshCatalogue::Triangles`: a
		// `Name` is already an integer in this process, and hashing the integer
		// skips the registry lock that comparing text would take.
		std::unordered_map<uint32_t, FlipbookFacts> Flipbooks;

		// What is known about a texture, or a zeroed record.
		//
		// **A zero `Side` means "not known here", not "one cell".** A still
		// image and an unregistered name give the same answer on purpose:
		// neither is something to play, and a consumer that had to tell them
		// apart would be asking a question with no use.
		//
		// @param texture The texture's name.
		// @return The facts, or a zeroed record.
		FlipbookFacts Find(const core::Name &texture) const;
	};

	// The world's texture catalogue, creating an empty one if it has none.
	//
	// **`RegisterSceneComponents` must have run first**, as it must before any
	// resource here is set - `MeshesOf` carries why in full: a resource id
	// minted before the explicit registration lands takes the compiler's
	// spelling of the type, which aborts the process at a call site with nothing
	// to do with this one.
	//
	// @param store The world.
	// @return The catalogue.
	TextureCatalogue &TexturesOf(ecs::Store &store);

	// Records how a texture's frames are laid out.
	//
	// **Last writer wins, and re-registering is legal**, because that is what
	// the content path does: a publisher may replace a texture under a name it
	// already used, and a catalogue that refused the second one would keep
	// answering with the sheet that is no longer drawn.
	//
	// @param store   The world.
	// @param texture The texture's name.
	// @param facts   Its grid, frame count and rate.
	// @return `false` for an invalid name.
	bool RecordTexture(ecs::Store &store, const core::Name &texture, const FlipbookFacts &facts);

	// What is known about a texture in this world.
	//
	// The `const` reader, so a system's refresh pass can use it: it never
	// creates the resource, and a world that has none answers zeroes rather than
	// acquiring an empty one from inside a read.
	//
	// @param store   The world.
	// @param texture The texture's name.
	// @return The facts, or a zeroed record.
	FlipbookFacts FlipbookOf(const ecs::Store &store, const core::Name &texture);
}

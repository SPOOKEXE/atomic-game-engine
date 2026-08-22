#pragma once

// Renderer textures are named device resources with one shared sampler.
// Mipmaps are not represented by the current asset format.
//
// @tier L12 · client

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/Flipbook.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

struct SDL_GPUDevice;
struct SDL_GPUSampler;
struct SDL_GPUTexture;

namespace engine::render {

	// Which of the three textures a drawable naming one should sample.
	//
	// @since v0.13
	enum class TextureChoice : uint8_t {
		// The one it named. It is here.
		Named,

		// The default material - white plastic. What a drawable that named
		// nothing gets, and **what one whose sheet is still on its way gets
		// too**.
		Default,

		// The purple checkerboard. Nothing is coming for this name.
		Missing,
	};

	// The three-way choice, as a rule a suite can state.
	//
	// **A free function rather than three lines inside the draw loop**, because
	// the rule is the whole of `D00107` and it is the one part of that entry a
	// test can reach - the loop around it needs a device, a frame and content in
	// flight. A rule nothing can assert is a rule that drifts back.
	//
	// **The middle case is what the entry was about.** A texture still streaming
	// and one that will never arrive used to be the same answer, so an imported
	// model wore the marker for the frames its sheets took to land - a purple
	// shimmer across a scene load, indistinguishable from a misspelling. Now the
	// marker means *nothing is coming*, which is the only meaning that is
	// useful, and a sheet in flight looks like an untextured part until it lands.
	//
	// **No timer, deliberately.** A grace period - "draw the default for the
	// first N frames after a name is asked for" - hides a genuinely missing
	// texture for exactly as long as it hides a streaming one, and with a byte
	// budget in the path there is no N right for both a small scene and a large
	// one.
	//
	// @param found    Whether the table holds it.
	// @param named    Whether the drawable named a texture at all.
	// @param expected Whether content is on its way under that name.
	// @return Which texture to sample.
	// @since v0.13
	constexpr TextureChoice ChooseTexture(bool found, bool named, bool expected) {
		if (found) {
			return TextureChoice::Named;
		}
		if (!named || expected) {
			return TextureChoice::Default;
		}
		return TextureChoice::Missing;
	}

	// The textures a renderer can sample.
	//
	// @client
	// @since v0.9
	class TextureTable {
	  public:
		// How much device memory the table will hold, in bytes.
		//
		// Bounds device memory reachable from content.
		static constexpr size_t MAXIMUM_BYTES = 512u * 1024u * 1024u;

		TextureTable() = default;
		~TextureTable();

		TextureTable(const TextureTable &) = delete;
		TextureTable &operator=(const TextureTable &) = delete;

		// Takes the device, creates the shared sampler and uploads the default.
		//
		// @param device The GPU device. Kept, not owned.
		// @return `false` when the sampler or the default could not be created.
		bool Initialise(SDL_GPUDevice *device);

		// Releases every texture and the sampler.
		void Shutdown();

		// Uploads a texture under a name, replacing one already there.
		//
		// Uploads immediately because textures are independent resources.
		//
		// @param name  The name a `SurfaceAppearance` or a submesh will ask
		//              for.
		// @param image The pixels. An invalid one is refused.
		// @return `false` for an invalid image, a full table or a failed
		//         upload.
		bool Add(const core::Name &name, const assets::TextureData &image);

		// Takes ownership of a texture somebody else created.
		//
		// **The one way into this table that does not start from pixels on the
		// host**, and it exists for a picture the GPU already drew: a rendered
		// thumbnail is a scene target, and reading it back to the CPU only to
		// upload it again would be a round trip across the bus for bytes that
		// never needed to leave the device.
		//
		// **Ownership transfers, which is the whole contract.** The table
		// releases it on `Drop`, on replacement and on `Shutdown`, exactly as it
		// does for one it uploaded - so a caller that also released it would be
		// a double free, and one that kept using the handle after a `Drop` would
		// be reading freed device memory. Hand it over and forget it.
		//
		// @param name    The name to publish it under.
		// @param texture The texture. Null is refused.
		// @param width   Its width in pixels, for `SizeOf`.
		// @param height  Its height.
		// @param bytes   What it cost in device memory, counted against
		//                `MAXIMUM_BYTES` like any upload. A caller that guessed
		//                low would let the ceiling be walked past.
		// @return `false` for an invalid name, a null texture or a full table -
		//         and on `false` the caller still owns it.
		// @since v0.10
		bool
		Adopt(const core::Name &name, SDL_GPUTexture *texture, uint32_t width, uint32_t height, size_t bytes);

		// The texture for a name, or null when it is not registered.
		//
		// **Stays honest about absence**, which is what makes it usable for the
		// two callers that need to tell "not registered" from "registered as
		// something": a thumbnail that has not been built and a particle run
		// whose sheet has not streamed both want null. A caller that wants a
		// picture instead of an answer asks `Default()` for one.
		//
		// @param name The name.
		// @return The texture, or null.
		SDL_GPUTexture *Find(const core::Name &name) const;

		// What to sample when a drawable names no texture, or names one that is
		// not here.
		//
		// **A real texture and not the one white texel.** `render/AGENTS.md` and
		// `DefaultTexture.hpp` carry the argument: a fallback binding exists so a
		// sampler is not reading uninitialised memory, and a *default material*
		// is what an author sees on a part they have not textured. Conflating the
		// two is why every untextured part in the engine was flat white.
		//
		// **Held apart from the map rather than under a reserved name**, so no
		// `Add` can replace it and no `Drop` can release it. A name would have to
		// be one content could never spell, and a rule like that is only as good
		// as the next person who reads it.
		//
		// @return The default texture. Null only before `Initialise`.
		// @since v0.10
		SDL_GPUTexture *Default() const {
			return DefaultHandle;
		}

		// What to sample when a drawable names a texture this table does not
		// hold.
		//
		// **Not the same answer as `Default`, and that is the point.** A part
		// nobody textured is finished and looks like the default material; a
		// part that names a sheet which is not here is not finished, and drawing
		// the two the same way makes a typo indistinguishable from a decision.
		// `MissingTexture.hpp` carries why it is a purple checkerboard.
		//
		// **Held apart from the map like the default**, so no `Add` can replace
		// it and no `Drop` can release it.
		//
		// @return The marker. Null only before `Initialise`.
		// @since v0.12
		SDL_GPUTexture *Missing() const {
			return MissingHandle;
		}

		// Says that content is on its way under this name.
		//
		// **The fact the renderer was missing, and only a host can supply it.**
		// This table knows what it holds; what is in flight belongs to the
		// content pump, which is a layer this module must not reach up into. So
		// the pump tells it, on both edges - see `StopExpecting`.
		//
		// Idempotent, and it says nothing about the *kind* of the content: a
		// host asking for a mesh marks that name too, which costs a set entry
		// and is honest - "something is coming for this name" is exactly what
		// the draw loop wants to know.
		//
		// @param name What was asked for.
		// @since v0.13
		void Expect(const core::Name &name);

		// Says that nothing more is coming under this name.
		//
		// **Called when the request finishes, whichever way it finished**, and
		// that is the half the deferred entry warned about: unmarking only on
		// arrival leaves a misspelled name expected for ever, which is precisely
		// the case the marker exists for. A host that unmarks on completion
		// rather than on success cannot get it wrong in any branch.
		//
		// `Add` and `Adopt` unmark too, so an arrival needs no second call.
		//
		// @param name What was asked for.
		// @since v0.13
		void StopExpecting(const core::Name &name);

		// Whether content is on its way under this name.
		//
		// @param name The name.
		// @return `true` between `Expect` and whatever finishes it.
		// @since v0.13
		bool Expecting(const core::Name &name) const;

		// How many names are in flight.
		//
		// For a diagnostic panel and for a test: a set that only ever grows is
		// the failure mode of this pair, and a number is how somebody sees it.
		//
		// @return The count.
		// @since v0.13
		size_t Awaited() const {
			return Awaiting.size();
		}

		// How big a registered texture is, in source pixels.
		//
		// @param name   The name.
		// @param width  Set to the width, or left alone when the name is absent.
		// @param height Set to the height, likewise.
		// @return `false` for a name this table does not hold.
		// @since v0.10
		bool SizeOf(const core::Name &name, uint32_t &width, uint32_t &height) const;

		// Where this texture's current cell sits, for a sheet that animates.
		//
		// **The identity for anything that is not a sheet**, so a caller applies
		// the transform unconditionally - `render::FlipbookCell` carries why
		// that shape rather than a cell index.
		//
		// @param name    The texture.
		// @param seconds How long animation has been running. The caller's
		//                clock; this module holds none.
		// @return The transform, or the identity for a still or an absent name.
		// @since v0.10
		FlipbookCell CellOf(const core::Name &name, double seconds) const;

		// The shared sampler.
		SDL_GPUSampler *Sampler() const {
			return SharedSampler;
		}

		SDL_GPUSampler *PixelSampler() const {
			return NearestSampler;
		}

		// How many textures are registered.
		size_t Count() const {
			return Textures.size();
		}

		// Forgets a texture and releases it.
		//
		// **Because a thumbnail cache has to have a ceiling.** Everything else
		// here is content that lives as long as the session; a preview is built
		// for a row somebody scrolled past, and a table that only ever grew
		// would hold a store's worth of images in video memory by the time they
		// had browsed it.
		//
		// @param name The name to drop.
		// @return `false` for a name this table does not hold.
		// @since v0.10
		bool Drop(const core::Name &name);

		// How many bytes of device memory the table has uploaded.
		size_t Bytes() const {
			return UploadedBytes;
		}

	  private:
		// One registered texture and what it cost.
		//
		// **The size is held per texture rather than only summed**, which it was
		// not before `Drop` existed - and the sum was wrong because of it:
		// replacing a texture under a name added the new size and never
		// subtracted the old, so a session that re-registered content drifted
		// upward until `MAXIMUM_BYTES` refused an upload that would have fit.
		// Nothing noticed, because nothing replaced a texture often.
		struct Entry {
			SDL_GPUTexture *Texture = nullptr;
			size_t Bytes = 0;

			// **What was uploaded, because a caller cannot ask the device.** A
			// nine-sliced or tiled `ImageLabel` is laid out in *source* pixels -
			// `gui::DrawCommand`'s slice insets are in them - so a painter
			// resolving a name to a handle needs the dimensions with it or it
			// draws every slice at the wrong scale.
			uint32_t Width = 0;
			uint32_t Height = 0;

			// **The sheet layout, kept because the pass that plays it has only a
			// name.** A GIF bakes to an ordinary texture carrying its grid,
			// frame count and rate - `assets::TextureData` - and every one of
			// those was thrown away on upload, so nothing downstream could tell
			// an animation from a tile atlas. They are three bytes an entry.
			uint8_t FlipbookSide = 0;
			uint8_t FlipbookFrames = 0;
			float FlipbookFrameRate = 0.0f;
		};

		// Creates one device texture and fills it, widening `R8` on the way.
		//
		// **Shared by `Add` and by the default's upload**, because two copies of
		// a create-transfer-copy-submit sequence is two chances to get the row
		// pitch wrong and only one of them under test.
		//
		// Every level the image carries is uploaded. A texture baked without a
		// chain gets one level and draws exactly as it did before - **this never
		// builds levels of its own**, because the box filter that would do it
		// lives in `Engine::bake`, which nothing a shipped game links may link.
		//
		// @param image  The pixels. Assumed valid; callers check.
		// @param label  What to name in a log line if it fails.
		// @param bytes  Set to what the upload cost in device memory.
		// @return The texture, or null.
		SDL_GPUTexture *Upload(const assets::TextureData &image, std::string_view label, size_t &bytes);

		// One entry from an upload and the image it came from. See the body.
		static Entry Describe(SDL_GPUTexture *texture, size_t bytes, const assets::TextureData &image);

		SDL_GPUDevice *Device = nullptr;
		SDL_GPUSampler *SharedSampler = nullptr;
		SDL_GPUSampler *NearestSampler = nullptr;

		// The default, outside `Textures` on purpose - see `Default()`. Its
		// bytes are not counted against `MAXIMUM_BYTES`: it is sixteen kilobytes
		// the engine always holds, and a ceiling that content can spend should
		// not shrink by a constant nobody can see.
		SDL_GPUTexture *DefaultHandle = nullptr;

		// The marker, outside `Textures` for the same reason and likewise not
		// counted against `MAXIMUM_BYTES`.
		SDL_GPUTexture *MissingHandle = nullptr;

		std::unordered_map<uint32_t, Entry> Textures;

		// The names something is fetching right now. See `Expect`.
		//
		// **Interned ids rather than strings**, like `Textures` beside it: the
		// draw loop asks this once per submesh per frame and a string compare
		// per draw is what `core::Name` exists to avoid.
		std::unordered_set<uint32_t> Awaiting;

		size_t UploadedBytes = 0;
	};
}

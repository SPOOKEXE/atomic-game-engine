// A picture of a file, small enough to put in a list.
//
// **The list without them is a column of names and nothing else**, which for a
// store of two hundred and forty seven textures is a scrolling list of hashes: a
// person imports `diffuse.png`, it lands as `<64 hex>.png`, and the only way to
// tell two of them apart is to open the folder in something else. A thumbnail is
// the whole difference between a browser and a table of identifiers.
//
// ## Where the pixels come from
//
// **`raw/`, by name, and that works because the two halves of the store agree
// about names.** `cdn::Publish` walks `raw/` and uses each file's path relative
// to it as the asset's name — so a published `abc123.png` *is* `raw/abc123.png`,
// and a preview needs no chunk reassembly, no manifest walk and no delivery
// client. Both tabs of the assets panel and every picker therefore resolve a
// thumbnail the same way.
//
// The one thing that costs: an asset published from somewhere other than this
// machine's `raw/` has no local file, so it has no preview. That is honest —
// there are no pixels here to show — and it draws the kind glyph instead.
//
// ## What is previewable
//
// Images. `bake::ReadImage` reads PNG, BMP, JPEG and GIF, and `assets::Texture`
// reads a baked `.atex`. **A mesh gets no thumbnail**, and that is a real gap
// rather than an oversight: a picture of a mesh is a render, which needs a
// camera, a pass and a target — the studio has all three and pointing them at a
// list of two hundred rows is a different feature. Meshes show their kind.
//
// ## Why it is bounded, and how
//
// Every thumbnail is an upload that lives in video memory. A store somebody has
// browsed for a while would otherwise hold every image in it at once, which for
// this repository's own seed content is seven hundred megabytes of source
// decoded to rather more. So:
//
// - **only rows imgui actually drew** get one, which for a clipped table is a
//   screenful rather than a store,
// - **a few a frame**, because decoding a 4K PNG is milliseconds and doing forty
//   in one frame is a visible stall on the frame somebody scrolls,
// - **a ceiling**, past which the least recently drawn are released.
//
// The three together mean scrolling a large store costs a steady trickle rather
// than a cliff, and stopping costs nothing.

#include <engine/assets/Texture.hpp>
#include <engine/bake/Image.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <fstream>
#include <studio/Editor.hpp>
#include <vector>

namespace studio {

	namespace {
		// How wide a thumbnail is kept, in pixels.
		//
		// **Square, and the source is letterboxed into it rather than
		// stretched.** A texture atlas is wide and a UI sprite is tall, and a
		// list whose rows changed height would be a list that jumps as it
		// loads.
		constexpr uint32_t THUMBNAIL_SIDE = 64;

		// How many thumbnails may exist at once.
		//
		// 256 at 64x64 RGBA is four megabytes, which is nothing beside a scene
		// and is far more than fits on a screen.
		constexpr size_t THUMBNAIL_CEILING = 256;

		// How many to decode in one frame.
		//
		// **One, because the cost is a decode and not an upload.** A 4096-wide
		// PNG is milliseconds of Huffman and unfiltering; forty of them on the
		// frame somebody scrolls is a stall they can see. One a frame fills a
		// screenful in well under a second and never drops a frame.
		constexpr size_t THUMBNAILS_PER_FRAME = 1;

		// The largest file this will try to decode for a preview.
		//
		// **A preview is not worth an arbitrary allocation.** The pixel ceiling
		// in `bake` already bounds what a decoder will produce; this bounds what
		// is read off disk to feed it, so a hundred-megabyte TIFF somebody
		// dropped in does not become a hundred-megabyte read on the frame its
		// row scrolled past.
		constexpr uint64_t MAXIMUM_SOURCE_BYTES = 64ull * 1024u * 1024u;

		std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path &path) {
			std::error_code failure;
			const auto size = std::filesystem::file_size(path, failure);
			if (failure || size == 0 || size > MAXIMUM_SOURCE_BYTES) {
				return std::nullopt;
			}

			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return std::nullopt;
			}

			std::vector<std::byte> bytes(static_cast<size_t>(size));
			file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
			if (!file) {
				return std::nullopt;
			}
			return bytes;
		}

		// Fits an image inside a square without stretching it.
		//
		// **Letterboxed onto transparent rather than cropped or squashed.** A
		// cropped preview of an atlas shows one corner of it and looks like a
		// different asset; a squashed one makes a tall sprite unrecognisable.
		// Neither is what somebody is scanning a list for.
		bool Letterbox(const engine::assets::TextureData &source, engine::assets::TextureData &out) {
			if (!source.IsValid()) {
				return false;
			}

			const double scale = std::min(
				static_cast<double>(THUMBNAIL_SIDE) / source.Width,
				static_cast<double>(THUMBNAIL_SIDE) / source.Height
			);
			const auto width = static_cast<uint32_t>(std::max(1.0, source.Width * scale));
			const auto height = static_cast<uint32_t>(std::max(1.0, source.Height * scale));

			engine::assets::TextureData fitted;
			if (!engine::bake::ResizeImage(source, width, height, fitted)) {
				return false;
			}

			// **Widened to RGBA here rather than left as R8.** The table would
			// widen it on upload anyway, and a mask drawn as red-only would read
			// as a broken preview rather than as a mask.
			out.Width = THUMBNAIL_SIDE;
			out.Height = THUMBNAIL_SIDE;
			out.Format = engine::assets::TextureFormat::RGBA8;
			out.Pixels.assign(static_cast<size_t>(THUMBNAIL_SIDE) * THUMBNAIL_SIDE * 4, std::byte{0});

			const uint32_t left = (THUMBNAIL_SIDE - width) / 2;
			const uint32_t top = (THUMBNAIL_SIDE - height) / 2;
			const uint32_t stride = engine::assets::BytesPerPixel(fitted.Format);

			for (uint32_t y = 0; y < height; y++) {
				for (uint32_t x = 0; x < width; x++) {
					const size_t from = (static_cast<size_t>(y) * width + x) * stride;
					const size_t to = ((static_cast<size_t>(top + y) * THUMBNAIL_SIDE) + (left + x)) * 4;

					if (fitted.Format == engine::assets::TextureFormat::R8) {
						const std::byte value = fitted.Pixels[from];
						out.Pixels[to + 0] = value;
						out.Pixels[to + 1] = value;
						out.Pixels[to + 2] = value;
						out.Pixels[to + 3] = std::byte{255};
					} else {
						out.Pixels[to + 0] = fitted.Pixels[from + 0];
						out.Pixels[to + 1] = fitted.Pixels[from + 1];
						out.Pixels[to + 2] = fitted.Pixels[from + 2];
						out.Pixels[to + 3] = fitted.Pixels[from + 3];
					}
				}
			}
			return true;
		}
	}

	void *Editor::ThumbnailFor(const std::string &name) {
		if (name.empty()) {
			return nullptr;
		}

		const auto found = Thumbnails.find(name);
		if (found != Thumbnails.end()) {
			// Touched so the eviction below knows this one is still being
			// looked at.
			found->second.LastSeen = ThumbnailClock;
			return found->second.Handle;
		}

		// **Requested, not built.** Building here would decode inside whichever
		// row imgui happened to be drawing, so a screenful of new rows would be
		// a screenful of decodes in one frame — the stall this file exists to
		// avoid. `PumpThumbnails` does a bounded number of them between frames.
		if (std::find(ThumbnailQueue.begin(), ThumbnailQueue.end(), name) == ThumbnailQueue.end()) {
			ThumbnailQueue.push_back(name);
		}
		return nullptr;
	}

	void Editor::PumpThumbnails() {
		ThumbnailClock++;

		for (size_t built = 0; built < THUMBNAILS_PER_FRAME && !ThumbnailQueue.empty();) {
			const std::string name = ThumbnailQueue.front();
			ThumbnailQueue.erase(ThumbnailQueue.begin());

			if (Thumbnails.find(name) != Thumbnails.end()) {
				// Asked for twice before it was built. Not a failure and not
				// work.
				continue;
			}

			// **Recorded whether or not it worked.** A file that cannot be
			// decoded — a `.pmx`, a `.zip`, a texture from another machine —
			// must be remembered as having no preview, or its row would queue a
			// decode on every frame it is drawn.
			Entry entry;
			entry.LastSeen = ThumbnailClock;
			entry.Handle = BuildThumbnail(name);
			Thumbnails.emplace(name, entry);

			built++;
		}

		if (Thumbnails.size() <= THUMBNAIL_CEILING) {
			return;
		}

		// **Least recently drawn goes first.** Somebody scrolling downward
		// evicts what is above them, which is what they are least likely to
		// scroll back to in the next second — and if they do, it costs one
		// frame's decode.
		std::vector<std::pair<uint64_t, std::string>> byAge;
		byAge.reserve(Thumbnails.size());
		for (const auto &[key, value] : Thumbnails) {
			byAge.emplace_back(value.LastSeen, key);
		}
		std::sort(byAge.begin(), byAge.end());

		const size_t excess = Thumbnails.size() - THUMBNAIL_CEILING;
		for (size_t index = 0; index < excess; index++) {
			const std::string &victim = byAge[index].second;
			const auto found = Thumbnails.find(victim);
			if (found != Thumbnails.end()) {
				if (found->second.Handle != nullptr) {
					Renderer.DropTexture(engine::core::Name(ThumbnailTextureName(victim)));
				}
				Thumbnails.erase(found);
			}
		}
	}

	std::string Editor::ThumbnailTextureName(const std::string &name) {
		// **Prefixed, so a thumbnail can never be sampled as content.** The
		// renderer resolves a `SurfaceAppearance`'s texture by name out of the
		// same table, and a 64-pixel preview registered under the asset's real
		// name would replace the real one — a part would quietly start drawing
		// its own thumbnail.
		return "studio.thumbnail/" + name;
	}

	void *Editor::BuildThumbnail(const std::string &name) {
		// `raw/<name>`, which is the same file the publisher read — see the
		// header on why that identity holds.
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		const std::filesystem::path source = paths.Raw / name;

		std::error_code failure;
		if (!std::filesystem::is_regular_file(source, failure)) {
			return nullptr;
		}

		const std::optional<std::vector<std::byte>> bytes = ReadWholeFile(source);
		if (!bytes) {
			return nullptr;
		}

		engine::assets::TextureData decoded;

		// A baked texture reads through its own format; everything else goes to
		// the importer, which picks its decoder from the bytes rather than from
		// the extension — so a `.png` that is really a JPEG still previews.
		engine::core::ByteReader reader(*bytes);
		if (!engine::assets::Texture::Read(reader, decoded)) {
			std::string ignored;
			if (!engine::bake::ReadImage(*bytes, decoded, ignored)) {
				// Not an image, or not one this reads. **Silent**: a store holds
				// meshes, archives and text, and warning about each one every
				// time its row is drawn would bury the log.
				return nullptr;
			}
		}

		engine::assets::TextureData thumbnail;
		if (!Letterbox(decoded, thumbnail)) {
			return nullptr;
		}

		const engine::core::Name key(ThumbnailTextureName(name));
		if (!Renderer.AddTexture(key, thumbnail)) {
			return nullptr;
		}
		return Renderer.TextureHandle(key);
	}
}

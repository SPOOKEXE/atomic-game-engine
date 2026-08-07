#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/render/GlyphAtlas.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

// **The two standalone headers `mono.vendor/imgui` carries, used without
// imgui.** `stb_truetype` and `stb_rectpack` are public-domain single headers
// that imgui bundles rather than parts of imgui — including them pulls in no
// imgui symbol and puts nothing of the editor's toolkit into a game binary,
// which is the whole reason one atlas can serve both halves.
//
// The `imstb_` prefix is imgui's rename of the upstream files. Nothing else
// about them is changed, and the `STB_*_IMPLEMENTATION` defines below are what
// turn a header into a translation unit — done here, once, so there is one copy
// of the code in the binary.
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#include <imstb_rectpack.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

namespace engine::render {

	namespace {
		// The file behind each face, in `Typeface` order.
		constexpr std::array<const char *, static_cast<size_t>(Typeface::Count)> FILES{
			"Inter.ttf",
			"JetBrainsMono.ttf",
			"Roboto.ttf",
			"NotoSans.ttf",
		};

		constexpr size_t FACES = static_cast<size_t>(Typeface::Count);
		constexpr size_t PER_FACE =
			static_cast<size_t>(GlyphAtlas::LAST_CODEPOINT - GlyphAtlas::FIRST_CODEPOINT + 1);

		// A pixel of empty space around every glyph.
		//
		// **Not decoration.** A sampler filtering at the edge of a packed glyph
		// reads the neighbour's coverage, which draws as a faint smear of an
		// unrelated letter along one side — the classic atlas bleed, and it only
		// shows at non-integer scales, which is to say on somebody else's
		// machine.
		constexpr int PADDING = 1;

		std::vector<unsigned char> ReadFile(const std::filesystem::path &path) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return {};
			}
			const std::streamsize size = file.tellg();
			file.seekg(0);

			std::vector<unsigned char> bytes(static_cast<size_t>(size));
			if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
				return {};
			}
			return bytes;
		}
	}

	const Glyph *GlyphAtlas::Find(Typeface face, char32_t codepoint) const {
		if (Glyphs.empty() || face >= Typeface::Count) {
			return nullptr;
		}
		if (codepoint < FIRST_CODEPOINT || codepoint > LAST_CODEPOINT) {
			return nullptr;
		}

		const size_t index =
			1 + static_cast<size_t>(face) * PER_FACE + static_cast<size_t>(codepoint - FIRST_CODEPOINT);
		const Glyph &glyph = Glyphs[index];

		// A glyph with no advance was never baked — the face was missing, or
		// the font has no such character. Null rather than a zero-size box, so
		// a caller draws its missing marker instead of nothing.
		return glyph.Advance > 0.0f || glyph.Width > 0 ? &glyph : nullptr;
	}

	float GlyphAtlas::Measure(Typeface face, std::string_view text) const {
		float width = 0.0f;
		for (const char character : text) {
			const auto codepoint = static_cast<char32_t>(static_cast<unsigned char>(character));
			if (const Glyph *glyph = Find(face, codepoint)) {
				width += glyph->Advance;
			}
		}
		return width;
	}

	bool GlyphAtlas::Build(float pixelSize) {
		Pixels.clear();
		Glyphs.assign(FACES * PER_FACE + 1, Glyph{});
		Size = pixelSize;
		Line = 0.0f;
		White = core::Vector2{0.0f, 0.0f};
		SheetWidth = 0;
		SheetHeight = 0;

		if (pixelSize <= 0.0f) {
			return false;
		}

		const std::filesystem::path root = core::Paths::Assets() / "fonts";

		// Read every face first, because a face that failed to load must not
		// take a slot in the packing — a hole in the sheet is wasted upload.
		std::array<std::vector<unsigned char>, FACES> files;
		std::array<stbtt_fontinfo, FACES> fonts{};
		std::array<bool, FACES> loaded{};

		size_t available = 0;
		for (size_t face = 0; face < FACES; face++) {
			files[face] = ReadFile(root / FILES[face]);
			if (files[face].empty()) {
				continue;
			}
			if (stbtt_InitFont(&fonts[face], files[face].data(), 0) == 0) {
				ENGINE_WARN("glyph atlas: '{}' is not a font this build can read", FILES[face]);
				files[face].clear();
				continue;
			}
			loaded[face] = true;
			available++;
		}

		if (available == 0) {
			ENGINE_WARN("glyph atlas: no face could be read from '{}'; text will not draw", root.string());
			return false;
		}

		// **Rasterise into per-glyph bitmaps first, then pack.** The packer
		// needs every rectangle up front to place them well, and a two-pass
		// shape is also what lets a face that failed halfway be dropped without
		// leaving its glyphs half-placed.
		struct Baked {
			std::vector<unsigned char> Bitmap;
			int Width = 0;
			int Height = 0;
			int OffsetX = 0;
			int OffsetY = 0;
			float Advance = 0.0f;
			size_t Slot = 0;
		};

		std::vector<Baked> baked;
		std::vector<stbrp_rect> rects;
		baked.reserve(FACES * PER_FACE + 1);
		rects.reserve(FACES * PER_FACE + 1);

		// **The white texel is packed, not placed in a corner the packer
		// "cannot reach".** It can: `stbrp` fills the whole sheet, and a large
		// glyph landing on a hand-picked texel would put a letter's coverage
		// under every filled rectangle in the interface — a bug that appears
		// only once the atlas is full enough for that glyph to go there, which
		// is to say on the machine with the wider font.
		//
		// So it takes a rect like any glyph. Padded like any glyph too, so a
		// sampler filtering at its edge cannot read a neighbour and draw a solid
		// quad at partial alpha.
		{
			Baked white;
			white.Width = 1;
			white.Height = 1;
			white.Bitmap.assign(1, 255);
			white.Slot = 0;

			stbrp_rect rect{};
			rect.id = 0;
			rect.w = static_cast<stbrp_coord>(1 + PADDING * 2);
			rect.h = static_cast<stbrp_coord>(1 + PADDING * 2);

			baked.push_back(std::move(white));
			rects.push_back(rect);
		}

		for (size_t face = 0; face < FACES; face++) {
			if (!loaded[face]) {
				continue;
			}

			const float scale = stbtt_ScaleForPixelHeight(&fonts[face], pixelSize);

			int ascent = 0;
			int descent = 0;
			int gap = 0;
			stbtt_GetFontVMetrics(&fonts[face], &ascent, &descent, &gap);

			// The tallest face decides the line height, so a run mixing faces
			// does not overlap the line below it.
			Line = std::max(Line, static_cast<float>(ascent - descent + gap) * scale);

			for (char32_t codepoint = FIRST_CODEPOINT; codepoint <= LAST_CODEPOINT; codepoint++) {
				int advance = 0;
				int bearing = 0;
				stbtt_GetCodepointHMetrics(&fonts[face], static_cast<int>(codepoint), &advance, &bearing);

				Baked entry;
				entry.Advance = static_cast<float>(advance) * scale;

				// Offset by one, because slot 0 is the reserved white texel and
				// is not a glyph. Written as `+ 1` here rather than as a
				// sentinel checked at every read: the blit loop below skips
				// `rect.id == 0` and nothing else has to know.
				entry.Slot = 1 + face * PER_FACE + static_cast<size_t>(codepoint - FIRST_CODEPOINT);

				unsigned char *bitmap = stbtt_GetCodepointBitmap(
					&fonts[face],
					scale,
					scale,
					static_cast<int>(codepoint),
					&entry.Width,
					&entry.Height,
					&entry.OffsetX,
					&entry.OffsetY
				);

				if (bitmap != nullptr && entry.Width > 0 && entry.Height > 0) {
					entry.Bitmap.assign(
						bitmap, bitmap + static_cast<size_t>(entry.Width) * static_cast<size_t>(entry.Height)
					);

					stbrp_rect rect{};
					rect.id = static_cast<int>(baked.size());
					rect.w = static_cast<stbrp_coord>(entry.Width + PADDING * 2);
					rect.h = static_cast<stbrp_coord>(entry.Height + PADDING * 2);
					rects.push_back(rect);
				}
				// A space has metrics and no pixels, which is not a failure —
				// it keeps its advance and takes no room in the sheet.

				if (bitmap != nullptr) {
					stbtt_FreeBitmap(bitmap, nullptr);
				}
				baked.push_back(std::move(entry));
			}
		}

		// **Grown until it fits rather than guessed at.** A sheet sized by a
		// formula is either wasteful or occasionally too small, and "occasionally"
		// means on the machine with the font that happens to be a little wider.
		// Powers of two, because that is what a sampler wants and what a driver
		// will not quietly re-lay-out.
		uint32_t side = 256;
		bool packed = false;

		while (!packed && side <= 4096) {
			std::vector<stbrp_node> nodes(side);
			stbrp_context context{};
			stbrp_init_target(
				&context,
				static_cast<int>(side),
				static_cast<int>(side),
				nodes.data(),
				static_cast<int>(nodes.size())
			);

			// Reset any placement a previous, too-small attempt made.
			for (stbrp_rect &rect : rects) {
				rect.was_packed = 0;
				rect.x = 0;
				rect.y = 0;
			}

			packed = stbrp_pack_rects(&context, rects.data(), static_cast<int>(rects.size())) != 0;
			if (!packed) {
				side *= 2;
			}
		}

		if (!packed) {
			ENGINE_WARN("glyph atlas: {} glyphs did not fit in 4096x4096", rects.size());
			return false;
		}

		SheetWidth = side;
		SheetHeight = side;
		Pixels.assign(static_cast<size_t>(side) * static_cast<size_t>(side), 0);

		for (const stbrp_rect &rect : rects) {
			const Baked &entry = baked[static_cast<size_t>(rect.id)];

			const auto x = static_cast<uint32_t>(rect.x + PADDING);
			const auto y = static_cast<uint32_t>(rect.y + PADDING);

			if (rect.id == 0) {
				// The reserved texel. Its centre, because a sampler filtering
				// at a corner reads three neighbours — all empty here, which
				// would draw a solid quad at a quarter alpha.
				Pixels[static_cast<size_t>(y) * side + x] = 255;
				White = core::Vector2{
					(static_cast<float>(x) + 0.5f) / static_cast<float>(side),
					(static_cast<float>(y) + 0.5f) / static_cast<float>(side),
				};
				continue;
			}

			for (int row = 0; row < entry.Height; row++) {
				std::memcpy(
					Pixels.data() + static_cast<size_t>(y + static_cast<uint32_t>(row)) * side + x,
					entry.Bitmap.data() + static_cast<size_t>(row) * static_cast<size_t>(entry.Width),
					static_cast<size_t>(entry.Width)
				);
			}

			Glyph &glyph = Glyphs[entry.Slot];
			glyph.X = static_cast<uint16_t>(x);
			glyph.Y = static_cast<uint16_t>(y);
			glyph.Width = static_cast<uint16_t>(entry.Width);
			glyph.Height = static_cast<uint16_t>(entry.Height);
			glyph.OffsetX = static_cast<float>(entry.OffsetX);
			glyph.OffsetY = static_cast<float>(entry.OffsetY);
			glyph.Advance = entry.Advance;
		}

		// The glyphs with metrics and no pixels — the space, and anything the
		// face has an advance for and no outline. They were never packed, so
		// their advance is written here.
		for (const Baked &entry : baked) {
			if (entry.Slot == 0) {
				continue;
			}
			Glyph &glyph = Glyphs[entry.Slot];
			if (glyph.Advance == 0.0f) {
				glyph.Advance = entry.Advance;
			}
		}

		ENGINE_INFO(
			"glyph atlas: {} faces at {:.0f}px into {}x{}", available, pixelSize, SheetWidth, SheetHeight
		);
		return true;
	}
}

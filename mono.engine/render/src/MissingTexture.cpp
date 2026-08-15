#include <engine/assets/Resample.hpp>
#include <engine/render/MissingTexture.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::render {

	namespace {
		// The two colours, opaque.
		//
		// **Purple rather than pure magenta**, which is the one place this
		// departs from the convention it is borrowing. Full 255/0/255 is the
		// brightest thing an sRGB display can show and it blooms under any
		// exposure the scene happens to be at, which turns a marker into a light
		// source; pulling the red back to 160 keeps it unmistakably magenta and
		// keeps it a surface.
		constexpr uint8_t PURPLE[4] = {160, 32, 240, 255};
		constexpr uint8_t BLACK[4] = {16, 16, 16, 255};

		// **Not zero for the dark check.** A true black square lit by nothing is
		// the same pixel as an unlit surface, so the pattern vanishes in shadow
		// exactly where somebody is most likely to be hunting for what went
		// wrong. Sixteen is dark enough to read as black beside the purple and
		// bright enough to survive being multiplied by a dim light.
		static_assert(BLACK[0] > 0, "the dark check has to survive being lit");

		static_assert(MISSING_TEXTURE_SIDE % MISSING_TEXTURE_CHECK == 0, "the check has to divide the side");
	}

	const assets::TextureData &MissingTexture() {
		static const assets::TextureData image = [] {
			assets::TextureData built;
			built.Width = MISSING_TEXTURE_SIDE;
			built.Height = MISSING_TEXTURE_SIDE;
			built.Format = assets::TextureFormat::RGBA8;
			built.Pixels.resize(static_cast<size_t>(MISSING_TEXTURE_SIDE) * MISSING_TEXTURE_SIDE * 4);

			size_t at = 0;
			for (uint32_t y = 0; y < MISSING_TEXTURE_SIDE; y++) {
				for (uint32_t x = 0; x < MISSING_TEXTURE_SIDE; x++) {
					// The parity of the check a pixel falls in, which is the
					// whole pattern.
					const uint32_t check = (x / MISSING_TEXTURE_CHECK) + (y / MISSING_TEXTURE_CHECK);
					const uint8_t *const colour = (check % 2) == 0 ? PURPLE : BLACK;

					for (size_t channel = 0; channel < 4; channel++) {
						built.Pixels[at++] = static_cast<std::byte>(colour[channel]);
					}
				}
			}

			// **A marker minifies to the mean of its two colours, and that is the
			// right answer.** The point of the checkerboard is to be legible at
			// the distance somebody notices the part; without a chain it was
			// legible as speckle instead, which reads as a renderer fault rather
			// than as a missing sheet. `DefaultTexture.cpp` carries where the
			// filter lives and why it may be called from here.
			assets::BuildMipChain(built);
			return built;
		}();
		return image;
	}
}

// The picture a part gets when it names a texture that never arrived.
//
// **`Renderer.hpp` needs a GPU and this does not.** The marker is pixels built
// from a parity test on the host, so everything that makes it *work as a marker*
// — the pattern, the two colours, the fact that it tiles without a seam — is
// checkable here. What is not checkable here is the binding, and the suite says
// so rather than mocking a device to pretend otherwise.

#include <engine/assets/Texture.hpp>
#include <engine/render/DefaultTexture.hpp>
#include <engine/render/MissingTexture.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

TEST_SUITE_ID("engine.render.missingtexture")

using engine::render::MISSING_TEXTURE_CHECK;
using engine::render::MISSING_TEXTURE_SIDE;
using engine::render::MissingTexture;

namespace {

	using Texel = std::array<uint8_t, 4>;

	// One pixel of the marker.
	Texel At(uint32_t x, uint32_t y) {
		const engine::assets::TextureData &image = MissingTexture();
		const size_t offset = (static_cast<size_t>(y) * image.Width + x) * 4;

		Texel texel{};
		for (size_t channel = 0; channel < 4; channel++) {
			texel[channel] = static_cast<uint8_t>(image.Pixels[offset + channel]);
		}
		return texel;
	}
}

TEST_CASE("the marker is a full RGBA sheet", "[render][missing]") {
	const engine::assets::TextureData &image = MissingTexture();

	CHECK(image.Width == MISSING_TEXTURE_SIDE);
	CHECK(image.Height == MISSING_TEXTURE_SIDE);

	// **RGBA where the default is R8**, because the colour is the message. An
	// R8 marker would arrive as a grey checkerboard, which is a pattern somebody
	// might have authored.
	CHECK(image.Format == engine::assets::TextureFormat::RGBA8);
	CHECK(image.Pixels.size() == static_cast<size_t>(MISSING_TEXTURE_SIDE) * MISSING_TEXTURE_SIDE * 4);

	// A still image, not a flipbook — the three sheet fields stay zero or
	// `TextureTable` would try to animate it.
	CHECK(image.FlipbookSide == 0);
	CHECK(image.FlipbookFrames == 0);
}

TEST_CASE("the marker is two colours in a checkerboard", "[render][missing]") {
	std::set<Texel> colours;
	for (uint32_t y = 0; y < MISSING_TEXTURE_SIDE; y++) {
		for (uint32_t x = 0; x < MISSING_TEXTURE_SIDE; x++) {
			colours.insert(At(x, y));
		}
	}

	// Exactly two. A gradient or an anti-aliased edge would be a texture rather
	// than a marker, and it would also stop the pattern reading at a distance.
	REQUIRE(colours.size() == 2);

	// Diagonal neighbours match and orthogonal ones do not — which *is* a
	// checkerboard, stated as the property rather than as four sampled pixels.
	const uint32_t step = MISSING_TEXTURE_CHECK;
	CHECK(At(0, 0) == At(step, step));
	CHECK(At(0, 0) != At(step, 0));
	CHECK(At(0, 0) != At(0, step));

	// And it keeps alternating the whole way across, so a part big enough to
	// show many checks does not land on a flat run somewhere in the middle.
	for (uint32_t check = 0; check + 1 < MISSING_TEXTURE_SIDE / step; check++) {
		CHECK(At(check * step, 0) != At((check + 1) * step, 0));
	}
}

TEST_CASE("the marker is purple against a black that survives being lit", "[render][missing]") {
	const Texel purple = At(0, 0);
	const Texel black = At(MISSING_TEXTURE_CHECK, 0);

	// Magenta: red and blue high, green low. Stated as the relationship rather
	// than as three literals, so tuning the shade does not break the suite while
	// turning it into some other colour does.
	CHECK(purple[0] > purple[1]);
	CHECK(purple[2] > purple[1]);
	CHECK(purple[2] > 128);
	CHECK(purple[1] < 64);

	// **Not pure black.** A zero check is the same pixel as an unlit surface, so
	// the pattern would vanish in shadow — exactly where somebody is most likely
	// to be hunting for what went wrong.
	CHECK(black[0] > 0);
	CHECK(black[0] < 64);
	CHECK(black[0] == black[1]);
	CHECK(black[1] == black[2]);

	// Opaque, both of them. A marker that alpha-blended would be a hole.
	CHECK(purple[3] == 255);
	CHECK(black[3] == 255);
}

TEST_CASE("the marker tiles without a seam", "[render][missing]") {
	// The sampler repeats, and imported UVs routinely exceed one. An odd number
	// of checks across would put two same-coloured checks against each other at
	// every tile boundary — a visible stripe through the pattern that reads as
	// part of the picture.
	REQUIRE(MISSING_TEXTURE_SIDE % MISSING_TEXTURE_CHECK == 0);
	CHECK((MISSING_TEXTURE_SIDE / MISSING_TEXTURE_CHECK) % 2 == 0);

	// Which is to say: the pixel that wraps onto column zero differs from it.
	CHECK(At(MISSING_TEXTURE_SIDE - 1, 0) != At(0, 0));
	CHECK(At(0, MISSING_TEXTURE_SIDE - 1) != At(0, 0));
}

TEST_CASE("the marker is not the default material", "[render][missing]") {
	// **The whole reason it exists.** The two answer different questions — "no
	// texture" and "a texture that is not here" — and a change that quietly made
	// one delegate to the other would put the engine back where an author's typo
	// looked like a finished part.
	const engine::assets::TextureData &missing = MissingTexture();
	const engine::assets::TextureData &fallback = engine::render::DefaultTexture();

	CHECK(missing.Format != fallback.Format);
	CHECK(missing.Pixels != fallback.Pixels);
}

TEST_CASE("the marker is built once", "[render][missing]") {
	// It is bound every frame a part is missing its sheet, so a call that
	// rebuilt the pixels would be sixteen kilobytes of work per lookup.
	CHECK(&MissingTexture() == &MissingTexture());
}

TEST_CASE("a sheet still on its way is not a missing one", "[render][missingtexture]") {
	// **`D00107`, and this is the one part of it a suite can reach.** The loop
	// around the rule needs a device, a frame and content in flight; the rule
	// itself is three lines and is the whole of what the entry decided, so it
	// lives in a free function precisely so this can state it.
	using engine::render::ChooseTexture;
	using engine::render::TextureChoice;

	// Here. Nothing else matters — a name that resolves is the answer whatever
	// anybody expected.
	CHECK(ChooseTexture(true, true, false) == TextureChoice::Named);
	CHECK(ChooseTexture(true, true, true) == TextureChoice::Named);

	// Named nothing. The default material, which is what an untextured part is
	// made of — see `DefaultTexture.hpp` for why that is not the fallback texel.
	CHECK(ChooseTexture(false, false, false) == TextureChoice::Default);

	// **Named something, and it is coming.** White, not purple. This is the
	// case the entry was about: an imported model's sheets land a frame or more
	// behind the geometry they belong to, and drawing the marker meanwhile made
	// a load look like a scene full of misspellings.
	CHECK(ChooseTexture(false, true, true) == TextureChoice::Default);

	// **Named something, and nothing is coming.** The marker, and now it means
	// only that — which is the only meaning that is useful.
	CHECK(ChooseTexture(false, true, false) == TextureChoice::Missing);

	// **A name nobody expects can never be the middle case**, which is the
	// property that keeps the marker honest: there is no state where a part
	// draws white for ever because somebody forgot to unmark it. That is a
	// claim about the hosts rather than about this function, and the hosts
	// unmark on the request *finishing* rather than on it succeeding — which is
	// what makes it true in the branch that matters, a misspelled name that
	// fails.
	CHECK(ChooseTexture(false, true, false) != TextureChoice::Default);
}

#include <engine/assets/AssetKind.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.assets.assetkind")

using engine::assets::AssetKind;
using engine::assets::Describe;
using engine::assets::KindFromName;
using engine::assets::KindOfName;

TEST_CASE("an extension names a kind", "[assets]") {
	CHECK(KindOfName("meshes/rock.mesh") == AssetKind::Mesh);
	CHECK(KindOfName("meshes/rock.glb") == AssetKind::Mesh);
	CHECK(KindOfName("textures/grass.png") == AssetKind::Texture);
	CHECK(KindOfName("textures/grass.ktx2") == AssetKind::Texture);
	CHECK(KindOfName("audio/bark.wav") == AssetKind::Audio);
	CHECK(KindOfName("audio/theme.ogg") == AssetKind::Audio);
	CHECK(KindOfName("materials/stone.mat") == AssetKind::Material);
	CHECK(KindOfName("fonts/JetBrainsMono.ttf") == AssetKind::Font);
	CHECK(KindOfName("scripts/Rings.luau") == AssetKind::Script);
	CHECK(KindOfName("levels/start.agame") == AssetKind::Data);
}

TEST_CASE("the extension is matched case-insensitively", "[assets]") {
	// A content pipeline sees `.PNG` from an artist's tool often enough that
	// treating it as a different format would be a support burden with no
	// upside.
	CHECK(KindOfName("textures/GRASS.PNG") == AssetKind::Texture);
	CHECK(KindOfName("meshes/Rock.Mesh") == AssetKind::Mesh);
}

TEST_CASE("an unrecognised extension is unknown rather than a guess", "[assets]") {
	// Such an asset still delivers — an origin moves bytes it does not
	// interpret — it is simply not something a kind-filtered request returns.
	CHECK(KindOfName("data/table.xyzzy") == AssetKind::Unknown);
	CHECK(KindOfName("noextension") == AssetKind::Unknown);
	CHECK(KindOfName("") == AssetKind::Unknown);
	CHECK(KindOfName("trailing.") == AssetKind::Unknown);
}

TEST_CASE("a dot in a directory is not an extension", "[assets]") {
	// `v1.2/rock` has no extension at all, and reading `2/rock` as one would
	// file an asset under whichever kind that happened to match.
	CHECK(KindOfName("content/v1.2/rock") == AssetKind::Unknown);
	CHECK(KindOfName("content/v1.2/rock.mesh") == AssetKind::Mesh);
}

TEST_CASE("every kind has a name and the name parses back", "[assets]") {
	// The round trip is what a command line, a studio panel and a log line all
	// depend on — AGENTS.md rule 4, a name crosses and a number does not.
	for (uint8_t value = 0; value <= static_cast<uint8_t>(AssetKind::Data); ++value) {
		const auto kind = static_cast<AssetKind>(value);
		const std::string name = Describe(kind);
		CHECK_FALSE(name.empty());
		CHECK(KindFromName(name) == kind);
	}
}

TEST_CASE("an unknown name parses as unknown rather than as the first kind", "[assets]") {
	CHECK(KindFromName("sculpture") == AssetKind::Unknown);
	CHECK(KindFromName("") == AssetKind::Unknown);
	// Describe is lowercase and KindFromName expects what Describe wrote, so
	// one spelling rather than two.
	CHECK(KindFromName("Mesh") == AssetKind::Unknown);
}

TEST_CASE("unknown is zero so an unwritten field makes no claim", "[assets]") {
	// The numbers are part of the manifest format: appending is safe and
	// renumbering is not.
	CHECK(static_cast<uint8_t>(AssetKind::Unknown) == 0);
	CHECK(static_cast<uint8_t>(AssetKind::Mesh) == 1);
	CHECK(static_cast<uint8_t>(AssetKind::Texture) == 2);
	CHECK(static_cast<uint8_t>(AssetKind::Audio) == 3);
}

TEST_CASE("a source form is not something a runtime reads", "[assets]") {
	// **The distinction the extension table cannot make.** Both halves of every
	// format classify the same way on purpose — a publisher pointed at a source
	// tree and one pointed at a baked tree must agree — and that is exactly why
	// "will this load" needs a second question.
	using engine::assets::IsRuntimeReadable;

	CHECK_FALSE(IsRuntimeReadable("characters/miku.pmx"));
	CHECK_FALSE(IsRuntimeReadable("props/crate.glb"));
	CHECK_FALSE(IsRuntimeReadable("tex/skin.png"));
	CHECK_FALSE(IsRuntimeReadable("tex/sheet.gif"));
	CHECK_FALSE(IsRuntimeReadable("materials/oak.mat"));

	CHECK(IsRuntimeReadable("characters/miku.amesh"));
	CHECK(IsRuntimeReadable("tex/skin.atex"));
	CHECK(IsRuntimeReadable("materials/oak.amat"));

	// **Formats with no baked form are readable as they are**, which is why
	// this is not a list of three extensions: a baker copies these across
	// unchanged and a runtime decodes them itself.
	CHECK(IsRuntimeReadable("audio/step.wav"));
	CHECK(IsRuntimeReadable("audio/theme.mp3"));
	CHECK(IsRuntimeReadable("scripts/Rings.luau"));
	CHECK(IsRuntimeReadable("fonts/JetBrainsMono.ttf"));
	CHECK(IsRuntimeReadable("levels/start.agame"));

	// Case-insensitively, for `KindOfName`'s reason: artists' tools emit `.PNG`.
	CHECK_FALSE(IsRuntimeReadable("tex/SKIN.PNG"));

	// A name with no extension is whatever it is rather than a source.
	CHECK(IsRuntimeReadable("readme"));
	CHECK(IsRuntimeReadable("trailing."));
}

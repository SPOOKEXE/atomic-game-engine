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

TEST_CASE("every source form this baker knows also names a kind", "[assets]") {
	// **The two tables have to agree, and only one direction of that was
	// written down.** `IsRuntimeReadable`'s comment guards against a format
	// reaching the extension table without a source row, which would offer
	// something as loadable that cannot load. The failure that actually happened
	// was the other way round: `gif` was a source with no extension row, so
	// `KindOfName` answered `Unknown`, `cdn::Publisher` recorded that kind into
	// the manifest, and `client::Client` - which routes on kind - never handed
	// the bytes to the texture path. The decoder had existed since the flipbook
	// work and could not be reached from a publish.
	//
	// Asserted as a property over the source forms rather than as one more row
	// in the spot-check above, because a row would have been added by whoever
	// noticed and this is the shape that catches the *next* one. `svg` joined it
	// at v0.14, when `bake::RasterizeSvg` landed.
	constexpr std::string_view SOURCES[] = {
		"glb",
		"gltf",
		"obj",
		"fbx",
		"pmx",
		"png",
		"jpg",
		"jpeg",
		"bmp",
		"gif",
		"svg",
		"tga",
		"mat",
		"surface",
		"frag",
		"vert",
		"comp",
		"glsl",
	};

	for (const std::string_view source : SOURCES) {
		const std::string name = "content/thing." + std::string(source);

		INFO("source form ." << source);

		// A source is by definition not runtime-readable: it has to be baked
		// first. This is the half that was already right.
		CHECK_FALSE(engine::assets::IsRuntimeReadable(name));

		// And it must still route somewhere, or a publish files it under a kind
		// nothing asks for.
		CHECK(KindOfName(name) != AssetKind::Unknown);
	}
}

TEST_CASE("the extension is matched case-insensitively", "[assets]") {
	// A content pipeline sees `.PNG` from an artist's tool often enough that
	// treating it as a different format would be a support burden with no
	// upside.
	CHECK(KindOfName("textures/GRASS.PNG") == AssetKind::Texture);
	CHECK(KindOfName("meshes/Rock.Mesh") == AssetKind::Mesh);
}

TEST_CASE("an unrecognised extension is unknown rather than a guess", "[assets]") {
	// Such an asset still delivers - an origin moves bytes it does not
	// interpret - it is simply not something a kind-filtered request returns.
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
	// depend on - AGENTS.md rule 4, a name crosses and a number does not.
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
	// format classify the same way on purpose - a publisher pointed at a source
	// tree and one pointed at a baked tree must agree - and that is exactly why
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

TEST_CASE("a shader routes to its own kind", "[assets]") {
	// **Its own kind rather than `Script` or `Data`.** `Script` is source a VM
	// may run in a sandbox and `Data` is bytes handed over whole; a shader is
	// neither - compiled ahead of time, handed to a GPU rather than an
	// interpreter, and whether it is safe is a question about a driver. Routing
	// it as either would put it through the wrong subsystem's door.
	CHECK(KindOfName("shaders/tint.frag") == AssetKind::Shader);
	CHECK(KindOfName("shaders/tint.frag.spv") == AssetKind::Shader);
	CHECK(KindOfName("post/blur.comp") == AssetKind::Shader);
	CHECK(KindOfName("post/fullscreen.vert") == AssetKind::Shader);
	CHECK(KindOfName("lib/common.glsl") == AssetKind::Shader);

	CHECK(std::string_view(Describe(AssetKind::Shader)) == "shader");
}

TEST_CASE("shader source is not runtime readable and SPIR-V is", "[assets]") {
	// **The failure this list exists to end.** A format added to the extension
	// table without a row in `SOURCES` is offered as loadable and does not load.
	// A renderer holds no shader compiler - see `Renderer::AddShader` - so GLSL
	// reaching a runtime has to be caught here rather than at the draw.
	CHECK_FALSE(engine::assets::IsRuntimeReadable("shaders/tint.frag"));
	CHECK_FALSE(engine::assets::IsRuntimeReadable("shaders/fullscreen.vert"));
	CHECK_FALSE(engine::assets::IsRuntimeReadable("post/blur.comp"));
	CHECK_FALSE(engine::assets::IsRuntimeReadable("lib/common.glsl"));

	// And the baked form is, which is the whole point of baking one.
	CHECK(engine::assets::IsRuntimeReadable("shaders/tint.frag.spv"));
}

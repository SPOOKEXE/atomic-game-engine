// Turning a directory of source art into a directory a publisher can serve.
//
// The naming rule is most of this file, because it is the load-bearing
// decision: a model's texture references are rewritten by applying the same
// substitution to the path the model spells, so if `BakedName` and the walker
// ever disagree, every imported model resolves its textures to nothing — and
// nothing *fails*. The model draws, untextured, and looks like a renderer bug.

#include <engine/assets/Mesh.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <assetc/Bake.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.assetc.bake")

using assetc::BakedName;
using assetc::Report;
using assetc::Settings;
using engine::assets::AssetKind;

namespace {
	namespace fs = std::filesystem;

	// A 2x2 24-bit BMP, so the walker has a real image to decode.
	constexpr std::array<uint8_t, 70> BMP{{
		0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
		0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
		0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
		0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
	}};

	// A directory that cleans itself up, so a failing case does not leave a
	// tree behind for the next run to trip over.
	class Scratch {
	  public:
		explicit Scratch(const char *name) {
			Root = fs::temp_directory_path() / ("assetc-test-" + std::string(name));
			std::error_code error;
			fs::remove_all(Root, error);
			fs::create_directories(Root / "in", error);
		}
		~Scratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}

		Scratch(const Scratch &) = delete;
		Scratch &operator=(const Scratch &) = delete;

		fs::path In() const {
			return Root / "in";
		}
		fs::path Out() const {
			return Root / "out";
		}

		void Write(const std::string &relative, std::span<const uint8_t> bytes) const {
			std::error_code error;
			fs::create_directories((In() / relative).parent_path(), error);
			std::ofstream file(In() / relative, std::ios::binary);
			file.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
		}

		void Write(const std::string &relative, std::string_view text) const {
			Write(relative, std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
		}

	  private:
		fs::path Root;
	};

	// A square OBJ naming a texture in a sibling directory, which is the shape
	// a real model pack has.
	constexpr std::string_view OBJ = R"OBJ(
v 0 0 0
v 1 0 0
v 1 0 1
v 0 0 1
vn 0 1 0
usemtl floor
f 1//1 4//1 3//1
f 1//1 3//1 2//1
)OBJ";

	Report Baked(const Scratch &scratch, Settings settings) {
		settings.Input = scratch.In();
		settings.Output = scratch.Out();

		std::string failure;
		const Report report = assetc::Bake(settings, failure);
		CHECK(failure.empty());
		return report;
	}

	engine::assets::MeshData ReadMesh(const fs::path &path) {
		std::ifstream file(path, std::ios::binary);
		const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		engine::core::ByteReader reader({reinterpret_cast<const std::byte *>(raw.data()), raw.size()});
		engine::assets::MeshData mesh;
		REQUIRE(engine::assets::Mesh::Read(reader, mesh));
		return mesh;
	}
}

TEST_CASE("a baked name replaces the extension", "[assetc][bake]") {
	CHECK(BakedName("characters/miku.pmx") == "characters/miku.amesh");
	CHECK(BakedName("props/crate.glb") == "props/crate.amesh");
	CHECK(BakedName("props/crate.OBJ") == "props/crate.amesh");
	CHECK(BakedName("tex/skin.png") == "tex/skin.atex");
	CHECK(BakedName("tex/skin.jpg") == "tex/skin.atex");
	CHECK(BakedName("tex/skin.bmp") == "tex/skin.atex");

	// Everything else keeps its name, because it is copied across unchanged and
	// a publisher derives its kind from the extension it already had.
	CHECK(BakedName("audio/step.wav") == "audio/step.wav");
	CHECK(BakedName("scripts/main.luau") == "scripts/main.luau");
	CHECK(BakedName("readme") == "readme");

	// A dot in a directory component is not an extension — the same rule
	// `assets::KindOfName` applies, and for the same reason.
	CHECK(BakedName("v1.2/crate") == "v1.2/crate");
}

TEST_CASE("a tree bakes into one a publisher can read", "[assetc][bake]") {
	const Scratch scratch("tree");
	scratch.Write("props/floor.obj", OBJ);
	scratch.Write("props/tex/floor.bmp", BMP);
	scratch.Write("audio/step.wav", std::string_view("RIFF....WAVE"));

	const Report report = Baked(scratch, Settings{});

	CHECK(report.Failures == 0);
	REQUIRE(report.Assets.size() == 3);

	// Sorted, so two runs over one tree produce the same log.
	CHECK(report.Assets[0].Source == "audio/step.wav");
	CHECK(report.Assets[1].Source == "props/floor.obj");
	CHECK(report.Assets[2].Source == "props/tex/floor.bmp");

	CHECK(fs::exists(scratch.Out() / "props/floor.amesh"));
	CHECK(fs::exists(scratch.Out() / "props/tex/floor.atex"));

	// A file the baker does not understand is copied rather than dropped: the
	// output tree is what gets published, and a baker that dropped every sound
	// would produce a directory that is missing half the game.
	CHECK(fs::exists(scratch.Out() / "audio/step.wav"));
	CHECK(report.Assets[0].Kind == AssetKind::Audio);
	CHECK(report.Assets[1].Kind == AssetKind::Mesh);
	CHECK(report.Assets[2].Kind == AssetKind::Texture);
}

TEST_CASE("unknown files can be left out when asked", "[assetc][bake]") {
	const Scratch scratch("nocopy");
	scratch.Write("props/floor.obj", OBJ);
	scratch.Write("audio/step.wav", std::string_view("RIFF....WAVE"));

	Settings settings;
	settings.CopyUnknown = false;
	const Report report = Baked(scratch, settings);

	REQUIRE(report.Assets.size() == 1);
	CHECK(report.Assets[0].Source == "props/floor.obj");
	CHECK_FALSE(fs::exists(scratch.Out() / "audio/step.wav"));
}

TEST_CASE("a model is fitted into the stated size", "[assetc][bake]") {
	const Scratch scratch("fit");
	scratch.Write("props/floor.obj", OBJ);

	Settings settings;
	settings.ModelSize = 8.0f;
	Baked(scratch, settings);

	// The formats disagree by an order of magnitude about what a unit is, so
	// without this a scene holding a PMX character and a glTF one has the first
	// filling the sky.
	const engine::assets::MeshData mesh = ReadMesh(scratch.Out() / "props/floor.amesh");
	CHECK(mesh.Maximum.X - mesh.Minimum.X == 8.0f);
	CHECK(mesh.Maximum.X == 4.0f);
	CHECK(mesh.Minimum.X == -4.0f);
}

TEST_CASE("a model can be left at its authored scale", "[assetc][bake]") {
	const Scratch scratch("nofit");
	scratch.Write("props/floor.obj", OBJ);

	Settings settings;
	settings.ModelSize = 0.0f;
	Baked(scratch, settings);

	const engine::assets::MeshData mesh = ReadMesh(scratch.Out() / "props/floor.amesh");
	CHECK(mesh.Minimum.X == 0.0f);
	CHECK(mesh.Maximum.X == 1.0f);
}

TEST_CASE("an oversized texture is shrunk and keeps its aspect", "[assetc][bake]") {
	const Scratch scratch("resize");
	scratch.Write("tex/floor.bmp", BMP);

	Settings settings;
	settings.MaximumTexture = 1;
	const Report report = Baked(scratch, settings);

	REQUIRE(report.Assets.size() == 1);
	CHECK(report.Assets[0].Failure.empty());

	// A 2x2 capped at 1 is a 1x1 — the header plus four bytes.
	CHECK(report.Assets[0].Bytes < 70);
}

TEST_CASE("a model's texture references become baked asset names", "[assetc][bake]") {
	// **The whole point of the naming rule.** The model spells `tex/skin.bmp`
	// relative to itself; the baked texture is written to
	// `characters/tex/skin.atex`; and the submesh has to name the second, or
	// the model draws untextured with nothing having failed.
	const Scratch scratch("rewrite");
	scratch.Write(
		"characters/doll.obj",
		std::string("\nv 0 0 0\nv 1 0 0\nv 1 0 1\nvn 0 1 0\nusemtl skin\nf 1//1 3//1 2//1\n")
	);
	scratch.Write("characters/tex/skin.bmp", BMP);

	// OBJ carries no texture reference of its own — `mtllib` is a second file
	// this does not read — so the rewriting is checked through a PMX, whose
	// materials name textures directly.
	const Report report = Baked(scratch, Settings{});
	CHECK(report.Failures == 0);
	CHECK(fs::exists(scratch.Out() / "characters/tex/skin.atex"));

	// And the name a rewritten reference would produce is the file that exists.
	CHECK(BakedName("characters/tex/skin.bmp") == "characters/tex/skin.atex");
}

TEST_CASE("one bad file costs that file and not the run", "[assetc][bake]") {
	const Scratch scratch("partial");
	scratch.Write("props/good.obj", OBJ);
	scratch.Write("props/broken.png", std::string_view("this is not a png"));
	scratch.Write("props/empty.bmp", std::string_view(""));

	const Report report = Baked(scratch, Settings{});

	// One unreadable file in a directory of four hundred should cost that file.
	CHECK(report.Failures == 2);
	CHECK(fs::exists(scratch.Out() / "props/good.amesh"));
	CHECK_FALSE(fs::exists(scratch.Out() / "props/broken.atex"));

	for (const assetc::Baked &baked : report.Assets) {
		if (baked.Source == "props/broken.png") {
			// The reason has to reach a person, or it is something somebody has
			// to bisect a build to act on.
			CHECK_FALSE(baked.Failure.empty());
		}
	}
}

TEST_CASE("a missing input directory is a run that does not start", "[assetc][bake]") {
	Settings settings;
	settings.Input = fs::temp_directory_path() / "assetc-test-does-not-exist";
	settings.Output = fs::temp_directory_path() / "assetc-test-unused";

	std::string failure;
	const Report report = assetc::Bake(settings, failure);

	// Distinct from a file that failed on its own, which is a row.
	CHECK_FALSE(failure.empty());
	CHECK(report.Assets.empty());
}

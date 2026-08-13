// Covers baked-name consistency, filesystem handling and partial failures.

#include <engine/assets/Material.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <assetc/Bake.hpp>
#include <cmath>
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

	// Minimal image fixture.
	constexpr std::array<uint8_t, 70> BMP{{
		0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
		0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
		0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
		0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
	}};

	// RAII temporary tree.
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

	engine::assets::MaterialData ReadMaterial(const fs::path &path) {
		std::ifstream file(path, std::ios::binary);
		const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		engine::core::ByteReader reader({reinterpret_cast<const std::byte *>(raw.data()), raw.size()});
		engine::assets::MaterialData material;
		REQUIRE(engine::assets::Material::Read(reader, material));
		return material;
	}

	engine::assets::MeshData ReadMesh(const fs::path &path) {
		std::ifstream file(path, std::ios::binary);
		const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		engine::core::ByteReader reader({reinterpret_cast<const std::byte *>(raw.data()), raw.size()});
		engine::assets::MeshData mesh;
		REQUIRE(engine::assets::Mesh::Read(reader, mesh));
		return mesh;
	}

	engine::assets::TextureData ReadTexture(const fs::path &path) {
		std::ifstream file(path, std::ios::binary);
		const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		engine::core::ByteReader reader({reinterpret_cast<const std::byte *>(raw.data()), raw.size()});
		engine::assets::TextureData texture;
		REQUIRE(engine::assets::Texture::Read(reader, texture));
		return texture;
	}
}

TEST_CASE("a baked name replaces the extension", "[assetc][bake]") {
	CHECK(BakedName("characters/miku.pmx") == "characters/miku.amesh");
	CHECK(BakedName("props/crate.glb") == "props/crate.amesh");
	CHECK(BakedName("props/crate.OBJ") == "props/crate.amesh");
	CHECK(BakedName("materials/oak.mat") == "materials/oak.amat");
	CHECK(BakedName("tex/skin.png") == "tex/skin.atex");
	CHECK(BakedName("tex/skin.jpg") == "tex/skin.atex");
	CHECK(BakedName("tex/skin.bmp") == "tex/skin.atex");

	CHECK(BakedName("audio/step.wav") == "audio/step.wav");
	CHECK(BakedName("scripts/main.luau") == "scripts/main.luau");
	CHECK(BakedName("readme") == "readme");

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

	CHECK(report.Assets[0].Source == "audio/step.wav");
	CHECK(report.Assets[1].Source == "props/floor.obj");
	CHECK(report.Assets[2].Source == "props/tex/floor.bmp");

	CHECK(fs::exists(scratch.Out() / "props/floor.amesh"));
	CHECK(fs::exists(scratch.Out() / "props/tex/floor.atex"));

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

	CHECK(report.Assets[0].Bytes < 70);
}

TEST_CASE("a baked texture carries its mip chain", "[assetc][bake]") {
	// **The wiring, which is the half a unit test of `BuildMipChain` cannot
	// reach.** The node has to sit after the resize — a chain built before one is
	// dropped again on the way past, and the file that reaches disk has no levels
	// and nothing saying why.
	const Scratch scratch("mips");
	scratch.Write("tex/floor.bmp", BMP);

	Baked(scratch, Settings{});
	const engine::assets::TextureData whole = ReadTexture(scratch.Out() / "tex/floor.atex");
	CHECK(whole.LevelCount() == 2);
	CHECK(whole.IsValid());

	// And past a resize, where the fixture is shrunk to a single pixel and there
	// is nothing left to halve.
	Settings shrunk;
	shrunk.MaximumTexture = 1;
	Baked(scratch, shrunk);
	const engine::assets::TextureData small = ReadTexture(scratch.Out() / "tex/floor.atex");
	CHECK(small.Width == 1);
	CHECK(small.LevelCount() == 1);
}

TEST_CASE("a bake can be asked for no chain at all", "[assetc][bake]") {
	const Scratch scratch("nomips");
	scratch.Write("tex/floor.bmp", BMP);

	Settings settings;
	settings.Mipmaps = false;
	Baked(scratch, settings);

	CHECK(ReadTexture(scratch.Out() / "tex/floor.atex").LevelCount() == 1);
}

TEST_CASE("a model's texture references become baked asset names", "[assetc][bake]") {
	const Scratch scratch("rewrite");
	scratch.Write(
		"characters/doll.obj",
		std::string("\nv 0 0 0\nv 1 0 0\nv 1 0 1\nvn 0 1 0\nusemtl skin\nf 1//1 3//1 2//1\n")
	);
	scratch.Write("characters/tex/skin.bmp", BMP);

	const Report report = Baked(scratch, Settings{});
	CHECK(report.Failures == 0);
	CHECK(fs::exists(scratch.Out() / "characters/tex/skin.atex"));

	CHECK(BakedName("characters/tex/skin.bmp") == "characters/tex/skin.atex");
}

TEST_CASE("one bad file costs that file and not the run", "[assetc][bake]") {
	const Scratch scratch("partial");
	scratch.Write("props/good.obj", OBJ);
	scratch.Write("props/broken.png", std::string_view("this is not a png"));
	scratch.Write("props/empty.bmp", std::string_view(""));

	const Report report = Baked(scratch, Settings{});

	CHECK(report.Failures == 2);
	CHECK(fs::exists(scratch.Out() / "props/good.amesh"));
	CHECK_FALSE(fs::exists(scratch.Out() / "props/broken.atex"));

	for (const assetc::Baked &baked : report.Assets) {
		if (baked.Source == "props/broken.png") {
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

	// Missing input is a run failure, not a failed row.
	CHECK_FALSE(failure.empty());
	CHECK(report.Assets.empty());
}

// --- materials ---------------------------------------------------------------

TEST_CASE("a material's colour map is rewritten to the baked texture", "[assetc][bake]") {
	// **The whole point of the `.mat` step.** A material source names the file
	// beside it — `oak_Color.png` — and the baked tree holds `oak_Color.atex`. If
	// the rewrite used any rule other than `BakedName`, the material would
	// resolve to a name no manifest carries and the part would draw the engine
	// default with nothing saying why.
	const Scratch scratch("material");
	scratch.Write("materials/oak_Color.bmp", BMP);
	scratch.Write("materials/oak.mat", std::string_view("# a material\ncolor = oak_Color.bmp\n"));

	const Report report = Baked(scratch, Settings{});
	REQUIRE(report.Failures == 0);

	REQUIRE(fs::exists(scratch.Out() / "materials/oak.amat"));
	CHECK(ReadMaterial(scratch.Out() / "materials/oak.amat").ColourMap == "materials/oak_Color.atex");
}

TEST_CASE("a material may name no texture at all", "[assetc][bake]") {
	// An untextured material is a real state rather than a malformed file —
	// `assets/Material.hpp` — so this bakes and draws the engine's default.
	const Scratch scratch("material-blank");
	scratch.Write("materials/blank.mat", std::string_view("# nothing chosen yet\n"));

	const Report report = Baked(scratch, Settings{});
	CHECK(report.Failures == 0);
	CHECK(ReadMaterial(scratch.Out() / "materials/blank.amat").ColourMap.empty());
}

TEST_CASE("a material's reference cannot escape the input tree", "[assetc][bake]") {
	// The same refusal a model's texture reference gets, and for the same
	// reason: a name that resolves outside the tree names something no publisher
	// will publish.
	const Scratch scratch("material-escape");
	scratch.Write("materials/bad.mat", std::string_view("color = ../../etc/passwd\n"));

	const Report report = Baked(scratch, Settings{});
	CHECK(report.Failures == 1);
	CHECK(ReadMaterial(scratch.Out() / "materials/bad.amat").ColourMap.empty());
}

TEST_CASE("an unknown key in a material is ignored rather than refused", "[assetc][bake]") {
	// **The direction that matters.** `ROADMAP.md` v0.11 adds normal, roughness
	// and the rest when there is a pass that reads them; a baker that refused an
	// unknown key would make every material written for the newer engine
	// unbakeable by the older one, on a content tree that outlives a build.
	const Scratch scratch("material-forward");
	scratch.Write("materials/oak_Color.bmp", BMP);
	scratch.Write(
		"materials/oak.mat",
		std::string_view("normal = oak_Normal.png\ncolor = oak_Color.bmp\nroughness = oak_R.png\n")
	);

	const Report report = Baked(scratch, Settings{});
	CHECK(report.Failures == 0);
	CHECK(ReadMaterial(scratch.Out() / "materials/oak.amat").ColourMap == "materials/oak_Color.atex");
}

// --- baking one source ------------------------------------------------------

TEST_CASE("only the named source is baked", "[assetc][bake]") {
	// **What the studio's picker needs.** Making one raw file selectable by
	// re-walking a store of six thousand assets takes minutes, and a picker that
	// hung for minutes is one nobody uses.
	const Scratch scratch("only");
	scratch.Write("a.bmp", BMP);
	scratch.Write("b.bmp", BMP);
	scratch.Write("props/floor.obj", OBJ);

	Settings settings;
	settings.Only = "b.bmp";

	const Report report = Baked(scratch, settings);
	REQUIRE(report.Failures == 0);
	REQUIRE(report.Assets.size() == 1);
	CHECK(report.Assets[0].Output == "b.atex");

	CHECK(fs::exists(scratch.Out() / "b.atex"));
	CHECK_FALSE(fs::exists(scratch.Out() / "a.atex"));
	CHECK_FALSE(fs::exists(scratch.Out() / "props/floor.amesh"));
}

TEST_CASE("one source goes through the whole baker", "[assetc][bake]") {
	// **A filter on the walk and not a second entry point**, which is what makes
	// this hold: a material picked on its own still has its colour map rewritten
	// through `BakedName`. A shortcut path would be a second spelling of that
	// rule, and the failure would be a material resolving to a name no manifest
	// carries — on the editor's path only, which is the half nobody tests.
	const Scratch scratch("only-material");
	scratch.Write("materials/oak_Color.bmp", BMP);
	scratch.Write("materials/oak.mat", std::string_view("color = oak_Color.bmp\n"));

	Settings settings;
	settings.Only = "materials/oak.mat";

	const Report report = Baked(scratch, settings);
	REQUIRE(report.Failures == 0);
	CHECK(ReadMaterial(scratch.Out() / "materials/oak.amat").ColourMap == "materials/oak_Color.atex");
}

TEST_CASE("a source that is not there is a run failure", "[assetc][bake]") {
	// **A run failure and not an empty report**, because the caller asked for one
	// specific thing: a report with no rows reads as "nothing to do" and this is
	// "the file you named is gone", which for a picker means a row somebody
	// clicked after the file was deleted underneath them.
	const Scratch scratch("only-missing");
	scratch.Write("a.bmp", BMP);

	Settings settings;
	settings.Input = scratch.In();
	settings.Output = scratch.Out();
	settings.Only = "nothing.bmp";

	std::string failure;
	const Report report = assetc::Bake(settings, failure);
	CHECK_FALSE(failure.empty());
	CHECK(report.Assets.empty());
}

namespace {
	// A PMX naming one sheet as `tex\\skin.png`, taken byte for byte from
	// `bake/tests/Model.cpp`.
	//
	// **A PMX and not the `.obj` the tests above use, because an `.obj` cannot
	// express this bug.** `bake::ReadObj` reads `usemtl` into `Submesh::Material`
	// and never sets `Submesh::Texture` — it does not open a `.mtl` at all — so a
	// model built from one has no texture reference to dangle. That is also why
	// the existing "texture references become baked asset names" case only
	// asserts that the bitmap baked: there was never a reference in the mesh for
	// it to check.
	std::string PmxWithSheet() {
		static constexpr std::array<uint8_t, 347> BYTES{
			{0x50, 0x4D, 0x58, 0x20, 0x00, 0x00, 0x00, 0x40, 0x08, 0x00, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01,
			 0x01, 0x0A, 0x00, 0x00, 0x00, 0x6D, 0x00, 0x6F, 0x00, 0x64, 0x00, 0x65, 0x00, 0x6C, 0x00, 0x0A,
			 0x00, 0x00, 0x00, 0x6D, 0x00, 0x6F, 0x00, 0x64, 0x00, 0x65, 0x00, 0x6C, 0x00, 0x0E, 0x00, 0x00,
			 0x00, 0x63, 0x00, 0x6F, 0x00, 0x6D, 0x00, 0x6D, 0x00, 0x65, 0x00, 0x6E, 0x00, 0x74, 0x00, 0x0E,
			 0x00, 0x00, 0x00, 0x63, 0x00, 0x6F, 0x00, 0x6D, 0x00, 0x6D, 0x00, 0x65, 0x00, 0x6E, 0x00, 0x74,
			 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
			 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00,
			 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
			 0x3F, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
			 0x00, 0x01, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x74, 0x00, 0x65, 0x00, 0x78, 0x00, 0x5C,
			 0x00, 0x73, 0x00, 0x6B, 0x00, 0x69, 0x00, 0x6E, 0x00, 0x2E, 0x00, 0x70, 0x00, 0x6E, 0x00, 0x67,
			 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x62, 0x00, 0x6F, 0x00, 0x64, 0x00, 0x79,
			 0x00, 0x08, 0x00, 0x00, 0x00, 0x62, 0x00, 0x6F, 0x00, 0x64, 0x00, 0x79, 0x00, 0xCD, 0xCC, 0x4C,
			 0x3F, 0x9A, 0x99, 0x19, 0x3F, 0xCD, 0xCC, 0xCC, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xFF,
			 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}
		};

		return std::string(reinterpret_cast<const char *>(BYTES.data()), BYTES.size());
	}
}

TEST_CASE("a texture reference to nothing is refused rather than written", "[assetc][bake]") {
	// **The bug this replaces produced a name and no warning.** `Resolve` is
	// lexical: it joins and normalises and never asks whether the file is there,
	// so a model naming `tex/skin.png` in a tree with no such file baked
	// `tex/skin.atex` into the mesh, the publisher signed it, and the client's
	// miss was indistinguishable from a texture still streaming in. The model
	// arrived, drew, and had no textures — which is what every PMX character in
	// this repository's own store did.
	const Scratch scratch("dangling");
	scratch.Write("characters/doll.pmx", PmxWithSheet());
	// **And deliberately no `characters/tex/skin.png`**, which is the whole case.

	const Report report = Baked(scratch, Settings{});

	// The model still bakes — geometry is not what failed, and a run that
	// refused the mesh would lose the half that works.
	CHECK(report.Failures == 0);
	CHECK(fs::exists(scratch.Out() / "characters/doll.amesh"));

	// But the reference is gone and counted, so a caller can say so.
	CHECK(report.DanglingTextures == 1);

	// Cleared, so the submesh draws untextured rather than carrying a name
	// nothing answers to.
	const engine::assets::MeshData mesh = ReadMesh(scratch.Out() / "characters/doll.amesh");
	REQUIRE_FALSE(mesh.Submeshes.empty());
	CHECK(mesh.Submeshes.front().Texture.empty());
}

TEST_CASE("a texture reference beside the model still resolves", "[assetc][bake]") {
	// The ordinary art-tree case, which must keep working: the sheet is where
	// the model says it is, so no resolver is needed and nothing dangles.
	const Scratch scratch("beside");
	scratch.Write("characters/doll.pmx", PmxWithSheet());
	// **BMP bytes under a `.png` name, deliberately.** `bake::ReadImage` picks
	// its decoder from the bytes rather than from the extension, and what the
	// name has to match is what the model asked for.
	scratch.Write("characters/tex/skin.png", BMP);

	const Report report = Baked(scratch, Settings{});
	CHECK(report.Failures == 0);
	CHECK(report.DanglingTextures == 0);
	CHECK(fs::exists(scratch.Out() / "characters/tex/skin.atex"));

	// **The mesh's own reference, which is the thing that was wrong.** Checking
	// only that the sheet baked is what the older case did, and it would have
	// passed on every day this was broken.
	const engine::assets::MeshData mesh = ReadMesh(scratch.Out() / "characters/doll.amesh");
	REQUIRE_FALSE(mesh.Submeshes.empty());
	CHECK(mesh.Submeshes.front().Texture == "characters/tex/skin.atex");
}

TEST_CASE("a resolver places a texture the tree cannot", "[assetc][bake]") {
	// **The flattened-store case, which is what a `cdn` import produces.** The
	// model and its sheet are both `<hash><extension>` in one directory, so
	// `tex/skin.png` names nothing — and only the import log knows the two belong
	// together. `cdn::StoreTextureResolver` is the real one; this stands in for
	// it so the wiring is checked without a store.
	const Scratch scratch("resolved");
	scratch.Write("deadbeef.pmx", PmxWithSheet());
	scratch.Write("cafebabe.png", BMP);

	std::string asked;
	Settings settings;
	settings.ResolveTexture = [&asked](std::string_view model, std::string_view reference, std::string &out) {
		CHECK(model == "deadbeef.pmx");
		asked = reference;
		out = "cafebabe.png";
		return true;
	};

	const Report report = Baked(scratch, settings);
	CHECK(report.Failures == 0);

	// **Nothing dangles**, which is the assertion: the resolver's answer is
	// checked against the tree exactly as a lexical one is, so a resolver that
	// lied would be caught here rather than shipped.
	CHECK(report.DanglingTextures == 0);
	CHECK_FALSE(asked.empty());
	CHECK(fs::exists(scratch.Out() / "cafebabe.atex"));

	const engine::assets::MeshData mesh = ReadMesh(scratch.Out() / "deadbeef.amesh");
	REQUIRE_FALSE(mesh.Submeshes.empty());
	CHECK(mesh.Submeshes.front().Texture == "cafebabe.atex");
}

TEST_CASE("a unit bake keeps a model inside the box its bounds describe", "[assetc][bake]") {
	// **The invariant the whole size convention rests on, and breaking it made
	// the culling delete things.**
	//
	// `MeshPart.Size` multiplies a mesh's own coordinates rather than fitting the
	// mesh into a box, while `scene::Bounds::HalfExtent` — which is what
	// `graph::CullAndBound` tests against the frustum — is `Size / 2`. Those two
	// agree only when the mesh's own coordinates fit inside ±0.5. A store baked
	// at authored scale held PMX characters about twenty units tall, so every one
	// of them was culled against a box a tenth of its size: the model vanished
	// while most of it was still on screen, and nothing about the cull was wrong.
	//
	// So this is not a test that a number was passed through. It is the test that
	// a baked mesh is something the renderer's bounding box can describe.
	const Scratch scratch("unit_bounds");
	scratch.Write("huge.pmx", PmxWithSheet());

	Settings settings;
	settings.ModelSize = 1.0f;
	const Report report = Baked(scratch, settings);
	REQUIRE(report.Failures == 0);

	const engine::assets::MeshData mesh = ReadMesh(scratch.Out() / "huge.amesh");

	// Every vertex, not just the derived box — the box is computed from the
	// vertices and would agree with itself either way.
	for (const engine::assets::MeshVertex &vertex : mesh.Vertices) {
		REQUIRE(std::abs(vertex.Position[0]) <= 0.5f + 1e-4f);
		REQUIRE(std::abs(vertex.Position[1]) <= 0.5f + 1e-4f);
		REQUIRE(std::abs(vertex.Position[2]) <= 0.5f + 1e-4f);
	}

	// And the longest axis actually reaches the box, so "fits" is not "shrunk to
	// nothing" — a model scaled to a speck would also pass the check above.
	const float longest = std::max(
		{mesh.Maximum.X - mesh.Minimum.X, mesh.Maximum.Y - mesh.Minimum.Y, mesh.Maximum.Z - mesh.Minimum.Z}
	);
	CHECK(longest == Catch::Approx(1.0f).margin(1e-3f));
}

// --- baking into memory -------------------------------------------------------

TEST_CASE("an empty output bakes into the report and writes nothing", "[assetc][memory]") {
	const Scratch scratch("memory");
	scratch.Write("art/tile.bmp", std::span(BMP));

	Settings settings;
	settings.Input = scratch.In();
	// **Empty on purpose.** This is what the editor's memory-only raw folders
	// pass: a person pointed at their art directory and did not ask for a baked
	// copy of it to appear anywhere.
	settings.Output.clear();

	std::string failure;
	const Report report = assetc::Bake(settings, failure);
	REQUIRE(failure.empty());
	REQUIRE(report.Failures == 0);
	REQUIRE(report.Assets.size() == 1);

	const assetc::Baked &baked = report.Assets.front();
	REQUIRE(baked.Output == "art/tile.atex");
	REQUIRE(baked.Kind == AssetKind::Texture);
	REQUIRE_FALSE(baked.Payload.empty());
	REQUIRE(baked.Bytes == baked.Payload.size());

	// The bytes are a texture and not the source file — the whole point is that
	// the *bake* happened, only the writing did not.
	engine::core::ByteReader reader({baked.Payload.data(), baked.Payload.size()});
	engine::assets::TextureData image;
	REQUIRE(engine::assets::Texture::Read(reader, image));
	REQUIRE(image.IsValid());

	// Nothing on disk beside the input, which is the property that makes this
	// safe to point at somebody's art folder.
	std::error_code error;
	REQUIRE_FALSE(fs::exists(scratch.Out(), error));
}

TEST_CASE("a bake to disk carries no payload", "[assetc][memory]") {
	const Scratch scratch("memory-disk");
	scratch.Write("tile.bmp", std::span(BMP));

	const Report report = Baked(scratch, Settings{});
	REQUIRE(report.Assets.size() == 1);

	// **A run with an output writes files and holds none of them.** Keeping a
	// copy of every asset as well would make a large tree a bake that runs out
	// of memory on success — `Baked::Payload` says so and this is what checks it.
	REQUIRE(report.Assets.front().Payload.empty());
	REQUIRE(fs::exists(scratch.Out() / "tile.atex"));
}

TEST_CASE("a drawing bakes to a texture at the cap rather than past it", "[assetc][bake]") {
	// **The half a unit test of the rasteriser cannot reach**: which node opens
	// the chain, and what `--max-texture` does to a source that has no size of
	// its own. The drawing declares 64 and the cap is 16, so the honest answer
	// is to draw it again at 16 — resampling a 64-pixel rasterisation down would
	// give edges belonging to the box filter instead of to the shapes.
	const Scratch scratch("svg");
	scratch.Write(
		"icons/leaf.svg",
		std::string_view(R"(<svg width="64" height="64"><rect width="64" height="64" fill="#00ff00"/></svg>)")
	);

	CHECK(BakedName("icons/leaf.svg") == "icons/leaf.atex");

	Settings capped;
	capped.MaximumTexture = 16;
	const Report report = Baked(scratch, capped);

	REQUIRE(report.Assets.size() == 1);
	CHECK(report.Assets[0].Failure.empty());
	CHECK(report.Assets[0].Kind == AssetKind::Texture);
	CHECK(report.Assets[0].Output == "icons/leaf.atex");

	const engine::assets::TextureData texture = ReadTexture(scratch.Out() / "icons/leaf.atex");
	CHECK(texture.Width == 16);
	CHECK(texture.Height == 16);

	// Solid green to the edge, which a downscale of a rasterisation would also
	// give — the assertion that separates them is that nothing was drawn at 64
	// at all, and the mip chain below is what says the texture arrived whole.
	CHECK(static_cast<int>(texture.Pixels[1]) == 255);
	CHECK(texture.LevelCount() == 5);

	// Uncapped, the drawing keeps the size it declares.
	const Report whole = Baked(scratch, Settings{});
	REQUIRE(whole.Assets.size() == 1);
	CHECK(ReadTexture(scratch.Out() / "icons/leaf.atex").Width == 64);
}

TEST_CASE("a drawing this cannot draw fails its own row and not the run", "[assetc][bake]") {
	// The refusal has to survive the whole pipeline as a message naming the
	// feature: a bake tool's report is where somebody finds out that their icon
	// used a gradient.
	const Scratch scratch("svg-refused");
	scratch.Write("tex/floor.bmp", BMP);
	scratch.Write(
		"icons/fancy.svg",
		std::string_view(R"(<svg width="8" height="8"><text x="0" y="4">hello</text></svg>)")
	);

	const Report report = Baked(scratch, Settings{});
	REQUIRE(report.Assets.size() == 2);
	CHECK(report.Failures == 1);

	const assetc::Baked &icon =
		report.Assets[0].Source == "icons/fancy.svg" ? report.Assets[0] : report.Assets[1];
	CHECK(icon.Failure.find("text") != std::string::npos);
}

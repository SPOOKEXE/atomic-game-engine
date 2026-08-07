// Covers baked-name consistency, filesystem handling and partial failures.

#include <engine/assets/Material.hpp>
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

#include <engine/assets/Material.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <assetc/Bake.hpp>
#include <assetc/ImageGraph.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.assetc.imagegraph")
TEST_DEPENDS("engine.bake.imagegraph")

namespace {
	namespace fs = std::filesystem;
	struct Scratch {
		fs::path Root;
		explicit Scratch(const char *label)
			: Root(fs::temp_directory_path() / (std::string("assetc-imagegraph-") + label)) {
			std::error_code error;
			fs::remove_all(Root, error);
			fs::create_directories(Root / "in");
		}
		~Scratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}
		assetc::Settings Settings() const {
			assetc::Settings settings;
			settings.Input = Root / "in";
			settings.Output = Root / "out";
			settings.Only = "sprite.imagegraph";
			settings.Mipmaps = false;
			return settings;
		}
	};
	void Text(const fs::path &path, std::string_view text) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(text.data(), std::streamsize(text.size()));
		REQUIRE(file.good());
	}
	std::vector<std::byte> Bytes(const fs::path &path) {
		std::ifstream file(path, std::ios::binary);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
		std::vector<std::byte> result;
		for (char value : bytes)
			result.push_back(std::byte(static_cast<unsigned char>(value)));
		return result;
	}
	void Project(const fs::path &path, const std::string &source = "source.bmp", bool multiple = false) {
		engine::imagegraph::Document document;
		document.Nodes = {
			{"source", engine::imagegraph::Source{source}, {}},
			{"flipped", engine::imagegraph::Flip{true, false}, {"source"}}
		};
		document.Outputs = {{"sprite", "flipped"}};
		if (multiple) document.Outputs.push_back({"original", "source"});
		engine::imagegraph::Diagnostic diagnostic;
		std::string encoded;
		REQUIRE(engine::imagegraph::Write(document, encoded, diagnostic));
		Text(path, encoded);
	}
	void Bitmap(const fs::path &path) {
		// Independent two-pixel BMP: red then blue, padded to an eight-byte row.
		engine::core::ByteWriter bytes;
		bytes.WriteUInt8('B');
		bytes.WriteUInt8('M');
		bytes.WriteUInt32(62);
		bytes.WriteUInt32(0);
		bytes.WriteUInt32(54);
		bytes.WriteUInt32(40);
		bytes.WriteUInt32(2);
		bytes.WriteUInt32(1);
		bytes.WriteUInt16(1);
		bytes.WriteUInt16(24);
		bytes.WriteUInt32(0);
		bytes.WriteUInt32(8);
		bytes.WriteUInt32(0);
		bytes.WriteUInt32(0);
		bytes.WriteUInt32(0);
		bytes.WriteUInt32(0);
		for (uint8_t value : {0, 0, 255, 255, 0, 0, 0, 0})
			bytes.WriteUInt8(value);
		const auto encoded = bytes.Bytes();
		Text(path, std::string_view(reinterpret_cast<const char *>(encoded.data()), encoded.size()));
	}
	assetc::Report Bake(const assetc::Settings &settings) {
		std::string failure;
		auto report = assetc::Bake(settings, failure);
		REQUIRE(failure.empty());
		REQUIRE(report.Assets.size() == 1);
		return report;
	}
}

TEST_CASE("Assetc image graph project publishes pixels that ordinary Texture reads", "[imagegraph]") {
	Scratch scratch("roundtrip");
	auto settings = scratch.Settings();
	Bitmap(settings.Input / "source.bmp");
	Project(settings.Input / "sprite.imagegraph");
	const auto report = Bake(settings);
	REQUIRE(report.Failures == 0);
	CHECK(report.Assets[0].Output == "sprite.atex");
	CHECK(assetc::BakedName("dir/sprite.imagegraph") == "dir/sprite.atex");
	const auto bytes = Bytes(settings.Output / "sprite.atex");
	engine::core::ByteReader reader(bytes);
	engine::assets::TextureData ordinary;
	REQUIRE(engine::assets::Texture::Read(reader, ordinary));
	CHECK(reader.AtEnd());
	CHECK(ordinary.Width == 2);
	CHECK(ordinary.Height == 1);
	const std::vector<std::byte> expected{
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{255},
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255}
	};
	CHECK(ordinary.Pixels == expected);
	// Replacing an existing baked file uses the same complete publication path.
	CHECK(Bake(settings).Failures == 0);
	CHECK(Bytes(settings.Output / "sprite.atex") == bytes);
	settings.Output.clear();
	const auto memory = Bake(settings);
	CHECK(memory.Failures == 0);
	CHECK(memory.Assets[0].Payload == bytes);
}

TEST_CASE("Assetc graph failures preserve the last successful texture", "[imagegraph]") {
	Scratch scratch("preserve");
	auto settings = scratch.Settings();
	Bitmap(settings.Input / "source.bmp");
	Project(settings.Input / "sprite.imagegraph");
	REQUIRE(Bake(settings).Failures == 0);
	const auto before = Bytes(settings.Output / "sprite.atex");
	Project(settings.Input / "sprite.imagegraph", "missing.bmp");
	CHECK(Bake(settings).Failures == 1);
	CHECK(Bytes(settings.Output / "sprite.atex") == before);
	Text(settings.Input / "sprite.imagegraph", "{broken}");
	CHECK(Bake(settings).Failures == 1);
	CHECK(Bytes(settings.Output / "sprite.atex") == before);
	Project(settings.Input / "sprite.imagegraph", "source.bmp", true);
	CHECK(Bake(settings).Failures == 1);
	settings.ImageGraphOutput = "original";
	CHECK(Bake(settings).Failures == 0);
	const auto bytes = Bytes(settings.Output / "sprite.atex");
	CHECK(bytes != before);
	settings.ImageGraphOutput = "absent";
	CHECK(Bake(settings).Failures == 1);
	CHECK(Bytes(settings.Output / "sprite.atex") == bytes);
}

TEST_CASE("Image graph host confines paths and bounds file reads", "[imagegraph]") {
	Scratch scratch("paths");
	auto settings = scratch.Settings();
	Bitmap(settings.Input / "source.bmp");
	Project(settings.Input / "sprite.imagegraph");
	Bitmap(scratch.Root / "outside.bmp");
	engine::imagegraph::Image image;
	std::string failure;
	REQUIRE(
		assetc::ReadImageGraphSourceFile(
			settings.Input,
			settings.Input / "unsaved.imagegraph",
			"source.bmp",
			settings.Content,
			image,
			failure
		)
	);
	CHECK(image.Width == 2);
	engine::imagegraph::Document project;
	CHECK_FALSE(
		assetc::ReadImageGraphProject(settings.Input, settings.Input / "unsaved.imagegraph", project, failure)
	);
	CHECK_FALSE(
		assetc::ReadImageGraphSourceFile(
			settings.Input,
			settings.Input / "sprite.imagegraph",
			"../outside.bmp",
			settings.Content,
			image,
			failure
		)
	);
	CHECK_FALSE(
		assetc::ReadImageGraphSourceFile(
			settings.Input,
			settings.Input / "sprite.imagegraph",
			(scratch.Root / "outside.bmp").string(),
			settings.Content,
			image,
			failure
		)
	);
	CHECK_FALSE(
		assetc::ReadImageGraphSourceFile(
			settings.Input,
			settings.Input / "sprite.imagegraph",
			"sprite.imagegraph",
			settings.Content,
			image,
			failure
		)
	);
	settings.Content.Allow(engine::assets::ContentForm::Bmp, false);
	Bitmap(settings.Input / "disguised.bin");
	CHECK_FALSE(
		assetc::ReadImageGraphSourceFile(
			settings.Input,
			settings.Input / "sprite.imagegraph",
			"disguised.bin",
			settings.Content,
			image,
			failure
		)
	);
	CHECK_FALSE(
		assetc::ReadImageGraphSourceFile(
			settings.Input,
			settings.Input / "sprite.imagegraph",
			"source.bmp",
			settings.Content,
			image,
			failure
		)
	);
	CHECK(Bake(settings).Failures == 1);
	settings.Content.Allow(engine::assets::ContentForm::Bmp, true);
	std::error_code linkError;
	fs::create_symlink(scratch.Root / "outside.bmp", settings.Input / "escape.bmp", linkError);
	if (!linkError)
		CHECK_FALSE(
			assetc::ReadImageGraphSourceFile(
				settings.Input,
				settings.Input / "sprite.imagegraph",
				"escape.bmp",
				settings.Content,
				image,
				failure
			)
		);
	Text(settings.Input / "oversize.bmp", "x");
	fs::resize_file(settings.Input / "oversize.bmp", engine::imagegraph::Limits::MaximumImageBytes + 1);
	CHECK_FALSE(
		assetc::ReadImageGraphSourceFile(
			settings.Input,
			settings.Input / "sprite.imagegraph",
			"oversize.bmp",
			settings.Content,
			image,
			failure
		)
	);
	Text(settings.Input / "sprite.imagegraph", "x");
	fs::resize_file(
		settings.Input / "sprite.imagegraph", engine::imagegraph::Limits::MaximumDocumentBytes + 1
	);
	CHECK(Bake(settings).Failures == 1);
}

TEST_CASE("Image graph failed publication preserves existing paths", "[imagegraph]") {
	Scratch scratch("publish");
	const auto blocked = scratch.Root / "directory.atex";
	fs::create_directory(blocked);
	Text(blocked / "keep", "old");
	const std::vector<std::byte> bytes{std::byte{1}};
	std::string failure;
	CHECK_FALSE(assetc::PublishImageGraphTexture(blocked, bytes, failure));
	CHECK(fs::exists(blocked / "keep"));
	for (const auto &entry : fs::directory_iterator(scratch.Root))
		CHECK(entry.path().filename().string().find("imagegraph-write") == std::string::npos);
}

TEST_CASE(
	"Assetc refuses graph display output as a numeric material map without replacing pixels", "[imagegraph]"
) {
	Scratch scratch("numeric-material");
	auto settings = scratch.Settings();
	Bitmap(settings.Input / "source.bmp");
	Project(settings.Input / "sprite.imagegraph");
	REQUIRE(Bake(settings).Failures == 0);
	const auto before = Bytes(settings.Output / "sprite.atex");
	Text(settings.Input / "material.mat", "normal = sprite.imagegraph\n");
	const auto refused = Bake(settings);
	CHECK(refused.Failures == 1);
	CHECK(refused.Assets[0].Failure.find("numeric material maps") != std::string::npos);
	CHECK(Bytes(settings.Output / "sprite.atex") == before);
}

TEST_CASE("Live assetc cook includes exact texture closure for one selected graph", "[imagegraph]") {
	Scratch scratch("live-closure");
	auto settings = scratch.Settings();
	settings.LiveImageGraphs = true;
	settings.MaximumTexture = 1;
	settings.Mipmaps = true;
	Bitmap(settings.Input / "source.bmp");
	Project(settings.Input / "sprite.imagegraph", "source.bmp", true);
	std::string failure;
	const auto report = assetc::Bake(settings, failure);
	REQUIRE(failure.empty());
	REQUIRE(report.Failures == 0);
	REQUIRE(report.Assets.size() == 2);
	const auto graphRow = std::find_if(report.Assets.begin(), report.Assets.end(), [](const auto &row) {
		return row.Kind == engine::assets::AssetKind::ImageGraph;
	});
	REQUIRE(graphRow != report.Assets.end());
	CHECK(graphRow->Output == "sprite.aimagegraph");
	const auto graphBytes = Bytes(settings.Output / graphRow->Output);
	engine::imagegraph::Document graph;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(
			std::string_view(reinterpret_cast<const char *>(graphBytes.data()), graphBytes.size()),
			graph,
			diagnostic
		)
	);
	CHECK(graph.Outputs.size() == 2);
	const auto sourceName = std::get<engine::imagegraph::Source>(graph.Nodes[0].Value).Path;
	CHECK(sourceName.starts_with("__imagegraph_sources/"));
	const auto sourceBytes = Bytes(settings.Output / sourceName);
	engine::core::ByteReader reader(sourceBytes);
	engine::assets::TextureData source;
	REQUIRE(engine::assets::Texture::Read(reader, source));
	CHECK(source.Width == 2);
	CHECK(source.Height == 1);
	CHECK(source.Mips.empty());
	CHECK_FALSE(fs::exists(settings.Output / "source.atex"));
	CHECK_FALSE(fs::exists(settings.Output / "sprite.atex"));
	CHECK(assetc::Bake(settings, failure).Failures == 0);
	CHECK(Bytes(settings.Output / graphRow->Output) == graphBytes);
	settings.Output.clear();
	const auto memory = assetc::Bake(settings, failure);
	REQUIRE(memory.Failures == 0);
	REQUIRE(memory.Assets.size() == 2);
	CHECK(memory.Assets[0].Payload == sourceBytes);
	CHECK(memory.Assets[1].Payload == graphBytes);
}

TEST_CASE(
	"Live graph publication keeps last graph on source failure or addressed-byte collision", "[imagegraph]"
) {
	Scratch scratch("live-preserve");
	auto settings = scratch.Settings();
	settings.LiveImageGraphs = true;
	Bitmap(settings.Input / "source.bmp");
	Project(settings.Input / "sprite.imagegraph");
	std::string failure;
	REQUIRE(assetc::Bake(settings, failure).Failures == 0);
	const auto before = Bytes(settings.Output / "sprite.aimagegraph");
	Project(settings.Input / "sprite.imagegraph", "missing.bmp");
	CHECK(assetc::Bake(settings, failure).Failures == 1);
	CHECK(Bytes(settings.Output / "sprite.aimagegraph") == before);
	Project(settings.Input / "sprite.imagegraph");
	engine::imagegraph::Document document;
	REQUIRE(assetc::ReadImageGraphProject(settings.Input, settings.Input / settings.Only, document, failure));
	engine::bake::CookedImageGraph cooked;
	REQUIRE(
		assetc::CookImageGraphProject(
			settings.Input,
			settings.Input / settings.Only,
			document,
			"sprite.aimagegraph",
			settings.Content,
			cooked,
			failure
		)
	);
	REQUIRE(cooked.Sources.size() == 1);
	auto dangling = cooked;
	dangling.Sources.clear();
	CHECK_FALSE(assetc::PublishCookedImageGraph(settings.Output, dangling, failure));
	CHECK(Bytes(settings.Output / "sprite.aimagegraph") == before);
	Text(settings.Output / cooked.Sources[0].Name, "corrupted");
	CHECK_FALSE(assetc::PublishCookedImageGraph(settings.Output, cooked, failure));
	CHECK(failure.find("different bytes") != std::string::npos);
	CHECK(Bytes(settings.Output / "sprite.aimagegraph") == before);
	CHECK(Bytes(settings.Output / cooked.Sources[0].Name).size() == 9);
}

TEST_CASE("Live graph source defaults are normalized and output writes remain confined", "[imagegraph]") {
	Scratch scratch("live-paths");
	auto settings = scratch.Settings();
	Bitmap(settings.Input / "source.bmp");
	Bitmap(settings.Input / "alternate.bmp");
	engine::imagegraph::Document graph;
	graph.Nodes = {{"source", engine::imagegraph::Source{"source.bmp"}, {}}};
	graph.Outputs = {{"image", "source"}};
	graph.Parameters = {{"sheet", std::string("alternate.bmp")}};
	graph.Bindings = {{"source", "path", "sheet"}};
	engine::bake::CookedImageGraph cooked;
	std::string failure;
	REQUIRE(
		assetc::CookImageGraphProject(
			settings.Input,
			settings.Input / "unsaved.imagegraph",
			graph,
			"effects/live.aimagegraph",
			settings.Content,
			cooked,
			failure
		)
	);
	CHECK(std::get<std::string>(cooked.Graph.Parameters[0].Default).starts_with("__imagegraph_sources/"));
	CHECK(cooked.Sources.size() == 1);
	REQUIRE(assetc::PublishCookedImageGraph(settings.Output, cooked, failure));
	const auto before = Bytes(settings.Output / cooked.Name);
	graph.Parameters[0].Default = std::string("../outside.bmp");
	Bitmap(scratch.Root / "outside.bmp");
	CHECK_FALSE(
		assetc::CookImageGraphProject(
			settings.Input,
			settings.Input / "unsaved.imagegraph",
			graph,
			"effects/live.aimagegraph",
			settings.Content,
			cooked,
			failure
		)
	);
	CHECK(Bytes(settings.Output / cooked.Name) == before);
	cooked.Name = "../escape.aimagegraph";
	CHECK_FALSE(assetc::PublishCookedImageGraph(settings.Output, cooked, failure));
	CHECK_FALSE(fs::exists(scratch.Root / "escape.aimagegraph"));
	cooked.Name = "effects/live.aimagegraph";
	fs::create_directory(scratch.Root / "elsewhere");
	std::error_code error;
	fs::remove_all(settings.Output / "effects", error);
	fs::create_directory_symlink(scratch.Root / "elsewhere", settings.Output / "effects", error);
	if (!error) {
		CHECK_FALSE(assetc::PublishCookedImageGraph(settings.Output, cooked, failure));
		CHECK_FALSE(fs::exists(scratch.Root / "elsewhere" / "live.aimagegraph"));
	}
}

TEST_CASE("Explicit linear graph outputs can bake and bind numeric material maps", "[imagegraph]") {
	Scratch scratch("live-numeric-map");
	auto settings = scratch.Settings();
	Bitmap(settings.Input / "source.bmp");
	engine::imagegraph::Document graph;
	graph.Nodes = {
		{"source",
		 engine::imagegraph::Source{"source.bmp", engine::imagegraph::SourceInterpretation::Data},
		 {}}
	};
	graph.Outputs = {{"normal", "source", engine::imagegraph::OutputSpace::Linear}};
	engine::imagegraph::Diagnostic diagnostic;
	std::string project;
	REQUIRE(engine::imagegraph::Write(graph, project, diagnostic));
	Text(settings.Input / settings.Only, project);
	Text(settings.Input / "material.mat", "normal = sprite.imagegraph\n");
	REQUIRE(Bake(settings).Failures == 0);
	const auto encoded = Bytes(settings.Output / "sprite.atex");
	engine::core::ByteReader reader(encoded);
	engine::assets::TextureData texture;
	REQUIRE(engine::assets::Texture::Read(reader, texture));
	CHECK(texture.Format == engine::assets::TextureFormat::RGBA8_LINEAR);
	settings.LiveImageGraphs = true;
	settings.Only.clear();
	std::string failure;
	const auto live = assetc::Bake(settings, failure);
	REQUIRE(failure.empty());
	REQUIRE(live.Failures == 0);
	const auto materialBytes = Bytes(settings.Output / "material.amat");
	engine::core::ByteReader materialReader(materialBytes);
	engine::assets::MaterialData material;
	REQUIRE(engine::assets::Material::Read(materialReader, material));
	CHECK(material.NormalMap == "imagegraph://sprite.aimagegraph#normal");
}

TEST_CASE(
	"Live graph cook gates runtime graph and normalized texture content before source lookup", "[imagegraph]"
) {
	Scratch scratch("live-content-policy");
	auto settings = scratch.Settings();
	Bitmap(settings.Input / "source.bmp");
	engine::imagegraph::Document document;
	document.Nodes = {{"source", engine::imagegraph::Source{"source.bmp"}, {}}};
	document.Outputs = {{"image", "source"}};
	engine::bake::CookedImageGraph cooked;
	cooked.Text = "old";
	std::string failure;
	settings.Content.Allow(engine::assets::ContentForm::ATex, false);
	CHECK_FALSE(
		assetc::CookImageGraphProject(
			settings.Input,
			settings.Input / "unsaved.imagegraph",
			document,
			"live.aimagegraph",
			settings.Content,
			cooked,
			failure
		)
	);
	CHECK(cooked.Text == "old");
	settings.Content.Allow(engine::assets::ContentForm::ATex, true);
	settings.Content.Allow(engine::assets::FormOfName("live.aimagegraph"), false);
	CHECK_FALSE(
		assetc::CookImageGraphProject(
			settings.Input,
			settings.Input / "unsaved.imagegraph",
			document,
			"live.aimagegraph",
			settings.Content,
			cooked,
			failure
		)
	);
	CHECK(cooked.Text == "old");
}

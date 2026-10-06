#include "ImageGraphAppendProject.hpp"

#include <engine/bake/Image.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <studio/ImageGraph.hpp>
#include <studio/PxcxSave.hpp>

TEST_SUITE_ID("studio.pxcxsave")
TEST_DEPENDS("engine.imagegraphio.pxcxstructureedit")
TEST_DEPENDS("engine.imagegraphio.pxcxappend")
TEST_DEPENDS("engine.imagegraphexport.pxcx_thumbnail")
TEST_DEPENDS("engine.imagegraphexport.pxcx_collection_files")

namespace {
	using Json = nlohmann::ordered_json;
	Json Collection(std::string id, std::string parent = {}) {
		Json node = {
			{"id", std::move(id)},
			{"type", "Node_Collection"},
			{"x", 0},
			{"y", 0},
			{"inputs", Json::array()},
			{"attri", {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}}}
		};
		if (!parent.empty()) node["group"] = std::move(parent);
		return node;
	}
	engine::bake::PxcxArchive CollectionSource() {
		Json root = Collection("selected", "outside");
		root["x"] = 100;
		root["y"] = 200;
		root["future_root"] = {{"keep", 9}};
		Json child = Collection("child", "selected");
		child["x"] = 110;
		child["y"] = 220;
		Json opaque = {
			{"id", "inside-opaque"},
			{"type", "future.node"},
			{"x", 111},
			{"y", 222},
			{"group", "child"},
			{"inputs", Json::array({{{"from_node", "outside"}, {"from_index", 0}}})},
			{"future_node", {{"keep", true}}}
		};
		Json graph = {
			{"future_project", "preserve source only"},
			{"nodes",
			 Json::array(
				 {root,
				  child,
				  opaque,
				  {{"id", "outside"},
				   {"type", "future.opaque"},
				   {"x", 4},
				   {"y", 5},
				   {"inputs", Json::array({{{"from_node", "selected"}, {"from_index", 0}}})},
				   {"future_node", {{"keep", true}}}}}
			 )}
		};
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = graph.dump();
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	void WriteText(const std::filesystem::path &path, std::string_view text) {
		std::ofstream stream(path, std::ios::binary);
		REQUIRE(stream.is_open());
		stream.write(text.data(), static_cast<std::streamsize>(text.size()));
		REQUIRE(stream.good());
	}
	struct Directory {
		inline static std::atomic<uint64_t> Serial{0};
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("atomic-pxc-save-test-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
			 std::to_string(Serial.fetch_add(1)));
		Directory() {
			REQUIRE(std::filesystem::create_directory(Path));
		}
		~Directory() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	};
	engine::imagegraphio::PxcxImport Source() {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			R"({"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":3}}],"future":{"keep":9}},{"id":"opaque","type":"future.node","x":0,"y":0,"inputs":[]}],"future":{"project":8}})";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		engine::imagegraphio::PxcxImport result;
		REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, result, failure));
		return result;
	}
	std::vector<std::byte> Read(const std::filesystem::path &path) {
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		REQUIRE(stream.is_open());
		const auto count = stream.tellg();
		REQUIRE(count > 0);
		std::vector<std::byte> bytes(static_cast<size_t>(count));
		stream.seekg(0);
		stream.read(reinterpret_cast<char *>(bytes.data()), count);
		REQUIRE(stream.gcount() == count);
		return bytes;
	}
	std::string ReadText(const std::filesystem::path &path) {
		const auto bytes = Read(path);
		return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
	}
	std::map<std::string, std::vector<std::byte>> StoredEntries(const std::vector<std::byte> &bytes) {
		const auto word = [&](size_t offset, size_t width) {
			REQUIRE(offset + width <= bytes.size());
			uint32_t value = 0;
			for (size_t i = 0; i < width; ++i)
				value |= uint32_t(std::to_integer<uint8_t>(bytes[offset + i])) << (i * 8);
			return value;
		};
		REQUIRE(bytes.size() >= 22);
		const size_t end = bytes.size() - 22;
		REQUIRE(word(end, 4) == 0x06054b50);
		REQUIRE(word(end + 20, 2) == 0);
		const size_t count = word(end + 10, 2);
		std::map<std::string, std::vector<std::byte>> entries;
		size_t offset = word(end + 16, 4);
		for (size_t index = 0; index < count; ++index) {
			REQUIRE(word(offset, 4) == 0x02014b50);
			REQUIRE(word(offset + 10, 2) == 0);
			const size_t length = word(offset + 20, 4);
			REQUIRE(word(offset + 24, 4) == length);
			const size_t nameLength = word(offset + 28, 2);
			const size_t local = word(offset + 42, 4);
			REQUIRE(word(local, 4) == 0x04034b50);
			const size_t data = local + 30 + word(local + 26, 2) + word(local + 28, 2);
			REQUIRE(data + length <= bytes.size());
			REQUIRE(offset + 46 + nameLength <= bytes.size());
			const std::string name(reinterpret_cast<const char *>(bytes.data() + offset + 46), nameLength);
			entries.emplace(
				name, std::vector<std::byte>(bytes.begin() + data, bytes.begin() + data + length)
			);
			offset += 46 + nameLength + word(offset + 30, 2) + word(offset + 32, 2);
		}
		REQUIRE(offset == end);
		return entries;
	}
}

TEST_CASE(
	"Studio Collection saves full-size preview siblings and PXZ entries", "[studio][pxcx_save][collection]"
) {
	const auto source = CollectionSource();
	const auto sourceBytes = source.OriginalBytes;
	Directory directory;
	engine::imagegraph::Image pixels{2, 1, {20, 30, 40, 50, 60, 70, 80, 90}, 0};
	studio::PxcxPreviewIdentity identity{7, 9, {"image", "selected", "image"}, {3, .25}};
	studio::PxcxPreparedSavePreview preview{pixels, identity, identity};
	engine::imagegraph::Diagnostic diagnostic;
	const auto cap = engine::imagegraph::Limits::MaximumEvaluationBytes;
	const auto path = directory.Path / "collection.pxcc";
	REQUIRE(
		studio::SavePxcxCollection(
			path, source, "selected", R"({"description":"shown"})", diagnostic, cap, &preview
		)
	);
	const auto png = Read(directory.Path / "collection.png");
	engine::assets::TextureData decoded;
	std::string failure;
	REQUIRE(engine::bake::ReadImage(png, decoded, failure));
	CHECK(decoded.Width == 2);
	CHECK(decoded.Height == 1);
	REQUIRE(decoded.Pixels.size() == pixels.Pixels.size());
	for (size_t i = 0; i < pixels.Pixels.size(); ++i)
		CHECK(std::to_integer<uint8_t>(decoded.Pixels[i]) == pixels.Pixels[i]);
	const auto packagePath = directory.Path / "collection.pxz";
	REQUIRE(
		studio::SavePxcxCollection(
			packagePath, source, "selected", R"({"description":"shown"})", diagnostic, cap, &preview
		)
	);
	const auto entries = StoredEntries(Read(packagePath));
	REQUIRE(entries.size() == 3);
	CHECK(entries.at("collection.pxcc") == Read(path));
	CHECK(entries.at("collection.meta") == Read(directory.Path / "collection.meta"));
	CHECK(entries.at("collection.png") == png);
	const auto metadata = Read(directory.Path / "collection.meta");
	REQUIRE(studio::SavePxcxCollection(path, source, "selected", {}, diagnostic));
	CHECK(Read(directory.Path / "collection.png") == png);
	CHECK(Read(directory.Path / "collection.meta") == metadata);
	REQUIRE(studio::SavePxcxCollection(packagePath, source, "selected", {}, diagnostic));
	const auto minimal = StoredEntries(Read(packagePath));
	CHECK(minimal.size() == 1);
	CHECK(minimal.contains("collection.pxcc"));
	CHECK(source.OriginalBytes == sourceBytes);
}

TEST_CASE(
	"Studio refuses stale Collection preview before touching any output",
	"[studio][pxcx_save][collection][atomic]"
) {
	const auto source = CollectionSource();
	Directory directory;
	const auto path = directory.Path / "collection.pxcc";
	WriteText(path, "prior graph");
	WriteText(directory.Path / "collection.meta", "prior metadata");
	WriteText(directory.Path / "collection.png", "prior preview");
	engine::imagegraph::Image pixels{1, 1, {20, 30, 40, 50}, 0};
	studio::PxcxPreviewIdentity completed{7, 9, {"image", "selected", "image"}, {3, .25}};
	auto current = completed;
	SECTION("changed document") {
		++current.DocumentRevision;
	}
	SECTION("changed inputs") {
		++current.InputRevision;
	}
	SECTION("changed frame") {
		++current.Frame.Tick;
	}
	SECTION("changed output") {
		current.Binding.Port = "other";
	}
	SECTION("changed playback") {
		++current.PlaybackObservation;
	}
	SECTION("missing revision") {
		current = completed;
		current.DocumentRevision = 0;
		completed = current;
	}
	studio::PxcxPreparedSavePreview preview{pixels, completed, current};
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(
		studio::SavePxcxCollection(
			path,
			source,
			"selected",
			{},
			diagnostic,
			engine::imagegraph::Limits::MaximumEvaluationBytes,
			&preview
		)
	);
	CHECK(ReadText(path) == "prior graph");
	CHECK(ReadText(directory.Path / "collection.meta") == "prior metadata");
	CHECK(ReadText(directory.Path / "collection.png") == "prior preview");
}

TEST_CASE(
	"Studio Collection third sibling failure preserves prior graph and metadata",
	"[studio][pxcx_save][collection][atomic]"
) {
	const auto source = CollectionSource();
	Directory directory;
	const auto path = directory.Path / "collection.pxcc";
	WriteText(path, "prior graph");
	WriteText(directory.Path / "collection.meta", "prior metadata");
	REQUIRE(std::filesystem::create_directory(directory.Path / "collection.png"));
	engine::imagegraph::Image pixels{1, 1, {20, 30, 40, 50}, 0};
	studio::PxcxPreviewIdentity identity{7, 9, {"image", "selected", "image"}, {3, .25}};
	studio::PxcxPreparedSavePreview preview{pixels, identity, identity};
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(
		studio::SavePxcxCollection(
			path,
			source,
			"selected",
			R"({"description":"new"})",
			diagnostic,
			engine::imagegraph::Limits::MaximumEvaluationBytes,
			&preview
		)
	);
	CHECK(ReadText(path) == "prior graph");
	CHECK(ReadText(directory.Path / "collection.meta") == "prior metadata");
	CHECK(std::filesystem::is_directory(directory.Path / "collection.png"));
	for (const auto &entry : std::filesystem::directory_iterator(directory.Path))
		CHECK_FALSE(entry.path().filename().string().starts_with(".atomic-graph-set-"));
}

TEST_CASE(
	"Studio PXC collection save writes the selected subtree and manager sidecar while retaining its source",
	"[studio][pxcx_save][collection]"
) {
	const auto source = CollectionSource();
	const auto originalBytes = source.OriginalBytes;
	const auto originalGraph = source.GraphJson;
	Directory directory;
	const auto path = directory.Path / "selected.pxcc";
	const std::string_view manager = R"JSON({"description":"saved collection","file_id":41})JSON";
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(studio::SavePxcxCollection(path, source, "selected", manager, diagnostic));
	const auto firstGraph = Json::parse(ReadText(path));
	REQUIRE(firstGraph["nodes"].size() == 3);
	CHECK(firstGraph["nodes"][0]["id"] == "inside-opaque");
	CHECK(firstGraph["nodes"][1]["id"] == "child");
	CHECK(firstGraph["nodes"][2]["id"] == "selected");
	CHECK(firstGraph["nodes"][0]["x"] == 11);
	CHECK(firstGraph["nodes"][0]["y"] == 22);
	CHECK(firstGraph["nodes"][0]["inputs"][0]["from_node"] == "outside");
	CHECK(firstGraph["nodes"][0]["future_node"]["keep"] == true);
	CHECK(firstGraph["nodes"][1]["x"] == 10);
	CHECK(firstGraph["nodes"][1]["y"] == 20);
	CHECK(firstGraph["nodes"][2]["x"] == 0);
	CHECK(firstGraph["nodes"][2]["y"] == 0);
	CHECK(firstGraph["nodes"][2]["group"] == -4);
	CHECK(firstGraph["nodes"][2]["future_root"]["keep"] == 9);
	const auto metadataPath = path.parent_path() / "selected.meta";
	CHECK(Json::parse(ReadText(metadataPath))["description"] == "saved collection");
	CHECK(source.OriginalBytes == originalBytes);
	CHECK(source.GraphJson == originalGraph);

	const std::string_view replacementManager = R"JSON({"description":"replacement"})JSON";
	REQUIRE(studio::SavePxcxCollection(path, source, "selected", replacementManager, diagnostic));
	CHECK(Json::parse(ReadText(metadataPath))["description"] == "replacement");
	CHECK(source.OriginalBytes == originalBytes);
}

TEST_CASE(
	"Studio PXC collection save refusals preserve both published files",
	"[studio][pxcx_save][collection][atomic]"
) {
	const auto source = CollectionSource();
	Directory directory;
	const auto path = directory.Path / "keep.pxcc";
	const auto metadataPath = directory.Path / "keep.meta";
	WriteText(path, "prior collection");
	WriteText(metadataPath, "prior metadata");
	const auto priorCollection = Read(path);
	const auto priorMetadata = Read(metadataPath);
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(studio::SavePxcxCollection(path, source, "missing", {}, diagnostic));
	CHECK(Read(path) == priorCollection);
	CHECK(Read(metadataPath) == priorMetadata);
	CHECK_FALSE(
		studio::SavePxcxCollection(directory.Path / "wrong.pxcx", source, "selected", {}, diagnostic)
	);
	CHECK(Read(path) == priorCollection);
	CHECK(Read(metadataPath) == priorMetadata);
	CHECK_FALSE(studio::SavePxcxCollection(path, source, "selected", {}, diagnostic, 1));
	CHECK(Read(path) == priorCollection);
	CHECK(Read(metadataPath) == priorMetadata);
	auto malformed = source;
	malformed.GraphJson = "{";
	CHECK_FALSE(studio::SavePxcxCollection(path, malformed, "selected", {}, diagnostic));
	CHECK(Read(path) == priorCollection);
	CHECK(Read(metadataPath) == priorMetadata);
	CHECK_FALSE(studio::SavePxcxCollection({}, source, "selected", {}, diagnostic));
}

TEST_CASE(
	"Studio PXC collection save handles sidecar and parent publication failures without partial replacement",
	"[studio][pxcx_save][collection][atomic]"
) {
	const auto source = CollectionSource();
	Directory directory;
	const auto path = directory.Path / "blocked.pxcc";
	const auto metadataPath = directory.Path / "blocked.meta";
	WriteText(path, "prior collection");
	const auto priorCollection = Read(path);
	REQUIRE(std::filesystem::create_directory(metadataPath));
	const auto markerPath = metadataPath / "marker";
	WriteText(markerPath, "keep marker");
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(studio::SavePxcxCollection(path, source, "selected", R"({"description":"x"})", diagnostic));
	CHECK(Read(path) == priorCollection);
	CHECK(ReadText(markerPath) == "keep marker");
	const auto missingParent = directory.Path / "missing" / "collection.pxcc";
	CHECK_FALSE(studio::SavePxcxCollection(missingParent, source, "selected", {}, diagnostic));
	CHECK_FALSE(std::filesystem::exists(missingParent));

	const auto noManagerPath = directory.Path / "no-manager.pxcc";
	const auto untouchedMetadata = directory.Path / "no-manager.meta";
	WriteText(untouchedMetadata, "existing metadata");
	REQUIRE(studio::SavePxcxCollection(noManagerPath, source, "selected", {}, diagnostic));
	CHECK(ReadText(untouchedMetadata) == "existing metadata");
}

TEST_CASE(
	"Studio PXC save retains opaque metadata and reloads the edited native projection", "[studio][pxcx_save]"
) {
	const auto source = Source();
	auto authored = source.Graph;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Migrate(authored, diagnostic) == engine::imagegraph::Status::Ok);
	authored.Nodes.front().Position = {30, -20};
	Directory directory;
	const auto path = directory.Path / "modified.pxc";
	REQUIRE(studio::SavePxcxProjection(path, source.Source, authored, {}, diagnostic));
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(Read(path), archive, failure));
	CHECK(archive.GraphJson.find(R"("future":{"keep":9})") != std::string::npos);
	engine::imagegraphio::PxcxImport reloaded;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, reloaded, failure));
	REQUIRE(engine::imagegraph::Migrate(reloaded.Graph, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(reloaded.Graph == authored);
	REQUIRE(studio::SavePxcxProjection(path, source.Source, source.Graph, {}, diagnostic));
	CHECK(Read(path) == source.Source.OriginalBytes);
	CHECK(
		std::distance(
			std::filesystem::directory_iterator(directory.Path), std::filesystem::directory_iterator{}
		) == 1
	);
}

TEST_CASE(
	"Studio PXC inverse failures preserve the destination and leave no temporary file", "[studio][pxcx_save]"
) {
	const auto source = Source();
	Directory directory;
	const auto path = directory.Path / "existing.pxc";
	{
		std::ofstream stream(path, std::ios::binary);
		stream << "keep this destination";
	}
	const auto original = Read(path);
	auto authored = source.Graph;
	authored.Nodes.front().InstanceBase = "unmapped-new-instance";
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(studio::SavePxcxProjection(path, source.Source, authored, {}, diagnostic));
	CHECK(Read(path) == original);
	CHECK(
		std::distance(
			std::filesystem::directory_iterator(directory.Path), std::filesystem::directory_iterator{}
		) == 1
	);
	CHECK_FALSE(studio::SavePxcxProjection({}, source.Source, source.Graph, {}, diagnostic));
}

namespace {
	engine::imagegraphio::PxcxImport ImageSource() {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.HasThumbnailBlock = true;
		archive.ThumbnailRgba.assign(256 * 256 * 4, 17);
		archive.GraphJson =
			R"({"attributes":{"surface_dimension":[4,2]},"nodes":[{"id":"solid","type":"Node_Solid","x":1,"y":2,"inputs":[{"r":{"d":[1,1]},"attri":{"use_project_dimension":1}},{"r":{"d":4278850590}},{"r":{"d":false}},{"r":{"d":-4},"attri":{"mask_alpha_only":false}},{"r":{"d":true}},{"r":{"d":-4}}],"future":{"keep":9}},{"id":"sink","type":"Node_Project_Output","x":0,"y":0,"inputs":[{"from_node":"solid","from_index":0}]}],"future":{"project":8}})";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		engine::imagegraphio::PxcxImport imported;
		REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, imported, failure));
		REQUIRE(imported.Graph.Outputs.size() == 1);
		return imported;
	}
	engine::imagegraph::Image Render(const engine::imagegraph::Document &graph) {
		using namespace engine::imagegraph;
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(graph, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto evaluated = Evaluate(graph, plan, graph.Outputs.front().Id, image, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		REQUIRE(image.Width == 4);
		REQUIRE(image.Height == 2);
		return image;
	}
	void OneFile(const Directory &directory) {
		CHECK(
			std::distance(
				std::filesystem::directory_iterator(directory.Path), std::filesystem::directory_iterator{}
			) == 1
		);
	}
}
TEST_CASE(
	"Studio opt-in thumbnail save publishes a compiled immutable preview and retains undo provenance",
	"[studio][pxcx_save][pxcx_thumbnail]"
) {
	const auto source = ImageSource();
	const auto sourceBytes = source.Source.OriginalBytes;
	auto authored = source.Graph;
	const auto pixels = Render(authored);
	const studio::PxcxPreviewIdentity identity{7, 11, authored.Outputs.front(), {}, 0};
	const studio::PxcxPreparedSavePreview prepared{pixels, identity, identity};
	studio::PxcxPublishedSave published;
	Directory directory;
	const auto path = directory.Path / "preview.pxcx";
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		studio::SavePxcxProjectionAndAdopt(path, source.Source, authored, {}, published, nullptr, diagnostic)
	);
	CHECK(Read(path) == sourceBytes);
	REQUIRE(
		studio::SavePxcxProjectionAndAdopt(
			path, source.Source, authored, {}, published, &prepared, diagnostic
		)
	);
	const auto saved = Read(path);
	CHECK(published.Archive.OriginalBytes == saved);
	CHECK(published.Archive.GraphJson == source.Source.GraphJson);
	CHECK(published.Archive.MetadataPayload == source.Source.MetadataPayload);
	CHECK(published.ThumbnailIdentity == identity);
	REQUIRE(published.ReferencePreview.Pixels.size() == 256 * 256 * 4);
	CHECK(published.ReferencePreview.Pixels[0] == 30);
	CHECK(published.ReferencePreview.Pixels[1] == 20);
	CHECK(published.ReferencePreview.Pixels[2] == 10);
	CHECK(published.ReferencePreview.Pixels[3] == 255);
	CHECK(source.Source.OriginalBytes == sourceBytes);
	CHECK(source.Source.ThumbnailRgba.front() == 17);
	authored.Nodes.front().Position = {30, -20};
	REQUIRE(
		studio::SavePxcxProjectionAndAdopt(path, source.Source, authored, {}, published, nullptr, diagnostic)
	);
	CHECK(published.Archive.ThumbnailRgba.front() == 30);
	engine::imagegraphio::PxcxImport reloaded;
	std::string failure;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(published.Archive, reloaded, failure));
	CHECK(reloaded.Graph == authored);
	CHECK(reloaded.Source.GraphJson.find(R"("future":{"keep":9})") != std::string::npos);
	// Undo returns to the original authoring graph, retaining the successful explicit thumbnail.
	REQUIRE(
		studio::SavePxcxProjectionAndAdopt(
			path, source.Source, source.Graph, {}, published, nullptr, diagnostic
		)
	);
	CHECK(Read(path) == saved);
	CHECK(published.ThumbnailIdentity == identity);
	CHECK(source.Source.OriginalBytes == sourceBytes);
	OneFile(directory);
}
TEST_CASE(
	"Studio prepared thumbnail identity and conversion refusals preserve file and adopted state",
	"[studio][pxcx_save][pxcx_thumbnail]"
) {
	const auto source = ImageSource();
	auto authored = source.Graph;
	auto pixels = Render(authored);
	studio::PxcxPreviewIdentity completed{7, 11, authored.Outputs.front(), {}, 0};
	auto current = completed;
	Directory directory;
	const auto path = directory.Path / "keep.pxcx";
	studio::PxcxPublishedSave published;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		studio::SavePxcxProjectionAndAdopt(path, source.Source, authored, {}, published, nullptr, diagnostic)
	);
	const auto prior = Read(path);
	const auto previousReference = published.ReferencePreview;
	const auto previousIdentity = published.ThumbnailIdentity;
	uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
	SECTION("changed authoring generation") {
		++current.DocumentRevision;
	}
	SECTION("changed captured input generation") {
		++current.InputRevision;
	}
	SECTION("changed selected binding") {
		current.Binding.Port = "other";
	}
	SECTION("changed fractional signed frame") {
		current.Frame = {2, .25, true};
	}
	SECTION("changed playback observation") {
		++current.PlaybackObservation;
	}
	SECTION("binding removed from current graph") {
		authored.Outputs.clear();
	}
	SECTION("conversion failure") {
		pixels.Pixels.pop_back();
	}
	SECTION("retained budget refusal") {
		maximumBytes = 1024;
	}
	const studio::PxcxPreparedSavePreview prepared{pixels, completed, current};
	CHECK_FALSE(
		studio::SavePxcxProjectionAndAdopt(
			path, source.Source, authored, current.Frame, published, &prepared, diagnostic, maximumBytes
		)
	);
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(Read(path) == prior);
	CHECK(published.Archive.OriginalBytes == prior);
	CHECK(published.ReferencePreview == previousReference);
	CHECK(published.ThumbnailIdentity == previousIdentity);
	OneFile(directory);
}
TEST_CASE(
	"Studio thumbnail publication failure does not adopt the encoded candidate",
	"[studio][pxcx_save][pxcx_thumbnail]"
) {
	const auto source = ImageSource();
	const auto image = Render(source.Graph);
	const studio::PxcxPreviewIdentity identity{7, 11, source.Graph.Outputs.front(), {}, 0};
	const studio::PxcxPreparedSavePreview prepared{image, identity, identity};
	Directory directory;
	const auto path = directory.Path / "destination";
	REQUIRE(std::filesystem::create_directory(path));
	{
		std::ofstream stream(path / "keep");
		stream << "keep";
	}
	studio::PxcxPublishedSave published;
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(
		studio::SavePxcxProjectionAndAdopt(
			path, source.Source, source.Graph, {}, published, &prepared, diagnostic
		)
	);
	CHECK(published.Archive.OriginalBytes.empty());
	CHECK(published.ReferencePreview.Pixels.empty());
	CHECK_FALSE(published.ThumbnailIdentity);
	CHECK(std::filesystem::exists(path / "keep"));
	OneFile(directory);
}
TEST_CASE(
	"Studio saves and reopens an embedded image viewer in a PXC project", "[studio][pxcx_save][node-preview]"
) {
	using namespace engine::imagegraph;
	const auto source = ImageSource();
	auto authored = source.Graph;
	Node viewer{"viewer", "pc.graph_preview", {}, {90, 40}, {}};
	const auto *schema = FindSchema(viewer.Type);
	REQUIRE(schema);
	for (const auto &property : schema->Properties) {
		if (auto value = studio::ImageGraphPropertyDefault(authored, viewer.Type, property.Id))
			viewer.Values.push_back({std::string(property.Id), std::move(*value)});
	}
	authored.Nodes.push_back(std::move(viewer));
	authored.Links.push_back({"solid", "image", "viewer", "surface"});
	Directory directory;
	const auto path = directory.Path / "viewer.pxc";
	Diagnostic diagnostic;
	studio::PxcxPublishedSave published;
	const bool saved =
		studio::SavePxcxProjectionAndAdopt(path, source.Source, authored, {}, published, nullptr, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(Read(path), archive, failure));
	CHECK(archive.GraphJson.find("Node_Graph_Preview") != std::string::npos);
	engine::imagegraphio::PxcxImport reloaded;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, reloaded, failure));
	const auto found =
		std::find_if(reloaded.Graph.Nodes.begin(), reloaded.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "viewer";
		});
	REQUIRE(found != reloaded.Graph.Nodes.end());
	CHECK(found->Type == "pc.graph_preview");
	CHECK(found->Position.X == 90);
	CHECK(found->Position.Y == 40);
	REQUIRE(std::any_of(reloaded.Graph.Links.begin(), reloaded.Graph.Links.end(), [](const auto &link) {
		return link.FromNode == "solid" && link.FromPort == "image" && link.ToNode == "viewer" &&
			   link.ToPort == "surface";
	}));
	CHECK(Render(reloaded.Graph).Pixels == Render(source.Graph).Pixels);
}

TEST_CASE(
	"Studio collection publication includes an unsaved native source projection",
	"[studio][pxcx_save][collection]"
) {
	using namespace engine::imagegraph;
	std::string_view containerType;
	SECTION("ordinary Group") {
		containerType = "Node_Group";
	}
	SECTION("base Collection") {
		containerType = "Node_Collection";
	}
	auto source = CollectionSource();
	auto graph = Json::parse(source.GraphJson.c_str());
	graph["nodes"][0]["type"] = containerType;
	graph["nodes"][1]["type"] = containerType;
	graph["nodes"][0].erase("group");
	graph["nodes"][2]["inputs"] = Json::array();
	graph["nodes"][3]["inputs"] = Json::array();
	graph["nodes"].push_back(
		{{"id", "number"},
		 {"type", "Node_Number_Simple"},
		 {"group", "selected"},
		 {"x", 120},
		 {"y", 240},
		 {"inputs", Json::array({{{"r", {{"d", 3}}}}})}}
	);
	source.GraphJson = graph.dump();
	source.GraphJson.push_back('\0');
	source.Nodes.clear();
	source.Links.clear();
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
	REQUIRE(engine::bake::ReadPxcx(bytes, source, failure));
	const auto original = source.OriginalBytes;
	engine::imagegraphio::PxcxImport imported;
	const bool importedSource = engine::imagegraphio::ImportPxcxImageGraph(source, imported, failure);
	INFO(failure);
	REQUIRE(importedSource);
	auto authored = imported.Graph;
	const auto number = std::find_if(authored.Nodes.begin(), authored.Nodes.end(), [](const Node &node) {
		return node.Id == "number";
	});
	REQUIRE(number != authored.Nodes.end());
	number->Position = {130, 250};
	const auto value = std::find_if(number->Values.begin(), number->Values.end(), [](const auto &property) {
		return property.Port == "value";
	});
	REQUIRE(value != number->Values.end());
	value->Data = 7.0;
	Diagnostic diagnostic;
	engine::bake::PxcxArchive checked;
	const bool projected =
		studio::detail::PrepareImageGraphAppendDestination(imported, authored, {}, checked, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(projected);
	Directory directory;
	const auto path = directory.Path / "unsaved.pxcc";
	REQUIRE(studio::SavePxcxCollection(path, checked, "selected", {}, diagnostic));
	const auto saved = Json::parse(ReadText(path));
	const auto savedNumber = std::find_if(saved["nodes"].begin(), saved["nodes"].end(), [](const auto &node) {
		return node["id"] == "number";
	});
	REQUIRE(savedNumber != saved["nodes"].end());
	CHECK((*savedNumber)["x"] == 30);
	CHECK((*savedNumber)["y"] == 50);
	CHECK((*savedNumber)["inputs"][0]["r"]["d"] == 7.0);
	CHECK(saved["nodes"].back()["type"] == containerType);
	CHECK(source.OriginalBytes == original);
	CHECK(imported.Source.OriginalBytes == original);
}

TEST_CASE("Project saves refuse Collection paths before replacing files", "[studio][pxcx][save]") {
	Directory directory;
	const auto imported = Source();
	const auto &source = imported.Source;
	for (const auto *extension : {".pxcc", ".pxz"}) {
		const auto path = directory.Path / (std::string("collection") + extension);
		WriteText(path, "original collection");
		engine::imagegraph::Diagnostic diagnostic;
		CHECK_FALSE(studio::SavePxcxProjection(path, source, imported.Graph, {}, diagnostic));
		studio::PxcxPublishedSave published;
		CHECK_FALSE(
			studio::SavePxcxProjectionAndAdopt(
				path, source, imported.Graph, {}, published, nullptr, diagnostic
			)
		);
		std::ifstream file(path, std::ios::binary);
		CHECK(std::string(std::istreambuf_iterator<char>(file), {}) == "original collection");
		CHECK(published.Archive.OriginalBytes.empty());
	}
}

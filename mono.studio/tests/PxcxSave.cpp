#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <studio/ImageGraph.hpp>
#include <studio/PxcxSave.hpp>

TEST_SUITE_ID("studio.pxcxsave")
TEST_DEPENDS("engine.imagegraphio.pxcxstructureedit")
TEST_DEPENDS("engine.imagegraphexport.pxcx_thumbnail")

namespace {
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

#include <engine/bake/Image.hpp>
#include <engine/bake/SpriteCache.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

TEST_SUITE_ID("tools.assetc.image_cache_cli")
TEST_DEPENDS("engine.bake.spritecache")
namespace {
	struct Directory {
		std::filesystem::path Path;
		Directory() {
			static std::atomic<uint64_t> sequence = 0;
			Path = std::filesystem::temp_directory_path() /
				   ("assetc-cache-cli-" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
					std::to_string(sequence.fetch_add(1)));
			REQUIRE(std::filesystem::create_directory(Path));
		}
		~Directory() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}
	};
	int Run(const std::vector<std::string> &arguments) {
		engine::parallel::Process child;
		REQUIRE(child.Start(std::filesystem::path(ASSETC_TEST_EXECUTABLE), arguments));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		auto state = child.Poll();
		while (state.Alive() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			state = child.Poll();
		}
		if (state.Alive()) {
			child.Kill();
			state = child.Wait();
		}
		REQUIRE(state.Reason == engine::parallel::ExitReason::Exited);
		return state.Code;
	}
	std::vector<char> Read(const std::filesystem::path &file) {
		REQUIRE(std::filesystem::file_size(file) < 1024 * 1024);
		std::ifstream stream(file, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), {}};
	}
}
TEST_CASE(
	"Assetc plays an explicitly observed saved cache and rejects stale layout observations atomically",
	"[assetc][image_cache][cli]"
) {
	using namespace engine::imagegraph;
	Directory directory;
	constexpr std::string_view cache = R"cache({"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"})cache";
	const auto identity = engine::bake::SpriteCacheDataHash(cache);
	REQUIRE(identity);
	const std::string hash(identity->data(), identity->size());
	Document document;
	document.FormatVersion = 9;
	Node image;
	image.Id = "image:original";
	image.Type = "pc.image";
	image.Values = {{"path", std::string("deleted-and-ungranted.png")}, {"padding", Vector4{}}};
	image.SourceProperties = {{"cache_use", true}, {"cache_data", std::string(cache)}};
	document.Nodes = {image};
	document.Outputs = {{"image", image.Id, "surface_out"}};
	const auto graph = directory.Path / "cache.graph", png = directory.Path / "saved.png";
	const auto write = [&] {
		std::ofstream stream(graph, std::ios::binary | std::ios::trunc);
		const auto text = Write(document);
		REQUIRE_FALSE(text.empty());
		stream << text;
		stream.close();
		REQUIRE(stream.good());
	};
	write();
	const std::vector<std::string> base = {
		"--export-graph", graph.string(), "--graph-output", "image", "--output", png.string()
	};
	const auto render = [&](std::span<const std::string> layouts) {
		auto args = base;
		for (const auto &layout : layouts) {
			args.push_back("--graph-image-cache-layout");
			args.push_back(layout);
		}
		return Run(args);
	};
	CHECK(render({}) == 1);
	CHECK_FALSE(std::filesystem::exists(png));
	const std::string rgba = image.Id + ":" + hash + "=rgba8-top-down";
	REQUIRE(render(std::span(&rgba, 1)) == 0);
	const auto previous = Read(png);
	engine::assets::TextureData pixels;
	std::string failure;
	REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(previous)), pixels, failure));
	CHECK(pixels.Width == 1);
	CHECK(pixels.Height == 1);
	CHECK(
		pixels.Pixels == std::vector<std::byte>{std::byte{12}, std::byte{34}, std::byte{56}, std::byte{78}}
	);
	std::string stale = hash;
	stale[0] = stale[0] == '0' ? '1' : '0';
	const std::string staleLayout = image.Id + ":" + stale + "=rgba8-top-down";
	CHECK(render(std::span(&staleLayout, 1)) == 1);
	CHECK(Read(png) == previous);
	const std::array duplicates{rgba, rgba};
	CHECK(render(duplicates) == 2);
	CHECK(Read(png) == previous);
	const std::string malformed = image.Id + ":" + hash + "=guess";
	CHECK(render(std::span(&malformed, 1)) == 2);
	CHECK(Read(png) == previous);
	const std::string bgra = image.Id + ":" + hash + "=bgra8-top-down";
	REQUIRE(render(std::span(&bgra, 1)) == 0);
	const auto changed = Read(png);
	REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(changed)), pixels, failure));
	CHECK(
		pixels.Pixels == std::vector<std::byte>{std::byte{56}, std::byte{34}, std::byte{12}, std::byte{78}}
	);
	CHECK(changed != previous);
	document.Nodes[0].SourceProperties[1].Data = std::string(cache) + " ";
	write();
	CHECK(render(std::span(&bgra, 1)) == 1);
	CHECK(Read(png) == changed);
	CHECK(
		Run(
			{"--input",
			 directory.Path.string(),
			 "--output",
			 (directory.Path / "rejected").string(),
			 "--graph-image-cache-layout",
			 rgba}
		) == 2
	);
	CHECK_FALSE(std::filesystem::exists(directory.Path / "rejected"));
}

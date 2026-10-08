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

TEST_CASE("Assetc exports selected frames from a saved source frame cache", "[assetc][image_cache][cli]") {
	using namespace engine::imagegraph;
	Directory directory;
	constexpr std::string_view cache =
		R"cache([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null,{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="}])cache";
	const auto identity = engine::bake::SpriteCacheDataHash(cache);
	REQUIRE(identity);
	const std::string hash(identity->data(), identity->size());
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{3, 0, 2, "loop", 24};
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = 2;
	document.Project->SurfaceHeight = 1;
	document.Nodes = {
		{"input",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}},
		{"cache", "pc.cache", "", {}, {{"animated", false}}}
	};
	document.Nodes[1].SourceProperties = {{"serialize", true}, {"cache", std::string(cache)}};
	document.Links = {{"input", "image", "cache", "surface_in"}};
	document.Outputs = {{"image", "cache", "cache_surface"}};
	const auto graph = directory.Path / "source-cache.graph";
	const auto png = directory.Path / "frame.png";
	{
		std::ofstream stream(graph, std::ios::binary | std::ios::trunc);
		const auto text = Write(document);
		REQUIRE_FALSE(text.empty());
		stream << text;
		stream.close();
		REQUIRE(stream.good());
	}
	const auto render = [&](uint64_t tick, std::string_view layout) {
		return Run(
			{"--export-graph",
			 graph.string(),
			 "--graph-output",
			 "image",
			 "--graph-tick",
			 std::to_string(tick),
			 "--output",
			 png.string(),
			 "--graph-image-cache-layout",
			 "cache:" + hash + "=" + std::string(layout)}
		);
	};
	REQUIRE(render(0, "rgba8-top-down") == 0);
	const auto first = Read(png);
	engine::assets::TextureData pixels;
	std::string failure;
	REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(first)), pixels, failure));
	CHECK(pixels.Width == 1);
	CHECK(pixels.Height == 1);
	CHECK(
		pixels.Pixels == std::vector<std::byte>{std::byte{12}, std::byte{34}, std::byte{56}, std::byte{78}}
	);
	REQUIRE(render(2, "rgba8-top-down") == 0);
	const auto second = Read(png);
	REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(second)), pixels, failure));
	CHECK(pixels.Width == 2);
	CHECK(pixels.Height == 1);
	CHECK(
		pixels.Pixels == std::vector<std::byte>{
							 std::byte{200},
							 std::byte{10},
							 std::byte{40},
							 std::byte{0},
							 std::byte{255},
							 std::byte{128},
							 std::byte{64},
							 std::byte{255}
						 }
	);
	std::string stale = hash;
	stale[0] = stale[0] == '0' ? '1' : '0';
	CHECK(
		Run(
			{"--export-graph",
			 graph.string(),
			 "--graph-output",
			 "image",
			 "--graph-tick",
			 "2",
			 "--output",
			 png.string(),
			 "--graph-image-cache-layout",
			 "cache:" + stale + "=rgba8-top-down"}
		) == 1
	);
	CHECK(Read(png) == second);
	REQUIRE(render(0, "bgra8-top-down") == 0);
	const auto bgra = Read(png);
	REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(bgra)), pixels, failure));
	CHECK(
		pixels.Pixels == std::vector<std::byte>{std::byte{56}, std::byte{34}, std::byte{12}, std::byte{78}}
	);
	document.Nodes.push_back(
		{"export",
		 "pc.export",
		 "",
		 {},
		 {{"directory", directory.Path.string()},
		  {"file_name", std::string{"cached"}},
		  {"template", std::string{"%d%n"}},
		  {"type", EnumValue{0}},
		  {"format", EnumValue{0}}}}
	);
	document.Links.push_back({"cache", "cache_surface", "export", "surface"});
	document.Outputs.push_back({"preview", "export", "preview"});
	{
		std::ofstream stream(graph, std::ios::binary | std::ios::trunc);
		const auto text = Write(document);
		REQUIRE_FALSE(text.empty());
		stream << text;
		stream.close();
		REQUIRE(stream.good());
	}
	const auto authoredPng = directory.Path / "cached.png";
	const auto exportAuthored = [&](std::string_view observedHash) {
		return Run(
			{"--export-graph",
			 graph.string(),
			 "--export-node",
			 "export",
			 "--graph-output",
			 "preview",
			 "--output",
			 directory.Path.string(),
			 "--graph-tick",
			 "0",
			 "--graph-image-cache-layout",
			 "cache:" + std::string(observedHash) + "=rgba8-top-down"}
		);
	};
	REQUIRE(exportAuthored(hash) == 0);
	const auto authoredBytes = Read(authoredPng);
	REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(authoredBytes)), pixels, failure));
	CHECK(
		pixels.Pixels == std::vector<std::byte>{std::byte{12}, std::byte{34}, std::byte{56}, std::byte{78}}
	);
	std::string staleAuthoredHash = hash;
	staleAuthoredHash[0] = staleAuthoredHash[0] == '0' ? '1' : '0';
	CHECK(exportAuthored(staleAuthoredHash) == 1);
	CHECK(Read(authoredPng) == authoredBytes);
}

TEST_CASE(
	"Assetc cooks saved frame caches into a standalone native graph artifact",
	"[assetc][source_frame_cache][cli]"
) {
	using namespace engine::imagegraph;
	Directory directory;
	constexpr std::string_view saved =
		R"cache([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null,{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="}])cache";
	const auto identity = engine::bake::SpriteCacheDataHash(saved);
	REQUIRE(identity);
	const std::string hash(identity->data(), identity->size());
	Document source;
	source.FormatVersion = 9;
	source.Timeline = TimelineSettings{3, 0, 2, "loop", 24};
	source.Nodes = {
		{"input",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}},
		{"cache", "pc.cache", "", {}, {{"animated", false}}}
	};
	source.Nodes[1].SourceProperties = {{"serialize", true}, {"cache", std::string(saved)}};
	source.Links = {{"input", "image", "cache", "surface_in"}};
	source.Outputs = {{"image", "cache", "cache_surface"}};
	const auto input = directory.Path / "source.graph", output = directory.Path / "cooked.graph",
			   png = directory.Path / "frame.png";
	const auto sourceText = Write(source);
	{
		std::ofstream stream(input, std::ios::binary);
		stream << sourceText;
	}
	const std::vector<std::string> cook = {
		"--export-graph",
		input.string(),
		"--cook-frame-caches",
		"--output",
		output.string(),
		"--graph-image-cache-layout",
		"cache:" + hash + "=rgba8-top-down"
	};
	REQUIRE(Run(cook) == 0);
	const auto artifact = Read(output);
	Document cooked;
	Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(std::string(artifact.data(), artifact.size()), cooked, diagnostic) ==
		Status::Ok
	);
	CHECK(cooked.Nodes[1].SourceProperties[1].Data == Value{std::string(saved)});
	CHECK(Read(input) == std::vector<char>(sourceText.begin(), sourceText.end()));
	for (const uint64_t tick : {0, 1, 2}) {
		REQUIRE(
			Run(
				{"--export-graph",
				 output.string(),
				 "--graph-output",
				 "image",
				 "--graph-tick",
				 std::to_string(tick),
				 "--output",
				 png.string()}
			) == 0
		);
		const auto bytes = Read(png);
		engine::assets::TextureData pixels;
		std::string failure;
		REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(bytes)), pixels, failure));
		if (tick == 0)
			CHECK(
				pixels.Pixels ==
				std::vector<std::byte>{std::byte{12}, std::byte{34}, std::byte{56}, std::byte{78}}
			);
		if (tick == 1)
			CHECK(
				pixels.Pixels == std::vector<std::byte>{
									 std::byte{1},
									 std::byte{2},
									 std::byte{3},
									 std::byte{255},
									 std::byte{1},
									 std::byte{2},
									 std::byte{3},
									 std::byte{255}
								 }
			);
		if (tick == 2)
			CHECK(
				pixels.Pixels == std::vector<std::byte>{
									 std::byte{200},
									 std::byte{10},
									 std::byte{40},
									 std::byte{0},
									 std::byte{255},
									 std::byte{128},
									 std::byte{64},
									 std::byte{255}
								 }
			);
	}
	auto stale = cook;
	stale.back()[6] = stale.back()[6] == '0' ? '1' : '0';
	CHECK(Run(stale) == 1);
	CHECK(Read(output) == artifact);
	auto conflicting = cook;
	conflicting.insert(conflicting.end(), {"--graph-output", "image"});
	CHECK(Run(conflicting) == 2);
	CHECK(Read(output) == artifact);
	auto same = cook;
	same[4] = input.string();
	CHECK(Run(same) == 1);
	CHECK(Read(input) == std::vector<char>(sourceText.begin(), sourceText.end()));
	auto missing = cook;
	missing.resize(5);
	CHECK(Run(missing) == 2);
	CHECK(Read(output) == artifact);
	std::get<std::string>(cooked.Nodes[1].SourceProperties[1].Data).push_back(' ');
	{
		std::ofstream stream(output, std::ios::binary | std::ios::trunc);
		stream << Write(cooked);
	}
	const auto previousPng = Read(png);
	CHECK(Run({"--export-graph", output.string(), "--graph-output", "image", "--output", png.string()}) == 1);
	CHECK(Read(png) == previousPng);
}

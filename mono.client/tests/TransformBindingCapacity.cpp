#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.transformbindingcapacity")

TEST_CASE(
	"publisher capacity refusal retires inserted runtime entry before cadence cleanup",
	"[client][transform-binding-capacity]"
) {
	using namespace engine;
	scene::RegisterSceneComponents();
	scene::RegisterSceneClasses();
	ecs::Store store("graph-binding-capacity");
	const core::Name owner("graph-binding-capacity-owner"), graph("capacity"), output("image");
	const auto directory = std::filesystem::temp_directory_path() / "pc-transform-binding-capacity65";
	std::filesystem::create_directories(directory / "imagegraphs");
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::filesystem::remove_all(Path);
		}
	} cleanup{directory};
	imagegraph::Document document;
	document.Nodes = {
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", imagegraph::Colour{1, 2, 3, 255}}}}
	};
	document.Outputs = {{"image", "solid", "image"}};
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << imagegraph::Write(document);
	}
	const size_t bindings = render::LiveImagePublisher::MAXIMUM_BINDINGS + 1;
	for (size_t i = 0; i < bindings; ++i) {
		scene::ImageGraphBinding binding;
		binding.Graph = graph;
		binding.Output = output;
		binding.Texture = core::Name("capacity-" + std::to_string(i));
		REQUIRE(scene::SetImageGraphBinding(store, store.Create(), binding));
	}
	render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	bool refused = false;
	const size_t presentations = (bindings + client::ImageGraphRuntime::MAXIMUM_CHECKS_PER_FRAME - 1) /
								 client::ImageGraphRuntime::MAXIMUM_CHECKS_PER_FRAME;
	for (size_t i = 0; i < presentations && !refused; ++i) {
		runtime.BeginFrame();
		CHECK(runtime.Refresh(store, renderer, owner, directory) == 0);
		refused = runtime.LastError() == "live image publisher has no binding capacity";
	}
	INFO(runtime.LastError());
	REQUIRE(refused);
	CHECK(runtime.DocumentParses() == 1);
	// The final inserted entry was erased on refusal. The cadence guard must
	// already be disarmed when it leaves the evaluation scope.
	runtime.Clear(renderer);
}

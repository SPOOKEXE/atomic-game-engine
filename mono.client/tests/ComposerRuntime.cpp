#include "ImageGraphComposerAdapter.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/render/ComposerSurface.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.composer_runtime")
TEST_DEPENDS("engine.render.cookedcomposer")
TEST_DEPENDS("engine.render.composersurface")
namespace {
	void Accepted(std::optional<std::string> error) {
		INFO(error.value_or("accepted"));
		REQUIRE_FALSE(error);
	}
	struct Files {
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() /
			("composer-runtime-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Files() {
			REQUIRE(std::filesystem::create_directory(Root));
			REQUIRE(std::filesystem::create_directory(Root / "imagegraphs"));
		}
		~Files() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	};
}
TEST_CASE(
	"real Composer binding demands missing artifact then loads arrival with exact root and ECS invalidation",
	"[client][composer-runtime]"
) {
	using namespace engine::imagegraph;
	using namespace engine::render::hlsl;
	Files files;
	const engine::core::Name owner("composer.world"), graph("composer"), output("result"),
		texture("composer.material.map");
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(2)}, {"height", int64_t(2)}, {"colour", Colour{255, 255, 255, 255}}}},
		{"shader",
		 "pc.hlsl",
		 "",
		 {},
		 {{"main", std::string("output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv);")},
		  {"vertex", std::string{}},
		  {"global", std::string{}},
		  {"libraries", std::string{}}}}
	};
	document.Links = {{"image", "image", "shader", "base_texture"}};
	document.Outputs = {{"result", "shader", "surface"}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "shader", {}, snapshot, diagnostic) == Status::Ok);
	Document cooked;
	engine::assets::ShaderData artifact;
	Accepted(CookNode(
		document,
		document.Nodes.back(),
		snapshot,
		{},
		"fixture-compiler",
		false,
		engine::core::Name("unpublished"),
		cooked,
		artifact
	));
	engine::core::ByteWriter bytes;
	REQUIRE(engine::assets::Shader::Write(bytes, artifact));
	const engine::core::Name asset(engine::assets::Hasher::Of(bytes.Bytes()).ToHex() + ".ashader");
	cooked.Nodes.back().SourceProperties[0].Data = std::string(asset.Text());

	{
		std::ofstream file(client::ImageGraphDocumentPath(files.Root, graph));
		file << Write(cooked);
		REQUIRE(file.good());
	}
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("composer-bound-world");
	const auto entity = store.Create();
	engine::scene::ImageGraphBinding binding;
	binding.Graph = graph;
	binding.Output = output;
	binding.Texture = texture;
	binding.ColorSpace = engine::scene::ImageGraphColorSpace::Linear;
	REQUIRE(engine::scene::SetImageGraphBinding(store, entity, binding));
	engine::render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	CHECK(runtime.Refresh(store, renderer, owner, files.Root) == 0);
	CHECK_FALSE(runtime.LastError().empty());
	CHECK(renderer.ComposerShaderRevision(owner, asset) == 0);
	std::vector<engine::core::Name> wanted;
	REQUIRE(runtime.CollectWantedComposerShaders(store, owner, files.Root, wanted));
	REQUIRE(wanted.size() == 1);
	CHECK(wanted.front() == asset);
	wanted.clear();
	REQUIRE(runtime.CollectWantedComposerShaders(store, owner, files.Root, wanted));
	CHECK(wanted.empty());
	{
		std::ofstream file(files.Root / std::string(asset.Text()), std::ios::binary);
		const auto data = bytes.Bytes();
		file.write(reinterpret_cast<const char *>(data.data()), data.size());
		REQUIRE(file.good());
	}
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, owner, files.Root) == 1);
	CHECK(runtime.LastError().empty());
	CHECK(renderer.ComposerShaderRevision(owner, asset) == 1);
	CHECK(renderer.ComposerShaderRevision(engine::core::Name("other.world"), asset) == 0);
	CHECK(renderer.SourceOutputStatus(owner, texture, 1) == engine::render::SourceTextureStatus::Pending);
	engine::ecs::Store emptyStore("composer-no-binding");
	std::vector<engine::core::Name> other;
	REQUIRE(
		runtime.CollectWantedComposerShaders(emptyStore, engine::core::Name("other.world"), files.Root, other)
	);
	CHECK(other.empty());
	Files secondRoot;
	auto secondArtifact = artifact;
	secondArtifact.CompilerVersion = "fixture-compiler.other";
	engine::core::ByteWriter secondBytes;
	REQUIRE(engine::assets::Shader::Write(secondBytes, secondArtifact));
	const engine::core::Name secondAsset(
		engine::assets::Hasher::Of(secondBytes.Bytes()).ToHex() + ".ashader"
	);
	auto secondDocument = cooked;
	secondDocument.Nodes.back().SourceProperties[0].Data = std::string(secondAsset.Text());
	{
		std::ofstream file(secondRoot.Root / std::string(secondAsset.Text()), std::ios::binary);
		file.write(reinterpret_cast<const char *>(secondBytes.Bytes().data()), secondBytes.Bytes().size());
		REQUIRE(file.good());
	}
	{
		std::ofstream file(client::ImageGraphDocumentPath(secondRoot.Root, graph));
		file << Write(secondDocument);
		REQUIRE(file.good());
	}
	engine::ecs::Store secondStore("composer-second-directory");
	REQUIRE(engine::scene::SetImageGraphBinding(secondStore, secondStore.Create(), binding));
	const engine::core::Name secondOwner("composer.other-directory");
	CHECK(runtime.Refresh(secondStore, renderer, secondOwner, secondRoot.Root) == 1);
	wanted.clear();
	REQUIRE(runtime.CollectWantedComposerShaders(store, owner, files.Root, wanted));
	REQUIRE(wanted.size() == 1);
	CHECK(wanted.front() == asset);
	wanted.clear();
	REQUIRE(runtime.CollectWantedComposerShaders(secondStore, secondOwner, secondRoot.Root, wanted));
	REQUIRE(wanted.size() == 1);
	CHECK(wanted.front() == secondAsset);
	wanted.clear();
	REQUIRE(runtime.CollectWantedComposerShaders(store, owner, files.Root, wanted));
	CHECK(wanted.empty());
	binding.Seed = 13;
	REQUIRE(engine::scene::SetImageGraphBinding(store, entity, binding));
	wanted.clear();
	REQUIRE(runtime.CollectWantedComposerShaders(store, owner, secondRoot.Root, wanted));
	REQUIRE(wanted.size() == 1);
	CHECK(wanted.front() == secondAsset);
	store.Remove<engine::scene::ImageGraphBinding>(entity);
	wanted.clear();
	REQUIRE(runtime.CollectWantedComposerShaders(store, owner, secondRoot.Root, wanted));
	CHECK(wanted.empty());
	REQUIRE(engine::scene::SetImageGraphBinding(store, entity, binding));
	std::vector<engine::core::Name> full(client::ImageGraphRuntime::MAXIMUM_SINK_REFERENCES + 1);
	CHECK_FALSE(runtime.CollectWantedComposerShaders(store, owner, files.Root, full));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, owner, files.Root) == 1);
	CHECK(runtime.DocumentParses() == 2);
	cooked.Nodes.front().Values.front().Data = int64_t(12);
	{
		std::ofstream file(client::ImageGraphDocumentPath(files.Root, graph));
		file << Write(cooked);
		REQUIRE(file.good());
	}
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, owner, files.Root) == 1);
	CHECK(runtime.DocumentParses() == 3);
	wanted.clear();
	REQUIRE(runtime.CollectWantedComposerShaders(store, owner, files.Root, wanted));
	REQUIRE(wanted.size() == 1);
	CHECK(wanted.front() == asset);
	CHECK_FALSE(renderer.RemoveComposerShader(owner, asset, 2));
	CHECK(renderer.ComposerShaderRevision(owner, asset) == 1);
	CHECK(renderer.RemoveComposerShader(owner, asset, 1));
	CHECK(renderer.SourceOutputStatus(owner, texture, 1) == engine::render::SourceTextureStatus::Absent);
	runtime.Clear(renderer);
}

TEST_CASE(
	"real chained Composer provider reaches upstream cooked stage and refuses missing device explicitly",
	"[client][composer-runtime]"
) {
	using namespace engine::imagegraph;
	using namespace engine::render::hlsl;
	const std::string source = "output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv);";
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(1)}, {"height", int64_t(1)}, {"colour", Colour{255, 255, 255, 255}}}},
		{"first",
		 "pc.hlsl",
		 "",
		 {},
		 {{"main", source},
		  {"vertex", std::string{}},
		  {"global", std::string{}},
		  {"libraries", std::string{}}}},
		{"second",
		 "pc.hlsl",
		 "",
		 {},
		 {{"main", source},
		  {"vertex", std::string{}},
		  {"global", std::string{}},
		  {"libraries", std::string{}}}}
	};
	document.Links = {
		{"image", "image", "first", "base_texture"}, {"first", "surface", "second", "base_texture"}
	};
	document.Outputs = {{"result", "second", "surface"}};
	engine::assets::ShaderData shader;
	Accepted(CookArtifact({"", source, "", "", {}}, {}, "fixture-compiler", false, shader));
	const engine::core::Name asset("chain.fixture.shader"), owner("chain.fixture.owner");
	for (size_t i = 1; i < document.Nodes.size(); ++i)
		document.Nodes[i].SourceProperties.push_back(
			{std::string(COOKED_SELECTOR), std::string(asset.Text())}
		);
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::render::Renderer renderer;
	Accepted(renderer.InstallComposerShader(owner, asset, shader));
	client::detail::ComposerProvider provider(renderer, owner, nullptr);
	EvaluationSnapshot snapshot;
	EvaluationRequest request{.HostProvider = &provider};
	CHECK(
		EvaluateNodeInputs(document, plan, "second", request, snapshot, diagnostic) ==
		Status::UnsupportedExecution
	);
	INFO(diagnostic.Message);
	CHECK(diagnostic.NodeId == "first");
	CHECK(diagnostic.Message.find("idle renderer device") != std::string::npos);
	HostNodeCapture retained;
	retained.Failure = "last good marker";
	const std::vector<AuthoredValue> inputs{
		{"main", source}, {"vertex", std::string{}}, {"global", std::string{}}, {"libraries", std::string{}}
	};
	Image base{1, 1, {0, 0, 0, 255}, 0};
	const std::array<HostResolvedImage, 1> images{{{"base_texture", &base}}};
	HostNodeInvocation invocation{document.Nodes[1], request, inputs, images, 1};
	std::string failure;
	CHECK_FALSE(renderer.CaptureComposerSurface(invocation, owner, retained, failure));
	CHECK(retained.Failure == "last good marker");
	CHECK(failure.find("budget") != std::string::npos);
	bool pending = true;
	const engine::core::Name captureName("studio.image-composer.first");
	CHECK_FALSE(
		renderer.CaptureComposerSurfaceAsync(invocation, owner, captureName, retained, failure, &pending)
	);
	CHECK_FALSE(pending);
	CHECK(retained.Failure == "last good marker");
	CHECK(failure.find("renderer device") != std::string::npos);
	renderer.CancelComposerCapture(owner, captureName);
	CHECK(renderer.ComposerShaderRevision(owner, asset) == 1);
	renderer.ForgetWorld(0, owner);
	CHECK(renderer.ComposerShaderRevision(owner, asset) == 0);
}

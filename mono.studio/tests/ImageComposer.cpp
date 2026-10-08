#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <nodegraph/Layout.hpp>
#include <nodegraph/Serialize.hpp>
#include <studio/ImageComposer.hpp>

TEST_SUITE_ID("studio.imagecomposer")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraph.evaluate")
TEST_DEPENDS("engine.bake.imagegraph")

namespace {
	using namespace engine::imagegraph;
	struct Directory {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("studio-imagecomposer-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Directory() {
			REQUIRE(std::filesystem::create_directory(Path));
		}
		~Directory() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}
	};
	struct Context {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Owned = ImGui::CreateContext();
		Context() {
			ImGui::SetCurrentContext(Owned);
			auto &io = ImGui::GetIO();
			io.IniFilename = io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.DisplaySize = {1200, 900};
			io.DeltaTime = 1.f / 60;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Context() {
			ImGui::SetCurrentContext(Owned);
			if (Owned->WithinFrameScope) ImGui::EndFrame();
			ImGui::DestroyContext(Owned);
			ImGui::SetCurrentContext(Previous);
		}
		void Frame(studio::ImageComposerState &state, const studio::ImageComposerHost &host) {
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({1150, 850}, ImGuiCond_Always);
			bool open = true;
			studio::DrawImageComposer(state, open, host);
			ImGui::Render();
		}
	};
	void Number(nodegraph::Graph &graph, nodegraph::NodeId id, const char *key, double number) {
		nodegraph::Value value;
		value.Kind = nodegraph::WidgetKind::Number;
		value.Number = number;
		nodegraph::SetValue(graph, id, key, value);
	}
	Document Fixture() {
		Document document;
		document.Nodes = {
			{"source", Source{"source.png"}, {}, {12, 17}},
			{"solid", Solid{2, 2, {18, 34, 52, 255}}, {}, {30, 40}},
			{"resize", Resize{2, 2, Sampling::Bilinear}, {"source"}, {50, 60}},
			{"crop", Crop{-1, 0, 2, 2}, {"resize"}, {70, 80}},
			{"transform", Transform{2, 2, 1, 0, -1, 1, 90, 1, 1, Sampling::Bilinear}, {"crop"}, {90, 100}},
			{"flip", Flip{true, false}, {"transform"}, {110, 120}},
			{"blend", Blend{.5}, {"solid", "flip"}, {130, 140}}
		};
		document.Outputs = {{"result", "blend"}, {"plain", "solid"}};
		return document;
	}
}

TEST_CASE("composer canvas roundtrips every 2D operation and named output", "[studio][imagecomposer]") {
	const auto document = Fixture();
	nodegraph::Graph graph;
	Diagnostic diagnostic;
	REQUIRE(studio::LoadImageComposerGraph(document, graph, diagnostic));
	Document restored;
	REQUIRE(studio::SaveImageComposerGraph(graph, document.Outputs, restored, diagnostic));
	std::string before, after;
	REQUIRE(Write(document, before, diagnostic));
	REQUIRE(Write(restored, after, diagnostic));
	CHECK(after == before);
	const auto held = nodegraph::Save(graph);
	auto invalid = document;
	invalid.Nodes.back().Inputs[1] = "absent";
	CHECK_FALSE(studio::LoadImageComposerGraph(invalid, graph, diagnostic));
	CHECK(nodegraph::Save(graph) == held);
	graph.Nodes().front().Type = "image.unsupported";
	CHECK_FALSE(studio::SaveImageComposerGraph(graph, document.Outputs, restored, diagnostic));
	CHECK_FALSE(diagnostic.Message.empty());
	std::string unchanged;
	REQUIRE(Write(restored, unchanged, diagnostic));
	CHECK(unchanged == before);
}

TEST_CASE(
	"composer copied identities survive original deletion, history and project reopen",
	"[studio][imagecomposer]"
) {
	Directory directory;
	studio::ImageComposerState state;
	state.SourceRoot = directory.Path;
	studio::InitialiseImageComposer(state);
	const auto originalId = state.Graph.Nodes().front().Id;
	const auto original = state.Graph;
	const auto copied = state.Graph.Absorb(original, 20, 20);
	REQUIRE(copied.size() == 1);
	Document document;
	Diagnostic diagnostic;
	// A const adapter cannot repair mutable canvas state behind its caller's back.
	CHECK_FALSE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	REQUIRE(studio::SetImageComposerOutput(state, "copy", copied.front()));
	REQUIRE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	REQUIRE(document.Nodes.size() == 2);
	const auto copyName = document.Nodes.back().Id;
	CHECK(document.Nodes.front().Id != copyName);
	CHECK(document.Outputs.back().Node == copyName);
	REQUIRE(state.Graph.Remove(originalId));
	studio::CommitImageComposer(state);
	CHECK(state.Graph.Find(copied.front())->Widgets.at("__composer.id").Text == copyName);
	CHECK(state.Outputs.front().Node == "solid");
	CHECK(state.Outputs.back().Node == copyName);
	CHECK_FALSE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	REQUIRE(studio::SetImageComposerOutput(state, "image", copied.front()));
	REQUIRE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	CHECK(document.Nodes.front().Id == copyName);
	REQUIRE(studio::UndoImageComposer(state));
	CHECK_FALSE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	REQUIRE(studio::UndoImageComposer(state));
	REQUIRE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	CHECK(document.Nodes.back().Id == copyName);
	REQUIRE(studio::RedoImageComposer(state));
	CHECK_FALSE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	REQUIRE(studio::RedoImageComposer(state));
	REQUIRE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
	CHECK(document.Nodes.front().Id == copyName);
	const auto project = directory.Path / "copies.imagegraph";
	REQUIRE(studio::SaveImageComposerProject(state, project));
	studio::ImageComposerState reopened;
	REQUIRE(studio::OpenImageComposerProject(reopened, project));
	REQUIRE(studio::SaveImageComposerGraph(reopened.Graph, reopened.Outputs, document, diagnostic));
	CHECK(document.Nodes.front().Id == copyName);
	CHECK(document.Outputs.front().Node == copyName);
	CHECK(document.Outputs.back().Node == copyName);
	const auto added = state.Graph.Add("image.solid", 50, 50);
	const auto heldOutput = "node-" + std::to_string(added);
	state.Outputs.push_back({"deleted", heldOutput});
	studio::CommitImageComposer(state);
	CHECK(state.Graph.Find(added)->Widgets.at("__composer.id").Text != heldOutput);
	CHECK_FALSE(studio::SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic));
}

TEST_CASE(
	"composer real frames reuse pixels across layout edits and track undo and output selection",
	"[studio][imagecomposer]"
) {
	Context context;
	studio::ImageComposerState state;
	studio::ImageComposerHost host;
	size_t uploads = 0;
	host.Upload = [&](const engine::assets::TextureData &, void *&handle, std::string &) {
		++uploads;
		handle = nullptr;
		return true;
	};
	context.Frame(state, host);
	REQUIRE(state.Preview.IsValid());
	CHECK(uploads == 1);
	context.Frame(state, host);
	CHECK(uploads == 1);
	const auto solid = state.Graph.Nodes().front().Id;
	state.Graph.Find(solid)->X += 120;
	studio::CommitImageComposer(state);
	context.Frame(state, host);
	CHECK(uploads == 1);
	state.Canvas.Select(solid);
	Number(state.Graph, solid, "width", 2);
	Number(state.Graph, solid, "height", 1);
	studio::CommitImageComposer(state);
	context.Frame(state, host);
	CHECK(state.Preview.Width == 2);
	CHECK(state.Preview.Height == 1);
	CHECK(uploads == 2);
	REQUIRE(studio::UndoImageComposer(state));
	context.Frame(state, host);
	CHECK(state.Preview.Width == 256);
	REQUIRE(studio::RedoImageComposer(state));
	context.Frame(state, host);
	CHECK(state.Preview.Width == 2);
	const auto flip = state.Graph.Add("image.flip", 400, 40);
	REQUIRE(state.Graph.Connect(solid, "Image", flip, "Image") == nodegraph::LinkResult::Made);
	REQUIRE(studio::SetImageComposerOutput(state, "flipped", flip));
	context.Frame(state, host);
	CHECK(state.SelectedOutput == "flipped");
	CHECK(state.Error.empty());
	const auto accepted = state.Preview.Pixels;
	Number(state.Graph, solid, "width", 0);
	studio::CommitImageComposer(state);
	context.Frame(state, host);
	CHECK_FALSE(state.Error.empty());
	CHECK(state.Preview.Pixels == accepted);
	const auto count = uploads;
	context.Frame(state, host);
	CHECK(uploads == count);
	const auto *window = ImGui::FindWindowByName("Image Composer");
	REQUIRE(window != nullptr);
	CHECK(window->WasActive);
}

TEST_CASE(
	"composer palette respects the host vocabulary during keyboard insertion", "[studio][imagecomposer]"
) {
	Context context;
	studio::ImageComposerState state;
	studio::ImageComposerHost host;
	context.Frame(state, host);
	state.Canvas.AcceptsType = [](const nodegraph::NodeType &type) { return type.Id == "image.flip"; };
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(300, 650);
	context.Frame(state, host);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	context.Frame(state, host);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	context.Frame(state, host);
	io.AddKeyEvent(ImGuiKey_Tab, true);
	context.Frame(state, host);
	io.AddKeyEvent(ImGuiKey_Tab, false);
	context.Frame(state, host);
	// The popup first measures its child list, then settles above the mouse near the window bottom.
	context.Frame(state, host);
	REQUIRE_FALSE(context.Owned->OpenPopupStack.empty());
	const auto *popup = context.Owned->OpenPopupStack.back().Window;
	REQUIRE(popup != nullptr);
	io.AddMousePosEvent(popup->Pos.x + 30, popup->Pos.y + 15);
	context.Frame(state, host);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	context.Frame(state, host);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	context.Frame(state, host);
	io.AddKeyEvent(ImGuiKey_Enter, true);
	context.Frame(state, host);
	io.AddKeyEvent(ImGuiKey_Enter, false);
	context.Frame(state, host);
	REQUIRE(state.Graph.Nodes().size() == 2);
	CHECK(state.Graph.Nodes().back().Type == "image.flip");
}

TEST_CASE("composer cache walks a deeply shared graph once", "[studio][imagecomposer]") {
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	const auto solid = state.Graph.Nodes().front().Id;
	Number(state.Graph, solid, "width", 1);
	Number(state.Graph, solid, "height", 1);
	auto prior = solid;
	for (int i = 0; i < 48; ++i) {
		const auto blend = state.Graph.Add("image.blend", 40, 40);
		REQUIRE(state.Graph.Connect(prior, "Image", blend, "Background") == nodegraph::LinkResult::Made);
		REQUIRE(state.Graph.Connect(prior, "Image", blend, "Foreground") == nodegraph::LinkResult::Made);
		prior = blend;
	}
	REQUIRE(studio::SetImageComposerOutput(state, "image", prior));
	studio::ImageComposerHost host;
	size_t uploads = 0;
	host.Upload = [&](const engine::assets::TextureData &, void *&, std::string &) {
		++uploads;
		return true;
	};
	REQUIRE(studio::RefreshImageComposer(state, host));
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(uploads == 1);
	CHECK(state.Preview.Width == 1);
}

TEST_CASE("composer source edits decode once until explicit reload", "[studio][imagecomposer]") {
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	Document document;
	document.Nodes = {{"source", Source{"source.png"}, {}, {0, 0}}};
	document.Outputs = {{"image", "source"}};
	Diagnostic diagnostic;
	REQUIRE(studio::LoadImageComposerGraph(document, state.Graph, diagnostic));
	state.Outputs = document.Outputs;
	size_t reads = 0, uploads = 0;
	studio::ImageComposerHost host;
	host.Sources = [&](std::string_view, Image &image, std::string &) {
		++reads;
		image = {1, 1, {std::byte{7}, std::byte{8}, std::byte{9}, std::byte{255}}};
		return true;
	};
	host.Upload = [&](const engine::assets::TextureData &, void *&, std::string &) {
		++uploads;
		return true;
	};
	REQUIRE(studio::RefreshImageComposer(state, host));
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(reads == 1);
	CHECK(uploads == 1);
	state.Graph.Nodes().front().Y += 40;
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(uploads == 1);
	studio::ReloadImageComposerSources(state);
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(reads == 2);
	CHECK(uploads == 2);
	const auto accepted = state.Preview.Pixels;
	host.Sources = [](std::string_view, Image &, std::string &failure) {
		failure = "source unavailable";
		return false;
	};
	studio::ReloadImageComposerSources(state);
	CHECK_FALSE(studio::RefreshImageComposer(state, host));
	CHECK(state.Preview.Pixels == accepted);
	CHECK(state.Error == "source: source unavailable");
}

TEST_CASE(
	"composer save reopen and export preserve accepted data after refusals", "[studio][imagecomposer]"
) {
	Directory directory;
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	state.SourceRoot = directory.Path;
	const auto solid = state.Graph.Nodes().front().Id;
	Number(state.Graph, solid, "width", 2);
	Number(state.Graph, solid, "height", 1);
	studio::ImageComposerHost host;
	host.BakedRoot = directory.Path / "baked";
	size_t registrations = 0;
	host.Register = [&](std::span<const std::byte> bytes, const std::string &name) {
		++registrations;
		CHECK(name == "made.atex");
		engine::core::ByteReader reader(bytes);
		engine::assets::TextureData texture;
		REQUIRE(engine::assets::Texture::Read(reader, texture));
		CHECK(texture.Width == 2);
		CHECK(texture.Height == 1);
		CHECK(texture.Pixels[3] == std::byte{255});
	};
	REQUIRE(studio::RefreshImageComposer(state, host));
	const auto project = directory.Path / "made.imagegraph";
	REQUIRE(studio::SaveImageComposerProject(state, project));
	studio::ImageComposerState reopened;
	REQUIRE(studio::OpenImageComposerProject(reopened, project));
	REQUIRE(studio::RefreshImageComposer(reopened, host));
	CHECK(reopened.Preview.Pixels == state.Preview.Pixels);
	REQUIRE(studio::ExportImageComposer(state, host, "made"));
	CHECK(registrations == 1);
	const auto canvas = nodegraph::Save(state.Graph);
	const auto image = state.Preview.Pixels;
	const auto broken = directory.Path / "broken.imagegraph";
	{
		std::ofstream file(broken);
		file << "{\"version\":1,\"nodes\":[]}\n";
	}
	CHECK_FALSE(studio::OpenImageComposerProject(state, broken));
	CHECK(nodegraph::Save(state.Graph) == canvas);
	CHECK(state.Preview.Pixels == image);
	// A file-operation diagnostic must not poison a valid cached evaluation.
	REQUIRE(studio::ExportImageComposer(state, host, "made"));
	CHECK(registrations == 2);
	CHECK_FALSE(studio::ExportImageComposer(state, host, "../escape"));
	CHECK(registrations == 2);
	Number(state.Graph, state.Graph.Nodes().front().Id, "width", 0);
	CHECK_FALSE(studio::SaveImageComposerProject(state, project));
	CHECK_FALSE(studio::ExportImageComposer(state, host, "made"));
	CHECK(registrations == 2);
	studio::ImageComposerState prior;
	REQUIRE(studio::OpenImageComposerProject(prior, project));
	REQUIRE(studio::RefreshImageComposer(prior, host));
	CHECK(prior.Preview.Width == 2);
}

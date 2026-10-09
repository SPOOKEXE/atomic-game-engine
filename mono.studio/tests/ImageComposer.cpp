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

TEST_CASE("composer named controls roundtrip defaults bindings and undo", "[studio][imagecomposer]") {
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	const auto solid = state.Graph.Nodes().front().Id;
	Diagnostic diagnostic;
	REQUIRE(studio::BindImageComposerInput(state, solid, "width", "size", diagnostic));
	REQUIRE(studio::BindImageComposerInput(state, solid, "height", "size", diagnostic));
	REQUIRE(studio::BindImageComposerInput(state, solid, "colour", "tint", diagnostic));
	const auto bindings = state.Bindings.size();
	CHECK_FALSE(studio::BindImageComposerInput(state, solid, "colour", "size", diagnostic));
	CHECK(state.Bindings.size() == bindings);
	CHECK_FALSE(studio::BindImageComposerInput(state, solid, "width", "bad/name", diagnostic));
	Document document;
	REQUIRE(studio::SaveImageComposerDocument(state, document, diagnostic));
	CHECK(document.Parameters.size() == 2);
	CHECK(document.Bindings.size() == 3);
	state.Parameters[0].Default = 2.;
	state.Parameters[1].Default = std::array<uint8_t, 4>{2, 30, 100, 255};
	studio::CommitImageComposer(state);
	studio::ImageComposerHost host;
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(state.Preview.Width == 2);
	CHECK(state.Preview.Pixels.front() == std::byte{2});
	REQUIRE(studio::UndoImageComposer(state));
	CHECK(std::get<double>(state.Parameters[0].Default) == 256.);
	REQUIRE(studio::RedoImageComposer(state));
	CHECK(std::get<double>(state.Parameters[0].Default) == 2.);
	Directory directory;
	REQUIRE(studio::SaveImageComposerProject(state, directory.Path / "named.imagegraph"));
	studio::ImageComposerState reopened;
	REQUIRE(studio::OpenImageComposerProject(reopened, directory.Path / "named.imagegraph"));
	REQUIRE(studio::SaveImageComposerDocument(reopened, document, diagnostic));
	CHECK(document.Parameters.size() == 2);
	CHECK(document.Bindings.size() == 3);
	REQUIRE(studio::RefreshImageComposer(reopened, host));
	CHECK(reopened.Preview.Pixels == state.Preview.Pixels);
	Context context;
	context.Frame(reopened, host);
}

TEST_CASE(
	"composer GPU adapter caches edits and retains last good output without CPU pixels",
	"[studio][imagecomposer]"
) {
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	state.Graph.Nodes().front().Widgets["colour"].Tint = {1.f, 64.f / 255.f, 0.f, 1.f};
	Directory directory;
	studio::ImageComposerHost host;
	host.BakedRoot = directory.Path;
	size_t calls = 0, uploads = 0;
	bool refuse = false;
	host.Gpu = [&](const Document &document,
				   std::string_view output,
				   const TypedSourceResolver &,
				   studio::ImageComposerPreview &preview,
				   Diagnostic &diagnostic) {
		++calls;
		CHECK(output == "image");
		CHECK(document.Parameters.empty());
		CHECK(document.Outputs.front().Space == OutputSpace::Linear);
		CHECK(
			std::get<Solid>(document.Nodes.front().Value).Colour == (std::array<uint8_t, 4>{255, 64, 0, 255})
		);
		if (refuse) {
			diagnostic = {{}, "GPU budget refused"};
			return false;
		}
		preview = {reinterpret_cast<void *>(uintptr_t{7}), 16, 16};
		return true;
	};
	host.Upload = [&](const engine::assets::TextureData &, void *&, std::string &) {
		++uploads;
		return true;
	};
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK_FALSE(state.Preview.IsValid());
	CHECK(state.PreviewTexture.Pixels.empty());
	CHECK(state.PreviewWidth == 16);
	Document authored;
	Diagnostic authoredDiagnostic;
	REQUIRE(studio::SaveImageComposerDocument(state, authored, authoredDiagnostic));
	CHECK(authored.Outputs.front().Space == OutputSpace::SRGB);
	CHECK(uploads == 0);
	const auto handle = state.PreviewHandle;
	state.Graph.Nodes().front().X += 33;
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(calls == 1);
	REQUIRE(studio::ExportImageComposer(state, host, "static.atex"));
	CHECK(state.PreviewTexture.Width == 256);
	CHECK(state.PreviewTexture.Pixels[1] == std::byte{64});
	CHECK(state.PreviewTexture.Format == engine::assets::TextureFormat::RGBA8);
	CHECK(state.PreviewHandle == handle);
	CHECK(calls == 1);
	refuse = true;
	Number(state.Graph, state.Graph.Nodes().front().Id, "width", 3);
	CHECK_FALSE(studio::RefreshImageComposer(state, host));
	CHECK(state.PreviewHandle == handle);
	CHECK(state.PreviewWidth == 16);
	CHECK_FALSE(studio::ExportImageComposer(state, host, "refused.atex"));
	CHECK_FALSE(std::filesystem::exists(directory.Path / "refused.atex"));
	CHECK(calls == 2);
}

TEST_CASE(
	"composer typed GPU sources distinguish colour data and explicit reload", "[studio][imagecomposer]"
) {
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	Document document;
	document.Nodes = {
		{"colour", Source{"same.png", SourceInterpretation::Colour}, {}, {}},
		{"data", Source{"same.png", SourceInterpretation::Data}, {}, {}}
	};
	document.Outputs = {{"image", "colour"}, {"data", "data", OutputSpace::Linear}};
	Diagnostic diagnostic;
	REQUIRE(studio::LoadImageComposerGraph(document, state.Graph, diagnostic));
	state.Outputs = document.Outputs;
	size_t decodes = 0;
	studio::ImageComposerHost host;
	host.TypedSources = [&](const Source &source, Image &image, std::string &) {
		++decodes;
		image = {
			1,
			1,
			{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}},
			source.Interpretation == SourceInterpretation::Colour ? OutputSpace::SRGB : OutputSpace::Linear
		};
		return true;
	};
	host.Gpu = [&](const Document &resolved,
				   std::string_view output,
				   const TypedSourceResolver &sources,
				   studio::ImageComposerPreview &preview,
				   Diagnostic &) {
		Image image;
		std::string failure;
		const auto &source = std::get<Source>(resolved.Nodes[output == "image" ? 0 : 1].Value);
		REQUIRE(sources(source, image, failure));
		CHECK(image.Space == (output == "image" ? OutputSpace::SRGB : OutputSpace::Linear));
		preview = {nullptr, 1, 1};
		return true;
	};
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(decodes == 1);
	state.SelectedOutput = "data";
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(decodes == 2);
	CHECK(state.SourceVersions.size() == 2);
	state.SelectedOutput = "image";
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(decodes == 2);
	studio::ReloadImageComposerSources(state);
	REQUIRE(studio::RefreshImageComposer(state, host));
	CHECK(decodes == 3);
}

TEST_CASE(
	"live composer publication commits accepted graph identity only after host success",
	"[studio][imagecomposer]"
) {
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	studio::ImageComposerHost host;
	host.LivePublish = [](const Document &, std::string_view name, std::string &accepted, std::string &) {
		accepted = name;
		return true;
	};
	REQUIRE(studio::PublishLiveImageComposer(state, host, "published.aimagegraph"));
	state.TargetProperty = "Image";
	CHECK(studio::CanApplyLiveImageComposer(state));
	const auto key = state.PublishedKey;
	host.LivePublish = [](const Document &, std::string_view, std::string &, std::string &failure) {
		failure = "signature refused";
		return false;
	};
	CHECK_FALSE(studio::PublishLiveImageComposer(state, host, "replacement.aimagegraph"));
	CHECK(state.PublishedGraph == "published.aimagegraph");
	CHECK(state.PublishedKey == key);
	CHECK(studio::CanApplyLiveImageComposer(state));
	studio::ReloadImageComposerSources(state);
	CHECK_FALSE(studio::CanApplyLiveImageComposer(state));
	host.LivePublish = [](const Document &, std::string_view name, std::string &accepted, std::string &) {
		accepted = name;
		return true;
	};
	REQUIRE(studio::PublishLiveImageComposer(state, host, "published.aimagegraph"));
	CHECK(state.PublishedKey != key);
}

TEST_CASE("source and toggle controls preserve binding defaults across Save As", "[studio][imagecomposer]") {
	Directory directory;
	const auto nested = directory.Path / "nested";
	REQUIRE(std::filesystem::create_directory(nested));
	std::ofstream(directory.Path / "input.png") << "path rebase fixture";
	studio::ImageComposerState state;
	studio::InitialiseImageComposer(state);
	state.SourceRoot = directory.Path;
	Document document;
	document.Nodes = {
		{"source", Source{"input.png", SourceInterpretation::Data}, {}, {}},
		{"flip", Flip{true, false}, {"source"}, {}}
	};
	document.Outputs = {{"image", "flip", OutputSpace::Linear}};
	Diagnostic diagnostic;
	REQUIRE(studio::LoadImageComposerGraph(document, state.Graph, diagnostic));
	state.Outputs = document.Outputs;
	const auto source = state.Graph.Nodes()[0].Id;
	const auto flip = state.Graph.Nodes()[1].Id;
	REQUIRE(studio::BindImageComposerInput(state, source, "path", "source", diagnostic));
	REQUIRE(studio::BindImageComposerInput(state, flip, "horizontal", "reverse", diagnostic));
	CHECK(std::get<bool>(state.Parameters[1].Default));
	CHECK_FALSE(studio::SaveImageComposerProject(state, nested / "escape.imagegraph"));
	CHECK(std::get<std::string>(state.Parameters[0].Default) == "input.png");
	state.SourceRoot = nested;
	nodegraph::Value sourcePath;
	sourcePath.Kind = nodegraph::WidgetKind::Text;
	sourcePath.Text = "sub/input.png";
	nodegraph::SetValue(state.Graph, source, "path", sourcePath);
	state.Parameters[0].Default = std::string("sub/input.png");
	REQUIRE(studio::SaveImageComposerProject(state, directory.Path / "rebased.imagegraph"));
	CHECK(std::get<std::string>(state.Parameters[0].Default) == "nested/sub/input.png");
	studio::ImageComposerState reopened;
	REQUIRE(studio::OpenImageComposerProject(reopened, directory.Path / "rebased.imagegraph"));
	REQUIRE(studio::SaveImageComposerDocument(reopened, document, diagnostic));
	CHECK(std::get<Source>(document.Nodes[0].Value).Interpretation == SourceInterpretation::Data);
	CHECK(document.Outputs[0].Space == OutputSpace::Linear);
	CHECK(std::get<std::string>(document.Parameters[0].Default) == "nested/sub/input.png");
	CHECK(std::get<bool>(document.Parameters[1].Default));
}

#include "../src/TimelineKeyDelete.hpp"

#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/ImageGraphSourceKeyEdit.hpp"

#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui_internal.h>

TEST_SUITE_ID("studio.timeline_key_delete")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	Document Keys(bool retired, bool captured = true) {
		Document document;
		document.FormatVersion = captured ? 10 : 9;
		document.Nodes = {
			{"owner", "pc.invert", {}, {}, {{"mix", .9}}}, {"alias", "pc.invert", {}, {}, {{"mix", .25}}}
		};
		document.Nodes[1].InstanceBase = "owner";
		for (auto &node : document.Nodes)
			node.SourceAnimatedInputs = {"mix"};
		Keyframe canonical{"owner", retired ? "native:animator:0" : "mix", 2, .25, "source", KeyframeEase{}};
		canonical.Subframe = .5;
		canonical.NegativeFrame = true;
		canonical.SourceKeyId = "original";
		auto alias = canonical;
		alias.NodeId = "alias";
		alias.Port = "mix";
		alias.SourceKeyId.clear();
		document.Keyframes = {
			retired ? Keyframe{"owner", "mix", 0, .9, "source", KeyframeEase{}} : canonical, alias
		};
		document.Tracks = {{"owner", "mix", "hold", -1}, {"alias", "mix", "hold", -1}};
		document.Outputs = {{"result", "alias", "surface_out"}};
		if (captured) {
			document.SourceAnimators.emplace();
			GroupSubtypeBinding binding{
				"alias", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"
			};
			if (retired) binding.AnimatorPort = canonical.Port;
			document.SourceAnimators->Bindings = {binding};
			if (retired) {
				DetachedSourceAnimator metadata;
				metadata.OwnerId = "owner";
				metadata.Id = canonical.Port;
				metadata.OriginalPort = "mix";
				metadata.Type = ValueType::Scalar;
				metadata.Writer = GroupSubtypeAnimator::Animated;
				metadata.Track = AnimationTrack{"owner", canonical.Port, "hold", -1};
				document.SourceAnimators->Detached = {metadata};
				document.SourceAnimators->DetachedValues = {
					{"owner", std::nullopt, {canonical}, canonical.Port}
				};
			}
		}
		return document;
	}
	struct Button {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Graph;
		studio::ImageGraphHistory History;
		studio::ImageGraphGroupHost Host;
		bool SourceEnvelope = false;
		Diagnostic Error;
		ImVec2 Center;
		unsigned Calls = 0, Accepted = 0;
		Button(Document document, size_t historyCapacity = 128, bool sourceEnvelope = false)
			: Graph(std::move(document)), History(historyCapacity), SourceEnvelope(sourceEnvelope) {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {640, 480};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Button() {
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		Button(const Button &) = delete;
		Button &operator=(const Button &) = delete;
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({300, 180});
			ImGui::Begin("Timeline delete", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			if (studio::detail::DrawTimelineKeyDelete(Graph, 1, Error, [&](const auto &edit) {
					++Calls;
					if (SourceEnvelope)
						return studio::ApplyImageGraphSourceKeyEdit(Graph, History, Host, 1, {}, edit, Error);
					return studio::ApplyImageGraphDocumentEdit(Graph, History, edit);
				}))
				++Accepted;
			const auto low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
			Center = {(low.x + high.x) * .5f, (low.y + high.y) * .5f};
			ImGui::End();
			ImGui::Render();
		}
		void Click() {
			Frame();
			Frame();
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(Center.x, Center.y);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Frame();
		}
	};
}
TEST_CASE("Timeline table Delete removes captured shared keys through one undo entry", "[studio][timeline]") {
	for (bool retired : {false, true}) {
		Button button(Keys(retired));
		const auto before = button.Graph;
		button.Click();
		CHECK(button.Calls == 1);
		REQUIRE(button.Accepted == 1);
		CHECK(button.Graph.Keyframes.size() == size_t(retired));
		CHECK(button.Graph.SourceAnimators->Bindings == before.SourceAnimators->Bindings);
		CHECK(button.Graph.SourceAnimators->Detached == before.SourceAnimators->Detached);
		if (retired) {
			CHECK(button.Graph.SourceAnimators->DetachedValues[0].Keys.empty());
			CHECK(button.Graph.Keyframes[0] == before.Keyframes[0]);
		}
		const auto after = button.Graph;
		Document restored;
		REQUIRE(Read(Write(after), restored, button.Error) == Status::Ok);
		CHECK(restored == after);
		GroupReplayState replay;
		REQUIRE(RestoreSourceAnimatorBindings(restored, {}, 1, replay, button.Error) == Status::Ok);
		REQUIRE(button.History.Undo(button.Graph));
		CHECK(button.Graph == before);
		CHECK_FALSE(button.History.CanUndo());
		REQUIRE(button.History.Redo(button.Graph));
		CHECK(button.Graph == after);
		CHECK_FALSE(button.History.CanRedo());
	}
}
TEST_CASE("Timeline table Delete preserves the document when history refuses", "[studio][timeline]") {
	Button button(Keys(true), 0);
	const auto before = button.Graph;
	button.Click();
	CHECK(button.Calls == 1);
	CHECK(button.Accepted == 0);
	CHECK(button.Graph == before);
	CHECK_FALSE(button.History.CanUndo());
}
TEST_CASE("Timeline table Delete keeps legacy keys local", "[studio][timeline]") {
	Button button(Keys(false, false));
	const auto owner = button.Graph.Keyframes[0];
	button.Click();
	REQUIRE(button.Accepted == 1);
	REQUIRE(button.Graph.Keyframes.size() == 1);
	CHECK(button.Graph.Keyframes[0] == owner);
	CHECK_FALSE(button.Graph.SourceAnimators);
}

TEST_CASE(
	"Timeline Delete captures the initial imported writer before undo", "[studio][timeline][source_aliases]"
) {
	auto document = Keys(false, false);
	document.Keyframes[1].SourceKeyId = "local-alias-id";
	Button ui(std::move(document), 128, true);
	ui.Click();
	INFO(ui.Error.Message);
	REQUIRE(ui.Accepted == 1);
	CHECK(ui.Graph.Keyframes.empty());
	REQUIRE(ui.Graph.SourceAnimators);
	REQUIRE(ui.History.Undo(ui.Graph));
	CHECK(ui.Graph.Keyframes.size() == 2);
	REQUIRE(ui.Graph.SourceAnimators);
	ui.Host.Clear();
	GroupReplayState restored;
	REQUIRE(RestoreSourceAnimatorBindings(ui.Graph, {}, 3, restored, ui.Error) == Status::Ok);
	REQUIRE(restored.Binding("alias", "mix"));
	CHECK(restored.Binding("alias", "mix")->OwnerId == "owner");
	REQUIRE(ui.History.Redo(ui.Graph));
	CHECK(ui.Graph.Keyframes.empty());
}

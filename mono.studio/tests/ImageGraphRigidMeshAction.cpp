#include "ImageGraphRigidMeshAction.hpp"

#include "../../mono.engine/imagegraphphysics/tests/RigidGraphFixture.hpp"
#include "ImageGraphGroupHost.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph.rigid_mesh_action")
TEST_DEPENDS("engine.imagegraph.rigid_mesh_action")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	struct MeshInspector {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc = engine::imagegraphphysics::testing::RigidGraphFixture();
		studio::ImageGraphHistory History;
		studio::ImageGraphPreviewCache Cache;
		studio::ImageGraphGroupHost Groups;
		CapturedFeedbackHost Host;
		Diagnostic Error;
		ImVec2 Action;
		uint64_t Revision = 7, Budget = Limits::MaximumEvaluationBytes;
		unsigned Captures = 0, Changes = 0;
		bool ChangeSelectionDuringCapture = false;
		std::string SelectedNode = "body";
		MeshInspector(size_t historyBytes = 16 * 1024 * 1024) : History(128, historyBytes) {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.Nodes[1].Values[0].Data = int64_t{5};
			Doc.Nodes[1].Values[1].Data = int64_t{5};
			ArrayValue triangle{
				ValueType::Any,
				{},
				{},
				{{std::vector<SourceArrayItem>{
					{ElementValue{Vector2{-2, -2}}},
					{ElementValue{Vector2{2, -2}}},
					{ElementValue{Vector2{0, 2}}}
				}}}
			};
			Doc.Nodes[2].Values.push_back({"attribute_mesh", std::move(triangle)});
			Doc.Nodes[2].Values.push_back({"shape", EnumValue{2}});
			Plan plan;
			const auto compiled = Compile(Doc, plan, Error);
			INFO(Error.NodeId << ':' << Error.Port << ' ' << Error.Message);
			REQUIRE(compiled == Status::Ok);
			engine::imagegraphphysics::RigidProvider provider;
			EvaluationRequest request;
			request.RigidProvider = &provider;
			StatefulEvaluationResult result;
			REQUIRE(EvaluateStateful(Doc, plan, "image", request, result, Error) == Status::Ok);
			REQUIRE(Cache.Store(Revision, 0, 0, std::get<Image>(result.Output)));
			// The action must replace a retained snapshot for another selected node
			// and authored revision, rather than borrowing that prior generation.
			REQUIRE(Host.PrepareNodeInputs(Doc, plan, Revision - 1, 1, "render", request, Error));
		}
		~MeshInspector() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		const ArrayValue &Mesh() const {
			const auto &values = Doc.Nodes[2].Values;
			const auto found = std::find_if(values.begin(), values.end(), [](const auto &value) {
				return value.Port == "attribute_mesh";
			});
			REQUIRE(found != values.end());
			return std::get<ArrayValue>(found->Data);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Rigid inspector", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			studio::detail::DrawImageGraphRigidMeshAction(
				Doc,
				History,
				"body",
				[&](Diagnostic &error) -> const EvaluationSnapshot * {
					++Captures;
					const auto capturedRevision = Revision;
					Plan plan;
					engine::imagegraphphysics::RigidProvider provider;
					EvaluationRequest request;
					request.RigidProvider = &provider;
					if (Compile(Doc, plan, error) != Status::Ok ||
						!Groups.Prepare(Doc, plan, Revision, request, error) ||
						!Host.PrepareNodeInputs(Doc, plan, Revision, 1, "body", request, error))
						return nullptr;
					REQUIRE(Host.Active());
					const auto &snapshot = Host.Snapshot();
					const auto texture = std::find_if(
						snapshot.Images().begin(), snapshot.Images().end(), [](const auto &input) {
							return input.Port == "texture";
						}
					);
					REQUIRE(texture != snapshot.Images().end());
					CHECK(texture->Data.Width == 5);
					CHECK(texture->Data.Height == 5);
					if (ChangeSelectionDuringCapture) SelectedNode = "texture";
					if (!studio::detail::ImageGraphRigidMeshCaptureCurrent(
							Doc, "body", SelectedNode, Revision, 1, capturedRevision, 1, error
						))
						return nullptr;
					return &snapshot;
				},
				[&] {
					++Changes;
					++Revision;
					Cache.Clear();
				},
				Error,
				Budget
			);
			auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
			Action = {(lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f};
			ImGui::End();
			ImGui::Render();
		}
		void Click() {
			Frame();
			Frame();
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(Action.x, Action.y);
			Frame();
			io.AddMouseButtonEvent(0, true);
			Frame();
			io.AddMouseButtonEvent(0, false);
			Frame();
		}
	};
}

TEST_CASE(
	"Generate Mesh button captures once and replaces saved geometry with one undo and preview invalidation",
	"[studio][rigid_mesh_action]"
) {
	MeshInspector ui;
	const auto original = ui.Doc;
	const auto before = ui.Mesh();
	ui.Click();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Captures == 1);
	CHECK(ui.Revision == 8);
	CHECK(ui.Error.Code == Status::Ok);
	REQUIRE(ui.Mesh() != before);
	REQUIRE(ui.Mesh().Items.size() == 1);
	const auto *points = std::get_if<std::vector<SourceArrayItem>>(&ui.Mesh().Items[0].Data);
	REQUIRE(points);
	CHECK(points->size() >= 3);
	CHECK(points->size() <= 8);
	CHECK_FALSE(ui.Cache.Find(7, 0, 0));
	CHECK_FALSE(ui.Cache.Find(8, 0, 0));
	CHECK(ui.Cache.HeldBytes() == 0);
	const auto edited = ui.Doc;
	Document persisted;
	REQUIRE(Read(Write(edited), persisted, ui.Error) == Status::Ok);
	CHECK(persisted == edited);
	ui.Click();
	CHECK(ui.Captures == 2);
	CHECK(ui.Changes == 1);
	CHECK(ui.Revision == 8);
	CHECK(ui.Error.Code == Status::Ok);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == original);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == edited);
}

TEST_CASE(
	"Generate Mesh refusal preserves saved geometry, history and preview", "[studio][rigid_mesh_action]"
) {
	SECTION("selected node changes after real input capture") {
		MeshInspector ui;
		const auto saved = Write(ui.Doc);
		const auto cacheBytes = ui.Cache.HeldBytes();
		ui.ChangeSelectionDuringCapture = true;
		ui.Click();
		CHECK(ui.Error.Code == Status::InvalidValue);
		CHECK(ui.SelectedNode == "texture");
		CHECK(Write(ui.Doc) == saved);
		CHECK(ui.Changes == 0);
		CHECK(ui.Captures == 1);
		CHECK(ui.Revision == 7);
		CHECK_FALSE(ui.History.CanUndo());
		CHECK(ui.Cache.HeldBytes() == cacheBytes);
		CHECK(ui.Cache.Find(7, 0, 0));
	}
	SECTION("byte admission") {
		MeshInspector ui;
		const auto saved = Write(ui.Doc);
		const auto cacheBytes = ui.Cache.HeldBytes();
		ui.Budget = 1;
		ui.Click();
		CHECK(ui.Error.Code == Status::LimitExceeded);
		CHECK(Write(ui.Doc) == saved);
		CHECK(ui.Changes == 0);
		CHECK(ui.Captures == 1);
		CHECK(ui.Revision == 7);
		CHECK_FALSE(ui.History.CanUndo());
		CHECK(ui.Cache.HeldBytes() == cacheBytes);
		CHECK(ui.Cache.Find(7, 0, 0));
	}
	SECTION("undo retention") {
		MeshInspector ui(1);
		const auto saved = Write(ui.Doc);
		const auto cacheBytes = ui.Cache.HeldBytes();
		ui.Click();
		CHECK(ui.Error.Code == Status::LimitExceeded);
		CHECK(Write(ui.Doc) == saved);
		CHECK(ui.Changes == 0);
		CHECK(ui.Captures == 1);
		CHECK(ui.Revision == 7);
		CHECK_FALSE(ui.History.CanUndo());
		CHECK(ui.Cache.HeldBytes() == cacheBytes);
		CHECK(ui.Cache.Find(7, 0, 0));
	}
}

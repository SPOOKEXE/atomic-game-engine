#include "NodeExecutors.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace engine::imagegraph::detail {
	bool SourceSceneAffector(NodeContext &);
}

TEST_SUITE_ID("engine.imagegraph.source_scene_affector")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraph::detail;
	void Check(bool success, const char *message) {
		INFO(message);
		REQUIRE(success);
	}
	SceneValue3D Scene(double x = 0) {
		SceneValue3D scene;
		MeshValue3D mesh;
		mesh.Data.emplace().LocalTransforms.push_back({});
		mesh.Data->LocalTransforms[0].Position = {x, 0, 0};
		mesh.Data->Parts.emplace_back().Vertices.resize(3);
		std::array<double, 16> local{};
		local[0] = local[5] = local[10] = local[15] = 1;
		local[12] = 42;
		mesh.Data->Parts.front().LocalMatrix = local;
		mesh.Data->Edges.push_back({{0, 0, 0}, {1, 0, 0}});
		mesh.Data->Materials.emplace_back();
		scene.Data.emplace().Objects.push_back({std::move(mesh)});
		return scene;
	}
	struct Run {
		Node node{"affector", "pc.3_d_affector", "", {}, {}};
		EvaluationRequest request;
		NodeContext context;
		Run(uint64_t tick = 0, const DataReplayState *owner = nullptr)
			: request(), context(node, *FindCatalogueEntry(node.Type), request) {
			request.Tick = tick;
			request.DataReplay = owner;
			context.ByteBudget = Limits::MaximumEvaluationBytes;
			for (const auto &input : context.Entry.Inputs)
				if (auto value = CatalogueDefault(input))
					context.Values.emplace_back(input.Id, std::move(*value));
		}
		void Set(std::string_view port, Value value) {
			for (auto &pair : context.Values)
				if (pair.first == port) {
					pair.second = std::move(value);
					return;
				}
			context.Values.emplace_back(port, std::move(value));
		}
		bool Execute() {
			const bool ok = SourceSceneAffector(context);
			INFO(context.FailureMessage);
			return ok;
		}
		const SceneValue3D &Result() {
			return std::get<SceneValue3D>(context.OutputValues.at(0).Data);
		}
	};
	const MeshTransform3D &Pose(const SceneValue3D &scene) {
		return std::get<MeshValue3D>(scene.Data->Objects[0].Data).Data->LocalTransforms.front();
	}
}

TEST_CASE(
	"source scene affector preserves source falloff clone and quaternion semantics",
	"[imagegraph][source_scene_affector]"
) {

	Run run;
	const SceneValue3D source = Scene();
	run.Set("scene", source);
	run.Set("affect_position", Vector3{2, 3, 4});
	run.Set("affect_scale", Vector3{2, 4, 6});
	Check(run.Execute(), "sphere center executes");
	Check(Pose(run.Result()).Position == Vector3{2, 3, 4}, "center fully affected");
	Check(Pose(run.Result()).Scale == Vector3{2, 4, 6}, "component scale source math");
	Check(Pose(source).Position == Vector3{}, "source input unchanged");
	const auto &clonedMesh = *std::get<MeshValue3D>(run.Result().Data->Objects[0].Data).Data;
	Check(
		!clonedMesh.CpuVerticesPresent && !clonedMesh.CpuEdgesPresent,
		"clone(false) clears CPU geometry availability"
	);
	Check(
		clonedMesh.Parts.size() == 1 && clonedMesh.Parts.front().Vertices.size() == 3,
		"clone retains drawable vertices"
	);
	Check(!clonedMesh.Parts.front().LocalMatrix, "source clone omits per-part matrix");
	Check(clonedMesh.Edges.size() == 1, "source clone retains drawable edge buffer");
	Check(
		std::get<MeshValue3D>(source.Data->Objects[0].Data).Data->Parts.front().LocalMatrix.has_value(),
		"source input keeps its part matrix"
	);
	Check(
		std::get<MeshValue3D>(source.Data->Objects[0].Data).Data->CpuVerticesPresent,
		"source CPU vertices remain available"
	);
	Check(run.context.DataUpdates.size() == 1, "first frame captured");
	Check(
		std::get<ArrayValue>(run.context.DataUpdates[0].Values[0].Data).Elements.size() == 101,
		"source 100 intervals captured"
	);
	DataReplayState replay{run.context.DataUpdates};
	Run later(1, &replay);
	later.Set("scene", Scene(.5));
	later.Set("affect_position", Vector3{4, 0, 0});
	Curve flat = std::get<Curve>(*CatalogueDefault(
		*std::find_if(later.context.Entry.Inputs.begin(), later.context.Entry.Inputs.end(), [](auto &i) {
			return i.Id == "falloff_curve";
		})
	));
	for (auto &a : flat.Anchors)
		a[1] = 0;
	later.Set("falloff_curve", flat);
	Check(later.Execute(), "later frame uses captured curve");
	Check(
		std::abs(Pose(later.Result()).Position.X - 2.5) < 1e-9, "captured original curve despite new input"
	);
	Check(later.context.DataUpdates.empty(), "no recapture later");
	Run absent(1);
	absent.Set("scene", Scene());
	Check(!absent.Execute(), "uncaptured seek diagnosed");
	Check(absent.context.FailureCode == Status::UnsupportedExecution, "uncaptured seek named status");
	Check(absent.context.OutputValues.empty(), "failure leaves no output");
	Run outer;
	outer.Set("scene", Scene(.75));
	outer.Set("affect_position", Vector3{2, 0, 0});
	Check(outer.Execute(), "outer boundary executes");
	Check(Pose(outer.Result()).Position.X == .75, "outer boundary unaffected");
	Run plane;
	plane.Set("scene", Scene());
	plane.Set("shape", EnumValue{1});
	plane.Set("affect_position", Vector3{0, 0, 4});
	Check(plane.Execute(), "plane midpoint executes");
	Check(std::abs(Pose(plane.Result()).Position.Z - 2) < 1e-9, "plane midpoint half influence");
	Run nested;
	auto group = Scene();
	auto inner = Scene();
	inner.Data->Transform.Position = {.5, 0, 0};
	group.Data->Objects.clear();
	group.Data->Objects.push_back({std::move(inner.Data)});
	nested.Set("scene", group);
	nested.Set("affect_position", Vector3{2, 0, 0});
	Check(nested.Execute(), "nested scene executes");
	const auto &child = std::get<OwnedPayload3D<SceneData3D>>(nested.Result().Data->Objects[0].Data);
	Check(std::abs(child->Transform.Position.X - 1.5) < 1e-9, "affects immediate group center");
	Check(
		std::get<MeshValue3D>(child->Objects[0].Data).Data->LocalTransforms.front().Position == Vector3{},
		"does not affect descendants independently"
	);
	Run lights;
	SceneValue3D lightScene;
	LightValue3D light;
	light.Data.emplace();
	lightScene.Data.emplace().Objects.push_back({std::move(light)});
	lights.Set("scene", lightScene);
	Check(lights.Execute(), "source light clone executes");
	Check(
		std::holds_alternative<MeshValue3D>(lights.Result().Data->Objects[0].Data),
		"source clone loses light subclass"
	);
	Check(
		std::get<MeshValue3D>(lights.Result().Data->Objects[0].Data).Data->Parts.empty(),
		"light clone has no drawable VB"
	);
	Run instancer;
	auto instanced = Scene();
	std::get<MeshValue3D>(instanced.Data->Objects[0].Data).Data->Instanced = true;
	instancer.Set("scene", instanced);
	Check(!instancer.Execute(), "undefined source instancer clone refused");
	Check(instancer.context.FailureCode == Status::UnsupportedExecution, "instancer source gap named");
	Run budget;
	budget.context.ByteBudget = 64;
	budget.Set("scene", Scene());
	Check(!budget.Execute(), "budget fails before clone");
	Check(budget.context.OutputValues.empty(), "budget output atomic");
	Check(budget.context.DataUpdates.empty(), "budget replay atomic");
	Run rotation;
	rotation.Set("scene", Scene());
	rotation.Set("affect_rotation", Quaternion{0, 0, 0, 2});
	Check(rotation.Execute(), "nonunit source quaternion executes");
	Check(Pose(rotation.Result()).Rotation.W == 4, "preserves source second norm multiply");
	Run invalid;
	invalid.Set("scene", Scene());
	invalid.Set("rotation", Quaternion{0, 0, 0, 0});
	invalid.Set("shape", EnumValue{1});
	Check(!invalid.Execute(), "zero quaternion plane invalid");
	Check(invalid.context.OutputValues.empty(), "nonfinite source math atomic");
}

TEST_CASE(
	"registered scene affector seeks through its captured first-frame curve",
	"[imagegraph][source_scene_affector]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"cube", "pc.3_d_mesh_cube", "", {}, {{"position", Vector3{.5, 0, 0}}}},
		{"scene", "pc.3_d_scene", "", {}, {}},
		{"affector", "pc.3_d_affector", "", {}, {{"affect_position", Vector3{4, 0, 0}}}}
	};
	document.Nodes[1].DynamicInputs = {{"cube", ValueType::Mesh, std::nullopt}};
	document.Links = {{"cube", "mesh", "scene", "cube"}, {"scene", "scene", "affector", "scene"}};
	document.Outputs = {{"out", "affector", "scene"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	const std::array<std::string, 1> outputs{"out"};
	const auto cone = AnalyzeStatefulTemporalCone(document, plan, outputs);
	CHECK(cone.DataProcessors == 1);
	CHECK(cone.FirstFrameData);
	CHECK_FALSE(cone.Simulation);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.Tick = 3;
	const bool prepared =
		host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out");
	INFO(diagnostic.Message);
	REQUIRE(prepared);
	REQUIRE(host.Value("out"));
	const auto &scene = std::get<SceneValue3D>(std::get<EvaluatedValue>(host.Value("out")->Output).Data);
	CHECK(Pose(scene).Position.X == Catch::Approx(2.5));
	const auto &mesh = *std::get<MeshValue3D>(scene.Data->Objects.front().Data).Data;
	CHECK_FALSE(mesh.CpuVerticesPresent);
	CHECK_FALSE(mesh.CpuEdgesPresent);
	CHECK_FALSE(mesh.Parts.empty());
	REQUIRE(request.DataReplay);
	REQUIRE(request.DataReplay->Entries.size() == 1);
	REQUIRE(request.DataReplay->Entries.front().Values.size() == 1);
	CHECK(
		std::get<ArrayValue>(request.DataReplay->Entries.front().Values.front().Data).Elements.size() == 101
	);
	request = {};
	request.Tick = 1;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(
		Pose(std::get<SceneValue3D>(std::get<EvaluatedValue>(host.Value("out")->Output).Data)).Position.X ==
		Catch::Approx(2.5)
	);
	request = {};
	request.Tick = 4097;
	CHECK_FALSE(
		host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
	);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(
		Pose(std::get<SceneValue3D>(std::get<EvaluatedValue>(host.Value("out")->Output).Data)).Position.X ==
		Catch::Approx(2.5)
	);
}

// Pinned mesh literals and wrapper metadata are checked independently of rasterization.

#include "../src/MeshPayload.hpp"
#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.mesh_ops")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	constexpr std::string_view PLANE = "pc.3_d_mesh_plane", TRANSFORM = "pc.3_d_transform",
							   GET = "pc.3_d_get_data";
	MeshValue3D DefaultPlane() {
		const auto run = RunNode(PLANE, {});
		INFO(run.Message);
		REQUIRE(run.Ok);
		return std::get<MeshValue3D>(*run.OutputValue("mesh"));
	}
	Value
	Replay(const Document &document, std::string_view output = "out", const EvaluationRequest &request = {}) {
		Document restored;
		Diagnostic diagnostic;
		const auto read = Read(Write(document), restored, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(read == Status::Ok);
		CHECK(restored == document);
		Plan plan;
		const auto compile = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compile == Status::Ok);
		EvaluatedValue value;
		const auto status = EvaluateValue(restored, plan, std::string(output), request, value, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return std::move(value.Data);
	}
	Node Solid(std::string id, Colour colour) {
		return {
			std::move(id),
			"pc.solid",
			"",
			{},
			{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"color", colour}}
		};
	}
	Document MaterialRows(int64_t mode) {
		Document doc;
		doc.FormatVersion = 7;
		Node rows{"rows", "value.array", "", {}, {}};
		rows.DynamicInputs = {
			{"red", ValueType::Image, std::nullopt}, {"green", ValueType::Image, std::nullopt}
		};
		doc.Nodes = {
			Solid("red", {255, 0, 0, 255}),
			Solid("green", {0, 255, 0, 255}),
			rows,
			{"plane", std::string(PLANE), "", {}, {{"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {
			{"red", "surface_out", "rows", "red"},
			{"green", "surface_out", "rows", "green"},
			{"rows", "array", "plane", "material"}
		};
		doc.Outputs = {{"out", "plane", "mesh"}};
		return doc;
	}
}

TEST_CASE("CPU Plane defaults persist and expose the source Z geometry", "[imagegraph][mesh_ops]") {
	Document doc;
	doc.FormatVersion = 6;
	doc.Nodes = {{"plane", std::string(PLANE), "", {}, {}}};
	doc.Outputs = {{"out", "plane", "mesh"}};
	const Value result = Replay(doc);
	const auto &mesh = std::get<MeshValue3D>(result);
	REQUIRE(mesh.Data);
	const auto &data = *mesh.Data;
	REQUIRE(data.Parts.size() == 1);
	REQUIRE(data.Materials.size() == 1);
	CHECK((data.Materials.front() == MaterialValue3D{}));
	CHECK((data.LocalTransforms == std::vector<MeshTransform3D>{MeshTransform3D{}}));
	CHECK(data.Edges.size() == 4);
	const std::array positions{
		Vector3{-.5, -.5, 0},
		Vector3{.5, .5, 0},
		Vector3{.5, -.5, 0},
		Vector3{-.5, -.5, 0},
		Vector3{-.5, .5, 0},
		Vector3{.5, .5, 0}
	};
	const std::array uvs{
		Vector2{0, 0}, Vector2{1, 1}, Vector2{0, 1}, Vector2{0, 0}, Vector2{1, 0}, Vector2{1, 1}
	};
	REQUIRE(data.Parts.front().Vertices.size() == 6);
	for (size_t i = 0; i < positions.size(); ++i) {
		const auto &vertex = data.Parts.front().Vertices[i];
		CHECK(vertex.Position == positions[i]);
		CHECK(vertex.UV == uvs[i]);
		CHECK((vertex.Normal == Vector3{0, 0, 1}));
		CHECK((vertex.Tint == Colour{255, 255, 255, 255}));
	}
	CHECK((data.Edges.front() == MeshEdge3D{{-.5, -.5, 0}, {.5, -.5, 0}}));
	CHECK((data.Edges.back() == MeshEdge3D{{-.5, .5, 0}, {-.5, -.5, 0}}));
}

TEST_CASE("CPU Plane axes and reverse back side preserve ordered source literals", "[imagegraph][mesh_ops]") {
	const std::array<std::array<Vector3, 6>, 3> positions{
		{{{{0, -.5, -.5}, {0, .5, .5}, {0, .5, -.5}, {0, -.5, -.5}, {0, -.5, .5}, {0, .5, .5}}},
		 {{{-.5, 0, -.5}, {.5, 0, -.5}, {.5, 0, .5}, {-.5, 0, -.5}, {.5, 0, .5}, {-.5, 0, .5}}},
		 {{{-.5, -.5, 0}, {.5, .5, 0}, {.5, -.5, 0}, {-.5, -.5, 0}, {-.5, .5, 0}, {.5, .5, 0}}}}
	};
	const std::array<std::array<Vector2, 6>, 3> uvs{
		{{{{0, 1}, {1, 0}, {1, 1}, {0, 1}, {0, 0}, {1, 0}}},
		 {{{1, 1}, {0, 1}, {0, 0}, {1, 1}, {0, 0}, {1, 0}}},
		 {{{0, 0}, {1, 1}, {0, 1}, {0, 0}, {1, 0}, {1, 1}}}}
	};
	const std::array normals{Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{0, 0, 1}};
	for (int64_t axis = 0; axis < 3; ++axis) {
		const auto run = RunNode(PLANE, {}, {{"normal", EnumValue{axis}}, {"both_side", true}});
		REQUIRE(run.Ok);
		const auto &data = *std::get<MeshValue3D>(*run.OutputValue("mesh")).Data;
		REQUIRE(data.Parts.size() == 2);
		CHECK(data.Materials.size() == 2);
		CHECK(data.Edges.size() == 4);
		for (size_t part = 0; part < 2; ++part) {
			CHECK(data.Parts[part].MaterialIndex == part);
			REQUIRE(data.Parts[part].Vertices.size() == 6);
			for (size_t i = 0; i < 6; ++i) {
				const size_t index = part == 0 ? i : 5 - i;
				const auto &v = data.Parts[part].Vertices[i];
				CHECK(v.Position == positions[size_t(axis)][index]);
				CHECK(v.UV == uvs[size_t(axis)][index]);
				const Vector3 n = normals[size_t(axis)];
				CHECK((v.Normal == (part == 0 ? n : Vector3{-n.X, -n.Y, -n.Z})));
				CHECK((v.Tint == Colour{255, 255, 255, 255}));
			}
		}
	}
}

TEST_CASE(
	"Plane material surface getters own distinct persisted front and back bytes", "[imagegraph][mesh_ops]"
) {
	Document doc;
	doc.FormatVersion = 6;
	doc.Nodes = {
		Solid("front", {12, 34, 56, 255}),
		Solid("back", {90, 80, 70, 255}),
		{"plane", std::string(PLANE), "", {}, {{"both_side", true}}},
		{"wrap", std::string(TRANSFORM), "", {}, {{"position", Vector3{7, 8, 9}}}}
	};
	doc.Links = {
		{"front", "surface_out", "plane", "material"},
		{"back", "surface_out", "plane", "back_material"},
		{"plane", "mesh", "wrap", "mesh"}
	};
	doc.Outputs = {{"out", "wrap", "mesh"}};
	const Value result = Replay(doc);
	const auto &data = *std::get<MeshValue3D>(result).Data;
	REQUIRE(data.Materials.size() == 2);
	REQUIRE(data.Materials[0].Get().Surface);
	REQUIRE(data.Materials[1].Get().Surface);
	CHECK((data.Materials[0].Get().Surface->Pixels == std::vector<uint8_t>{12, 34, 56, 255}));
	CHECK((data.Materials[1].Get().Surface->Pixels == std::vector<uint8_t>{90, 80, 70, 255}));
	CHECK((data.LocalTransforms[0].Position == Vector3{7, 8, 9}));
	CHECK(data.LocalTransforms.size() == 2);
}

TEST_CASE(
	"Transform wrappers preserve local chain and GetData reads the outer record", "[imagegraph][mesh_ops]"
) {
	Document doc;
	doc.FormatVersion = 6;
	const MeshTransform3D base{{1, 2, 3}, {4, 5, 6}, {.1, .2, .3, .4}, {-1, 0, 2}};
	const MeshTransform3D inner{{7, 8, 9}, {-4, -5, -6}, {.5, .6, .7, .8}, {3, 4, 5}};
	const MeshTransform3D outer{{11, 12, 13}, {14, 15, 16}, {.9, 1., 1.1, 1.2}, {-2, 0, 7}};
	const auto node = [](std::string id, std::string_view type, const MeshTransform3D &t) -> Node {
		return {
			std::move(id),
			std::string(type),
			"",
			{},
			{{"position", t.Position}, {"anchor", t.Anchor}, {"rotation", t.Rotation}, {"scale", t.Scale}}
		};
	};
	doc.Nodes = {
		node("plane", PLANE, base),
		node("inner", TRANSFORM, inner),
		node("outer", TRANSFORM, outer),
		{"get", std::string(GET), "", {}, {}}
	};
	doc.Links = {
		{"plane", "mesh", "inner", "mesh"},
		{"inner", "mesh", "outer", "mesh"},
		{"outer", "mesh", "get", "mesh"}
	};
	doc.Outputs = {
		{"mesh", "outer", "mesh"},
		{"origin", "get", "origin"},
		{"position", "get", "position"},
		{"rotation", "get", "rotation"},
		{"scale", "get", "scale"}
	};
	const Value result = Replay(doc, "mesh");
	const auto &data = *std::get<MeshValue3D>(result).Data;
	CHECK((data.LocalTransforms == std::vector<MeshTransform3D>{outer, inner, base}));
	const auto defaultMesh = DefaultPlane();
	CHECK(data.Parts == defaultMesh.Data->Parts);
	CHECK(data.Materials == defaultMesh.Data->Materials);
	CHECK(data.Edges == defaultMesh.Data->Edges);
	const Value origin = Replay(doc, "origin"), position = Replay(doc, "position"),
				rotation = Replay(doc, "rotation"), scale = Replay(doc, "scale");
	CHECK((origin == Value{outer.Anchor}));
	CHECK((position == Value{outer.Position}));
	CHECK((rotation == Value{Vector4{.9, 1., 1.1, 1.2}}));
	CHECK((scale == Value{outer.Scale}));
}

TEST_CASE(
	"Material surface arrays schedule exact getters through all source processor modes",
	"[imagegraph][mesh_ops]"
) {
	for (int64_t mode = 0; mode < 4; ++mode) {
		const auto doc = MaterialRows(mode);
		const Value result = Replay(doc);
		const auto &meshes = std::get<ArrayValue>(result);
		CHECK(meshes.ElementType == ValueType::Mesh);
		REQUIRE(meshes.Elements.size() == 2);
		const auto &red = *std::get<MeshValue3D>(meshes.Elements[0]).Data;
		const auto &green = *std::get<MeshValue3D>(meshes.Elements[1]).Data;
		CHECK((red.Materials.front().Get().Surface->Pixels == std::vector<uint8_t>{255, 0, 0, 255}));
		CHECK(
			green.Materials.front().Get().Surface->Pixels ==
			(mode == 3 ? std::vector<uint8_t>{255, 0, 0, 255} : std::vector<uint8_t>{0, 255, 0, 255})
		);
	}
	Document doc = MaterialRows(0);
	doc.Nodes.push_back({"wrap", std::string(TRANSFORM), "", {}, {{"position", Vector3{3, 4, 5}}}});
	doc.Nodes.push_back({"get", std::string(GET), "", {}, {}});
	doc.Links.push_back({"plane", "mesh", "wrap", "mesh"});
	doc.Links.push_back({"wrap", "mesh", "get", "mesh"});
	doc.Outputs = {{"out", "get", "position"}};
	const Value result = Replay(doc);
	CHECK((result == Value{ArrayValue{ValueType::Vector3, {Vector3{3, 4, 5}, Vector3{3, 4, 5}}}}));
}

TEST_CASE(
	"Plane scalar Normal clamps endpoints but refuses unmatched fractional source geometry",
	"[imagegraph][mesh_ops]"
) {
	for (double normal : {-20., -.5, 2.5, 20.}) {
		const auto run = RunNode(PLANE, {}, {{"normal", normal}});
		REQUIRE(run.Ok);
		const auto &first =
			std::get<MeshValue3D>(*run.OutputValue("mesh")).Data->Parts.front().Vertices.front();
		CHECK((first.Normal == (normal < 0 ? Vector3{1, 0, 0} : Vector3{0, 0, 1})));
	}
	for (double normal : {.25, 1.5}) {
		const auto run = RunNode(PLANE, {}, {{"normal", normal}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::UnsupportedExecution);
		CHECK(run.Port == "normal");
		CHECK(run.Values.empty());
	}
	const auto *entry = FindCatalogueEntry(PLANE);
	REQUIRE(entry);
	const Node node{"plane", std::string(PLANE), "", {}, {}};
	const EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"attribute_array_process", EnumValue{0}},
		{"both_side", ArrayValue{ValueType::Boolean, {false, true}}}
	};
	CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor(PLANE)));
	CHECK(context.FailureCode == Status::UnsupportedExecution);
	CHECK(context.FailurePort == "both_side");
	CHECK(context.OutputValues.empty());
	for (bool linked : {false, true}) {
		Document doc;
		doc.FormatVersion = 7;
		doc.Nodes = {{"plane", std::string(PLANE), "", {}, {}}};
		if (linked) {
			Node rows{"flags", "pc.array", "", {}, {{"type", EnumValue{0}}}};
			rows.DynamicInputs = {
				{"input_0", ValueType::Boolean, false}, {"input_1", ValueType::Boolean, true}
			};
			doc.Nodes.push_back(rows);
			doc.Outputs = {{"flags", "flags", "array"}};
			CHECK((Replay(doc, "flags") == Value{ArrayValue{ValueType::Boolean, {false, true}}}));
			doc.Links = {{"flags", "array", "plane", "both_side"}};
		} else
			doc.Nodes.front().Values = {{"both_side", ArrayValue{ValueType::Boolean, {false, true}}}};
		doc.Outputs = {{"out", "plane", "mesh"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		EvaluatedValue sentinel{"unchanged", 31.0};
		CHECK((EvaluateValue(doc, plan, "out", {}, sentinel, diagnostic) == Status::UnsupportedExecution));
		CHECK(diagnostic.Port == "both_side");
		CHECK(sentinel.Port == "unchanged");
		CHECK((sentinel.Data == Value{31.0}));
	}
}

TEST_CASE("Absent mesh retains source Transform and GetData defaults", "[imagegraph][mesh_ops]") {
	const auto transformed = RunNode(TRANSFORM, {});
	REQUIRE(transformed.Ok);
	CHECK((*transformed.OutputValue("mesh") == Value{MeshValue3D{}}));
	for (const auto &run : {RunNode(GET, {}), RunNode(GET, {}, {{"mesh", MeshValue3D{}}})}) {
		REQUIRE(run.Ok);
		CHECK((*run.OutputValue("origin") == Value{Vector3{}}));
		CHECK((*run.OutputValue("position") == Value{Vector3{}}));
		CHECK(((*run.OutputValue("rotation")) == Value{Vector4{0, 0, 0, 1}}));
		CHECK((*run.OutputValue("scale") == Value{Vector3{}}));
	}
	Document doc;
	doc.Nodes = {{"wrap", std::string(TRANSFORM), "", {}, {}}, {"get", std::string(GET), "", {}, {}}};
	doc.Links = {{"wrap", "mesh", "get", "mesh"}};
	doc.Outputs = {{"out", "get", "scale"}};
	const Value result = Replay(doc);
	CHECK((result == Value{Vector3{}}));
}

TEST_CASE("Mesh runtime validation rejects malformed data and authored payloads", "[imagegraph][mesh_ops]") {
	MeshValue3D mesh = DefaultPlane();
	CHECK((detail::ValidRuntimeValue(Value{mesh})));
	CHECK_FALSE(detail::ValidValuePayload(Value{mesh}, false));
	CHECK_FALSE(detail::ValidValuePayload(Value{MaterialValue3D{}}, false));
	MeshValue3D invalid = mesh;
	invalid.Data->Parts.front().MaterialIndex = 99;
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
	invalid = mesh;
	invalid.Data->LocalTransforms.clear();
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
	invalid = mesh;
	invalid.Data->Parts.front().Vertices.front().Position.X = std::numeric_limits<double>::infinity();
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
	invalid = mesh;
	invalid.Data->Parts.front().Vertices.pop_back();
	CHECK_FALSE(detail::ValidRuntimeValue(Value{invalid}));
	MaterialValue3D material;
	material.Edit().Surface = MaterialSurface3D{1, 1, {1, 2, 3}};
	CHECK_FALSE(detail::ValidRuntimeValue(Value{material}));
	for (const auto &run :
		 {RunNode(GET, {}, {{"mesh", invalid}}),
		  RunNode(PLANE, {}, {{"material", material}}),
		  RunNode(TRANSFORM, {}, {{"mesh", 2.0}}),
		  RunNode(PLANE, {}, {{"rotation", Vector3{1, 2, 3}}})}) {
		CHECK_FALSE(run.Ok);
		CHECK(run.Values.empty());
	}
	Document doc;
	doc.FormatVersion = 6;
	doc.Nodes = {{"plane", std::string(PLANE), "", {}, {{"material", mesh}}}};
	doc.Outputs = {{"out", "plane", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(doc, plan, diagnostic) == Status::UnknownPort);
	Document restored;
	CHECK(Read(Write(doc), restored, diagnostic) == Status::Malformed);
	CHECK(diagnostic.Message == "malformed imagegraph record at line 3");
	CHECK(restored.Nodes.empty());
}

TEST_CASE(
	"Mesh transform chain admits 64 records and refuses the next without publication",
	"[imagegraph][mesh_ops]"
) {
	MeshValue3D mesh = DefaultPlane();
	mesh.Data->LocalTransforms.resize(63);
	const auto accepted = RunNode(TRANSFORM, {}, {{"mesh", mesh}});
	REQUIRE(accepted.Ok);
	const auto &last = std::get<MeshValue3D>(*accepted.OutputValue("mesh"));
	CHECK(last.Data->LocalTransforms.size() == 64);
	const auto denied = RunNode(TRANSFORM, {}, {{"mesh", last}});
	CHECK_FALSE(denied.Ok);
	CHECK(denied.Code == Status::LimitExceeded);
	CHECK(denied.Values.empty());
	CHECK(last.Data->LocalTransforms.size() == 64);
	mesh.Data->LocalTransforms.resize(65);
	CHECK_FALSE(detail::ValidRuntimeValue(Value{mesh}));
}

TEST_CASE(
	"Owned mesh clone admission includes prior live payload and retained capacities",
	"[imagegraph][mesh_ops][allocation_ledger]"
) {
	MeshValue3D source = DefaultPlane();
	source.Data->Parts.front().Vertices.reserve(12);
	source.Data->Materials.front().Edit().Surface = MaterialSurface3D{1, 1, {9, 8, 7, 255}};
	source.Data->Materials.front().Edit().Surface->Pixels.reserve(64);
	CHECK(detail::RetainedPayloadBytes(source) > detail::PayloadOwnedBytes(source));
	const auto *entry = FindCatalogueEntry(TRANSFORM);
	REQUIRE(entry);
	const Node node{"wrap", std::string(TRANSFORM), "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto evaluate = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		{
			auto prior = ledger.Reserve(detail::RetainedPayloadBytes(source));
			REQUIRE(prior);
			MeshValue3D previous;
			auto &data = previous.Data.emplace();
			data.Parts.reserve(source.Data->Parts.size());
			data.Edges = source.Data->Edges;
			data.Materials.reserve(source.Data->Materials.size());
			data.LocalTransforms = source.Data->LocalTransforms;
			for (const auto &part : source.Data->Parts) {
				auto &copy = data.Parts.emplace_back();
				copy.MaterialIndex = part.MaterialIndex;
				copy.Vertices.reserve(part.Vertices.capacity());
				copy.Vertices.insert(copy.Vertices.end(), part.Vertices.begin(), part.Vertices.end());
			}
			MaterialValue3D material;
			auto &surface = material.Edit().Surface.emplace();
			surface.Width = surface.Height = 1;
			surface.Pixels.reserve(64);
			surface.Pixels.assign({9, 8, 7, 255});
			data.Materials.push_back(std::move(material));
			const Value input = std::move(previous);
			CHECK(detail::RetainedPayloadBytes(input) == prior->Bytes());
			{
				detail::NodeContext context(node, *entry, request, ledger);
				context.ByteBudget = maximum;
				context.ValueViews.emplace_back("mesh", &input);
				CHECK(detail::FindExecutor(TRANSFORM)(context) == accepted);
				if (accepted) {
					REQUIRE(context.OutputValues.size() == 1);
					const auto &copy = *std::get<MeshValue3D>(context.OutputValues.front().Data).Data;
					CHECK(copy.Parts == source.Data->Parts);
					CHECK(copy.Materials == source.Data->Materials);
					CHECK(copy.LocalTransforms.size() == 2);
					peak = ledger.Peak();
				} else {
					CHECK(context.FailureCode == Status::LimitExceeded);
					CHECK(context.OutputValues.empty());
				}
			}
			CHECK(ledger.Used() == prior->Bytes());
			CHECK(
				std::get<MeshValue3D>(input).Data->Materials.front().Get().Surface->Pixels ==
				std::vector<uint8_t>{9, 8, 7, 255}
			);
		}
		CHECK(ledger.Used() == 0);
	};
	evaluate(Limits::MaximumEvaluationBytes, true);
	REQUIRE(peak > 0);
	evaluate(peak, true);
	evaluate(peak - 1, false);
}

TEST_CASE(
	"Flat mesh arrays retain charged clones and nested rows refuse atomically",
	"[imagegraph][mesh_ops][allocation_ledger]"
) {
	const auto *entry = FindCatalogueEntry(TRANSFORM);
	REQUIRE(entry);
	const Node node{"wrap", std::string(TRANSFORM), "", {}, {}};
	const EvaluationRequest request;
	const MeshValue3D source = DefaultPlane();
	uint64_t peak = 0;
	const auto evaluate = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		{
			const uint64_t inputBytes = 2 * (sizeof(ElementValue) + detail::PayloadOwnedBytes(source));
			auto inputCharge = ledger.Reserve(inputBytes);
			REQUIRE(inputCharge);
			ArrayValue meshes{ValueType::Mesh, {}};
			meshes.Elements.reserve(2);
			meshes.Elements.emplace_back(source);
			meshes.Elements.emplace_back(source);
			const Value input = std::move(meshes);
			{
				detail::NodeContext context(node, *entry, request, ledger);
				context.ByteBudget = maximum;
				context.ValueViews.emplace_back("mesh", &input);
				CHECK(detail::RunProcessorBatch(context, detail::FindExecutor(TRANSFORM)) == accepted);
				if (accepted) {
					REQUIRE(context.OutputValues.size() == 1);
					const auto &output = std::get<ArrayValue>(context.OutputValues.front().Data);
					REQUIRE(output.Elements.size() == 2);
					for (const auto &element : output.Elements) {
						const auto &mesh = *std::get<MeshValue3D>(element).Data;
						CHECK(mesh.LocalTransforms.size() == 2);
						CHECK(mesh.Parts == source.Data->Parts);
					}
					peak = ledger.Peak();
				} else {
					CHECK(context.FailureCode == Status::LimitExceeded);
					CHECK(context.OutputValues.empty());
				}
			}
			CHECK(ledger.Used() == inputCharge->Bytes());
		}
		CHECK(ledger.Used() == 0);
	};
	evaluate(Limits::MaximumEvaluationBytes, true);
	REQUIRE(peak > 0);
	evaluate(peak, true);
	evaluate(peak - 1, false);
	ArrayValue nested{ValueType::Mesh, {}};
	nested.Nested = {{source, source}, {source}};
	const Value input = std::move(nested);
	CHECK(detail::ValidRuntimeValue(input));
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {{"attribute_array_process", EnumValue{0}}};
	context.ValueViews.emplace_back("mesh", &input);
	CHECK_FALSE(detail::RunProcessorBatch(context, detail::FindExecutor(TRANSFORM)));
	CHECK(context.FailureCode == Status::UnsupportedExecution);
	CHECK(context.FailurePort == "mesh");
	CHECK(context.OutputValues.empty());
}

TEST_CASE(
	"Owned typed front and back materials retain every descriptor and texture field", "[imagegraph][mesh_ops]"
) {
	MaterialValue3D front;
	front.Edit().TextureScale = {2, -3};
	front.Edit().TextureShift = {.25, -.5};
	front.Edit().TextureFilter = 2;
	front.Edit().Diffuse = .2;
	front.Edit().Specular = .7;
	front.Edit().Shininess = 19;
	front.Edit().Reflectance = .6;
	front.Edit().Metal = true;
	front.Edit().NormalStrength = 1.5;
	front.Edit().MetallicRange = {.1, .8};
	front.Edit().RoughnessRange = {.2, .9};
	front.Edit().MetallicMapped = true;
	front.Edit().RoughnessMapped = true;
	front.Edit().Surface = MaterialSurface3D{1, 1, {11, 22, 33, 255}};
	front.Edit().Normal = MaterialSurface3D{1, 1, {128, 128, 255, 255}};
	front.Edit().PropertiesMap = MaterialSurface3D{1, 1, {42, 53, 0, 255}};
	MaterialValue3D back;
	back.Edit().TextureShift = {-2, 7};
	back.Edit().Diffuse = .9;
	back.Edit().Surface = MaterialSurface3D{1, 1, {99, 88, 77, 255}};
	const auto run = RunNode(PLANE, {}, {{"both_side", true}, {"material", front}, {"back_material", back}});
	REQUIRE(run.Ok);
	const MeshValue3D mesh = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	front.Edit().Surface->Pixels.front() = 0;
	back.Edit().Surface->Pixels.front() = 0;
	const auto wrapped = RunNode(TRANSFORM, {}, {{"mesh", mesh}, {"position", Vector3{5, 6, 7}}});
	REQUIRE(wrapped.Ok);
	const auto &materials = std::get<MeshValue3D>(*wrapped.OutputValue("mesh")).Data->Materials;
	CHECK(materials == mesh.Data->Materials);
	CHECK(materials[0].Get().Surface->Pixels.front() == 11);
	CHECK(materials[1].Get().Surface->Pixels.front() == 99);
	CHECK(materials[0].Get().Normal == mesh.Data->Materials[0].Get().Normal);
	CHECK(materials[0].Get().PropertiesMap == mesh.Data->Materials[0].Get().PropertiesMap);
	CHECK(materials[0].Get().TextureScale == mesh.Data->Materials[0].Get().TextureScale);
	CHECK(materials[0].Get().Metal);
	CHECK(materials[0].Get().MetallicMapped);
	CHECK(materials[0].Get().RoughnessMapped);
}

TEST_CASE(
	"Material getter rows retain the source full-slot unequal expansion order", "[imagegraph][mesh_ops]"
) {
	Document doc = MaterialRows(0);
	Node backs{"backs", "value.array", "", {}, {}};
	backs.DynamicInputs = {
		{"a", ValueType::Image, std::nullopt},
		{"b", ValueType::Image, std::nullopt},
		{"c", ValueType::Image, std::nullopt}
	};
	doc.Nodes.push_back(Solid("blue_a", {0, 0, 10, 255}));
	doc.Nodes.push_back(Solid("blue_b", {0, 0, 20, 255}));
	doc.Nodes.push_back(Solid("blue_c", {0, 0, 30, 255}));
	doc.Nodes.push_back(backs);
	doc.Nodes[3].Values.push_back({"both_side", true});
	doc.Links.insert(
		doc.Links.end(),
		{{"blue_a", "surface_out", "backs", "a"},
		 {"blue_b", "surface_out", "backs", "b"},
		 {"blue_c", "surface_out", "backs", "c"},
		 {"backs", "array", "plane", "back_material"}}
	);
	// Source slots 0..7 have lengths [1,1,1,1,2,1,1,3]. Inverse uses the reversed full suffix table.
	const std::array<std::vector<std::pair<bool, uint8_t>>, 4> expected{
		{{{true, 10}, {false, 20}, {true, 30}},
		 {{true, 10}, {false, 20}, {false, 30}},
		 {{true, 10}, {true, 20}, {true, 30}, {false, 10}, {false, 20}, {false, 30}},
		 {{true, 10}, {true, 10}, {true, 10}, {true, 10}, {true, 10}, {true, 10}}}
	};
	for (int64_t mode = 0; mode < 4; ++mode) {
		doc.Nodes[3].Values.front().Data = EnumValue{mode};
		const Value result = Replay(doc);
		const auto &array = std::get<ArrayValue>(result);
		REQUIRE(array.Elements.size() == expected[size_t(mode)].size());
		for (size_t i = 0; i < array.Elements.size(); ++i) {
			const auto &mesh = *std::get<MeshValue3D>(array.Elements[i]).Data;
			REQUIRE(mesh.Materials.size() == 2);
			const auto [red, blue] = expected[size_t(mode)][i];
			CHECK(
				mesh.Materials[0].Get().Surface->Pixels ==
				(red ? std::vector<uint8_t>{255, 0, 0, 255} : std::vector<uint8_t>{0, 255, 0, 255})
			);
			CHECK((mesh.Materials[1].Get().Surface->Pixels == std::vector<uint8_t>{0, 0, blue, 255}));
		}
	}
}

TEST_CASE(
	"Boxed mesh descriptors leave scalar slots small and retain logical defaults", "[imagegraph][mesh_ops]"
) {
	CHECK(sizeof(MaterialValue3D) == sizeof(std::unique_ptr<MaterialData3D>));
	CHECK(sizeof(MeshValue3D) == sizeof(std::unique_ptr<MeshData3D>));
	CHECK(sizeof(Value) <= sizeof(Curve) + 2 * sizeof(void *));
	CHECK(sizeof(ElementValue) <= sizeof(Curve) + 2 * sizeof(void *));
	MaterialValue3D implicit, explicitDefault;
	explicitDefault.Edit();
	CHECK(implicit == explicitDefault);
	CHECK(detail::RetainedPayloadBytes(implicit) == 0);
	CHECK(detail::RetainedPayloadBytes(explicitDefault) == sizeof(MaterialData3D));
	CHECK((detail::RetainedPayloadBytes(MeshValue3D{}) == 0));
	const auto plane = DefaultPlane();
	CHECK(detail::RetainedPayloadBytes(plane) >= sizeof(MeshData3D));
}

TEST_CASE("Box assignment admits old and new backing before deep replacement", "[imagegraph][mesh_ops]") {
	MaterialValue3D material;
	material.Edit().Surface = MaterialSurface3D{1, 1, {11, 22, 33, 255}};
	material.Edit().Surface->Pixels.reserve(64);
	const auto replacement = [&]<class T>(const T &source, const T &old) {
		const uint64_t sourceBytes = detail::RetainedPayloadBytes(source);
		const uint64_t oldBytes = detail::RetainedPayloadBytes(old);
		const uint64_t peak = sourceBytes + oldBytes + sourceBytes;
		for (bool accepted : {false, true}) {
			T target = old;
			detail::EvaluationBudget ledger(peak - (accepted ? 0 : 1));
			auto inputCharge = ledger.Reserve(sourceBytes);
			auto oldCharge = ledger.Reserve(oldBytes);
			REQUIRE(inputCharge);
			REQUIRE(oldCharge);
			auto nextCharge = ledger.Reserve(sourceBytes);
			CHECK(bool(nextCharge) == accepted);
			if (nextCharge) {
				target = source;
				CHECK(target == source);
				REQUIRE(target.Data);
				REQUIRE(source.Data);
				CHECK(&*target.Data != &*source.Data);
				CHECK(ledger.Peak() == peak);
			} else {
				CHECK(target == old);
				CHECK(ledger.Used() == sourceBytes + oldBytes);
			}
		}
	};
	MaterialValue3D previous;
	previous.Edit().Diffuse = .5;
	replacement(material, previous);
	MeshValue3D source = DefaultPlane(), old = DefaultPlane();
	source.Data->Parts.front().Vertices.reserve(32);
	source.Data->Materials.front() = material;
	old.Data->LocalTransforms.front().Position = {7, 8, 9};
	replacement(source, old);
	MaterialValue3D copy = material;
	copy.Edit().Surface->Pixels.front() = 99;
	CHECK(material.Get().Surface->Pixels.front() == 11);
	MeshValue3D meshCopy = source;
	meshCopy.Data->Materials.front().Edit().Surface->Pixels.front() = 88;
	CHECK(source.Data->Materials.front().Get().Surface->Pixels.front() == 11);
}

TEST_CASE(
	"Material node creates source default texture and property descriptors", "[imagegraph][mesh_ops][surface]"
) {
	const auto run = RunNode("pc.3_d_material", {});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &data = std::get<MaterialValue3D>(*run.OutputValue("material")).Get();
	REQUIRE(data.Surface);
	CHECK(data.Surface->Width == 1);
	CHECK(data.Surface->Height == 1);
	CHECK((data.Surface->Pixels == std::vector<uint8_t>{0, 0, 0, 0}));
	CHECK((data.TextureScale == Vector2{1, 1}));
	CHECK((data.TextureShift == Vector2{}));
	CHECK(data.TextureFilter == 0);
	CHECK(data.Diffuse == 1);
	CHECK(data.Specular == 0);
	CHECK(data.Shininess == 1);
	CHECK_FALSE(data.Metal);
	CHECK(data.Reflectance == 0);
	CHECK(data.NormalStrength == 1);
	CHECK_FALSE(data.Normal);
	CHECK_FALSE(data.MetallicMapped);
	CHECK_FALSE(data.RoughnessMapped);
	CHECK((data.MetallicRange == Vector2{}));
	CHECK((data.RoughnessRange == Vector2{1, 1}));
	REQUIRE(data.PropertiesMap);
	CHECK(data.PropertiesMap->Width == 32);
	CHECK(data.PropertiesMap->Height == 32);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(*data.PropertiesMap, 31, 31, pixel));
	CHECK((pixel == SurfacePixel{0, 0, 0, 1}));
}

TEST_CASE(
	"Material preserves signed HDR textures normal maps and every authored descriptor",
	"[imagegraph][mesh_ops][surface]"
) {
	for (SurfaceFormat format : {SurfaceFormat::RGBA16Float, SurfaceFormat::RGBA32Float}) {
		const auto layout = CheckedSurfaceLayout(1, 1, format, Limits::MaximumArrayBytes);
		REQUIRE(layout);
		Image texture{1, 1, std::vector<uint8_t>(size_t(layout->Bytes), 0), 0, format};
		REQUIRE(StoreSurfacePixel(texture, 0, 0, {-2, 4, .25, 1}));
		texture.Hash = SurfaceHash(texture);
		Image normal = texture;
		REQUIRE(StoreSurfacePixel(normal, 0, 0, {.5, -.25, 2, 1}));
		normal.Hash = SurfaceHash(normal);
		for (int64_t shader : {0, 1, 2}) {
			const auto run = RunNode(
				"pc.3_d_material",
				{{"texture", &texture}, {"normal_map", &normal}},
				{{"shader", EnumValue{shader}},
				 {"interpolation", EnumValue{2}},
				 {"scale", Vector2{2, -3}},
				 {"shift", Vector2{.25, -.5}},
				 {"diffuse", .2},
				 {"specular", .7},
				 {"shininess", 19.},
				 {"metal", true},
				 {"reflectance", .6},
				 {"normal_strength", 1.5},
				 {"metalic_mapped", true},
				 {"metalic_map_range", Vector2{.1, .8}},
				 {"roughness_mapped", true},
				 {"roughness_map_range", Vector2{.2, .9}}}
			);
			INFO(run.Message);
			REQUIRE(run.Ok);
			const auto &material = std::get<MaterialValue3D>(*run.OutputValue("material"));
			const auto &data = material.Get();
			CHECK((data.Surface == std::optional<Image>{texture}));
			CHECK((data.Normal == std::optional<Image>{normal}));
			CHECK((data.TextureScale == Vector2{2, -3}));
			CHECK((data.TextureShift == Vector2{.25, -.5}));
			CHECK(data.TextureFilter == 2);
			CHECK(data.Diffuse == .2);
			CHECK(data.Specular == .7);
			CHECK(data.Shininess == 19);
			CHECK(data.Metal);
			CHECK(data.Reflectance == .6);
			CHECK(data.NormalStrength == 1.5);
			CHECK(data.MetallicMapped);
			CHECK(data.RoughnessMapped);
			CHECK((data.MetallicRange == Vector2{.1, .8}));
			CHECK((data.RoughnessRange == Vector2{.2, .9}));
			const auto plane = RunNode(PLANE, {}, {{"material", material}});
			REQUIRE(plane.Ok);
			const auto &mesh = std::get<MeshValue3D>(*plane.OutputValue("mesh"));
			CHECK(mesh.Data->Materials.front() == material);
			const auto wrapped = RunNode(TRANSFORM, {}, {{"mesh", mesh}});
			REQUIRE(wrapped.Ok);
			CHECK(std::get<MeshValue3D>(*wrapped.OutputValue("mesh")).Data->Materials.front() == material);
			SurfacePixel pixel;
			REQUIRE(LoadSurfacePixel(*mesh.Data->Materials.front().Get().Surface, 0, 0, pixel));
			CHECK((pixel == SurfacePixel{-2, 4, .25, 1}));
		}
	}
}

TEST_CASE(
	"Material equal-size property maps copy red and green without alpha attenuation",
	"[imagegraph][mesh_ops][surface]"
) {
	Image metallic{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
	Image roughness = metallic;
	for (uint32_t y = 0; y < 32; ++y)
		for (uint32_t x = 0; x < 32; ++x) {
			REQUIRE(StoreSurfacePixel(metallic, x, y, {double(x) / 31, .8, .7, .1}));
			REQUIRE(StoreSurfacePixel(roughness, x, y, {.9, double(y) / 31, .6, .2}));
		}
	const auto run = RunNode(
		"pc.3_d_material",
		{{"metalic_map", &metallic}, {"roughness_map", &roughness}},
		{{"metalic", .3}, {"roughness", .4}}
	);
	REQUIRE(run.Ok);
	const auto &data = std::get<MaterialValue3D>(*run.OutputValue("material")).Get();
	CHECK((data.MetallicRange == Vector2{.3, .3}));
	CHECK((data.RoughnessRange == Vector2{.4, .4}));
	REQUIRE(data.PropertiesMap);
	for (uint32_t y = 0; y < 32; ++y)
		for (uint32_t x = 0; x < 32; ++x) {
			const size_t offset = (size_t(y) * 32 + x) * 4;
			CHECK(data.PropertiesMap->Pixels[offset] == metallic.Pixels[offset]);
			CHECK(data.PropertiesMap->Pixels[offset + 1] == roughness.Pixels[offset + 1]);
			CHECK(data.PropertiesMap->Pixels[offset + 2] == 0);
			CHECK(data.PropertiesMap->Pixels[offset + 3] == 255);
		}
}

TEST_CASE("Material malformed float textures refuse before publication", "[imagegraph][mesh_ops][surface]") {
	Image nonfinite{1, 1, std::vector<uint8_t>(16, 0), 0, SurfaceFormat::RGBA32Float};
	nonfinite.Pixels[2] = 128;
	nonfinite.Pixels[3] = 127;
	for (std::string_view port : {"texture", "normal_map", "metalic_map", "roughness_map"}) {
		const auto invalid = RunNode("pc.3_d_material", {{port, &nonfinite}});
		CHECK_FALSE(invalid.Ok);
		CHECK(invalid.Code == Status::InvalidValue);
		CHECK(invalid.Port == port);
		CHECK(invalid.Values.empty());
	}
	MaterialValue3D invalid;
	invalid.Edit().Surface = nonfinite;
	CHECK_FALSE(detail::ValidMaterialPayload(invalid));
	const auto plane = RunNode(PLANE, {}, {{"material", invalid}});
	CHECK_FALSE(plane.Ok);
	CHECK(plane.Code == Status::InvalidValue);
	CHECK(plane.Port == "material");
}

TEST_CASE(
	"Material output admission covers prior live float capacity and owned backing",
	"[imagegraph][mesh_ops][surface]"
) {
	Image source{1, 1, std::vector<uint8_t>(16, 0), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(source, 0, 0, {-2, 4, .25, 1}));
	source.Pixels.reserve(128);
	const auto *entry = FindCatalogueEntry("pc.3_d_material");
	REQUIRE(entry);
	const Node node{"material", "pc.3_d_material", "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto evaluate = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		auto prior = ledger.Reserve(source.Pixels.capacity());
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, ledger);
			context.ByteBudget = maximum;
			// The same source at two ports still needs two independent admitted output copies.
			context.Images = {{"texture", &source}, {"normal_map", &source}};
			CHECK(detail::FindExecutor(node.Type)(context) == accepted);
			if (accepted) {
				REQUIRE(context.OutputValues.size() == 1);
				const auto &material = std::get<MaterialValue3D>(context.OutputValues.front().Data);
				REQUIRE(material.Data);
				CHECK((material.Get().Surface == std::optional<Image>{source}));
				CHECK((material.Get().Normal == std::optional<Image>{source}));
				CHECK(&*material.Get().Surface != &*material.Get().Normal);
				CHECK(detail::RetainedPayloadBytes(material) >= sizeof(MaterialData3D) + 16 * 2 + 4096);
				peak = ledger.Peak();
			} else {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			}
		}
		CHECK(ledger.Used() == prior->Bytes());
	};
	evaluate(Limits::MaximumEvaluationBytes, true);
	REQUIRE(peak > source.Pixels.capacity());
	evaluate(peak, true);
	evaluate(peak - 1, false);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(source, 0, 0, pixel));
	CHECK((pixel == SurfacePixel{-2, 4, .25, 1}));
}

TEST_CASE(
	"Material ordinary scalar rows persist with source array modes", "[imagegraph][mesh_ops][surface]"
) {
	for (int64_t mode : {0, 1, 2}) {
		Document doc;
		doc.FormatVersion = 7;
		Node rows{"rows", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		rows.DynamicInputs = {{"input_0", ValueType::Scalar, .1}, {"input_1", ValueType::Scalar, .9}};
		doc.Nodes = {
			rows, {"material", "pc.3_d_material", "", {}, {{"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {{"rows", "array", "material", "metalic"}};
		doc.Outputs = {{"out", "material", "material"}};
		const Value result = Replay(doc);
		const auto &materials = std::get<ArrayValue>(result);
		CHECK(materials.ElementType == ValueType::Material3D);
		REQUIRE(materials.Elements.size() == 2);
		CHECK((std::get<MaterialValue3D>(materials.Elements[0]).Get().MetallicRange == Vector2{.1, .1}));
		CHECK((std::get<MaterialValue3D>(materials.Elements[1]).Get().MetallicRange == Vector2{.9, .9}));
	}
}

TEST_CASE(
	"Material nearest property resizing replicates single-channel source "
	"shaders",
	"[imagegraph][mesh_ops][surface]"
) {
	for (SurfaceFormat format : {SurfaceFormat::R8Unorm, SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		const auto layout = CheckedSurfaceLayout(2, 1, format, Limits::MaximumArrayBytes);
		REQUIRE(layout);
		Image map{2, 1, std::vector<uint8_t>(size_t(layout->Bytes), 0), 0, format};
		REQUIRE(StoreSurfacePixel(map, 0, 0, {.25, 0, 0, 1}));
		REQUIRE(StoreSurfacePixel(map, 1, 0, {.75, 0, 0, 1}));
		for (int64_t interpolation : {0, 1, 2}) {
			const auto run = RunNode(
				"pc.3_d_material",
				{{"metalic_map", &map}, {"roughness_map", &map}},
				{{"interpolation", EnumValue{interpolation}}}
			);
			INFO(run.Message);
			REQUIRE(run.Ok);
			const auto &properties =
				*std::get<MaterialValue3D>(*run.OutputValue("material")).Get().PropertiesMap;
			CHECK(properties.Format == SurfaceFormat::RGBA8Unorm);
			CHECK(properties.Width == 32);
			CHECK(properties.Height == 32);
			for (uint32_t y = 0; y < 32; ++y)
				for (uint32_t x = 0; x < 32; ++x) {
					const size_t offset = (size_t(y) * 32 + x) * 4;
					CHECK(properties.Pixels[offset] == uint8_t(x < 16 ? 64 : 191));
					CHECK(properties.Pixels[offset + 1] == properties.Pixels[offset]);
					CHECK(properties.Pixels[offset + 2] == 0);
					CHECK(properties.Pixels[offset + 3] == 255);
				}
		}
	}
}

TEST_CASE(
	"Material RGBA maps preserve channels and quantize additive alpha "
	"after each pass",
	"[imagegraph][mesh_ops][surface]"
) {
	Image metal{3, 2, std::vector<uint8_t>(3 * 2 * 16, 0), 0, SurfaceFormat::RGBA32Float};
	Image rough{1, 1, std::vector<uint8_t>(16, 0), 0, SurfaceFormat::RGBA32Float};
	for (uint32_t y = 0; y < 2; ++y)
		for (uint32_t x = 0; x < 3; ++x)
			REQUIRE(StoreSurfacePixel(metal, x, y, {x == 0 ? -.5 : x == 1 ? .5 : 2., .9, .7, -.25}));
	REQUIRE(StoreSurfacePixel(rough, 0, 0, {.8, .25, .6, .125}));
	const auto run = RunNode("pc.3_d_material", {{"metalic_map", &metal}, {"roughness_map", &rough}});
	REQUIRE(run.Ok);
	const auto &properties = *std::get<MaterialValue3D>(*run.OutputValue("material")).Get().PropertiesMap;
	for (uint32_t y = 0; y < 32; ++y)
		for (uint32_t x = 0; x < 32; ++x) {
			const uint32_t sx = ((2 * x + 1) * 3) / 64;
			const size_t offset = (size_t(y) * 32 + x) * 4;
			CHECK(properties.Pixels[offset] == uint8_t(sx == 0 ? 0 : sx == 1 ? 128 : 255));
			CHECK(properties.Pixels[offset + 1] == 64);
			CHECK(properties.Pixels[offset + 2] == 0);
			CHECK(properties.Pixels[offset + 3] == 223);
		}
}

TEST_CASE(
	"Material mapped ranges use source depth1 and the exact16-slot array "
	"schedule",
	"[imagegraph][mesh_ops][surface]"
) {
	const std::array<Vector2, 2> metallic{Vector2{.1, .8}, Vector2{.2, .9}};
	const std::array<Vector2, 3> roughness{Vector2{.3, .6}, Vector2{.4, .7}, Vector2{.5, .8}};
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		Node textures{"textures", "value.array", "", {}, {}};
		textures.DynamicInputs = {
			{"red", ValueType::Image, std::nullopt}, {"green", ValueType::Image, std::nullopt}
		};
		doc.Nodes = {
			Solid("red", {255, 0, 0, 255}),
			Solid("green", {0, 255, 0, 255}),
			textures,
			{"material",
			 "pc.3_d_material",
			 "",
			 {},
			 {{"attribute_array_process", EnumValue{mode}},
			  {"metalic_mapped", true},
			  {"roughness_mapped", true},
			  {"metalic_map_range", ArrayValue{ValueType::Vector2, {metallic[0], metallic[1]}}},
			  {"roughness_map_range",
			   ArrayValue{ValueType::Vector2, {roughness[0], roughness[1], roughness[2]}}}}}
		};
		doc.Links = {
			{"red", "surface_out", "textures", "red"},
			{"green", "surface_out", "textures", "green"},
			{"textures", "array", "material", "texture"}
		};
		doc.Outputs = {{"out", "material", "material"}};
		const auto value = Replay(doc);
		const auto &rows = std::get<ArrayValue>(value);
		const size_t count = mode < 2 ? 3 : 12;
		REQUIRE(rows.Elements.size() == count);
		for (size_t i = 0; i < count; ++i) {
			const size_t t = mode == 0	 ? i % 2
							 : mode == 1 ? std::min(i, size_t(1))
							 : mode == 2 ? i / 6
										 : i % 2;
			const size_t m = mode == 0	 ? i % 2
							 : mode == 1 ? std::min(i, size_t(1))
							 : mode == 2 ? i % 2
										 : i / 6;
			const size_t r = mode < 2 ? i : (i / 2) % 3;
			const auto &data = std::get<MaterialValue3D>(rows.Elements[i]).Get();
			CHECK(data.MetallicRange == metallic[m]);
			CHECK(data.RoughnessRange == roughness[r]);
			REQUIRE(data.Surface);
			CHECK(
				(data.Surface->Pixels ==
				 std::vector<uint8_t>{uint8_t(t == 0 ? 255 : 0), uint8_t(t == 1 ? 255 : 0), 0, 255})
			);
		}
	}
	const auto *entry = FindCatalogueEntry("pc.3_d_material");
	REQUIRE(entry);
	const Node node{"material", "pc.3_d_material", "", {}, {}};
	const EvaluationRequest request;
	for (bool nested : {false, true}) {
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		ArrayValue raw{ValueType::Scalar, {.1, .8}};
		if (nested) {
			raw.Elements.clear();
			raw.Nested = {{.1, .8}, {.2, .9}};
		}
		context.Values = {
			{"metalic_mapped", true}, {"metalic", raw}, {"attribute_array_process", EnumValue{0}}
		};
		REQUIRE(detail::RunProcessorBatch(context, detail::FindExecutor(node.Type)));
		REQUIRE(context.OutputValues.size() == 1);
		if (nested) {
			const auto &rows = std::get<ArrayValue>(context.OutputValues.front().Data);
			REQUIRE(rows.Elements.size() == 2);
			CHECK(std::get<MaterialValue3D>(rows.Elements[0]).Get().MetallicRange == metallic[0]);
			CHECK(std::get<MaterialValue3D>(rows.Elements[1]).Get().MetallicRange == metallic[1]);
		} else
			CHECK(
				std::get<MaterialValue3D>(context.OutputValues.front().Data).Get().MetallicRange ==
				metallic[0]
			);
	}
}

TEST_CASE(
	"Persisted Material controls resize captured R maps and preserve HDR "
	"textures and normals",
	"[imagegraph][mesh_ops][surface]"
) {
	std::array<RequestImageSource, 4> sources{
		{{"texture", Image{1, 1, std::vector<uint8_t>(16, 0), 0, SurfaceFormat::RGBA32Float}},
		 {"normal", Image{2, 1, std::vector<uint8_t>(16, 0), 0, SurfaceFormat::RGBA16Float}},
		 {"metal", Image{2, 1, std::vector<uint8_t>(8, 0), 0, SurfaceFormat::R32Float}},
		 {"rough", Image{1, 3, std::vector<uint8_t>(24, 0), 0, SurfaceFormat::RGBA16Float}}}
	};
	REQUIRE(StoreSurfacePixel(sources[0].Data, 0, 0, {-2, 4, .25, 1}));
	for (uint32_t x = 0; x < 2; ++x) {
		REQUIRE(StoreSurfacePixel(sources[1].Data, x, 0, {double(x), -.5, 2, 1}));
		REQUIRE(StoreSurfacePixel(sources[2].Data, x, 0, {x == 0 ? .25 : .75, 0, 0, 1}));
	}
	for (uint32_t y = 0; y < 3; ++y)
		REQUIRE(StoreSurfacePixel(sources[3].Data, 0, y, {.9, .5 - double(y) * .25, .6, -.25}));
	for (auto &source : sources)
		source.Data.Hash = SurfaceHash(source.Data);
	Document doc;
	doc.FormatVersion = 9;
	for (const auto &source : sources)
		doc.Nodes.push_back({source.SourceId, "image.captured", "", {}, {{"source_id", source.SourceId}}});
	doc.Nodes.push_back(
		{"material",
		 "pc.3_d_material",
		 "",
		 {},
		 {{"interpolation", EnumValue{2}},
		  {"shader", EnumValue{2}},
		  {"scale", Vector2{2, -3}},
		  {"shift", Vector2{.1, .2}},
		  {"metalic_mapped", true},
		  {"roughness_mapped", true},
		  {"metalic_map_range", Vector2{.2, .9}},
		  {"roughness_map_range", Vector2{.3, .8}}}}
	);
	doc.Nodes.push_back({"plane", std::string(PLANE), "", {}, {}});
	doc.Links = {
		{"texture", "image", "material", "texture"},
		{"normal", "image", "material", "normal_map"},
		{"metal", "image", "material", "metalic_map"},
		{"rough", "image", "material", "roughness_map"},
		{"material", "material", "plane", "material"}
	};
	doc.Outputs = {{"out", "plane", "mesh"}};
	EvaluationRequest request;
	request.ImageSources = std::span<const RequestImageSource>(sources);
	const auto output = Replay(doc, "out", request);
	const auto &data = std::get<MeshValue3D>(output).Data->Materials.front().Get();
	CHECK(data.Surface == std::optional<Image>{sources[0].Data});
	CHECK(data.Normal == std::optional<Image>{sources[1].Data});
	CHECK((data.TextureScale == Vector2{2, -3}));
	CHECK((data.TextureShift == Vector2{.1, .2}));
	CHECK((data.MetallicRange == Vector2{.2, .9}));
	CHECK((data.RoughnessRange == Vector2{.3, .8}));
	CHECK(data.TextureFilter == 2);
	REQUIRE(data.PropertiesMap);
	for (uint32_t y = 0; y < 32; ++y)
		for (uint32_t x = 0; x < 32; ++x) {
			const uint32_t sy = ((2 * y + 1) * 3) / 64;
			const size_t offset = (size_t(y) * 32 + x) * 4;
			CHECK(data.PropertiesMap->Pixels[offset] == uint8_t(x < 16 ? 64 : 191));
			CHECK(data.PropertiesMap->Pixels[offset + 1] == uint8_t(sy == 0 ? 128 : sy == 1 ? 64 : 0));
			CHECK(data.PropertiesMap->Pixels[offset + 2] == 0);
			CHECK(data.PropertiesMap->Pixels[offset + 3] == 191);
		}
}

TEST_CASE(
	"Mapped material processor owns complete range rows within shared "
	"exact capacity",
	"[imagegraph][mesh_ops][surface]"
) {
	ArrayValue ranges{ValueType::Scalar, {}};
	ranges.Nested = {{.1, .8}, {.2, .9}};
	ranges.Nested.reserve(8);
	for (auto &row : ranges.Nested)
		row.reserve(8);
	const Value input = std::move(ranges);
	const auto *entry = FindCatalogueEntry("pc.3_d_material");
	REQUIRE(entry);
	const Node node{"material", "pc.3_d_material", "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto evaluate = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		auto prior = ledger.Reserve(detail::RetainedPayloadBytes(input));
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, ledger);
			context.ByteBudget = maximum;
			context.Values = {{"metalic_mapped", true}, {"attribute_array_process", EnumValue{0}}};
			context.ValueViews.emplace_back("metalic", &input);
			CHECK(detail::RunProcessorBatch(context, detail::FindExecutor(node.Type)) == accepted);
			if (accepted) {
				REQUIRE(context.OutputValues.size() == 1);
				const auto &rows = std::get<ArrayValue>(context.OutputValues.front().Data);
				REQUIRE(rows.Elements.size() == 2);
				CHECK((std::get<MaterialValue3D>(rows.Elements[0]).Get().MetallicRange == Vector2{.1, .8}));
				CHECK((std::get<MaterialValue3D>(rows.Elements[1]).Get().MetallicRange == Vector2{.2, .9}));
				peak = ledger.Peak();
			} else {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			}
		}
		CHECK(ledger.Used() == prior->Bytes());
	};
	evaluate(Limits::MaximumEvaluationBytes, true);
	REQUIRE(peak > 0);
	evaluate(peak, true);
	evaluate(peak - 1, false);
	const auto &unchanged = std::get<ArrayValue>(input);
	CHECK(unchanged.Nested.size() == 2);
	CHECK((unchanged.Nested[0] == std::vector<ElementValue>{.1, .8}));
}

TEST_CASE(
	"Material mapped range authoring accepts endpoint rows only on its exact projection", "[imagegraph][mesh]"
) {
	const auto *entry = FindCatalogueEntry("pc.3_d_material");
	REQUIRE(entry);
	for (const auto port : {"metalic_map_range", "roughness_map_range"}) {
		const auto *input = FindCatalogueInput(*entry, port);
		REQUIRE(input);
		const ArrayValue rows{ValueType::Vector2, {Vector2{-.5, 2}, Vector2{.25, 1}}};
		CHECK(CatalogueAuthoredArray(*entry, *input, rows));
		CHECK(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Scalar, {-.5, 2.0}}));
		CHECK(
			CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Integer, {int64_t{-1}, int64_t{2}}})
		);
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Scalar, {0.0}}));
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Scalar, {0.0, 1.0, 2.0}}));
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Boolean, {false, true}}));
		CHECK_FALSE(CatalogueAuthoredArray(
			*entry,
			*input,
			ArrayValue{ValueType::Vector2, {Vector2{0, std::numeric_limits<double>::infinity()}}}
		));
		auto wrongKind = *input;
		wrongKind.SourceKind = "Vec2";
		CHECK_FALSE(CatalogueAuthoredArray(*entry, wrongKind, rows));
		auto wrongNode = *entry;
		wrongNode.Type = "pc.3_d_mesh_plane";
		CHECK_FALSE(CatalogueAuthoredArray(wrongNode, *input, rows));
		ArrayValue nested{ValueType::Scalar, {}};
		nested.Nested = {{0.0, 1.0}, {0.25, 2.0}};
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, nested));
	}
}

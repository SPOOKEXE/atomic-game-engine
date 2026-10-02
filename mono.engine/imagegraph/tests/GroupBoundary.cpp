#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.group_boundary")
using namespace engine::imagegraph;
namespace {
	Document Boundary(Value raw, int64_t type) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"input",
			 "pc.group_input",
			 "group",
			 {},
			 {{"input_type", EnumValue{type}}, {"subtype", EnumValue{0}}, {"vector_size", EnumValue{0}}}},
			{"output", "pc.group_output", "group", {}, {}}
		};
		Group group{"group", "Group"};
		group.Ports = {
			{"input", "input/parent-value", PortDirection::Input, "input"},
			{"output", "output/parent-value", PortDirection::Output, "output"}
		};
		document.Groups.push_back(std::move(group));
		document.Junctions = {
			{"input/parent-value", "group", ValueType::Any, std::move(raw)},
			{"output/parent-value", "group", ValueType::Any, std::nullopt}
		};
		document.Links = {
			{"input/parent-value", "value", "input", "parent_value"},
			{"input", "value", "output", "value"},
			{"output", "value", "output/parent-value", "value"}
		};
		document.Outputs = {{"result", "output", "value"}};
		return document;
	}
	EvaluatedValue Sample(const Document &document, EvaluationRequest request = {}) {
		Plan plan;
		Diagnostic diagnostic;
		{
			const auto status = Compile(document, plan, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
		}
		EvaluatedValue value;
		{
			const auto status = EvaluateValue(document, plan, "result", request, value, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
		}
		return value;
	}
}
TEST_CASE(
	"Persisted group raw numeric transport defers source getters", "[imagegraph][groups][group_boundary]"
) {
	for (double raw : {.5, .500001}) {
		const auto authored = Boundary(raw, 2);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(authored), restored, diagnostic) == Status::Ok);
		CHECK(restored == authored);
		CHECK(std::get<double>(*restored.Junctions.front().Default) == raw);
		const auto boundaryResult = Sample(restored);
		CHECK(std::get<double>(boundaryResult.Data) == raw);
		REQUIRE(boundaryResult.Domain.has_value());
		CHECK(boundaryResult.Domain->Type == ValueType::Boolean);
		CHECK(boundaryResult.Domain->Kind == SourceSocketKind::Boolean);
		CHECK(boundaryResult.Domain->Display == SourceValueDisplay::Default);
		restored.Nodes.push_back({"consumer", "pc.number", "group", {}, {{"value", 10.7}}});
		restored.Links.push_back({"input", "value", "consumer", "integer"});
		restored.Outputs.front() = {"result", "consumer", "number"};
		CHECK(std::get<double>(Sample(restored).Data) == (raw > .5 ? 11.0 : 10.7));
		CHECK(std::get<double>(*restored.Junctions.front().Default) == raw);
	}
	for (double raw : {2.5, 3.5}) {
		auto document = Boundary(raw, 0);
		const auto boundaryResult = Sample(document);
		CHECK(std::get<double>(boundaryResult.Data) == raw);
		REQUIRE(boundaryResult.Domain.has_value());
		CHECK(boundaryResult.Domain->Type == ValueType::Integer);
		CHECK(boundaryResult.Domain->Kind == SourceSocketKind::Integer);
		document.Nodes.push_back(
			{"consumer",
			 "pc.string_get_char",
			 "group",
			 {},
			 {{"text", std::string("ABCD")}, {"amount", int64_t(1)}}}
		);
		document.Links.push_back({"input", "value", "consumer", "index"});
		document.Outputs.front() = {"result", "consumer", "text"};
		CHECK(std::get<std::string>(Sample(document).Data) == (raw == 2.5 ? "B" : "D"));
	}
}
TEST_CASE(
	"Group domain controls resolve linked signed timeline values", "[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(.500001, 2);
	document.Nodes.push_back({"type", "pc.number_simple", "group", {}, {{"value", 2.0}}});
	document.Links.push_back({"type", "number", "input", "input_type"});
	Keyframe negative{"type", "value", 1, 2.0, "step"};
	REQUIRE(SetFrameTime(negative, {1, .5, true}));
	Keyframe positive{"type", "value", 1, 1.0, "step"};
	REQUIRE(SetFrameTime(positive, {1, .25, false}));
	document.Keyframes = {negative, positive};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {1, .5, true}));
	CHECK(std::get<double>(Sample(restored, request).Data) == .500001);
	Plan plan;
	{
		const auto status = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(restored, plan, "input", request, snapshot, diagnostic) == Status::Ok);
	auto control = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
		return value.Port == "input_type";
	});
	REQUIRE(control != snapshot.Values().end());
	CHECK(std::get<double>(control->Data) == 2.0);
	REQUIRE(SetFrameTime(request, {1, .25, false}));
	CHECK(std::get<double>(Sample(restored, request).Data) == .500001);
	REQUIRE(EvaluateNodeInputs(restored, plan, "input", request, snapshot, diagnostic) == Status::Ok);
	control = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
		return value.Port == "input_type";
	});
	REQUIRE(control != snapshot.Values().end());
	CHECK(std::get<double>(control->Data) == 1.0);
	CHECK(restored == document);
}
TEST_CASE(
	"Unsupported boundary resource keeps last successful result atomic",
	"[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(.500001, 2);
	EvaluatedValue previous = Sample(document);
	const EvaluatedValue before = previous;
	document.Nodes.front().Values.front().Data = EnumValue{8};
	Plan plan;
	Diagnostic diagnostic;
	{
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	CHECK(EvaluateValue(document, plan, "result", {}, previous, diagnostic) == Status::UnsupportedExecution);
	CHECK(previous == before);
	CHECK(diagnostic.NodeId == "input");
	CHECK(diagnostic.Port == "input_type");
}
TEST_CASE(
	"Dynamic boundary identity rejects a foreign control atomically", "[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(.500001, 2);
	Plan plan;
	Diagnostic diagnostic;
	{
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	const Plan before = plan;
	document.Groups.front().Ports.front().ControlNodeId = "output";
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidGroup);
	CHECK(plan == before);
}

TEST_CASE(
	"Instance clone payload admission includes fixed and owned values without scalar underflow",
	"[imagegraph][groups][group_boundary]"
) {
	CHECK(ValueClonePayloadBytes(Value{.5}) == sizeof(Value));
	Node scalar{"id", "pc.number_simple", "", {}, {{"value", .5}}};
	const auto scalarBytes = NodeClonePayloadBytes(scalar);
	REQUIRE(scalarBytes);
	CHECK(*scalarBytes >= sizeof(Node) + sizeof(AuthoredValue));
	CHECK(*scalarBytes < 1024);
	scalar.DynamicInputs.push_back({"text", ValueType::Text, std::string(128, 'x')});
	const auto textBytes = NodeClonePayloadBytes(scalar);
	REQUIRE(textBytes);
	CHECK(*textBytes >= *scalarBytes + sizeof(DynamicInput) + 128);
	scalar.DynamicInputs.back().Default = std::numeric_limits<double>::infinity();
	CHECK_FALSE(NodeClonePayloadBytes(scalar));
}

TEST_CASE(
	"Node instances sample the persisted base animator without copying authored keys",
	"[imagegraph][groups][group_boundary]"
) {
	Document authored;
	authored.FormatVersion = 9;
	authored.Groups = {{"base", "Base"}, {"instance", "Instance", "", {}, 1, "base"}};
	authored.Nodes = {
		{"original", "pc.number_simple", "base", {}, {{"value", 3.0}}},
		{"copy", "pc.number_simple", "instance", {}, {{"value", 99.0}}, {}, "original"}
	};
	authored.Outputs = {{"result", "copy", "number"}};
	authored.Keyframes = {{"original", "value", 0, 3.0, "linear"}, {"original", "value", 2, 7.0, "linear"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(authored), restored, diagnostic) == Status::Ok);
	CHECK(restored == authored);
	CHECK(std::get<double>(Sample(restored, {.Tick = 1}).Data) == 5.0);
	restored.Keyframes.back().Data = 11.0;
	CHECK(std::get<double>(Sample(restored, {.Tick = 1}).Data) == 7.0);
	CHECK(restored.Nodes.back().Values.front().Data == Value{99.0});
	CHECK(restored.Keyframes.size() == 2);
	restored.Nodes.back().InstanceOverrides = {"value"};
	Document overridden;
	REQUIRE(Read(Write(restored), overridden, diagnostic) == Status::Ok);
	CHECK(std::get<double>(Sample(overridden, {.Tick = 1}).Data) == 99.0);
	CHECK(overridden == restored);
}
TEST_CASE(
	"Group surface forwarding preserves a prior live bounded snapshot on refusal",
	"[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(-4.0, 11);
	document.Nodes.push_back(
		{"surface",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(8)}, {"height", int64_t(2)}, {"colour", Colour{12, 34, 56, 255}}}}
	);
	document.Junctions.front().Default.reset();
	document.Links.push_back({"surface", "image", "input/parent-value", "value"});
	Plan plan;
	Diagnostic diagnostic;
	{
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "output", {}, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 1);
	const auto bytes = snapshot.RetainedBytes();
	const auto pixels = snapshot.Images().front().Data.Pixels;
	CHECK(
		EvaluateNodeInputs(document, plan, "output", {}, snapshot, diagnostic, bytes) == Status::LimitExceeded
	);
	CHECK(snapshot.RetainedBytes() == bytes);
	CHECK(snapshot.Images().front().Data.Pixels == pixels);
	CHECK(document.Nodes.back().Values.front().Data == Value{int64_t(8)});
}

TEST_CASE(
	"Persisted group sampling overrides project settings on an actual surface route",
	"[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(-4.0, 11);
	document.Project = ProjectSettings{};
	document.Project->Interpolation = 0;
	document.Project->Oversample = 3;
	document.Groups.front().Interpolation = 1;
	document.Groups.front().Oversample = 1;
	document.Nodes.push_back(
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(2)}, {"height", int64_t(2)}, {"colour", Colour{20, 40, 80, 255}}}}
	);
	document.Nodes.push_back(
		{"skew",
		 "pc.skew",
		 "group",
		 {},
		 {{"strength", 1.0},
		  {"center", Vector2{0, 0}},
		  {"center_unit", EnumValue{0}},
		  {"axis", EnumValue{0}},
		  {"attribute_color_depth", EnumValue{3}}}}
	);
	document.Junctions.front().Default.reset();
	document.Links = {
		{"source", "image", "input/parent-value", "value"},
		{"input/parent-value", "value", "input", "parent_value"},
		{"input", "value", "skew", "surface_in"},
		{"skew", "surface_out", "output", "value"},
		{"output", "value", "output/parent-value", "value"}
	};
	const auto alpha = [&](const Document &authored) {
		Document persisted;
		Diagnostic diagnostic;
		Plan plan;
		Image image;
		REQUIRE(Read(Write(authored), persisted, diagnostic) == Status::Ok);
		REQUIRE(persisted == authored);
		{
			const auto status = Compile(persisted, plan, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
		}
		{
			const auto status = Evaluate(persisted, plan, "result", image, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
		}
		SurfacePixel pixel;
		REQUIRE(LoadSurfacePixel(image, 1, 1, pixel));
		return pixel[3];
	};
	CHECK(alpha(document) == 0.0);
	document.Groups.front().Oversample = 4;
	CHECK(alpha(document) == 1.0);
	document.Groups.front().Oversample = 0;
	CHECK(alpha(document) == 1.0);
}
TEST_CASE(
	"Generic group parent processing preserves numeric colour and wraps scalar palettes",
	"[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(Colour{1, 2, 3, 255}, 3);
	document.Nodes.front().Values[1].Data = EnumValue{1};
	const auto palette = Sample(document);
	REQUIRE(std::holds_alternative<ArrayValue>(palette.Data));
	REQUIRE(std::get<ArrayValue>(palette.Data).Elements.size() == 1);
	CHECK(std::get<Colour>(std::get<ArrayValue>(palette.Data).Elements[0]) == Colour{1, 2, 3, 255});
	document = Boundary(0.0, 3);
	document.Nodes.push_back({"number", "pc.number_simple", "group", {}, {{"value", .75}}});
	document.Links.erase(document.Links.begin());
	document.Links.push_back({"number", "number", "input", "parent_value"});
	CHECK(std::get<double>(Sample(document).Data) == .75);
}
TEST_CASE(
	"Generic group parent converts a declared colour palette into source gradient positions",
	"[imagegraph][groups][group_boundary]"
) {
	ArrayValue colours;
	colours.ElementType = ValueType::Colour;
	colours.Elements = {Colour{255, 0, 0, 255}, Colour{0, 0, 255, 255}};
	auto document = Boundary(Gradient{0, {{0, Colour{255, 255, 255, 255}}}}, 31);
	document.Nodes.push_back({"colours", "pc.color", "group", {}, {{"color", colours}}});
	document.Links.erase(document.Links.begin());
	document.Links.push_back({"colours", "color", "input", "parent_value"});
	const auto result = Sample(document);
	REQUIRE(std::holds_alternative<Gradient>(result.Data));
	const auto &gradient = std::get<Gradient>(result.Data);
	REQUIRE(gradient.Keys.size() == 2);
	CHECK(gradient.Keys[0].Time == 0);
	CHECK(gradient.Keys[1].Time == .5);
	CHECK(gradient.Keys[0].Color == Colour{255, 0, 0, 255});
	CHECK(gradient.Keys[1].Color == Colour{0, 0, 255, 255});
}

TEST_CASE(
	"Empty source group animators persist mode independently of keys", "[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(1.0, 3);
	document.Nodes.front().SourceAnimatedInputs = {"subtype"};
	Document restored;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK(restored.Keyframes.empty());
	REQUIRE(Migrate(restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	restored.Nodes.front().SourceAnimatedInputs.push_back("subtype");
	CHECK(Compile(restored, plan, diagnostic) == Status::InvalidValue);
	CHECK(Migrate(restored, diagnostic) == Status::InvalidValue);
	restored.Nodes.front().SourceAnimatedInputs = {"unknown"};
	CHECK(Compile(restored, plan, diagnostic) == Status::InvalidValue);
	CHECK(Migrate(restored, diagnostic) == Status::InvalidValue);
	restored = document;
	restored.FormatVersion = 8;
	CHECK(Migrate(restored, diagnostic) == Status::InvalidValue);
	CHECK(Read(Write(document) + "source_anim \"input\" \"subtype\"\n", restored, diagnostic) != Status::Ok);
}
TEST_CASE(
	"Source animator clone estimates include retained long port capacity",
	"[imagegraph][groups][group_boundary]"
) {
	Node node{"long", "pc.group_input", {}, {}, {}};
	node.SourceAnimatedInputs.emplace_back(4096, 'p');
	node.SourceAnimatedInputs.front().reserve(8192);
	node.SourceStaticInputs.emplace_back(4096, 's');
	node.SourceStaticInputs.front().reserve(8192);
	const auto clone = NodeClonePayloadBytes(node);
	REQUIRE(clone);
	CHECK(*clone >= sizeof(Node) + 2 * sizeof(std::string) + 8192);
	Document document;
	document.Nodes.push_back(node);
	document.Nodes.front().SourceAnimatedInputs.front().reserve(8192);
	document.Nodes.front().SourceStaticInputs.front().reserve(8192);
	const auto retained = DocumentRetainedPayloadBytes(document);
	REQUIRE(retained);
	CHECK(*retained >= sizeof(Document) + sizeof(Node) + 2 * sizeof(std::string) + 16384);
}

TEST_CASE("source expression clone accounting includes code and vector storage", "[imagegraph][groups]") {
	Node node{"expression", "pc.number_simple", {}, {}, {}};
	node.SourceInputExpressions.push_back({"value", "answer", true});
	node.SourceInputExpressions.back().Port.reserve(128);
	node.SourceInputExpressions.back().Code.reserve(512);
	const auto clone = NodeClonePayloadBytes(node);
	REQUIRE(clone);
	const Node copied = node;
	CHECK(
		*clone >= sizeof(Node) + sizeof(SourceInputExpression) +
					  copied.SourceInputExpressions[0].Port.capacity() +
					  copied.SourceInputExpressions[0].Code.capacity()
	);

	Document document;
	document.Nodes.push_back(node);
	document.Nodes.front().SourceInputExpressions.front().Port.reserve(128);
	document.Nodes.front().SourceInputExpressions.front().Code.reserve(512);
	const auto retained = DocumentRetainedPayloadBytes(document);
	REQUIRE(retained);
	CHECK(*retained >= sizeof(Document) + sizeof(Node) + sizeof(SourceInputExpression) + 128 + 512);
}

TEST_CASE(
	"Dynamic output clone accounting includes durable socket storage", "[imagegraph][groups][group_boundary]"
) {
	Node node{"split", "pc.array_split", {}, {}, {}};
	node.DynamicOutputs = {{std::string(4096, 'p'), ValueType::Any}};
	const auto clone = NodeClonePayloadBytes(node);
	REQUIRE(clone);
	CHECK(*clone >= sizeof(Node) + sizeof(DynamicOutput) + 4096);
	Document document;
	document.Nodes.push_back(node);
	document.Nodes[0].DynamicOutputs[0].Id.reserve(8192);
	const auto retained = DocumentRetainedPayloadBytes(document);
	REQUIRE(retained);
	CHECK(*retained >= sizeof(Document) + sizeof(Node) + sizeof(DynamicOutput) + 8192);
	node.DynamicOutputs.resize(Limits::MaximumDynamicOutputsPerNode + 1);
	CHECK_FALSE(NodeClonePayloadBytes(node));
}

TEST_CASE(
	"Declared empty source animators preserve end settings without a timeline",
	"[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(1.0, 3);
	document.Nodes.front().SourceAnimatedInputs = {"subtype"};
	document.Tracks = {{"input", "subtype", "wrap", 7}};
	Document restored;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	restored.Nodes.front().SourceAnimatedInputs.clear();
	CHECK(Compile(restored, plan, diagnostic) == Status::InvalidValue);
	restored = document;
	restored.Tracks.front().Port = "parent_value";
	CHECK(Compile(restored, plan, diagnostic) == Status::InvalidValue);
	restored = document;
	restored.Tracks.front().End = "unknown";
	CHECK(Compile(restored, plan, diagnostic) == Status::InvalidValue);
}

TEST_CASE(
	"Empty Group range storage requires matching durable animator declaration",
	"[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(1.0, 0);
	document.Nodes.front().Values.push_back({"range", ArrayValue{ValueType::Scalar, {}}});
	document.Nodes.front().SourceAnimatedInputs = {"range"};
	document.Tracks = {{"input", "range", "hold", -1}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "input", {}, snapshot, diagnostic) == Status::Ok);
	const auto range =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "range";
		});
	REQUIRE(range != snapshot.Values().end());
	CHECK(std::get<Vector2>(range->Data) == Vector2{0, 0});
	std::vector<AuthoredValue> resolved;
	REQUIRE(ResolveNodeValues(document, plan, "result", "input", {}, resolved, diagnostic) == Status::Ok);
	const auto resolvedRange = std::find_if(resolved.begin(), resolved.end(), [](const auto &value) {
		return value.Port == "range";
	});
	REQUIRE(resolvedRange != resolved.end());
	CHECK(std::get<Vector2>(resolvedRange->Data) == Vector2{0, 0});
	CHECK(std::get<ArrayValue>(document.Nodes.front().Values.back().Data).Elements.empty());
	const auto originalDocument = document;
	auto malformed = document;
	auto &rawRange = std::get<ArrayValue>(malformed.Nodes.front().Values.back().Data);
	rawRange.Items = {SourceArrayItem{ElementValue{int64_t{7}}}};
	CHECK(Compile(malformed, plan, diagnostic) == Status::LimitExceeded);
	CHECK(document == originalDocument);
	document.Nodes.front().SourceAnimatedInputs.clear();
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Nodes.front().SourceAnimatedInputs = {"range"};
	document.Tracks.clear();
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
}

TEST_CASE(
	"Static empty Group vector zero is guarded against nonzero and keyed scalar storage",
	"[imagegraph][groups][group_boundary]"
) {
	auto document = Boundary(1.0, 0);
	document.Nodes.front().Values.push_back({"range", 0.0});
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	document.Nodes.front().Values.back().Data = 0.5;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	document.Nodes.front().Values.back().Data = 0.0;
	document.Keyframes.push_back({"input", "range", 0, Vector2{1, 2}, "step"});
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
}

TEST_CASE(
	"Static shared quaternion getters process raw keys with receiving local metadata",
	"[imagegraph][group_bootstrap]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"owner", "pc.quarternion_to_euler", "", {}, {}},
		{"middle", "pc.quarternion_to_euler", "", {}, {}},
		{"leaf", "pc.quarternion_to_euler", "", {}, {}}
	};
	document.Nodes[1].InstanceBase = "owner";
	document.Nodes[1].InstanceOverrides = {"rotation"};
	document.Nodes[2].InstanceBase = "middle";
	for (auto &node : document.Nodes)
		node.SourceStaticInputs = {"rotation"};
	Keyframe key{"owner", "rotation", 0, Quaternion{90, 0, 0, 0}, "source"};
	key.Ease = KeyframeEase{"linear", "linear", {0, 1}, {0, 0}};
	document.Keyframes = {key};
	document.Tracks = {
		{"owner", "rotation", "hold", -1},
		{"middle", "rotation", "hold", -1},
		{"leaf", "rotation", "hold", -1}
	};
	document.Tracks[0].QuaternionMode = 1;
	document.Tracks[1].QuaternionMode = 0;
	document.Tracks[2].QuaternionMode = 1;
	document.Outputs = {
		{"owner-result", "owner", "euler_angles"},
		{"middle-result", "middle", "euler_angles"},
		{"leaf-result", "leaf", "euler_angles"}
	};
	Diagnostic diagnostic;
	Plan plan;
	const auto diagnosticStatus1 = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(diagnosticStatus1 == Status::Ok);
	const std::array<GroupSubtypeBinding, 2> bindings = {
		{{"middle", "owner", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "rotation"},
		 {"leaf", "owner", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "rotation"}}
	};
	GroupReplayState empty, local, bound;
	const auto diagnosticStatus2 = RebindGroupReplay(document, empty, 1, local, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(diagnosticStatus2 == Status::Ok);
	const auto diagnosticStatus3 = BindGroupReplay(document, bindings, local, 1, bound, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(diagnosticStatus3 == Status::Ok);
	EvaluationRequest clock;
	clock.Tick = 3;
	clock.Subframe = .5;
	clock.NegativeFrame = true;
	clock.GroupReplay = &bound;
	clock.GroupAuthoringRevision = 1;
	Quaternion expected;
	REQUIRE(ConvertSourceQuaternion(Quaternion{90, 0, 0, 0}, 1, expected));
	for (const auto id : {"owner", "middle", "leaf"}) {
		EvaluationSnapshot snapshot;
		const auto diagnosticStatus4 = EvaluateNodeInputs(document, plan, id, clock, snapshot, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus4 == Status::Ok);
		const auto input =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "rotation";
			});
		REQUIRE(input != snapshot.Values().end());
		CHECK(input->Data == Value{std::string_view(id) == "middle" ? Quaternion{90, 0, 0, 0} : expected});
	}
	CHECK(document.Keyframes.front().Data == Value{Quaternion{90, 0, 0, 0}});
	Value raw = Quaternion{0, 90, 0, 0};
	GroupRefreshEvent edit;
	edit.NodeId = "leaf";
	edit.Reason = GroupRefreshReason::Edit;
	edit.EditedPort = "rotation";
	edit.LocalValue = &raw;
	edit.At = clock;
	GroupReplayState changed;
	REQUIRE(
		ReplayGroupAnimatorEdits(document, std::span(&edit, 1), bound, 1, changed, diagnostic) == Status::Ok
	);
	Document projected;
	REQUIRE(ProjectGroupReplay(document, changed, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Keyframes.front().Data == Value{Quaternion{0, 90, 0, 0}});
	CHECK(projected.Keyframes.front().Ease == key.Ease);
	const auto diagnosticStatus5 = Compile(projected, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(diagnosticStatus5 == Status::Ok);
	clock.GroupReplay = nullptr;
	EvaluationSnapshot snapshot;
	const auto diagnosticStatus6 = EvaluateNodeInputs(projected, plan, "owner", clock, snapshot, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(diagnosticStatus6 == Status::Ok);
	REQUIRE(ConvertSourceQuaternion(Quaternion{0, 90, 0, 0}, 1, expected));
	const auto input =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "rotation";
		});
	REQUIRE(input != snapshot.Values().end());
	CHECK(input->Data == Value{expected});
}

TEST_CASE(
	"Animated quaternion aliases keep owner interpolation and receiving local getter metadata",
	"[imagegraph][group_bootstrap]"
) {
	for (const int scenario : {0, 1, 2}) {
		const bool writerAnimated = scenario != 1;
		const int64_t ownerMode = scenario == 2 ? 0 : 1;
		const int64_t receiverMode = scenario == 2 ? 1 : 0;
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"owner", "pc.quarternion_to_euler", "", {}, {}},
			{"target", "pc.quarternion_to_euler", "", {}, {}},
			{"leaf", "pc.quarternion_to_euler", "", {}, {}},
			{"static", "pc.quarternion_to_euler", "", {}, {}}
		};
		document.Nodes[1].InstanceBase = "owner";
		document.Nodes[1].InstanceOverrides = {"rotation"};
		document.Nodes[2].InstanceBase = "target";
		document.Nodes[3].InstanceBase = "owner";
		document.Nodes[3].InstanceOverrides = {"rotation"};
		if (writerAnimated)
			document.Nodes[0].SourceAnimatedInputs = {"rotation"};
		else
			document.Nodes[0].SourceStaticInputs = {"rotation"};
		document.Nodes[1].SourceAnimatedInputs = {"rotation"};
		document.Nodes[2].SourceAnimatedInputs = {"rotation"};
		document.Nodes[3].SourceStaticInputs = {"rotation"};
		const Quaternion first = scenario == 1 ? Quaternion{90, 0, 0, 0} : Quaternion{0, 0, 0, 1};
		Keyframe key{"owner", "rotation", 0, first, "source"};
		key.Ease = KeyframeEase{"linear", "linear", {0, 1}, {0, 0}};
		if (scenario == 1) key.SourceDriver = KeyframeLinearDriver{1};
		document.Keyframes = {key};
		if (writerAnimated) {
			auto last = key;
			last.Tick = 10;
			last.Data = scenario == 2 ? Quaternion{0, 0, 1, 0} : Quaternion{0, 0, 90, 0};
			document.Keyframes.push_back(last);
		}
		document.Tracks = {
			{"owner", "rotation", "hold", -1},
			{"target", "rotation", "hold", -1},
			{"leaf", "rotation", "hold", -1},
			{"static", "rotation", "hold", -1}
		};
		document.Tracks[0].QuaternionMode = ownerMode;
		document.Tracks[1].QuaternionMode = receiverMode;
		document.Tracks[2].QuaternionMode = ownerMode;
		document.Tracks[3].QuaternionMode = ownerMode;
		for (const auto &node : document.Nodes)
			document.Outputs.push_back({node.Id, node.Id, "euler_angles"});
		const auto originalKeys = document.Keyframes;
		const auto originalTracks = document.Tracks;
		const GroupSubtypeAnimator writer =
			writerAnimated ? GroupSubtypeAnimator::Animated : GroupSubtypeAnimator::Static;
		const std::array<GroupSubtypeBinding, 3> bindings = {
			{{"target", "owner", GroupSubtypeAnimator::Animated, writer, "rotation"},
			 {"leaf", "owner", GroupSubtypeAnimator::Animated, writer, "rotation"},
			 {"static", "owner", GroupSubtypeAnimator::Static, writer, "rotation"}}
		};
		Diagnostic diagnostic;
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		Plan plan;
		const auto diagnosticStatus101 = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus101 == Status::Ok);
		GroupReplayState empty, local, bound;
		const auto diagnosticStatus102 = RebindGroupReplay(restored, empty, 1, local, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus102 == Status::Ok);
		const auto diagnosticStatus103 = BindGroupReplay(restored, bindings, local, 1, bound, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus103 == Status::Ok);
		EvaluationRequest clock;
		clock.Tick = scenario == 1 ? 2 : 5;
		clock.Subframe = scenario == 1 ? .5 : 0;
		clock.GroupReplay = &bound;
		clock.GroupAuthoringRevision = 1;
		const Quaternion rawSample = scenario == 0	 ? Quaternion{0, 0, 45, .5}
									 : scenario == 1 ? Quaternion{92.5, 2.5, 2.5, 2.5}
													 : Quaternion{0, 0, std::sqrt(.5), std::sqrt(.5)};
		const auto check = [&](const Value &value, const Quaternion &expected) {
			REQUIRE(std::holds_alternative<Quaternion>(value));
			const auto tuple = std::get<Quaternion>(value);
			CHECK(tuple.X == Catch::Approx(expected.X).margin(1e-12));
			CHECK(tuple.Y == Catch::Approx(expected.Y).margin(1e-12));
			CHECK(tuple.Z == Catch::Approx(expected.Z).margin(1e-12));
			CHECK(tuple.W == Catch::Approx(expected.W).margin(1e-12));
		};
		for (const auto id : {"owner", "target", "leaf", "static"}) {
			const bool own = std::string_view(id) == "owner";
			const bool staticGetter = std::string_view(id) == "static" || (own && !writerAnimated);
			const bool leaf = std::string_view(id) == "leaf";
			Quaternion expected;
			REQUIRE(ConvertSourceQuaternion(
				staticGetter ? first : rawSample,
				own || staticGetter || leaf ? ownerMode : receiverMode,
				expected
			));
			EvaluationSnapshot snapshot;
			const auto diagnosticStatus104 =
				EvaluateNodeInputs(restored, plan, id, clock, snapshot, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(diagnosticStatus104 == Status::Ok);
			const auto input =
				std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
					return value.Port == "rotation";
				});
			REQUIRE(input != snapshot.Values().end());
			check(input->Data, expected);
			std::vector<AuthoredValue> values;
			REQUIRE(ResolveNodeValues(restored, plan, id, id, clock, values, diagnostic) == Status::Ok);
			const auto authored = std::find_if(values.begin(), values.end(), [](const auto &value) {
				return value.Port == "rotation";
			});
			REQUIRE(authored != values.end());
			check(authored->Data, expected);
		}
		CHECK(restored.Keyframes == originalKeys);
		CHECK(restored.Tracks == originalTracks);
	}
}

TEST_CASE(
	"Linked quaternion aliases process producer data with receiving local metadata",
	"[imagegraph][group_bootstrap]"
) {
	for (const int64_t mode : {0, 1}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"producer", "pc.quarternion_from_euler", "", {}, {{"euler_rotation", Vector3{90, 0, 0}}}},
			{"owner", "pc.quarternion_to_euler", "", {}, {}},
			{"target", "pc.quarternion_to_euler", "", {}, {}}
		};
		document.Nodes[1].SourceStaticInputs = {"rotation"};
		document.Nodes[2].SourceStaticInputs = {"rotation"};
		document.Nodes[2].InstanceBase = "owner";
		Keyframe abandoned{"owner", "rotation", 0, Quaternion{0, 0, 180, 0}, "source"};
		abandoned.Ease = KeyframeEase{"linear", "linear", {0, 1}, {0, 0}};
		document.Keyframes = {abandoned};
		document.Tracks = {{"owner", "rotation", "hold", -1}, {"target", "rotation", "hold", -1}};
		document.Tracks[0].QuaternionMode = 1 - mode;
		document.Tracks[1].QuaternionMode = mode;
		document.Links = {{"producer", "rotation", "target", "rotation"}};
		document.Outputs = {{"producer", "producer", "rotation"}, {"target", "target", "euler_angles"}};
		Diagnostic diagnostic;
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		Plan plan;
		const auto diagnosticStatus105 = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus105 == Status::Ok);
		GroupReplayState empty, local, bound;
		const auto diagnosticStatus106 = RebindGroupReplay(restored, empty, 1, local, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus106 == Status::Ok);
		const GroupSubtypeBinding binding{
			"target", "owner", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "rotation"
		};
		const auto diagnosticStatus107 =
			BindGroupReplay(restored, {&binding, 1}, local, 1, bound, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus107 == Status::Ok);
		EvaluationRequest clock;
		REQUIRE(SetFrameTime(clock, {3, .25, true}));
		clock.GroupReplay = &bound;
		clock.GroupAuthoringRevision = 1;
		EvaluatedValue produced;
		const auto diagnosticStatus108 =
			EvaluateValue(restored, plan, "producer", clock, produced, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus108 == Status::Ok);
		REQUIRE(std::holds_alternative<Vector4>(produced.Data));
		const auto vector = std::get<Vector4>(produced.Data);
		Quaternion expected;
		REQUIRE(ConvertSourceQuaternion({vector.X, vector.Y, vector.Z, vector.W}, mode, expected));
		EvaluationSnapshot snapshot;
		const auto diagnosticStatus109 =
			EvaluateNodeInputs(restored, plan, "target", clock, snapshot, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus109 == Status::Ok);
		const auto input =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "rotation";
			});
		REQUIRE(input != snapshot.Values().end());
		CHECK(input->Data == Value{expected});
		CHECK(restored.Keyframes == document.Keyframes);
		CHECK(restored.Tracks == document.Tracks);
	}
}

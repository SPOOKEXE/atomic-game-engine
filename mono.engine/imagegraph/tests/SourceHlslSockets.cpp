#include "../src/SourceLuaSockets.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_hlsl_sockets")
using namespace engine::imagegraph;
namespace {
	Node Hlsl(size_t mode) {
		const std::array types{
			ValueType::Scalar,
			ValueType::Integer,
			ValueType::Array,
			ValueType::Array,
			ValueType::Array,
			ValueType::Array,
			ValueType::Array,
			ValueType::Image,
			ValueType::Colour
		};
		Node node{
			"shader",
			"pc.hlsl",
			"",
			{},
			{},
			{{"argument_name_0", ValueType::Text, Value{std::string{"uniform"}}},
			 {"argument_type_0", ValueType::Enum, Value{EnumValue{int64_t(mode)}}},
			 {"argument_value_0", types[mode], std::nullopt}}
		};
		if (mode == 0 || mode == 1) node.DynamicInputs[2].Default = 2.5;
		if (mode >= 2 && mode <= 6) {
			constexpr std::array<size_t, 7> lengths{0, 0, 2, 3, 4, 9, 16};
			ArrayValue value;
			value.ElementType = ValueType::Scalar;
			for (size_t i = 0; i < lengths[mode]; ++i)
				value.Elements.emplace_back(double(i) + .5);
			node.DynamicInputs[2].Default = std::move(value);
		}
		if (mode == 8) node.DynamicInputs[2].Default = Colour{10, 20, 30, 255};
		return node;
	}
	Document Graph(size_t mode) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {Hlsl(mode)};
		doc.Outputs = {{"out", "shader", "surface"}};
		return doc;
	}
} // namespace
TEST_CASE(
	"HLSL nine source argument declarations persist and resolve native inputs", "[imagegraph][hlsl_sockets]"
) {
	for (size_t mode = 0; mode < 9; ++mode) {
		auto doc = Graph(mode);
		Plan plan;
		Diagnostic error;
		INFO(mode);
		REQUIRE(Compile(doc, plan, error) == Status::Ok);
		Document parsed;
		REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
		CHECK(parsed == doc);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		auto found = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
			return v.Port == "argument_value_0";
		});
		if (mode == 7)
			CHECK(found == snapshot.Values().end());
		else {
			REQUIRE(found != snapshot.Values().end());
			if (mode == 1)
				CHECK(std::get<double>(found->Data) == 2.5);
			else
				CHECK(found->Data == *doc.Nodes[0].DynamicInputs[2].Default);
		}
		doc.Nodes[0].DynamicInputs[2].Type = ValueType::Text;
		CHECK(Compile(doc, plan, error) == Status::TypeMismatch);
	}
}
TEST_CASE(
	"HLSL sampler socket captures its linked surface without executing shader", "[imagegraph][hlsl_sockets]"
) {
	auto doc = Graph(7);
	doc.Nodes.push_back(
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{3}}, {"colour", Colour{10, 20, 30, 255}}}}
	);
	doc.Links = {{"image", "image", "shader", "argument_value_0"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	auto image = std::find_if(snapshot.Images().begin(), snapshot.Images().end(), [](const auto &v) {
		return v.Port == "argument_value_0";
	});
	REQUIRE(image != snapshot.Images().end());
	CHECK(image->Data.Width == 2);
	CHECK(image->Data.Height == 3);
}
TEST_CASE(
	"HLSL typed array uniforms reject wrong shapes and nonnumeric leaves", "[imagegraph][hlsl_sockets]"
) {
	for (size_t mode = 2; mode <= 6; ++mode) {
		auto doc = Graph(mode);
		Plan plan;
		Diagnostic error;
		std::get<ArrayValue>(*doc.Nodes[0].DynamicInputs[2].Default).Elements.pop_back();
		CHECK(Compile(doc, plan, error) == Status::TypeMismatch);
		CHECK(error.Port == "argument_value_0");
		doc = Graph(mode);
		std::get<ArrayValue>(*doc.Nodes[0].DynamicInputs[2].Default).Elements[0] = std::string{"invalid"};
		CHECK(Compile(doc, plan, error) == Status::InvalidValue);
		CHECK(error.Message == "dynamic input default is invalid");
	}
	auto doc = Graph(1);
	doc.Nodes[0].DynamicInputs[2].Default = std::string{"invalid"};
	Plan plan;
	Diagnostic error;
	CHECK(Compile(doc, plan, error) == Status::TypeMismatch);
	doc = Graph(0);
	doc.Nodes[0].DynamicInputs[1].Default = EnumValue{99};
	CHECK(Compile(doc, plan, error) == Status::TypeMismatch);
}
TEST_CASE("HLSL instance sampler inherits source routes", "[imagegraph][hlsl_sockets]") {
	auto doc = Graph(7);
	auto instance = doc.Nodes[0];
	instance.Id = "instance";
	instance.InstanceBase = "shader";
	doc.Nodes.push_back(instance);
	doc.Nodes.push_back(
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{3}}, {"colour", Colour{}}}}
	);
	doc.Links = {{"image", "image", "shader", "argument_value_0"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(doc, plan, "instance", {}, snapshot, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	CHECK(std::any_of(snapshot.Images().begin(), snapshot.Images().end(), [](const auto &v) {
		return v.Port == "argument_value_0" && v.Data.Width == 2;
	}));
}

TEST_CASE(
	"HLSL raw integer keys and unbound sampler markers persist source values", "[imagegraph][hlsl_sockets]"
) {
	for (size_t mode : {size_t{1}, size_t{7}, size_t{8}}) {
		auto doc = Graph(mode);
		doc.Nodes[0].DynamicInputs[2].Default = mode == 7 ? Value{-4.0} : Value{2.5};
		doc.Keyframes = {
			{"shader", "argument_value_0", 0, mode == 7 ? Value{-4.0} : Value{3.5}, "source", KeyframeEase{}}
		};
		doc.Tracks = {{"shader", "argument_value_0", "hold", -1}};
		Plan plan;
		Diagnostic error;
		const auto compiled = Compile(doc, plan, error);
		INFO(error.Message);
		REQUIRE(compiled == Status::Ok);
		Document read;
		REQUIRE(Read(Write(doc), read, error) == Status::Ok);
		CHECK(read == doc);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
				return v.Port == "argument_value_0";
			});
		REQUIRE(value != snapshot.Values().end());
		if (mode == 1)
			CHECK(std::get<double>(value->Data) == 3.5);
		else
			CHECK(value->Data == (mode == 7 ? Value{-4.0} : Value{3.5}));
	}
}
TEST_CASE("HLSL vector uniforms validate actual linked array rows", "[imagegraph][hlsl_sockets]") {
	auto doc = Graph(2);
	doc.Nodes.push_back(
		{"array",
		 "pc.array",
		 "",
		 {},
		 {{"type", EnumValue{2}}},
		 {{"input_0", ValueType::Integer, Value{int64_t{1}}},
		  {"input_1", ValueType::Integer, Value{int64_t{2}}}}}
	);
	doc.Links = {{"array", "array", "shader", "argument_value_0"}};
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	const auto value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
		return v.Port == "argument_value_0";
	});
	REQUIRE(value != snapshot.Values().end());
	CHECK(std::get<ArrayValue>(value->Data).Elements.size() == 2);
	doc.Nodes[1].DynamicInputs.push_back({"input_2", ValueType::Integer, Value{int64_t{3}}});
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	CHECK(EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error) == Status::TypeMismatch);
	CHECK(error.Port == "argument_value_0");
	const auto retained = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
		return v.Port == "argument_value_0";
	});
	REQUIRE(retained != snapshot.Values().end());
	CHECK(std::get<ArrayValue>(retained->Data).Elements.size() == 2);
}
TEST_CASE(
	"HLSL static junction selectors resolve through the existing route solver", "[imagegraph][hlsl_sockets]"
) {
	auto doc = Graph(1);
	doc.Nodes[0].DynamicInputs[1].Default = EnumValue{0};
	doc.Junctions = {
		{"selector", "", ValueType::Enum, Value{EnumValue{1}}}, {"relay", "", ValueType::Enum, std::nullopt}
	};
	doc.Links = {{"selector", "value", "relay", "value"}, {"relay", "value", "shader", "argument_type_0"}};
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	Document parsed;
	REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
	CHECK(parsed == doc);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error) == Status::Ok);
	const auto selector = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
		return v.Port == "argument_type_0";
	});
	REQUIRE(selector != snapshot.Values().end());
	CHECK(selector->Data == Value{EnumValue{1}});
	const auto value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
		return v.Port == "argument_value_0";
	});
	REQUIRE(value != snapshot.Values().end());
	CHECK(value->Data == Value{2.5});
}
TEST_CASE("HLSL native cooked schema refuses genuinely dynamic selectors", "[imagegraph][hlsl_sockets]") {
	auto doc = Graph(0);
	doc.Nodes.push_back({"selector", "pc.number_simple", "", {}, {{"value", 1.0}}});
	doc.Links = {{"selector", "number", "shader", "argument_type_0"}};
	Plan plan;
	Diagnostic error;
	CHECK(Compile(doc, plan, error) == Status::UnsupportedExecution);
	CHECK(error.Port == "argument_type_0");
	doc = Graph(0);
	doc.Keyframes = {{"shader", "argument_type_0", 0, EnumValue{0}, "source", KeyframeEase{}}};
	doc.Tracks = {{"shader", "argument_type_0", "hold", -1}};
	CHECK(Compile(doc, plan, error) == Status::UnsupportedExecution);
	CHECK(error.Port == "argument_type_0");
}
TEST_CASE(
	"HLSL instance vector shape follows its inherited selector and values", "[imagegraph][hlsl_sockets]"
) {
	auto doc = Graph(2);
	auto instance = doc.Nodes[0];
	instance.Id = "instance";
	instance.InstanceBase = "shader";
	instance.DynamicInputs[1].Default = EnumValue{0};
	doc.Nodes.push_back(instance);
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(doc, plan, "instance", {}, snapshot, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	const auto value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
		return v.Port == "argument_value_0";
	});
	REQUIRE(value != snapshot.Values().end());
	CHECK(value->Data == *doc.Nodes[0].DynamicInputs[2].Default);
}
TEST_CASE("HLSL vector array captures preserve numeric uniform rows", "[imagegraph][hlsl_sockets]") {
	auto doc = Graph(2);
	ArrayValue rows;
	rows.ElementType = ValueType::Scalar;
	rows.Nested = {{1.0, 2.0}, {3.0, 4.0}};
	doc.Nodes[0].DynamicInputs[2].Default = rows;
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	const auto value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
		return v.Port == "argument_value_0";
	});
	REQUIRE(value != snapshot.Values().end());
	CHECK(value->Data == Value{rows});
	Document parsed;
	REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
	CHECK(parsed == doc);
	std::get<ArrayValue>(*doc.Nodes[0].DynamicInputs[2].Default).Nested[1].pop_back();
	CHECK(Compile(doc, plan, error) == Status::TypeMismatch);
}

TEST_CASE(
	"HLSL tuple capture admits canonical vectors with atomic byte refusal", "[imagegraph][hlsl_sockets]"
) {
	const std::array<Value, 3> tuples{Vector2{1, 2}, Vector3{1, 2, 3}, Vector4{1, 2, 3, 4}};
	for (size_t i = 0; i < tuples.size(); ++i) {
		auto doc = Graph(i + 2);
		doc.Nodes[0].DynamicInputs[2].Default = tuples[i];
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(doc, plan, error) == Status::Ok);
		Document parsed;
		REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
		CHECK(parsed == doc);
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error) == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
				return v.Port == "argument_value_0";
			});
		REQUIRE(value != snapshot.Values().end());
		const auto retained = value->Data;
		CHECK(std::get<ArrayValue>(retained).Elements.size() == i + 2);
		CHECK(EvaluateNodeInputs(doc, plan, "shader", {}, snapshot, error, 1) == Status::LimitExceeded);
		const auto after =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
				return v.Port == "argument_value_0";
			});
		REQUIRE(after != snapshot.Values().end());
		CHECK(after->Data == retained);
	}
}
TEST_CASE(
	"HLSL tuple storage refuses mixed representations and empty processor rows", "[imagegraph][hlsl_sockets]"
) {
	auto doc = Graph(2);
	auto &array = std::get<ArrayValue>(*doc.Nodes[0].DynamicInputs[2].Default);
	array.Nested = {{1.0, 2.0}};
	Plan plan;
	Diagnostic error;
	CHECK(Compile(doc, plan, error) != Status::Ok);
	array.Elements.clear();
	array.Nested = {{}};
	CHECK(Compile(doc, plan, error) == Status::TypeMismatch);
	CHECK(error.Port == "argument_value_0");
}

TEST_CASE("HLSL saved sampler links capture owned producer pixels", "[imagegraph][hlsl_sockets]") {
	auto doc = Graph(7);
	doc.Nodes[0].DynamicInputs[2].Default = -4.0;
	doc.Nodes.push_back(
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}}
	);
	doc.Links = {{"image", "image", "shader", "argument_value_0"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	Document parsed;
	REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
	CHECK(parsed == doc);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(parsed, plan, "shader", {}, snapshot, error) == Status::Ok);
	const auto found =
		std::find_if(snapshot.Images().begin(), snapshot.Images().end(), [](const auto &image) {
			return image.Port == "argument_value_0";
		});
	REQUIRE(found != snapshot.Images().end());
	CHECK(found->Data.Width == 1);
	CHECK(found->Data.Height == 1);
	CHECK(found->Data.Pixels == std::vector<uint8_t>{10, 20, 30, 255});
	auto unsupported = Graph(7);
	SurfaceValue runtime;
	runtime.Data = found->Data;
	unsupported.Nodes[0].DynamicInputs[2].Default = runtime;
	CHECK(detail::SourceHlslArgumentValue(unsupported.Nodes[0], "argument_value_0", Value{runtime}));
	CHECK(Compile(unsupported, plan, error) == Status::InvalidValue);
	CHECK(error.Port == "argument_value_0");
	CHECK(error.Message == "dynamic input default is invalid");
}

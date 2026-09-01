#include "DatasetValue.hpp"

#include <engine/script/Codec.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.datasetvalue")
TEST_DEPENDS("engine.script.codec")

TEST_CASE("dataset text round trips every script value kind", "[studio][dataset]") {
	engine::script::ScriptValue root(engine::script::ValueTag::Map);
	root.Entries.emplace_back("nil", engine::script::ScriptValue{});
	root.Entries.emplace_back("yes", engine::script::ScriptValue(engine::script::ValueTag::True));

	engine::script::ScriptValue array(engine::script::ValueTag::Array);
	engine::script::ScriptValue number(engine::script::ValueTag::Number);
	number.Number = 42.5;
	array.Items.push_back(number);
	engine::script::ScriptValue text(engine::script::ValueTag::String);
	text.Text = std::string("hello\0world", 11);
	array.Items.push_back(text);
	root.Entries.emplace_back("array", array);

	engine::script::ScriptValue vector(engine::script::ValueTag::Vector3);
	vector.Vector = {1.0f, 2.0f, 3.0f};
	root.Entries.emplace_back("vector", vector);
	engine::script::ScriptValue colour(engine::script::ValueTag::Color3);
	colour.Colour = {0.1f, 0.2f, 0.3f};
	root.Entries.emplace_back("colour", colour);
	engine::script::ScriptValue frame(engine::script::ValueTag::CFrame);
	frame.Frame.Position = {4.0f, 5.0f, 6.0f};
	frame.Frame.QuaternionY = 0.5f;
	frame.Frame.QuaternionW = 0.75f;
	root.Entries.emplace_back("frame", frame);

	std::vector<std::byte> encoded;
	REQUIRE(engine::script::Encode(root, encoded) == engine::script::CodecStatus::Ok);
	std::string document;
	std::string error;
	REQUIRE(studio::DatasetValueToText(encoded, document, error));

	std::vector<std::byte> roundTrip;
	REQUIRE(studio::DatasetValueFromText(document, roundTrip, error));
	CHECK(roundTrip == encoded);

	engine::script::ScriptValue binary(engine::script::ValueTag::String);
	binary.Text = std::string("\xff\0\x80", 3);
	REQUIRE(engine::script::Encode(binary, encoded) == engine::script::CodecStatus::Ok);
	REQUIRE(studio::DatasetValueToText(encoded, document, error));
	REQUIRE(studio::DatasetValueFromText(document, roundTrip, error));
	CHECK(roundTrip == encoded);
}

TEST_CASE("dataset text refuses malformed and ambiguous values", "[studio][dataset]") {
	std::vector<std::byte> bytes{std::byte{0x7f}};
	std::string document;
	std::string error;
	CHECK_FALSE(studio::DatasetValueToText(bytes, document, error));
	CHECK_FALSE(error.empty());

	CHECK_FALSE(studio::DatasetValueFromText("not json", bytes, error));
	CHECK_FALSE(studio::DatasetValueFromText(R"({"type":"number","value":"seven"})", bytes, error));
	CHECK_FALSE(studio::DatasetValueFromText(R"({"type":"vector3","value":[1,2]})", bytes, error));
	CHECK_FALSE(studio::DatasetValueFromText(R"({"type":"mystery","value":0})", bytes, error));
	CHECK_FALSE(
		studio::DatasetValueFromText(
			R"({"type":"map","value":[
			{"key":{"encoding":"utf8","value":"same"},"value":{"type":"nil"}},
			{"key":{"encoding":"utf8","value":"same"},"value":{"type":"nil"}}
		]})",
			bytes,
			error
		)
	);
}

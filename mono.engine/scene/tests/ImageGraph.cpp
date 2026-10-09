#include "ImageGraphRegistration.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ImageGraph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

TEST_SUITE_ID("engine.scene.imagegraph")

namespace {
	using namespace engine::scene;
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	Entity Make(Store &store, const char *key = "weather") {
		const auto entity = store.CreateInstance(ImageGraphClass(), "LiveGraph");
		REQUIRE(SetImageGraphInstanceKey(store, entity, Name(key)));
		REQUIRE(SetImageGraphAsset(store, entity, Name("textures/weather.aimagegraph")));
		return entity;
	}
	ImageGraphInput Number(const char *name, double number) {
		ImageGraphInput input;
		input.Name = Name(name);
		input.Number = number;
		return input;
	}
}

TEST_CASE("live image graphs use durable unique names and refuse unsafe references", "[scene][imagegraph]") {
	ImageGraphClass();
	Store store("imagegraph.names");
	const auto graph = Make(store);
	CHECK(ImageGraphContentName(store, graph).Text() == "imagegraph-instance://weather#image");
	CHECK(ImageGraphContentName(store, graph, Name("mask")).Text() == "imagegraph-instance://weather#mask");
	const auto revision = store.Get<ImageGraph>(graph)->Revision;
	REQUIRE(SetImageGraphInstanceKey(store, graph, Name("weather")));
	REQUIRE(SetImageGraphAsset(store, graph, Name("textures/weather.aimagegraph")));
	REQUIRE(SetImageGraphOutput(store, graph, Name("image")));
	CHECK(store.Get<ImageGraph>(graph)->Revision == revision);
	for (const char *key : {"", "../weather", "weather#mask", "weather:mask", "white space", "."})
		CHECK_FALSE(SetImageGraphInstanceKey(store, graph, Name(key)));
	for (const char *asset :
		 {"a.imagegraph", "../a.aimagegraph", "/a.aimagegraph", "a.aimagegraph#mask", "a\\b.aimagegraph"})
		CHECK_FALSE(SetImageGraphAsset(store, graph, Name(asset)));
	CHECK_FALSE(SetImageGraphOutput(store, graph, Name("image#other")));
	const auto second = store.CreateInstance(ImageGraphClass(), "OtherGraph");
	CHECK_FALSE(SetImageGraphInstanceKey(store, second, Name("weather")));
	REQUIRE(SetImageGraphInstanceKey(store, second, Name("other")));
	const auto copy = store.CloneInstance(graph);
	REQUIRE(copy != engine::ecs::NULL_ENTITY);
	CHECK_FALSE(ImageGraphContentName(store, graph).IsValid());
	CHECK_FALSE(ImageGraphContentName(store, copy).IsValid());
	REQUIRE(SetImageGraphInstanceKey(store, copy, Name("weather-copy")));
	CHECK(ImageGraphContentName(store, graph).IsValid());
	CHECK(ImageGraphContentName(store, copy).Text() == "imagegraph-instance://weather-copy#image");
}

TEST_CASE("typed image graph input changes are bounded atomic and quiet on no-op", "[scene][imagegraph]") {
	ImageGraphClass();
	Store store("imagegraph.inputs");
	const auto graph = Make(store);
	store.Observe<ImageGraph>();
	store.ClearChanges();
	REQUIRE(SetImageGraphInput(store, graph, Number("opacity", .5)));
	CHECK(store.Changed<ImageGraph>(graph));
	store.ClearChanges();
	const auto revision = store.Get<ImageGraph>(graph)->Revision;
	REQUIRE(SetImageGraphInput(store, graph, Number("opacity", .5)));
	CHECK(store.Get<ImageGraph>(graph)->Revision == revision);
	CHECK_FALSE(store.Changed<ImageGraph>(graph));
	CHECK_FALSE(SetImageGraphInput(store, graph, Number("opacity", std::numeric_limits<double>::infinity())));
	CHECK_FALSE(
		SetImageGraphInput(store, graph, Number("opacity", std::numeric_limits<double>::quiet_NaN()))
	);
	CHECK_FALSE(SetImageGraphInput(store, graph, Number("unsafe/name", 1)));
	ImageGraphInput text;
	text.Name = Name("caption");
	text.Kind = ImageGraphInputKind::String;
	text.String.assign(MAXIMUM_IMAGE_GRAPH_STRING_BYTES + 1, 'x');
	CHECK_FALSE(SetImageGraphInput(store, graph, text));
	text.String = "line\nbreak";
	CHECK_FALSE(SetImageGraphInput(store, graph, text));
	text.String = std::string("a\0b", 3);
	CHECK_FALSE(SetImageGraphInput(store, graph, text));
	CHECK(store.Get<ImageGraph>(graph)->Revision == revision);
	ImageGraphInput held;
	REQUIRE(GetImageGraphInput(store, graph, Name("opacity"), held));
	CHECK(held.Number == .5);
	std::vector<ImageGraphInput> tooMany;
	for (size_t index = 0; index <= MAXIMUM_IMAGE_GRAPH_INPUTS; ++index)
		tooMany.push_back(Number(("input-" + std::to_string(index)).c_str(), 1));
	CHECK_FALSE(SetImageGraphInputs(store, graph, tooMany));
	std::array duplicates{Number("duplicate", 1), Number("duplicate", 2)};
	CHECK_FALSE(SetImageGraphInputs(store, graph, duplicates));
	CHECK(store.Get<ImageGraph>(graph)->Inputs.size() == 1);
	REQUIRE(ResetImageGraphInput(store, graph, Name("opacity")));
	CHECK_FALSE(GetImageGraphInput(store, graph, Name("opacity"), held));
	const auto resetRevision = store.Get<ImageGraph>(graph)->Revision;
	REQUIRE(ResetImageGraphInput(store, graph, Name("opacity")));
	CHECK(store.Get<ImageGraph>(graph)->Revision == resetRevision);
	store.SetAdoptOnly(true);
	CHECK_FALSE(SetImageGraphInput(store, graph, Number("opacity", .9)));
	CHECK_FALSE(ResetImageGraphInput(store, graph, Name("opacity")));
	CHECK_FALSE(SetImageGraphInstanceKey(store, graph, Name("renamed")));
	CHECK_FALSE(SetImageGraphAsset(store, graph, Name("other.aimagegraph")));
	CHECK_FALSE(SetImageGraphOutput(store, graph, Name("mask")));
}

TEST_CASE("image graph reflected defaults accept writeback and mark declared writes", "[scene][imagegraph]") {
	ImageGraphClass();
	Store store("imagegraph.property.defaults");
	const auto graph = store.CreateInstance(ImageGraphClass(), "Unconfigured");
	store.Observe<ImageGraph>();
	for (const char *property : {"InstanceKey", "Graph", "Output"}) {
		CAPTURE(property);
		Name value;
		REQUIRE(store.GetProperty(graph, Name(property), &value, sizeof(value)));
		store.ClearChanges();
		REQUIRE(store.SetProperty(graph, Name(property), &value, sizeof(value)));
		CHECK(store.Changed<ImageGraph>(graph));
		CHECK(store.Get<ImageGraph>(graph)->Revision == 0);
		Name restored;
		REQUIRE(store.GetProperty(graph, Name(property), &restored, sizeof(restored)));
		CHECK(restored == value);
	}
	CHECK_FALSE(ImageGraphContentName(store, graph).IsValid());
	REQUIRE(SetImageGraphInstanceKey(store, graph, Name("weather")));
	REQUIRE(SetImageGraphAsset(store, graph, Name("weather.aimagegraph")));
	const auto revision = store.Get<ImageGraph>(graph)->Revision;
	for (const char *property : {"InstanceKey", "Graph", "Output"}) {
		Name value;
		REQUIRE(store.GetProperty(graph, Name(property), &value, sizeof(value)));
		store.ClearChanges();
		REQUIRE(store.SetProperty(graph, Name(property), &value, sizeof(value)));
		CHECK(store.Changed<ImageGraph>(graph));
		CHECK(store.Get<ImageGraph>(graph)->Revision == revision);
	}
	store.ClearChanges();
	const Name empty;
	CHECK_FALSE(store.SetProperty(graph, Name("InstanceKey"), &empty, sizeof(empty)));
	CHECK_FALSE(store.SetProperty(graph, Name("Graph"), &empty, sizeof(empty)));
	CHECK_FALSE(store.Changed<ImageGraph>(graph));
	CHECK(store.Get<ImageGraph>(graph)->Revision == revision);
	REQUIRE(SetImageGraphInstanceKey(store, graph, Name("weather")));
	REQUIRE(SetImageGraphAsset(store, graph, Name("weather.aimagegraph")));
	REQUIRE(SetImageGraphOutput(store, graph, Name("image")));
	CHECK_FALSE(store.Changed<ImageGraph>(graph));
	std::string inputs;
	REQUIRE(store.GetProperty(graph, Name("Inputs"), &inputs, sizeof(inputs)));
	REQUIRE(store.SetProperty(graph, Name("Inputs"), &inputs, sizeof(inputs)));
	CHECK(store.Changed<ImageGraph>(graph));
	CHECK(store.Get<ImageGraph>(graph)->Revision == revision);
}

TEST_CASE(
	"live graph snapshots and reflected save data retain every primitive input", "[scene][imagegraph]"
) {
	ImageGraphClass();
	Store source("imagegraph.snapshot");
	const auto entity = Make(source);
	auto boolean = Number("enabled", 0);
	boolean.Kind = ImageGraphInputKind::Boolean;
	boolean.Boolean = true;
	auto colour = Number("tint", 0);
	colour.Kind = ImageGraphInputKind::Colour;
	colour.Colour = {12, 34, 56, 78};
	auto text = Number("caption", 0);
	text.Kind = ImageGraphInputKind::String;
	text.String = "quoted \"value\"";
	const std::array inputs{colour, text, Number("opacity", .25), boolean};
	REQUIRE(SetImageGraphInputs(source, entity, inputs));
	std::string encoded;
	REQUIRE(source.GetProperty(entity, Name("Inputs"), &encoded, sizeof(encoded)));
	const auto other = source.CreateInstance(ImageGraphClass(), "Other");
	REQUIRE(source.SetProperty(other, Name("Inputs"), &encoded, sizeof(encoded)));
	CHECK(source.Get<ImageGraph>(other)->Inputs == source.Get<ImageGraph>(entity)->Inputs);
	const auto prior = source.Get<ImageGraph>(other)->Inputs;
	for (const std::string bad : {"v2:00000000", "v1:ff", "v1:ffffffff", "v1:zz"}) {
		CHECK_FALSE(source.SetProperty(other, Name("Inputs"), &bad, sizeof(bad)));
		CHECK(source.Get<ImageGraph>(other)->Inputs == prior);
	}
	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));
	Store restored("imagegraph.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	REQUIRE(reader.AtEnd());
	const auto *graph = restored.Get<ImageGraph>(entity);
	REQUIRE(graph != nullptr);
	CHECK(graph->Inputs == source.Get<ImageGraph>(entity)->Inputs);
	CHECK(graph->Revision == 0);
	CHECK(source.Get<ImageGraph>(entity)->Revision > 0);
	CHECK(ImageGraphContentName(restored, entity).Text() == "imagegraph-instance://weather#image");
	ImageGraph changedRevision = *graph;
	changedRevision.Revision = 999;
	engine::core::ByteWriter unchangedBytes, changedBytes;
	engine::scene::detail::WriteImageGraphs(unchangedBytes, graph, 1);
	engine::scene::detail::WriteImageGraphs(changedBytes, &changedRevision, 1);
	CHECK(std::ranges::equal(unchangedBytes.Bytes(), changedBytes.Bytes()));
	ImageGraph invalid = *graph;
	invalid.Inputs.front().Kind = static_cast<ImageGraphInputKind>(255);
	engine::core::ByteWriter refused;
	REQUIRE_THROWS_AS(engine::scene::detail::WriteImageGraphs(refused, &invalid, 1), std::invalid_argument);
	CHECK(refused.Empty());
	engine::core::ByteWriter malformed;
	malformed.WriteString("weather");
	malformed.WriteString("weather.aimagegraph");
	malformed.WriteString("image");
	malformed.WriteUInt32(MAXIMUM_IMAGE_GRAPH_INPUTS + 1);
	engine::core::ByteReader badReader(malformed.Bytes());
	ImageGraph destination = *graph;
	const auto accepted = destination.Inputs;
	engine::scene::detail::ReadImageGraphs(badReader, &destination, 1);
	CHECK(badReader.Failed());
	CHECK(destination.Inputs == accepted);
}

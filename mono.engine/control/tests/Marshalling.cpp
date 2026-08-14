// Every `PropertyType` through `instance_set` and back out of `instance_get`.
//
// **The marshaller is a switch with a case per type and had none of them
// covered.** `engine.control.surface` drives the protocol and the storage tools;
// this drives the conversion underneath the instance tools, which is where a
// wrong field name, a swapped pair of components or a case that silently falls
// through costs an author the value they typed rather than an error.
//
// The class is declared here rather than borrowed. `control` links `ecs` and
// `world` and no component module, so there is no `Part` to write a `Vector3`
// on - and a probe holding one field of every type covers the table exactly
// once, which no real class does.

#include <engine/control/Surface.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.control.marshalling")
TEST_DEPENDS("engine.control.surface")
TEST_DEPENDS("engine.ecs.classes")

using Catch::Approx;
using engine::control::Surface;
using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::EnumTable;
using engine::ecs::PropertyDescriptor;
using engine::ecs::PropertyType;
using engine::ecs::Store;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using nlohmann::json;

namespace marshalling_test {
	namespace core = engine::core;

	// One field per `PropertyType` the surface claims to write. `Reference` and
	// `Opaque` are absent because the surface refuses both by name, which the
	// last case here asserts.
	struct Probe {
		core::NumberSequence Curve;
		core::ColorSequence Gradient;
		core::CFrame Frame;
		core::Rect Area;
		core::UDim2 Box;
		core::Vector3 Point;
		core::Color3 Tint;
		core::Vector2 Flat;
		core::UDim Span;
		core::NumberRange Between;
		double Large = 0.0;
		int64_t Wide = 0;
		float Small = 0.0f;
		int32_t Whole = 0;
		Name Label;
		Name Face;
		bool Flag = false;
	};

	// Separate from `Probe` because a `std::string` makes a component
	// non-trivial, and keeping the two apart leaves the value cases operating
	// on the memcpy path every real component takes.
	struct Wording {
		std::string Text;
	};

	// The surface's `Enum` case looks the member up in this table, so a probe
	// with an enum property needs one registered.
	const Name &FaceEnum() {
		static const Name name = [] {
			const Name key("engine.control.marshalling.Face");
			const std::string_view members[] = {"Front", "Back"};
			EnumTable::Register(key.Text(), members);
			return key;
		}();
		return name;
	}

	PropertyDescriptor EnumProperty() {
		PropertyDescriptor property = {};
		property.Name = Name("Face");
		property.Type = PropertyType::Enum;
		property.Size = sizeof(Name);
		property.EnumName = FaceEnum();
		property.Reads = &engine::ecs::ComponentSet::Intern({Components::Of<Probe>()});
		property.Writes = property.Reads;
		property.Get = [](const Store &store, Entity instance, void *out) -> bool {
			const Probe *probe = store.Get<Probe>(instance);
			if (probe == nullptr) {
				return false;
			}
			*static_cast<Name *>(out) = probe->Face;
			return true;
		};
		property.Set = [](Store &store, Entity instance, const void *value) -> bool {
			Probe *probe = store.GetMutable<Probe>(instance);
			if (probe == nullptr) {
				return false;
			}
			probe->Face = *static_cast<const Name *>(value);
			return true;
		};
		return property;
	}

	// Registered once for the process, because the class table never forgets.
	ClassId ProbeClass() {
		static const ClassId id = [] {
			const ComponentId components[] = {Components::Of<Probe>(), Components::Of<Wording>()};
			const ClassId klass =
				Classes::Register("MarshallingProbe", Classes::RegisterInstanceRoot(), components);

			Classes::Property<&Probe::Flag>(klass, "Flag");
			Classes::Property<&Probe::Whole>(klass, "Whole");
			Classes::Property<&Probe::Wide>(klass, "Wide");
			Classes::Property<&Probe::Small>(klass, "Small");
			Classes::Property<&Probe::Large>(klass, "Large");
			Classes::Property<&Probe::Label>(klass, "Label");
			Classes::Property<&Probe::Flat>(klass, "Flat");
			Classes::Property<&Probe::Point>(klass, "Point");
			Classes::Property<&Probe::Tint>(klass, "Tint");
			Classes::Property<&Probe::Frame>(klass, "Frame");
			Classes::Property<&Probe::Span>(klass, "Span");
			Classes::Property<&Probe::Box>(klass, "Box");
			Classes::Property<&Probe::Area>(klass, "Area");
			Classes::Property<&Probe::Between>(klass, "Between");
			Classes::Property<&Probe::Curve>(klass, "Curve");
			Classes::Property<&Probe::Gradient>(klass, "Gradient");
			Classes::Property<&Wording::Text>(klass, "Text");
			Classes::Computed(klass, EnumProperty());

			return klass;
		}();
		return id;
	}

	// The surface's reply, undone from the text block it travels in. The same
	// shape `engine.control.surface` uses, repeated rather than shared because
	// the two suites are separate translation units and this is nine lines.
	json Called(Surface &surface, const std::string &tool, const json &arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", json{{"name", tool}, {"arguments", arguments}}},
		};

		const std::string reply = surface.Answer(request.dump());
		INFO(tool << " -> " << reply);
		REQUIRE_FALSE(reply.empty());

		const json parsed = json::parse(reply);
		REQUIRE(parsed.contains("result"));
		failed = parsed["result"].value("isError", false);
		return json::parse(parsed["result"]["content"][0]["text"].get<std::string>(), nullptr, false);
	}

	json Called(Surface &surface, const std::string &tool, const json &arguments) {
		bool failed = false;
		const json payload = Called(surface, tool, arguments, failed);
		INFO(tool << " refused: " << payload.dump());
		REQUIRE_FALSE(failed);
		return payload;
	}

	// A universe holding one world with one probe instance in it, plus the
	// surface that talks to it.
	struct Fixture {
		Universe Worlds;
		Surface Panel{"marshalling", "0"};
		WorldId World;
		Entity Instance;

		Fixture() {
			WorldSettings settings;
			settings.Name = Name("probes");
			World = Worlds.Create(settings);

			Worlds.Enter(World, [&](Store &store) {
				Instance = store.CreateInstance(ProbeClass(), "Probe");
			});

			Panel.AddUniverseTools(Worlds, true);
		}

		json Read() {
			return Called(
				Panel, "instance_get", json{{"world", "probes"}, {"id", Instance.Id}}
			)["properties"];
		}

		void Write(const std::string &property, const json &value) {
			Called(
				Panel,
				"instance_set",
				json{{"world", "probes"}, {"id", Instance.Id}, {"property", property}, {"value", value}}
			);
		}

		std::string Refuse(const std::string &property, const json &value) {
			bool failed = false;
			const json payload = Called(
				Panel,
				"instance_set",
				json{{"world", "probes"}, {"id", Instance.Id}, {"property", property}, {"value", value}},
				failed
			);
			REQUIRE(failed);
			return payload.dump();
		}
	};
}

using namespace marshalling_test;

TEST_CASE("every scalar survives the round trip", "[control][marshalling]") {
	Fixture fixture;

	fixture.Write("Flag", true);
	fixture.Write("Whole", -7);
	fixture.Write("Wide", 4'000'000'000LL);
	fixture.Write("Small", 1.5);
	fixture.Write("Large", 2.25);
	fixture.Write("Label", "a-name");

	const json read = fixture.Read();
	CHECK(read["Flag"] == true);
	CHECK(read["Whole"] == -7);
	CHECK(read["Wide"] == 4'000'000'000LL);
	CHECK(read["Small"].get<double>() == Approx(1.5));
	CHECK(read["Large"].get<double>() == Approx(2.25));
	CHECK(read["Label"] == "a-name");
}

TEST_CASE("every compound value survives the round trip", "[control][marshalling]") {
	// **One case per shape rather than one for all of them**, because the
	// failure this catches is a field read under the wrong name - `Min`/`Max`
	// where the writer said `X`/`Y` - and a single assertion over the lot would
	// name the suite rather than the shape.
	Fixture fixture;

	fixture.Write("Flat", json{{"X", 3.0}, {"Y", 4.0}});
	fixture.Write("Point", json{{"X", 1.0}, {"Y", 2.0}, {"Z", 3.0}});
	fixture.Write("Tint", json{{"R", 0.25}, {"G", 0.5}, {"B", 0.75}});
	fixture.Write("Span", json{{"Scale", 0.5}, {"Offset", 12.0}});
	fixture.Write(
		"Box",
		json{{"X", json{{"Scale", 0.1}, {"Offset", 2.0}}}, {"Y", json{{"Scale", 0.3}, {"Offset", 4.0}}}}
	);
	fixture.Write("Area", json{{"Min", json{{"X", 1.0}, {"Y", 2.0}}}, {"Max", json{{"X", 3.0}, {"Y", 4.0}}}});
	fixture.Write("Between", json{{"Min", 5.0}, {"Max", 9.0}});

	const json read = fixture.Read();

	CHECK(read["Flat"]["X"].get<double>() == Approx(3.0));
	CHECK(read["Flat"]["Y"].get<double>() == Approx(4.0));

	CHECK(read["Point"]["X"].get<double>() == Approx(1.0));
	CHECK(read["Point"]["Y"].get<double>() == Approx(2.0));
	CHECK(read["Point"]["Z"].get<double>() == Approx(3.0));

	CHECK(read["Tint"]["R"].get<double>() == Approx(0.25));
	CHECK(read["Tint"]["G"].get<double>() == Approx(0.5));
	CHECK(read["Tint"]["B"].get<double>() == Approx(0.75));

	CHECK(read["Span"]["Scale"].get<double>() == Approx(0.5));
	CHECK(read["Span"]["Offset"].get<double>() == Approx(12.0));

	CHECK(read["Box"]["X"]["Scale"].get<double>() == Approx(0.1));
	CHECK(read["Box"]["X"]["Offset"].get<double>() == Approx(2.0));
	CHECK(read["Box"]["Y"]["Scale"].get<double>() == Approx(0.3));
	CHECK(read["Box"]["Y"]["Offset"].get<double>() == Approx(4.0));

	CHECK(read["Area"]["Min"]["X"].get<double>() == Approx(1.0));
	CHECK(read["Area"]["Min"]["Y"].get<double>() == Approx(2.0));
	CHECK(read["Area"]["Max"]["X"].get<double>() == Approx(3.0));
	CHECK(read["Area"]["Max"]["Y"].get<double>() == Approx(4.0));

	CHECK(read["Between"]["Min"].get<double>() == Approx(5.0));
	CHECK(read["Between"]["Max"].get<double>() == Approx(9.0));
}

TEST_CASE("a CFrame survives position and rotation together", "[control][marshalling]") {
	// Separate because a `CFrame` is the one value whose two halves are
	// different kinds of number, and a writer that took the position and left
	// the rotation would pass every other assertion in this file.
	Fixture fixture;

	// A quarter turn about Y, which is the shape both halves were dropping:
	// the read reported position alone and the write reset the rotation to
	// identity, so an editor reading a rotated part and writing it back stood
	// it up.
	constexpr double HALF_ROOT_TWO = 0.70710678;

	fixture.Write(
		"Frame",
		json{
			{"Position", json{{"X", 5.0}, {"Y", 6.0}, {"Z", 7.0}}},
			{"Rotation", json{{"X", 0.0}, {"Y", HALF_ROOT_TWO}, {"Z", 0.0}, {"W", HALF_ROOT_TWO}}},
		}
	);

	const json read = fixture.Read()["Frame"];
	CHECK(read["Position"]["X"].get<double>() == Approx(5.0));
	CHECK(read["Position"]["Y"].get<double>() == Approx(6.0));
	CHECK(read["Position"]["Z"].get<double>() == Approx(7.0));
	CHECK(read["Rotation"]["X"].get<double>() == Approx(0.0));
	CHECK(read["Rotation"]["Y"].get<double>() == Approx(HALF_ROOT_TWO).margin(1e-6));
	CHECK(read["Rotation"]["Z"].get<double>() == Approx(0.0));
	CHECK(read["Rotation"]["W"].get<double>() == Approx(HALF_ROOT_TWO).margin(1e-6));

	// And writing what was read leaves it where it was, which is the editor's
	// nudge: read the frame, change one number, write it back.
	fixture.Write("Frame", read);

	const json again = fixture.Read()["Frame"];
	CHECK(again["Rotation"]["Y"].get<double>() == Approx(HALF_ROOT_TWO).margin(1e-6));
	CHECK(again["Rotation"]["W"].get<double>() == Approx(HALF_ROOT_TWO).margin(1e-6));
}

TEST_CASE("a CFrame written with no rotation is upright rather than degenerate", "[control][marshalling]") {
	// The default a missing `Rotation` takes. Zero for all four would not be a
	// rotation at all, and every consumer normalising it would divide by zero.
	Fixture fixture;

	fixture.Write("Frame", json{{"Position", json{{"X", 1.0}, {"Y", 0.0}, {"Z", 0.0}}}});

	const json read = fixture.Read()["Frame"];
	CHECK(read["Rotation"]["W"].get<double>() == Approx(1.0));
	CHECK(read["Rotation"]["X"].get<double>() == Approx(0.0));
}

TEST_CASE("a sequence survives its keypoints", "[control][marshalling]") {
	Fixture fixture;

	fixture.Write(
		"Curve",
		json{
			{"Keypoints",
			 json::array(
				 {json{{"Time", 0.0}, {"Value", 1.0}, {"Envelope", 0.0}},
				  json{{"Time", 1.0}, {"Value", 0.0}, {"Envelope", 0.25}}}
			 )},
		}
	);

	fixture.Write(
		"Gradient",
		json{
			{"Keypoints",
			 json::array(
				 {json{{"Time", 0.0}, {"Value", json{{"R", 1.0}, {"G", 0.0}, {"B", 0.0}}}},
				  json{{"Time", 1.0}, {"Value", json{{"R", 0.0}, {"G", 0.0}, {"B", 1.0}}}}}
			 )},
		}
	);

	const json read = fixture.Read();

	REQUIRE(read["Curve"]["Keypoints"].size() == 2);
	CHECK(read["Curve"]["Keypoints"][1]["Value"].get<double>() == Approx(0.0));
	CHECK(read["Curve"]["Keypoints"][1]["Envelope"].get<double>() == Approx(0.25));

	REQUIRE(read["Gradient"]["Keypoints"].size() == 2);
	CHECK(read["Gradient"]["Keypoints"][1]["Value"]["B"].get<double>() == Approx(1.0));
}

TEST_CASE(
	"a sequence longer than a sequence holds is refused rather than truncated", "[control][marshalling]"
) {
	// The refusal `ValueFromJson` states in as many words: a gradient silently
	// missing its last stop is subtly wrong everywhere and obviously wrong
	// nowhere.
	Fixture fixture;

	json stops = json::array();
	for (int index = 0; index < 25; index++) {
		stops.push_back(json{{"Time", index / 24.0}, {"Value", 1.0}, {"Envelope", 0.0}});
	}

	const std::string refusal = fixture.Refuse("Curve", json{{"Keypoints", stops}});
	CHECK(refusal.find("20 keypoints") != std::string::npos);
}

TEST_CASE("a sequence that is not an array is refused", "[control][marshalling]") {
	Fixture fixture;

	const std::string refusal = fixture.Refuse("Curve", json{{"Keypoints", 3.0}});
	CHECK(refusal.find("Keypoints") != std::string::npos);
}

TEST_CASE("a string property takes a string and nothing else", "[control][marshalling]") {
	// The one type that does not go through the byte buffer, which is why it
	// has a refusal of its own to get wrong.
	Fixture fixture;

	fixture.Write("Text", "a score that changes");
	CHECK(fixture.Read()["Text"] == "a score that changes");

	const std::string refusal = fixture.Refuse("Text", 12);
	CHECK(refusal.find("string") != std::string::npos);
}

TEST_CASE("an enum takes a member name and refuses anything else", "[control][marshalling]") {
	Fixture fixture;

	fixture.Write("Face", "Back");
	CHECK(fixture.Read()["Face"] == "Back");

	// Refused where it was written rather than landing in the component as a
	// face nobody chose, which is `PropertyType::Enum`'s whole argument for
	// existing beside `Name`.
	CHECK_FALSE(fixture.Refuse("Face", "Frnot").empty());
}

TEST_CASE("a missing axis reads as zero rather than as what was there", "[control][marshalling]") {
	// **Stated by `ValueFromJson` and worth a case, because the opposite is the
	// intuitive guess.** A caller sending only `{"X":...}` is setting a whole
	// value, and a half-write that kept the old Y would be a value nobody
	// authored.
	Fixture fixture;

	fixture.Write("Point", json{{"X", 1.0}, {"Y", 2.0}, {"Z", 3.0}});
	fixture.Write("Point", json{{"X", 9.0}});

	const json read = fixture.Read()["Point"];
	CHECK(read["X"].get<double>() == Approx(9.0));
	CHECK(read["Y"].get<double>() == Approx(0.0));
	CHECK(read["Z"].get<double>() == Approx(0.0));
}

TEST_CASE("a property that is not on the class is a refusal naming it", "[control][marshalling]") {
	Fixture fixture;

	const std::string refusal = fixture.Refuse("NotAProperty", 1);
	CHECK(refusal.find("NotAProperty") != std::string::npos);
}

TEST_CASE("a read-only property is refused rather than silently ignored", "[control][marshalling]") {
	Fixture fixture;

	// `Instance.Parent` is writable, so the read-only one to reach for is
	// declared here: the same descriptor with the flag cleared.
	PropertyDescriptor frozen = EnumProperty();
	frozen.Name = Name("FrozenFace");
	frozen.Writable = false;
	Classes::Computed(ProbeClass(), frozen);

	const std::string refusal = fixture.Refuse("FrozenFace", "Back");
	CHECK(refusal.find("read-only") != std::string::npos);
}

TEST_CASE("an instance that is not alive is a refusal rather than a write", "[control][marshalling]") {
	Fixture fixture;

	bool failed = false;
	Called(
		fixture.Panel,
		"instance_set",
		json{{"world", "probes"}, {"id", 999'999}, {"property", "Flag"}, {"value", true}},
		failed
	);
	CHECK(failed);
}

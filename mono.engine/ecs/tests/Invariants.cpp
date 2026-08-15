// The registry sweep, and the rules it is checking, against types declared
// here on purpose.
//
// Two halves. The first registers a type that breaks each rule and requires the
// sweep to name it, because a sweep nobody has watched fail is a sweep that
// passes on an empty registry. The second runs it over everything this binary
// registered and requires silence.
//
// The broken types are declared here rather than reached for elsewhere so that
// the suite proves the check works without needing a real component to be
// wrong. Once one is fixed, a test that pointed at it would go quiet.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Invariants.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.ecs.invariants")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::ecs::AuditChecksPadding;
using engine::ecs::AuditComponents;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::ComponentComplaint;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::PropertyComplaint;
using engine::ecs::PropertyDescriptor;
using engine::ecs::PropertyKind;
using engine::ecs::PropertyType;
using engine::ecs::Store;

namespace invariants_test {
	// Trailing padding under a raw writer: the shape `scene::CameraController`
	// had, and the shape `scene::CharacterLimb` carried into every save.
	struct Gappy {
		uint64_t Wide = 0;
		uint8_t Narrow = 0;
	};

	// The same padding with a writer of its own, which is the other legitimate
	// answer and must not be complained about.
	struct GappyButWritten {
		uint64_t Wide = 0;
		uint8_t Narrow = 0;
	};

	void WriteGappy(ByteWriter &writer, const void *source, size_t count) {
		const auto *values = static_cast<const GappyButWritten *>(source);
		for (size_t index = 0; index < count; index++) {
			writer.WriteUInt64(values[index].Wide);
			writer.WriteUInt8(values[index].Narrow);
		}
	}

	void ReadGappy(ByteReader &reader, void *destination, size_t count) {
		auto *values = static_cast<GappyButWritten *>(destination);
		for (size_t index = 0; index < count; index++) {
			values[index].Wide = reader.ReadUInt64();
			values[index].Narrow = reader.ReadUInt8();
		}
	}

	// No padding, but a member with no default, so its bytes are whatever the
	// column held before. `HasPadding` cannot see this and the poisoned-buffer
	// comparison can.
	struct Uninitialised {
		uint32_t Set = 0;
		uint32_t Loose;

		Uninitialised() : Set(0) {}
	};

	// Reads fewer bytes than it writes, which walks every later column in a
	// snapshot off its offset.
	struct Lopsided {
		uint32_t First = 0;
		uint32_t Second = 0;
	};

	void WriteLopsided(ByteWriter &writer, const void *source, size_t count) {
		const auto *values = static_cast<const Lopsided *>(source);
		for (size_t index = 0; index < count; index++) {
			writer.WriteUInt32(values[index].First);
			writer.WriteUInt32(values[index].Second);
		}
	}

	void ReadLopsided(ByteReader &reader, void *destination, size_t count) {
		auto *values = static_cast<Lopsided *>(destination);
		for (size_t index = 0; index < count; index++) {
			values[index].First = reader.ReadUInt32();
		}
	}

	// A wire format that lies about its own width.
	struct Miscounted {
		uint32_t Value = 0;
	};

	// A wire format whose decoder refuses input its own encoder never
	// produces, which is the shape that lets a peer stall a host.
	struct Partial {
		uint32_t Value = 0;
	};

	std::vector<std::string> RulesFor(std::string_view component) {
		std::vector<std::string> found;
		for (const ComponentComplaint &complaint : AuditComponents()) {
			if (complaint.Component.Text() == component) {
				found.push_back(complaint.Rule);
			}
		}
		return found;
	}

	bool Mentions(const std::vector<std::string> &rules, std::string_view fragment) {
		return std::any_of(rules.begin(), rules.end(), [&](const std::string &rule) {
			return rule.find(fragment) != std::string::npos;
		});
	}
}

using namespace invariants_test;

TEST_CASE("padding under a raw writer is named", "[ecs][invariants]") {
	if (!AuditChecksPadding()) {
		SKIP("this toolchain has no __builtin_clear_padding, so the padding rule is unchecked");
	}

	Components::Register<Gappy>("test.Gappy");

	const std::vector<std::string> rules = RulesFor("test.Gappy");
	REQUIRE(Mentions(rules, "padding"));
	REQUIRE(Mentions(rules, "object representation"));
}

TEST_CASE("padding behind a writer of its own is not", "[ecs][invariants]") {
	Components::Register<GappyButWritten>("test.GappyButWritten", WriteGappy, ReadGappy);

	// The point of the `RawSerialisation` flag. This type has exactly the
	// padding the case above complains about; what differs is that no path puts
	// those bytes anywhere.
	REQUIRE(RulesFor("test.GappyButWritten").empty());
}

TEST_CASE("a member nothing initialises is named", "[ecs][invariants]") {
	Components::Register<Uninitialised>("test.Uninitialised");

	REQUIRE(Mentions(RulesFor("test.Uninitialised"), "without ever being"));
}

TEST_CASE("a reader that consumes less than its writer produced is named", "[ecs][invariants]") {
	Components::Register<Lopsided>("test.Lopsided", WriteLopsided, ReadLopsided);

	REQUIRE(Mentions(RulesFor("test.Lopsided"), "different number of bytes"));
}

TEST_CASE("a wire format that lies about its width is named", "[ecs][invariants]") {
	engine::ecs::WireFormat wire;
	wire.Size = 2;
	wire.Write = [](ByteWriter &writer, const void *source, size_t count) {
		const auto *values = static_cast<const Miscounted *>(source);
		for (size_t index = 0; index < count; index++) {
			writer.WriteUInt32(values[index].Value);
		}
	};
	wire.Read = [](ByteReader &reader, void *destination, size_t count) {
		auto *values = static_cast<Miscounted *>(destination);
		for (size_t index = 0; index < count; index++) {
			values[index].Value = reader.ReadUInt32();
		}
	};

	Components::Register<Miscounted>("test.Miscounted", wire);

	REQUIRE(Mentions(RulesFor("test.Miscounted"), "declares a wire size of 2"));
}

TEST_CASE("a wire decoder that is not total is named", "[ecs][invariants]") {
	engine::ecs::WireFormat wire;
	wire.Size = 4;
	wire.Write = [](ByteWriter &writer, const void *source, size_t count) {
		const auto *values = static_cast<const Partial *>(source);
		for (size_t index = 0; index < count; index++) {
			writer.WriteUInt32(values[index].Value);
		}
	};

	// The bug this shape stands for: a decoder that trusts a byte from the
	// datagram as a length and reads that many more.
	wire.Read = [](ByteReader &reader, void *destination, size_t count) {
		auto *values = static_cast<Partial *>(destination);
		for (size_t index = 0; index < count; index++) {
			const uint32_t value = reader.ReadUInt32();
			values[index].Value = value;
			if (value != 0) {
				reader.Fail();
			}
		}
	};

	Components::Register<Partial>("test.Partial", wire);

	REQUIRE(Mentions(RulesFor("test.Partial"), "total over its input"));
}

TEST_CASE("every component this module registers obeys every rule", "[ecs][invariants]") {
	// **Kept to `ecs.`, which is this module's own.** The rest of this binary's
	// registry is the fixtures every other suite here declares, and several of
	// them are deliberately shaped in ways a real component must not be -
	// `column_test::Wide` is `alignas(64)` around one `double` precisely so
	// that a column of it spans chunks. Sweeping those would be asking a
	// scaffold to be a building.
	//
	// The unfiltered sweep is `client.componentinvariants` and
	// `server.componentinvariants`, run from binaries whose whole registry is
	// real. Between them and each module's own suite, nothing registered by
	// anything that ships goes unchecked.
	CHECK(engine::ecs::Describe(AuditComponents("ecs.")) == "");
}

// --- the class table ------------------------------------------------------
//
// The same shape as above: one class carrying one deliberately wrong property
// per rule, so that each rule is watched failing before the sweep over the real
// table is trusted for saying nothing.

namespace invariants_test {
	struct Dial {
		float Value = 0.5f;
	};

	// Declared once and shared, because `Classes::Register` is idempotent and a
	// per-case class would be a new one each time the case ran.
	ClassId Widget() {
		static const ClassId id = [] {
			const ComponentId dial = Components::Register<Dial>("test.Dial");
			const ComponentId ids[] = {dial};
			return Classes::Register("TestWidget", ids);
		}();
		return id;
	}

	PropertyDescriptor DialProperty(std::string_view name) {
		PropertyDescriptor property;
		property.Name = engine::core::Name(name);
		property.Type = PropertyType::Float;
		property.Size = sizeof(float);
		property.Kind = PropertyKind::Computed;
		property.Reads = &engine::ecs::ComponentSet::Intern({Components::Register<Dial>("test.Dial")});
		property.Writes = property.Reads;
		property.Get = [](const Store &store, Entity instance, void *out) -> bool {
			const Dial *dial = store.Get<Dial>(instance);
			if (dial == nullptr) {
				return false;
			}
			*static_cast<float *>(out) = dial->Value;
			return true;
		};
		return property;
	}

	std::vector<std::string> PropertyRulesFor(std::string_view name) {
		std::vector<std::string> found;
		for (const PropertyComplaint &complaint : engine::ecs::AuditProperties()) {
			if (complaint.Property.Text() == name) {
				found.push_back(complaint.Rule);
			}
		}
		return found;
	}
}

TEST_CASE("a setter that reaches the bytes behind the store's back is named", "[ecs][invariants]") {
	PropertyDescriptor property = DialProperty("Unmarked");

	// `const_cast` off a `Get` rather than `GetMutable`, which is the shape of
	// the mistake: the value lands, the column is never marked, and a delta
	// carries nothing. It compiles, it works when you try it by hand, and it
	// desyncs every client.
	property.Set = [](Store &store, Entity instance, const void *value) -> bool {
		const Dial *dial = store.Get<Dial>(instance);
		if (dial == nullptr) {
			return false;
		}
		const_cast<Dial *>(dial)->Value = *static_cast<const float *>(value);
		return true;
	};

	Classes::Computed(Widget(), property);

	REQUIRE(Mentions(PropertyRulesFor("Unmarked"), "marks none of the components"));
}

TEST_CASE("a read-only property whose setter writes is named", "[ecs][invariants]") {
	PropertyDescriptor property = DialProperty("Frozen");
	property.Writable = false;
	property.Set = [](Store &store, Entity instance, const void *value) -> bool {
		Dial *dial = store.GetMutable<Dial>(instance);
		if (dial == nullptr) {
			return false;
		}
		dial->Value = *static_cast<const float *>(value);
		return true;
	};

	Classes::Computed(Widget(), property);

	REQUIRE(Mentions(PropertyRulesFor("Frozen"), "read-only and its setter accepted"));
}

TEST_CASE("a property that never settles is named", "[ecs][invariants]") {
	PropertyDescriptor property = DialProperty("Creeping");
	property.Set = [](Store &store, Entity instance, const void *value) -> bool {
		Dial *dial = store.GetMutable<Dial>(instance);
		if (dial == nullptr) {
			return false;
		}

		// A conversion that rounds settles after a trip or two. This one adds,
		// which is the difference the check is about.
		dial->Value = *static_cast<const float *>(value) + 1.0f;
		return true;
	};

	Classes::Computed(Widget(), property);

	REQUIRE(Mentions(PropertyRulesFor("Creeping"), "never settles"));
}

TEST_CASE("a getter that declines on an ordinary instance is named", "[ecs][invariants]") {
	PropertyDescriptor property = DialProperty("Unreadable");
	property.Writable = false;
	property.Get = [](const Store &, Entity, void *) -> bool { return false; };

	Classes::Computed(Widget(), property);

	REQUIRE(Mentions(PropertyRulesFor("Unreadable"), "cannot be read from an ordinary instance"));
}

TEST_CASE("every property this module declares obeys every rule", "[ecs][invariants]") {
	// `TestWidget` is the class the cases above hang their broken properties
	// on, so it is the one thing excluded. Everything else in this binary's
	// class table is `ecs`'s own.
	std::vector<PropertyComplaint> real;
	for (const PropertyComplaint &complaint : engine::ecs::AuditProperties()) {
		if (complaint.Class.Text() != "TestWidget") {
			real.push_back(complaint);
		}
	}

	CHECK(engine::ecs::Describe(real) == "");
}

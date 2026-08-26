// What a beam and a trail cost on the far side of a serialiser, measured.
//
// **The number that matters about `effects::Trail` is not `sizeof`.** The row is
// 1152 bytes and 448 of them are the ring of recorded edge points, which reads
// like a large saved row with a large hole in it - and `docs/ARCH_REVIEW.md` §D3
// recorded it that way, as "448 hot bytes inside a 1068-byte saved row", with a
// proposal to split the history into a component of its own.
//
// The saved row is nothing like 1068 bytes. **It is 82**, measured by the case
// below. `WriteTrails` walks the authored fields one at a time and never touches
// the history at all, so what reaches an `.agame` is the two sequences, a
// texture name, three floats and two flags - and `ReadTrails` puts the ring back
// at empty on the way in, because a recording of where something has been is a
// fact about a run and not about a world. These cases measure that rather than
// describing it, so the split stays declined for a reason a reader can check.
//
// The same applies to the attachments: an `ecs::Entity` is a handle within one
// world, so neither writer carries one and both readers clear them.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

TEST_SUITE_ID("engine.effects.ribbon")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::TypeDescriptor;
using engine::effects::Beam;
using engine::effects::Trail;
using engine::effects::TRAIL_POINTS;

namespace {
	// How many bytes one value of a registered component writes.
	size_t Written(const char *component, const void *value) {
		const TypeDescriptor &type = Components::Describe(Components::Find(Name(component)));
		REQUIRE(type.Serialisable);
		ByteWriter writer;
		type.Write(writer, value, 1);
		return writer.Size();
	}
}

TEST_CASE("a trail's history never reaches a file", "[effects][ribbon]") {
	engine::effects::RegisterEffectComponents();

	Trail full;
	full.Recorded = TRAIL_POINTS;
	full.Head = 3;
	for (uint32_t index = 0; index < TRAIL_POINTS; index++) {
		full.Top[index] = engine::core::Vector3{static_cast<float>(index), 1.0f, 0.0f};
		full.Bottom[index] = engine::core::Vector3{static_cast<float>(index), 0.0f, 0.0f};
		full.Age[index] = 0.1f * static_cast<float>(index);
	}

	Trail empty;
	empty.Colour = full.Colour;
	empty.Transparency = full.Transparency;

	// **The whole claim in one line: a full ring and an empty one write the
	// same bytes.** So the history is not in the file, and moving it to a
	// component of its own would remove nothing from one.
	CHECK(Written("effects.Trail", &full) == Written("effects.Trail", &empty));

	// And the size itself, so "much smaller than the row" is a number rather
	// than an adjective. Two default sequences of two keypoints each is 64
	// bytes, an empty texture name is 4, and the three floats and two flags are
	// 14: **82 against a 1152-byte row.**
	//
	// Pinned rather than bounded, because the point of the case is the figure.
	// A change here is a save-format change and should arrive as one.
	const size_t saved = Written("effects.Trail", &empty);
	INFO("effects.Trail: " << sizeof(Trail) << " bytes in the store, " << saved << " on the way to a file");
	CHECK(saved == 82);
}

TEST_CASE("a trail restored from a file starts with no history", "[effects][ribbon]") {
	engine::effects::RegisterEffectComponents();

	Trail full;
	full.Recorded = TRAIL_POINTS;
	full.Head = 7;
	full.Lifetime = 2.5f;
	full.MinimumAngle = 30.0f;
	full.Attachment0 = Entity{4};
	full.Attachment1 = Entity{5};

	const TypeDescriptor &type = Components::Describe(Components::Find(Name("effects.Trail")));
	ByteWriter writer;
	type.Write(writer, &full, 1);

	Trail restored;
	restored.Recorded = 9;
	restored.Head = 2;
	ByteReader reader(writer.Bytes());
	type.Read(reader, &restored, 1);

	// The authored half survives.
	CHECK(restored.Lifetime == 2.5f);
	CHECK(restored.MinimumAngle == 30.0f);

	// The recorded half does not, and neither do the handles: a ring of world
	// positions describes a run, and an `ecs::Entity` is a directory index
	// inside one world.
	CHECK(restored.Recorded == 0u);
	CHECK(restored.Head == 0u);
	CHECK(restored.Attachment0 == engine::ecs::NULL_ENTITY);
	CHECK(restored.Attachment1 == engine::ecs::NULL_ENTITY);
}

TEST_CASE("a beam's attachments do not survive a file either", "[effects][ribbon]") {
	engine::effects::RegisterEffectComponents();

	Beam beam;
	beam.Attachment0 = Entity{11};
	beam.Attachment1 = Entity{12};
	beam.Width0 = 3.0f;

	const TypeDescriptor &type = Components::Describe(Components::Find(Name("effects.Beam")));
	ByteWriter writer;
	type.Write(writer, &beam, 1);

	Beam restored;
	restored.Attachment0 = Entity{99};
	ByteReader reader(writer.Bytes());
	type.Read(reader, &restored, 1);

	CHECK(restored.Width0 == 3.0f);
	CHECK(restored.Attachment0 == engine::ecs::NULL_ENTITY);
	CHECK(restored.Attachment1 == engine::ecs::NULL_ENTITY);
}

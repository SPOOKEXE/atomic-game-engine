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
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
using engine::effects::Decal;
using engine::effects::RibbonBuffer;
using engine::effects::Texture;
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

TEST_CASE("decals and textures keep every authored field across a file", "[effects][ribbon]") {
	engine::effects::RegisterEffectComponents();

	Decal decal;
	decal.Colour = engine::core::Color3{0.2f, 0.4f, 0.6f};
	decal.Image = Name("sign.atex");
	decal.Transparency = 0.25f;
	decal.ZIndex = 7;
	decal.Face = engine::scene::NormalId::Top;

	const TypeDescriptor &decalType = Components::Describe(Components::Find(Name("effects.Decal")));
	ByteWriter decalBytes;
	decalType.Write(decalBytes, &decal, 1);
	Decal restoredDecal;
	ByteReader decalReader(decalBytes.Bytes());
	decalType.Read(decalReader, &restoredDecal, 1);
	CHECK(restoredDecal.Colour == decal.Colour);
	CHECK(restoredDecal.Image == decal.Image);
	CHECK(restoredDecal.Transparency == decal.Transparency);
	CHECK(restoredDecal.ZIndex == decal.ZIndex);
	CHECK(restoredDecal.Face == decal.Face);

	Texture texture;
	texture.Image = Name("tiles.atex");
	texture.StudsPerTileU = 3.0f;
	texture.StudsPerTileV = 4.0f;
	texture.OffsetStudsU = 0.5f;
	texture.OffsetStudsV = -1.0f;
	texture.Face = engine::scene::NormalId::Right;

	const TypeDescriptor &textureType = Components::Describe(Components::Find(Name("effects.Texture")));
	ByteWriter textureBytes;
	textureType.Write(textureBytes, &texture, 1);
	Texture restoredTexture;
	ByteReader textureReader(textureBytes.Bytes());
	textureType.Read(textureReader, &restoredTexture, 1);
	CHECK(restoredTexture.Image == texture.Image);
	CHECK(restoredTexture.StudsPerTileU == texture.StudsPerTileU);
	CHECK(restoredTexture.StudsPerTileV == texture.StudsPerTileV);
	CHECK(restoredTexture.OffsetStudsU == texture.OffsetStudsU);
	CHECK(restoredTexture.OffsetStudsV == texture.OffsetStudsV);
	CHECK(restoredTexture.Face == texture.Face);
}

TEST_CASE("face images become one quad on their parent part", "[effects][ribbon]") {
	using Catch::Approx;
	using engine::core::Vector3;
	using engine::ecs::Classes;
	using engine::ecs::Store;

	engine::effects::RegisterEffectClasses();
	Store store("ribbon.face-images");
	engine::scene::PartDesc partDesc;
	partDesc.Frame = engine::core::CFrame{Vector3{10.0f, 20.0f, 30.0f}};
	partDesc.Size = Vector3{4.0f, 2.0f, 6.0f};
	const Entity part = engine::scene::MakePart(store, partDesc);

	const Entity decal = store.CreateInstance(Classes::Find(Name("Decal")), "Sign");
	REQUIRE(store.SetParent(decal, part));
	Decal *decalRow = store.GetMutable<Decal>(decal);
	REQUIRE(decalRow != nullptr);
	decalRow->Image = Name("sign.atex");
	decalRow->Face = engine::scene::NormalId::Front;

	const Entity texture = store.CreateInstance(Classes::Find(Name("Texture")), "Tiles");
	REQUIRE(store.SetParent(texture, part));
	Texture *textureRow = store.GetMutable<Texture>(texture);
	REQUIRE(textureRow != nullptr);
	textureRow->Image = Name("tiles.atex");
	textureRow->Face = engine::scene::NormalId::Top;
	textureRow->StudsPerTileU = 2.0f;
	textureRow->StudsPerTileV = 2.0f;

	store.SetResource(RibbonBuffer{});
	CHECK(engine::effects::BuildRibbons(store, Vector3::Zero, 0.0f) == 2);

	const auto vertices = engine::effects::RibbonStream(store);
	const auto runs = engine::effects::RibbonRuns(store);
	REQUIRE(vertices.size() == 8);
	REQUIRE(runs.size() == 2);
	CHECK(runs[0].Texture == Name("sign.atex"));
	CHECK_FALSE(runs[0].RepeatV);
	CHECK(runs[1].Texture == Name("tiles.atex"));
	CHECK(runs[1].RepeatV);

	// Front is local -Z, three metres from the centre. The quad spans the
	// part's full X and Y face.
	for (size_t index = 0; index < 4; index++) {
		CHECK(vertices[index].Position.Z == Approx(27.0f));
	}
	CHECK(std::abs(vertices[0].Position.X - vertices[2].Position.X) == Approx(4.0f));
	CHECK(std::abs(vertices[0].Position.Y - vertices[1].Position.Y) == Approx(2.0f));

	// The top face is four by six studs, so two-stud tiles repeat twice and
	// three times respectively.
	CHECK(vertices[6].Coordinate.X - vertices[4].Coordinate.X == Approx(2.0f));
	CHECK(vertices[5].Coordinate.Y - vertices[4].Coordinate.Y == Approx(3.0f));
}

TEST_CASE(
	"request ribbons face their own camera without overwriting retained world geometry",
	"[effects][ribbon][request-ribbons]"
) {
	using namespace engine;
	effects::RegisterEffectClasses();
	ecs::Store store("request-ribbons");
	std::array<Entity, 2> ends;
	for (size_t index = 0; index < ends.size(); ++index) {
		ends[index] = store.CreateInstance(ecs::Classes::Find(Name("Attachment")), "End");
		scene::Attachment attachment;
		attachment.Frame.Position = {index == 0 ? -2.0f : 2.0f, 0, -3};
		store.Set(ends[index], attachment);
	}
	const auto entity = store.CreateInstance(ecs::Classes::Find(Name("Beam")), "Beam");
	Beam beam;
	beam.Attachment0 = ends[0];
	beam.Attachment1 = ends[1];
	beam.Width0 = beam.Width1 = 2;
	beam.FaceCamera = true;
	store.Set(entity, beam);
	store.SetResource(RibbonBuffer{});
	REQUIRE(effects::BuildRibbons(store, {0, 100, -3}, 0) == 1);
	const auto retained = effects::RibbonStream(store);
	REQUIRE(!retained.empty());
	CHECK(retained.front().Position.Y == 0);
	const auto previousPosition = retained.front().Position;
	RibbonBuffer request;
	REQUIRE(effects::BuildRibbons(store, {0, 0, 0}, 0, request) == 1);
	REQUIRE(request.Vertices.size() == retained.size());
	CHECK(std::abs(request.Vertices.front().Position.Y) == Catch::Approx(1));
	CHECK(request.Vertices.front().Position.Z == Catch::Approx(-3));
	CHECK(effects::RibbonStream(store).front().Position == previousPosition);
	const auto *allocation = request.Vertices.data();
	REQUIRE(effects::BuildRibbons(store, {0, 0, 0}, 1, request) == 1);
	CHECK(request.Vertices.data() == allocation);
	CHECK(effects::RibbonStream(store).front().Position == previousPosition);
}

TEST_CASE(
	"child ribbon orientation matches a fresh curved tapered beam and preserves fixed strips",
	"[effects][ribbon][request-ribbons]"
) {
	using namespace engine;
	effects::RegisterEffectClasses();
	ecs::Store store("child-ribbons");
	std::array<Entity, 2> ends;
	for (size_t index = 0; index < ends.size(); ++index) {
		ends[index] = store.CreateInstance(ecs::Classes::Find(Name("Attachment")), "End");
		scene::Attachment attachment;
		attachment.Frame.Position = {index == 0 ? -2.0f : 2.0f, 0, -3};
		store.Set(ends[index], attachment);
	}
	const auto entity = store.CreateInstance(ecs::Classes::Find(Name("Beam")), "Beam");
	Beam beam;
	beam.Attachment0 = ends[0];
	beam.Attachment1 = ends[1];
	beam.Width0 = .3f;
	beam.Width1 = 3;
	beam.CurveSize0 = 1.25f;
	beam.CurveSize1 = -.75f;
	SECTION("camera-facing curved tapered strip") {
		beam.FaceCamera = true;
	}
	SECTION("authored strip orientation") {
		beam.FaceCamera = false;
	}
	store.Set(entity, beam);
	RibbonBuffer original, reference;
	REQUIRE(effects::BuildRibbons(store, {0, 100, -3}, .25f, original) == 1);
	REQUIRE(effects::BuildRibbons(store, {1, 0, 0}, .25f, reference) == 1);
	std::vector<effects::RibbonVertex> child;
	REQUIRE(effects::FaceRibbonVertices(original.Vertices, original.Runs, {1, 0, 0}, child));
	REQUIRE(child.size() == reference.Vertices.size());
	for (size_t index = 0; index < child.size(); ++index) {
		CHECK((child[index].Position - reference.Vertices[index].Position).Magnitude() < 1e-5f);
		CHECK(child[index].Coordinate == reference.Vertices[index].Coordinate);
		CHECK(child[index].Colour == reference.Vertices[index].Colour);
	}
	const auto *allocation = child.data();
	REQUIRE(effects::FaceRibbonVertices(child, original.Runs, {0, 100, -3}, child));
	CHECK(child.data() == allocation);
	for (size_t index = 0; index < child.size(); ++index) {
		CHECK((child[index].Position - original.Vertices[index].Position).Magnitude() < 1e-5f);
	}
	const auto beforeRefusal = child.front().Position;
	original.Runs.front().Count = UINT32_MAX;
	CHECK_FALSE(effects::FaceRibbonVertices(original.Vertices, original.Runs, {}, child));
	CHECK(child.front().Position == beforeRefusal);
}

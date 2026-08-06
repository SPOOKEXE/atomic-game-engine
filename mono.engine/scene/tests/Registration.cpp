#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.scene.registration")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Classes;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::ecs::TypeDescriptor;
using engine::scene::ActiveCamera;
using engine::scene::RegisterSceneClasses;
using engine::scene::RegisterSceneComponents;
using engine::scene::Surface;
using engine::scene::SurfaceProperties;
using engine::scene::SurfaceTable;
using engine::scene::Transform;
using engine::scene::Visual;

namespace registration_test {
	// Every name this module promises. A file, a wire and a recording all carry
	// these strings, so renaming one is a format change and this list is what
	// makes it show up as a failing test rather than as a snapshot that loads
	// into a narrower world.
	const std::vector<std::string_view> EXPECTED{
		"scene.Transform",
		"scene.PreviousTransform",
		"scene.Bounds",
		"scene.Motion",
		"scene.RigidBody",
		"scene.Collider",
		"scene.Surface",
		"scene.Visual",
		"scene.Camera",
		"scene.QuickHash",
		"scene.SurfaceCamera",
		"scene.Transient",
		"scene.Service",
		"scene.LightingService",
		"scene.Rendered",
		"scene.SurfaceTable",
		"scene.ActiveCamera",
		"scene.WorldBounds",
		"scene.RenderedSignature",
	};
}

TEST_CASE("every type is registered under its explicit name", "[scene][registration]") {
	RegisterSceneComponents();

	for (const std::string_view expected : registration_test::EXPECTED) {
		const ComponentId id = Components::Find(Name(expected));
		INFO(expected);
		REQUIRE(id.IsValid());

		// Explicit rather than the compiler's spelling, which differs between
		// compilers and would put `engine::scene::Transform` as GCC happens to
		// print it into a recording.
		CHECK(Components::Describe(id).Name.Text() == expected);
	}
}

TEST_CASE("registering twice mints nothing new", "[scene][registration]") {
	RegisterSceneComponents();

	const size_t before = Components::Count();
	const ComponentId transform = Components::Find(Name("scene.Transform"));

	RegisterSceneComponents();
	RegisterSceneComponents();

	CHECK(Components::Count() == before);
	CHECK(Components::Find(Name("scene.Transform")) == transform);
}

TEST_CASE("registering classes registers the components first", "[scene][registration]") {
	// A class is a set of component ids, so it cannot be declared before they
	// exist. Making the class entry point do the ordering is what stops a
	// caller getting it wrong.
	RegisterSceneClasses();

	CHECK(Classes::Find(Name("Part")).IsValid());
	CHECK(Components::Find(Name("scene.Collider")).IsValid());
}

TEST_CASE("everything registered here can be snapshotted", "[scene][registration]") {
	// `Store::Save` refuses outright when a world holds one component with no
	// serialisation, so a single unregistered writer here takes out every
	// recording, replay and world migration rather than just its own type.
	RegisterSceneComponents();

	for (const std::string_view expected : registration_test::EXPECTED) {
		const TypeDescriptor &descriptor = Components::Describe(Components::Find(Name(expected)));
		INFO(expected);
		CHECK(descriptor.Serialisable);
	}
}

TEST_CASE("a name-carrying component crosses as text, not as an id", "[scene][registration]") {
	// The reason `Surface` and `Visual` are registered with explicit writers. A
	// name's id is a counter this process assigned in first-seen order; written
	// raw it would restore in another process as whatever string happened to
	// take the same number, which is a file that loads and is wrong.
	RegisterSceneComponents();

	Store source("registration_test.source");
	const Entity entity = source.Create();
	source.Set(entity, Transform{CFrame(Vector3(1.0f, 2.0f, 3.0f))});
	source.Set(entity, Surface{Name("registration_test.Granite")});

	Visual visual;
	visual.Mesh = Name("registration_test.Column");
	visual.Visible = false;
	source.Set(entity, visual);

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	// Interning a fresh string between the two halves shifts every id assigned
	// afterwards, so a snapshot that had written ids would come back naming
	// something else. This line is what makes the case prove anything.
	const Name shifter("registration_test.ShiftsTheIdSpace");
	CHECK(shifter.IsValid());

	Store restored("registration_test.restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const Surface *surface = restored.Get<Surface>(entity);
	REQUIRE(surface != nullptr);
	CHECK(surface->Material.Text() == "registration_test.Granite");

	const Visual *back = restored.Get<Visual>(entity);
	REQUIRE(back != nullptr);
	CHECK(back->Mesh.Text() == "registration_test.Column");
	CHECK_FALSE(back->Visible);
}

TEST_CASE("the surface table crosses a snapshot in order", "[scene][registration]") {
	// A resource holding a vector and a name has no raw representation worth
	// writing, and `Save` refuses a world containing one without a writer. Its
	// order has to survive too: the table is stored in program order precisely
	// so two runs agree, and a restore that reordered it would break that
	// quietly.
	RegisterSceneComponents();

	SurfaceTable table;
	table.Set(Name("registration_test.Ice"), SurfaceProperties{0.02f, 0.1f});
	table.Set(Name("registration_test.Tar"), SurfaceProperties{1.4f, 0.0f});

	Store source("registration_test.table.source");
	source.SetResource(table);

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("registration_test.table.restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const SurfaceTable *back = restored.Resource<SurfaceTable>();
	REQUIRE(back != nullptr);
	REQUIRE(back->Rows.size() == 2);
	CHECK(back->Rows[0].Material.Text() == "registration_test.Ice");
	CHECK(back->Rows[1].Material.Text() == "registration_test.Tar");
	CHECK(back->Rows[1].Properties.Friction == 1.4f);
}

TEST_CASE("the active camera resource survives a snapshot", "[scene][registration]") {
	// Registered because a resource is keyed by a component id too — one that
	// is never named here would be minted by the first `SetResource` under the
	// compiler's spelling, and would abort once the table is sealed.
	RegisterSceneComponents();

	Store source("registration_test.camera.source");
	ActiveCamera active;
	active.AspectRatio = 16.0f / 9.0f;
	source.SetResource(active);

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("registration_test.camera.restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const ActiveCamera *back = restored.Resource<ActiveCamera>();
	REQUIRE(back != nullptr);
	CHECK(back->AspectRatio == 16.0f / 9.0f);
}

// **The test that catches the field somebody forgets, rather than the field
// somebody already forgot.**
//
// `Visual` is registered with a hand-written writer and reader, because it
// holds `core::Name`s that have to cross as text. The cost of that pair is that
// a field added to the struct crosses only if a person remembers to add two
// lines, and nothing in the build checks — so the field silently resets to its
// default on every load, which looks like a bug in whatever reads it.
//
// This has happened three times. `Transparency` and `Surface` were both added
// at v0.6 and neither was written, so a glass pane turned solid and a mirror
// went blank the first time a world was saved and reopened. `CastShadow`
// arrived at v0.7 and was written the same day, because of this.
//
// The check is per field and it is a *difference* rather than a round trip: two
// visuals that differ in exactly one field must produce different bytes. A
// round-trip test passes for a field the writer skips whenever the reader
// leaves the default in place, which is exactly the case that goes wrong.
//
// Adding a field to `Visual` and not adding a case here leaves it untested —
// which is why `engine.scene.components` pins `sizeof(Visual)`. That assertion
// fails first, and it fails in a file whose comment points back at this one.
TEST_CASE("every field of Visual reaches the wire", "[scene][registration]") {
	RegisterSceneComponents();

	const TypeDescriptor &descriptor = Components::Describe(Components::Find(Name("scene.Visual")));
	REQUIRE(descriptor.Write != nullptr);
	REQUIRE(descriptor.Read != nullptr);

	const auto written = [&descriptor](const Visual &visual) {
		ByteWriter writer;
		descriptor.Write(writer, &visual, 1);
		return std::vector<std::byte>(writer.Bytes().begin(), writer.Bytes().end());
	};

	const Visual base;
	const std::vector<std::byte> reference = written(base);

	// One mutation per field, each away from the default so that a writer which
	// skipped it would produce the reference bytes unchanged.
	struct Case {
		std::string_view Field;
		Visual Value;
	};

	std::vector<Case> cases;
	{
		Visual tint = base;
		tint.Tint = engine::core::Color3{0.1f, 0.2f, 0.3f};
		cases.push_back({"Tint", tint});

		Visual mesh = base;
		mesh.Mesh = Name("registration_test.Mesh");
		cases.push_back({"Mesh", mesh});

		Visual material = base;
		material.Material = Name("registration_test.Material");
		cases.push_back({"Material", material});

		Visual transparency = base;
		transparency.Transparency = 0.5f;
		cases.push_back({"Transparency", transparency});

		Visual visible = base;
		visible.Visible = !base.Visible;
		cases.push_back({"Visible", visible});

		Visual surface = base;
		surface.Surface = 3;
		cases.push_back({"Surface", surface});

		Visual shadow = base;
		shadow.CastShadow = !base.CastShadow;
		cases.push_back({"CastShadow", shadow});
	}

	for (const Case &one : cases) {
		INFO(one.Field);
		CHECK(written(one.Value) != reference);
	}

	// And the whole thing survives the round trip, which is the half the
	// difference check cannot make on its own: bytes that change are not
	// necessarily bytes that come back.
	Visual authored;
	authored.Tint = engine::core::Color3{0.1f, 0.2f, 0.3f};
	authored.Mesh = Name("registration_test.Mesh");
	authored.Material = Name("registration_test.Material");
	authored.Transparency = 0.5f;
	authored.Visible = false;
	authored.Surface = 3;
	authored.CastShadow = false;

	const std::vector<std::byte> bytes = written(authored);
	ByteReader reader(bytes);

	Visual restored;
	descriptor.Read(reader, &restored, 1);

	CHECK(restored.Tint.R == authored.Tint.R);
	CHECK(restored.Tint.G == authored.Tint.G);
	CHECK(restored.Tint.B == authored.Tint.B);
	CHECK(restored.Mesh == authored.Mesh);
	CHECK(restored.Material == authored.Material);
	CHECK(restored.Transparency == authored.Transparency);
	CHECK(restored.Visible == authored.Visible);
	CHECK(restored.Surface == authored.Surface);
	CHECK(restored.CastShadow == authored.CastShadow);
}

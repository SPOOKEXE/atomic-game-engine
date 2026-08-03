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
		"scene.SurfaceTable",
		"scene.ActiveCamera",
		"scene.WorldBounds",
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

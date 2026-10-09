#include <engine/gui/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/ImageGraph.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <studio/Commands.hpp>
#include <studio/ImageComposer.hpp>

TEST_SUITE_ID("studio.imagecomposer.apply")
TEST_DEPENDS("engine.scene.imagegraph")
TEST_DEPENDS("studio.commands")

namespace {
	using namespace engine;
	game::PropertyValue Image(ecs::Store &store, ecs::Entity instance) {
		game::PropertyValue value;
		for (const auto &field : store.PropertiesOf(instance))
			if (field.Name == core::Name("Image")) {
				REQUIRE(game::ReadProperty(store, instance, field, value));
				return value;
			}
		FAIL("Image property was not found");
		return value;
	}
	const ecs::PropertyDescriptor &Inputs(ecs::Store &store, ecs::Entity instance) {
		for (const auto &field : store.PropertiesOf(instance))
			if (field.Name == core::Name("Inputs")) return field;
		throw std::runtime_error("Inputs property was not found");
	}
}

TEST_CASE("live image application and its controller undo as one saved edit", "[studio][imagecomposer]") {
	using namespace engine;
	parallel::Jobs::Start(1);
	struct StopJobs {
		~StopJobs() {
			parallel::Jobs::Stop();
		}
	} stop;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	world::Universe universe;
	world::WorldSettings settings;
	settings.Name = core::Name("composer.apply");
	const auto world = universe.Create(settings);
	studio::CommandLog log(universe);
	std::array<ecs::Entity, 2> labels;
	universe.Enter(world, [&](ecs::Store &store) {
		scene::InstallServices(store);
		labels = {
			store.CreateInstance(ecs::Classes::Find(core::Name("ImageLabel")), "First"),
			store.CreateInstance(ecs::Classes::Find(core::Name("ImageButton")), "Second")
		};
	});
	const auto recording = log.TryBeginRecording("composer.apply", "Apply live image");
	REQUIRE(recording.has_value());
	universe.Enter(world, [&](ecs::Store &store) {
		ecs::Entity controller = ecs::NULL_ENTITY;
		std::vector<studio::ImageComposerWrite> writes;
		std::string failure;
		REQUIRE(
			studio::ApplyImageComposerSelection(
				store,
				labels,
				core::Name("Image"),
				core::Name("published.aimagegraph"),
				core::Name("image"),
				core::Name("saved-controller"),
				true,
				controller,
				writes,
				failure
			)
		);
		REQUIRE(writes.size() == 2);
		CHECK(store.ParentOf(controller) == scene::WorkspaceOf(store));
		CHECK(store.Get<scene::ImageGraph>(controller)->Inputs.empty());
		log.RecordCreate(store, world, controller, "Create ImageGraph");
		for (const auto &write : writes)
			log.RecordProperty(
				world, write.Instance, core::Name("Image"), write.Before, write.After, "Apply live image"
			);
		CHECK(Image(store, labels[0]).Name.Text() == "imagegraph-instance://saved-controller#image");
		CHECK(Image(store, labels[1]).Name == Image(store, labels[0]).Name);
	});
	REQUIRE(log.FinishRecording(*recording, studio::FinishOperation::Commit));
	REQUIRE(log.Undo());
	universe.Enter(world, [&](ecs::Store &store) {
		CHECK_FALSE(Image(store, labels[0]).Name.IsValid());
		CHECK_FALSE(Image(store, labels[1]).Name.IsValid());
		size_t controllers = 0;
		store.Each<scene::ImageGraph>([&](ecs::Entity, const scene::ImageGraph &) { ++controllers; });
		CHECK(controllers == 0);
	});
	REQUIRE(log.Redo());
	universe.Enter(world, [&](ecs::Store &store) {
		CHECK(Image(store, labels[0]).Name.Text() == "imagegraph-instance://saved-controller#image");
		size_t controllers = 0;
		store.Each<scene::ImageGraph>([&](ecs::Entity controller, const scene::ImageGraph &value) {
			++controllers;
			CHECK(value.Graph.Text() == "published.aimagegraph");
			CHECK(scene::ImageGraphContentName(store, controller) == Image(store, labels[0]).Name);
		});
		CHECK(controllers == 1);
	});
}

TEST_CASE(
	"live image application refuses unauthorized or unsuitable targets atomically", "[studio][imagecomposer]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store store("composer.refusal");
	const auto label = store.CreateInstance(ecs::Classes::Find(core::Name("ImageLabel")), "Label");
	const auto part = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "Part");
	ecs::Entity created = ecs::NULL_ENTITY;
	std::vector<studio::ImageComposerWrite> writes;
	std::string failure;
	const std::array selected{label};
	CHECK_FALSE(
		studio::ApplyImageComposerSelection(
			store,
			selected,
			core::Name("Image"),
			core::Name("published.aimagegraph"),
			core::Name("image"),
			core::Name("identity"),
			false,
			created,
			writes,
			failure
		)
	);
	const std::array mixed{label, part};
	CHECK_FALSE(
		studio::ApplyImageComposerSelection(
			store,
			mixed,
			core::Name("Image"),
			core::Name("published.aimagegraph"),
			core::Name("image"),
			core::Name("identity"),
			true,
			created,
			writes,
			failure
		)
	);
	CHECK_FALSE(Image(store, label).Name.IsValid());
	CHECK(created == ecs::NULL_ENTITY);
	CHECK(writes.empty());
	size_t controllers = 0;
	store.Each<scene::ImageGraph>([&](ecs::Entity, const scene::ImageGraph &) { ++controllers; });
	CHECK(controllers == 0);
}

TEST_CASE(
	"named scene input edits validate graph types and preserve undo values", "[studio][imagecomposer]"
) {
	using namespace engine;
	scene::ImageGraphClass();
	ecs::Store store("composer.inputs");
	const auto instance = store.CreateInstance(scene::ImageGraphClass(), "Controller");
	imagegraph::Document graph;
	graph.Nodes = {{"solid", imagegraph::Solid{1, 1}, {}, {}}};
	graph.Outputs = {{"image", "solid"}};
	graph.Parameters = {{"width", 1.}, {"tint", std::array<uint8_t, 4>{1, 2, 3, 255}}};
	graph.Bindings = {{"solid", "width", "width"}, {"solid", "colour", "tint"}};
	game::PropertyValue before, after;
	std::string failure;
	CHECK_FALSE(
		studio::EditImageGraphInstanceInput(
			store, instance, {"width", 2.}, false, false, &graph, before, after, failure
		)
	);
	CHECK_FALSE(
		studio::EditImageGraphInstanceInput(
			store, instance, {"width", true}, false, true, &graph, before, after, failure
		)
	);
	CHECK(store.Get<scene::ImageGraph>(instance)->Inputs.empty());
	REQUIRE(
		studio::EditImageGraphInstanceInput(
			store, instance, {"width", 2.}, false, true, &graph, before, after, failure
		)
	);
	CHECK(store.Get<scene::ImageGraph>(instance)->Inputs.front().Number == 2.);
	REQUIRE(game::WriteAuthoredProperty(store, instance, Inputs(store, instance), before));
	CHECK(store.Get<scene::ImageGraph>(instance)->Inputs.empty());
	REQUIRE(game::WriteAuthoredProperty(store, instance, Inputs(store, instance), after));
	CHECK(store.Get<scene::ImageGraph>(instance)->Inputs.front().Number == 2.);
	REQUIRE(
		studio::EditImageGraphInstanceInput(
			store, instance, {"width", 0.}, true, true, &graph, before, after, failure
		)
	);
	CHECK(store.Get<scene::ImageGraph>(instance)->Inputs.empty());
}

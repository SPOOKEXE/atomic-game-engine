#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/DataFactoryRenderOnly.hpp>

TEST_SUITE_ID("client.data-factory.render-only")

TEST_CASE("client render-only adapter holds one copied preserve request", "[client][data-factory]") {
	client::data_factory_render_only::Queue queue;
	engine::world::DataFactoryRenderOnlyRequest request{
		.InstanceId = "data-world",
		.SnapshotId = "snapshot-1-1",
		.ExpectedWorldEpoch = 1,
		.ExpectedWorldVersion = 4,
		.ExpectedTick = 12,
	};
	std::string detail;
	REQUIRE(queue.Enqueue(request, detail));
	REQUIRE(queue.Pending());
	REQUIRE(queue.Request() != nullptr);
	CHECK(queue.Request()->InstanceId == "data-world");
	CHECK(queue.Request()->SnapshotId == "snapshot-1-1");
	CHECK(queue.Request()->ExpectedWorldEpoch == 1);
	CHECK(queue.Request()->ExpectedWorldVersion == 4);
	CHECK(queue.Request()->ExpectedTick == 12);
	CHECK(queue.Request()->TemporalHistory == engine::world::DataFactoryTemporalHistory::Preserve);
	CHECK_FALSE(queue.Enqueue(request, detail));
	CHECK(detail == "a render-only presentation is already queued");
	CHECK_FALSE(queue.AllowsInteractiveGui());
	CHECK(queue.ParticleDelta(0.25f) == 0.0f);

	queue.Consume();
	CHECK_FALSE(queue.Pending());
	CHECK(queue.Request() == nullptr);
	CHECK(queue.AllowsInteractiveGui());
	CHECK(queue.ParticleDelta(0.25f) == 0.25f);
}

TEST_CASE("client render-only adapter refuses policies the renderer cannot apply", "[client][data-factory]") {
	client::data_factory_render_only::Queue queue;
	engine::world::DataFactoryRenderOnlyRequest request;
	request.TemporalHistory = engine::world::DataFactoryTemporalHistory::Reset;
	std::string detail;
	CHECK_FALSE(queue.Enqueue(request, detail));
	CHECK(detail == "reset and disable temporal history require renderer-local history support");
	CHECK_FALSE(queue.Pending());
}

TEST_CASE(
	"client queued render-only frame keeps snapshot bytes unchanged while input waits",
	"[client][data-factory]"
) {
	engine::world::Universe worlds;
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("retained-gui");
	const engine::world::WorldId world = worlds.Create(settings);
	engine::core::ByteWriter before;
	REQUIRE(worlds.Save(before));

	client::data_factory_render_only::Queue queue;
	engine::world::DataFactoryRenderOnlyRequest request;
	request.InstanceId = "retained-gui";
	request.SnapshotId = "snapshot-1-1";
	std::string detail;
	REQUIRE(queue.Enqueue(request, detail));
	int inputCallbacks = 0;
	if (queue.AllowsInteractiveGui()) {
		inputCallbacks++;
		REQUIRE(
			worlds.SetState(world, engine::world::WorldState::Suspended) == engine::world::WorldStatus::Ok
		);
	}
	engine::core::ByteWriter after;
	REQUIRE(worlds.Save(after));
	CHECK(inputCallbacks == 0);
	REQUIRE(before.Bytes().size() == after.Bytes().size());
	CHECK(std::equal(before.Bytes().begin(), before.Bytes().end(), after.Bytes().begin()));
}

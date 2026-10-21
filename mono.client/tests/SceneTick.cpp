// The client's own tick, without a window.
//
// This is a client test rather than an engine one because what it covers is
// client code: the demo scene's components, resources and systems, and the
// shape of the data they hand to the renderer. It links several engine modules
// because the client does, not because it is testing them - each of those has
// its own suite under mono.engine/<module>/tests/.
//
// Headless on purpose. Everything up to the point where a GPU is required is
// testable on a machine with no GPU, and drawing the line there is what keeps
// the suite runnable everywhere.

#include "ScenarioChecks.hpp"
#include "TunnelsScenario.hpp"
#include "WorldSystems.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/WeatherEffects.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/script/PlayerGui.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/script/UpdateCadenceService.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <DataCaptureDriver.hpp>
#include <NamedCaptureView.hpp>
#include <algorithm>
#include <array>
#include <client/Scene.hpp>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.scene.tick")
TEST_SCENARIO_GROUP("client.scripted.tunnels")
TEST_DEPENDS("engine.testing.scenario")
TEST_DEPENDS("engine.ecs.scheduler")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.ecs.resources")
TEST_DEPENDS("engine.render.debugpanels")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("engine.scene.drawinstance")
TEST_DEPENDS("engine.scene.input")
TEST_DEPENDS("engine.gui.services")
TEST_DEPENDS("engine.gui.compile")
TEST_DEPENDS("engine.gui.input")
TEST_DEPENDS("engine.gui.typing")
TEST_DEPENDS("engine.game.roundtrip")
TEST_DEPENDS("engine.weather.runtime")
TEST_DEPENDS("engine.weather.consumers")
TEST_DEPENDS("engine.script.updatecadenceservice")

using Catch::Approx;
using engine::core::FrameGraph;
using engine::core::Metrics;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::scene::ActiveCamera;
using engine::scene::Camera;
using engine::scene::DrawInstance;
using engine::scene::PreviousTransform;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::scene::WorldBounds;

namespace {
	constexpr uint32_t ENTITIES = 512;
	constexpr float STEP = 1.0f / 60.0f;
	// The authored world has one server script, one StarterPlayerScript, and
	// its local-player copy in a combined host.
	constexpr size_t BLADEBORNE_STARTUP_SCRIPT_COUNT = 2;
	// The world loader puts each <Source> child in this cache.
	constexpr size_t BLADEBORNE_SOURCE_COUNT = 14;

	// A world and the scheduler that ticks it. Nothing else - which is the
	// point: after the scene loads, everything the tick reads and writes is
	// inside the store.
	//
	// **Built from the script now, not from `BuildDemoWorld`.** That function
	// died at v0.6: `Rings.luau` builds the same scene through the same class
	// table, and this suite exercising the C++ path while the client ran the
	// scripted one would have been testing something nothing shipped.
	struct Session {
		Store World{"integration"};
		Scheduler Systems;
		std::shared_ptr<engine::script::Runtime> Scripts;

		explicit Session(std::string_view scene = "Rings.luau", bool preparedPhysics = false) {
			engine::parallel::Jobs::Start(2);

			// **The staged assets root, not the test binary's own directory.**
			// `Paths::Assets` defaults to where the running executable sits, and
			// a test binary sits in `tests/` while the examples stage into
			// `assets/`. A client finds them because a client stages beside
			// them; this has to be told.
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
			if (preparedPhysics) {
				engine::physics::PreparePhysicsWorld(World, 8.0f);
				engine::physics::SetPhysicsTickRate(World, 120.0);
				engine::physics::SetPhysicsPaused(World, true);
			}

			const bool built = client::BuildScriptedWorld(
				World, Systems, engine::examples::ExamplePath(std::string(scene)), ENTITIES, &Scripts
			);
			REQUIRE(built);
			REQUIRE(Scripts != nullptr);
		}

		~Session() {
			engine::parallel::Jobs::Stop();
		}

		void Tick(int ticks, float step = STEP) {
			for (int tick = 0; tick < ticks; tick++) {
				Systems.Tick(World, step);
			}
		}

		const std::vector<DrawInstance> &Drawn() const {
			return World.Resource<engine::render::DrawList>()->Instances;
		}

		const std::vector<engine::core::CFrame> &Joints() const {
			return World.Resource<engine::render::DrawList>()->JointFrames;
		}
	};

	engine::ecs::Entity InWorkspace(Store &store, std::string_view name) {
		const engine::ecs::Entity workspace = engine::scene::WorkspaceOf(store);
		return workspace == engine::ecs::NULL_ENTITY ? engine::ecs::NULL_ENTITY
													 : store.FindFirstChild(workspace, name);
	}

	size_t CountNamedDescendants(Store &store, engine::ecs::Entity root, std::string_view name) {
		size_t found = 0;
		store.EachDescendant(root, [&](engine::ecs::Entity entity) {
			if (store.InstanceNameOf(entity) == engine::core::Name(name)) found++;
		});
		return found;
	}

	engine::ecs::Entity FirstNamedDescendant(Store &store, engine::ecs::Entity root, std::string_view name) {
		engine::ecs::Entity found = engine::ecs::NULL_ENTITY;
		store.EachDescendant(root, [&](engine::ecs::Entity entity) {
			if (found == engine::ecs::NULL_ENTITY &&
				store.InstanceNameOf(entity) == engine::core::Name(name)) {
				found = entity;
			}
		});
		return found;
	}

	engine::ecs::Entity LastNamedDescendant(Store &store, engine::ecs::Entity root, std::string_view name) {
		engine::ecs::Entity found = engine::ecs::NULL_ENTITY;
		store.EachDescendant(root, [&](engine::ecs::Entity entity) {
			if (store.InstanceNameOf(entity) == engine::core::Name(name)) found = entity;
		});
		return found;
	}

	engine::ecs::Entity FirstGuiElement(Store &store, std::string_view name) {
		engine::ecs::Entity found = engine::ecs::NULL_ENTITY;
		store.Each<const engine::gui::Element>([&](engine::ecs::Entity entity, const engine::gui::Element &) {
			if (found == engine::ecs::NULL_ENTITY &&
				store.InstanceNameOf(entity) == engine::core::Name(name)) {
				found = entity;
			}
		});
		return found;
	}

	const engine::gui::DrawCommand *
	CommandFor(const engine::gui::DrawList &list, engine::ecs::Entity source) {
		const auto found =
			std::find_if(list.Commands.begin(), list.Commands.end(), [source](const auto &command) {
				return command.Source == source;
			});
		return found == list.Commands.end() ? nullptr : &*found;
	}

	const engine::gui::DrawCommand *
	TextCommandFor(const engine::gui::DrawList &list, engine::ecs::Entity source) {
		const auto found =
			std::find_if(list.Commands.begin(), list.Commands.end(), [source](const auto &command) {
				return command.Source == source && command.Kind == engine::gui::DrawKind::Text;
			});
		return found == list.Commands.end() ? nullptr : &*found;
	}
}

TEST_CASE(
	"scripted client towns query authored colliders during the startup heartbeat",
	"[client][scripting][cadence][startup]"
) {
	for (const auto *path : {"TownCadence.luau", "TownCadence.js"}) {
		for (const bool prepared : {false, true}) {
			INFO(path);
			INFO(prepared);
			Session session(path, prepared);
			auto &store = session.World;
			const auto *physics = store.Resource<engine::physics::PhysicsWorld>();
			REQUIRE(physics != nullptr);
			REQUIRE(engine::physics::PhysicsClockOf(store) != nullptr);
			CHECK(store.Time().Tick == 0);
			if (prepared) {
				CHECK_FALSE(physics->CellSizeMeasured());
				CHECK(physics->CellSize() == 8.0f);
				CHECK(engine::physics::PhysicsTickRate(store) == 120.0);
				CHECK(engine::physics::IsPhysicsPaused(store));
			}
			const auto target = InWorkspace(store, "Practice Target");
			REQUIRE(target != engine::ecs::NULL_ENTITY);
			for (const auto *name : {"North Guard", "South Guard"}) {
				INFO(name);
				const auto guard = InWorkspace(store, name);
				REQUIRE(guard != engine::ecs::NULL_ENTITY);
				const auto root = store.FindFirstChild(guard, "Body");
				const auto humanoid = store.FindFirstChild(guard, "Humanoid");
				REQUIRE(root != engine::ecs::NULL_ENTITY);
				REQUIRE(humanoid != engine::ecs::NULL_ENTITY);
				const auto *observed = store.Get<engine::script::NpcPerception>(guard);
				const auto *decision = store.Get<engine::script::NpcBehaviour>(guard);
				const auto *state = store.Get<engine::scene::UpdateCadenceState>(guard);
				REQUIRE(observed != nullptr);
				REQUIRE(decision != nullptr);
				REQUIRE(state != nullptr);
				CHECK(observed->Visible);
				CHECK(observed->TargetPosition == store.Get<Transform>(target)->Frame.Position);
				for (size_t domain = 0; domain < 2; ++domain) {
					CHECK(state->Domains[domain].Generation > 0);
					CHECK(state->Domains[domain].LastCommittedTick == 0);
				}
				CHECK(decision->MoveDirection.Magnitude() > 0.9f);
				CHECK(store.Get<engine::scene::Humanoid>(humanoid)->MoveDirection == decision->MoveDirection);
				const auto position = store.Get<Transform>(root)->Frame.Position;
				const auto direction = (observed->TargetPosition - position).Unit();
				const auto hit = engine::physics::Raycast(
					store, engine::core::Ray{position + direction * 2, direction}, 100.0f
				);
				REQUIRE(hit);
				CHECK(hit->Owner == target);
			}
			if (!prepared) {
				const auto guard = InWorkspace(store, "North Guard");
				const auto root = store.FindFirstChild(guard, "Body");
				const auto initial = store.Get<Transform>(root)->Frame.Position;
				session.Tick(30);
				CHECK(store.Time().Tick == 30);
				CHECK(store.Get<Transform>(root)->Frame.Position.Z < initial.Z - 0.25f);
				CHECK(store.Get<engine::script::NpcPerception>(guard)->Visible);
			}
		}
	}
}

TEST_CASE("bare scene loading keeps physics optional", "[client][scripting][startup]") {
	Store store("bare-script-loader");
	Scheduler systems;
	engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
	CHECK(engine::physics::PhysicsClockOf(store) == nullptr);
	std::string failure;
	std::shared_ptr<engine::script::Runtime> runtime;
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("Rings.luau"), failure, &runtime
		)
	);
	INFO(failure);
	REQUIRE(runtime != nullptr);
	CHECK(engine::physics::PhysicsClockOf(store) == nullptr);
	CHECK(store.Time().Tick == 0);
}

TEST_CASE("GuiInteraction routes client controls into visible scripted state", "[client][gui][scripting]") {
	Session session("GuiInteraction.luau");
	Store &store = session.World;

	const engine::ecs::Entity textButton = FirstGuiElement(store, "TextButton");
	const engine::ecs::Entity imageButton = FirstGuiElement(store, "ImageButton");
	const engine::ecs::Entity textBox = FirstGuiElement(store, "TextBox");
	const engine::ecs::Entity status = FirstGuiElement(store, "InteractionStatus");
	const engine::ecs::Entity eventStatus = FirstGuiElement(store, "EventStatus");
	REQUIRE(textButton != engine::ecs::NULL_ENTITY);
	REQUIRE(imageButton != engine::ecs::NULL_ENTITY);
	REQUIRE(textBox != engine::ecs::NULL_ENTITY);
	REQUIRE(status != engine::ecs::NULL_ENTITY);
	REQUIRE(eventStatus != engine::ecs::NULL_ENTITY);

	engine::gui::CompileRequest request;
	request.Display = {
		.Width = 1280.0f,
		.Height = 720.0f,
		.SafeArea = {},
		.Occluded = {},
	};
	request.ScreenGuis = engine::gui::ScreenGuiSource::PlayerGui;
	const auto *local = store.Resource<engine::scene::LocalPlayer>();
	REQUIRE(local != nullptr);
	request.Viewer = local->Instance;

	engine::gui::Compiled compiled;
	engine::gui::Router router;
	const auto frame = [&](engine::core::Vector2 position, bool down) {
		request.Hovered = router.Hovered();
		request.Pressed = router.Pressed();
		REQUIRE(engine::gui::Layout(store, request.Display) > 0);
		compiled.Rebuild(store, request);

		engine::gui::Pointer pointer;
		pointer.Position = position;
		pointer.Down = down;
		pointer.Inside = true;
		pointer.ScreenOnly = true;
		session.Scripts->DeliverGuiEvents(router.Update(store, compiled.Commands(), pointer));
		session.Tick(1);
	};
	const auto centre = [&](engine::ecs::Entity element) {
		const auto *resolved = store.Get<engine::gui::Resolved>(element);
		REQUIRE(resolved != nullptr);
		return resolved->AbsolutePosition + resolved->AbsoluteSize * 0.5f;
	};

	// The compiled rectangles are the render and hit-test contract. The button
	// centres below come from them, so a stale or displaced visual cannot pass
	// by routing against a hand-written coordinate.
	frame({0.0f, 0.0f}, false);
	for (const engine::ecs::Entity element : {textButton, imageButton, textBox}) {
		const auto *resolved = store.Get<engine::gui::Resolved>(element);
		const auto *command = CommandFor(compiled.Commands(), element);
		REQUIRE(resolved != nullptr);
		REQUIRE(command != nullptr);
		CHECK(command->Bounds.Min == resolved->AbsolutePosition);
		CHECK(command->Bounds.Size() == resolved->AbsoluteSize);
	}

	frame(centre(textButton), false);
	CHECK(store.Get<engine::gui::Label>(eventStatus)->Text == "TextButton hover");
	frame(centre(textButton), true);
	CHECK(store.Get<engine::gui::Label>(eventStatus)->Text == "TextButton down");
	frame(centre(textButton), false);
	CHECK(store.Get<engine::gui::Label>(eventStatus)->Text == "TextButton activated");
	CHECK(store.Get<engine::gui::Label>(status)->Text == "PASS  TextButton 2   ImageButton 1");

	frame(centre(imageButton), false);
	frame(centre(imageButton), true);
	frame(centre(imageButton), false);
	CHECK(store.Get<engine::gui::Label>(eventStatus)->Text == "ImageButton activated");
	CHECK(store.Get<engine::gui::Label>(status)->Text == "PASS  TextButton 2   ImageButton 2");

	frame(centre(textBox), false);
	frame(centre(textBox), true);
	CHECK(engine::gui::FocusedTextBox(store) == textBox);
	CHECK(store.Get<engine::gui::Label>(eventStatus)->Text == "TextBox focused");

	engine::gui::Typing typing;
	typing.Text = " checked";
	const engine::gui::TypeResult typed = engine::gui::Type(store, typing);
	CHECK(typed.Instance == textBox);
	CHECK(typed.Changed);
	CHECK(store.Get<engine::gui::Label>(textBox)->Text == "TextBox component checked");

	request.Hovered = router.Hovered();
	request.Pressed = router.Pressed();
	REQUIRE(engine::gui::Layout(store, request.Display) > 0);
	REQUIRE(compiled.Rebuild(store, request));
	const auto *text = TextCommandFor(compiled.Commands(), textBox);
	REQUIRE(text != nullptr);
	CHECK(text->Text == "TextBox component checked");

	// `Type` owns the string write and has no artificial GUI event. Moving the
	// pointer outside the canvas proves the router, rather than a direct focus
	// mutation, releases the keyboard and delivers the script's focus-lost signal.
	frame(centre(textBox), false);
	frame({0.0f, 0.0f}, false);
	frame({0.0f, 0.0f}, true);
	CHECK(engine::gui::FocusedTextBox(store) == engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::gui::Label>(eventStatus)->Text == "TextBox focus lost");
}

TEST_CASE(
	"a GUI weather preset changes authored controls before the next accepted precipitation boundary",
	"[client][gui][particles][weather]"
) {
	Session session("Weather.luau");
	Store &store = session.World;
	session.Tick(1);

	const auto environment = engine::scene::EnvironmentOf(store);
	REQUIRE(environment.HasWeatherCompute);
	const auto provider = environment.CloudProvider;
	REQUIRE(provider != engine::ecs::NULL_ENTITY);
	CHECK(store.InstanceNameOf(provider) == engine::core::Name("Weather Clouds"));
	const auto clear = FirstGuiElement(store, "Weather Preset CLEAR");
	REQUIRE(clear != engine::ecs::NULL_ENTITY);
	REQUIRE(store.Get<engine::scene::WeatherCompute>(provider));
	CHECK(store.Get<engine::scene::WeatherCompute>(provider)->PrecipitationRate == Approx(38));
	engine::render::WeatherEffectsFrame precipitation;
	engine::effects::RibbonBuffer ribbons;
	std::vector<engine::render::SceneLight> lights;
	const auto collectPrecipitation = [&] {
		const auto *active = store.Resource<ActiveCamera>();
		REQUIRE(active);
		const auto *transform = store.Get<Transform>(active->Entity);
		REQUIRE(transform);
		engine::render::View view;
		view.World = store.Identity();
		view.WorldName = engine::core::Name(store.Name());
		view.CameraFrame = transform->Frame;
		view.Lighting = engine::scene::LightingOf(store);
		view.Instances = session.Drawn();
		engine::render::CollectWeatherEffects(store, view, precipitation, ribbons, lights);
	};
	collectPrecipitation();
	REQUIRE(precipitation.Precipitation);
	CHECK(precipitation.Precipitation->Source == provider);
	CHECK(engine::scene::ValidGpuParticleField(precipitation.Precipitation->Field));
	CHECK_FALSE(precipitation.Precipitation->Field.SpawnSamples.empty());
	const auto wetDefinition = precipitation.Definition;

	engine::gui::CompileRequest request;
	request.Display = {
		.Width = 1280.0f,
		.Height = 720.0f,
		.SafeArea = {},
		.Occluded = {},
	};
	request.ScreenGuis = engine::gui::ScreenGuiSource::PlayerGui;
	const auto *local = store.Resource<engine::scene::LocalPlayer>();
	REQUIRE(local != nullptr);
	request.Viewer = local->Instance;

	engine::gui::Compiled compiled;
	engine::gui::Router router;
	const auto frame = [&](bool down) {
		request.Hovered = router.Hovered();
		request.Pressed = router.Pressed();
		REQUIRE(engine::gui::Layout(store, request.Display) > 0);
		compiled.Rebuild(store, request);
		const auto *resolved = store.Get<engine::gui::Resolved>(clear);
		REQUIRE(resolved != nullptr);

		engine::gui::Pointer pointer;
		pointer.Position = resolved->AbsolutePosition + resolved->AbsoluteSize * 0.5f;
		pointer.Down = down;
		pointer.Inside = true;
		pointer.ScreenOnly = true;
		session.Scripts->DeliverGuiEvents(router.Update(store, compiled.Commands(), pointer));
		session.Tick(1);
	};

	frame(false);
	frame(true);
	const auto clickTick = store.Time().Tick;
	frame(false);
	CHECK(store.Time().Tick == clickTick + 1);
	CHECK(store.Get<engine::scene::WeatherCompute>(provider)->PrecipitationRate == 0);
	// GUI handlers run after the clock pass. The accepted definition drains its
	// existing tick before the next boundary admits the authored preset.
	REQUIRE(engine::scene::WeatherControlsOf(store, provider));
	CHECK(engine::scene::WeatherControlsOf(store, provider)->PrecipitationRate == Approx(38));
	collectPrecipitation();
	REQUIRE(precipitation.Precipitation);
	CHECK(precipitation.Definition == wetDefinition);
	session.Tick(1);
	CHECK(engine::scene::WeatherControlsOf(store, provider)->PrecipitationRate == 0);
	collectPrecipitation();
	REQUIRE(precipitation.Sample);
	CHECK(precipitation.Sample->Precipitation.LiquidMillimetresPerHour == 0);
	CHECK(precipitation.Sample->Precipitation.SnowMillimetresPerHour == 0);
	CHECK(precipitation.Sample->Precipitation.HailMillimetresPerHour == 0);
	CHECK_FALSE(precipitation.Precipitation);
	CHECK(precipitation.Definition != wetDefinition);
}

TEST_CASE("data capture driver ticket transition is paused, strict, and terminal", "[client][data-capture]") {
	using namespace client::data_capture_driver;
	State state;
	const engine::script::HostCallback callback{1};
	CHECK(Transition(state, false, callback, nullptr) == Action::None);
	CHECK(Transition(state, true, callback, nullptr) == Action::None);
	engine::script::HostValue queued(engine::script::HostTag::Map);
	engine::script::HostValue status(engine::script::HostTag::String);
	status.Text = "queued";
	engine::script::HostValue ticket(engine::script::HostTag::String);
	ticket.Text = "7";
	queued.Entries.emplace_back("status", std::move(status));
	queued.Entries.emplace_back("ticket", std::move(ticket));
	CHECK(Transition(state, true, callback, &queued) == Action::None);
	REQUIRE(state.Ticket == 7);
	CHECK(Transition(state, true, callback, &queued) == Action::None);
	CHECK(state.Cancelling);
	state.Cancelling = false;
	state.Ticket = 7;
	engine::script::HostValue pending(engine::script::HostTag::Map);
	engine::script::HostValue pendingStatus(engine::script::HostTag::String);
	pendingStatus.Text = "pending";
	pending.Entries.emplace_back("status", std::move(pendingStatus));
	CHECK(Transition(state, true, callback, &pending) == Action::None);
	state.Ticket.reset();
	CHECK(Transition(state, true, callback, &pending) == Action::Invalid);
	queued.Entries[1].second.Text = "18446744073709551616";
	CHECK(Transition(state, true, callback, &queued) == Action::Invalid);
	queued.Entries[1].second.Text = "7";
	state.Ticket = 7;
	for (const std::string_view terminal :
		 {"ready", "partial", "cancelled", "error", "failed", "unsupported", "invalid", "stale_snapshot"}) {
		engine::script::HostValue result(engine::script::HostTag::Map);
		engine::script::HostValue terminalStatus(engine::script::HostTag::String);
		terminalStatus.Text = terminal;
		result.Entries.emplace_back("status", std::move(terminalStatus));
		CHECK(Transition(state, true, callback, &result) == Action::Release);
		CHECK(state.Ticket == 7);
		state.Ticket = 7;
	}
	engine::script::HostValue malformed(engine::script::HostTag::String);
	CHECK(Transition(state, true, callback, &malformed) == Action::None);
	CHECK(state.Cancelling);
	state.Cancelling = false;
	CHECK(Transition(state, false, callback, nullptr) == Action::None);
	CHECK(state.Cancelling);
	state.Cancelling = false;
	state.Ticket = 7;
	engine::script::HostValue unknown(engine::script::HostTag::Map);
	engine::script::HostValue unknownStatus(engine::script::HostTag::String);
	unknownStatus.Text = "wat";
	unknown.Entries.emplace_back("status", std::move(unknownStatus));
	CHECK(Transition(state, true, callback, &unknown) == Action::None);
	CHECK(state.Cancelling);
	state.Cancelling = false;
	state.Ticket = 7;
	CHECK(Transition(state, true, engine::script::HostCallback{2}, nullptr) == Action::None);
	CHECK(state.Cancelling);
	CHECK(Transition(state, true, std::nullopt, nullptr) == Action::None);
	engine::script::HostValue cancelled(engine::script::HostTag::Map);
	engine::script::HostValue cancelledStatus(engine::script::HostTag::String);
	cancelledStatus.Text = "cancelled";
	cancelled.Entries.emplace_back("status", std::move(cancelledStatus));
	CHECK(Transition(state, true, std::nullopt, &cancelled) == Action::Release);
	CHECK(state.Ticket == 7);
	engine::script::HostValue richReady(engine::script::HostTag::Map);
	engine::script::HostValue richStatus(engine::script::HostTag::String);
	richStatus.Text = "ready";
	engine::script::HostValue planes(engine::script::HostTag::Array);
	richReady.Entries.emplace_back("status", std::move(richStatus));
	richReady.Entries.emplace_back("planes", std::move(planes));
	CHECK(Transition(state, true, callback, &richReady) == Action::Release);

	const int firstOwner = 1;
	const int secondOwner = 2;
	State owners;
	CHECK(Transition(owners, true, callback, nullptr, &firstOwner) == Action::None);
	CHECK(Transition(owners, true, callback, &queued, &firstOwner) == Action::None);
	CHECK(Transition(owners, true, callback, nullptr, &secondOwner) == Action::None);
	CHECK(owners.Cancelling);
	CHECK(owners.Ticket == 7);

	State cleanup{callback, 9, true};
	bool cancelSent = false;
	int cancels = 0;
	int pumps = 0;
	int releases = 0;
	std::string bridgeStatus = "pending";
	bool releaseAccepted = false;
	auto cancel = [&](uint64_t ticket) {
		CHECK(ticket == 9);
		cancels++;
	};
	auto pump = [&] { pumps++; };
	auto poll = [&](uint64_t ticket) {
		CHECK(ticket == 9);
		return bridgeStatus;
	};
	auto release = [&](uint64_t ticket) {
		CHECK(ticket == 9);
		releases++;
		return releaseAccepted;
	};
	CHECK_FALSE(AdvanceCancellation(cleanup, cancelSent, cancel, pump, poll, release));
	CHECK(cancels == 1);
	CHECK(pumps == 1);
	CHECK(releases == 0);
	CHECK_FALSE(AdvanceCancellation(cleanup, cancelSent, cancel, pump, poll, release));
	CHECK(cancels == 1);
	CHECK(pumps == 2);
	bridgeStatus = "cancelled";
	CHECK_FALSE(AdvanceCancellation(cleanup, cancelSent, cancel, pump, poll, release));
	CHECK(releases == 1);
	CHECK(cleanup.Ticket == 9);
	releaseAccepted = true;
	CHECK(AdvanceCancellation(cleanup, cancelSent, cancel, pump, poll, release));
	CHECK(releases == 2);
	CHECK_FALSE(cleanup.Ticket.has_value());
	CHECK_FALSE(cleanup.Cancelling);

	State invokeFailure{callback, 9};
	engine::script::HostValue noResult;
	CHECK(Transition(invokeFailure, true, callback, &noResult) == Action::None);
	CHECK(invokeFailure.Cancelling);
	State noTicketFailure;
	noTicketFailure.Callback = callback;
	CHECK(Transition(noTicketFailure, true, callback, &noResult) == Action::Invalid);
	CHECK_FALSE(noTicketFailure.Cancelling);
	CHECK(Transition(noTicketFailure, true, callback, &queued) == Action::None);
	CHECK(noTicketFailure.Ticket == 7);
}

TEST_CASE(
	"named capture views rebuild the camera cache and leave the next view normal", "[client][data-capture]"
) {
	Session session;
	Store &store = session.World;
	const auto workspace = engine::scene::WorkspaceOf(store);
	engine::scene::PartDesc portalPart;
	portalPart.Frame.Position = {0, 0, -8};
	portalPart.Size = {2, 2, .1f};
	const auto portalPane = engine::scene::MakePart(store, portalPart);
	portalPart.Frame.Position = {8, 0, -8};
	const auto portalDestination = engine::scene::MakePart(store, portalPart);
	portalPart.Frame.Position = {-8, 0, -8};
	const auto mirrorPane = engine::scene::MakePart(store, portalPart);
	REQUIRE(store.SetParent(portalPane, workspace));
	REQUIRE(store.SetParent(portalDestination, workspace));
	REQUIRE(store.SetParent(mirrorPane, workspace));
	const auto portalCamera =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), "PortalCamera");
	REQUIRE(store.SetParent(portalCamera, portalPane));
	auto portalSurface = *store.Get<engine::scene::SurfaceCamera>(portalCamera);
	portalSurface.Surface = -1;
	store.Set(portalCamera, portalSurface);
	engine::scene::Portal portal;
	portal.Destination = portalDestination;
	store.Set(portalCamera, portal);
	const auto mirrorCamera =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), "MirrorCamera");
	REQUIRE(store.SetParent(mirrorCamera, mirrorPane));
	auto mirrorSurface = *store.Get<engine::scene::SurfaceCamera>(mirrorCamera);
	mirrorSurface.Surface = -1;
	store.Set(mirrorCamera, mirrorSurface);
	const auto billboard = store.CreateInstance(engine::gui::GuiClass("BillboardGui"), "CaptureLabel");
	REQUIRE(store.SetParent(billboard, mirrorPane));
	engine::gui::Billboard label;
	label.Size = {2, 0, 2, 0};
	store.Set(billboard, label);
	const auto panel = store.CreateInstance(engine::gui::GuiClass("Frame"), "Panel");
	REQUIRE(store.SetParent(panel, billboard));
	engine::gui::Element element;
	element.Size = {1, 0, 1, 0};
	store.Set(panel, element);
	store.Set(panel, engine::gui::Background{});
	const auto camera = store.Create();
	store.Set(camera, Transform{engine::core::CFrame(engine::core::Vector3{8, 0, 0})});
	Camera lens;
	lens.FieldOfViewRadians = 0.8f;
	store.Set(camera, lens);

	engine::render::View normal;
	normal.World = 1;
	normal.WorldName = engine::core::Name("named-capture");
	normal.Pipeline = engine::core::Name("capture-pipeline");
	normal.CameraFrame = engine::core::CFrame(engine::core::Vector3{-4, 0, 0});
	normal.Camera.FieldOfViewRadians = 0.4f;
	normal.Grid.Enabled = true;
	const auto normalSignature = engine::render::ScenePresentationSignature(normal, {});

	engine::render::View capture = normal;
	capture.CameraFrame = store.Get<Transform>(camera)->Frame;
	capture.Camera = *store.Get<Camera>(camera);
	engine::render::WorldViewFrame frame;
	engine::render::WorldCameraFrame cameraFrame;
	engine::render::InterfacePass namedInterface;
	REQUIRE(
		client::BindNamedCapturePresentation(
			store,
			{.World = capture.World,
			 .Name = capture.WorldName,
			 .Identity = store.Identity(),
			 .ContentOwner = capture.WorldName,
			 .ForeignContentOwners = {},
			 .Pipeline = capture.Pipeline},
			{64, 64},
			capture,
			frame,
			cameraFrame,
			namedInterface,
			[] {}
		)
	);
	CHECK(capture.CameraFrame.Position.X == Approx(8));
	CHECK(capture.Camera.FieldOfViewRadians == Approx(0.8f));
	REQUIRE(capture.Portals.size() == 1);
	CHECK(capture.Portals.data() == frame.Portals.data());
	CHECK(capture.Portals[0].Index == 0);
	CHECK(capture.Portals[0].Centre.X == Approx(0));
	REQUIRE(capture.Surfaces.size() == 1);
	CHECK(capture.Surfaces.data() == cameraFrame.Surfaces.data());
	CHECK(capture.Surfaces[0].Index == 1);
	CHECK(capture.Surfaces[0].PaneCentre.X == Approx(-8));
	REQUIRE_FALSE(cameraFrame.SpatialCommands.Commands.empty());
	CHECK(cameraFrame.SpatialCommands.Commands[0].Collector == billboard);
	CHECK(namedInterface.AffectsScene());
	CHECK(engine::render::ScenePresentationSignature(capture, {}) != normalSignature);
	CHECK(engine::render::ScenePresentationSignature(normal, {}) == normalSignature);
	int aborted = 0;
	engine::render::View refused = capture;
	CHECK_FALSE(
		client::BindNamedCaptureWorldView(
			store,
			{.World = capture.World,
			 .Name = capture.WorldName,
			 .Identity = store.Identity() + 1,
			 .ContentOwner = capture.WorldName,
			 .ForeignContentOwners = {},
			 .Pipeline = capture.Pipeline},
			{64, 64},
			refused,
			frame,
			cameraFrame,
			[&aborted] { aborted++; }
		)
	);
	CHECK(aborted == 1);
	CHECK(refused.CameraFrame.Position.X == Approx(8));

	engine::world::Universe worlds;
	aborted = 0;
	CHECK_FALSE(
		client::BindNamedCaptureWorldView(
			worlds,
			{},
			{.World = capture.World,
			 .Name = capture.WorldName,
			 .Identity = 0,
			 .ContentOwner = capture.WorldName,
			 .ForeignContentOwners = {},
			 .Pipeline = capture.Pipeline},
			{64, 64},
			refused,
			frame,
			cameraFrame,
			[&aborted] { aborted++; }
		)
	);
	CHECK(aborted == 1);
}

namespace {
	// Copies only the facts these checks need. No world or borrowed draw rows
	// survive publication; temporal checks keep just their own previous value.
	struct RingsSample {
		size_t DrawCount = 0;
		size_t VisualCount = 0;
		bool BoundsPresent = false;
		bool CameraPresent = false;
		bool DrawListPresent = false;
		uint64_t Tick = 0;
		engine::core::Vector3 FirstDrawn;
		engine::core::Vector3 FourthDrawn;
		engine::core::Vector3 FourthSimulated;
		engine::core::Vector3 FourthPrevious;
		bool PreviousPresent = false;
		bool CameraRow = false;
		engine::core::Vector3 CameraPosition;
		float FarPlane = 0;
		float Extent = 0;
	};

	RingsSample ObserveRings(Session &session) {
		RingsSample sample;
		auto &world = session.World;
		sample.Tick = world.Time().Tick;
		sample.VisualCount = world.CountMatching<Transform, Visual>();
		sample.BoundsPresent = world.HasResource<WorldBounds>();
		sample.CameraPresent = world.HasResource<ActiveCamera>();
		sample.DrawListPresent = world.HasResource<engine::render::DrawList>();
		if (sample.BoundsPresent) sample.Extent = world.Resource<WorldBounds>()->HalfExtent;
		if (sample.DrawListPresent) {
			const auto &drawn = session.Drawn();
			sample.DrawCount = drawn.size();
			if (!drawn.empty()) sample.FirstDrawn = drawn[0].Frame.Position;
			if (drawn.size() > 3) {
				sample.FourthDrawn = drawn[3].Frame.Position;
				const auto entity = engine::ecs::Entity(drawn[3].Source);
				if (const auto *previous = world.Get<PreviousTransform>(entity)) {
					sample.PreviousPresent = true;
					sample.FourthPrevious = previous->Frame.Position;
				}
			}
		}
		int seen = 0;
		world.Each<const Transform, const Visual>(
			[&](engine::ecs::Entity, const Transform &transform, const Visual &) {
				if (seen++ == 3) sample.FourthSimulated = transform.Frame.Position;
			}
		);
		if (sample.CameraPresent) {
			const auto camera = world.Resource<ActiveCamera>()->Entity;
			const auto *lens = world.Get<Camera>(camera);
			const auto *transform = world.Get<Transform>(camera);
			sample.CameraRow = lens != nullptr && transform != nullptr;
			if (sample.CameraRow) {
				sample.CameraPosition = transform->Frame.Position;
				sample.FarPlane = lens->FarPlane;
			}
		}
		return sample;
	}

	engine::testing::ScenarioReport RingsScenario() {
		using namespace engine::testing;
		using client::tests::Near;
		std::vector<ScenarioCheck<RingsSample>> checks;
		const auto add = [&](std::string name, std::vector<HookRequirement> hooks, auto observe) {
			checks.push_back({std::move(name), "client.scene.tick", {std::move(hooks)}, observe, {}});
		};
		add("instances", {{"tick1"}}, [](const auto &observation, CheckResult &result) {
			const auto &sample = observation.Payload;
			result.Record("draw instances", sample.DrawCount == ENTITIES);
			result.Record("drawable rows exclude camera", sample.VisualCount == ENTITIES);
		});
		add("resources", {{"initial"}}, [](const auto &observation, CheckResult &result) {
			const auto &sample = observation.Payload;
			result.Record("world bounds resource", sample.BoundsPresent);
			result.Record("active camera resource", sample.CameraPresent);
			result.Record("draw list resource", sample.DrawListPresent);
			result.Record("initial clock", sample.Tick == 0);
		});
		add("bounded", {{"tick60"}}, [](const auto &observation, CheckResult &result) {
			result.Record("draw list rebuilt", observation.Payload.DrawCount == ENTITIES);
		});
		add("movement",
			{{"tick1"}, {"tick31"}},
			[before = engine::core::Vector3{}](const auto &observation, CheckResult &result) mutable {
				const auto &sample = observation.Payload;
				result.Record("movement draw row available", sample.DrawCount > 0);
				if (observation.Hook == "tick1")
					before = sample.FirstDrawn;
				else
					result.Record(
						"entities move", before.X != sample.FirstDrawn.X || before.Z != sample.FirstDrawn.Z
					);
			});
		add("camera",
			{{"initial"}, {"tick120"}},
			[before = engine::core::Vector3{}](const auto &observation, CheckResult &result) mutable {
				const auto &sample = observation.Payload;
				result.Record("camera component and transform", sample.CameraRow);
				if (observation.Hook == "initial")
					before = sample.CameraPosition;
				else {
					result.Record(
						"camera moves",
						!Near(before.X, sample.CameraPosition.X) || !Near(before.Z, sample.CameraPosition.Z)
					);
					result.Record("camera bounds available", sample.BoundsPresent);
					result.Record("camera fits bounds", sample.FarPlane > sample.Extent);
				}
			});
		add("interpolation",
			{{"tick4.alpha0"}, {"tick4.alpha1"}},
			[previous = engine::core::Vector3{}](const auto &observation, CheckResult &result) mutable {
				const auto &sample = observation.Payload;
				result.Record("interpolation draw row available", sample.DrawCount > 3);
				if (observation.Hook == "tick4.alpha0")
					previous = sample.FourthDrawn;
				else {
					result.Record("alpha moves draw position", !Near(sample.FourthDrawn.X, previous.X));
					result.Record(
						"alpha one simulation X", Near(sample.FourthDrawn.X, sample.FourthSimulated.X, 1e-4)
					);
					result.Record(
						"alpha one simulation Z", Near(sample.FourthDrawn.Z, sample.FourthSimulated.Z, 1e-4)
					);
				}
			});
		add("previous", {{"tick4.alpha0"}}, [](const auto &observation, CheckResult &result) {
			const auto &sample = observation.Payload;
			result.Record("previous transform exists", sample.PreviousPresent);
			result.Record("alpha zero previous X", Near(sample.FourthDrawn.X, sample.FourthPrevious.X, 1e-4));
			result.Record("alpha zero previous Z", Near(sample.FourthDrawn.Z, sample.FourthPrevious.Z, 1e-4));
		});
		ScenarioKey key;
		key.Scene = "Rings.luau";
		// Staged assets are immutable during this test process. Completed reports
		// never survive the process or substitute for the runner's source invalidation.
		key.ContentRevision = "staged-examples";
		key.Configuration = "entities=512;physics=default;jobs=2";
		key.TickSchedule = "120@float(1/60);observe=0,1,4,31,60,120";
		key.FrameSchedule = "default-alpha0;tick4:alpha1,restore-alpha0";
		key.Actions = "tick4:PreRender(alpha1)";
		key.CaptureConfiguration = "owned scene facts;no GPU";
		key.ObservationSchema = "client.rings.facts.v1";
		key.CheckSet = "instances,resources,bounded,movement,camera,interpolation,previous.v1";
		ScenarioDriver<RingsSample> driver;
		driver.Run = [](ScenarioSession<RingsSample> &scenario) {
			Session session;
			const auto publish = [&](std::string hook) {
				scenario.Publish({std::move(hook), session.World.Time().Tick, ObserveRings(session)});
			};
			publish("initial");
			for (int tick = 1; tick <= 120; tick++) {
				scenario.Checkpoint();
				session.Tick(1);
				if (tick == 1 || tick == 31 || tick == 60 || tick == 120)
					publish("tick" + std::to_string(tick));
				if (tick == 4) {
					publish("tick4.alpha0");
					session.World.SetFrame(0.0f, 1.0f);
					session.Systems.RunPhases(session.World, Phase::PreRender, Phase::PreRender);
					publish("tick4.alpha1");
					session.World.SetFrame(0.0f, 0.0f);
				}
			}
		};
		static ScenarioCache<RingsSample> cache(1);
		return cache.Run(std::move(key), std::move(checks), driver);
	}
}

TEST_CASE("a built scene produces one instance per entity", "[demo]") {
	client::tests::CheckScenario(RingsScenario(), "instances");
}

TEST_CASE("the world holds the scene's state, not a scene object", "[demo]") {
	client::tests::CheckScenario(RingsScenario(), "resources");
}

TEST_CASE("two worlds tick independently", "[demo]") {
	// The property the private-member version could not have had. Two scenes
	// sharing one clock or one draw list is a bug that only appears the day
	// something hosts two worlds, which is exactly when it is hardest to find.
	Session first;
	Session second;

	first.Tick(10);
	second.Tick(3);

	REQUIRE(first.World.Time().Tick == 10);
	REQUIRE(second.World.Time().Tick == 3);
	REQUIRE(first.World.Time().Elapsed > second.World.Time().Elapsed);

	REQUIRE(first.Drawn().size() == ENTITIES);
	REQUIRE(second.Drawn().size() == ENTITIES);
	REQUIRE(first.Drawn()[0].Frame.Position.X != Approx(second.Drawn()[0].Frame.Position.X));
}

TEST_CASE("the instance buffer is rebuilt, not appended to", "[demo]") {
	client::tests::CheckScenario(RingsScenario(), "bounded");
}

TEST_CASE("entities actually move", "[demo]") {
	client::tests::CheckScenario(RingsScenario(), "movement");
}

TEST_CASE("the animation demo advances a real track into a bone pose", "[demo][animation]") {
	Session session("Animation.luau");
	const engine::ecs::Entity rig = InWorkspace(session.World, "AnimatedRig");
	REQUIRE(rig != engine::ecs::NULL_ENTITY);
	REQUIRE(session.World.Get<engine::scene::Skeleton>(rig) != nullptr);

	const engine::ecs::Entity root = session.World.FindFirstChild(rig, "Root");
	REQUIRE(root != engine::ecs::NULL_ENTITY);
	session.Tick(1);
	REQUIRE(session.Joints().size() == 3);
	const engine::core::CFrame before = session.World.Get<engine::scene::Bone>(root)->Transform;
	const engine::core::CFrame paletteBefore = session.Joints()[0];

	session.Tick(30);

	const engine::scene::Bone *posed = session.World.Get<engine::scene::Bone>(root);
	REQUIRE(posed != nullptr);
	CHECK(posed->Transform.QuaternionY != Approx(before.QuaternionY));
	CHECK(session.Joints()[0].QuaternionY != Approx(paletteBefore.QuaternionY));
	const engine::scene::AnimationClip *animation = nullptr;
	session.World.Each<const engine::scene::AnimationClip>(
		[&](engine::ecs::Entity, const engine::scene::AnimationClip &candidate) { animation = &candidate; }
	);
	REQUIRE(animation != nullptr);
	CHECK(animation->Buffer != engine::ecs::NULL_ENTITY);
}

TEST_CASE("a scripted NPC is integrated and crosses a portal in a standalone client", "[demo][portal]") {
	client::tests::CheckScenario(client::tests::TunnelsScenario(), "npc");
}

TEST_CASE("the tunnels remote portal destination fits the replicated world", "[demo][portal]") {
	client::tests::CheckScenario(client::tests::TunnelsScenario(), "wire");
}

TEST_CASE("the hallway camera and NPC cross portals in a standalone client", "[demo][portal]") {
	Session session("Hallway.luau");
	const engine::ecs::Entity character = InWorkspace(session.World, "HallwayWalker");
	REQUIRE(character != engine::ecs::NULL_ENTITY);
	const engine::ecs::Entity root = session.World.FindFirstChild(character, "HumanoidRootPart");
	const engine::ecs::Entity humanoid = session.World.FindFirstChild(character, "Humanoid");
	REQUIRE(root != engine::ecs::NULL_ENTITY);
	REQUIRE(humanoid != engine::ecs::NULL_ENTITY);

	const engine::ecs::Entity camera = session.World.Resource<ActiveCamera>()->Entity;
	session.Tick(100);
	CHECK(session.World.Get<Transform>(camera)->Frame.Position.X == Approx(20.0f).margin(0.1f));

	session.Tick(140);
	const auto *characterTransit = session.World.Get<engine::scene::PortalTransit>(root);
	REQUIRE(characterTransit != nullptr);
	CHECK(characterTransit->Serial >= 1u);

	// The demonstration is a shuttle rather than a one-way walk. Wait for the
	// authored controller to reverse at the south end, then prove the same body
	// is walking north and has crossed another mouth on its return leg.
	bool returning = false;
	for (int tick = 0; tick < 240 && !returning; tick++) {
		session.Tick(1);
		const auto *intent = session.World.Get<engine::scene::Humanoid>(humanoid);
		returning = intent != nullptr && intent->MoveDirection.Z > 0.5f;
	}
	CHECK(returning);
	const auto *returnTransit = session.World.Get<engine::scene::PortalTransit>(root);
	REQUIRE(returnTransit != nullptr);
	CHECK(returnTransit->Serial >= 2u);
}

TEST_CASE("the camera is a row the systems move, not a resource holding a value", "[demo]") {
	client::tests::CheckScenario(RingsScenario(), "camera");
}

namespace {
	// Simulation state, not render output. The two differ once interpolation
	// exists, and most of what these tests are about is the former.
	//
	// Cubes only. The camera has a Transform as well, and a query that swept it
	// up would compare the nth *row* rather than the nth cube.
	engine::core::Vector3 PositionOf(Store &store, int nth) {
		engine::core::Vector3 found;
		int seen = 0;
		store.Each<const Transform, const Visual>(
			[&](engine::ecs::Entity, const Transform &transform, const Visual &) {
				if (seen++ == nth) {
					found = transform.Frame.Position;
				}
			}
		);
		return found;
	}
}

TEST_CASE("the scene is a function of elapsed time, not of tick count", "[demo]") {
	// Two sessions stepped over the same total duration in different-sized
	// steps must agree. Without that, a frame-time comparison between two runs
	// compares two different scenes.
	Session coarse;
	Session fine;

	coarse.Tick(60, 1.0f / 60.0f);
	fine.Tick(120, 1.0f / 120.0f);

	REQUIRE(coarse.World.Time().Elapsed == Approx(fine.World.Time().Elapsed).margin(1e-6));

	// Compared on the Transform rather than on the emitted instance, because
	// the instance is *interpolated* and at alpha 0 it shows the previous
	// tick - 59/60 of a second against 119/120, which are legitimately
	// different places. What has to match is the simulation.
	const auto a = PositionOf(coarse.World, 7);
	const auto b = PositionOf(fine.World, 7);

	// Orbit position is derived from the world's clock, so it matches. Spin
	// integrates per tick and deliberately does not, which is why only the
	// translation is compared here.
	REQUIRE(a.X == Approx(b.X).margin(1e-3));
	REQUIRE(a.Y == Approx(b.Y).margin(1e-3));
	REQUIRE(a.Z == Approx(b.Z).margin(1e-3));
}

TEST_CASE("rendering interpolates between the last two ticks", "[demo]") {
	client::tests::CheckScenario(RingsScenario(), "interpolation");
}

TEST_CASE("alpha zero draws the previous tick exactly", "[demo]") {
	client::tests::CheckScenario(RingsScenario(), "previous");
}

TEST_CASE("a simulation system cannot reach the frame delta", "[demo]") {
	Session session;

	// A frame far shorter than a tick, which is the ordinary case at a high
	// frame rate. If anything in the simulation read it, the scene would land
	// somewhere different from the same world stepped without it.
	Session control;

	for (int tick = 0; tick < 30; tick++) {
		session.World.SetFrame(1.0f / 500.0f, 0.0f);
		session.Systems.Tick(session.World, STEP);
	}
	control.Tick(30);

	const auto a = PositionOf(session.World, 11);
	const auto b = PositionOf(control.World, 11);
	REQUIRE(a.X == Approx(b.X));
	REQUIRE(a.Z == Approx(b.Z));
}

TEST_CASE("two sessions built the same way stay identical", "[demo]") {
	Session first;
	Session second;

	first.Tick(45);
	second.Tick(45);

	REQUIRE(first.Drawn().size() == second.Drawn().size());
	for (size_t index = 0; index < first.Drawn().size(); index++) {
		const auto &a = first.Drawn()[index];
		const auto &b = second.Drawn()[index];
		REQUIRE(a.Tint.R == Approx(b.Tint.R));
		REQUIRE(a.Frame.Position.X == Approx(b.Frame.Position.X));
		REQUIRE(a.Frame.Position.Z == Approx(b.Frame.Position.Z));
	}
}

TEST_CASE("a tick with a job pool matches one without", "[demo]") {
	Session pooled;
	Session serial;

	engine::parallel::Jobs::Start(4);
	pooled.Tick(30);
	engine::parallel::Jobs::Stop();

	serial.Tick(30);

	for (size_t index = 0; index < serial.Drawn().size(); index += 37) {
		REQUIRE(pooled.Drawn()[index].Frame.Position.X == Approx(serial.Drawn()[index].Frame.Position.X));
	}
}

TEST_CASE("a tick reports itself to the frame graph and the metrics sink", "[demo]") {
	Metrics::Clear();

	Session session;

	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	session.Tick(1);
	FrameGraph::EndFrame();

	const auto spans = FrameGraph::Spans();
	FrameGraph::SetEnabled(false);

	// Every registered system is a span, with no instrumentation inside the
	// systems themselves. That is what makes the F5 overlay useful on code
	// nobody remembered to annotate.
	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	REQUIRE(named("ecs.systems"));

	// **`script-heartbeat` where `orbit` and `spin` used to be.** The scene
	// moves itself now: `Rings.luau` connects to `RunService.Heartbeat` and
	// writes `CFrame` directly, so there is one system driving the motion
	// instead of two C++ ones reading components the script never fills in.
	REQUIRE(named("capture-previous"));
	REQUIRE(named("script-heartbeat"));
	REQUIRE(named("move-camera"));
	REQUIRE(named("collect-instances"));

	// A span per phase between the scheduler and its systems, so "which part of
	// the tick" is answerable before "which system" - and every one of them is
	// ECS time, because every engine and game system runs through the ECS.
	REQUIRE(named("simulation"));
	REQUIRE(named("physics"));
	REQUIRE(named("animation"));
	REQUIRE(named("replication"));
	REQUIRE(named("render preparation"));
	REQUIRE(named("render"));

	const auto categorised = [&spans](std::string_view name) {
		const auto found =
			std::find_if(spans.begin(), spans.end(), [name](const auto &span) { return span.Name == name; });
		return found != spans.end() && found->Category == engine::core::ProfileCategory::ECS;
	};
	REQUIRE(categorised("script-heartbeat"));
	REQUIRE(categorised("collect-instances"));

	const auto counters = Metrics::Drain();
	const auto instances = std::find_if(counters.begin(), counters.end(), [](const auto &counter) {
		return counter.Name == engine::core::Name("render.instances");
	});
	REQUIRE(instances != counters.end());
	REQUIRE(instances->Value == Approx(static_cast<double>(ENTITIES)));
}

TEST_CASE("the magic camera orbit does not rebuild static terrain draw rows", "[client][magic][profile]") {
	Session session("Magic.luau");

	// The first frame establishes the rendered tag and cached source rows. The
	// scene's first cast is delayed, so its next beat changes only the Camera's
	// transform as it orbits the 5k-plus terrain parts.
	session.Tick(1);
	REQUIRE(session.Drawn().size() > 500);
	const std::vector<DrawInstance> previousDrawn(session.Drawn().begin(), session.Drawn().end());
	const ActiveCamera *activeCamera = session.World.Resource<ActiveCamera>();
	REQUIRE(activeCamera != nullptr);
	const engine::ecs::Entity cameraEntity = activeCamera->Entity;
	const Transform *cameraTransform = session.World.Get<Transform>(cameraEntity);
	REQUIRE(cameraTransform != nullptr);
	const engine::core::CFrame previousCameraFrame = cameraTransform->Frame;
	const uint64_t previousTransformEpoch = session.World.ComponentChangeVersion<Transform>();
	// Match PrepareTick: consumed setup writes do not belong to the next batch.
	session.World.ClearChanges();

	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	session.Tick(1);
	FrameGraph::EndFrame();

	const auto spans = FrameGraph::Spans();
	FrameGraph::SetEnabled(false);
	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	const ActiveCamera *currentActiveCamera = session.World.Resource<ActiveCamera>();
	REQUIRE(currentActiveCamera != nullptr);
	CHECK(currentActiveCamera->Entity == cameraEntity);
	const Transform *currentCameraTransform = session.World.Get<Transform>(cameraEntity);
	REQUIRE(currentCameraTransform != nullptr);
	CHECK(
		(currentCameraTransform->Frame.Position != previousCameraFrame.Position ||
		 currentCameraTransform->Frame.Rotation() != previousCameraFrame.Rotation())
	);
	CHECK(session.World.ComponentChangeVersion<Transform>() > previousTransformEpoch);
	const std::vector<DrawInstance> &currentDrawn = session.Drawn();
	REQUIRE(currentDrawn.size() == previousDrawn.size());
	for (size_t index = 0; index < previousDrawn.size(); ++index) {
		CHECK(currentDrawn[index].Source == previousDrawn[index].Source);
		CHECK(currentDrawn[index].Frame.Position == previousDrawn[index].Frame.Position);
		CHECK(currentDrawn[index].Frame.Rotation() == previousDrawn[index].Frame.Rotation());
	}
	session.World.EachChanged<Transform>([&](engine::ecs::Entity entity, const Transform &) {
		CHECK_FALSE(std::ranges::any_of(previousDrawn, [entity](const DrawInstance &instance) {
			return instance.Source == entity.Id;
		}));
	});

	// A global Transform epoch is not enough to invalidate visible rows. The
	// old path refreshed every terrain instance here because the eye moved.
	CHECK(named("reuse draw list"));
	CHECK_FALSE(named("update draw frames"));
	CHECK_FALSE(named("sync rendered.walk"));
}

TEST_CASE("the panels render a real tick's data", "[demo]") {
	Session session;

	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	session.Tick(1);
	FrameGraph::EndFrame();

	engine::render::FrameStatistics statistics;
	statistics.Record(0.0, STEP);

	std::vector<engine::render::SystemTiming> timings;
	for (const auto &timing : session.Systems.Timings()) {
		timings.push_back({timing.Name, timing.Milliseconds});
	}
	// Timings cover the complete authority tick and its presentation passes.
	// Weather clock, terrain and stateful preparation precede simulation;
	// ground queries and event consumers run after physics, before collection.
	REQUIRE(timings.size() == 36);
	const auto hasTiming = [&timings](const std::string_view name) {
		return std::ranges::any_of(timings, [name](const auto &timing) { return timing.Name == name; });
	};
	CHECK(hasTiming("advance-animation-tracks"));
	CHECK(hasTiming("evaluate-animations"));
	CHECK(hasTiming("resolve-bones"));
	for (const auto name :
		 {"weather.clock", "weather.terrain", "weather.stateful", "weather.ground", "weather.runtime"}) {
		CAPTURE(name);
		CHECK(hasTiming(name));
	}

	engine::render::OverlayImage image;
	image.Resize(1280, 720);

	engine::render::DebugPanelData panels;
	panels.ShowStatistics = true;
	panels.ShowFrameGraph = true;
	panels.Statistics = &statistics;
	panels.Spans = FrameGraph::Spans();
	panels.FrameMilliseconds = FrameGraph::FrameMilliseconds();
	panels.Systems = timings;
	panels.Entities = ENTITIES;

	engine::render::DrawDebugPanels(image, panels);
	FrameGraph::SetEnabled(false);

	REQUIRE(image.IsDirty());
}

// --- what a scene script may write before the systems are installed ---------

TEST_CASE("a top-level pointer-mode write survives the install", "[demo]") {
	// **`InputState` used to be minted after the script had already run.**
	// `UserInputService.MouseBehavior` writes onto that resource and the setter
	// drops a write onto a world that has none - correctly, because a server
	// must not mint a window's state - so a `--script` scene that locked the
	// pointer at the top level was silently ignored and only a write from inside
	// a `Heartbeat` took. Reproduced with a real scene before it was fixed: the
	// top-level write read back `Default` and the tick-three write read back
	// `LockCenter`.
	engine::parallel::Jobs::Start(2);
	engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");

	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "client_scene_tick_pointer_mode.luau";
	{
		std::ofstream out(scene);
		out << "local UIS = game:GetService('UserInputService')\n";
		out << "UIS.MouseBehavior = Enum.MouseBehavior.LockCenter\n";
		out << "UIS.MouseIconEnabled = false\n";
		out << "local block = Instance.new('Part')\n";
		out << "block.Parent = workspace\n";
	}

	Store world("pointer_mode");
	Scheduler systems;
	REQUIRE(client::BuildScriptedWorld(world, systems, scene.string(), 1));

	const auto *input = world.Resource<engine::scene::InputState>();
	REQUIRE(input != nullptr);
	CHECK(input->Behaviour == engine::scene::MouseBehavior::LockCenter);
	CHECK_FALSE(input->MouseIconEnabled);

	std::filesystem::remove(scene);
	engine::parallel::Jobs::Stop();
}

TEST_CASE("a scripted client sees a local player and draws its completed PlayerGui", "[demo][gui]") {
	engine::parallel::Jobs::Start(2);
	engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");

	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "client_scene_tick_player_gui.luau";
	{
		std::ofstream out(scene);
		out << "local Players = game:GetService('Players')\n";
		out << "assert(Players.LocalPlayer ~= nil, 'LocalPlayer was absent during startup')\n";
		out << "local screen = Instance.new('ScreenGui')\n";
		out << "screen.Name = 'ClientScreen'\n";
		out << "screen.Parent = game:GetService('StarterGui')\n";
	}

	Store world("player_gui");
	Scheduler systems;
	REQUIRE(client::BuildScriptedWorld(world, systems, scene.string(), 1));

	const auto *local = world.Resource<engine::scene::LocalPlayer>();
	REQUIRE(local != nullptr);
	REQUIRE(local->Instance != engine::ecs::NULL_ENTITY);
	const engine::ecs::Entity playerGui = world.FindFirstChild(local->Instance, engine::gui::PLAYER_GUI);
	REQUIRE(playerGui != engine::ecs::NULL_ENTITY);
	CHECK(world.FindFirstChild(playerGui, "ClientScreen") != engine::ecs::NULL_ENTITY);

	std::filesystem::remove(scene);
	engine::parallel::Jobs::Stop();
}

TEST_CASE("the shipped Bladeborne world runs both single-player roles", "[client][world][gui]") {
	engine::parallel::Jobs::Start(2);
	struct StopJobs {
		~StopJobs() {
			engine::parallel::Jobs::Stop();
		}
	} stopJobs;

	const auto demo =
		engine::examples::DemosLoader().Find(engine::examples::DemoKind::World, "BladeborneDemo.aworld");
	REQUIRE(demo.has_value());

	engine::world::Universe universe;
	std::string error;
	const engine::world::WorldId id =
		engine::game::ImportWorld(universe, demo->Path, engine::core::Name{}, error);
	INFO(error);
	REQUIRE(id.IsValid());

	std::shared_ptr<engine::script::Runtime> runtime;
	const auto settings = universe.SettingsOf(id);
	universe.Enter(id, [&](Store &store, Scheduler &systems) {
		client::InstallPresentation(store, systems);
		client::InstallClientWorldSystems(store, systems, settings.PhysicsTickRate);
		REQUIRE(store.Resource<engine::physics::PhysicsWorld>());
		(void)client::InstallDefaultCamera(store, systems);
		const engine::ecs::Entity localPlayer = client::EnsureLocalPlayer(store);
		REQUIRE(localPlayer != engine::ecs::NULL_ENTITY);

		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();
		runtime = engine::game::StartWorldScripts(
			store, systems, limits, error, nullptr, universe.SettingsOf(id).ScriptTickRate
		);
		INFO(error);
		REQUIRE(error.empty());
		REQUIRE(runtime != nullptr);
		REQUIRE(runtime->Costs().size() == BLADEBORNE_STARTUP_SCRIPT_COUNT);
		CHECK(std::ranges::all_of(runtime->Costs(), &engine::script::ScriptCost::Completed));
		(void)engine::script::ResetPlayerGui(store, localPlayer);

		const engine::ecs::Entity players = engine::scene::PlayersOf(store);
		const auto *playerSettings = store.Get<engine::scene::PlayersServiceComponent>(players);
		REQUIRE(playerSettings != nullptr);
		CHECK(playerSettings->CharacterAutoLoads);
		CHECK(engine::scene::CharacterOf(store, localPlayer) == engine::ecs::NULL_ENTITY);
		auto *mutableSettings = store.GetMutable<engine::scene::PlayersServiceComponent>(players);
		REQUIRE(mutableSettings != nullptr);
		mutableSettings->CharacterAutoLoads = false;
		std::vector<engine::ecs::Entity> spawned;
		CHECK(engine::scene::UpdateRespawns(store, spawned) == 0);
		CHECK(spawned.empty());
		CHECK(engine::scene::CharacterOf(store, localPlayer) == engine::ecs::NULL_ENTITY);
		mutableSettings->CharacterAutoLoads = true;
		REQUIRE(engine::scene::LoadCharacter(store, localPlayer) != engine::ecs::NULL_ENTITY);
		const engine::ecs::Entity firstCharacter = engine::scene::CharacterOf(store, localPlayer);
		REQUIRE(firstCharacter != engine::ecs::NULL_ENTITY);
		CHECK(CountNamedDescendants(store, engine::scene::WorkspaceOf(store), "Player") == 1);
		const auto *firstRig = store.Get<engine::scene::Character>(firstCharacter);
		REQUIRE(firstRig != nullptr);
		systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
		const auto *active = store.Resource<engine::scene::ActiveCamera>();
		REQUIRE(active != nullptr);
		const auto *subject = store.Get<engine::scene::CameraSubject>(active->Entity);
		REQUIRE(subject != nullptr);
		CHECK(subject->Target == firstRig->Humanoid);
		CHECK(engine::scene::CameraSubjectRoot(store, active->Entity) == firstRig->Root);
		const auto *firstCameraTransform = store.Get<Transform>(active->Entity);
		REQUIRE(firstCameraTransform != nullptr);
		const engine::core::Vector3 firstCameraPosition = firstCameraTransform->Frame.Position;
		systems.RunPhases(store, Phase::Simulation, Phase::Simulation);
		const auto *simulatedCameraTransform = store.Get<Transform>(active->Entity);
		REQUIRE(simulatedCameraTransform != nullptr);
		CHECK(simulatedCameraTransform->Frame.Position == firstCameraPosition);

		CHECK(engine::scene::RemoveCharacter(store, localPlayer));
		CHECK(engine::scene::CharacterOf(store, localPlayer) == engine::ecs::NULL_ENTITY);
		REQUIRE(engine::scene::LoadCharacter(store, localPlayer) != engine::ecs::NULL_ENTITY);
		const engine::ecs::Entity respawnedCharacter = engine::scene::CharacterOf(store, localPlayer);
		REQUIRE(respawnedCharacter != engine::ecs::NULL_ENTITY);
		CHECK(respawnedCharacter != firstCharacter);
		CHECK(CountNamedDescendants(store, engine::scene::WorkspaceOf(store), "Player") == 1);
		systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
		const auto *respawnedRig = store.Get<engine::scene::Character>(respawnedCharacter);
		REQUIRE(respawnedRig != nullptr);
		const auto *respawnedSubject = store.Get<engine::scene::CameraSubject>(active->Entity);
		REQUIRE(respawnedSubject != nullptr);
		CHECK(respawnedSubject->Target == respawnedRig->Humanoid);
		CHECK(engine::scene::CameraSubjectRoot(store, active->Entity) == respawnedRig->Root);
		const auto *respawnedCameraTransform = store.Get<Transform>(active->Entity);
		REQUIRE(respawnedCameraTransform != nullptr);
		const engine::core::Vector3 respawnedCameraPosition = respawnedCameraTransform->Frame.Position;
		systems.RunPhases(store, Phase::Simulation, Phase::Simulation);
		const auto *simulatedRespawnCamera = store.Get<Transform>(active->Entity);
		REQUIRE(simulatedRespawnCamera != nullptr);
		CHECK(simulatedRespawnCamera->Frame.Position == respawnedCameraPosition);

		const engine::script::SourceCache *sources = store.Resource<engine::script::SourceCache>();
		REQUIRE(sources != nullptr);
		CHECK(sources->Count() == BLADEBORNE_SOURCE_COUNT);

		const engine::ecs::Entity arena = InWorkspace(store, "BladeborneArena");
		REQUIRE(arena != engine::ecs::NULL_ENTITY);
		CHECK(CountNamedDescendants(store, arena, "ServerProfileMarker") == 1);

		const engine::ecs::Entity starterGui = store.FindFirstRoot(engine::gui::STARTER_GUI);
		REQUIRE(starterGui != engine::ecs::NULL_ENTITY);
		CHECK(store.FindFirstChild(starterGui, "BladeborneHUD") == engine::ecs::NULL_ENTITY);

		const engine::ecs::Entity playerGui = store.FindFirstChild(localPlayer, engine::gui::PLAYER_GUI);
		REQUIRE(playerGui != engine::ecs::NULL_ENTITY);
		const engine::ecs::Entity liveHud = store.FindFirstChild(playerGui, "BladeborneHUD");
		REQUIRE(liveHud != engine::ecs::NULL_ENTITY);
		CHECK(engine::scene::InPlayerGui(store, liveHud, localPlayer));
		CHECK(CountNamedDescendants(store, liveHud, "Ability1") == 1);
		CHECK(CountNamedDescendants(store, liveHud, "Minimap") == 1);
		CHECK(CountNamedDescendants(store, liveHud, "HotbarSlot1") == 2);

		engine::gui::Screen display;
		display.Width = 1920.0f;
		display.Height = 1080.0f;
		CHECK(engine::gui::Layout(store, display) > 0);
		engine::gui::CompileRequest request;
		request.Display = display;
		request.ScreenGuis = engine::gui::ScreenGuiSource::PlayerGui;
		request.Viewer = localPlayer;
		engine::gui::Compiled compiled;
		REQUIRE(compiled.Rebuild(store, request));
		CHECK(compiled.Commands().Elements > 0);
		CHECK(std::ranges::any_of(compiled.Commands().Commands, [&](const auto &command) {
			return command.Kind == engine::gui::DrawKind::Text && !command.Text.empty() &&
				   store.IsDescendantOf(command.Source, liveHud);
		}));
		const bool opaqueScreenCover =
			std::ranges::any_of(compiled.Commands().Commands, [&](const auto &command) {
				return command.Kind == engine::gui::DrawKind::Rectangle && command.Transparency == 0.0f &&
					   command.Bounds.Min.X <= 0.0f && command.Bounds.Min.Y <= 0.0f &&
					   command.Bounds.Max.X >= display.Width && command.Bounds.Max.Y >= display.Height;
			});
		CHECK_FALSE(opaqueScreenCover);

		const engine::ecs::Entity ability = FirstNamedDescendant(store, liveHud, "Ability1");
		REQUIRE(ability != engine::ecs::NULL_ENTITY);
		const engine::gui::Resolved *placed = store.Get<engine::gui::Resolved>(ability);
		REQUIRE(placed != nullptr);
		CHECK(placed->Rendered);

		systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
		const engine::render::DrawList *drawList = store.Resource<engine::render::DrawList>();
		REQUIRE(drawList != nullptr);
		for (const std::string_view name : std::array{
				 "ArenaFloor",
				 "CentralDais",
				 "NorthMarker",
				 "SouthMarker",
				 "WestBlade",
				 "EastBlade",
				 "PlayerSpawn",
				 "ServerProfileMarker",
			 }) {
			const engine::ecs::Entity part = FirstNamedDescendant(store, arena, name);
			REQUIRE(part != engine::ecs::NULL_ENTITY);
			CHECK(std::ranges::any_of(drawList->Instances, [part](const DrawInstance &instance) {
				return instance.Source == part.Id;
			}));
		}
	});
}

TEST_CASE(
	"the TornadoSim world streams resettable storm scenery through its live scripts",
	"[client][world][tornado]"
) {
	engine::parallel::Jobs::Start(2);
	struct StopJobs {
		~StopJobs() {
			engine::parallel::Jobs::Stop();
		}
	} stopJobs;

	const auto demo =
		engine::examples::DemosLoader().Find(engine::examples::DemoKind::World, "TornadoSim.aworld");
	REQUIRE(demo.has_value());

	engine::world::Universe universe;
	std::string error;
	const engine::world::WorldId id =
		engine::game::ImportWorld(universe, demo->Path, engine::core::Name{}, error);
	INFO(error);
	REQUIRE(id.IsValid());

	universe.Enter(id, [&](Store &store, Scheduler &systems) {
		client::InstallPresentation(store, systems);
		REQUIRE(client::InstallDefaultCamera(store, systems));
		client::InstallClientWorldSystems(store, systems);
		const engine::ecs::Entity localPlayer = client::EnsureLocalPlayer(store);
		REQUIRE(localPlayer != engine::ecs::NULL_ENTITY);

		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();
		const std::shared_ptr<engine::script::Runtime> runtime = engine::game::StartWorldScripts(
			store, systems, limits, error, nullptr, universe.SettingsOf(id).ScriptTickRate
		);
		INFO(error);
		REQUIRE(error.empty());
		REQUIRE(runtime != nullptr);
		CHECK(std::ranges::all_of(runtime->Costs(), &engine::script::ScriptCost::Completed));

		const engine::ecs::Entity environment = InWorkspace(store, "Tornado Environment Cells");
		REQUIRE(environment != engine::ecs::NULL_ENTITY);
		auto childCount = [&](engine::ecs::Entity parent) {
			size_t count = 0;
			store.EachChild(parent, [&](engine::ecs::Entity) { count++; });
			return count;
		};
		CHECK(childCount(environment) == 26);
		CHECK(CountNamedDescendants(store, environment, "Cell Tree 0") == 25);
		CHECK(CountNamedDescendants(store, environment, "Cell Sign 1") == 25);
		CHECK(CountNamedDescendants(store, environment, "Cell Roof Panel 2") == 25);
		CHECK(CountNamedDescendants(store, environment, "Cell Chase Vehicle") > 0);

		REQUIRE(runtime->Run(R"(
			assert(World:Count("Tornado.Response") >= 80)
			assert(World:Count("Tornado.Link") >= 60)
			local state = workspace:FindFirstChild("StormAnchor", true):GetComponent("Tornado.State")
			assert(state ~= nil)
			local expected = {
				Energy = 0.72, CoreRadius = 34, InfluenceRadius = 260,
				PeakTangentialSpeed = 92, PeakInflowSpeed = 34, PeakUpdraftSpeed = 58,
				PeakDowndraftSpeed = 32, SurfaceOutflowSpeed = 28, PressureDrop = 72,
				Humidity = 0.82, RainRate = 0.68, Turbulence = 13, GroundFriction = 0.28,
				DebrisDensity = 0.65, VortexTightness = 2.25, TopHeight = 360,
			}
			for field, wanted in expected do
				assert(math.abs(state[field] - wanted) < 0.0001, field)
			end
			assert(state.UpperWind == Vector3.new(18, 0, -6))
			assert(state.TranslationVelocity == Vector3.new(4, 0, 1.5))
		)"));

		REQUIRE(runtime->Run(R"(
			game:GetService("ReplicatedStorage").TornadoControl:FireServer('{"kind":"motion","x":-15,"y":5,"z":25}')
			game:GetService("ReplicatedStorage").TornadoControl:FireServer('{"kind":"rotation","counterClockwise":false}')
		)"));
		REQUIRE(runtime->Run(R"(
			local state = workspace:FindFirstChild("StormAnchor", true):GetComponent("Tornado.State")
			assert(state.TranslationVelocity == Vector3.new(-15, 5, 25))
			assert(not state.CounterClockwise)
		)"));

		const engine::ecs::Entity playerGui = store.FindFirstChild(localPlayer, engine::gui::PLAYER_GUI);
		REQUIRE(playerGui != engine::ecs::NULL_ENTITY);
		const engine::ecs::Entity condensationButton = FirstNamedDescendant(store, playerGui, "COND");
		// A combined host runs the StarterPlayerScripts template and the local
		// player's clone. The panel belongs to the clone, whose cosmetic emitter
		// was created last; asserting against the template's earlier emitter
		// would observe a different client script's local state.
		const engine::ecs::Entity condensationEmitter =
			LastNamedDescendant(store, engine::scene::WorkspaceOf(store), "Condensation Streamers");
		const engine::ecs::Entity gpuParticles =
			FirstNamedDescendant(store, engine::scene::WorkspaceOf(store), "TornadoParticles");
		REQUIRE(condensationButton != engine::ecs::NULL_ENTITY);
		REQUIRE(condensationEmitter != engine::ecs::NULL_ENTITY);
		REQUIRE(gpuParticles != engine::ecs::NULL_ENTITY);

		engine::gui::CompileRequest panelRequest;
		panelRequest.Display = {
			.Width = 1280.0f,
			.Height = 960.0f,
			.SafeArea = {},
			.Occluded = {},
		};
		panelRequest.ScreenGuis = engine::gui::ScreenGuiSource::PlayerGui;
		panelRequest.Viewer = localPlayer;
		engine::gui::Compiled panel;
		engine::gui::Router panelRouter;
		const auto panelFrame = [&](engine::core::Vector2 position, bool down) {
			panelRequest.Hovered = panelRouter.Hovered();
			panelRequest.Pressed = panelRouter.Pressed();
			REQUIRE(engine::gui::Layout(store, panelRequest.Display) > 0);
			REQUIRE(panel.Rebuild(store, panelRequest));

			engine::gui::Pointer pointer;
			pointer.Position = position;
			pointer.Down = down;
			pointer.Inside = true;
			pointer.ScreenOnly = true;
			runtime->DeliverGuiEvents(panelRouter.Update(store, panel.Commands(), pointer));
			systems.Tick(store, STEP);
		};
		panelFrame({0.0f, 0.0f}, false);
		const engine::gui::Resolved *condensationResolved =
			store.Get<engine::gui::Resolved>(condensationButton);
		REQUIRE(condensationResolved != nullptr);
		const engine::core::Vector2 condensationCentre =
			condensationResolved->AbsolutePosition + condensationResolved->AbsoluteSize * 0.5f;
		panelFrame(condensationCentre, false);
		panelFrame(condensationCentre, true);
		panelFrame(condensationCentre, false);

		const auto *condensation = store.Get<engine::effects::ParticleEmitter>(condensationEmitter);
		const auto *field = store.Get<engine::scene::GpuParticleField>(gpuParticles);
		REQUIRE(condensation != nullptr);
		REQUIRE(field != nullptr);
		CHECK_FALSE(condensation->Enabled);
		CHECK(condensation->Rate == Approx(0.0f));
		CHECK_FALSE(engine::scene::HasGpuParticleLayer(*field, engine::scene::GpuParticleLayer::First));

		REQUIRE(runtime->Run(R"(
			game:GetService("ReplicatedStorage").TornadoControl:FireServer('{"kind":"preset","name":"EF5"}')
		)"));
		REQUIRE(runtime->Run(R"(
			local state = workspace:FindFirstChild("StormAnchor", true):GetComponent("Tornado.State")
			assert(math.abs(state.CoreRadius - 45) < 0.0001)
		)"));
		systems.Tick(store, STEP);
		REQUIRE(runtime->Run(R"(
			assert(workspace:FindFirstChild("StormAnchor", true):GetComponent("Tornado.State").ElapsedSeconds > 0)
		)"));

		// The first fixed step delivers the user-lane request after the server
		// heartbeat. The second consumes its refresh flag without a long physics run.
		REQUIRE(runtime->Run(R"(
			game:GetService("ReplicatedStorage").TornadoControl:FireServer('{"kind":"observer","x":960,"z":735}')
		)"));
		systems.Tick(store, STEP);
		systems.Tick(store, STEP);
		CHECK(runtime->LastError().empty());
		CHECK(store.FindFirstChild(environment, "Storm Cell 4:2") != engine::ecs::NULL_ENTITY);

		const engine::ecs::Entity firstCell = store.FindFirstChild(environment, "Storm Cell 4:2");
		REQUIRE(firstCell != engine::ecs::NULL_ENTITY);
		store.DestroyInstance(firstCell);
		CHECK(store.FindFirstChild(environment, "Storm Cell 4:2") == engine::ecs::NULL_ENTITY);
		const engine::ecs::Entity interaction = InWorkspace(store, "StormInteraction");
		REQUIRE(interaction != engine::ecs::NULL_ENTITY);
		const engine::ecs::Entity woodenSign = FirstNamedDescendant(store, interaction, "WoodenSign");
		const engine::ecs::Entity woodenLink =
			FirstNamedDescendant(store, interaction, "Wood Sign Wind Link");
		REQUIRE(woodenSign != engine::ecs::NULL_ENTITY);
		REQUIRE(woodenLink != engine::ecs::NULL_ENTITY);
		store.DestroyInstance(woodenSign);
		store.DestroyInstance(woodenLink);
		CHECK(FirstNamedDescendant(store, interaction, "WoodenSign") == engine::ecs::NULL_ENTITY);
		CHECK(FirstNamedDescendant(store, interaction, "Wood Sign Wind Link") == engine::ecs::NULL_ENTITY);
		systems.Tick(store, STEP);
		CHECK(runtime->LastError().empty());

		REQUIRE(runtime->Run(R"(
			game:GetService("ReplicatedStorage").TornadoControl:FireServer('{"kind":"reset"}')
		)"));
		REQUIRE(runtime->Run(R"(
			assert(workspace:FindFirstChild("StormAnchor", true):GetComponent("Tornado.State").ElapsedSeconds == 0)
		)"));
		systems.Tick(store, STEP);

		CHECK(childCount(environment) == 26);
		CHECK(store.FindFirstChild(environment, "Storm Cell 4:2") != engine::ecs::NULL_ENTITY);
		CHECK(CountNamedDescendants(store, environment, "Cell Tree 0") == 25);
		const engine::ecs::Entity resetInteraction = InWorkspace(store, "StormInteraction");
		REQUIRE(resetInteraction != engine::ecs::NULL_ENTITY);
		CHECK(FirstNamedDescendant(store, resetInteraction, "WoodenSign") != engine::ecs::NULL_ENTITY);
		CHECK(
			FirstNamedDescendant(store, resetInteraction, "Wood Sign Wind Link") != engine::ecs::NULL_ENTITY
		);
		REQUIRE(runtime->Run(R"(
			local state = workspace:FindFirstChild("StormAnchor", true):GetComponent("Tornado.State")
			assert(math.abs(state.Energy - 0.72) < 0.0001)
			assert(math.abs(state.CoreRadius - 34) < 0.0001)
		)"));
	});
}

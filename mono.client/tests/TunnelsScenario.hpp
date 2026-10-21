#pragma once

// The untouched staged Tunnels world serves three original cases in two suites.
// Selection is frozen by the host before any case, so an unselected NPC check
// never extends a lighting or wire-only request to 240 ticks.

#include "ScenarioChecks.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Scenario.hpp>
#include <engine/testing/Suite.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace client::tests {
	namespace tunnels {
		inline constexpr std::string_view NPC_CASE =
			"a scripted NPC is integrated and crosses a portal in a standalone client";
		inline constexpr std::string_view WIRE_CASE =
			"the tunnels remote portal destination fits the replicated world";
		inline constexpr std::string_view LIGHTS_CASE = "tunnels local lights stay below the device cap";
		inline constexpr float STEP = 1.0f / 60.0f;

		inline bool Selected(std::string_view suiteId, std::string_view caseName) {
			const auto &suites = engine::testing::Registry::All();
			const auto suite = std::ranges::find(suites, suiteId, &engine::testing::Suite::Id);
			if (suite == suites.end()) return false;
			return std::ranges::any_of(engine::testing::SelectedCases(), [&](const auto &selected) {
				return selected.File == suite->File && selected.Name == caseName;
			});
		}

		struct Selection {
			bool Npc = Selected("client.scene.tick", NPC_CASE);
			bool Wire = Selected("client.scene.tick", WIRE_CASE);
			bool Lights = Selected("client.scene.portallighting", LIGHTS_CASE);
		};

		struct WireCorners {
			engine::core::Vector3 Minimum;
			engine::core::Vector3 Maximum;
		};

		struct Facts {
			bool InitialCaptured = false;
			bool NpcFinalCaptured = false;
			bool LightsCaptured = false;
			bool CharacterPresent = false;
			bool RootPresent = false;
			bool RootTransformPresent = false;
			float BeforeZ = 0.0f;
			float AfterZ = 0.0f;
			bool TransitPresent = false;
			uint64_t TransitSerial = 0;
			bool RemotePanePresent = false;
			bool DestinationTransformPresent = false;
			bool DestinationBoundsPresent = false;
			engine::core::Vector3 DestinationPosition;
			engine::core::Vector3 DestinationHalfExtent;
			bool EastPanePresent = false;
			bool PortalPresent = false;
			bool PortalDestinationMatches = false;
			bool GroundPresent = false;
			bool GroundBoundsPresent = false;
			engine::core::Vector3 GroundHalfExtent;
			std::vector<WireCorners> BoundsCorners;
			std::vector<engine::core::Vector3> Positions;
			size_t LightCount = 0;
			uint64_t Ticks = 0;
		};

		struct Runtime {
			engine::ecs::Store World{"integration"};
			engine::ecs::Scheduler Systems;
			std::shared_ptr<engine::script::Runtime> Scripts;
			std::filesystem::path PreviousAssets = engine::core::Paths::Assets();

			Runtime() {
				engine::parallel::Jobs::Start(2);
				engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
			}
			~Runtime() {
				engine::core::Paths::SetAssetsOverride(PreviousAssets);
				engine::parallel::Jobs::Stop();
			}
			void Build() {
				if (!client::BuildScriptedWorld(
						World, Systems, engine::examples::ExamplePath("Tunnels.luau"), 512, &Scripts
					) ||
					!Scripts) {
					throw std::runtime_error("shared Tunnels world failed to load its runtime");
				}
			}
		};

		inline engine::ecs::Entity InWorkspace(engine::ecs::Store &world, std::string_view name) {
			const auto workspace = engine::scene::WorkspaceOf(world);
			return workspace == engine::ecs::NULL_ENTITY ? engine::ecs::NULL_ENTITY
														 : world.FindFirstChild(workspace, name);
		}

		inline engine::ecs::Entity CopyNpcInitial(engine::ecs::Store &world, Facts &facts) {
			const auto character = InWorkspace(world, "TunnelWalker");
			facts.CharacterPresent = character != engine::ecs::NULL_ENTITY;
			const auto root = facts.CharacterPresent ? world.FindFirstChild(character, "HumanoidRootPart")
													 : engine::ecs::NULL_ENTITY;
			facts.RootPresent = root != engine::ecs::NULL_ENTITY;
			if (const auto *transform = world.Get<engine::scene::Transform>(root)) {
				facts.RootTransformPresent = true;
				facts.BeforeZ = transform->Frame.Position.Z;
			}
			return root;
		}

		inline void CopyWire(engine::ecs::Store &world, Facts &facts) {
			const auto remotePane = InWorkspace(world, "ShortInteriorNorth");
			facts.RemotePanePresent = remotePane != engine::ecs::NULL_ENTITY;
			if (const auto *transform = world.Get<engine::scene::Transform>(remotePane)) {
				facts.DestinationTransformPresent = true;
				facts.DestinationPosition = transform->Frame.Position;
			}
			if (const auto *bounds = world.Get<engine::scene::Bounds>(remotePane)) {
				facts.DestinationBoundsPresent = true;
				facts.DestinationHalfExtent = bounds->HalfExtent;
			}
			const auto eastPane = InWorkspace(world, "ShortNorth");
			facts.EastPanePresent = eastPane != engine::ecs::NULL_ENTITY;
			if (facts.EastPanePresent) {
				world.EachChild(eastPane, [&](engine::ecs::Entity child) {
					if (facts.PortalPresent) return;
					if (const auto *portal = world.Get<engine::scene::Portal>(child)) {
						facts.PortalPresent = true;
						facts.PortalDestinationMatches = portal->Destination == remotePane;
					}
				});
			}
			const auto ground = InWorkspace(world, "Ground");
			facts.GroundPresent = ground != engine::ecs::NULL_ENTITY;
			if (const auto *bounds = world.Get<engine::scene::Bounds>(ground)) {
				facts.GroundBoundsPresent = true;
				facts.GroundHalfExtent = bounds->HalfExtent;
			}
			world.Each<const engine::scene::Transform, const engine::scene::Bounds>(
				[&](engine::ecs::Entity, const auto &transform, const auto &bounds) {
					facts.BoundsCorners.push_back(
						{transform.Frame.Position - bounds.HalfExtent,
						 transform.Frame.Position + bounds.HalfExtent}
					);
				}
			);
			world.Each<const engine::scene::Transform>([&](engine::ecs::Entity, const auto &transform) {
				facts.Positions.push_back(transform.Frame.Position);
			});
		}

		inline std::vector<engine::testing::ScenarioCheck<Facts>> Checks(const Selection &selected) {
			using engine::testing::CheckResult;
			std::vector<engine::testing::ScenarioCheck<Facts>> checks;
			if (selected.Npc) {
				// Intent alone is not motion. This is the original standalone
				// regression for the missing character physics pipeline.
				checks.push_back(
					{"npc",
					 "client.scene.tick",
					 {{{"checkpoints"}}},
					 [](const auto &observation, CheckResult &result) {
						 const auto &facts = observation.Payload;
						 result.Record(
							 "initial and 240-tick checkpoints",
							 facts.InitialCaptured && facts.NpcFinalCaptured && facts.Ticks == 240
						 );
						 result.Record("TunnelWalker exists", facts.CharacterPresent);
						 result.Record("HumanoidRootPart exists", facts.RootPresent);
						 result.Record("root transform exists", facts.RootTransformPresent);
						 result.Record("NPC moved", !Near(facts.AfterZ, facts.BeforeZ));
						 result.Record("portal transit exists", facts.TransitPresent);
						 result.Record("NPC crossed a portal", facts.TransitSerial >= 1);
					 },
					 {}}
				);
			}
			if (selected.Wire) {
				// Preserve every authored corner and transform check: the remote
				// room once crossed the wire edge and detached a player's camera.
				checks.push_back(
					{"wire",
					 "client.scene.tick",
					 {{{"checkpoints"}}},
					 [](const auto &observation, CheckResult &result) {
						 const auto &facts = observation.Payload;
						 const auto inside = [](engine::core::Vector3 position) {
							 const float limit = engine::scene::WIRE_POSITION_HALF_EXTENT_METRES;
							 return std::abs(position.X) <= limit && std::abs(position.Y) <= limit &&
									std::abs(position.Z) <= limit;
						 };
						 result.Record("initial wire checkpoint", facts.InitialCaptured);
						 result.Record("remote pane exists", facts.RemotePanePresent);
						 result.Record("destination transform exists", facts.DestinationTransformPresent);
						 result.Record("destination bounds exist", facts.DestinationBoundsPresent);
						 result.Record("destination inside wire range", inside(facts.DestinationPosition));
						 result.Record("east pane exists", facts.EastPanePresent);
						 result.Record("east portal exists", facts.PortalPresent);
						 result.Record("east portal targets remote pane", facts.PortalDestinationMatches);
						 result.Record("ground exists", facts.GroundPresent);
						 result.Record("ground bounds exist", facts.GroundBoundsPresent);
						 result.Record(
							 "destination outside ground",
							 facts.DestinationPosition.X - facts.DestinationHalfExtent.X >
								 facts.GroundHalfExtent.X
						 );
						 for (const auto &corners : facts.BoundsCorners) {
							 result.Record("bounds minimum inside wire range", inside(corners.Minimum));
							 result.Record("bounds maximum inside wire range", inside(corners.Maximum));
						 }
						 for (const auto position : facts.Positions) {
							 result.Record("transform inside wire range", inside(position));
						 }
					 },
					 {}}
				);
			}
			if (selected.Lights) {
				checks.push_back(
					{"authored",
					 "client.scene.portallighting",
					 {{{"checkpoints"}}},
					 [](const auto &observation, CheckResult &result) {
						 const auto &facts = observation.Payload;
						 result.Record("one-tick light checkpoint", facts.LightsCaptured);
						 result.Record("ten authored tunnel lights", facts.LightCount == 10);
						 result.Record(
							 "below device cap", facts.LightCount < engine::render::MAX_SCENE_LIGHTS
						 );
					 },
					 {}}
				);
			}
			return checks;
		}
	}

	// Reuse the completed report, including failures, only within the frozen run.
	// A failed shared setup must be attributed to every selected original case.
	inline engine::testing::ScenarioReport TunnelsScenario() {
		using namespace engine::testing;
		struct Memo {
			uint64_t Generation = 0;
			std::optional<ScenarioReport> Report;
		};
		static Memo memo;
		const uint64_t generation = SelectionGeneration();
		if (memo.Generation != generation) {
			memo.Report.reset();
			memo.Generation = generation;
		}
		if (memo.Report) {
			auto reused = *memo.Report;
			reused.Reused = true;
			reused.Metrics = {};
			return reused;
		}
		const tunnels::Selection selected;
		if (generation == 0 || (!selected.Npc && !selected.Wire && !selected.Lights)) {
			throw std::logic_error("Tunnels scenario requires the host's frozen selected-case inventory");
		}
		ScenarioKey key;
		key.Scene = "Tunnels.luau";
		key.ContentRevision = "staged-examples;immutable-for-selected-process";
		key.Configuration = "world=integration;reserve=512;jobs=2;physics=default;host=standalone";
		key.TickSchedule = selected.Npc ? "240@float(1/60)" : selected.Lights ? "1@float(1/60)" : "0";
		key.FrameSchedule = "native scheduler tick only;no GPU";
		key.Actions = "none";
		key.Transport = "standalone";
		key.Faults = "none";
		key.CaptureConfiguration = "selected copied checkpoints at 0/1/240;one immutable fact bundle";
		key.ObservationSchema = "client.tunnels.checkpoints.v1";
		key.CheckSet = std::string("npc=") + (selected.Npc ? "1" : "0") +
					   ";wire=" + (selected.Wire ? "1" : "0") + ";lights=" + (selected.Lights ? "1" : "0") +
					   ";checks.v1";
		uint64_t advancedTicks = 0;
		ScenarioDriver<tunnels::Facts> driver;
		driver.Run = [selected, &advancedTicks](ScenarioSession<tunnels::Facts> &scenario) {
			tunnels::Runtime runtime;
			runtime.Build();
			tunnels::Facts facts;
			facts.InitialCaptured = selected.Npc || selected.Wire;
			engine::ecs::Entity root = engine::ecs::NULL_ENTITY;
			if (selected.Npc) root = tunnels::CopyNpcInitial(runtime.World, facts);
			if (selected.Wire) tunnels::CopyWire(runtime.World, facts);
			const size_t ticks = selected.Npc ? 240 : selected.Lights ? 1 : 0;
			for (size_t tick = 1; tick <= ticks; tick++) {
				scenario.Checkpoint();
				runtime.Systems.Tick(runtime.World, tunnels::STEP);
				advancedTicks++;
				if (tick == 1 && selected.Lights) {
					std::vector<engine::render::SceneLight> lights;
					engine::render::CollectLights(runtime.World, engine::core::Vector3{}, lights);
					facts.LightCount = lights.size();
					facts.LightsCaptured = true;
				}
			}
			facts.Ticks = runtime.World.Time().Tick;
			if (selected.Npc) {
				facts.NpcFinalCaptured = true;
				if (const auto *transform = runtime.World.Get<engine::scene::Transform>(root)) {
					facts.AfterZ = transform->Frame.Position.Z;
				} else
					facts.RootTransformPresent = false;
				if (const auto *transit = runtime.World.Get<engine::scene::PortalTransit>(root)) {
					facts.TransitPresent = true;
					facts.TransitSerial = transit->Serial;
				}
			}
			const size_t bytes = sizeof(tunnels::Facts) +
								 facts.BoundsCorners.capacity() * sizeof(tunnels::WireCorners) +
								 facts.Positions.capacity() * sizeof(engine::core::Vector3);
			scenario.Publish(
				{.Hook = "checkpoints",
				 .Tick = facts.Ticks,
				 .Payload = std::move(facts),
				 .RetainedBytes = bytes}
			);
		};
		memo.Report = RunScenario(std::move(key), tunnels::Checks(selected), driver);
		std::cout << "shared Tunnels scenario: setups=" << memo.Report->Metrics.SetupCount
				  << " native_ticks=" << advancedTicks
				  << " publications=" << memo.Report->Metrics.PublishedSamples
				  << " checks=" << memo.Report->Key.CheckSet << '\n';
		return *memo.Report;
	}
}

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/game/CollisionContent.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/CameraPortalView.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cmath>
#include <span>

namespace client {

	using engine::core::CFrame;
	using engine::core::Color3;
	using engine::core::Random;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::ecs::SystemOrder;
	using engine::render::DrawList;
	using engine::scene::ActiveCamera;
	using engine::scene::DrawInstance;
	using engine::scene::Transform;
	using engine::scene::WorldBounds;

	namespace {
		// Whether the client's fallback camera should hold its authored view.
		// The loader derives this once after the script has finished building the
		// world, so a fallback camera does not walk the instance tree every tick.
		struct FallbackCameraState {
			bool Standing = false;
		};

		// The deterministic sequence used to be an integer mixer written out
		// here, and the same one again in mono.server/src/Simulation.cpp. It is
		// engine::core::Random now - same reason, one copy, and a specified
		// algorithm rather than three constants nobody can check.

		// --- systems -------------------------------------------------------
		//
		// Every one of these is a plain function. None captures anything,
		// because there is nothing outside the world for it to capture - which
		// is what makes them registerable from bindings, replayable from a
		// recording, and reusable by a second world.

		// The camera is part of the scene, so it moves in the simulation on
		// simulated time. Driving it from wall time would slide it past
		// everything it is looking at whenever the frame rate changed.
		//
		// It is a row like anything else with a place in the world: a
		// `scene::Camera` and a `scene::Transform` on an entity, with the
		// `ActiveCamera` resource naming which of a world's cameras is live.
		// That is what makes a second one - a spectator, a mirror - a create
		// rather than a rewrite of this function.
		void MoveCamera(Store &store) {
			const ActiveCamera *active = store.Resource<ActiveCamera>();
			if (active == nullptr) {
				return;
			}

			// Read out before anything is written: `Set` may move the row this
			// resource's entity handle resolves to, and holding a pointer
			// across that is holding a pointer into storage that moved.
			const Entity entity = active->Entity;

			const auto now = static_cast<float>(store.Time().Elapsed);
			const float extent = store.Resource<WorldBounds>()->HalfExtent;

			// Far enough out that the whole scene fits, and drifting slowly so
			// that the depth buffer and the culling are visibly doing
			// something.
			const float distance = extent * 1.7f + 4.0f;

			// **Held still for a world somebody stands in, and it reads as a bug
			// otherwise.** The drift exists so that a scene with nothing moving
			// in it still shows the depth buffer and the culling working; a
			// world with a spawn pad has a character to do that. At 0.12 rad a
			// second a square plate like `Playground.luau` is a quarter turn
			// round within seven seconds of starting, which is exactly the
			// "sometimes the baseplate is rotated 45 degrees" report - the scene
			// is identical every run and how far it has turned by the time
			// anybody looks is not.
			//
			// The loader derives this after the script finishes authoring the
			// scene. Walking the instance tree here made the placeholder camera
			// cost grow with every part in every headless world.
			const auto *fallback = store.Resource<FallbackCameraState>();
			const bool standing = fallback != nullptr && fallback->Standing;
			const float angle = standing ? 0.6f : now * 0.12f;

			// The height drifts for the same reason the angle does, and is held
			// for the same reason: a world with a pad is looked at from one
			// place, so that two runs of it frame the scene identically.
			// Proportional to the scene rather than a constant, for the reason
			// `distance` already is: a fixed 6.5 studs is a grazing, nearly
			// edge-on look at anything the size of a baseplate, and the drifting
			// version only ever cleared that because it bobbed.
			const float height = standing ? extent * 0.55f + 6.0f : 5.0f + std::sin(now * 0.21f) * 3.5f;

			const Vector3 eye{
				std::cos(angle) * distance,
				height,
				std::sin(angle) * distance,
			};

			store.Set(entity, Transform{CFrame::LookAt(eye, Vector3::Zero)});

			if (engine::scene::Camera *lens = store.GetMutable<engine::scene::Camera>(entity)) {
				// The far plane follows the orbit rather than being a constant,
				// so growing the scene does not clip its far side away.
				lens->FarPlane = distance * 3.0f;
			}
		}

		// Brings the render gate in step with the tree.
		//
		// **`PreRender`, before `aim-surface-cameras` and collection**, because
		// what it produces is presentation state and this is the phase that
		// derives presentation state. The scheduler edges at registration make
		// that ordering explicit.
		//
		// **It is the one thing here that is structural**, which is worth
		// naming rather than hiding: adding or removing `Rendered` moves a row
		// to another archetype. That is acceptable because nothing in the
		// simulation reads `Rendered` - it exists to be a query term for the
		// draw list and for nothing else - and because every host derives it
		// the same way, so two runs of one scene still agree. The alternative
		// was `PostSimulation`, and it fails a world that presents without
		// ticking: the studio edits a suspended world, and it would have shown
		// nothing until somebody pressed play.
		void SyncVisibility(Store &store) {
			(void)engine::scene::SyncRendered(store);
		}

		// Places every surface camera parented to a part. See
		// `scene/SurfaceCameras.hpp` for the reflection and why it lives in
		// `scene` rather than in a script.
		//
		// **Before the draw list is collected, and that ordering matters here.**
		// Aiming a camera also writes its part's `Visual::Surface`, so a pass
		// that ran afterwards would publish a draw list built from last frame's
		// answer - a mirror would be one frame late to start showing anything,
		// which is invisible in a still scene and a flicker in a moving one.
		void AimSurfaces(Store &store) {
			(void)engine::scene::AimSurfaceCameras(store);
		}

	}

	// --- what the systems need, whoever built the entities --------------------
	//
	// Split out of `BuildDemoWorld` when the scene became loadable from a
	// script, and **`BuildDemoWorld` is gone now.** There is one path: a script
	// builds the world and a client installs the two systems it owns. Keeping
	// the C++ scene beside the Luau one would have been two ways to do one job,
	// which is the most expensive kind of debt in a monorepo because both
	// accumulate callers - and the scripted path is the one that proves the
	// bindings work.

	namespace {
		Entity InstallCamera(Store &store) {
			const Entity camera = store.Create();
			store.Set<Transform>(camera, Transform{});
			store.Set<engine::scene::Camera>(camera, engine::scene::Camera{});
			return camera;
		}

		// Built-in mesh counts are available before scripts load, without delivery.
		void RecordBuiltinMeshes(Store &store) {
			engine::scene::RegisterSceneComponents();

			struct Counted {
				engine::core::Name Name;
				uint32_t Triangles = 0;
			};

			static const std::vector<Counted> BUILTINS = [] {
				std::vector<Counted> counted;
				for (uint8_t index = 0; index < engine::assets::BUILTIN_MESH_COUNT; index++) {
					const auto builtin = static_cast<engine::assets::BuiltinMesh>(index);
					const engine::assets::MeshData mesh = engine::assets::MakeBuiltin(builtin);
					counted.push_back(
						{engine::core::Name(engine::assets::BuiltinName(builtin)),
						 static_cast<uint32_t>(mesh.Indices.size() / 3)}
					);
				}
				return counted;
			}();

			for (const Counted &builtin : BUILTINS) {
				engine::scene::RecordMesh(store, builtin.Name, builtin.Triangles);
			}

			// **And what they collide as, which nothing else was ever going to
			// do.** `MakeBuiltin` generates the six rather than shipping files,
			// so they never travel the content path that bakes a hull for an
			// arriving mesh - a `MeshPart` set to `Cube` with a hull collider
			// resolved to nothing and fell back to its bound. The same six
			// shapes on every host, out of `game::AddBuiltinCollisionShapes`.
			engine::game::RecordBuiltinCollisionShapes(store);
		}

		void InstallResources(Store &store, Entity camera, float extent, uint32_t reserve) {
			store.SetResource(WorldBounds{extent});

			ActiveCamera live;
			live.Entity = camera;
			store.SetResource(live);
			store.SetResource(DrawList{});

			store.ResourceMutable<DrawList>()->Instances.reserve(reserve);
		}

	}

	size_t SurveyWorlds(engine::world::Universe &universe, std::vector<WorldIdentity> &worlds) {
		worlds.clear();

		for (const engine::world::WorldId candidate : universe.Worlds()) {
			WorldIdentity found;
			found.Id = candidate;
			found.Authored = universe.NameOf(candidate);

			universe.Enter(candidate, [&found](Store &store) {
				const auto *replica = store.Resource<engine::world::Replica>();
				if (replica == nullptr || !replica->Active) {
					return;
				}
				found.IsReplica = true;
				found.View = replica->View;
				const auto *received = store.Resource<engine::replication::SnapshotBuffer>();
				found.Ready = received != nullptr && received->Newest() != 0;

				// **Only when it names one.** A `--connect` client's replica
				// mirrors a world in another process, so there is nothing here
				// for the name to mean and its own is the honest answer.
				if (replica->Of.IsValid()) {
					found.Authored = replica->Of;
				}
			});

			worlds.push_back(found);
		}

		return worlds.size();
	}

	engine::world::WorldId ResolveDestinationWorld(
		std::span<const WorldIdentity> worlds, engine::world::WorldId viewer, const engine::core::Name &wanted
	) {
		if (!wanted.IsValid()) {
			return {};
		}

		// Whose view is asking, so a copy can prefer a copy. A world that is not
		// a replica has an invalid `View`, and so does every authoritative
		// world - which is what makes the same comparison do both jobs.
		engine::core::Name view;
		engine::core::Name mine;
		for (const WorldIdentity &known : worlds) {
			if (known.Id == viewer) {
				view = known.View;
				mine = known.Authored;
				break;
			}
		}

		// **A pane naming its own world is nobody's destination**, which is the
		// rule this had before the survey and is worth keeping by name rather
		// than by handle: the check used to be `found == world`, and from inside
		// a replica the authority now answers to the same authored name and
		// would be found instead. What such a pane shows is this world - a
		// mirror, and visible as one.
		if (mine.IsValid() && mine == wanted) {
			return {};
		}

		engine::world::WorldId first;
		for (const WorldIdentity &known : worlds) {
			if (known.Id == viewer || known.Authored != wanted || !known.Ready) {
				continue;
			}
			if (known.View == view) {
				return known.Id;
			}
			if (!first.IsValid()) {
				first = known.Id;
			}
		}

		// **The fallback is deliberate rather than a leftover.** A client whose
		// universe holds one replica and the authority it mirrors has no second
		// copy to find, and showing the authority is a live room rather than
		// nothing.
		return first;
	}

	engine::world::WorldId ResolveCameraPortalWorld(
		engine::world::Universe &universe,
		engine::world::WorldId inputWorld,
		engine::world::WorldId visualWorld,
		engine::core::CFrame &eye,
		engine::scene::Camera &lens,
		engine::render::PortalImageHost *images,
		engine::render::PortalImageHost::Time now,
		engine::world::WorldId topologyOwner
	) {
		using namespace engine;
		scene::CameraPortalView history;
		ecs::Entity camera = ecs::NULL_ENTITY;
		universe.Enter(inputWorld, [&](Store &store) {
			if (const auto *active = store.Resource<scene::ActiveCamera>()) {
				camera = active->Entity;
				if (const auto *found = store.Get<scene::CameraPortalView>(camera)) history = *found;
			}
		});
		if (camera == ecs::NULL_ENTITY) return visualWorld;
		std::vector<WorldIdentity> worlds;
		SurveyWorlds(universe, worlds);
		core::Name authored;
		for (const auto &entry : worlds)
			if (entry.Id == inputWorld) authored = entry.Authored;
		if (!authored.IsValid()) return {};
		auto resolve = [&](const core::Name &name) {
			if (name == authored) return visualWorld;
			return ResolveDestinationWorld(worlds, visualWorld, name);
		};
		std::vector<scene::PortalSeam> seams;
		for (size_t hop = 0; hop < 8; ++hop) {
			const auto selected = history.Started ? resolve(core::Name(history.World)) : visualWorld;
			if (!selected.IsValid()) {
				ENGINE_LOG(
					core::LogLevel::Trace,
					"client",
					"camera route missing world {} from {}",
					history.World,
					authored.Text()
				);
				return {};
			}
			if (universe.IsRemote(selected)) {
				if (images == nullptr) return {};
				(void)images->RequestTopology(
					topologyOwner.IsValid() ? topologyOwner : inputWorld, selected, now
				);
				const auto *topology = images->Topology(selected, now);
				if (topology == nullptr) {
					ENGINE_LOG(
						core::LogLevel::Trace,
						"client",
						"camera route waiting for topology {} from {}",
						history.World,
						authored.Text()
					);
					return {};
				}
				seams = topology->Seams;
			} else {
				universe.Enter(selected, [&](Store &store) { scene::GatherPortalSeams(store, seams); });
			}
			const auto step = scene::StepCameraPortalView(history, authored.Text(), eye, seams);
			if (step == scene::CameraPortalStep::Invalid) {
				ENGINE_LOG(
					core::LogLevel::Trace,
					"client",
					"camera route invalid step in {} from {} at {},{},{}",
					history.World,
					authored.Text(),
					eye.Position.X,
					eye.Position.Y,
					eye.Position.Z
				);
				return {};
			}
			if (step == scene::CameraPortalStep::Crossed) {
				continue;
			}
			universe.Enter(inputWorld, [&](Store &store) { store.Set(camera, history); });
			eye = history.FromInput.Place(eye);
			lens.NearPlane *= history.FromInput.Scale;
			lens.FarPlane *= history.FromInput.Scale;
			return selected;
		}
		ENGINE_LOG(
			core::LogLevel::Trace,
			"client",
			"camera route hop limit in {} from {}",
			history.World,
			authored.Text()
		);
		return {};
	}

	bool UpdatePortalImages(
		engine::world::Universe &universe,
		engine::render::PortalImageHost &images,
		engine::world::WorldId world,
		const engine::render::View &viewer,
		const engine::render::PortalImageDemandSettings &settings,
		std::vector<engine::render::PortalView> &portals,
		std::vector<engine::render::SurfaceView> &surfaces,
		float alpha,
		engine::render::PortalImageHost::Time now,
		engine::world::WorldId topologyOwner
	) {
		ENGINE_PROFILE("product.portal images");
		bool requested = false;
		universe.Enter(world, [&](engine::ecs::Store &store) {
			store.Each<const engine::scene::Portal>([&](engine::ecs::Entity, const auto &portal) {
				requested = requested || (portal.Enabled && portal.DestinationWorld.IsValid());
			});
		});
		if (!requested) {
			images.RemoveViewport(viewer.Slot);
			return true;
		}
		std::vector<engine::render::PortalImageDemand> demands;
		std::vector<engine::scene::DrawInstance> sourceRows;
		for (const auto &row : viewer.Instances) {
			if (!row.SourceWorld.IsValid() || row.SourceWorld == viewer.WorldName) {
				sourceRows.push_back(row);
			}
		}
		auto sourceView = viewer;
		sourceView.Instances = sourceRows;
		universe.Enter(world, [&](engine::ecs::Store &store) {
			engine::render::CollectPortalImageDemands(store, sourceView, settings, demands, portals);
		});
		std::vector<WorldIdentity> worlds;
		if (!demands.empty()) {
			SurveyWorlds(universe, worlds);
		}
		std::vector<engine::render::PortalImageDestination> routes;
		for (const auto &demand : demands) {
			if (std::any_of(routes.begin(), routes.end(), [&](const auto &route) {
					return route.Authored == demand.DestinationWorld;
				})) {
				continue;
			}
			auto destination = ResolveDestinationWorld(worlds, world, demand.DestinationWorld);
			const auto authority = universe.Find(demand.DestinationWorld);
			// Keep an authenticated image producer across local body-replica arrival.
			if (authority.IsValid() && universe.IsRemote(authority) &&
				universe.LookupPresentation(authority, engine::render::PORTAL_REQUEST_CHANNEL).Generation !=
					0)
				destination = authority;
			if (destination.IsValid()) {
				routes.push_back({demand.DestinationWorld, destination});
				// Fetch return seams while the entrance image is already demanded.
				if (universe.IsRemote(destination))
					(void)images.RequestTopology(
						topologyOwner.IsValid() ? topologyOwner : world, destination, now
					);
			}
		}
		const auto issued = images.Submit(world, viewer.Slot, demands, routes, now);
		const auto progress = images.Pump(0, alpha, now, true);
		engine::core::Metrics::Count("product.portal.requests", issued);
		engine::core::Metrics::Count("product.portal.captures", progress.Rendered);
		engine::core::Metrics::Count("product.portal.replies", progress.Sent);
		engine::core::Metrics::Count(
			"product.portal.source-copy-bytes", sourceRows.size() * sizeof(engine::scene::DrawInstance)
		);
		for (auto &portal : portals) {
			if (portal.ExternalImage) {
				portal.ImportedImage = images.Image(viewer.Slot, portal.ImagePortal);
				const auto captured = images.Capture(viewer.Slot, portal.ImagePortal);
				if (!captured || captured->TransparentImages[0] == 0) continue;
				portal.ImportedImage = 0;
				const auto demand = std::find_if(demands.begin(), demands.end(), [&](const auto &entry) {
					return entry.Binding.Portal == portal.ImagePortal;
				});
				if (demand == demands.end() || captured->EyePlayer.empty() ||
					captured->EyePlayer != demand->Request.EyePlayer)
					continue;
				std::vector<engine::scene::DrawInstance> copied, bodyRows;
				std::vector<engine::core::CFrame> joints;
				engine::render::PortalDrawSelection selected{captured->EyePlayer, {}};
				std::string error;
				if (!demand->Request.Geometry.empty() &&
					!engine::render::AppendPortalDraws(
						demand->Request.Geometry, viewer.WorldName, copied, joints, error, &selected
					))
					continue;
				const bool firstPerson =
					viewer.EyePlayer && std::to_string(*viewer.EyePlayer) == captured->EyePlayer;
				if (!firstPerson)
					for (const auto index : selected.Hidden)
						bodyRows.push_back(copied[index]);
				engine::render::SceneTarget target{settings.Width, settings.Height};
				engine::render::View body;
				body.World = viewer.World;
				body.WorldName = viewer.WorldName;
				body.Slot = viewer.Slot;
				body.Target = &target;
				body.Instances = bodyRows;
				body.JointFrames = joints;
				portal.ImportedImage = images.ComposeBodyImage(portal.ImagePortal, body);
			}
		}
		std::erase_if(surfaces, [&](const engine::render::SurfaceView &surface) {
			return std::any_of(portals.begin(), portals.end(), [&](const auto &portal) {
				return portal.ExternalImage && portal.Index == surface.Index;
			});
		});
		return std::all_of(demands.begin(), demands.end(), [&](const auto &demand) {
			return std::any_of(portals.begin(), portals.end(), [&](const auto &portal) {
				return portal.ExternalImage && portal.ImagePortal == demand.Binding.Portal &&
					   portal.ImportedImage != 0;
			});
		});
	}

	size_t AppendForeignPortalClones(
		engine::world::Universe &universe,
		engine::world::WorldId world,
		std::vector<engine::scene::DrawInstance> &drawn,
		std::vector<engine::core::CFrame> *joints
	) {
		ENGINE_PROFILE("product.portal foreground");
		std::vector<engine::scene::PortalSeam> seams;
		universe.Enter(world, [&](Store &store) { engine::scene::GatherPortalSeams(store, seams); });
		std::vector<engine::core::Name> wanted;
		for (const auto &seam : seams) {
			if (seam.Crosses &&
				std::find(wanted.begin(), wanted.end(), seam.DestinationWorld) == wanted.end()) {
				wanted.push_back(seam.DestinationWorld);
			}
		}
		if (wanted.empty()) {
			return 0;
		}
		std::vector<WorldIdentity> surveyed;
		SurveyWorlds(universe, surveyed);
		const auto source = std::find_if(surveyed.begin(), surveyed.end(), [world](const auto &entry) {
			return entry.Id == world;
		});
		if (source == surveyed.end()) {
			return 0;
		}
		const auto here = source->Authored;
		const size_t first = drawn.size();
		std::vector<engine::world::WorldId> visited;
		for (const auto destination : wanted) {
			const auto far = ResolveDestinationWorld(surveyed, world, destination);
			if (!far.IsValid() || std::find(visited.begin(), visited.end(), far) != visited.end()) {
				continue;
			}
			visited.push_back(far);
			const auto farName = universe.NameOf(far);
			universe.Enter(far, [&](Store &store) {
				const auto *list = store.Resource<DrawList>();
				if (list == nullptr) {
					return;
				}
				engine::scene::GatherPortalSeams(store, seams);
				for (const auto &seam : seams) {
					if (!seam.Crosses || seam.DestinationWorld != here) {
						continue;
					}
					const size_t begin = drawn.size();
					engine::scene::AppendPortalClones(store, seam, list->Instances, drawn);
					auto clones = std::span(drawn).subspan(begin);
					for (auto &clone : clones) {
						clone.SourceWorld = farName;
					}
					if (joints != nullptr) {
						engine::render::RebaseSkinPalettes(clones, list->JointFrames, *joints);
					} else {
						for (auto &clone : clones) {
							clone.SkinFirst = 0;
							clone.SkinCount = 0;
						}
					}
				}
			});
		}
		return drawn.size() - first;
	}

	namespace {
		// The three effects systems, installed together because they are one
		// dependency chain and installing two of the three is a scene where
		// nothing emits.
		//
		// **`ResolveAttachments` first and in `PreSimulation`**, because an
		// emitter parented to an attachment reads that attachment's world frame
		// when it spawns - and spawning happens in the same phase. Resolving after
		// would emit from where the attachment was last frame, which on a fast
		// projectile is a visible lag between the rocket and its exhaust.
		//
		// **`RecordTrails` in the simulation and not here**, which is the one that
		// does not follow the pattern: a trail is a record of where something has
		// been, so sampling it at frame rate would make its length depend on the
		// machine drawing it. `Ribbon.hpp` carries the argument.
		void InstallEffects(
			Store &store, Scheduler &scheduler, uint32_t poolCapacity, uint32_t maximumPoolCapacity
		) {
			engine::effects::RegisterEffectClasses();

			if (!store.HasResource<engine::effects::ParticleSystem>()) {
				engine::effects::InstallParticles(store, poolCapacity, maximumPoolCapacity);
			}

			// **Every world a client installs is stepped on the device**, which
			// is what turns the ageing half of `StepParticles` off. A client has
			// a renderer by definition, and `render::Renderer` owns the pool: it
			// emits, integrates and shades on the device, then writes instances
			// straight into the draw stream. Nothing crosses the bus unless an
			// emitter's resident parameters change.
			//
			// The host-side pass is what a test asserts against and what a build
			// with no compute device would fall back on; it is not what a client
			// runs. See `ParticleSystem::DeviceStepped`.
			if (auto *particles = store.ResourceMutable<engine::effects::ParticleSystem>()) {
				particles->DeviceStepped = true;
			}
			if (!store.HasResource<engine::effects::RibbonBuffer>()) {
				store.SetResource(engine::effects::RibbonBuffer{});
			}

			// **Registered in both phases, and that is not a duplicate system.**
			// `ResolveAttachments` is a pure recompute of a cache - one multiply
			// per attachment, from state it does not own - so running it twice
			// gives the same answer twice, which is what makes this safe where
			// two *different* systems writing one field would not be.
			//
			// It has a consumer in each phase and they need different things:
			//
			//   - `refresh-emitters`, below, reads `Attachment::WorldFrame` to
			//     place a spawn, and it runs in `PreSimulation`. Resolving only
			//     at `PreRender` would hand it the previous tick's frame, so a
			//     rocket's exhaust would trail its nozzle by a tick.
			//   - `render::CollectLights` reads it to place a lamp, and it runs
			//     at present time. A world that is being *authored* never ticks
			//     at all - `World::Present` runs `PreRender` alone - so
			//     resolving only at `PreSimulation` left every attachment at the
			//     identity and every lamp in the studio lighting the origin.
			//
			// `Attachments.hpp` says this pass runs in `PreRender`; it was
			// registered in `PreSimulation` alone, and the header was the half
			// that was right about the draw path. Both are true and both are
			// declared.
			scheduler.Add("resolve-attachments", Phase::PreSimulation, [](Store &world) {
				(void)engine::scene::ResolveAttachments(world);
			});
			scheduler.Add(
				"refresh-emitters",
				Phase::PreSimulation,
				[](Store &world) { (void)engine::effects::RefreshEmitters(world); },
				SystemOrder{{}, {"resolve-attachments"}}
			);
			scheduler.Add("step-particles", Phase::Simulation, [](Store &world) {
				(void)engine::effects::StepParticles(world, static_cast<float>(world.Time().Delta));
			});
			scheduler.Add(
				"record-trails",
				Phase::Simulation,
				[](Store &world) {
					(void)engine::effects::RecordTrails(world, static_cast<float>(world.Time().Delta));
				},
				SystemOrder{{}, {"step-particles"}}
			);
			// The `PreRender` half of the pair above. `build-ribbons` depends on
			// it, so everything the host reads after `Present` -
			// `render::CollectLights`, `CollectParticleBatches` - see a frame
			// resolved against the transforms this frame is being drawn with.
			scheduler.Add("resolve-attachments", Phase::PreRender, [](Store &world) {
				(void)engine::scene::ResolveAttachments(world);
			});
			scheduler.Add(
				"build-ribbons",
				Phase::PreRender,
				[](Store &world) {
					const auto *active = world.Resource<ActiveCamera>();
					const engine::scene::Transform *eye =
						active == nullptr ? nullptr : world.Get<engine::scene::Transform>(active->Entity);
					(void)engine::effects::BuildRibbons(
						world,
						eye == nullptr ? Vector3::Zero : eye->Frame.Position,
						static_cast<float>(world.Time().Elapsed)
					);
				},
				SystemOrder{{}, {"resolve-attachments"}}
			);
		}

		// The two resources a script writes `UserInputService` through.
		//
		// **Created by the host and never by a script**, because a world with no
		// resource is one where every input query answers "nothing pressed" -
		// which is exactly right for a server and exactly wrong for a client that
		// forgot to install it. Creating it at install time makes the presence of
		// the resource mean "somebody is looking at this world".
		//
		// **Its own function because it has to happen before the scene script's
		// top-level chunk runs, and installing the systems there would be far too
		// early.** `UserInputService.MouseBehavior`, `MouseIconEnabled` and
		// `MouseDeltaSensitivity` are writes onto these two resources, and
		// `script::UserInputService` drops a write onto a world that has neither -
		// deliberately, so that a server does not mint a window's state. Until
		// v0.19 they were minted by `InstallControls`, which runs *after*
		// `LoadScene`, so a `--script` scene that locked the pointer at the top
		// level had the write silently dropped and only a write from inside a
		// `Heartbeat` took.
		//
		// Idempotent, and every caller relies on that: `InstallControls` calls it
		// again on a world that has already been through here, and a second
		// `SetResource` would throw away the pointer mode the script just set.
		void InstallInputResources(Store &store) {
			if (!store.HasResource<engine::scene::InputState>()) {
				store.SetResource(engine::scene::InputState{});
			}
			if (!store.HasResource<engine::scene::ControllerState>()) {
				store.SetResource(engine::scene::ControllerState{});
			}
			if (!store.HasResource<engine::scene::CameraController>()) {
				store.SetResource(engine::scene::CameraController{});
			}
		}

		// The camera and character systems, installed together.
		//
		// **The ground check is the client's rather than `scene`'s**, and that is
		// the tier doing its job: `scene` may not link `physics`, so
		// `StepCharacters` reads `Humanoid::Grounded` and this is what writes it.
		// The same split `replication::DistancePriority::Blocked` already has -
		// the arithmetic there, the query here.
		void InstallControls(Store &store, Scheduler &scheduler) {
			InstallInputResources(store);

			// **Camera control in `PreRender` and character control in
			// `Simulation`**, which is not an inconsistency. A camera is
			// presentation - it should turn at frame rate, because a mouse moves
			// at frame rate and a camera locked to the tick judders. A character
			// moves a body the physics step integrates, so it has to be on the
			// tick or two players at different frame rates would move at different
			// speeds.
			scheduler.Add("camera-control", Phase::PreRender, [](Store &world) {
				(void)engine::scene::UpdateCameraControl(world);

				// **Between the two, and that is the whole reason it is not a
				// separate scheduler entry.** It has to run after
				// `UpdateCameraControl` has settled the player's own distance
				// for this frame and before `PlaceCamera` reads
				// `CameraController::OccludedDistance` - two scheduler
				// entries in one phase have no ordering promise, and a lambda
				// does.
				(void)engine::physics::UpdatePoppercam(world);

				(void)engine::scene::PlaceCamera(world);
			});

			scheduler.Add("character-control", Phase::PreSimulation, [](Store &world) {
				(void)engine::scene::UpdateCharacterControl(world);
				if (auto *input = world.ResourceMutable<engine::scene::InputState>(); input != nullptr) {
					input->ConsumeKeyTaps();
				}
				if (auto *controllers = world.ResourceMutable<engine::scene::ControllerState>();
					controllers != nullptr) {
					controllers->ConsumeTaps();
				}
			});

			// **The other three are `physics`', because grounding needs a
			// query.** They used to be a static `GroundCharacters` in this file
			// plus two lambdas beside it, which meant a dedicated server hosting
			// the same world had no grounding at all and its characters could
			// never jump. `physics::RegisterCharacterSystems` is the one
			// installation, and a client, a server and the studio share it.
			engine::physics::RegisterCharacterSystems(scheduler);
		}

		// How many particle rows a world starts with and may grow to.
		//
		// **The client starts empty because its particle simulation is device
		// stepped.** Preallocating the old 524,288-row host pool cost 44 MiB per
		// world, then the first tick released both arrays without reading them.
		// `GrowFor` still raises the logical pool geometrically as emitters claim
		// blocks, so a quiet world costs no particle storage while the roadmap's
		// half-million-particle target remains below the same hard ceiling.
		//
		// **Measured before it was raised**: at 250,000 the stress scene's grid
		// starved after about 41,000 of its 102,400 emitters, and the symptom was
		// an effect that simply was not there rather than an error -
		// `ParticleStatistics::EmittersRefused` is the number that says so.
		constexpr uint32_t DEFAULT_PARTICLE_POOL = 0;
		constexpr uint32_t MAXIMUM_PARTICLE_POOL = 1048576;
	}

	bool BuildScriptedWorld(
		Store &store,
		Scheduler &scheduler,
		const std::string &path,
		uint32_t reserve,
		std::shared_ptr<engine::script::Runtime> *runtime
	) {
		// Before anything mints an automatic id for `DrawList`. See
		// `RegisterClientComponents`: `Components::Of<T>` caches its answer per
		// type per process, so an explicit registration that arrives second
		// aborts rather than quietly leaving two names for one thing.
		RegisterClientComponents();

		// **Before the script runs, not after.** `InstallEffects` below registers
		// the same classes and is too late: the script is what calls
		// `Instance.new("ParticleEmitter")`, and a class table that gains the name
		// afterwards is a scene whose emitters all failed to resolve.
		engine::effects::RegisterEffectClasses();

		// Register built-in metadata before the script can query it.
		RecordBuiltinMeshes(store);

		// **The viewer exists before the script and receives the completed template
		// after it.** A client compiles only `PlayerGui`; drawing `StarterGui`
		// directly would expose an editing template in a shipped game and would
		// render every player's interface together. The example loader installs
		// these same registries and services idempotently, but it also starts the
		// script, which is too late for a top-level `Players.LocalPlayer` read.
		const Entity localPlayer = EnsureLocalPlayer(store);
		if (localPlayer == engine::ecs::NULL_ENTITY) {
			ENGINE_ERROR("could not establish the single-player client");
			return false;
		}

		// **Before the script runs, for `RegisterEffectClasses`' reason with a
		// quieter failure.** A top-level `UserInputService.MouseBehavior` is a
		// write onto `scene::InputState`, and a write onto a world that has none
		// is dropped rather than refused - so the scene simply did not lock the
		// pointer and nothing said why. `InstallInputResources` states the whole
		// argument.
		InstallInputResources(store);

		// The scene, the components and the systems that move it are the
		// engine's and every program's. What follows is the client's half.
		std::string error;
		if (!engine::examples::LoadScene(store, scheduler, path, error, runtime)) {
			ENGINE_ERROR("script '{}' failed:\n{}", path, error);
			return false;
		}

		// A standalone scripted client is an authority, not a passive replica.
		// The studio and game-file paths already furnish their worlds with these
		// systems, but `--script` stopped at presentation. A Humanoid could hold a
		// non-zero MoveDirection for ever while no integrator consumed the Motion
		// it produced. Anchored examples still cost no body work because they have
		// no Simulated rows.
		engine::physics::PreparePhysicsWorld(store);
		engine::physics::RegisterPhysicsSystems(scheduler);
		engine::scene::PrepareGravity(store);
		engine::scene::RegisterGravitySystem(scheduler);
		engine::scene::RegisterOwnershipSystem(scheduler);

		// The script has now finished authoring `StarterGui`. Clone that completed
		// template rather than the empty service that existed before it ran. An
		// interactive local script may instead author directly into `PlayerGui`;
		// resetting an empty template would delete that live interface and replace
		// it with nothing.
		bool hasTemplate = false;
		const Entity starterGui = store.FindFirstRoot(engine::gui::STARTER_GUI);
		if (starterGui != engine::ecs::NULL_ENTITY) {
			store.EachChild(starterGui, [&hasTemplate](Entity) { hasTemplate = true; });
		}
		if (hasTemplate) {
			(void)engine::gui::ResetPlayerGui(store, localPlayer);
		}

		const float extent = store.Resource<WorldBounds>()->HalfExtent;

		// **A scene that placed its own camera keeps it.** `MoveCamera` is this
		// client's placeholder - it orbits whatever `ActiveCamera` names so that
		// a scene with no camera of its own is still looked at from somewhere -
		// and running it beside a script that aimed one is two things writing
		// one `Transform`, the second winning silently every tick.
		//
		// That is not hypothetical: `Mirrors-1-world.luau` computes its
		// reflection camera from where the eye stands, so an orbiting eye makes
		// the reflection correct for a position the viewer is no longer at. The
		// mirror looked broken and the camera was the reason.
		const auto *existing = store.Resource<ActiveCamera>();
		const bool scripted = existing != nullptr && existing->Entity != engine::ecs::NULL_ENTITY &&
							  store.Alive(existing->Entity);

		const Entity camera = scripted ? existing->Entity : InstallCamera(store);
		InstallResources(store, camera, extent, std::max<uint32_t>(reserve, 1));

		if (!scripted) {
			store.SetResource(
				FallbackCameraState{
					engine::scene::FindSpawn(store).Position != Vector3::Zero,
				}
			);
			scheduler.Add("move-camera", Phase::Simulation, MoveCamera);
		}

		// `PreRender`, ahead of the pass that reads it - `InstallPresentation`
		// carries the argument, and this is the same three lines.
		scheduler.Add("resolve-materials", Phase::PreRender, [](Store &world) {
			(void)engine::scene::ResolveMaterials(world);
		});
		scheduler.Add("sync-rendered", Phase::PreRender, SyncVisibility);
		scheduler.Add(
			"aim-surface-cameras", Phase::PreRender, AimSurfaces, SystemOrder{{}, {"sync-rendered"}}
		);

		// **Before the collection, and it was not.** Everything these two
		// install in `PreRender` produces what the draw list is built *from*:
		// `character.pose` puts a rig's limbs on its root, `resolve-attachments`
		// puts an emitter where its part is, `camera-control` settles the eye
		// the surfaces were just aimed at. A phase runs its systems in the order
		// they were added, so with these registered after `collect-instances`
		// every one of them was published a frame late - a character's limbs
		// drawn where the root was last frame, which is the judder that reads as
		// a rig lagging its own body. The collection dependencies below now
		// state that ordering directly.
		//
		// Three comments already asserted this order and none of them was true:
		// the `PreRender` half of `resolve-attachments` says "first in this
		// phase", `physics::RegisterCharacterSystems` says the pose runs before
		// the draw list is built, and `camera-control` says `PlaceCamera` sees
		// this frame's distance.
		InstallEffects(store, scheduler, DEFAULT_PARTICLE_POOL, MAXIMUM_PARTICLE_POOL);
		InstallControls(store, scheduler);
		scheduler.Add("advance-animation-tracks", Phase::Simulation, [](Store &world) {
			(void)engine::scene::AdvanceAnimationTracks(world);
		});
		scheduler.Add("evaluate-animations", Phase::PreRender, [](Store &world) {
			(void)engine::render::EvaluateAnimations(world);
		});
		scheduler.Add(
			"resolve-bones",
			Phase::PreRender,
			[](Store &world) { (void)engine::scene::ResolveBones(world); },
			SystemOrder{{}, {"evaluate-animations"}}
		);

		scheduler.Add(
			"collect-instances",
			Phase::PreRender,
			engine::render::CollectInstances,
			SystemOrder{{}, {"resolve-materials", "aim-surface-cameras", "build-ribbons", "resolve-bones"}}
		);
		return true;
	}

	Entity EnsureLocalPlayer(Store &store) {
		engine::scene::RegisterSceneClasses();
		engine::gui::RegisterGuiClasses();
		(void)engine::scene::InstallServices(store);
		(void)engine::gui::InstallGuiServices(store);

		if (const auto *local = store.Resource<engine::scene::LocalPlayer>();
			local != nullptr && local->Instance != engine::ecs::NULL_ENTITY && store.Alive(local->Instance)) {
			return local->Instance;
		}

		return engine::scene::AddPlayer(store, "Player", true, 1);
	}

	bool InstallDefaultCamera(Store &store, Scheduler &scheduler) {
		RegisterClientComponents();

		const auto *existing = store.Resource<ActiveCamera>();
		if (existing != nullptr && existing->Entity != engine::ecs::NULL_ENTITY &&
			store.Alive(existing->Entity)) {
			return false;
		}

		ActiveCamera live;
		live.Entity = InstallCamera(store);
		store.SetResource(live);
		store.SetResource(
			FallbackCameraState{
				engine::scene::FindSpawn(store).Position != Vector3::Zero,
			}
		);

		scheduler.Add("move-camera", Phase::Simulation, MoveCamera);
		return true;
	}

	engine::core::Name InstallRenderingProfiles(
		const engine::graph::PipelineSet &profiles,
		engine::render::Renderer &renderer,
		uint64_t world,
		engine::core::Name selected
	) {
		return engine::render::InstallWorldPipeline(profiles, renderer, world, selected);
	}

	void RegisterClientComponents() {
		// **The simulation's components, registered here because this program
		// was the only one not doing it.** `mono.server` calls this from
		// `Simulation.cpp` and `mono.studio` from `Editor.cpp`; the client
		// relied on `physics::Prepare` reaching `RegisterPhysicsComponents` the
		// first time a world was given physics, which happens *during the run*
		// rather than at start-up.
		//
		// That was invisible until the component table started being sealed:
		// registering a type mid-run takes an id decided by whichever world got
		// there first, which is the nondeterminism `Components::Seal` exists to
		// refuse. The same paragraph below about `DrawList` is this failure at
		// v0.7, one component earlier.
		engine::physics::RegisterPhysicsComponents();

		// **A `DrawList` is derived state, and its serialisation says so by
		// writing nothing.**
		//
		// It had no registration at all before v0.7, which meant
		// `Store::SetResource` minted one under the compiler's spelling of the
		// type - rule 4's exact failure, sitting unnoticed because nothing had
		// ever tried to snapshot a world that had one. The studio's Stop does:
		// it saves the universe when Play is pressed and restores it when Stop
		// is, and `Store::Save` refuses a resource with no serialisation rather
		// than writing bytes that cannot be read back. That refusal is correct
		// and this is the fix for it.
		//
		// Nothing is written and nothing is read because the list is rebuilt by
		// `collect-instances` in `PreRender`, every frame, before anything
		// looks at it. Writing a frame's worth of interpolated cubes into every
		// save file would be storing an answer that is recomputed before it is
		// ever used.
		engine::render::RegisterPresentationComponents();
		engine::ecs::Components::Register<FallbackCameraState>("client.FallbackCameraState");
	}

	void InstallPresentation(Store &store, Scheduler &scheduler, uint32_t reserve) {
		RegisterClientComponents();

		// Game-file and studio worlds need the same built-in metadata.
		RecordBuiltinMeshes(store);

		if (!store.HasResource<DrawList>()) {
			store.SetResource(DrawList{});
			store.ResourceMutable<DrawList>()->Instances.reserve(reserve);
		}

		if (!store.HasResource<WorldBounds>()) {
			// A default rather than nothing. `WorldBounds` is what the
			// replication wire quantises against and what a camera would frame,
			// and a world opened in an editor has authored no such number - so
			// it gets the type's own default instead of a missing resource
			// somebody later reads through a null pointer.
			store.SetResource(WorldBounds{});
		}

		// **The same system `engine::examples` installs, from the same place.**
		// It moved into `scene` at v0.7 precisely so this call site could exist:
		// two copies of a system that writes `PreviousTransform` can both be
		// installed into one world, and the second wins silently every tick.
		scheduler.Add("capture-previous", Phase::PreSimulation, engine::scene::CapturePreviousTransforms);

		// **The other half of the same system, and it runs at the other end of
		// the frame.** `capture-previous` records where a body was when the tick
		// began; this cancels that record for anything that has been through a
		// hole since the last frame was drawn, because blending across a
		// teleport is a body streaking across the world. It has to be at
		// `PreRender` rather than beside its partner: what it reads is a serial
		// that arrives with a replication delta, and a delta lands after the
		// tick's `PreSimulation` has already run.
		//
		// **On every world this program presents, replica or not.** The
		// authority takes its own counter inside `CrossPortals`, so this is an
		// integer compare that finds nothing there - and a client, which never
		// runs `CrossPortals` at all, is exactly the case that needs it.
		scheduler.Add("snap-portal-transit", Phase::PreRender, [](Store &world) {
			(void)engine::scene::SnapPortalTransit(world);
		});

		// **Who a teleport brings in, and it must not depend on scripts.** A
		// destination is chosen by a script in *another* world, so a world can be
		// somebody's destination without containing a line of code - and
		// admitting used to happen inside the Luau runtime's own delivery pump.
		// A world with no runtime took the payload into its inbox and left it
		// there: destroyed in the world you left, never built in the world you
		// went to. `script::RegisterTeleportAdmission` carries the argument.
		engine::script::RegisterTeleportAdmission(scheduler);

		// **`PreRender`, ahead of everything that reads a `SurfaceAppearance`.**
		// A `Material` instance names an asset and the part it hangs off is what
		// the draw-list pass reads, so resolving after collection would draw last
		// frame's texture for a frame every time somebody changed one.
		//
		// **It was `PreSimulation`, and that made it do nothing at all in an
		// edited world.** `World::Present` runs `PreRender` alone and the studio
		// never ticks while somebody is authoring, so a `Material` dropped onto
		// a part changed the part's appearance only after Play was pressed -
		// which reads as the material not being loaded rather than as a pass
		// that did not run. The same mistake put every part at the origin
		// through `PreviousTransform`; `studio::PresentationAlpha` is that one.
		//
		// Nothing in the tick reads a `SurfaceAppearance`, so the phase this
		// belongs in is the one its only consumer runs in. It now costs once per
		// *frame* rather than once per tick, which is a walk of the material
		// instances - a handful in a scene, against a draw list of thousands.
		scheduler.Add("resolve-materials", Phase::PreRender, [](Store &world) {
			(void)engine::scene::ResolveMaterials(world);
		});

		scheduler.Add("sync-rendered", Phase::PreRender, SyncVisibility);

		// **And the mirrors, which only `BuildScriptedWorld` was installing.**
		// That is why a mirror worked under `--scene` and was a plain white
		// rectangle everywhere else: the studio, `--game` and an imported world
		// all come through here, and none of them was aiming anything.
		//
		// The visible half of that failure is not the camera at all - it is
		// step 4 of `scene/SurfaceCameras.hpp`. Aiming a camera is also what
		// writes `Visual::Surface` on the pane it is parented to, so without
		// this system the pane keeps the component's default of -1, samples no
		// texture, and draws as its own flat `Tint`. `Mirrors-1-world.luau`
		// tints its pane white, so the symptom was a white part beside a frame
		// that was rendering perfectly - which reads as a broken surface pass
		// rather than as a missing system.
		//
		// **Between the two, not beside them.** `sync-rendered` decides what is
		// drawn at all and `collect-instances` reads the `Visual` this writes,
		// so a mirror aimed after collection would publish last frame's answer.
		scheduler.Add(
			"aim-surface-cameras", Phase::PreRender, AimSurfaces, SystemOrder{{}, {"sync-rendered"}}
		);

		// **The same three systems `BuildScriptedWorld` installs, from the same
		// place.** This is the argument `aim-surface-cameras` already makes one
		// line up, arriving again: the studio, `--game` and an imported world all
		// come through here, and a world with emitters and no step is a world
		// whose effects are authored, saved, loaded and then motionless.
		//
		// **And before the collection, for the reason `BuildScriptedWorld`
		// gives at length**: what these install in `PreRender` is what the draw
		// list is built from. The collection dependencies state that order.
		InstallEffects(store, scheduler, DEFAULT_PARTICLE_POOL, MAXIMUM_PARTICLE_POOL);
		InstallControls(store, scheduler);
		scheduler.Add("advance-animation-tracks", Phase::Simulation, [](Store &world) {
			(void)engine::scene::AdvanceAnimationTracks(world);
		});
		scheduler.Add("evaluate-animations", Phase::PreRender, [](Store &world) {
			(void)engine::render::EvaluateAnimations(world);
		});
		scheduler.Add(
			"resolve-bones",
			Phase::PreRender,
			[](Store &world) { (void)engine::scene::ResolveBones(world); },
			SystemOrder{{}, {"evaluate-animations"}}
		);

		scheduler.Add(
			"collect-instances",
			Phase::PreRender,
			engine::render::CollectInstances,
			SystemOrder{{}, {"resolve-materials", "aim-surface-cameras", "build-ribbons", "resolve-bones"}}
		);
	}
}

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/world/Postbox.hpp>

#include <client/Client.hpp>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <limits>

namespace client {
	// Client composes its displayed worlds in slot zero. Slot one is reserved
	// for the staged successor and retires with that replica.
	static constexpr size_t PORTAL_SUCCESSOR_VIEW = 1;
	// Whole-eye replies belong to the viewport lifetime, not its current body replica.
	static constexpr size_t PORTAL_EYE_VIEW = 2;

	void Client::DropPortalObservation() {
		if (!PortalPrevious) return;
		if (PortalDrawing == &PortalPrevious->View) PortalDrawing = nullptr;
		const auto world = PortalPrevious->World;
		PortalPrevious.reset();
		DropPortalReplica(world);
	}

	void Client::PumpPortalObservation(double nowSeconds) {
		if (!PortalPrevious) return;
		ENGINE_PROFILE("client portal observation");
		auto &observation = *PortalPrevious;
		Universe_->Enter(observation.World, [&](engine::ecs::Store &store) {
			observation.Connection->Poll(store, nowSeconds);
			RecordReplicatedTick(store, observation.Connection->Applied());
		});
		observation.Connection->Advance(nowSeconds);
		if (!observation.Connection->Live() || observation.Connection->Rejected()) DropPortalObservation();
	}

	void Client::RetainPortalObservation(engine::world::WorldId world) {
		using namespace engine;
		DropPortalObservation();
		PortalPrevious = std::make_unique<PortalObservation>();
		auto &observation = *PortalPrevious;
		observation.World = world;
		observation.View.Pipeline = PipelineSelected;
		observation.Socket = std::move(Socket);
		observation.Connection = std::move(Connection);
		observation.Content = std::move(ContentState);
		ContentState = std::make_unique<ContentSession>();
		observation.Connection->SetForeign({});
		observation.Connection->OnUserMessage([this,
											   observed = &observation](std::span<const std::byte> bytes) {
			game::ContentDirectory directory;
			if (game::DecodeContentDirectory(bytes, directory)) {
				AdoptContentDirectory(*observed->Content, directory);
				return;
			}
			if (observed->Content->Relay) (void)observed->Content->Relay->Receive(bytes);
		});
		bool scriptsStopped = false;
		Universe_->Enter(world, [&](ecs::Store &store, ecs::Scheduler &systems) {
			if (const auto *replica = store.Resource<world::Replica>()) observation.Authored = replica->Of;
			// Keep the received presentation clock, but retire the old player's script callbacks.
			const auto stop = [&](std::string_view name) {
				const auto revision = systems.SystemRevision(name);
				return revision != 0 && revision != std::numeric_limits<uint64_t>::max() &&
					   systems.Replace(name, revision + 1, [](ecs::Store &) {});
			};
			const bool stoppedStarts = stop("replica-scripts");
			const bool stoppedHeartbeat = stop("script-heartbeat");
			scriptsStopped = stoppedStarts && stoppedHeartbeat;
			if (auto *input = store.ResourceMutable<scene::InputState>()) *input = {};
			if (auto *controllers = store.ResourceMutable<scene::ControllerState>()) *controllers = {};
			scene::ReleaseCameraCharacterHold(store);
			store.RemoveResource<PortalInputHistory>();
			store.RemoveResource<LocalPlayerPrediction>();
			store.RemoveResource<NativePlayerPrediction>();
		});
		Views.Untrack(world);
		if (!scriptsStopped) {
			ENGINE_WARN("portal observation could not retire gameplay scripts");
			DropPortalObservation();
		}
	}

	bool Client::PreparePortalWorldView(
		PortalWorldView &packet,
		engine::world::WorldId world,
		engine::render::View &view,
		uint32_t width,
		uint32_t height
	) {
		using namespace engine;
		if (!PortalImages || width == 0 || height == 0) return false;
		ENGINE_PROFILE("client observed eye");
		if (PortalNext && PortalNext->World == world && PortalNext->Ready &&
			PortalNext->DrawingArrivedPlayer) {
			std::optional<PortalPredictionContinuation> prediction;
			Universe_->Enter(Replicated, [&](ecs::Store &store) {
				prediction =
					CapturePortalPrediction(store, PortalNext->Offer.Claim, Universe_->AlphaOf(Replicated));
			});
			// The authenticated successor displays the same continued body pose that
			// adoption will take over. Its older snapshot must not shrink the body.
			if (prediction) {
				bool applied = false;
				Universe_->Enter(world, [&](ecs::Store &store) {
					applied = AdoptPortalPrediction(
						store, PortalNext->Player, *prediction, Universe_->AlphaOf(world)
					);
				});
				if (!applied) return false;
			}
		}
		if (!packet.InterfaceReady) {
			const auto backend = Renderer.Backend();
			if (!backend.Device) return false;
			packet.InterfaceReady = packet.Interface.Initialise(backend.Device, backend.ColourFormat);
			if (!packet.InterfaceReady) return false;
			packet.Interface.SetImageSource([this, captured = &packet, world](const core::Name &name) {
				const auto owner = Universe_->NameOf(world);
				render::InterfaceImage image;
				image.Texture = Renderer.TextureHandle(name, owner);
				image.Cell = Renderer.TextureCell(name, captured->Frame.Seconds, owner);
				Renderer.TextureSize(name, image.Width, image.Height, owner);
				return image;
			});
		}
		if (Universe_->Present(world, ParticleDeltaSeconds, Universe_->AlphaOf(world)) !=
			world::WorldStatus::Ok)
			return false;
		const core::Vector2 extent{float(width), float(height)};
		uint64_t nativeEyeRig = 0;
		uint64_t storeIdentity = 0;
		const auto entered = Universe_->Enter(world, [&](ecs::Store &store) {
			const auto owner = Universe_->NameOf(world);
			storeIdentity = store.Identity();
			const auto images =
				Settings.EnableEditableImages ? EditableImages.Refresh(store, Renderer, owner) : 0;
			const auto meshes =
				Settings.EnableEditableMeshes ? EditableMeshes.Refresh(store, Renderer, owner) : 0;
			const bool shaders = render::PrepareWorldShaders(
				store, owner, Shaders, Renderer, &packet.Interface, Settings.EnablePostProcessing
			);
			VisualResourcesChanged = images > 0 || meshes > 0 || shaders || VisualResourcesChanged;
			render::CollectWorldView(store, Universe_->NameOf(world), packet.Frame);
			render::View nativeEye;
			nativeEye.EyePlayer = view.EyePlayer;
			render::ResolveEyeBody(store, nativeEye);
			nativeEyeRig = nativeEye.EyeRig;
			render::CollectWorldCamera(store, view, extent, packet.Camera);
			render::CollectSurfaceViews(
				store, packet.Surfaces, packet.Frame.Portals, &view, packet.Frame.Slots
			);
			packet.SurfaceBounces = Settings.SurfaceBounces > 0
										? Settings.SurfaceBounces
										: uint32_t(std::max(scene::SurfaceBouncesOf(store), 0));
			packet.SurfaceLimit = uint32_t(std::max(scene::SurfaceLimitOf(store), 0));
		});
		if (entered != world::WorldStatus::Ok) return false;
		auto &frame = packet.Frame;
		const size_t nativeRows = frame.Instances.size();
		AppendForeignPortalClones(*Universe_, world, frame.Instances, &frame.Joints);
		packet.HiddenBodyRows.clear();
		if (view.EyeRig != 0) {
			for (size_t index = nativeRows; index < frame.Instances.size(); ++index) {
				const auto &row = frame.Instances[index];
				if (row.Rig == view.EyeRig && row.SourceWorld == view.WorldName)
					packet.HiddenBodyRows.push_back(uint32_t(index));
			}
		}
		view.EyeHiddenRows = packet.HiddenBodyRows;
		view.EyeRig = nativeEyeRig;
		if (!render::BindWorldView(
				frame,
				packet.Camera,
				{.World = world.Index,
				 .Name = Universe_->NameOf(world),
				 .Identity = storeIdentity,
				 .ContentOwner = frame.Name,
				 .ForeignContentOwners = ContentBindings,
				 .Pipeline = packet.Pipeline},
				view
			))
			return false;
		if (!Settings.EnableParticles) view.Particles = {};
		PortalImages->SetContentOwner(world, frame.Name, ContentBindings);
		const bool imagesReady = UpdatePortalImages(
			*Universe_,
			*PortalImages,
			world,
			view,
			{.Width = width, .Height = height, .ComposePlayerBody = true},
			frame.Portals,
			packet.Surfaces,
			Universe_->AlphaOf(world),
			std::chrono::steady_clock::now(),
			Rendered,
			Replicated
		);
		// Keep the completed eye until every visible destination can supply its room.
		if (!imagesReady) return false;
		view.Portals = frame.Portals;
		view.Surfaces = packet.Surfaces;
		packet.Interface.SetContentOwner(Universe_->NameOf(world));
		packet.Interface.Submit(packet.Camera, extent, extent);
		PortalDrawing = &packet;
		return true;
	}

	bool Client::WaitingForPortalViews() const {
		return Windowed && ReportedJoin && !PresentationLink && !InitialPortalViewsReady;
	}

	bool Client::PreparePortalEye(
		engine::render::View &view,
		engine::world::WorldId inputWorld,
		uint32_t width,
		uint32_t height,
		bool prepareNative
	) {
		using namespace engine;
		ENGINE_PROFILE("client portal eye");
		bool retainedCharacter = false;
		Universe_->Enter(inputWorld, [&](ecs::Store &store) {
			render::SelectFirstPersonBody(store, view);
			const auto *held = store.Resource<scene::CameraCharacterHold>();
			retainedCharacter = held && held->Active;
		});
		if (!ReportedJoin || !PortalImages) return false;
		bool tracked = Windowed;
		if (!tracked)
			Universe_->Enter(inputWorld, [&](ecs::Store &store) {
				const auto *active = store.Resource<scene::ActiveCamera>();
				if (!active) return;
				const auto *history = store.Get<scene::CameraPortalView>(active->Entity);
				tracked = history && history->Started;
			});
		if (!tracked) return false;
		const auto now = std::chrono::steady_clock::now();
		const auto selected = ResolveCameraPortalWorld(
			*Universe_,
			inputWorld,
			inputWorld,
			view.CameraFrame,
			view.Camera,
			PortalImages.get(),
			now,
			Rendered
		);
		core::Name eyeWorld = Universe_->NameOf(selected);
		if (selected.IsValid() && !Universe_->IsRemote(selected))
			Universe_->Enter(selected, [&](ecs::Store &store) {
				if (const auto *replica = store.Resource<world::Replica>(); replica && replica->Of.IsValid())
					eyeWorld = replica->Of;
			});
		if (prepareNative && PortalPrevious && selected.IsValid() && eyeWorld.IsValid() &&
			eyeWorld != PortalPrevious->Authored) {
			std::vector<scene::PortalSeam> visibleSeams;
			if (Universe_->IsRemote(selected)) {
				if (const auto *topology = PortalImages->Topology(selected, now))
					visibleSeams = topology->Seams;
			} else {
				Universe_->Enter(selected, [&](ecs::Store &store) {
					scene::GatherPortalSeams(store, visibleSeams);
				});
			}
			const bool visible = std::any_of(visibleSeams.begin(), visibleSeams.end(), [&](const auto &seam) {
				if (!seam.Crosses || seam.DestinationWorld != PortalPrevious->Authored) return false;
				render::PortalImageDemand demand;
				return render::BuildPortalImageDemand(
						   seam,
						   core::Name("observed-world-visibility"),
						   view,
						   view.Slot,
						   {.Width = width, .Height = height},
						   demand
					   ) == render::PortalDemandStatus::Ready;
			});
			if (!visible) DropPortalObservation();
		}
		if (!prepareNative) {
			if (selected == inputWorld && !retainedCharacter) return false;
			// Successor readiness must not submit its staged camera into the displayed eye's slots.
			view.EyeImage = 0;
			for (size_t slot = 0; slot < std::size(PortalEyeDestinations); ++slot) {
				if (PortalEyeDestinations[slot] != eyeWorld) continue;
				view.EyeImage = PortalImages->Image(PORTAL_EYE_VIEW + slot, core::Name("viewport-eye"));
				break;
			}
			return true;
		}
		bool nativeImagesReady = true;
		if (prepareNative && Windowed && selected == inputWorld) {
			auto sourceView = view;
			sourceView.Instances = Views.Instances();
			sourceView.JointFrames = Views.JointFrames();
			world::WorldId admitted;
			if (PortalPrevious &&
				Universe_->Present(
					PortalPrevious->World, ParticleDeltaSeconds, Universe_->AlphaOf(PortalPrevious->World)
				) == world::WorldStatus::Ok)
				admitted = PortalPrevious->World;
			nativeImagesReady = UpdatePortalImages(
				*Universe_,
				*PortalImages,
				inputWorld,
				sourceView,
				{.Width = width, .Height = height, .ComposePlayerBody = true},
				Portals,
				Surfaces,
				Universe_->AlphaOf(inputWorld),
				now,
				Rendered,
				admitted
			);
			view.Portals = Portals;
			view.Surfaces = Surfaces;
		}
		bool retainNativeEye = false;
		if (!nativeImagesReady) {
			for (size_t slot = 0; slot < std::size(PortalEyeDestinations); ++slot) {
				if (PortalEyeDestinations[slot] == eyeWorld &&
					PortalImages->Image(PORTAL_EYE_VIEW + slot, core::Name("viewport-eye")) != 0)
					retainNativeEye = true;
			}
		}
		const auto requestEye = [&](core::Name destination, render::View &eye, core::Name retain) {
			eye.EyePlayer = view.EyePlayer;
			size_t slot = 0;
			if (PortalEyeDestinations[1] == destination)
				slot = 1;
			else if (PortalEyeDestinations[0] != destination) {
				if (PortalEyeDestinations[0].IsValid() &&
					(!PortalEyeDestinations[1].IsValid() || PortalEyeDestinations[0] == retain))
					slot = 1;
			}
			PortalEyeDestinations[slot] = destination;
			eye.Slot = PORTAL_EYE_VIEW + slot;
			// Prefer the authenticated producer over a transient local body replica.
			const auto producer = Universe_->Find(destination);
			if (producer.IsValid()) (void)PortalImages->RequestTopology(Rendered, producer, now);
			(void)PortalImages->SubmitEye(
				Rendered,
				{destination, producer},
				eye,
				{.Width = width, .Height = height},
				now,
				{inputWorld, view.Instances, view.JointFrames}
			);
		};
		render::View remote;
		remote.CameraFrame = view.CameraFrame;
		remote.Camera = view.Camera;
		remote.Target = view.Target;
		if (selected != inputWorld || PortalNext || retainedCharacter || retainNativeEye)
			requestEye(eyeWorld, remote, {});

		// Keep one neighbouring eye warm before its plane is crossed. Two slots
		// bound residency independently of portal count and preserve the return view.
		std::vector<scene::PortalSeam> seams;
		// An unresolved route leaves the eye in input coordinates. Keep warming
		// its known neighbour while destination discovery or topology catches up.
		const auto prefetchWorld = selected.IsValid() ? selected : inputWorld;
		if (prefetchWorld.IsValid()) {
			if (Universe_->IsRemote(prefetchWorld)) {
				if (const auto *topology = PortalImages->Topology(prefetchWorld, now))
					seams = topology->Seams;
			} else {
				Universe_->Enter(prefetchWorld, [&](ecs::Store &store) {
					scene::GatherPortalSeams(store, seams);
				});
			}
		}
		const scene::PortalSeam *nearest = nullptr;
		float nearestDistance = 0;
		for (const auto &seam : seams) {
			if (!seam.Crosses || !seam.DestinationWorld.IsValid() || seam.DestinationWorld == eyeWorld)
				continue;
			const float distance = scene::SeamDistance(seam, view.CameraFrame.Position);
			// Limit speculative work to one aperture diameter around the mouth.
			if (distance > 2 * std::max(seam.First.Magnitude(), seam.Second.Magnitude())) continue;
			if (!nearest || distance < nearestDistance) {
				nearest = &seam;
				nearestDistance = distance;
			}
		}
		bool neighbourReady = nearest == nullptr;
		if (nearest) {
			const auto producer = Universe_->Find(nearest->DestinationWorld);
			if (producer.IsValid()) {
				(void)PortalImages->RequestTopology(Rendered, producer, now);
				const auto through = scene::SeamMapping(*nearest);
				render::View neighbour;
				neighbour.CameraFrame = through.Place(view.CameraFrame);
				neighbour.Camera = view.Camera;
				neighbour.Camera.NearPlane *= through.Scale;
				neighbour.Camera.FarPlane *= through.Scale;
				requestEye(nearest->DestinationWorld, neighbour, eyeWorld);
				if (!InitialPortalViewsReady) {
					// Look input may already have started another refresh. Require a usable
					// completed image with this body-hiding profile, not an idle request slot.
					const auto captured = PortalImages->Capture(neighbour.Slot, neighbour.EyeImageKey);
					neighbourReady = captured &&
									 PortalImages->Image(neighbour.Slot, neighbour.EyeImageKey) != 0 &&
									 captured->EyePlayer ==
										 (view.EyePlayer ? std::to_string(*view.EyePlayer) : std::string{});
				}
			}
		}
		(void)PortalImages->Pump(0, 1, now);
		// Start remote captures before this frame's GPU and capture-file waits.
		// Run's final pump still covers connection work on frames without an eye.
		PumpPlayPresentation();
		if (!InitialPortalViewsReady && selected == inputWorld && nativeImagesReady && neighbourReady) {
			InitialPortalViewsReady = true;
			ENGINE_INFO("portal entry: initial views ready");
		}
		// A held subject can use the current native camera once its portal images are ready.
		if (selected == inputWorld && (!retainedCharacter || nativeImagesReady) && !retainNativeEye)
			return false;
		if (prepareNative && PortalPrevious && selected == PortalPrevious->World &&
			PreparePortalWorldView(PortalPrevious->View, PortalPrevious->World, view, width, height))
			return true;
		if (prepareNative && PortalNext && PortalNext->World.IsValid() && !PortalNext->Refused &&
			PortalNext->Failure.empty() && PortalNext->Connection && PortalNext->Connection->Admitted() &&
			PortalNext->Connection->Joined() && PortalNext->Connection->Live() &&
			!PortalNext->Connection->Rejected() && eyeWorld.Text() == PortalNext->Offer.Claim.Destination &&
			PreparePortalWorldView(PortalNext->View, PortalNext->World, view, width, height))
			return true;
		remote.EyeImage = selected.IsValid() ? PortalImages->Image(remote.Slot, remote.EyeImageKey) : 0;
		// The held subject has no visual rows after source retirement. Its source
		// eye producer still sees the destination body through the aperture.
		if (selected == inputWorld && remote.EyeImage == 0) return false;
		view = std::move(remote);
		return true;
	}

	bool Client::PortalSuccessorDrawable() {
		using namespace engine;
		ENGINE_PROFILE("client portal drawable");
		if (!PortalNext || !PortalNext->Camera || !PortalImages) return false;
		auto &next = *PortalNext;
		Universe_->Enter(next.World, [&](ecs::Store &store) {
			(void)AimReplicaViewer(store, next.Camera->Frame, next.Camera->Lens);
		});
		render::View view;
		view.Slot = PORTAL_SUCCESSOR_VIEW;
		view.CameraFrame = next.Camera->Frame;
		view.Camera = next.Camera->Lens;
		const render::PortalImageDemandSettings dimensions{
			.Width = static_cast<uint32_t>(std::max(Settings.Width, 1)),
			.Height = static_cast<uint32_t>(std::max(Settings.Height, 1))
		};
		render::PortalImageDemand demand;
		if (render::BuildPortalEyeDemand(core::Name("viewport-eye"), view, dimensions, demand) !=
			render::PortalDemandStatus::Ready)
			return false;
		const auto now = std::chrono::steady_clock::now();
		// Drawable readiness accepts one completed image. Cancelling on every
		// camera move prevents a remote child capture from ever completing.
		(void)PortalImages->Pump(0, Universe_->AlphaOf(next.World), now);
		if (PortalImages->CurrentImage(view.Slot, core::Name("viewport-eye")) != 0) return true;
		(void)PortalImages->SubmitEye(
			next.World, {core::Name(next.Offer.Claim.Destination), next.World}, view, dimensions, now
		);
		(void)PortalImages->Pump(0, Universe_->AlphaOf(next.World), now);
		return PortalImages->CurrentImage(view.Slot, view.EyeImageKey) != 0;
	}

	bool Client::ReceivePortalSession(const engine::game::PortalSessionMessage &message) {
		using namespace engine;
		if (message.Kind == game::PortalSessionKind::Transfer) {
			if (PortalNext) {
				if (!PortalNext->ProceedSent && message.Attempt > PortalNext->Offer.Attempt &&
					(!PortalNext->Following || message.Attempt > PortalNext->Following->Attempt))
					PortalNext->Following = message;
				return true;
			}
			DropPortalObservation();
			PortalNext = std::make_unique<PortalSuccessor>();
			PortalNext->Offer = message;
			PortalNext->Endpoint = ConnectedServer;
			PortalNext->Endpoint.Port = message.Port;
			PortalNext->Deadline = core::Clock::Seconds() + 15;
			return true;
		}
		if (!PortalNext || message.Attempt != PortalNext->Offer.Attempt) return false;
		if (message.Kind == game::PortalSessionKind::Motion) {
			if (!PortalNext->ProceedSent || PortalNext->Refused || message.Claim != PortalNext->Offer.Claim ||
				!message.Motion || message.Motion->InputTick > SubmittedMoveTick)
				return false;
			const auto &motion = *message.Motion;
			if (!PortalNext->Motion || (motion.DestinationTick > PortalNext->Motion->DestinationTick &&
										motion.InputTick >= PortalNext->Motion->InputTick))
				PortalNext->Motion = motion;
			return true;
		}
		if (message.Kind == game::PortalSessionKind::Crossed && message.Claim == PortalNext->Offer.Claim) {
			PortalNext->Crossed = true;
			return true;
		}
		if (message.Kind == game::PortalSessionKind::Refused && !PortalNext->Crossed) {
			PortalNext->Refused = true;
			PortalNext->Failure = message.Diagnostic;
			return true;
		}
		return false;
	}

	void Client::DropPortalReplica(engine::world::WorldId world) {
		if (!world.IsValid()) return;
		Universe_->Enter(world, [this](engine::ecs::Store &store) {
			EditableImages.ForgetWorld(store.Identity());
			EditableMeshes.ForgetWorld(store.Identity());
			engine::scene::ReleaseCameraCharacterHold(store);
			store.RemoveResource<PortalInputHistory>();
		});
		Views.Untrack(world);
		if (PortalImages) PortalImages->RemoveWorld(world);
		Shaders.DropOwner(Universe_->NameOf(world));
		Interface.DropContentOwner(Universe_->NameOf(world));
		Renderer.DropContentOwner(Universe_->NameOf(world));
		VisualResourcesChanged = true;
		if (auto sound = Stages.find(world.Index); sound != Stages.end() && Sound)
			sound->second.Clear(Sound->Mixer());
		std::erase_if(Runtimes, [world](const auto &runtime) { return runtime.first == world; });
		(void)Universe_->Destroy(world);
	}

	void Client::PumpPortalSuccessor(double nowSeconds, bool presentationReady) {
		using namespace engine;
		if (!PortalNext) return;
		ENGINE_PROFILE("client portal successor");
		auto &next = *PortalNext;
		if (next.Refused) {
			Universe_->Enter(Replicated, [](ecs::Store &store) {
				scene::ReleaseCameraCharacterHold(store);
				store.RemoveResource<PortalInputHistory>();
			});
			ENGINE_WARN("portal transfer refused: {}", next.Failure);
			const auto world = next.World;
			auto following = std::move(next.Following);
			if (PortalDrawing == &PortalNext->View) PortalDrawing = nullptr;
			PortalNext.reset();
			DropPortalReplica(world);
			if (following) (void)ReceivePortalSession(*following);
			return;
		}
		if (!next.ProceedSent && nowSeconds >= next.Deadline && next.Failure.empty())
			next.Failure = "destination connection or snapshot did not become ready";
		if (!next.Failure.empty()) {
			if (next.ProceedSent) {
				ENGINE_WARN("portal successor reconnecting: {}", next.Failure);
				next.Content.reset();
				next.Connection.reset();
				next.Socket.reset();
				const auto previous = next.World;
				next.World = {};
				DropPortalReplica(previous);
				next.ResumeSent = false;
				next.Ready = false;
				next.CommitSent = false;
				next.Committed = false;
				next.DrawingArrivedPlayer = false;
				next.Failure.clear();
				next.ReconnectAt = nowSeconds + .5;
				return;
			}
			// Completion is the source acknowledgement, not transport acceptance.
			// Keep the same attempt until the source confirms cancellation.
			if (nowSeconds < next.CancellationRetryAt) return;
			game::PortalSessionMessage refused;
			refused.Kind = game::PortalSessionKind::Refused;
			refused.Attempt = next.Offer.Attempt;
			refused.Diagnostic = next.Failure;
			if (Connection->SendUser(game::EncodePortalSession(refused), nowSeconds))
				next.CancellationRetryAt = nowSeconds + .5;
			return;
		}
		if (nowSeconds < next.ReconnectAt) return;
		Universe_->Enter(Replicated, [&](ecs::Store &store) {
			(void)BeginPortalInputHistory(store, next.Offer.Claim, next.Offer.Through, SubmittedMoveTick);
			if (next.Motion) (void)ReconcilePortalInputHistory(store, next.Offer.Claim, *next.Motion);
			const auto *player = store.Resource<scene::LocalPlayer>();
			const auto *active = store.Resource<scene::ActiveCamera>();
			if (!player || !active) return;
			const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player->Instance));
			const auto *subject = store.Get<scene::CameraSubject>(active->Entity);
			if (!subject) return;
			const bool bound = rig && (subject->Target == rig->Humanoid || subject->Target == rig->Root);
			// Automatic follow clears a retired subject; explicit selection keeps its
			// dead handle. An explicit null is an author's change and must stay excluded.
			const bool holdsRetired = subject->Target == next.SourceSubject ||
									  (subject->Automatic && subject->Target == ecs::NULL_ENTITY);
			const bool retiring =
				!rig && holdsRetired && next.ProceedSent && next.Camera &&
				active->Entity == next.SourceCamera && next.SourceSubject != ecs::NULL_ENTITY &&
				!store.Alive(next.SourceSubject) && subject->Automatic == next.Camera->Automatic;
			const bool explicitlyCleared =
				next.Camera && !subject->Automatic && subject->Target == ecs::NULL_ENTITY;
			if (!bound && !retiring && !explicitlyCleared) return;
			auto camera = scene::CaptureCameraContinuation(store);
			if (camera && scene::MapCameraContinuation(*camera, next.Offer.Through)) {
				if (bound) {
					next.SourceCamera = active->Entity;
					next.SourceSubject = subject->Target;
				}
				next.Camera = std::move(camera);
			}
		});
		if (!next.World.IsValid()) {
			world::WorldSettings settings;
			settings.Name = core::Name("client.portal." + std::to_string(NextPortalReplica++));
			settings.TickRate = Settings.TickRate;
			next.World = Universe_->Create(settings);
			next.View.Pipeline = PipelineSelected;
			if (!next.World.IsValid()) {
				next.Failure = "destination replica could not be created";
				return;
			}
			Universe_->Enter(next.World, [&](ecs::Store &store, ecs::Scheduler &systems) {
				world::Replica replica;
				replica.Of = core::Name(next.Offer.Claim.Destination);
				store.SetResource(replica);
				store.SetAdoptOnly(true);
				replication::InterpolationSettings interpolation;
				interpolation.TickRate = Settings.TickRate;
				auto runtime = BuildReplicatedWorld(store, systems, interpolation);
				if (runtime) Runtimes.emplace_back(next.World, std::move(runtime));
			});
			next.Socket = net::MakeUdpTransport(0);
			if (!next.Socket || !ClientIdentity) {
				next.Failure = "destination transport could not be opened";
				return;
			}
			replication::ConnectorSettings connecting;
			connecting.Prediction.MaximumPending = PortalInputHistory::CAPACITY;
			connecting.ClientIdentity = &*ClientIdentity;
			connecting.ServerIdentity = next.Offer.Identity;
			connecting.Quic.BytesPerTick = connecting.Session.Link.BytesPerTick;
			next.Connection =
				std::make_unique<replication::Connector>(*next.Socket, next.Endpoint, nowSeconds, connecting);
			next.Content = std::make_unique<ContentSession>();
			next.Content->RelayName = next.Endpoint.Text();
			next.Content->Relay =
				std::make_unique<ContentLink>([connection =
												   next.Connection.get()](std::span<const std::byte> bytes) {
					return connection->SendUser(bytes, core::Clock::Seconds());
				});
			if (!Settings.ContentPublisherKey.empty()) (void)BuildContentClient(*next.Content);
			next.Presentation = std::make_unique<world::PresentationStream>();
			next.PresentationRoutes.reset();
			next.PresentationAnnounced = false;
			const auto destination = next.World;
			next.Connection->OnUserMessage([this, destination](std::span<const std::byte> bytes) {
				if (Replicated == destination) {
					ReceiveServerMessage(bytes);
					return;
				}
				if (!PortalNext || PortalNext->World != destination) return;
				auto &pending = *PortalNext;
				if (world::PresentationStream::Recognizes(bytes)) {
					if (pending.Presentation->Receive(bytes) == world::PresentationStreamReceive::Refused) {
						pending.Failure = "destination presentation stream refused";
						return;
					}
					for (auto &frame : pending.Presentation->Take()) {
						if (frame.Kind != world::PresentationStreamKind::Routes) {
							pending.Failure = "destination sent unsolicited presentation data";
							return;
						}
						pending.PresentationRoutes = std::move(frame.Directory);
					}
					return;
				}
				game::ContentDirectory directory;
				if (game::DecodeContentDirectory(bytes, directory)) {
					AdoptContentDirectory(*pending.Content, directory);
					return;
				}
				if (pending.Content->Relay && pending.Content->Relay->Receive(bytes)) return;
				game::PortalSessionMessage reply;
				if (!game::DecodePortalSession(bytes, reply) || reply.Attempt != pending.Offer.Attempt)
					return;
				if (reply.Kind == game::PortalSessionKind::Refused) {
					if (pending.Crossed) {
						// The body already belongs to the destination. Retry its exact
						// lease instead of attempting a fresh player or restoring source authority.
						pending.ResumeSent = false;
						pending.Ready = false;
						pending.CommitSent = false;
						pending.AdmissionRetryAt = core::Clock::Seconds() + .25;
						return;
					}
					pending.Failure = reply.Diagnostic;
					return;
				}
				if (reply.World != pending.Offer.Claim.Destination) return;
				if (reply.Kind == game::PortalSessionKind::Ready && pending.ResumeSent) {
					if (pending.Player != ecs::NULL_ENTITY && pending.Player != reply.Player) return;
					pending.Player = reply.Player;
					pending.Ready = true;
				}
				if (reply.Kind == game::PortalSessionKind::Committed && pending.CommitSent &&
					reply.Player == pending.Player)
					pending.Committed = true;
			});
		}
		Universe_->Enter(next.World, [&](ecs::Store &store) {
			next.Connection->Poll(store, nowSeconds);
			RecordReplicatedTick(store, next.Connection->Applied());
		});
		next.Connection->Advance(nowSeconds);
		if (!next.Failure.empty()) return;
		if (next.Connection->Admitted() && next.Presentation && next.Presentation->Open()) {
			if (!next.PresentationAnnounced) {
				world::PresentationStreamFrame directory;
				directory.Kind = world::PresentationStreamKind::Directory;
				directory.Directory.Session = Universe_->LocalPresentationDirectory().Session;
				directory.Directory.Revision = 1;
				// Discover authenticated routes without advertising duplicate reply ownership.
				next.PresentationAnnounced =
					next.Presentation->Queue(directory) == world::PresentationStatus::Ok;
			}
			next.Presentation->Flush([&](auto packet) {
				return next.Connection->SendUser(packet, nowSeconds);
			});
		}
		if (next.Connection->Rejected() || !next.Connection->Live()) {
			next.Failure = "destination authenticated connection ended";
			return;
		}
		if (!next.ProceedSent) {
			if (!next.Connection->Admitted() || !next.Connection->Joined() || !next.Camera) return;
			if (!presentationReady || !PortalSuccessorDrawable()) return;
			game::PortalSessionMessage proceed;
			proceed.Kind = game::PortalSessionKind::Proceed;
			proceed.Attempt = next.Offer.Attempt;
			proceed.Claim = next.Offer.Claim;
			next.ProceedSent = Connection->SendUser(game::EncodePortalSession(proceed), nowSeconds);
			return;
		}
		if (!next.Crossed) return;
		if (nowSeconds < next.AdmissionRetryAt) return;
		if (!next.ResumeSent) {
			game::PortalSessionMessage resume;
			resume.Kind = game::PortalSessionKind::Resume;
			resume.Attempt = next.Offer.Attempt;
			resume.Claim = next.Offer.Claim;
			next.ResumeSent = next.Connection->SendUser(game::EncodePortalSession(resume), nowSeconds);
			return;
		}
		if (!next.Ready || !next.Connection->Joined() || !presentationReady) return;
		bool cameraReady = false;
		size_t entities = 0;
		Universe_->Present(next.World, 0, 0);
		Universe_->Enter(next.World, [&](ecs::Store &store) {
			store.SetResource(scene::LocalPlayer{next.Player});
			if (next.Camera) (void)AimReplicaViewer(store, next.Camera->Frame, next.Camera->Lens);
			const auto *active = store.Resource<scene::ActiveCamera>();
			const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, next.Player));
			if (!active || !rig || !next.Camera) return;
			cameraReady = scene::ApplyCameraContinuation(store, active->Entity, rig->Humanoid, *next.Camera);
			store.EachEntity([&](ecs::Entity) { entities++; });
		});
		if (!cameraReady) return;
		if (!next.DrawingArrivedPlayer) {
			PortalImages->RemoveViewport(PORTAL_SUCCESSOR_VIEW);
			next.DrawingArrivedPlayer = true;
		}
		if (!PortalSuccessorDrawable()) return;
		// The body can arrive while a trailing or cleared eye remains in another world.
		// Keep the old presentation until that eye's persistent viewport has an image.
		render::View arrivedEye;
		arrivedEye.CameraFrame = next.Camera->Frame;
		arrivedEye.Camera = next.Camera->Lens;
		if (PreparePortalEye(
				arrivedEye,
				next.World,
				static_cast<uint32_t>(std::max(Settings.Width, 1)),
				static_cast<uint32_t>(std::max(Settings.Height, 1))
			) &&
			arrivedEye.EyeImage == 0)
			return;
		if (!next.CommitSent) {
			game::PortalSessionMessage commit;
			commit.Kind = game::PortalSessionKind::Commit;
			commit.Attempt = next.Offer.Attempt;
			commit.Claim = next.Offer.Claim;
			next.CommitSent = next.Connection->SendUser(game::EncodePortalSession(commit), nowSeconds);
			return;
		}
		if (!next.Committed || !next.PresentationRoutes) return;
		if (PlayRoutesRevision == std::numeric_limits<uint64_t>::max()) {
			next.Failure = "presentation route revision exhausted";
			return;
		}
		// Replace the authenticated directory directly. An empty intermediate directory
		// would retire unchanged producers and their resident images at every handoff.
		auto routes = *next.PresentationRoutes;
		routes.Session = 1;
		routes.Revision = ++PlayRoutesRevision;
		if (PortalImages->AcceptDriverRoutes(routes) != world::PresentationStatus::Ok) {
			next.Failure = "destination presentation routes refused";
			return;
		}
		const auto previous = Replicated;
		std::optional<PortalPredictionContinuation> prediction;
		std::optional<uint64_t> inputEpoch;
		bool adoptedPrediction = false;
		uint64_t localInputEpoch = 0;
		Universe_->Enter(previous, [&](ecs::Store &store) {
			prediction = CapturePortalPrediction(store, next.Offer.Claim, Universe_->AlphaOf(previous));
			inputEpoch = InputTickAt(store.Time().Tick);
		});
		if (!inputEpoch || *inputEpoch < SubmittedMoveTick) {
			next.Failure = "client input clock could not continue";
			return;
		}
		Universe_->Enter(next.World, [&](ecs::Store &store) {
			localInputEpoch = store.Time().Tick;
			if (prediction)
				adoptedPrediction =
					AdoptPortalPrediction(store, next.Player, *prediction, Universe_->AlphaOf(next.World));
		});
		if (adoptedPrediction) {
			next.Connection->UsePoseAcknowledgements();
			if (!next.Connection->ContinueInputs(prediction->Inputs, prediction->CoveredThrough)) {
				next.Failure = "destination input history could not continue";
				return;
			}
		}
		RetainPortalObservation(previous);
		Discovery.reset();
		Socket = std::move(next.Socket);
		Connection = std::move(next.Connection);
		PlayPresentation = std::move(next.Presentation);
		PortalImages->RestartRequests();
		PlayDirectorySession = PlayDirectoryRevision = 0;
		ConnectedServer = next.Endpoint;
		Settings.ConnectAddress = next.Endpoint.Text();
		Replicated = next.World;
		ReportedJoin = true;
		ReportedAdmission = true;
		FreshAdmissionSent = true;
		InputLocalEpoch = localInputEpoch;
		InputSequenceEpoch = *inputEpoch;
		NetworkSampled = false;
		Views.Track(Replicated, Universe_->NameOf(Replicated), entities * 2);
		ContentState = std::move(next.Content);
		ENGINE_INFO("portal session adopted {} as player {}", next.Offer.Claim.Destination, next.Player.Id);
		if (PortalDrawing == &PortalNext->View) PortalDrawing = nullptr;
		PortalNext.reset();
		PortalImages->RemoveViewport(PORTAL_SUCCESSOR_VIEW);
		VisualResourcesChanged = true;
		PresentationInvalidated = true;
	}
}

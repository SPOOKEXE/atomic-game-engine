#include "PortalEyeHandoff.hpp"
#include "PortalReadiness.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/world/Postbox.hpp>

#include <client/Client.hpp>
#include <client/ContentDemand.hpp>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <limits>

namespace client {
	// Client composes its displayed worlds in slot zero. Slot one is reserved
	// for the staged successor and retires with that replica.
	static constexpr size_t PORTAL_SUCCESSOR_VIEW = 1;
	// Whole-eye replies belong to the viewport lifetime, not its current body replica.
	static constexpr size_t PORTAL_EYE_VIEW = 2;
	// The local route that trails a crossing has a different capture owner from the
	// successor readiness image and the displayed view.
	static constexpr size_t PORTAL_TRAILING_VIEW = 4;
	static constexpr size_t PORTAL_OBSERVATION_VIEW = 5;

	std::optional<PortalReadinessReservation> PortalPrewarmReservation(const Options &settings) {
		const uint32_t width = static_cast<uint32_t>(std::max(settings.Width, 1));
		const uint32_t height = static_cast<uint32_t>(std::max(settings.Height, 1));
		return PortalReadinessReservation::ForRgba16fEyes(
			width, height, static_cast<uint64_t>(settings.PortalReadinessAssetUploadBytes)
		);
	}

	void Client::DropPortalObservation() {
		if (!PortalPrevious) return;
		if (PortalDrawing == &PortalPrevious->View) PortalDrawing = nullptr;
		if (PortalImages) PortalImages->RemoveViewport(PORTAL_OBSERVATION_VIEW);
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
		observation.HoldDisplayEye = true;
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
			const auto meshes = Settings.EnableEditableMeshes
									? EditableMeshes.Refresh(store, Renderer, owner)
									: EditableMeshes.RefreshLods(store, Renderer, owner);
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
			{.TopologyOwner = Rendered,
			 .AdmittedDestination = Replicated,
			 .PresentedDestination = ReportedJoin ? Replicated : world::WorldId{}}
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
		bool prepareNative,
		bool honorDisplayHold
	) {
		using namespace engine;
		ENGINE_PROFILE("client portal eye");
		bool retainedCharacter = false;
		Universe_->Enter(inputWorld, [&](ecs::Store &store) {
			render::SelectFirstPersonBody(store, view);
			const auto *held = store.Resource<scene::CameraCharacterHold>();
			retainedCharacter = held && held->Active;
		});
		if ((!ReportedJoin && Settings.CaptureSequence.empty()) || !PortalImages) return false;
		// Captures are presentation output, not a separate camera path. Let the
		// capture sequence exercise the same portal route as the interactive view.
		bool tracked = Windowed || !Settings.CaptureSequence.empty();
		if (!tracked)
			Universe_->Enter(inputWorld, [&](ecs::Store &store) {
				const auto *active = store.Resource<scene::ActiveCamera>();
				if (!active) return;
				const auto *history = store.Get<scene::CameraPortalView>(active->Entity);
				tracked = history && history->Started;
			});
		if (!tracked) return false;
		if (prepareNative && honorDisplayHold && PortalPrevious && PortalPrevious->HoldDisplayEye) {
			auto observedView = view;
			observedView.Slot = PORTAL_OBSERVATION_VIEW;
			if (PreparePortalWorldView(
					PortalPrevious->View, PortalPrevious->World, observedView, width, height
				)) {
				view = std::move(observedView);
				// Input ownership changes between frames. Keep the already drawn source
				// route for this one display draw, then resolve the adopted eye normally.
				PortalPrevious->HoldDisplayEye = false;
				return true;
			}
		}
		const auto now = std::chrono::steady_clock::now();
		auto visibilityFrame = view.VisibilityCameraFrame();
		const auto selected = ResolveCameraPortalWorld(
			*Universe_,
			inputWorld,
			inputWorld,
			visibilityFrame,
			view.Camera,
			PortalImages.get(),
			now,
			Rendered
		);
		view.VisibilityFrame = visibilityFrame;
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
			world::WorldId presentedPrevious;
			if (PortalPrevious &&
				Universe_->Present(
					PortalPrevious->World, ParticleDeltaSeconds, Universe_->AlphaOf(PortalPrevious->World)
				) == world::WorldStatus::Ok) {
				admitted = PortalPrevious->World;
				presentedPrevious = PortalPrevious->World;
			}
			const auto activeDestination = AdmittedPortalCaptureWorld(core::Clock::Seconds());
			if (activeDestination.IsValid() && activeDestination != Rendered) admitted = activeDestination;
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
				{.TopologyOwner = Rendered,
				 .AdmittedDestination = admitted,
				 .PresentedDestination = presentedPrevious}
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
		const auto requestEye =
			[&](core::Name destination, render::View &eye, core::Name retain, bool primary) {
				// A frame whose route did not resolve names no destination. Submitting
				// it would take over an eye slot and remove its source, retiring the
				// request in flight; under enough latency no reply could ever land.
				if (!destination.IsValid()) return;
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
				const bool transferEye = primary && PortalNext && PortalNext->Crossed && PortalNext->Ready &&
										 destination == PortalNext->ArrivedEyeWorld;
				const size_t submitted = PortalImages->SubmitEye(
					Rendered,
					{destination, producer},
					eye,
					{.Width = width, .Height = height},
					now,
					{inputWorld, view.Instances, view.JointFrames},
					transferEye
				);
				if (!Settings.CaptureSequence.empty() && transferEye) {
					auto &probe = PortalNext->CapturedEyeProbe.emplace();
					probe.Submitted = submitted;
					probe.ProducerValid = producer.IsValid();
				}
			};
		render::View remote;
		remote.CameraFrame = view.CameraFrame;
		remote.VisibilityFrame = view.VisibilityFrame;
		remote.Camera = view.Camera;
		remote.Target = view.Target;
		if (selected != inputWorld || PortalNext || retainedCharacter || retainNativeEye)
			requestEye(eyeWorld, remote, {}, true);

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
			const float distance = scene::SeamDistance(seam, view.VisibilityCameraFrame().Position);
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
				neighbour.VisibilityFrame = through.Place(view.VisibilityCameraFrame());
				neighbour.Camera = view.Camera;
				neighbour.Camera.NearPlane *= through.Scale;
				neighbour.Camera.FarPlane *= through.Scale;
				requestEye(nearest->DestinationWorld, neighbour, eyeWorld, false);
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
		if (PortalPrevious && selected == inputWorld && !Universe_->IsRemote(inputWorld)) {
			std::optional<scene::SeamTransform> trailing;
			Universe_->Enter(inputWorld, [&](ecs::Store &store) {
				const auto body = PortalHandoffBody(store);
				if (!body) return;
				std::vector<scene::PortalSeam> paired;
				scene::GatherPortalSeams(store, paired);
				trailing = TrailingPortalEye(
					paired, PortalPrevious->Authored, view.VisibilityCameraFrame().Position, *body
				);
			});
			if (trailing) {
				auto sourceView = view;
				sourceView.Slot = PORTAL_OBSERVATION_VIEW;
				sourceView.CameraFrame = trailing->Place(view.CameraFrame);
				sourceView.VisibilityFrame = trailing->Place(view.VisibilityCameraFrame());
				sourceView.Camera.NearPlane *= trailing->Scale;
				sourceView.Camera.FarPlane *= trailing->Scale;
				// The admitted body is visible in the destination capture through the old pane.
				sourceView.EyePlayer.reset();
				sourceView.EyeRig = 0;
				if (PreparePortalWorldView(
						PortalPrevious->View, PortalPrevious->World, sourceView, width, height
					)) {
					view = std::move(sourceView);
					return true;
				}
			}
		}
		// A held subject can use the current native camera once its portal images are ready.
		if (selected == inputWorld && (!retainedCharacter || nativeImagesReady) && !retainNativeEye)
			return false;
		if (prepareNative && PortalPrevious && selected == PortalPrevious->World) {
			auto observedView = view;
			observedView.Slot = PORTAL_OBSERVATION_VIEW;
			if (PreparePortalWorldView(
					PortalPrevious->View, PortalPrevious->World, observedView, width, height
				)) {
				view = std::move(observedView);
				return true;
			}
		}
		if (prepareNative && PortalNext && selected == Replicated && selected != inputWorld &&
			!Universe_->IsRemote(selected)) {
			if (!PortalNext->LocalTrailingEye) {
				PortalNext->LocalTrailingEye = std::make_unique<PortalWorldView>();
				PortalNext->LocalTrailingEye->Pipeline = PipelineSelected;
			}
			auto trailingView = view;
			trailingView.Slot = PORTAL_TRAILING_VIEW;
			if (PreparePortalWorldView(
					*PortalNext->LocalTrailingEye, selected, trailingView, width, height
				)) {
				view = std::move(trailingView);
				return true;
			}
		}
		if (prepareNative && PortalNext && PortalNext->World.IsValid() && !PortalNext->Refused &&
			PortalNext->Failure.empty() && PortalNext->Connection && PortalNext->Connection->Admitted() &&
			PortalNext->Connection->Joined() && PortalNext->Connection->Live() &&
			!PortalNext->Connection->Rejected() && PortalNext->LivePresentationReady &&
			eyeWorld.Text() == PortalNext->Offer.Claim.Destination &&
			PreparePortalWorldView(PortalNext->View, PortalNext->World, view, width, height))
			return true;
		remote.EyeImage = selected.IsValid() ? PortalImages->Image(remote.Slot, remote.EyeImageKey) : 0;
		if (!Settings.CaptureSequence.empty() && PortalNext && inputWorld == PortalNext->World) {
			PortalSuccessor::EyeProbe probe =
				PortalNext->CapturedEyeProbe.value_or(PortalSuccessor::EyeProbe{});
			probe.SelectedWorld = Universe_->NameOf(selected).Text();
			probe.EyeWorld = eyeWorld.Text();
			probe.ImageKey = remote.EyeImageKey.Text();
			probe.Slot = remote.Slot;
			probe.Image = remote.EyeImage;
			probe.CurrentImage = PortalImages->CurrentImage(remote.Slot, remote.EyeImageKey);
			if (const auto capture = PortalImages->Capture(remote.Slot, remote.EyeImageKey)) {
				probe.CaptureImage = capture->Image;
				probe.CaptureTick = capture->CaptureTick;
			}
			for (size_t slot = 0; slot < probe.ViewportImages.size(); ++slot) {
				probe.ViewportImages[slot] =
					PortalImages->Image(PORTAL_EYE_VIEW + slot, core::Name("viewport-eye"));
				probe.ViewportDestinations[slot] = PortalEyeDestinations[slot].Text();
			}
			PortalNext->CapturedEyeProbe = std::move(probe);
		}
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
		next.ProceedEyeSource = Universe_->NameOf(next.World).Text();
		next.ProceedEyeSourceRemote = Universe_->IsRemote(next.World);
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
		if (PortalImages->Image(view.Slot, core::Name("viewport-eye")) != 0) return true;
		next.ProceedEyeSubmitted = PortalImages->SubmitEye(
			next.World,
			{core::Name(next.Offer.Claim.Destination), next.World},
			view,
			dimensions,
			now,
			{},
			true
		);
		const auto progress = PortalImages->Pump(0, Universe_->AlphaOf(next.World), now);
		next.ProceedProducerRequests = progress.Requests;
		next.ProceedProducerRendered = progress.Rendered;
		next.ProceedProducerRefused = progress.Refused;
		next.ProceedEyeImage = PortalImages->Image(view.Slot, view.EyeImageKey);
		if (const auto capture = PortalImages->Capture(view.Slot, view.EyeImageKey))
			next.ProceedEyeCapture = capture->Image;
		return next.ProceedEyeImage != 0;
	}

	bool Client::ReceivePortalSession(const engine::game::PortalSessionMessage &message) {
		using namespace engine;
		if (message.Kind == game::PortalSessionKind::Approach) {
			if (message.Port == 0 || PortalNext) return false;
			if (PortalApproach && PortalApproach->Route.Seam == message.Seam &&
				PortalApproach->Route.Destination == message.Destination) {
				if (PortalApproach->Route.Identity == message.Identity &&
					PortalApproach->Route.Port == message.Port) {
					PortalApproach->Route = message;
					PortalApproach->Deadline = core::Clock::Seconds() + 20.0;
					return true;
				}
			}
			DropPortalApproach();
			PortalApproach = std::make_unique<PortalApproachReplica>();
			PortalApproach->Route = message;
			PortalApproach->Endpoint = ConnectedServer;
			PortalApproach->Endpoint.Port = message.Port;
			PortalApproach->Deadline = core::Clock::Seconds() + 20.0;
			return true;
		}
		if (message.Kind == game::PortalSessionKind::Transfer) {
			if (PortalNext) {
				if (!PortalNext->ProceedSent && message.Attempt > PortalNext->Offer.Attempt &&
					(!PortalNext->Following || message.Attempt > PortalNext->Following->Attempt))
					PortalNext->Following = message;
				return true;
			}
			DropPortalObservation();
			PortalNext = std::make_unique<PortalSuccessor>();
			auto &next = *PortalNext;
			next.Offer = message;
			next.Endpoint = ConnectedServer;
			next.Endpoint.Port = message.Port;
			next.Deadline = core::Clock::Seconds() + 15;
			next.View.Pipeline = PipelineSelected;
			next.Readiness = std::make_shared<PortalReadinessController>(PortalReadinessSettings{
				.EnterDistance = 16,
				.ExitDistance = 20,
				.CapacityBytes = static_cast<uint64_t>(Settings.PortalReadinessBudgetBytes)
			});
			if (PortalApproach && PortalApproach->World.IsValid() && PortalApproach->Connection &&
				PortalApproach->Content && PortalApproachMatchesTransfer(PortalApproach->Route, message)) {
				// The advisory route proved this endpoint before a receipt existed.
				// Rebinding its callback is enough to turn the same replica into the
				// authoritative successor without reopening a socket or reloading content.
				auto warmed = std::move(PortalApproach);
				next.World = warmed->World;
				next.Socket = std::move(warmed->Socket);
				next.Connection = std::move(warmed->Connection);
				next.Content = std::move(warmed->Content);
				next.ReadinessDistance = warmed->LastDistance;
				next.HasReadinessDistance = warmed->HasDistance;
				if (const auto reservation = PortalPrewarmReservation(Settings))
					(void)next.Readiness->Reserve(*reservation);
				next.Presentation = std::make_unique<world::PresentationStream>();
				next.PresentationRoutes.reset();
				next.PresentationAnnounced = false;
				const auto destination = next.World;
				next.Connection->OnUserMessage([this, destination](std::span<const std::byte> bytes) {
					if (!PortalNext || PortalNext->World != destination) return;
					auto &pending = *PortalNext;
					if (world::PresentationStream::Recognizes(bytes)) {
						if (pending.Presentation->Receive(bytes) ==
							world::PresentationStreamReceive::Refused) {
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
						pending.DestinationFence = reply.Fence;
						pending.Ready = true;
					}
					if (reply.Kind == game::PortalSessionKind::Committed && pending.CommitSent &&
						reply.Player == pending.Player)
						pending.Committed = true;
				});
			} else {
				DropPortalApproach();
			}
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
			if (!message.Fence) return false;
			PortalNext->SourceFence = message.Fence;
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

	void Client::DropPortalApproach() {
		if (!PortalApproach) return;
		const auto world = PortalApproach->World;
		PortalApproach.reset();
		DropPortalReplica(world);
	}

	engine::world::WorldId Client::AdmittedPortalCaptureWorld(double nowSeconds) const {
		PortalCaptureCandidate approach;
		if (PortalApproach && PortalApproach->Connection) {
			const auto &connection = *PortalApproach->Connection;
			approach = {
				PortalApproach->World,
				connection.Admitted(),
				connection.Joined(),
				connection.Live(),
				connection.Rejected(),
				nowSeconds >= PortalApproach->Deadline
			};
		}
		PortalCaptureCandidate successor;
		if (PortalNext && PortalNext->Connection) {
			const auto &connection = *PortalNext->Connection;
			successor = {
				PortalNext->World,
				connection.Admitted(),
				connection.Joined(),
				connection.Live(),
				connection.Rejected() || PortalNext->Refused,
				!PortalNext->Failure.empty() || nowSeconds >= PortalNext->Deadline
			};
		}
		return PortalCaptureDestination(Rendered, approach, successor);
	}

	void Client::PumpPortalApproach(double nowSeconds) {
		using namespace engine;
		if (!PortalApproach) return;
		auto &approach = *PortalApproach;
		if (nowSeconds >= approach.Deadline) {
			DropPortalApproach();
			return;
		}
		float distance = std::numeric_limits<float>::infinity();
		Universe_->Enter(Replicated, [&](ecs::Store &store) {
			const auto *player = store.Resource<scene::LocalPlayer>();
			const auto character = player ? scene::CharacterOf(store, player->Instance) : ecs::NULL_ENTITY;
			const auto *placement = store.Get<scene::Transform>(character);
			if (placement) distance = (placement->Frame.Position - approach.Route.Through.Origin).Magnitude();
		});
		const float limit = approach.Active ? 20.0f : 16.0f;
		if (distance > limit) {
			DropPortalApproach();
			return;
		}
		approach.LastDistance = distance;
		approach.HasDistance = true;
		approach.Active = true;
		if (!approach.World.IsValid()) {
			world::WorldSettings settings;
			settings.Name = core::Name("client.portal.approach." + std::to_string(NextPortalReplica++));
			settings.TickRate = Settings.TickRate;
			approach.World = Universe_->Create(settings);
			if (!approach.World.IsValid()) {
				DropPortalApproach();
				return;
			}
			Universe_->Enter(approach.World, [&](ecs::Store &store, ecs::Scheduler &systems) {
				store.SetResource(
					world::Replica{.Active = true, .Of = core::Name(approach.Route.Destination), .View = {}}
				);
				store.SetAdoptOnly(true);
				replication::InterpolationSettings interpolation;
				interpolation.TickRate = Settings.TickRate;
				if (auto runtime = BuildReplicatedWorld(store, systems, interpolation))
					Runtimes.emplace_back(approach.World, std::move(runtime));
			});
			approach.Socket = OpenSocket(1);
			if (!approach.Socket || !ClientIdentity) {
				DropPortalApproach();
				return;
			}
			replication::ConnectorSettings connecting;
			connecting.Prediction.MaximumPending = PortalInputHistory::CAPACITY;
			connecting.ClientIdentity = &*ClientIdentity;
			connecting.ServerIdentity = approach.Route.Identity;
			connecting.Quic.BytesPerTick = connecting.Session.Link.BytesPerTick;
			approach.Connection = std::make_unique<replication::Connector>(
				*approach.Socket, approach.Endpoint, nowSeconds, connecting
			);
			approach.Content = std::make_unique<ContentSession>();
			approach.Content->RelayName = approach.Endpoint.Text();
			approach.Content->Relay = std::make_unique<ContentLink>(
				[connection = approach.Connection.get()](std::span<const std::byte> bytes) {
					return connection->SendUser(bytes, core::Clock::Seconds());
				}
			);
			if (!Settings.ContentPublisherKey.empty()) (void)BuildContentClient(*approach.Content);
			const auto world = approach.World;
			approach.Connection->OnUserMessage([this, world](std::span<const std::byte> bytes) {
				if (!PortalApproach || PortalApproach->World != world) return;
				game::ContentDirectory directory;
				if (game::DecodeContentDirectory(bytes, directory)) {
					AdoptContentDirectory(*PortalApproach->Content, directory);
					return;
				}
				if (PortalApproach->Content->Relay) (void)PortalApproach->Content->Relay->Receive(bytes);
			});
		}
		Universe_->Enter(approach.World, [&](ecs::Store &store) {
			approach.Connection->Poll(store, nowSeconds);
			RecordReplicatedTick(store, approach.Connection->Applied());
		});
		approach.Connection->Advance(nowSeconds);
		// A connector may replace a failed wire during its initial handshake.
		// Its deadline owns that provisional period; only an exhausted exchange is terminal here.
		if (approach.Connection->Rejected()) DropPortalApproach();
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
		Renderer.ForgetWorld(world.Index, Universe_->NameOf(world));
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
		next.PromotionBlocker = nullptr;
		next.ProceedBlocker = nullptr;
		if (next.Refused) {
			Universe_->Enter(Replicated, [](ecs::Store &store) {
				scene::ReleaseCameraCharacterHold(store);
				store.RemoveResource<PortalInputHistory>();
			});
			ENGINE_WARN("portal transfer refused: {}", next.Failure);
			const auto world = next.World;
			auto following = std::move(next.Following);
			if (PortalDrawing == &PortalNext->View ||
				(PortalNext->LocalTrailingEye && PortalDrawing == PortalNext->LocalTrailingEye.get()))
				PortalDrawing = nullptr;
			PortalImages->RemoveViewport(PORTAL_TRAILING_VIEW);
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
				if (next.Readiness) next.Readiness->Release();
				next.SuppressRetainedCapture = false;
				const auto previous = next.World;
				next.World = {};
				PortalImages->RemoveViewport(PORTAL_TRAILING_VIEW);
				DropPortalReplica(previous);
				next.ResumeSent = false;
				next.Ready = false;
				next.CommitSent = false;
				next.Committed = false;
				next.DrawingArrivedPlayer = false;
				next.CapturedReadiness.reset();
				next.CapturedReadinessDecision.reset();
				next.EmptyContentDemandRevision.reset();
				next.UndeliverableAssetNames = 0;
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
			next.Socket = OpenSocket(2);
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
			if (next.Readiness) {
				// Reserve the destination and return images plus content/upload headroom
				// before work begins. Failure remains explicitly image-only.
				if (const auto reservation = PortalPrewarmReservation(Settings))
					(void)next.Readiness->Reserve(*reservation);
			}
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
					pending.DestinationFence = reply.Fence;
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
			if (!next.Connection->Admitted() || !next.Connection->Joined() || !next.Camera) {
				next.ProceedBlocker = "destination-connection-or-camera";
				return;
			}
			if (!presentationReady) {
				next.ProceedBlocker = "presentation";
				return;
			}
			if (!PortalSuccessorDrawable()) {
				next.ProceedBlocker = "successor-eye-image";
				return;
			}
			// Proceed starts authority preparation. It is not permission to replace
			// the retained image, which remains selected until the destination sends
			// a sealed, observed readiness fence in its later Ready response.
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
		if (!next.Readiness) return;
		PortalReadinessEvidence evidence;
		evidence.Distance =
			next.HasReadinessDistance ? next.ReadinessDistance : std::numeric_limits<float>::infinity();
		Universe_->Enter(Replicated, [&](ecs::Store &store) {
			const auto *player = store.Resource<scene::LocalPlayer>();
			const auto character = player ? scene::CharacterOf(store, player->Instance) : ecs::NULL_ENTITY;
			const auto *placement = store.Get<scene::Transform>(character);
			if (placement) {
				next.ReadinessDistance = (placement->Frame.Position - next.Offer.Through.Origin).Magnitude();
				next.HasReadinessDistance = true;
				evidence.Distance = next.ReadinessDistance;
			}
		});
		evidence.RetainedCapture =
			PortalImages->Image(PORTAL_SUCCESSOR_VIEW, core::Name("viewport-eye")) != 0;
		evidence.RetainedCaptureFresh = evidence.RetainedCapture;
		// Both RGBA16F eyes and bounded content/upload headroom must fit before
		// destination geometry can replace the retained image.
		if (const auto reservation = PortalPrewarmReservation(Settings))
			evidence.CapacityReserved = next.Readiness->Reserve(*reservation);
		if (next.Content) {
			// Content delivery retains deferred work, so a clear pair of queues is
			// the only point this replica may promote its already scanned far region.
			evidence.AssetsResident =
				next.Content->Requested && next.Content->Pending.empty() && next.Content->Issued.empty();
			const auto scanned = next.Content->ScannedAtRevision.find(next.World.Index);
			if (scanned != next.Content->ScannedAtRevision.end()) {
				evidence.RequiredAssetRevision = scanned->second;
				evidence.ResidentAssetRevision = evidence.AssetsResident ? scanned->second : 0;
			}
			if (!next.Content->Client) {
				// A game with no content origin can still have an asset-free successor.
				// Prove that from the replicated rows, and repeat only when their asset
				// references change. A named asset without a client remains image-only.
				Universe_->Enter(next.World, [&](ecs::Store &store) {
					const uint64_t revision = WantedContentRevision(store);
					if (next.EmptyContentDemandRevision != revision) {
						std::vector<core::Name> wanted;
						CollectWantedContent(store, wanted);
						next.UndeliverableAssetNames = wanted.size();
						next.EmptyContentDemandRevision = revision;
					}
					(void)ApplyAssetlessReadiness(evidence, revision, next.UndeliverableAssetNames);
				});
			}
		}
		Universe_->Enter(Replicated, [&](ecs::Store &store) {
			const auto *player = store.Resource<scene::LocalPlayer>();
			if (!player) return;
			const auto receipt = script::PortalTransferOfPlayer(store, player->Instance);
			if (!receipt || receipt->Stage < script::PortalTransferStage::Prepared) return;
			const auto &fence = receipt->Fence;
			if (fence.TopologyRevision == 0 || fence.BaselineId == 0 || fence.H.DestinationTick == 0) return;
			evidence.RequiredBaseline = fence.BaselineId;
			evidence.RequiredBaselineHash = fence.BaselineHash;
			// Connector exposes its completed tick, but not the fenced baseline
			// identity, hash, or topology revision. Do not manufacture evidence
			// from that tick: a missing destination receipt remains image-only.
			evidence.RequiredTopologyRevision = fence.TopologyRevision;
			evidence.RequiredAuthorityEpoch = fence.AuthorityEpoch;
			evidence.RequiredPrepareRevision = fence.PrepareRevision;
			evidence.RequiredClockDomain = fence.H.Domain;
			evidence.RequiredSourceTick = fence.H.SourceTick;
			evidence.RequiredDestinationTick = fence.H.DestinationTick;
			evidence.RequiredPoseBegin = fence.H.DestinationTick;
			evidence.RequiredPoseEnd = fence.H.DestinationTick;
		});
		if (next.SourceFence) {
			const auto &fence = *next.SourceFence;
			evidence.RequiredBaseline = fence.BaselineId;
			evidence.RequiredBaselineHash = fence.BaselineHash;
			evidence.RequiredTopologyRevision = fence.TopologyRevision;
			evidence.RequiredAuthorityEpoch = fence.AuthorityEpoch;
			evidence.RequiredPrepareRevision = fence.PrepareRevision;
			evidence.RequiredClockDomain = fence.H.Domain;
			evidence.RequiredSourceTick = fence.H.SourceTick;
			evidence.RequiredDestinationTick = fence.H.DestinationTick;
			evidence.RequiredPoseBegin = fence.H.DestinationTick;
			evidence.RequiredPoseEnd = fence.H.DestinationTick;
		}
		std::optional<script::PortalTransferFence> localFence;
		Universe_->Enter(next.World, [&](ecs::Store &store) {
			localFence = script::PortalTransferDestinationFence(store, next.Offer.Claim.Transfer);
		});
		// Crossed and Ready arrive on authenticated source and destination channels.
		// If replication also carries the receipt, it must agree with both hosts.
		if (next.SourceFence && next.DestinationFence && *next.SourceFence == *next.DestinationFence &&
			(!localFence || *localFence == *next.DestinationFence)) {
			const auto &observed = *next.DestinationFence;
			evidence.ReplicaBaseline = observed.BaselineId;
			evidence.ReplicaBaselineHash = observed.BaselineHash;
			evidence.ReplicaTopologyRevision = observed.TopologyRevision;
			evidence.ReplicaAuthorityEpoch = observed.AuthorityEpoch;
			evidence.ReplicaPrepareRevision = observed.PrepareRevision;
			evidence.ReplicaClockDomain = observed.H.Domain;
			evidence.ReplicaSourceTick = observed.H.SourceTick;
			evidence.ReplicaDestinationTick = observed.H.DestinationTick;
		}
		// The local connector's applied watermark proves the destination body pose
		// reached the named baseline after its receipt was replicated.
		evidence.ReplicaPoseBegin = 0;
		evidence.ReplicaPoseEnd = next.Connection->Applied();
		const PortalReadiness readiness = next.Readiness->Evaluate(evidence);
		if (!Settings.CaptureSequence.empty()) {
			next.CapturedReadiness = std::make_shared<PortalReadinessEvidence>(evidence);
			next.CapturedReadinessDecision = std::make_shared<PortalReadiness>(readiness);
		}
		next.SuppressRetainedCapture = readiness.SuppressRetainedCapture;
		next.LivePresentationReady = readiness.Live;
		if (!next.LivePresentationReady) return;
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
			// The retained eye kept the destination visible while the local replica
			// caught up. Retire it before the matching live body is submitted.
			if (next.SuppressRetainedCapture) PortalImages->RemoveViewport(PORTAL_SUCCESSOR_VIEW);
			next.DrawingArrivedPlayer = true;
		}
		// The body can arrive while a trailing or cleared eye remains in another world.
		// Keep the old presentation until that eye's persistent viewport has an image.
		render::View arrivedEye;
		// The gate prepares a staged local packet while slot zero keeps presenting the
		// source route. Its portal captures belong to the successor lifetime.
		arrivedEye.Slot = PORTAL_TRAILING_VIEW;
		arrivedEye.CameraFrame = next.Camera->Frame;
		arrivedEye.Camera = next.Camera->Lens;
		const bool arrivedEyePrepared = PreparePortalEye(
			arrivedEye,
			next.World,
			static_cast<uint32_t>(std::max(Settings.Width, 1)),
			static_cast<uint32_t>(std::max(Settings.Height, 1)),
			true,
			false
		);
		next.ArrivedEyeWorld = arrivedEye.WorldName;
		// PreparePortalWorldView assigns PortalDrawing only after it has presented the
		// local scene, collected its rows, and submitted its interface. A retained source
		// presentation is never proof that the arrived eye is locally drawable.
		const bool arrivedEyeWasAlreadyBound =
			(PortalDrawing == &next.View && arrivedEye.WorldName == Universe_->NameOf(next.World)) ||
			(next.LocalTrailingEye && PortalDrawing == next.LocalTrailingEye.get() &&
			 arrivedEye.WorldName == Universe_->NameOf(Replicated));
		if (!Settings.CaptureSequence.empty()) {
			if (!next.CapturedEyeProbe) next.CapturedEyeProbe.emplace();
			auto &probe = *next.CapturedEyeProbe;
			probe.Prepared = arrivedEyePrepared;
			probe.BoundWorld = arrivedEye.WorldName.Text();
			probe.Slot = arrivedEye.Slot;
			probe.ImageKey = arrivedEye.EyeImageKey.Text();
			probe.Image = arrivedEye.EyeImage;
		}
		if (PortalArrivedEyeBlocked(arrivedEyePrepared, arrivedEyeWasAlreadyBound, arrivedEye.EyeImage)) {
			next.PromotionBlocker = "arrived-eye-image";
			return;
		}
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
		bool destinationPredictionReady = false;
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
			if (prediction) {
				adoptedPrediction =
					AdoptPortalPrediction(store, next.Player, *prediction, Universe_->AlphaOf(next.World));
				destinationPredictionReady = adoptedPrediction;
			} else {
				// A destination snapshot can precede the source Motion message. Seed its
				// local body before switching input ownership, as the primary poll would
				// otherwise do only on the following frame.
				destinationPredictionReady = SeedPortalAdoptionPrediction(
					store,
					next.Player,
					next.Connection->Applied(),
					next.Connection->Unconfirmed(),
					next.Offer.Claim.DestinationIncarnation
				);
			}
			if (destinationPredictionReady)
				if (const auto *active = store.Resource<scene::ActiveCamera>())
					store.Remove<scene::CameraPortalView>(active->Entity);
		});
		if (!destinationPredictionReady) {
			next.PromotionBlocker = "destination-prediction";
			return;
		}
		if (adoptedPrediction) {
			next.Connection->UsePoseAcknowledgements();
			if (!next.Connection->ContinueInputs(prediction->Inputs, prediction->CoveredThrough)) {
				next.Failure = "destination input history could not continue";
				return;
			}
		} else
			next.Connection->UsePoseAcknowledgements();
		RetainPortalObservation(previous);
		Discovery.reset();
		Socket = std::move(next.Socket);
		Connection = std::move(next.Connection);
		// The successor callback only accepts its pending attempt. Once this socket is
		// primary it must receive the next portal offer through the normal server path.
		Connection->OnUserMessage([this](std::span<const std::byte> message) {
			ReceiveServerMessage(message);
		});
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
		if (PortalDrawing == &PortalNext->View ||
			(PortalNext->LocalTrailingEye && PortalDrawing == PortalNext->LocalTrailingEye.get()))
			PortalDrawing = nullptr;
		PortalNext.reset();
		PortalImages->RemoveViewport(PORTAL_SUCCESSOR_VIEW);
		PortalImages->RemoveViewport(PORTAL_TRAILING_VIEW);
		VisualResourcesChanged = true;
		PresentationInvalidated = true;
	}
}

// PortalImageProducer::Pump, which serves capture requests each frame.
#include "portal/PortalImageProducerState.hpp"

namespace engine::render {
	PortalProducerProgress
	PortalImageProducer::Pump(float frameSeconds, float alpha, Time now, bool destinationPresented) {
		ENGINE_PROFILE_CAT("portal image producer", core::ProfileCategory::Render);
		auto &state = *State;
		PortalProducerProgress progress;
		if (!Clock(state.LastTime, now) || !std::isfinite(frameSeconds) || frameSeconds < 0 ||
			!std::isfinite(alpha) || alpha < 0 || alpha > 1) {
			return progress;
		}
		if (state.Requests.Channel != PORTAL_REQUEST_CHANNEL ||
			(state.Resident != nullptr && !state.Render.OwnsResident(*state.Resident)) ||
			!Local(state.Universe, state.World, state.Requests)) {
			Clear();
			return progress;
		}
		state.ExpireShadows(now);
		state.AdvanceShadowTransfers(now, progress);
		std::erase_if(state.FailureReplies, [&](Impl::Capture &capture) {
			if (now >= capture.Deadline) {
				++progress.Refused;
				return true;
			}
			return state.Send(capture, progress) != world::PresentationStatus::Full;
		});
		std::erase_if(state.Captures, [&](Impl::Capture &capture) {
			if (!state.Authorized(capture.ReplyTo, capture.RetainedBodyPlayer)) {
				if (now >= capture.Deadline) {
					state.Cancel(capture);
					++progress.Refused;
					return true;
				}
				state.DenyRetainedBody(capture);
				return state.Send(capture, progress) != world::PresentationStatus::Full;
			}
			if (capture.Token == 0 && now >= capture.Deadline) {
				state.Cancel(capture);
				++progress.Refused;
				return true;
			}
			if (capture.LayerTokens[0] != 0) {
				if (!state.CollectLayers(capture, now)) return false;
			} else if (capture.Token != 0) {
				auto image = state.Render.TakeResourceImage(capture.Token);
				if (!image && now < capture.Deadline) {
					return false;
				}
				if (image && image->Status == ResourceImageStatus::Ok) {
					capture.Reply.Status = PortalImageStatus::Ok;
					capture.Reply.Width = image->Width;
					capture.Reply.Height = image->Height;
					capture.Reply.RowStride = image->RowStride;
					capture.Reply.Pixels = std::move(image->Pixels);
					capture.Reply.PixelHash = assets::Hasher::Of(capture.Reply.Pixels);
					capture.Reply.Depth = std::move(image->Depth);
					if (!capture.Reply.Depth.empty())
						capture.Reply.DepthHash = assets::Hasher::Of(capture.Reply.Depth);
				} else {
					state.Render.CancelResourceImage(capture.Token);
					capture.Reply.Status = PortalImageStatus::Failed;
					capture.Reply.Diagnostic =
						image ? "destination graph capture failed" : "destination capture expired";
				}
				capture.Token = 0;
			}
			const auto status = state.Send(capture, progress);
			const bool retire = status != world::PresentationStatus::Full || now >= capture.Deadline;
			if (retire) state.Cancel(capture);
			return retire;
		});
		using Job = Impl::Job;
		std::vector<Job *> jobs;
		for (size_t slot = 0; slot < state.Pending.size(); ++slot) {
			std::array<std::vector<std::byte>, MAX_PORTAL_CAPTURE_TREE_DEPTH + 1> shadowPulls;
			std::array<PortalShadowReplyExpectation, MAX_PORTAL_CAPTURE_TREE_DEPTH + 1> shadowExpected;
			size_t shadowCount = 0;
			for (const auto &transfer : state.Transfers) {
				if (!state.Pending[slot].Source || !transfer ||
					transfer->Hop.Requester != state.Pending[slot].Replies ||
					transfer->Phase != Impl::ShadowTransferPhase::WaitChild || now >= transfer->Deadline ||
					!state.TransferCurrent(*transfer, false))
					continue;
				std::string error;
				if (!EncodePortalShadowPull(transfer->Forwarded, shadowPulls[shadowCount], error)) continue;
				shadowExpected[shadowCount] = {
					transfer->Hop.Producer,
					transfer->Hop.Requester,
					transfer->ChildCorrelation,
					shadowPulls[shadowCount]
				};
				++shadowCount;
			}
			const auto expectedShadow = std::span(shadowExpected).first(shadowCount);
			auto &pending = state.Pending[slot];
			if (!pending.Work) {
				if (pending.Source) {
					(void)pending.Source->PollExpected(now, expectedShadow);
					state.CollectShadowReply(*pending.Source);
				}
				continue;
			}
			auto &job = *pending.Work;
			const bool expired = now >= job.Output.Deadline;
			const auto requester = state.Universe.LookupPresentation(
				state.Universe.Find(core::Name(job.Output.ReplyTo.World)), job.Output.ReplyTo.Channel
			);
			if (expired || requester != job.Output.ReplyTo) {
				job.Output.Reply.Status = PortalImageStatus::Unavailable;
				job.Output.Reply.Diagnostic =
					expired ? "nested destination capture expired" : "nested destination requester retired";
				if (state.EyeDiagnostic && state.EyeDiagnosticCount++ < 64)
					ENGINE_WARN(
						"portal eye producer job ended world={} reason={} reply_to={}/{}/{}/{} "
						"current={}/{}/{}/{}",
						state.Requests.World,
						expired ? "expired" : "requester-retired",
						job.Output.ReplyTo.World,
						job.Output.ReplyTo.Channel,
						job.Output.ReplyTo.Session,
						job.Output.ReplyTo.Generation,
						requester.World,
						requester.Channel,
						requester.Session,
						requester.Generation
					);
				state.SendFailure(job.Output, progress);
				state.Finish(slot);
				continue;
			}
			job.Waiting = false;
			if (pending.Source) {
				for (const auto &completion : pending.Source->PollExpected(now, expectedShadow)) {
					for (auto &child : job.Children) {
						if (child.RequestId != completion.RequestId) continue;
						child.Version = completion.Version;
						if (job.Request.OrderedLayers && completion.Status == PortalImageStatus::Ok) {
							child.Tree = pending.Source->TakeTree(child.Demand.Binding.Portal.Text(), now);
							if (!child.Tree) {
								job.Failure = "nested ordered payload unavailable";
							} else {
								const auto &opaque = child.Tree->Nodes.front().Layers.Opaque;
								child.Version = {opaque.ContentRevision, opaque.LightingRevision};
								size_t nodeCount = 1;
								for (const auto &accepted : job.Children)
									if (accepted.Tree) nodeCount += accepted.Tree->Nodes.size();
								if (nodeCount > MAX_PORTAL_CAPTURE_TREE_NODES) {
									child.Tree.reset();
									job.Failure = "nested capture node budget exceeded";
									job.FailureStatus = PortalImageStatus::BudgetExceeded;
								}
							}
						}
						if (completion.Status != PortalImageStatus::Ok) {
							job.Failure = "nested destination capture failed: " + completion.Diagnostic;
							job.FailureStatus = completion.Status;
						}
					}
				}
				state.CollectShadowReply(*pending.Source);
			}
			jobs.push_back(&job);
		}
		for (auto &message : state.Universe.TakePresentation(state.Requests)) {
			++progress.Requests;
			PortalShadowPull pull;
			std::string pullError;
			if (DecodePortalShadowPull(message.Payload, pull, pullError)) {
				state.StartShadowPull(message, std::move(pull), now, progress);
				continue;
			}
			PortalImageRequest request;
			std::string error;
			if (!ReplySlot(message.From.Channel) ||
				!DecodePortalImageRequest(message.Payload, request, error) ||
				request.Key.RequestId != message.Correlation) {
				if (state.EyeDiagnostic && state.EyeDiagnosticCount++ < 64)
					ENGINE_WARN(
						"portal eye producer decode refused world={} correlation={} from={} reason={}",
						state.Requests.World,
						message.Correlation,
						message.From.Channel,
						error
					);
				++progress.Refused;
				continue;
			}
			if (state.EyeDiagnostic && request.Key.PortalKey == "viewport-eye" &&
				state.EyeDiagnosticCount++ < 64)
				ENGINE_WARN(
					"portal eye producer received world={} request={} from={} eye_player={} "
					"retained_player={}",
					state.Requests.World,
					request.Key.RequestId,
					message.From.Channel,
					request.EyePlayer,
					request.RetainedBodyPlayer
				);
			Impl::Capture capture;
			capture.ReplyTo = std::move(message.From);
			capture.WorldProvenanceComplete = request.RecursionDepth == 0;
			if (request.Entrance && !request.Entrance->SourceWorld.empty())
				Impl::RetainWorld(capture, request.Entrance->SourceWorld);
			state.RetainContentWorlds(capture);
			capture.Reply.Key = request.Key;
			capture.Reply.Scope = request.Scope;
			capture.TransferEye = request.TransferEye;
			if (request.TransferEye) core::Metrics::Count("render.portal.transfer-eye.received", 1);
			capture.RetainedBodyPlayer = request.RetainedBodyPlayer;
			capture.Deadline = now + CAPTURE_TIMEOUT;
			if (!state.Authorized(capture.ReplyTo, request.RetainedBodyPlayer)) {
				state.DenyRetainedBody(capture);
				state.SendFailure(capture, progress);
				continue;
			}
			ENGINE_TRACE(
				"portal image producer received: {} {} request {}",
				capture.ReplyTo.Channel,
				state.Requests.World,
				request.Key.RequestId
			);
			View view;
			const auto published = PublishedEndpoint(state.EndpointBindings, Borrow(state.Requests));
			capture.Producer = {
				std::string(published.World),
				std::string(published.Channel),
				published.Session,
				published.Generation
			};
			if (request.OrderedLayers &&
				(request.RecursionDepth != 0 || !request.RetainedBodyPlayer.empty()) &&
				state.EndpointBindings &&
				std::none_of(
					state.EndpointBindings->Exports.begin(),
					state.EndpointBindings->Exports.end(),
					[&](const auto &pair) { return pair.Local == state.Requests; }
				)) {
				capture.Reply.Status = PortalImageStatus::Unavailable;
				capture.Reply.Diagnostic = "delegated capture endpoint binding unavailable";
				state.SendFailure(capture, progress);
				continue;
			}
			if (request.RecursionDepth > MAX_PORTAL_DEPTH ||
				(request.Scope != PortalImageScope::CompleteWorld && !request.OrderedLayers &&
				 request.RecursionDepth != 0) ||
				!ResolvePortalCaptureCamera(
					{request.Position, request.Orientation, request.Frustum, request.ClipPlane},
					request.Projection,
					view
				)) {
				capture.Reply.Status = PortalImageStatus::Unsupported;
				capture.Reply.Diagnostic = "unsupported recursion depth or camera clip plane";
				state.SendFailure(capture, progress);
				continue;
			}
			const auto freeSlot =
				std::find_if(state.Pending.begin(), state.Pending.end(), [](const auto &slot) {
					return !slot.Work;
				});
			if (freeSlot == state.Pending.end()) {
				capture.Reply.Status = PortalImageStatus::BudgetExceeded;
				capture.Reply.Diagnostic = "destination capture queue is full";
				state.SendFailure(capture, progress);
				continue;
			}
			const size_t slot = size_t(freeSlot - state.Pending.begin());
			freeSlot->Work.emplace(Job{});
			auto &job = *freeSlot->Work;
			job.Request = std::move(request);
			job.Output = std::move(capture);
			job.Viewpoint = std::move(view);
			job.Slot = slot;
			if (job.Request.OrderedLayers && job.Request.RecursionDepth != 0) {
				job.Output.Tree.emplace();
				job.Output.Tree->Nodes.push_back(
					{{std::string(published.World),
					  std::string(published.Channel),
					  published.Session,
					  published.Generation},
					 {job.Request.Position,
					  job.Request.Orientation,
					  job.Request.Frustum,
					  job.Request.ClipPlane,
					  job.Request.Projection},
					 {},
					 job.Request.RetainedBodyPlayer}
				);
			}
			jobs.push_back(&job);
		}
		state.AdvanceShadowTransfers(now, progress);
		if (jobs.empty()) {
			return progress;
		}
		for (auto *entry : jobs) {
			auto &job = *entry;
			if (!state.Authorized(job.Output.ReplyTo, job.Request.RetainedBodyPlayer)) {
				job.Failure = "retained body authorization was withdrawn";
				continue;
			}
			const auto profile = CaptureProfile(job.Request);
			if (!state.PipelineReady[profile]) {
				state.PipelineReady[profile] =
					InstallCapture(state.Render, job.Request.Scope, job.Request.OrderedLayers);
			}
		}
		std::erase_if(jobs, [&](Job *entry) {
			auto &job = *entry;
			if (!job.Failure.empty()) {
				job.Output.Reply.Status = PortalImageStatus::Unavailable;
				job.Output.Reply.Diagnostic = job.Failure;
				state.SendFailure(job.Output, progress);
				state.Finish(job.Slot);
				return true;
			}
			if (state.PipelineReady[CaptureProfile(job.Request)]) {
				return false;
			}
			job.Output.Reply.Status = PortalImageStatus::Unavailable;
			job.Output.Reply.Diagnostic = "destination renderer capture pipeline unavailable";
			state.SendFailure(job.Output, progress);
			state.Finish(job.Slot);
			return true;
		});
		if (jobs.empty()) {
			return progress;
		}
		if (state.Shaders == nullptr) {
			state.OwnedShaders = std::make_unique<ShaderLibrary>();
			state.Shaders = state.OwnedShaders.get();
		}
		for (size_t index = 0; index < jobs.size(); ++index) {
			auto &frame = state.Frames[index];
			if ((jobs[index]->Request.Scope != PortalImageScope::CompleteWorld &&
				 !jobs[index]->Request.OrderedLayers) ||
				frame.InterfaceReady || !state.Render.HasDevice()) {
				continue;
			}
			frame.InterfaceReady = state.Render.InitialiseInterface(frame.Interface);
			frame.Interface.SetImageSource([&state, source = &frame](const core::Name &name) {
				const auto text = name.Text();
				const core::Name owner =
					text.starts_with("imagegraph-instance://") || text.starts_with("editable-image://")
						? state.OwnerName
						: state.ContentOwner;
				InterfaceImage image;
				image.Texture = state.Render.TextureHandle(name, owner);
				image.SampledSRGB = state.Render.RendererRef().TextureSamplesSRGB(name, owner);
				image.Cell = state.Render.TextureCell(name, source->PresentationSeconds, owner);
				state.Render.TextureSize(name, image.Width, image.Height, owner);
				return image;
			});
		}
		const world::Presentation presentation{state.World, frameSeconds, alpha};
		const bool presented =
			destinationPresented || state.Universe.PresentMany(std::span(&presentation, 1)) == 1;
		auto &instances = state.WorldFrame.Instances;
		auto &joints = state.WorldFrame.Joints;
		instances.clear();
		joints.clear();
		scene::WorldLighting lighting;
		uint64_t tick = 0, identity = 0;
		RegisterPresentationComponents();
		effects::RegisterEffectComponents();
		gui::RegisterGuiComponents();
		const bool copied = presented && state.Universe.Enter(state.World, [&](ecs::Store &store) {
			// Prepare once for this batch, before lookups and capture renewal compare resources.
			state.EditableImages.Refresh(store, state.Render.RendererRef(), state.OwnerName);
			state.EditableMeshes.Refresh(store, state.Render.RendererRef(), state.ContentOwner);
			PrepareWorldShaders(
				store,
				state.ContentOwner,
				*state.Shaders,
				state.Render.RendererRef(),
				nullptr,
				state.PostProcessing
			);
			gui::DemandedShaders(store, state.GuiShaders);
			state.GuiShaderSignature = 0;
			for (const auto name : state.GuiShaders) {
				state.GuiShaderSignature = scene::MixSignature(state.GuiShaderSignature, name.Id());
				const auto *module = state.Shaders->Find(name, state.ContentOwner);
				if (module == nullptr) continue;
				for (const auto byte : module->CodeHash.Digest)
					state.GuiShaderSignature = scene::MixSignature(state.GuiShaderSignature, byte);
			}
			for (size_t index = 0; index < jobs.size(); ++index) {
				auto &frame = state.Frames[index];
				if (frame.InterfaceReady)
					frame.Interface.RefreshShaders(state.GuiShaders, *state.Shaders, state.ContentOwner);
			}

			if (store.Resource<DrawList>() == nullptr) {
				store.SetResource(DrawList{});
			}
			// Replica presentation already sampled its received snapshots. Rebuilding
			// from ECS transforms here would replace that pose with the latest packet.
			if (!destinationPresented && !store.AdoptOnly()) {
				scene::SyncRendered(store);
				CollectInstances(store);
			}
			CollectWorldView(store, state.OwnerName, state.WorldFrame);
			lighting = state.WorldFrame.Lighting;
			tick = state.WorldFrame.Tick;
			identity = state.WorldFrame.Identity;
			const auto &seams = state.WorldFrame.Seams;

			for (size_t index = 0; index < jobs.size(); ++index) {
				auto &frame = state.Frames[index];
				auto &view = jobs[index]->Viewpoint;
				frame.PresentationSeconds = PortalPresentationSeconds(state.WorldFrame.Seconds, std::nullopt);
				frame.Entrance = jobs[index]->Request.Entrance
									 ? ResolvePortalEntrance(*jobs[index]->Request.Entrance, seams)
									 : std::nullopt;
				frame.EntranceRows.clear();
				if (frame.Entrance) {
					for (const auto &row : instances) {
						if (row.Surface != *frame.Entrance) frame.EntranceRows.push_back(row);
					}
				}
				auto &job = *jobs[index];
				job.ViewPrepared = false;
				view.World = state.World.Index;
				view.WorldName = state.OwnerName;
				view.ContentOwner = state.ContentOwner;
				view.ForeignContentOwners = state.ForeignContentOwners;
				view.EyePlayer.reset();
				if (!job.Request.EyePlayer.empty()) {
					int64_t player = 0;
					const auto &text = job.Request.EyePlayer;
					std::from_chars(text.data(), text.data() + text.size(), player);
					view.EyePlayer = player;
				}
				ResolveEyeBody(store, view);
				view.Instances = instances;
				view.JointFrames = joints;
				const bool revalidateChildren = job.Collected;
				if (!job.Collected) {
					job.Collected = true;
					job.Portals = state.WorldFrame.Portals;
					job.LocalPixels = job.Request.PixelBudget - uint64_t(job.Request.Width) *
																	job.Request.Height *
																	(job.Request.OrderedLayers ? 4 : 1);
					if ((job.Request.Scope == PortalImageScope::CompleteWorld || job.Request.OrderedLayers) &&
						job.Request.RecursionDepth != 0) {
						SceneTarget target{job.Request.Width, job.Request.Height};
						view.Target = &target;
						PortalImageDemandSettings settings;
						settings.Width = job.Request.Width;
						settings.Height = job.Request.Height;
						settings.RecursionDepth = job.Request.RecursionDepth - 1;
						settings.PixelBudget = job.Request.PixelBudget;
						std::vector<PortalImageDemand> demands;
						const auto counts = CollectPortalImageDemands(
							store, view, settings, demands, job.Portals, state.WorldFrame.Slots
						);
						view.Target = nullptr;
						std::erase_if(demands, [&](const auto &demand) {
							return frame.Entrance && demand.Portal.Index == *frame.Entrance;
						});
						std::erase_if(job.Portals, [&](const auto &portal) {
							return frame.Entrance && portal.Index == *frame.Entrance;
						});
						if (counts.Invalid || counts.Unsupported)
							job.Failure = "nested portal demand is invalid or unsupported";
						for (auto &demand : demands) {
							if (job.Request.OrderedLayers) {
								demand.Request.OrderedLayers = true;
								demand.Request.Scope = PortalImageScope::OpaqueLighting;
								demand.Request.EyePlayer = job.Request.EyePlayer;
								demand.Request.RetainedBodyPlayer = job.Request.RetainedBodyPlayer;
								demand.Binding.ExpectedScope = PortalImageScope::OpaqueLighting;
							}
							if (!job.Request.Geometry.empty()) {
								const auto seam =
									std::find_if(seams.begin(), seams.end(), [&](const auto &candidate) {
										return candidate.Surface == demand.Portal.Index;
									});
								if (seam == seams.end() ||
									!ForwardPortalDraws(
										job.Request.Geometry, *seam, demand.Request.Geometry, job.Failure
									)) {
									if (job.Failure.empty())
										job.Failure = "nested portal geometry has no matching seam";
									break;
								}
							}
							Impl::Child child;
							child.Edge.PortalKey = demand.Request.Key.PortalKey;
							child.Demand = std::move(demand);
							job.Children.push_back(std::move(child));
						}
					}
				}
				if (revalidateChildren &&
					(job.Request.Scope == PortalImageScope::CompleteWorld || job.Request.OrderedLayers) &&
					job.Request.RecursionDepth != 0) {
					// Check the visible set as well as existing children. New openings must
					// not inherit a parent capture that never requested their destination.
					PortalImageDemandSettings settings;
					settings.Width = job.Request.Width;
					settings.Height = job.Request.Height;
					settings.RecursionDepth = job.Request.RecursionDepth - 1;
					settings.PixelBudget = job.Request.PixelBudget;
					size_t matched = 0;
					for (const auto &seam : seams) {
						if (!seam.Crosses || (frame.Entrance && seam.Surface == *frame.Entrance)) continue;
						const auto child = std::find_if(
							job.Children.begin(), job.Children.end(), [&](const auto &candidate) {
								return !candidate.Demand.SeamRadiance &&
									   candidate.Demand.Portal.Index == seam.Surface;
							}
						);
						const auto key = child == job.Children.end() ? core::Name("nested-validation")
																	 : child->Demand.Binding.Portal;
						PortalImageDemand current;
						const auto status =
							BuildPortalImageDemand(seam, key, view, view.Slot, settings, current);
						if (status == PortalDemandStatus::Hidden) continue;
						if (status != PortalDemandStatus::Ready || child == job.Children.end() ||
							current.Request.Key.SeamRevision != child->Demand.Request.Key.SeamRevision) {
							job.Failure = "nested portal mapping changed while capture waited";
							break;
						}
						++matched;
					}
					const size_t primaryChildren = static_cast<size_t>(
						std::count_if(job.Children.begin(), job.Children.end(), [](const auto &child) {
							return !child.Demand.SeamRadiance;
						})
					);
					// A radiance child shares its seam with the primary image child.
					if (matched != primaryChildren)
						job.Failure = "nested portal mapping changed while capture waited";
				}

				CollectSurfaceViews(store, frame.Surfaces, job.Portals, &view, state.WorldFrame.Slots);
				std::erase_if(frame.Surfaces, [&](const auto &surface) {
					return (frame.Entrance && surface.Index == *frame.Entrance) ||
						   std::any_of(job.Portals.begin(), job.Portals.end(), [&](const auto &portal) {
							   return portal.ExternalImage && portal.Index == surface.Index;
						   });
				});
				if (job.Request.OrderedLayers && job.Request.RecursionDepth != 0 && !frame.Surfaces.empty()) {
					job.Failure = "ordered capture tree cannot represent local reflected surfaces";
					job.FailureStatus = PortalImageStatus::Unsupported;
				}
				const auto &pending = state.Pending[job.Slot];
				const bool waitingForChildren =
					std::any_of(job.Children.begin(), job.Children.end(), [&](const auto &child) {
						if (job.Request.OrderedLayers) return !child.Tree.has_value();
						return child.RequestId == 0 || !pending.Source ||
							   pending.Source->CurrentImage(child.Demand.Binding.Portal.Text()) == 0;
					});
				// Ordered jobs need current spatial collectors to reserve top-layer pixels
				// before child requests. Complete-world jobs wait before camera preparation.
				if ((waitingForChildren && !job.Request.OrderedLayers) || !job.Failure.empty()) continue;
				job.ViewPrepared = true;
				core::Metrics::Count("render.portal_snapshot.prepared_views", 1);
				const core::Vector2 extent{
					float(jobs[index]->Request.Width), float(jobs[index]->Request.Height)
				};
				CollectWorldCamera(store, view, extent, frame.Camera);
				if (job.Request.OrderedLayers && !waitingForChildren) {
					for (auto &child : job.Children) {
						const auto &portal = child.Demand.Portal;
						auto &edge = child.Edge;
						edge.Centre = {portal.Centre.X, portal.Centre.Y, portal.Centre.Z};
						edge.First = {portal.First.X, portal.First.Y, portal.First.Z};
						edge.Second = {portal.Second.X, portal.Second.Y, portal.Second.Z};
						const auto position = portal.Warp.Point({});
						const auto rotation = portal.Warp.Frame.Rotation();
						edge.Position = {position.X, position.Y, position.Z};
						edge.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
						edge.Scale = portal.Warp.Scale;
						std::vector<scene::DrawInstance> aperture;
						for (const auto &row : instances)
							if (row.Surface == portal.Index) aperture.push_back(row);
						if (aperture.empty() ||
							!EncodePortalDraws(store, aperture, joints, edge.Geometry, job.Failure)) {
							if (job.Failure.empty()) job.Failure = "nested aperture geometry unavailable";
							break;
						}
					}
				}
			}
		}) == world::WorldStatus::Ok;
		size_t copiedBytes =
			instances.size() * sizeof(scene::DrawInstance) + joints.size() * sizeof(core::CFrame);
		for (size_t index = 0; index < jobs.size(); ++index) {
			const auto &frame = state.Frames[index];
			copiedBytes += frame.EntranceRows.size() * sizeof(scene::DrawInstance) +
						   frame.Surfaces.size() * sizeof(SurfaceView);
			if (jobs[index]->ViewPrepared)
				copiedBytes += frame.Camera.Lights.size() * sizeof(SceneLight) +
							   frame.Camera.Ribbons.Vertices.size() * sizeof(effects::RibbonVertex) +
							   frame.Camera.Ribbons.Runs.size() * sizeof(effects::RibbonRun) +
							   frame.Camera.SpatialCollectors.size() * sizeof(SpatialCollector);
		}
		copiedBytes += state.WorldFrame.Portals.size() * sizeof(PortalView);
		copiedBytes += state.WorldFrame.Particles.Blocks.size() * sizeof(effects::EmitterBlock) +
					   state.WorldFrame.Particles.SpawnStates.size() * sizeof(effects::EmitterSpawnState) +
					   state.WorldFrame.Particles.RuntimeStates.size() * sizeof(effects::EmitterRuntime);
		if (copied) {
			copiedBytes += sizeof(lighting);
		}
		core::Metrics::Count("render.portal_snapshot.bytes", copiedBytes);
		for (size_t index = 0; index < jobs.size(); ++index) {
			auto &job = *jobs[index];
			if (!copied) {
				job.Output.Reply.Status = PortalImageStatus::Unavailable;
				job.Output.Reply.Diagnostic = "destination world presentation unavailable";
				state.SendFailure(job.Output, progress);
				continue;
			}
			auto &pending = state.Pending[job.Slot];
			if (!job.Children.empty() && job.Failure.empty()) {
				if (job.Request.OrderedLayers && job.Children.size() >= MAX_PORTAL_CAPTURE_TREE_NODES) {
					job.Failure = "nested capture node budget exceeded";
					job.FailureStatus = PortalImageStatus::BudgetExceeded;
				}
				if (pending.Source && pending.PayloadSource != job.Request.OrderedLayers) {
					pending.Source.reset();
					// A fresh inbox restarts correlations, so delayed replies need a new incarnation.
					state.Universe.ClosePresentation(pending.Replies);
					pending.Replies = {};
				}
				if (!pending.Source) {
					if (pending.Replies.Generation == 0) {
						pending.Replies =
							state.Universe
								.OpenPresentation(
									state.World,
									core::Name(std::string(NESTED_REPLY_CHANNEL) + std::to_string(job.Slot))
								)
								.Address;
					}
					if (pending.Replies.Generation != 0)
						pending.Source = std::make_unique<PortalImageSource>(
							state.Universe,
							state.Render.RendererRef(),
							state.World,
							pending.Replies,
							PortalInboxLimits{},
							state.Resident,
							job.Request.OrderedLayers ? PortalImageSourceDelivery::CapturePayloads
													  : PortalImageSourceDelivery::ImportedImages
						);
					pending.PayloadSource = job.Request.OrderedLayers;
					if (pending.Source) pending.Source->SetEndpointBindings(state.EndpointBindings);
				}
				if (!pending.Source) job.Failure = "nested reply endpoint unavailable";
				const auto &collectors = state.Frames[index].Camera.SpatialCollectors;
				const bool topOverlay =
					std::any_of(collectors.begin(), collectors.end(), [](const auto &placed) {
						return placed.Canvas.Visible && placed.Canvas.AlwaysOnTop;
					});
				const uint64_t ownPixels = uint64_t(job.Request.Width) * job.Request.Height *
										   (job.Request.OrderedLayers ? 4 + topOverlay : 1);
				if (ownPixels > job.Request.PixelBudget) {
					job.Failure = "destination layers exceed capture pixel budget";
					job.FailureStatus = PortalImageStatus::BudgetExceeded;
				}
				const uint64_t remaining =
					ownPixels <= job.Request.PixelBudget ? job.Request.PixelBudget - ownPixels : 0;
				const bool hasLocalShare =
					!job.Request.OrderedLayers && !state.Frames[index].Surfaces.empty();
				const uint32_t childPixelMultiplier = job.Request.OrderedLayers ? 4u : 1u;
				const size_t mandatoryCount = static_cast<size_t>(
					std::count_if(job.Children.begin(), job.Children.end(), [](const auto &child) {
						return !child.Demand.SeamRadiance;
					})
				);
				const size_t mandatoryShares = mandatoryCount + (hasLocalShare ? 1u : 0u);
				const uint64_t mandatoryBudget = mandatoryShares == 0 ? 0 : remaining / mandatoryShares;
				// Fit visible children to their fair slice before the planner applies its refusal rule.
				std::vector<PortalChildBudget> budgets;
				budgets.reserve(job.Children.size());
				std::vector<PortalChildExtentFit> childFits(job.Children.size());
				bool fitPossible = true;
				for (size_t childIndex = 0; childIndex < job.Children.size(); ++childIndex) {
					const auto &child = job.Children[childIndex];
					const auto &request = child.Demand.Request;
					uint64_t pixels = uint64_t(request.Width) * request.Height * childPixelMultiplier;
					if (!child.Demand.SeamRadiance && child.RequestId == 0) {
						const bool fitted = FitPortalChildExtent(
							request.Width,
							request.Height,
							mandatoryBudget,
							childPixelMultiplier,
							childFits[childIndex]
						);
						fitPossible = fitPossible && fitted;
						if (fitted) pixels = childFits[childIndex].Pixels;
					}
					budgets.push_back({pixels, child.Demand.SeamRadiance});
				}
				uint32_t childBudget = 0;
				const bool budgetsPlanned =
					PlanPortalChildBudgets(budgets, remaining, hasLocalShare, childBudget, job.LocalPixels);
				if (!fitPossible || !budgetsPlanned) {
					job.Failure = "nested portal pixel budget exceeded";
					job.FailureStatus = PortalImageStatus::BudgetExceeded;
				}
				for (size_t childIndex = job.Children.size(); childIndex != 0; --childIndex) {
					const size_t selected = childIndex - 1;
					if (budgets[selected].Accepted) continue;
					const auto &discarded = job.Children[selected].Demand;
					for (auto &portal : job.Portals) {
						if (portal.Index != discarded.Portal.Index) continue;
						portal.LightImagePortal = {};
						portal.ImportedLightImage = 0;
						portal.LightOutward = {};
					}
					job.Children.erase(job.Children.begin() + selected);
					childFits.erase(childFits.begin() + selected);
					budgets.erase(budgets.begin() + selected);
				}
				auto &view = job.Viewpoint;
				const auto &seams = state.WorldFrame.Seams;
				for (size_t childIndex = 0; childIndex < job.Children.size(); ++childIndex) {
					auto &child = job.Children[childIndex];
					if (!job.Failure.empty()) break;
					auto &demand = child.Demand;
					const auto destination = state.Universe.Find(demand.DestinationWorld);
					const auto producer =
						state.Universe.LookupPresentation(destination, PORTAL_REQUEST_CHANNEL);
					if (child.RequestId == 0) {
						if (!budgets[childIndex].Accepted) continue;
						if (!demand.SeamRadiance) {
							auto original = std::move(demand);
							auto &originalRequest = original.Request;
							const auto seam =
								std::find_if(seams.begin(), seams.end(), [&](const auto &candidate) {
									return candidate.Surface == original.Portal.Index;
								});
							PortalImageDemand fitted;
							PortalImageDemandSettings settings;
							settings.Width = originalRequest.Width;
							settings.Height = originalRequest.Height;
							settings.MaximumExtent = childFits[childIndex].MaximumExtent;
							settings.RecursionDepth = originalRequest.RecursionDepth;
							settings.PixelBudget = childBudget;
							if (seam == seams.end() ||
								BuildPortalImageDemand(
									*seam, original.Binding.Portal, view, view.Slot, settings, fitted
								) != PortalDemandStatus::Ready) {
								job.Failure = "nested portal demand could not fit its pixel budget";
								job.FailureStatus = PortalImageStatus::BudgetExceeded;
								break;
							}
							fitted.Portal = original.Portal;
							fitted.Request.Geometry = std::move(originalRequest.Geometry);
							fitted.Request.KnownImage = originalRequest.KnownImage;
							fitted.Request.Scope = originalRequest.Scope;
							fitted.Request.EyePlayer = std::move(originalRequest.EyePlayer);
							fitted.Request.RetainedBodyPlayer = std::move(originalRequest.RetainedBodyPlayer);
							fitted.Request.OrderedLayers = originalRequest.OrderedLayers;
							fitted.Request.TransferEye = originalRequest.TransferEye;
							if (originalRequest.Entrance && fitted.Request.Entrance)
								fitted.Request.Entrance->SourceWorld = originalRequest.Entrance->SourceWorld;
							fitted.Binding.ExpectedScope = original.Binding.ExpectedScope;
							fitted.Binding.ExpectedProjection = original.Binding.ExpectedProjection;
							fitted.Binding.Layer = original.Binding.Layer;
							demand = std::move(fitted);
						} else {
							demand.Request.PixelBudget = childBudget;
						}

						if (uint64_t(demand.Request.Width) * demand.Request.Height *
								(job.Request.OrderedLayers ? 4 : 1) >
							childBudget) {
							job.Failure = "nested portal pixel budget exceeded";
							job.FailureStatus = PortalImageStatus::BudgetExceeded;
							break;
						}
						if (producer.Generation == 0) {
							job.Waiting = true;
							continue;
						}
						const auto key = core::Name(
							std::string(NESTED_REPLY_CHANNEL) + std::to_string(job.Slot) + "/" +
							std::to_string(demand.Portal.Index)
						);
						demand.Request.Key.PortalKey = key.Text();
						demand.Binding.Portal = key;
						demand.Portal.ImagePortal = key;
						for (auto &portal : job.Portals)
							if (portal.Index == demand.Portal.Index) portal.ImagePortal = key;
						const auto issued =
							pending.Source->Issue(producer, demand.Request, demand.Binding, now);
						if (issued.Status == PortalInboxStatus::Issued) {
							child.RequestId = issued.RequestId;
							child.Producer = producer;
						} else if (issued.Status != PortalInboxStatus::Busy &&
								   issued.Transport != world::PresentationStatus::Full) {
							job.Failure = "nested image request refused";
							job.FailureStatus = issued.Status == PortalInboxStatus::Full
													? PortalImageStatus::BudgetExceeded
													: PortalImageStatus::Unsupported;
						}
						// A full transport queue releases the unsent reservation; retry after it drains.
						job.Waiting = true;
						continue;
					}
					if (producer != child.Producer) {
						job.Failure = "nested producer endpoint retired";
						break;
					}
					if (job.Request.OrderedLayers) {
						if (!child.Tree) job.Waiting = true;
						continue;
					}
					const auto image = pending.Source->CurrentImage(demand.Binding.Portal.Text());
					if (image == 0) job.Waiting = true;
					for (auto &portal : job.Portals)
						if (portal.Index == demand.Portal.Index) portal.ImportedImage = image;
				}
			}
			if (!job.Failure.empty()) {
				job.Waiting = false;
				job.Output.Reply.Status = job.FailureStatus;
				job.Output.Reply.Diagnostic = job.Failure;
				state.SendFailure(job.Output, progress);
				continue;
			}
			if (job.Waiting || state.Captures.size() >= MAX_CAPTURES || progress.Rendered >= MAX_CAPTURES) {
				job.Waiting = true;
				continue;
			}
			auto &view = job.Viewpoint;
			SceneTarget target{job.Request.Width, job.Request.Height};
			view.Target = &target;
			view.World = state.World.Index;
			view.WorldName = state.OwnerName;
			view.ContentOwner = state.ContentOwner;
			view.ForeignContentOwners = state.ForeignContentOwners;
			view.Pipeline = CapturePipeline(job.Request.Scope, job.Request.OrderedLayers);
			view.Instances = instances;
			if (state.Frames[index].Entrance) view.Instances = state.Frames[index].EntranceRows;
			view.JointFrames = joints;
			std::vector<scene::DrawInstance> joinedInstances;
			std::vector<core::CFrame> joinedJoints;
			PortalDrawSelection bodySelection{job.Request.EyePlayer, {}};
			bodySelection.RetainedBodyPlayer = job.Request.RetainedBodyPlayer;
			view.DirectionalShadowBounds.reset();
			view.EyeHiddenRows = {};
			if (!job.Request.Geometry.empty()) {
				PortalGeometry packet;
				std::string packetError;
				if (!DecodePortalGeometry(job.Request.Geometry, packet, packetError)) {
					job.Output.Reply.Status = PortalImageStatus::Failed;
					job.Output.Reply.Diagnostic = std::move(packetError);
					state.SendFailure(job.Output, progress);
					continue;
				}
				state.Frames[index].PresentationSeconds = PortalPresentationSeconds(
					state.Frames[index].PresentationSeconds, packet.PresentationSeconds
				);
				joinedInstances.assign(view.Instances.begin(), view.Instances.end());
				joinedJoints = joints;
				std::string error;
				bool appended = false;
				const auto entered = state.Universe.Enter(state.World, [&](ecs::Store &store) {
					appended = AppendPortalDraws(
						job.Request.Geometry,
						core::Name(job.Output.ReplyTo.World),
						joinedInstances,
						joinedJoints,
						error,
						&bodySelection,
						&store
					);
				});
				if (entered != world::WorldStatus::Ok || !appended) {
					if (error.empty()) error = "portal geometry destination unavailable";
					job.Output.Reply.Status = PortalImageStatus::Failed;
					job.Output.Reply.Diagnostic = std::move(error);
					state.SendFailure(job.Output, progress);
					continue;
				}
				view.Instances = joinedInstances;
				view.JointFrames = joinedJoints;
				view.EyeHiddenRows = bodySelection.Hidden;
				core::Metrics::Count("render.portal_geometry.bytes", job.Request.Geometry.size());
				core::Metrics::Count("render.portal_geometry.rows", bodySelection.Appended);
				core::Metrics::Count("render.portal_geometry.replaced", bodySelection.Replaced);
			}
			if (!job.Request.RetainedBodyPlayer.empty()) {
				if (!state.Authorized(job.Output.ReplyTo, job.Request.RetainedBodyPlayer)) {
					state.DenyRetainedBody(job.Output);
					state.SendFailure(job.Output, progress);
					continue;
				}
				if (job.Request.Geometry.empty())
					joinedInstances.assign(view.Instances.begin(), view.Instances.end());
				std::vector<scene::DrawInstance> drawable;
				for (const auto &row : joinedInstances) {
					core::Vector3 meshExtent;
					if (!row.Mesh.IsValid() ||
						state.Render.MeshExtentOf(row.Mesh, meshExtent, view.ContentOwnerOf(row.SourceWorld)))
						drawable.push_back(row);
				}
				view.DirectionalShadowBounds = graph::BoundsOfAll(drawable);
				std::string error;
				bool removed = false;
				const auto entered = state.Universe.Enter(state.World, [&](ecs::Store &store) {
					removed = RemoveRetainedPortalBody(
						store,
						job.Request.RetainedBodyPlayer,
						joinedInstances,
						bodySelection.RetainedBodyRows,
						bodySelection.Hidden,
						error
					);
				});
				if (entered != world::WorldStatus::Ok || !removed) {
					job.Output.Reply.Status = PortalImageStatus::Unavailable;
					job.Output.Reply.Diagnostic = error.empty() ? "retained body source unavailable" : error;
					state.SendFailure(job.Output, progress);
					continue;
				}
				view.Instances = joinedInstances;
				view.EyeHiddenRows = bodySelection.Hidden;
			}
			view.Lights = state.Frames[index].Camera.Lights;
			view.LensTimeSeconds = static_cast<float>(state.Frames[index].PresentationSeconds);
			view.Lighting = lighting;
			view.OverrideLighting = true;

			FrameOverlayHook *interface = nullptr;
			if (CaptureIncludesWorldEffects(job.Request.Scope, job.Request.OrderedLayers)) {
				view.Surfaces = state.Frames[index].Surfaces;
				view.Portals = job.Portals;
				view.Particles = state.WorldFrame.Particles.Batches;
				view.ParticleSeams = state.WorldFrame.Particles.Seams;
				view.ParticleRevision = state.WorldFrame.Particles.Revision;
				view.ParticleLayoutRevision = state.WorldFrame.Particles.LayoutRevision;
				view.ParticleResidentRevision = state.WorldFrame.Particles.ResidentRevision;
				view.ParticleBlocks = state.WorldFrame.Particles.BlockCount;
				view.ParticlePool = state.WorldFrame.Particles.Pool;
				view.ParticleDelta = index == 0 ? frameSeconds : 0;
				view.RibbonVertices = state.Frames[index].Camera.Ribbons.Vertices;
				view.RibbonRuns = state.Frames[index].Camera.Ribbons.Runs;
				view.SurfaceBudget = View::SurfaceCaptureBudget{job.Request.RecursionDepth, job.LocalPixels};
			}
			if (job.Request.Scope == PortalImageScope::CompleteWorld || job.Request.OrderedLayers) {
				auto &frame = state.Frames[index];
				if (!frame.Camera.SpatialCommands.Commands.empty()) {
					if (!frame.InterfaceReady) {
						job.Output.Reply.Status = PortalImageStatus::Unavailable;
						job.Output.Reply.Diagnostic = "destination spatial interface unavailable";
						state.SendFailure(job.Output, progress);
						continue;
					}
					const core::Vector2 extent{float(job.Request.Width), float(job.Request.Height)};
					frame.Interface.SetContentOwner(state.ContentOwner);
					frame.Interface.Submit(frame.Camera, extent, extent);
					interface = &frame.Interface;
				}
			}

			ScenePresentationState signatureState;
			signatureState.Lighting = lighting;
			signatureState.Resources = scene::MixSignature(identity, state.Render.ResourceRevision());
			if (job.Request.Scope == PortalImageScope::CompleteWorld || job.Request.OrderedLayers)
				signatureState.Resources =
					scene::MixSignature(signatureState.Resources, state.GuiShaderSignature);
			signatureState.Resources = scene::MixSignature(signatureState.Resources, state.ContentOwner.Id());
			for (const WorldContentOwner &binding : state.ForeignContentOwners) {
				signatureState.Resources = scene::MixSignature(signatureState.Resources, binding.World.Id());
				signatureState.Resources = scene::MixSignature(signatureState.Resources, binding.Owner.Id());
			}
			signatureState.Animation = scene::MixSignature(
				state.Frames[index].Camera.Compiled.Signature(),
				state.Render.TextureAnimationSignature(state.Frames[index].PresentationSeconds)
			);
			const bool shaderClock =
				lighting.ShaderLensCount != 0 ||
				state.Render.PostProcessShaderName(state.ContentOwner).IsValid() ||
				std::any_of(view.Instances.begin(), view.Instances.end(), [](const auto &row) {
					return row.Shader.IsValid();
				});
			if (shaderClock) {
				signatureState.Animation = scene::MixSignature(
					signatureState.Animation, std::bit_cast<uint64_t>(state.Frames[index].PresentationSeconds)
				);
			}
			job.Output.Reply.CaptureTick = tick;
			ENGINE_TRACE(
				"portal image prepared: world {} request {} channel {} tick {} rows {} native-rig rows {} "
				"eye {} camera "
				"{},{},{}",
				state.OwnerName.Text(),
				job.Request.Key.RequestId,
				job.Output.ReplyTo.Channel,
				tick,
				view.Instances.size(),
				std::count_if(
					view.Instances.begin(), view.Instances.end(), [](const auto &row) { return row.Rig != 0; }
				),
				job.Request.EyePlayer,
				view.CameraFrame.Position.X,
				view.CameraFrame.Position.Y,
				view.CameraFrame.Position.Z
			);
			job.Output.Reply.ContentRevision = ScenePresentationSignature(view, signatureState);
			for (unsigned char byte : job.Request.RetainedBodyPlayer)
				job.Output.Reply.ContentRevision =
					scene::MixSignature(job.Output.Reply.ContentRevision, byte);
			if (view.DirectionalShadowBounds) {
				const auto &bounds = *view.DirectionalShadowBounds;
				for (float component :
					 {bounds.Minimum.X,
					  bounds.Minimum.Y,
					  bounds.Minimum.Z,
					  bounds.Maximum.X,
					  bounds.Maximum.Y,
					  bounds.Maximum.Z})
					job.Output.Reply.ContentRevision = scene::MixSignature(
						job.Output.Reply.ContentRevision, std::bit_cast<uint32_t>(component)
					);
			}
			// A complete capture can contain only spatial UI. The scene signature
			// excludes that layer, but its images still depend on residency and time.
			job.Output.Reply.ContentRevision =
				scene::MixSignature(job.Output.Reply.ContentRevision, signatureState.Resources);
			job.Output.Reply.ContentRevision =
				scene::MixSignature(job.Output.Reply.ContentRevision, signatureState.Animation);
			for (const auto &child : job.Children) {
				job.Output.Reply.ContentRevision =
					scene::MixSignature(job.Output.Reply.ContentRevision, child.Version.ContentRevision);
				job.Output.Reply.ContentRevision =
					scene::MixSignature(job.Output.Reply.ContentRevision, child.Version.LightingRevision);
			}
			// These are process-local presentation signatures, not serialized ECS identities.
			// The outer endpoint incarnation scopes them to this producer.
			job.Output.Reply.LightingRevision =
				LightingPresentationSignature(lighting, state.Frames[index].Camera.Lights);
			job.Output.Reply.CaptureLighting = PortalCaptureLighting{
				{lighting.Direction.X, lighting.Direction.Y, lighting.Direction.Z},
				{lighting.Ambient.R, lighting.Ambient.G, lighting.Ambient.B},
				{lighting.OutdoorAmbient.R, lighting.OutdoorAmbient.G, lighting.OutdoorAmbient.B},
				{lighting.Direct.R, lighting.Direct.G, lighting.Direct.B}
			};
			static_assert(MAX_PORTAL_CAPTURE_LIGHTS == MAX_SCENE_LIGHTS);
			auto &capturedLighting = *job.Output.Reply.CaptureLighting;
			capturedLighting.FogColour = {lighting.FogColor.R, lighting.FogColor.G, lighting.FogColor.B};
			capturedLighting.FogStart = lighting.FogStart;
			capturedLighting.FogEnd = lighting.FogEnd;
			capturedLighting.LightCount =
				static_cast<uint8_t>(std::min(view.Lights.size(), MAX_SCENE_LIGHTS));
			for (size_t lightIndex = 0; lightIndex < capturedLighting.LightCount; ++lightIndex) {
				const auto &light = view.Lights[lightIndex];
				capturedLighting.Lights[lightIndex] = {
					{light.Position.X, light.Position.Y, light.Position.Z},
					light.Range,
					{light.Colour.R, light.Colour.G, light.Colour.B},
					{light.Direction.X, light.Direction.Y, light.Direction.Z},
					light.ConeCosine
				};
			}
			if (!ResolvePortalCaptureLighting(capturedLighting, view, state.Frames[index].Camera.Lights)) {
				job.Output.Reply.Status = PortalImageStatus::Failed;
				job.Output.Reply.Diagnostic = "invalid destination capture lighting";
				state.SendFailure(job.Output, progress);
				continue;
			}
			if (job.Request.Scope == PortalImageScope::SeamRadiance) {
				view.Lighting.Ambient = {};
				view.Lighting.OutdoorAmbient = {};
				view.Lighting.Direct = {};
				view.Lighting.FogColor = {};
				view.Lighting.FogStart = 1.0e6f;
				view.Lighting.FogEnd = 1.0e6f + 1.0f;
			}
			if (job.Request.OrderedLayers && lighting.ShaderLensCount != 0) {
				static_assert(MAX_PORTAL_CAPTURE_LENSES == scene::MAX_SCENE_SHADER_LENSES);
				auto &capturedLenses = job.Output.Lenses;
				capturedLenses.TimeSeconds = static_cast<float>(state.Frames[index].PresentationSeconds);
				bool programsAvailable = true;
				size_t programBytes = 0;
				for (size_t lensIndex = 0; lensIndex < lighting.ShaderLensCount; ++lensIndex) {
					const auto &lens = lighting.ShaderLenses[lensIndex];
					const auto hash = state.Render.LensShaderHash(lens.Shader, state.ContentOwner);
					const auto *program = state.Shaders->FindLens(lens.Shader, state.ContentOwner);
					if (!program || hash.IsZero() || program->CodeHash != hash || program->SpirV.empty()) {
						programsAvailable = false;
						break;
					}
					const bool capturedProgram = std::any_of(
						capturedLenses.Programs.begin(),
						capturedLenses.Programs.end(),
						[&](const auto &entry) { return entry.Hash == hash; }
					);
					if (!capturedProgram) {
						if (program->SpirV.size() >
							(MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES - programBytes) / sizeof(uint32_t)) {
							programsAvailable = false;
							break;
						}
						programBytes += program->SpirV.size() * sizeof(uint32_t);
						capturedLenses.Programs.push_back({hash, program->SpirV});
					}
					capturedLenses.Entries.push_back(
						PortalCaptureLens{
							.Position = {lens.Frame.Position.X, lens.Frame.Position.Y, lens.Frame.Position.Z},
							.Orientation =
								{lens.Frame.QuaternionX,
								 lens.Frame.QuaternionY,
								 lens.Frame.QuaternionZ,
								 lens.Frame.QuaternionW},
							.Shader = std::string(lens.Shader.Text()),
							.ProgramHash = hash,
							.Radius = lens.Radius,
							.InnerRadius = lens.InnerRadius,
							.Falloff = lens.Falloff,
							.Strength = lens.Strength,
							.Spin = lens.Spin,
							.Priority = lens.Priority,
							.Shape = static_cast<uint8_t>(lens.Shape)
						}
					);
				}
				if (!programsAvailable || !ValidPortalCaptureLenses(capturedLenses)) {
					job.Output.Reply.Status = PortalImageStatus::Unavailable;
					job.Output.Reply.Diagnostic = "destination lens program or parameters unavailable";
					state.SendFailure(job.Output, progress);
					continue;
				}
			}
			state.RetainContentWorlds(job.Output);
			if (!job.Output.Tree) {
				bool complete = true;
				for (const auto &child : job.Children) {
					if (child.Demand.Request.RecursionDepth != 0 && !child.Tree) complete = false;
					Impl::RetainWorld(job.Output, child.Producer.World);
					Impl::RetainWorld(job.Output, child.Demand.DestinationWorld.Text());
					if (child.Demand.Request.Entrance)
						Impl::RetainWorld(job.Output, child.Demand.Request.Entrance->SourceWorld);
					if (child.Tree)
						for (const auto &node : child.Tree->Nodes)
							Impl::RetainWorld(job.Output, node.Producer.World);
				}
				job.Output.WorldProvenanceComplete = complete;
			}
			if (job.Request.KnownImage &&
				job.Request.KnownImage->ContentRevision == job.Output.Reply.ContentRevision &&
				job.Request.KnownImage->LightingRevision == job.Output.Reply.LightingRevision) {
				const PortalResidentReceipt receipt{
					job.Request.Key,
					job.Request.Scope,
					tick,
					job.Output.Reply.ContentRevision,
					job.Output.Reply.LightingRevision,
					job.Request.Width,
					job.Request.Height,
					job.Output.Reply.CaptureLighting
				};
				std::string error;
				if (EncodePortalImageRenewal(receipt, job.Output.Wire, error)) {
					++progress.Reused;
					job.KeepChildren = true;
					core::Metrics::Count("render.portal_images.renewed", 1);
					const auto sent = state.Send(job.Output, progress);
					if (sent == world::PresentationStatus::Full)
						state.Captures.push_back(std::move(job.Output));
					continue;
				}
			}
			const bool resident =
				!job.Request.OrderedLayers && state.Resident != nullptr &&
				state.Resident->Contains(job.Output.ReplyTo, state.Requests, job.Request, now);
			if (job.Request.OrderedLayers) {
				std::array nodes{
					CaptureNode(),
					LayerCaptureNode(0),
					LayerCaptureNode(1),
					LayerCaptureNode(2),
					core::Name("portal-overlay-export"),
					ShadowCaptureNode()
				};
				const bool overlay = interface && interface->HasWorldOverlay();
				const bool shadow = !job.Request.RetainedBodyPlayer.empty();
				const size_t colourCount = overlay ? 5 : 4;
				const size_t count = colourCount + shadow;
				if (!overlay) nodes[4] = ShadowCaptureNode();
				uint64_t chargedPixels = uint64_t(job.Request.Width) * job.Request.Height * colourCount;
				for (const auto &child : job.Children)
					chargedPixels += child.Demand.Request.PixelBudget;
				if (chargedPixels > job.Request.PixelBudget) {
					job.Output.Reply.Status = PortalImageStatus::BudgetExceeded;
					job.Output.Reply.Diagnostic = "destination spatial overlay exceeds capture pixel budget";
					state.SendFailure(job.Output, progress);
					continue;
				}
				if (shadow && !state.ReserveRoutes(job.Output)) {
					job.Output.Reply.Status = PortalImageStatus::BudgetExceeded;
					job.Output.Reply.Diagnostic = "source shadow route budget or identity refused capture";
					state.SendFailure(job.Output, progress);
					continue;
				}
				if (job.Output.Tree) {
					auto &tree = *job.Output.Tree;
					for (auto &child : job.Children) {
						if (!child.Tree ||
							child.Tree->Nodes.size() > MAX_PORTAL_CAPTURE_TREE_NODES - tree.Nodes.size()) {
							job.Failure = "nested capture node budget exceeded";
							job.FailureStatus = PortalImageStatus::BudgetExceeded;
							break;
						}
						for (const auto &node : child.Tree->Nodes) {
							const auto &producer = node.Producer;
							if (!CurrentPublishedEndpoint(
									state.Universe,
									state.EndpointBindings,
									{producer.World, producer.Channel, producer.Session, producer.Generation}
								)) {
								job.Failure = "nested producer endpoint retired before capture";
								break;
							}
						}
						if (!job.Failure.empty()) break;
						if (shadow &&
							!state.AppendRoutes(job.Output, child, state.Pending[job.Slot].Replies)) {
							job.Failure = "nested source shadow route identity refused capture";
							break;
						}
						const auto offset = static_cast<uint8_t>(tree.Nodes.size());
						child.Edge.Parent = 0;
						child.Edge.Child = offset;
						tree.Edges.push_back(std::move(child.Edge));
						for (auto &node : child.Tree->Nodes)
							tree.Nodes.push_back(std::move(node));
						for (auto &edge : child.Tree->Edges) {
							edge.Parent += offset;
							edge.Child += offset;
							tree.Edges.push_back(std::move(edge));
						}
					}
					if (!job.Failure.empty()) {
						job.Output.Reply.Status = job.FailureStatus;
						job.Output.Reply.Diagnostic = job.Failure;
						state.SendFailure(job.Output, progress);
						continue;
					}
				}
				if (shadow && !state.ReserveShadow(job.Output, view)) {
					job.Output.Reply.Status = PortalImageStatus::BudgetExceeded;
					job.Output.Reply.Diagnostic =
						"source shadow retention budget or identity refused capture";
					state.SendFailure(job.Output, progress);
					continue;
				}
				std::array<uint64_t, 6> tokens{};
				if (state.Render.QueueResourceImages(
						view.Pipeline,
						std::span(nodes).first(count),
						view.Slot,
						ResourceImageDelivery::CopiedPixels,
						std::span(tokens).first(count)
					)) {
					job.Output.Token = tokens[0];
					std::copy_n(
						tokens.begin() + 1, job.Output.LayerTokens.size(), job.Output.LayerTokens.begin()
					);
					job.Output.OverlayToken = overlay ? tokens[4] : 0;
					job.Output.ShadowToken = shadow ? tokens[colourCount] : 0;
				}
			} else {
				job.Output.Token = state.Render.QueueResourceImage(
					view.Pipeline,
					CaptureNode(),
					view.Slot,
					resident ? ResourceImageDelivery::Resident : ResourceImageDelivery::CopiedPixels
				);
			}
			if (job.Output.Token == 0) {
				job.Output.Reply.Status = PortalImageStatus::BudgetExceeded;
				job.Output.Reply.Diagnostic = "renderer image export queue refused capture";
				state.SendFailure(job.Output, progress);
				continue;
			}
			if (state.EyeDiagnostic && job.Request.Key.PortalKey == "viewport-eye" &&
				state.EyeDiagnosticCount++ < 64)
				ENGINE_WARN(
					"portal eye producer queued world={} request={} token={} rows={} eye_rig={}",
					state.Requests.World,
					job.Request.Key.RequestId,
					job.Output.Token,
					view.Instances.size(),
					view.EyeRig
				);
			OverlayImage overlay;
			state.Render.SetAnimationTime(state.Frames[index].PresentationSeconds);
			const auto rendered = state.Render.RenderViews(std::span(&view, 1), overlay, interface, false);
			if (rendered.SurfaceBudgetExceeded) {
				state.Cancel(job.Output);
				job.Output.Reply.Status = PortalImageStatus::BudgetExceeded;
				job.Output.Reply.Diagnostic = "visible destination surfaces exceeded capture budget";
				state.SendFailure(job.Output, progress);
				continue;
			}
			++progress.Rendered;
			job.KeepChildren = true;
			if (resident) {
				const PortalResidentReceipt receipt{
					job.Request.Key,
					job.Request.Scope,
					tick,
					job.Output.Reply.ContentRevision,
					job.Output.Reply.LightingRevision,
					job.Request.Width,
					job.Request.Height,
					job.Output.Reply.CaptureLighting
				};
				std::string error;
				if (!EncodePortalResidentReceipt(receipt, job.Output.Wire, error) ||
					!state.Resident->Publish(
						job.Output.ReplyTo, state.Requests, receipt, job.Output.Token, now
					)) {
					state.Render.CancelResourceImage(job.Output.Token);
					job.Output.Wire.clear();
					job.Output.Reply.Status = PortalImageStatus::Failed;
					job.Output.Reply.Diagnostic = "resident capture receipt refused";
				}
				job.Output.Token = 0;
				const auto sent = state.Send(job.Output, progress);
				if (sent != world::PresentationStatus::Full) {
					continue;
				}
			}
			state.Captures.push_back(std::move(job.Output));
		}
		for (const auto *job : jobs) {
			if (!job->Waiting) state.Finish(job->Slot, job->KeepChildren);
		}
		return progress;
	}
}

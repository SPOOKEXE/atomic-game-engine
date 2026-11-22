// Capture camera and lighting resolution, and PortalImageSource issue, lookup, pinning and invalidation.
#include "portal/PortalImageRuntimeInternal.hpp"

namespace engine::render {
	bool ResolvePortalCaptureCamera(
		const PortalCaptureCamera &camera, PortalImageProjection projection, View &view
	) {
		const auto finite = [](const auto &values) {
			return std::all_of(values.begin(), values.end(), [](float value) {
				return std::isfinite(value);
			});
		};
		const auto unit = [](std::span<const float> values) {
			double norm = 0;
			for (const float value : values)
				norm += double(value) * value;
			return std::abs(norm - 1) <= .001;
		};
		const auto &f = camera.Frustum;
		if ((projection != PortalImageProjection::Eye && projection != PortalImageProjection::Seam) ||
			!finite(camera.Position) || !finite(camera.Orientation) || !unit(camera.Orientation) ||
			!finite(f) || f[0] >= f[1] || f[2] >= f[3] || f[4] <= 0 || f[5] <= f[4] ||
			!finite(camera.ClipPlane))
			return false;
		if (projection == PortalImageProjection::Eye) {
			if (camera.ClipPlane != std::array<float, 4>{}) return false;
		} else if (!unit(std::span(camera.ClipPlane).first<3>()))
			return false;
		View resolved;
		if (!CameraOf(camera, projection, resolved)) return false;
		view.CameraFrame = resolved.CameraFrame;
		view.Camera = resolved.Camera;
		view.Projection = resolved.Projection;
		return true;
	}
	bool ResolvePortalCaptureLighting(
		const PortalCaptureLighting &lighting, View &view, std::span<SceneLight> storage
	) {
		if (!ValidPortalCaptureLighting(lighting) || storage.size() < lighting.LightCount) return false;
		const auto vector = [](const std::array<float, 3> &value) {
			return core::Vector3{value[0], value[1], value[2]};
		};
		const auto colour = [](const std::array<float, 3> &value) {
			return core::Color3{value[0], value[1], value[2]};
		};
		for (size_t index = 0; index < lighting.LightCount; ++index) {
			const auto &light = lighting.Lights[index];
			storage[index] = {
				vector(light.Position),
				light.Range,
				colour(light.Colour),
				vector(light.Direction),
				light.ConeCosine
			};
		}
		view.Lighting.Direction = vector(lighting.Direction);
		view.Lighting.Ambient = colour(lighting.Ambient);
		view.Lighting.OutdoorAmbient = colour(lighting.OutdoorAmbient);
		view.Lighting.Direct = colour(lighting.Direct);
		view.Lighting.FogColor = colour(lighting.FogColour);
		view.Lighting.FogStart = lighting.FogStart;
		view.Lighting.FogEnd = lighting.FogEnd;
		view.Lights = storage.first(lighting.LightCount);
		view.OverrideLighting = true;
		return true;
	}
	core::Name PortalReplyChannel(size_t viewSlot) {
		return core::Name(
			viewSlot == 0 ? std::string(PORTAL_REPLY_CHANNEL)
						  : std::string(PORTAL_REPLY_CHANNEL) + '/' + std::to_string(viewSlot)
		);
	}
	PortalImageSource::PortalImageSource(
		world::Universe &universe,
		Renderer &renderer,
		world::WorldId world,
		world::PresentationAddress replies,
		PortalInboxLimits limits,
		PortalResidentImages *resident,
		PortalImageSourceDelivery delivery
	)
		: State(
			  std::make_unique<Impl>(
				  universe, renderer, world, std::move(replies), limits, resident, delivery
			  )
		  ) {}
	PortalImageSource::~PortalImageSource() {
		Clear();
	}
	void PortalImageSource::SetEndpointBindings(const world::PresentationBindings *bindings) {
		if (State->EndpointBindings == bindings) return;
		Clear();
		State->EndpointBindings = bindings;
	}
	PortalRuntimeIssue PortalImageSource::Issue(
		const world::PresentationAddress &producer,
		PortalImageRequest request,
		PortalImageBinding binding,
		Time now
	) {
		auto &state = *State;
		PortalRuntimeIssue result;
		if (request.Geometry.size() > MAX_PORTAL_GEOMETRY_BYTES || !state.ViewSlot ||
			(state.Delivery == PortalImageSourceDelivery::CapturePayloads && !request.OrderedLayers) ||
			binding.ViewSlot != *state.ViewSlot ||
			(state.Resident != nullptr && !state.Render.OwnsResident(*state.Resident)) ||
			!Local(state.Universe, state.World, state.Replies) ||
			producer.Channel != PORTAL_REQUEST_CHANNEL || binding.World != state.World.Index ||
			binding.WorldName.Text() != state.Replies.World ||
			binding.Portal.Text() != request.Key.PortalKey ||
			!Clock(state.LastTime, now, state.Limits.Timeout)) {
			return result;
		}
		const PortalImageRequest rawRequest = request;
		state.Expire(now);
		if (request.OrderedLayers)
			request.Key.SeamRevision = scene::MixSignature(request.Key.SeamRevision, 0x4c4159455253ULL);
		if (request.Projection != PortalImageProjection::Seam)
			request.Key.CameraRevision =
				scene::MixSignature(request.Key.CameraRevision, uint8_t(request.Projection));
		if (request.Entrance) {
			request.Key.SeamRevision = scene::MixSignature(request.Key.SeamRevision, 1);
			for (const auto byte : request.Entrance->SourceWorld)
				request.Key.SeamRevision = scene::MixSignature(request.Key.SeamRevision, uint8_t(byte));
			for (const auto &axis :
				 {request.Entrance->Centre, request.Entrance->First, request.Entrance->Second})
				for (const float value : axis)
					request.Key.SeamRevision =
						scene::MixSignature(request.Key.SeamRevision, std::bit_cast<uint32_t>(value));
		}
		for (const unsigned char byte : request.EyePlayer)
			request.Key.SeamRevision = scene::MixSignature(request.Key.SeamRevision, byte);
		if (!request.RetainedBodyPlayer.empty()) {
			request.Key.SeamRevision =
				scene::MixSignature(request.Key.SeamRevision, request.RetainedBodyPlayer.size());
			for (const unsigned char byte : request.RetainedBodyPlayer)
				request.Key.SeamRevision = scene::MixSignature(request.Key.SeamRevision, byte);
		}
		if (!request.Geometry.empty()) {
			const auto hash = assets::Hasher::Of(request.Geometry);
			for (const auto byte : hash.Digest) {
				request.Key.CameraRevision = scene::MixSignature(request.Key.CameraRevision, byte);
			}
		}
		auto existing =
			std::find_if(state.Previews.begin(), state.Previews.end(), [&](const Impl::Preview &preview) {
				return preview.Binding.Portal == binding.Portal;
			});
		// A handoff eye may arrive after this viewport already issued a routine
		// request. Replace that local correlation so the producer can prioritize its
		// reply. A late routine reply is ignored by the new expected request id.
		if (existing != state.Previews.end() && existing->Pending != 0 && request.TransferEye &&
			!existing->PendingRequest.TransferEye && existing->Producer == producer &&
			state.SameBindingProfile(existing->Binding, binding)) {
			if (state.Resident != nullptr) state.Resident->Cancel(state.Replies, existing->Pending);
			state.Inbox.CancelRequest(existing->Pending);
			existing->Pending = 0;
			existing->Deferred.reset();
		}
		// Keep the current request alive, but retain only the latest normalized
		// demand. Geometry contributes to CameraRevision above, before this decision.
		if (existing != state.Previews.end() && existing->Pending != 0 && existing->Producer == producer &&
			existing->OrderedLayers == request.OrderedLayers && existing->EyePlayer == request.EyePlayer &&
			existing->RetainedBodyPlayer == request.RetainedBodyPlayer &&
			state.SameBindingProfile(existing->Binding, binding) &&
			existing->Binding.Expected.SeamRevision == request.Key.SeamRevision &&
			existing->Binding.ExpectedScope == request.Scope &&
			existing->Binding.ExpectedProjection == request.Projection) {
			if (state.SameRequestDemand(existing->PendingRequest, request) &&
				state.SameBindingDemand(existing->Binding, binding)) {
				// The request already in flight is the most recent demand, so a
				// previously retained movement no longer needs a follow-up capture.
				existing->Deferred.reset();
			} else if (!existing->Deferred ||
					   !state.SameRequestDemand(existing->Deferred->Normalized, request) ||
					   !state.SameBindingDemand(existing->Deferred->Binding, binding)) {
				existing->Deferred = Impl::Demand{
					rawRequest, std::move(request), std::move(binding), now + state.Limits.Timeout
				};
			}
			result.Status = PortalInboxStatus::Busy;
			return result;
		}
		if (existing == state.Previews.end() && state.Previews.size() >= MAX_IMPORTED_PORTAL_IMAGES) {
			result.Status = PortalInboxStatus::Full;
			return result;
		}
		request.KnownImage.reset();
		if (!request.OrderedLayers && existing != state.Previews.end() &&
			existing->TransparentImages[0] == 0 && existing->Handle != 0 && existing->ImageVersion &&
			existing->Producer == producer && existing->Binding.Index == binding.Index &&
			existing->Binding.Sampling == binding.Sampling &&
			existing->ImageVersion->Key.CameraRevision == request.Key.CameraRevision &&
			existing->ImageVersion->Key.SeamRevision == request.Key.SeamRevision &&
			existing->ImageVersion->Scope == request.Scope &&
			existing->ImageVersion->Width == request.Width &&
			existing->ImageVersion->Height == request.Height) {
			request.KnownImage = PortalImageVersion{
				existing->ImageVersion->ContentRevision, existing->ImageVersion->LightingRevision
			};
		}
		auto issued = state.Inbox.Issue(Borrow(state.Replies), Borrow(producer), std::move(request), now);
		result.Status = issued.Status;
		if (issued.Status != PortalInboxStatus::Issued) {
			return result;
		}
		result.RequestId = issued.Request.Key.RequestId;
		binding.Expected = issued.Request.Key;
		binding.ExpectedScope = issued.Request.Scope;
		binding.ExpectedProjection = issued.Request.Projection;
		ENGINE_TRACE(
			"portal image issued: {} {} request {} geometry-bytes {} eye {}",
			state.Replies.Channel,
			producer.World,
			result.RequestId,
			issued.Request.Geometry.size(),
			issued.Request.EyePlayer
		);
		if (!issued.Request.OrderedLayers && state.Resident != nullptr &&
			!state.Resident->Reserve(state.Replies, producer, issued.Request, binding, now)) {
			state.Inbox.CancelRequest(result.RequestId);
			result.Status = PortalInboxStatus::Full;
			return result;
		}
		std::array<std::string_view, 1> referencedWorlds;
		size_t referenceCount = 0;
		if (issued.Request.Entrance)
			referencedWorlds[referenceCount++] = issued.Request.Entrance->SourceWorld;
		result.Transport = state.Universe.SendPresentation(
			state.World,
			state.Replies,
			producer,
			result.RequestId,
			issued.Wire,
			world::PresentationPriority::Routine,
			std::span(referencedWorlds).first(referenceCount)
		);
		if (result.Transport != world::PresentationStatus::Ok) {
			if (state.Resident != nullptr) {
				state.Resident->Cancel(state.Replies, result.RequestId);
			}
			state.Inbox.CancelRequest(result.RequestId);
			result.Status = PortalInboxStatus::Invalid;
			return result;
		}
		binding.Expected = issued.Request.Key;
		binding.ExpectedScope = issued.Request.Scope;
		binding.ExpectedProjection = issued.Request.Projection;
		Impl::Preview preview{};
		preview.Producer = producer;
		preview.Binding = std::move(binding);
		preview.Pending = result.RequestId;
		preview.PendingDeadline = now + state.Limits.Timeout;
		preview.OfferedVersion = issued.Request.KnownImage;
		preview.EyePlayer = issued.Request.EyePlayer;
		preview.PendingRequest = issued.Request;
		preview.OrderedLayers = issued.Request.OrderedLayers;
		preview.RetainedBodyPlayer = issued.Request.RetainedBodyPlayer;
		const auto published = PublishedEndpoint(state.EndpointBindings, Borrow(producer));
		preview.ExpectedProducer = {
			std::string(published.World),
			std::string(published.Channel),
			published.Session,
			published.Generation
		};
		preview.PendingCamera = {
			issued.Request.Position,
			issued.Request.Orientation,
			issued.Request.Frustum,
			issued.Request.ClipPlane
		};
		if (existing == state.Previews.end()) {
			state.Previews.push_back(std::move(preview));
		} else {
			state.CancelUpload(*existing);
			if (state.Resident != nullptr) {
				state.Resident->Cancel(state.Replies, existing->Pending);
			}
			// The rendered handle belongs to this renderer, rather than the producer
			// endpoint. Keep it while a route changes producer so a complete portal
			// frame remains drawable until the replacement reply lands. Endpoint
			// invalidation still retires it when its old producer is no longer valid.
			if (existing->Binding.World == preview.Binding.World &&
				existing->Binding.ViewSlot == preview.Binding.ViewSlot &&
				existing->Binding.Index == preview.Binding.Index) {
				preview.Handle = existing->Handle;
				preview.Tree = existing->Tree;
				preview.TreeProducers = std::move(existing->TreeProducers);
				existing->Tree = 0;
				preview.TransparentImages = existing->TransparentImages;
				preview.SpatialOverlayImage = existing->SpatialOverlayImage;
				preview.CapturedLenses = std::move(existing->CapturedLenses);
				preview.LensPrograms = existing->LensPrograms;
				existing->LensPrograms = 0;
				preview.ImageDeadline = existing->ImageDeadline;
				preview.AcceptedAt = existing->AcceptedAt;
				preview.CapturePinDeadline = existing->CapturePinDeadline;
				preview.CapturePinned = existing->CapturePinned;
				preview.CaptureLease = existing->CaptureLease;
				existing->CaptureLease = 0;
				preview.ImageVersion = existing->ImageVersion;
				preview.CapturedBinding = std::move(existing->CapturedBinding);
				preview.CapturedCamera = existing->CapturedCamera;
				preview.CapturedProducer = existing->CapturedProducer;
				preview.CapturedEyePlayer = std::move(existing->CapturedEyePlayer);
				preview.CapturedEntranceWorld = std::move(existing->CapturedEntranceWorld);
				preview.CapturedWorldProvenanceKnown = existing->CapturedWorldProvenanceKnown;
				preview.CapturedRetainedBodyPlayer = std::move(existing->CapturedRetainedBodyPlayer);
			} else {
				state.Drop(*existing);
			}
			*existing = std::move(preview);
		}
		return result;
	}
	std::optional<world::PresentationMessage> PortalImageSource::TakeShadowReply() {
		if (!State->ShadowReply) return {};
		const auto &target = State->ShadowReplyTarget;
		if (State->Universe.LookupPresentation(State->World, State->Replies.Channel) != State->Replies ||
			!CurrentEndpoint(State->Universe, Borrow(State->ShadowReply->From)) ||
			(State->ShadowReplyTargetWasCurrent &&
			 !State->EndpointCurrent({target.World, target.Channel, target.Session, target.Generation}))) {
			State->ShadowReply.reset();
			State->ShadowReplyTarget = {};
			State->ShadowReplyTargetWasCurrent = false;
			return {};
		}
		auto reply = std::move(State->ShadowReply);
		State->ShadowReply.reset();
		State->ShadowReplyTarget = {};
		State->ShadowReplyTargetWasCurrent = false;
		return reply;
	}
	std::optional<PortalCaptureTree> PortalImageSource::TakeTree(std::string_view portal, Time now) {
		auto &state = *State;
		if (state.Delivery != PortalImageSourceDelivery::CapturePayloads ||
			!Clock(state.LastTime, now, state.Limits.Timeout))
			return {};
		if (!Local(state.Universe, state.World, state.Replies) ||
			state.Universe.LookupPresentation(state.World, state.Replies.Channel) != state.Replies) {
			Clear();
			return {};
		}
		state.Expire(now);
		const auto preview =
			std::find_if(state.Previews.begin(), state.Previews.end(), [&](const auto &entry) {
				return entry.Binding.Portal.Text() == portal && entry.PayloadReady && entry.Pending == 0;
			});
		if (preview == state.Previews.end()) return {};
		const world::PresentationAddress producer = preview->Producer;
		const std::optional<Impl::Demand> deferred = preview->Deferred;
		auto tree = state.Inbox.TakeTree(Borrow(state.Replies), Borrow(preview->Producer), portal, now);
		if (!tree) {
			auto layers =
				state.Inbox.TakeLayers(Borrow(state.Replies), Borrow(preview->Producer), portal, now);
			if (layers) {
				const auto producer = Borrow(preview->ExpectedProducer);
				const auto &camera = preview->CapturedCamera;
				tree.emplace();
				tree->Nodes.push_back(
					{{std::string(producer.World),
					  std::string(producer.Channel),
					  producer.Session,
					  producer.Generation},
					 {camera.Position,
					  camera.Orientation,
					  camera.Frustum,
					  camera.ClipPlane,
					  preview->CapturedBinding.ExpectedProjection},
					 std::move(*layers),
					 preview->CapturedRetainedBodyPlayer}
				);
			}
		}
		if (!tree) {
			state.Previews.erase(preview);
			return {};
		}
		if (!ValidPortalCaptureTree(*tree)) {
			state.Previews.erase(preview);
			return {};
		}
		for (const auto &node : tree->Nodes) {
			const auto &nodeProducer = node.Producer;
			if (!state.EndpointCurrent(
					{nodeProducer.World, nodeProducer.Channel, nodeProducer.Session, nodeProducer.Generation}
				)) {
				state.Previews.erase(preview);
				return {};
			}
		}
		if (!deferred || now >= deferred->Deadline) {
			state.Previews.erase(preview);
			return tree;
		}
		preview->PayloadReady = false;
		const PortalRuntimeIssue issued = Issue(producer, deferred->Request, deferred->Binding, now);
		if (issued.Status == PortalInboxStatus::Issued || issued.Status == PortalInboxStatus::Full ||
			issued.Transport == world::PresentationStatus::Full)
			return tree;
		preview->Deferred.reset();
		state.Previews.erase(preview);
		return tree;
	}
	bool PortalImageSource::HasPendingUploads() const {
		return std::any_of(State->Previews.begin(), State->Previews.end(), [](const Impl::Preview &preview) {
			return preview.UploadImages[0] != 0 || preview.UploadTree != 0;
		});
	}

	uint64_t PortalImageSource::Image(std::string_view portal) const {
		for (const auto &preview : State->Previews) {
			if (preview.Binding.Portal.Text() == portal) {
				if (State->EyeDiagnostic && State->ViewSlot == 1 && portal == "viewport-eye" &&
					State->EyeDiagnosticCount++ < 64)
					ENGINE_WARN(
						"portal eye resident image slot=1 pending={} handle={} image_request={} "
						"expected_request={} deferred={}",
						preview.Pending,
						preview.Handle,
						preview.ImageVersion ? preview.ImageVersion->Key.RequestId : 0,
						preview.Binding.Expected.RequestId,
						preview.Deferred.has_value()
					);
				return preview.Handle;
			}
		}
		if (State->EyeDiagnostic && State->ViewSlot == 1 && portal == "viewport-eye" &&
			State->EyeDiagnosticCount++ < 64)
			ENGINE_WARN("portal eye resident image slot=1 preview-missing");
		return 0;
	}
	uint64_t PortalImageSource::CurrentImage(std::string_view portal) const {
		for (const auto &preview : State->Previews) {
			if (preview.Binding.Portal.Text() == portal && preview.Pending == 0 && preview.ImageVersion &&
				preview.ImageVersion->Key == preview.Binding.Expected)
				return preview.Handle;
		}
		return 0;
	}
	PortalInboxUsage PortalImageSource::InboxUsage() const {
		return State->Inbox.Usage();
	}
	bool PortalImageSource::RenameIdle() const {
		const auto usage = State->Inbox.Usage();
		if (usage.PendingCount != 0 || usage.HeldCount != 0 || State->ShadowReply ||
			!State->PinnedCaptures.empty())
			return false;
		for (const auto &preview : State->Previews) {
			if (preview.Pending != 0 || preview.Deferred || preview.PayloadReady || preview.CapturePinned ||
				preview.CaptureLease != 0 || preview.UploadTree != 0 || preview.UploadVersion ||
				preview.UploadLensPrograms != 0 ||
				std::any_of(preview.UploadImages.begin(), preview.UploadImages.end(), [](uint64_t id) {
					return id != 0;
				}))
				return false;
		}
		return true;
	}
	bool PortalImageSource::RenameIdleForUnrelatedRoute(std::string_view world) const {
		const auto usage = State->Inbox.Usage();
		if (ReferencesWorld(world)) return false;
		size_t pending = 0;
		for (const auto &preview : State->Previews) {
			if ((preview.Handle && !preview.CapturedWorldProvenanceKnown) ||
				(preview.Tree && preview.TreeProducers.empty()) ||
				(preview.UploadTree && preview.UploadTreeProducers.empty()))
				return false;
			if (!preview.Pending) continue;
			const auto &request = preview.PendingRequest;
			const bool uploading =
				preview.UploadTree || preview.UploadVersion || preview.UploadLensPrograms ||
				std::any_of(preview.UploadImages.begin(), preview.UploadImages.end(), [](uint64_t id) {
					return id != 0;
				});
			if ((request.RecursionDepth != 0 && !preview.UploadTree) ||
				(request.Entrance && request.Entrance->SourceWorld == world))
				return false;
			if (!uploading) ++pending;
		}
		return pending == usage.PendingCount;
	}
	bool PortalImageSource::ReferencesWorld(std::string_view world) const {
		if (State->Replies.World == world || State->Inbox.ReferencesWorld(world)) return true;
		if (State->EndpointBindings)
			for (const auto *pairs : {&State->EndpointBindings->Exports, &State->EndpointBindings->Returns})
				for (const auto &pair : *pairs)
					if (pair.Local.World == world && State->Inbox.ReferencesWorld(pair.Published.World))
						return true;
		if (State->ShadowReply &&
			(State->ShadowReply->From.World == world || State->ShadowReply->To.World == world ||
			 State->ShadowReplyTarget.World == world))
			return true;
		if (State->ShadowReply && State->EndpointBindings)
			for (const auto *pairs : {&State->EndpointBindings->Exports, &State->EndpointBindings->Returns})
				for (const auto &pair : *pairs)
					if (pair.Local.World == world &&
						Borrow(pair.Published) == PortalEndpointView{
													  State->ShadowReplyTarget.World,
													  State->ShadowReplyTarget.Channel,
													  State->ShadowReplyTarget.Session,
													  State->ShadowReplyTarget.Generation
												  })
						return true;
		const auto references = [&](const auto &producers) {
			return std::any_of(producers.begin(), producers.end(), [&](const auto &entry) {
				if (entry.World == world) return true;
				if (State->EndpointBindings)
					for (const auto *pairs :
						 {&State->EndpointBindings->Exports, &State->EndpointBindings->Returns})
						for (const auto &pair : *pairs)
							if (pair.Local.World == world &&
								PortalEndpointView{
									entry.World, entry.Channel, entry.Session, entry.Generation
								} == Borrow(pair.Published))
								return true;
				return false;
			});
		};
		for (const auto &preview : State->Previews) {
			if (preview.Deferred && (preview.Deferred->Request.RecursionDepth != 0 ||
									 preview.Deferred->Binding.WorldName.Text() == world ||
									 (preview.Deferred->Request.Entrance &&
									  preview.Deferred->Request.Entrance->SourceWorld == world)))
				return true;
			if (preview.Producer.World == world || preview.CapturedProducer.World == world ||
				preview.ExpectedProducer.World == world || references(preview.TreeProducers) ||
				references(preview.UploadTreeProducers) ||
				(preview.Handle &&
				 (!preview.CapturedWorldProvenanceKnown || preview.CapturedEntranceWorld == world)) ||
				(preview.Pending && preview.PendingRequest.Entrance &&
				 preview.PendingRequest.Entrance->SourceWorld == world))
				return true;
		}
		for (const auto &capture : State->PinnedCaptures)
			if (!capture.WorldProvenanceKnown || capture.Producer.World == world ||
				capture.EntranceWorld == world || references(capture.Producers))
				return true;
		return false;
	}
	std::optional<PortalImageCapture> PortalImageSource::Capture(std::string_view portal) const {
		for (const auto &preview : State->Previews) {
			if (preview.Binding.Portal.Text() == portal && preview.Handle != 0 && preview.ImageVersion)
				return PortalImageCapture{
					preview.Handle,
					preview.CapturedProducer,
					preview.CapturedBinding,
					preview.CapturedCamera,
					preview.CapturedEyePlayer,
					preview.ImageVersion->Width,
					preview.ImageVersion->Height,
					preview.ImageVersion->CaptureLighting,
					preview.TransparentImages,
					preview.SpatialOverlayImage,
					preview.CapturedLenses,
					preview.LensPrograms,
					preview.Tree,
					preview.CapturedRetainedBodyPlayer,
					preview.AcceptedAt,
					preview.ImageVersion->CaptureTick
				};
		}
		return {};
	}
	std::optional<PortalImageSource::Time> PortalImageSource::PinCapture(
		std::string_view portal,
		uint64_t exactTree,
		const PortalExchangeKey &exactEye,
		Time now,
		Time requestedDeadline
	) {
		auto &state = *State;
		if (exactTree == 0 || requestedDeadline <= now || now > Time::max() - SHADOW_TRANSFER_TIMEOUT ||
			requestedDeadline > now + SHADOW_TRANSFER_TIMEOUT ||
			state.Delivery != PortalImageSourceDelivery::ImportedImages ||
			!Clock(state.LastTime, now, state.Limits.Timeout))
			return {};
		if (!Local(state.Universe, state.World, state.Replies) ||
			state.Universe.LookupPresentation(state.World, state.Replies.Channel) != state.Replies) {
			Clear();
			return {};
		}
		state.Expire(now);
		for (auto &preview : state.Previews) {
			if (preview.Binding.Portal.Text() != portal || preview.Tree != exactTree || preview.Handle == 0 ||
				!preview.ImageVersion || preview.ImageVersion->Key != exactEye ||
				preview.CapturedBinding.Expected != exactEye)
				continue;
			const auto *tree = state.Render.FindPortalCaptureTree(exactTree);
			if (tree == nullptr || tree->Nodes.empty() || tree->Nodes[0].Images[0] != preview.Handle ||
				tree->Nodes[0].Binding.Expected != exactEye)
				return {};
			if (preview.CapturePinDeadline && now >= *preview.CapturePinDeadline) return {};
			if (preview.CaptureLease == 0) {
				preview.CaptureLease = state.Render.AcquirePortalCaptureTreeLease(exactTree);
				if (preview.CaptureLease == 0) return {};
			}
			if (!preview.CapturePinDeadline) preview.CapturePinDeadline = requestedDeadline;
			if (now >= *preview.CapturePinDeadline) return {};
			preview.CapturePinned = true;
			return preview.CapturePinDeadline;
		}
		return {};
	}
	void PortalImageSource::UnpinCapture(uint64_t exactTree) {
		if (exactTree == 0) return;
		for (auto &preview : State->Previews)
			if (preview.Tree == exactTree) State->ReleaseCaptureLease(preview);
		std::erase_if(State->PinnedCaptures, [&](Impl::PinnedCapture &capture) {
			if (capture.Tree != exactTree) return false;
			State->Render.ReleasePortalCaptureTreeLease(capture.Lease);
			return true;
		});
		if (State->LastTime) State->Expire(*State->LastTime);
	}
	void PortalImageSource::InvalidateEndpoint(const world::PresentationAddress &endpoint) {
		State->Inbox.InvalidateEndpoint(Borrow(endpoint));
		const auto published = PublishedEndpoint(State->EndpointBindings, Borrow(endpoint));
		bool invalidatePublished = published != Borrow(endpoint);
		if (invalidatePublished && State->EndpointBindings)
			for (const auto *pairs : {&State->EndpointBindings->Exports, &State->EndpointBindings->Returns})
				for (const auto &pair : *pairs)
					if (Borrow(pair.Published) == published && pair.Local != endpoint &&
						CurrentEndpoint(State->Universe, Borrow(pair.Local)))
						invalidatePublished = false;
		if (invalidatePublished) State->Inbox.InvalidateEndpoint(published);
		const auto matches = [&](const auto &producer) {
			const PortalEndpointView address{
				producer.World, producer.Channel, producer.Session, producer.Generation
			};
			return address == Borrow(endpoint) || (invalidatePublished && address == published);
		};
		if (State->ShadowReply && (State->ShadowReply->From == endpoint ||
								   State->ShadowReply->To == endpoint || matches(State->ShadowReplyTarget))) {
			State->ShadowReply.reset();
			State->ShadowReplyTarget = {};
			State->ShadowReplyTargetWasCurrent = false;
		}
		std::erase_if(State->PinnedCaptures, [&](Impl::PinnedCapture &capture) {
			const auto dependent = capture.Producer == endpoint ||
								   std::any_of(capture.Producers.begin(), capture.Producers.end(), matches);
			if (!dependent) return false;
			State->Render.DropPortalCaptureTree(capture.Tree);
			State->Render.ReleasePortalCaptureTreeLease(capture.Lease);
			return true;
		});
		std::erase_if(State->Previews, [&](Impl::Preview &preview) {
			const auto dependent = [&](const auto &producers) {
				return std::any_of(producers.begin(), producers.end(), matches);
			};
			if (State->Replies == endpoint || preview.Producer == endpoint) {
				State->Drop(preview);
				return true;
			}
			if (preview.Handle != 0 && preview.CapturedProducer == endpoint) State->RetireImage(preview);
			if (dependent(preview.TreeProducers)) State->RetireImage(preview);
			if (dependent(preview.UploadTreeProducers)) State->CancelPending(preview);
			return preview.Pending == 0 && preview.Handle == 0 && !preview.PayloadReady;
		});
	}
	void PortalImageSource::RestartRequests() {
		for (auto &preview : State->Previews) {
			if (preview.Pending == 0) continue;
			State->CancelUpload(preview);
			if (State->Resident) State->Resident->Cancel(State->Replies, preview.Pending);
			State->Inbox.CancelRequest(preview.Pending);
			preview.Pending = 0;
			preview.OfferedVersion.reset();
			preview.Deferred.reset();
		}
	}

	void PortalImageSource::InvalidatePortal(std::string_view portal) {
		State->Inbox.InvalidatePortal(Borrow(State->Replies), portal);
		std::erase_if(State->PinnedCaptures, [&](Impl::PinnedCapture &capture) {
			if (capture.Portal.Text() != portal) return false;
			State->Render.DropPortalCaptureTree(capture.Tree);
			State->Render.ReleasePortalCaptureTreeLease(capture.Lease);
			return true;
		});
		std::erase_if(State->Previews, [&](Impl::Preview &preview) {
			if (preview.Binding.Portal.Text() != portal) {
				return false;
			}
			State->Drop(preview);
			return true;
		});
	}
	void PortalImageSource::Clear() {
		State->ShadowReply.reset();
		State->ShadowReplyTarget = {};
		State->ShadowReplyTargetWasCurrent = false;
		for (auto &preview : State->Previews) {
			State->Drop(preview);
		}
		State->Previews.clear();
		for (const auto &capture : State->PinnedCaptures) {
			State->Render.DropPortalCaptureTree(capture.Tree);
			State->Render.ReleasePortalCaptureTreeLease(capture.Lease);
		}
		State->PinnedCaptures.clear();
		State->Inbox.Clear();
	}
}

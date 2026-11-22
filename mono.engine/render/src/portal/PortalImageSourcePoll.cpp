// PortalImageSource polling: turns inbox replies into runtime completions.
#include "portal/PortalImageRuntimeInternal.hpp"

namespace engine::render {
	std::vector<PortalRuntimeCompletion> PortalImageSource::Poll(Time now) {
		return PollInternal(now, nullptr);
	}
	std::vector<PortalRuntimeCompletion>
	PortalImageSource::PollExpected(Time now, std::span<const PortalShadowReplyExpectation> expected) {
		return PollInternal(now, &expected);
	}
	std::vector<PortalRuntimeCompletion>
	PortalImageSource::PollInternal(Time now, const std::span<const PortalShadowReplyExpectation> *expected) {
		auto &state = *State;
		std::vector<PortalRuntimeCompletion> completions;
		if (!Clock(state.LastTime, now, state.Limits.Timeout)) {
			return completions;
		}
		if (!state.ViewSlot || !Local(state.Universe, state.World, state.Replies)) {
			Clear();
			return completions;
		}
		state.Expire(now);
		const auto expectedReply = [&](const world::PresentationMessage &message,
									   const PortalShadowPull &pull) {
			if (!expected) return true;
			return std::any_of(expected->begin(), expected->end(), [&](const auto &entry) {
				if (entry.Producer != message.From || entry.Requester != message.To ||
					entry.Correlation != message.Correlation)
					return false;
				PortalShadowPull requested;
				std::string error;
				return DecodePortalShadowPull(entry.Pull, requested, error) && requested == pull;
			});
		};
		if (expected && state.ShadowReply) {
			PortalShadowPacket held;
			std::string error;
			if (!DecodePortalShadowPacket(state.ShadowReply->Payload, held, error) ||
				!expectedReply(*state.ShadowReply, held.Pull)) {
				state.ShadowReply.reset();
				state.ShadowReplyTarget = {};
				state.ShadowReplyTargetWasCurrent = false;
			}
		}
		for (auto &preview : state.Previews) {
			if (preview.UploadTree != 0) {
				const auto *tree = state.Render.FindPortalCaptureTree(preview.UploadTree);
				if (!tree) continue;
				const auto &root = tree->Nodes.front();
				state.ReleaseImageOwner(preview);
				state.Render.ReleasePortalLensPrograms(preview.LensPrograms);
				preview.Tree = preview.UploadTree;
				preview.UploadTree = 0;
				preview.TreeProducers = std::move(preview.UploadTreeProducers);
				preview.Handle = root.Images[0];
				preview.TransparentImages = {root.Images[1], root.Images[2]};
				preview.SpatialOverlayImage = root.Images[3];
				preview.CapturedLenses = root.Lenses;
				// Tree ownership includes its program leases.
				preview.LensPrograms = 0;
				preview.ImageVersion = std::move(preview.UploadVersion);
				preview.UploadVersion.reset();
				preview.CaptureAccepted(now);
				preview.ImageDeadline = now + state.Limits.Timeout;
				if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 && *state.ViewSlot <= 3 &&
					state.EyeDiagnosticCount++ < 256)
					ENGINE_WARN(
						"portal eye source completed tree slot={} request={} handle={} capture_tick={}",
						*state.ViewSlot,
						preview.Pending,
						preview.Handle,
						preview.ImageVersion->CaptureTick
					);
				completions.push_back(
					{preview.Pending,
					 PortalImageStatus::Ok,
					 preview.Handle,
					 {},
					 {preview.ImageVersion->ContentRevision, preview.ImageVersion->LightingRevision}}
				);
				preview.Pending = 0;
				continue;
			}
			if (!preview.UploadVersion || !state.Render.PortalImageLayerSetReady(preview.UploadImages[0]))
				continue;
			state.ReleaseImageOwner(preview);
			preview.Tree = 0;
			preview.TreeProducers.clear();
			preview.Handle = preview.UploadImages[0];
			std::copy_n(
				preview.UploadImages.begin() + 1,
				preview.TransparentImages.size(),
				preview.TransparentImages.begin()
			);
			preview.SpatialOverlayImage = preview.UploadImages.back();
			state.Render.ReleasePortalLensPrograms(preview.LensPrograms);
			preview.LensPrograms = preview.UploadLensPrograms;
			preview.UploadLensPrograms = 0;
			preview.CapturedLenses = std::move(preview.UploadLenses);
			preview.UploadLenses = {};
			preview.ImageVersion = std::move(preview.UploadVersion);
			preview.UploadVersion.reset();
			preview.UploadImages = {};
			preview.CaptureAccepted(now);
			preview.ImageDeadline = now + state.Limits.Timeout;
			if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 && *state.ViewSlot <= 3 &&
				state.EyeDiagnosticCount++ < 256)
				ENGINE_WARN(
					"portal eye source completed layers slot={} request={} handle={} capture_tick={}",
					*state.ViewSlot,
					preview.Pending,
					preview.Handle,
					preview.ImageVersion->CaptureTick
				);
			completions.push_back(
				{preview.Pending,
				 PortalImageStatus::Ok,
				 preview.Handle,
				 {},
				 {preview.ImageVersion->ContentRevision, preview.ImageVersion->LightingRevision}}
			);
			preview.Pending = 0;
		}
		for (auto &message : state.Universe.TakePresentation(state.Replies)) {
			if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 && *state.ViewSlot <= 3 &&
				state.EyeDiagnosticCount++ < 256)
				ENGINE_WARN(
					"portal eye source received slot={} correlation={} from={} channel={} session={} "
					"bytes={}",
					*state.ViewSlot,
					message.Correlation,
					message.From.World,
					message.From.Channel,
					message.From.Session,
					message.Payload.size()
				);
			if (IsPortalShadowPacket(message.Payload)) {
				PortalShadowPacket packet;
				std::string error;
				if (!state.ShadowReply && message.Payload.size() <= MAX_PORTAL_EXCHANGE_BYTES &&
					message.Payload.capacity() <= MAX_PORTAL_EXCHANGE_BYTES && message.To == state.Replies &&
					CurrentEndpoint(state.Universe, Borrow(message.From)) &&
					DecodePortalShadowPacket(message.Payload, packet, error)) {
					if (!expectedReply(message, packet.Pull)) continue;
					const auto &target = packet.Pull.TargetProducer;
					const PortalEndpointView endpoint{
						target.World, target.Channel, target.Session, target.Generation
					};
					const bool successful = packet.Status == PortalImageStatus::Ok &&
											packet.Pull.Part != PORTAL_SHADOW_CANCEL_PART;
					const bool known =
						successful &&
						KnownPublishedEndpoint(state.Universe, state.EndpointBindings, endpoint);
					if (known && !state.EndpointCurrent(endpoint)) continue;
					state.ShadowReplyTargetWasCurrent = known;
					state.ShadowReplyTarget = std::move(packet.Pull.TargetProducer);
					state.ShadowReply = std::move(message);
				}
				continue;
			}
			PortalResidentReceipt renewed;
			std::string renewalError;
			if (DecodePortalImageRenewal(message.Payload, renewed, renewalError)) {
				for (auto &preview : state.Previews) {
					if (preview.OrderedLayers || preview.Pending == 0 ||
						preview.Pending != message.Correlation || preview.Producer != message.From ||
						message.To != state.Replies || preview.Binding.Expected != renewed.Key ||
						preview.Binding.ExpectedScope != renewed.Scope || preview.Handle == 0 ||
						!preview.ImageVersion || !preview.OfferedVersion ||
						preview.OfferedVersion->ContentRevision != renewed.ContentRevision ||
						preview.OfferedVersion->LightingRevision != renewed.LightingRevision ||
						preview.ImageVersion->Width != renewed.Width ||
						preview.ImageVersion->Height != renewed.Height ||
						preview.ImageVersion->CaptureLighting != renewed.CaptureLighting ||
						renewed.CaptureTick < preview.ImageVersion->CaptureTick)
						continue;
					if (state.Resident) state.Resident->Cancel(state.Replies, preview.Pending);
					state.Inbox.CancelRequest(preview.Pending);
					preview.Pending = 0;
					preview.ImageVersion = renewed;
					preview.CaptureAccepted(now);
					preview.ImageDeadline = now + state.Limits.Timeout;
					completions.push_back(
						{message.Correlation,
						 PortalImageStatus::Ok,
						 preview.Handle,
						 {},
						 {renewed.ContentRevision, renewed.LightingRevision}}
					);
					break;
				}
				continue;
			}

			if (state.Resident != nullptr) {
				PortalResidentReceipt receipt;
				std::string error;
				if (DecodePortalResidentReceipt(message.Payload, receipt, error)) {
					const bool traceEye = state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 1 &&
										  *state.ViewSlot <= 3 && state.EyeDiagnosticCount++ < 256;
					if (traceEye)
						ENGINE_WARN(
							"portal eye resident receipt slot={} correlation={} request={} producer={} "
							"capture_tick={}",
							*state.ViewSlot,
							message.Correlation,
							receipt.Key.RequestId,
							message.From.World,
							receipt.CaptureTick
						);
					bool matched = false;
					for (auto &preview : state.Previews) {
						if (preview.OrderedLayers || preview.Pending != message.Correlation ||
							preview.Producer != message.From || message.To != state.Replies ||
							preview.Binding.Expected != receipt.Key ||
							preview.Binding.ExpectedScope != receipt.Scope) {
							continue;
						}
						matched = true;
						const auto handle = state.Resident->Take(state.Replies, message.From, receipt, now);
						if (traceEye)
							ENGINE_WARN(
								"portal eye resident take slot={} request={} handle={} deferred={}",
								*state.ViewSlot,
								receipt.Key.RequestId,
								handle,
								preview.Deferred.has_value()
							);
						if (handle == 0) {
							continue;
						}
						state.Inbox.CancelRequest(preview.Pending);
						preview.Pending = 0;
						if (preview.Tree != 0 || preview.TransparentImages[0] != 0)
							state.ReleaseImageOwner(preview);
						preview.Tree = 0;
						preview.TreeProducers.clear();
						preview.TransparentImages = {};
						preview.SpatialOverlayImage = 0;
						preview.CapturedLenses = {};
						state.Render.ReleasePortalLensPrograms(preview.LensPrograms);
						preview.LensPrograms = 0;
						preview.Handle = handle;
						preview.ImageVersion = receipt;
						preview.CaptureAccepted(now);
						preview.ImageDeadline = now + state.Limits.Timeout;
						completions.push_back(
							{message.Correlation,
							 PortalImageStatus::Ok,
							 handle,
							 {},
							 {receipt.ContentRevision, receipt.LightingRevision}}
						);
						break;
					}
					if (traceEye && !matched)
						ENGINE_WARN(
							"portal eye resident unmatched slot={} request={} previews={}",
							*state.ViewSlot,
							receipt.Key.RequestId,
							state.Previews.size()
						);
					continue;
				}
			}
			auto accepted = state.Inbox.AcceptAuthenticated(
				Borrow(message.From),
				Borrow(message.To),
				message.Correlation,
				message.Payload,
				now,
				[&](PortalCaptureTreeEndpointView endpoint) { return state.EndpointCurrent(endpoint); },
				PublishedEndpoint(state.EndpointBindings, Borrow(message.From))
			);
			if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 && *state.ViewSlot <= 3 &&
				state.EyeDiagnosticCount++ < 256)
				ENGINE_WARN(
					"portal eye source admission slot={} correlation={} status={} error={}",
					*state.ViewSlot,
					message.Correlation,
					static_cast<int>(accepted.Status),
					accepted.Error
				);
			auto preview =
				std::find_if(state.Previews.begin(), state.Previews.end(), [&](const Impl::Preview &entry) {
					return entry.Pending == message.Correlation;
				});
			if (preview == state.Previews.end()) {
				if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 && *state.ViewSlot <= 3 &&
					state.EyeDiagnosticCount++ < 256)
					ENGINE_WARN(
						"portal eye source ignored slot={} correlation={} admission={} preview=missing",
						*state.ViewSlot,
						message.Correlation,
						static_cast<int>(accepted.Status)
					);
				continue;
			}
			if (accepted.Status == PortalInboxStatus::CompletedFailure) {
				if (state.Resident != nullptr) {
					state.Resident->Cancel(state.Replies, message.Correlation);
				}
				completions.push_back(
					{message.Correlation,
					 accepted.Failure->Status,
					 0,
					 std::move(accepted.Failure->Diagnostic),
					 {}}
				);
				preview->Pending = 0;
				continue;
			}
			if (accepted.Status != PortalInboxStatus::Accepted) {
				continue;
			}
			if (state.Resident != nullptr) {
				state.Resident->Cancel(state.Replies, message.Correlation);
			}
			if (state.Delivery == PortalImageSourceDelivery::CapturePayloads) {
				preview->PayloadReady = true;
				preview->CaptureAccepted(now);
				preview->ImageDeadline = now + state.Limits.Timeout;
				preview->Pending = 0;
				completions.push_back({message.Correlation, PortalImageStatus::Ok, 0, {}, {}});
				continue;
			}
			if (preview->OrderedLayers) {
				auto tree = state.Inbox.TakeTree(
					Borrow(state.Replies), Borrow(message.From), preview->Binding.Expected.PortalKey, now
				);
				std::optional<PortalImageLayerSet> layers;
				if (!tree) {
					layers = state.Inbox.TakeLayers(
						Borrow(state.Replies), Borrow(message.From), preview->Binding.Expected.PortalKey, now
					);
					if (layers && !preview->RetainedBodyPlayer.empty()) {
						// A leaf eye needs the same owned node identity as recursive body composition.
						tree.emplace();
						tree->Nodes.push_back(
							{{preview->ExpectedProducer.World,
							  preview->ExpectedProducer.Channel,
							  preview->ExpectedProducer.Session,
							  preview->ExpectedProducer.Generation},
							 {preview->PendingCamera.Position,
							  preview->PendingCamera.Orientation,
							  preview->PendingCamera.Frustum,
							  preview->PendingCamera.ClipPlane,
							  preview->Binding.ExpectedProjection},
							 std::move(*layers),
							 preview->RetainedBodyPlayer}
						);
					}
				}
				if (tree) {
					const auto &opaque = tree->Nodes.front().Layers.Opaque;
					const PortalResidentReceipt version{
						opaque.Key,
						opaque.Scope,
						opaque.CaptureTick,
						opaque.ContentRevision,
						opaque.LightingRevision,
						opaque.Width,
						opaque.Height,
						opaque.CaptureLighting
					};
					std::vector<PortalCaptureTreeEndpoint> producers;
					for (const auto &node : tree->Nodes)
						producers.push_back(node.Producer);
					const auto token =
						state.Render.QueuePortalCaptureTree(preview->Binding, std::move(*tree));
					if (token == 0) {
						preview->Pending = 0;
						completions.push_back(
							{message.Correlation,
							 PortalImageStatus::BudgetExceeded,
							 0,
							 "source capture tree import refused",
							 {}}
						);
						continue;
					}
					preview->UploadTree = token;
					preview->UploadTreeProducers = std::move(producers);
					preview->UploadVersion = version;
					if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 &&
						*state.ViewSlot <= 3 && state.EyeDiagnosticCount++ < 256)
						ENGINE_WARN(
							"portal eye source queued tree slot={} request={} token={} capture_tick={}",
							*state.ViewSlot,
							preview->Pending,
							token,
							version.CaptureTick
						);
					continue;
				}
				if (!layers) continue;
				const auto &opaque = layers->Opaque;
				const PortalResidentReceipt version{
					opaque.Key,
					opaque.Scope,
					opaque.CaptureTick,
					opaque.ContentRevision,
					opaque.LightingRevision,
					opaque.Width,
					opaque.Height,
					opaque.CaptureLighting
				};
				const size_t imageCount = 1 + layers->Transparent.size() + layers->SpatialOverlay.has_value();
				const auto lensPrograms = state.Render.RetainPortalLensPrograms(layers->Lenses);
				if (!layers->Lenses.Programs.empty() && lensPrograms == 0) {
					preview->Pending = 0;
					completions.push_back(
						{message.Correlation,
						 PortalImageStatus::Unsupported,
						 0,
						 "captured lens program refused",
						 {}}
					);
					continue;
				}
				auto uploadedLenses = std::move(layers->Lenses);
				layers->Lenses = {};
				// The renderer lease owns accepted code; captures keep only evaluated inputs.
				uploadedLenses.Programs = {};
				if (!state.Render.QueuePortalImageLayerSet(
						preview->Binding,
						std::move(*layers),
						std::span(preview->UploadImages).first(imageCount)
					)) {
					state.Render.ReleasePortalLensPrograms(lensPrograms);
					preview->Pending = 0;
					completions.push_back(
						{message.Correlation,
						 PortalImageStatus::BudgetExceeded,
						 0,
						 "source layer import refused",
						 {}}
					);
					continue;
				}
				preview->UploadVersion = version;
				preview->UploadLenses = std::move(uploadedLenses);
				preview->UploadLensPrograms = lensPrograms;
				continue;
			}
			auto image = state.Inbox.Take(
				Borrow(state.Replies), Borrow(message.From), preview->Binding.Expected.PortalKey, now
			);
			if (!image) {
				if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 && *state.ViewSlot <= 3 &&
					state.EyeDiagnosticCount++ < 256)
					ENGINE_WARN(
						"portal eye source accepted without image slot={} correlation={} expected={}",
						*state.ViewSlot,
						message.Correlation,
						preview->Binding.Expected.RequestId
					);
				continue;
			}
			const PortalResidentReceipt imageVersion{
				image->Key,
				image->Scope,
				image->CaptureTick,
				image->ContentRevision,
				image->LightingRevision,
				image->Width,
				image->Height,
				image->CaptureLighting
			};
			const auto handle = state.Render.QueuePortalImage(preview->Binding, std::move(*image));
			preview->Pending = 0;
			if (state.EyeDiagnostic && state.ViewSlot && *state.ViewSlot >= 2 && *state.ViewSlot <= 3 &&
				state.EyeDiagnosticCount++ < 256)
				ENGINE_WARN(
					"portal eye source imported slot={} correlation={} handle={} capture_tick={}",
					*state.ViewSlot,
					message.Correlation,
					handle,
					imageVersion.CaptureTick
				);
			if (handle == 0) {
				completions.push_back(
					{message.Correlation,
					 PortalImageStatus::BudgetExceeded,
					 0,
					 "source image import refused",
					 {}}
				);
				continue;
			}
			if (preview->Tree != 0 || preview->TransparentImages[0] != 0) state.ReleaseImageOwner(*preview);
			preview->Tree = 0;
			preview->TreeProducers.clear();
			preview->TransparentImages = {};
			preview->SpatialOverlayImage = 0;
			preview->CapturedLenses = {};
			state.Render.ReleasePortalLensPrograms(preview->LensPrograms);
			preview->LensPrograms = 0;
			preview->Handle = handle;
			ENGINE_TRACE(
				"portal image received: {} {} request {} handle {}",
				state.Replies.Channel,
				message.From.World,
				message.Correlation,
				handle
			);
			preview->ImageVersion = imageVersion;
			preview->CaptureAccepted(now);
			preview->ImageDeadline = now + state.Limits.Timeout;
			completions.push_back(
				{message.Correlation,
				 PortalImageStatus::Ok,
				 handle,
				 {},
				 {imageVersion.ContentRevision, imageVersion.LightingRevision}}
			);
		}
		// Dispatch after reply processing so the completed preview has released its
		// pending slot. Retain the entries while collecting, since Issue can replace
		// previews and therefore invalidate a range-for reference.
		struct DeferredIssue {
			world::PresentationAddress Producer;
			Impl::Demand Demand;
		};
		std::vector<DeferredIssue> deferred;
		for (const auto &preview : state.Previews) {
			if (preview.Pending != 0 || !preview.Deferred || now >= preview.Deferred->Deadline ||
				(state.Delivery == PortalImageSourceDelivery::CapturePayloads && preview.PayloadReady))
				continue;
			deferred.push_back({preview.Producer, *preview.Deferred});
		}
		for (const auto &next : deferred) {
			const PortalRuntimeIssue issued =
				Issue(next.Producer, next.Demand.Request, next.Demand.Binding, now);
			if (issued.Status == PortalInboxStatus::Issued || issued.Status == PortalInboxStatus::Full ||
				issued.Transport == world::PresentationStatus::Full)
				continue;
			auto preview =
				std::find_if(state.Previews.begin(), state.Previews.end(), [&](const Impl::Preview &entry) {
					return entry.Producer == next.Producer &&
						   entry.Binding.Portal == next.Demand.Binding.Portal && entry.Pending == 0;
				});
			if (preview != state.Previews.end()) preview->Deferred.reset();
		}
		return completions;
	}
}

// PortalImageProducer::Impl, the producer state shared by the producer and pump sources.
#pragma once

#include "portal/PortalImageRuntimeInternal.hpp"

namespace engine::render {
	struct PortalImageProducer::Impl {
		Impl(
			world::Universe &universe,
			Renderer &renderer,
			world::WorldId world,
			world::PresentationAddress requests,
			core::Name ownerName,
			PortalResidentImages *resident
		)
			: Universe(universe), Render(renderer), World(world), Requests(std::move(requests)),
			  OwnerName(ownerName), Resident(resident), ContentOwner(OwnerName) {}

		world::Universe &Universe;
		PortalRenderOperations Render;
		world::WorldId World;
		world::PresentationAddress Requests;
		core::Name OwnerName;
		PortalResidentImages *Resident = nullptr;
		const world::PresentationBindings *EndpointBindings = nullptr;
		core::Name ContentOwner = OwnerName;
		std::unique_ptr<ShaderLibrary> OwnedShaders;
		ShaderLibrary *Shaders = nullptr;
		bool PostProcessing = true;
		bool EyeDiagnostic = std::getenv("PORTAL_EYE_PRODUCER_DIAGNOSTIC") != nullptr;
		size_t EyeDiagnosticCount = 0;
		std::vector<core::Name> GuiShaders;
		uint64_t GuiShaderSignature = 0;
		std::vector<WorldContentOwner> ForeignContentOwners;
		EditableImageUploader EditableImages;
		EditableMeshUploader EditableMeshes;
		std::optional<Time> LastTime;
		std::array<bool, 4> PipelineReady{};
		WorldViewFrame WorldFrame;
		RetainedBodyAuthorization AuthorizeRetainedBody;
		struct PresentationFrame {
			std::vector<scene::DrawInstance> EntranceRows;
			std::optional<int16_t> Entrance;
			// Each capture owns one clock. Imported body packets replace this
			// destination clock so their materials and temporal passes agree.
			double PresentationSeconds = 0;
			WorldCameraFrame Camera;
			std::vector<SurfaceView> Surfaces;
			InterfacePass Interface;
			bool InterfaceReady = false;
		};
		std::array<PresentationFrame, MAX_WAITING_CAPTURES> Frames;
		static constexpr size_t MAX_CAPTURE_WORLD_DEPENDENCIES = 256;
		struct Capture {
			world::PresentationAddress ReplyTo;
			std::vector<std::string> WorldDependencies;
			size_t WorldDependencyBytes = 0;
			bool WorldDependenciesKnown = true;
			bool WorldProvenanceComplete = false;
			PortalImageReply Reply;
			std::vector<std::byte> Wire;
			bool TransferEye = false;
			uint64_t Token = 0;
			std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS + 1> LayerTokens{};
			uint64_t OverlayToken = 0;
			uint64_t ShadowToken = 0;
			std::optional<size_t> ShadowSlot;
			std::optional<size_t> RouteSlot;
			PortalCaptureTreeEndpoint Producer;
			PortalCaptureLenses Lenses;
			std::optional<PortalCaptureTree> Tree;
			Time Deadline;
			std::string RetainedBodyPlayer;
		};
		static void RetainWorld(Capture &capture, std::string_view world) {
			constexpr size_t MAX_BYTES = 16384;
			if (!capture.WorldDependenciesKnown || world.empty() ||
				std::find(capture.WorldDependencies.begin(), capture.WorldDependencies.end(), world) !=
					capture.WorldDependencies.end())
				return;
			if (capture.WorldDependencies.size() == MAX_CAPTURE_WORLD_DEPENDENCIES ||
				world.size() > MAX_BYTES - capture.WorldDependencyBytes) {
				capture.WorldDependenciesKnown = false;
				capture.WorldDependencies.clear();
				return;
			}
			try {
				capture.WorldDependencies.emplace_back(world);
			} catch (const std::bad_alloc &) {
				capture.WorldDependenciesKnown = false;
				capture.WorldDependencies.clear();
				return;
			}
			capture.WorldDependencyBytes += world.size();
		}
		void RetainContentWorlds(Capture &capture) const {
			const auto add = [&](core::Name world) {
				if (world.IsValid()) RetainWorld(capture, world.Text());
			};
			add(ContentOwner);
			for (const auto &binding : ForeignContentOwners) {
				add(binding.World);
				add(binding.Owner);
			}
		}
		std::vector<Capture> Captures;
		std::vector<Capture> FailureReplies;
		struct ProducedShadow {
			world::PresentationAddress Requester;
			PortalShadowImage Image;
			View Snapshot;
			std::vector<scene::DrawInstance> Rows, ForeignRows;
			std::vector<core::CFrame> Joints, ForeignJoints;
			std::vector<SurfaceView> Surfaces;
			std::vector<WorldContentOwner> ContentOwners;
			std::vector<ParticleBatch> Particles;
			std::vector<ParticleSeam> ParticleSeams;
			std::vector<effects::RibbonVertex> RibbonVertices;
			std::vector<effects::RibbonRun> RibbonRuns;
			std::vector<SceneLight> Lights;
			std::vector<PortalView> Portals;
			std::vector<uint32_t> EyeHiddenRows;
			uint32_t Width = 0, Height = 0;
			uint64_t Revision = 0, FitToken = 0;
			std::optional<core::AABB> FitBounds;
			std::optional<PortalShadowImage> FitImage;
			Time Deadline;
			std::optional<Time> PinnedUntil;
			bool Ready = false;
		};
		std::array<std::optional<ProducedShadow>, MAX_PORTAL_PRODUCER_SHADOW_BYTES / PORTAL_SHADOW_BYTES>
			Shadows;
		struct ShadowRouteTarget {
			PortalCaptureTreeEndpoint Producer;
			PortalExchangeKey Eye;
			PortalShadowRoute Hop;
			PortalCaptureTreeEndpoint HopProducer;
		};
		struct ProducedShadowRoutes {
			world::PresentationAddress Requester;
			PortalExchangeKey ParentEye;
			PortalCaptureTreeEndpoint Producer;
			std::string RetainedBodyPlayer;
			std::array<ShadowRouteTarget, MAX_PORTAL_CAPTURE_TREE_NODES> Targets;
			size_t Count = 0;
			Time Deadline;
			std::optional<Time> PinnedUntil;
			bool Ready = false;
		};
		std::array<std::optional<ProducedShadowRoutes>, MAX_PORTAL_PRODUCER_SHADOW_ROUTES> ShadowRoutes;
		enum class ShadowTransferPhase { SendChild, WaitChild, SendParent, Done, CancelChildren };
		struct ShadowTransfer {
			world::PresentationAddress Requester;
			PortalCaptureTreeEndpoint Producer;
			PortalShadowPull Pull, Forwarded;
			PortalShadowRoute Hop;
			uint64_t Correlation = 0, ChildCorrelation = 0;
			Time Deadline;
			std::optional<Time> LastChildSend;
			std::string RetainedBodyPlayer;
			std::vector<std::byte> Wire;
			std::array<ShadowRouteTarget, MAX_PORTAL_CAPTURE_TREE_NODES> CancelHops;
			size_t CancelCount = 0, CancelNext = 0;
			ShadowTransferPhase Phase = ShadowTransferPhase::SendParent;
			PortalImageStatus ResponseStatus = PortalImageStatus::Unavailable;
		};
		// A bounded dependency chain can revisit this producer while its parent waits.
		// Waiting contexts own metadata only; all outbound packets share the wire cap.
		std::array<std::optional<ShadowTransfer>, MAX_PORTAL_CAPTURE_TREE_DEPTH + 1> Transfers;
		uint64_t NextShadowCorrelation = 1;
		struct Child {
			PortalImageDemand Demand;
			world::PresentationAddress Producer;
			uint64_t RequestId = 0;
			PortalImageVersion Version;
			std::optional<PortalCaptureTree> Tree;
			PortalCaptureTreeEdge Edge;
		};
		struct Job {
			PortalImageRequest Request;
			Capture Output;
			View Viewpoint;
			size_t Slot = 0;
			bool Collected = false;
			bool ViewPrepared = false;
			bool Waiting = false;
			bool KeepChildren = false;
			PortalImageStatus FailureStatus = PortalImageStatus::Unavailable;
			std::vector<Child> Children;
			std::vector<PortalView> Portals;
			uint64_t LocalPixels = 0;
			std::string Failure;
		};
		struct PendingSlot {
			std::optional<Job> Work;
			world::PresentationAddress Replies;
			std::unique_ptr<PortalImageSource> Source;
			bool PayloadSource = false;
		};
		std::array<PendingSlot, MAX_WAITING_CAPTURES> Pending;
		void Finish(size_t slot, bool keepChildren = false) {
			auto &pending = Pending[slot];
			if (pending.Source && !keepChildren) pending.Source->Clear();
			pending.Work.reset();
		}

		void ReleaseProducedShadow(std::optional<ProducedShadow> &shadow) {
			if (shadow && shadow->FitToken) Render.CancelResourceImage(shadow->FitToken);
			shadow.reset();
		}
		void ReleaseShadow(Capture &capture) {
			if (capture.ShadowSlot) ReleaseProducedShadow(Shadows[*capture.ShadowSlot]);
			capture.ShadowSlot.reset();
		}
		void ReleaseRoutes(Capture &capture) {
			if (capture.RouteSlot) ShadowRoutes[*capture.RouteSlot].reset();
			capture.RouteSlot.reset();
		}
		void Cancel(Capture &capture, bool releaseShadow = true) {
			Render.CancelResourceImage(capture.ShadowToken);
			capture.ShadowToken = 0;
			if (releaseShadow) {
				ReleaseShadow(capture);
				ReleaseRoutes(capture);
			}
			Render.CancelResourceImage(capture.OverlayToken);
			capture.OverlayToken = 0;
			Render.CancelResourceImage(capture.Token);
			capture.Token = 0;
			for (auto &token : capture.LayerTokens) {
				Render.CancelResourceImage(token);
				token = 0;
			}
		}
		bool CollectLayers(Capture &capture, Time now) {
			std::array tokens{
				capture.Token,
				capture.LayerTokens[0],
				capture.LayerTokens[1],
				capture.LayerTokens[2],
				capture.OverlayToken,
				capture.ShadowToken
			};
			const bool overlay = capture.OverlayToken != 0;
			const bool shadow = capture.ShadowToken != 0;
			const size_t colourCount = overlay ? 5 : 4;
			if (!overlay) tokens[4] = capture.ShadowToken;
			auto images = Render.TakeResourceImages(std::span(tokens).first(colourCount + shadow));
			if (!images && now < capture.Deadline) return false;
			Cancel(capture, false);
			capture.Reply.Status = PortalImageStatus::Failed;
			capture.Reply.Diagnostic =
				images ? "destination layer capture failed" : "destination layer capture expired";
			if (!images) return true;
			const auto &opaque = images->front();
			for (size_t index = 0; index < colourCount; ++index) {
				const auto &image = (*images)[index];
				if (image.Status != ResourceImageStatus::Ok || image.CaptureFrame == 0 ||
					image.CaptureFrame != opaque.CaptureFrame || image.Width != opaque.Width ||
					image.Height != opaque.Height ||
					image.Depth.size() != (index < 4 ? size_t(image.Width) * image.Height * 4 : 0) ||
					image.Normal.size() != (index == 0 ? size_t(image.Width) * image.Height * 4 : 0) ||
					image.AmbientResponse.size() !=
						(index == 0 ? size_t(image.Width) * image.Height * 16 : 0) ||
					image.LightingBaseline.size() !=
						(index == 0 ? size_t(image.Width) * image.Height * 16 : 0) ||
					image.DirectionalResponse.size() !=
						(index == 0 ? size_t(image.Width) * image.Height * 16 : 0))
					return true;
			}
			if (shadow) {
				auto &image = (*images)[colourCount];
				if (!capture.ShadowSlot || !Shadows[*capture.ShadowSlot] ||
					image.Status != ResourceImageStatus::Ok ||
					image.Kind != ResourceImageKind::DirectionalShadow || !image.Shadow ||
					image.CaptureFrame != opaque.CaptureFrame || image.Width != PORTAL_SHADOW_EXTENT ||
					image.Height != PORTAL_SHADOW_EXTENT || image.Depth.size() != PORTAL_SHADOW_BYTES ||
					image.Depth.capacity() > PORTAL_SHADOW_BYTES)
					return true;
				auto &retained = Shadows[*capture.ShadowSlot]->Image;
				auto &snapshot = retained.Snapshot;
				snapshot.CaptureTick = capture.Reply.CaptureTick;
				snapshot.ContentRevision = capture.Reply.ContentRevision;
				snapshot.LightingRevision = capture.Reply.LightingRevision;
				snapshot.EyePixelHash = assets::Hasher::Of(opaque.Pixels);
				snapshot.DepthHash = assets::Hasher::Of(image.Depth);
				snapshot.SourceEmpty = image.Shadow->SourceEmpty;
				const auto bounds = [](const core::AABB &box) {
					return std::array{
						box.Minimum.X,
						box.Minimum.Y,
						box.Minimum.Z,
						box.Maximum.X,
						box.Maximum.Y,
						box.Maximum.Z
					};
				};
				snapshot.SourceBounds = bounds(image.Shadow->SourceBounds);
				snapshot.DomainBounds = bounds(image.Shadow->DomainBounds);
				snapshot.LightViewProjection = image.Shadow->LightViewProjection;
				retained.Depth = std::move(image.Depth);
				if (!ValidPortalShadowImage(retained)) return true;
				Shadows[*capture.ShadowSlot]->Revision = Render.ResourceRevision();
				core::Metrics::Count("render.portal_shadow.captured_bytes", retained.Depth.size());
				core::Metrics::Count("render.portal_shadow.captures", 1);
			}
			const auto &overflow = (*images)[3];
			if (std::any_of(overflow.Depth.begin(), overflow.Depth.end(), [](std::byte value) {
					return value != std::byte{};
				})) {
				capture.Reply.Status = PortalImageStatus::BudgetExceeded;
				capture.Reply.Diagnostic = "destination transparency exceeds two ordered layers";
				core::Metrics::Count("render.portal_images.layer_overflow", 1);
				return true;
			}
			capture.Reply.Status = PortalImageStatus::Ok;
			capture.Reply.Diagnostic.clear();
			PortalImageLayerSet layers;
			layers.Lenses = std::move(capture.Lenses);
			layers.Transparent.resize(MAX_PORTAL_TRANSPARENT_LAYERS);
			for (size_t index = 0; index <= MAX_PORTAL_TRANSPARENT_LAYERS; ++index) {
				auto &reply = index == 0 ? layers.Opaque : layers.Transparent[index - 1];
				auto &image = (*images)[index];
				reply = capture.Reply;
				reply.Width = image.Width;
				reply.Height = image.Height;
				reply.RowStride = image.RowStride;
				reply.Pixels = std::move(image.Pixels);
				reply.Depth = std::move(image.Depth);
				reply.PixelHash = assets::Hasher::Of(reply.Pixels);
				reply.DepthHash = assets::Hasher::Of(reply.Depth);
				if (index == 0) {
					reply.Normal = std::move(image.Normal);
					reply.AmbientResponse = std::move(image.AmbientResponse);
					reply.LightingBaseline = std::move(image.LightingBaseline);
					reply.DirectionalResponse = std::move(image.DirectionalResponse);
					reply.NormalHash = assets::Hasher::Of(reply.Normal);
					reply.AmbientResponseHash = assets::Hasher::Of(reply.AmbientResponse);
					reply.LightingBaselineHash = assets::Hasher::Of(reply.LightingBaseline);
					reply.DirectionalResponseHash = assets::Hasher::Of(reply.DirectionalResponse);
				}
			}
			if (overlay) {
				layers.SpatialOverlay = capture.Reply;
				auto &reply = *layers.SpatialOverlay;
				auto &image = (*images)[4];
				reply.Width = image.Width;
				reply.Height = image.Height;
				reply.RowStride = image.RowStride;
				reply.Pixels = std::move(image.Pixels);
				reply.PixelHash = assets::Hasher::Of(reply.Pixels);
			}
			std::string error;
			bool encoded = false;
			if (capture.Tree) {
				capture.Tree->Nodes.front().Layers = std::move(layers);
				encoded = EncodePortalCaptureTree(*capture.Tree, capture.Wire, error);
				for (const auto &node : capture.Tree->Nodes)
					RetainWorld(capture, node.Producer.World);
				capture.WorldProvenanceComplete = true;
				capture.Tree.reset();
			} else {
				encoded = EncodePortalImageLayerSet(layers, capture.Wire, error);
			}
			if (!encoded) {
				capture.Reply.Status = PortalImageStatus::Failed;
				capture.Reply.Diagnostic = "destination layer encoding failed: " + error;
			}
			return true;
		}

		bool Authorized(const world::PresentationAddress &requester, std::string_view player) const {
			if (player.empty()) return true;
			return AuthorizeRetainedBody &&
				   Universe.LookupPresentation(
					   Universe.Find(core::Name(requester.World)), requester.Channel
				   ) == requester &&
				   AuthorizeRetainedBody(requester, player);
		}
		bool ShadowCurrent(const ProducedShadow &shadow) const {
			const auto &snapshot = shadow.Image.Snapshot;
			return Universe.LookupPresentation(World, Requests.Channel) == Requests &&
				   PublishedEndpoint(EndpointBindings, Borrow(Requests)) ==
					   PortalEndpointView{
						   snapshot.Producer.World,
						   snapshot.Producer.Channel,
						   snapshot.Producer.Session,
						   snapshot.Producer.Generation
					   } &&
				   Authorized(shadow.Requester, snapshot.ExcludedPlayer);
		}
		static bool RouteText(std::string_view text) {
			return !text.empty() && text.size() <= 256 && text.find('\0') == std::string_view::npos;
		}
		static bool ValidRouteEndpoint(PortalEndpointView endpoint) {
			return RouteText(endpoint.World) && RouteText(endpoint.Channel) && endpoint.Session != 0 &&
				   endpoint.Generation != 0;
		}
		static PortalEndpointView BorrowRouteEndpoint(const PortalCaptureTreeEndpoint &endpoint) {
			return {endpoint.World, endpoint.Channel, endpoint.Session, endpoint.Generation};
		}
		static bool RouteEye(const PortalExchangeKey &eye) {
			return eye.RequestId != 0 && RouteText(eye.PortalKey);
		}
		bool RoutesCurrent(const ProducedShadowRoutes &routes, bool checkAuthorization = true) const {
			if (Universe.LookupPresentation(World, Requests.Channel) != Requests ||
				PublishedEndpoint(EndpointBindings, Borrow(Requests)) !=
					BorrowRouteEndpoint(routes.Producer) ||
				(checkAuthorization && !Authorized(routes.Requester, routes.RetainedBodyPlayer)))
				return false;
			for (const auto &target : std::span(routes.Targets).first(routes.Count)) {
				const auto &hop = target.Hop;
				if (Universe.LookupPresentation(
						Universe.Find(core::Name(hop.Requester.World)), hop.Requester.Channel
					) != hop.Requester ||
					!CurrentEndpoint(Universe, Borrow(hop.Producer)) ||
					PublishedEndpoint(EndpointBindings, Borrow(hop.Producer)) !=
						BorrowRouteEndpoint(target.HopProducer) ||
					!CurrentPublishedEndpoint(
						Universe, EndpointBindings, BorrowRouteEndpoint(target.Producer)
					))
					return false;
			}
			return true;
		}
		bool ReserveRoutes(Capture &capture) {
			if (!ValidRouteEndpoint(Borrow(capture.ReplyTo)) || !ValidRouteEndpoint(Borrow(Requests)) ||
				!ValidRouteEndpoint(BorrowRouteEndpoint(capture.Producer)) || !RouteEye(capture.Reply.Key))
				return false;
			for (const auto &routes : ShadowRoutes)
				if (routes && routes->Requester == capture.ReplyTo && routes->ParentEye == capture.Reply.Key)
					return false;
			for (size_t index = 0; index < ShadowRoutes.size(); ++index) {
				if (ShadowRoutes[index]) continue;
				ProducedShadowRoutes routes;
				routes.Requester = capture.ReplyTo;
				routes.ParentEye = capture.Reply.Key;
				routes.Producer = capture.Producer;
				routes.RetainedBodyPlayer = capture.RetainedBodyPlayer;
				routes.Deadline = capture.Deadline;
				routes.Targets[0] = {
					capture.Producer,
					capture.Reply.Key,
					{capture.ReplyTo, Requests, capture.Reply.Key},
					capture.Producer
				};
				routes.Count = 1;
				if (!RoutesCurrent(routes)) return false;
				ShadowRoutes[index] = std::move(routes);
				capture.RouteSlot = index;
				return true;
			}
			return false;
		}
		bool AppendRoutes(Capture &capture, const Child &child, const world::PresentationAddress &requester) {
			if (!capture.RouteSlot || !ShadowRoutes[*capture.RouteSlot] || !child.Tree ||
				child.Tree->Nodes.empty() || !ValidRouteEndpoint(Borrow(requester)) ||
				!ValidRouteEndpoint(Borrow(child.Producer)))
				return false;
			auto &routes = *ShadowRoutes[*capture.RouteSlot];
			const auto &root = child.Tree->Nodes.front();
			const auto &childEye = root.Layers.Opaque.Key;
			if (!RouteEye(childEye) || childEye.RequestId != child.RequestId ||
				PublishedEndpoint(EndpointBindings, Borrow(child.Producer)) !=
					BorrowRouteEndpoint(root.Producer))
				return false;
			for (const auto &node : child.Tree->Nodes) {
				const auto &eye = node.Layers.Opaque.Key;
				if (!RouteEye(eye) || !ValidRouteEndpoint(BorrowRouteEndpoint(node.Producer)) ||
					node.RetainedBodyPlayer != capture.RetainedBodyPlayer ||
					routes.Count == routes.Targets.size())
					return false;
				// An identical published eye reached through two different parents is ambiguous.
				for (const auto &existing : std::span(routes.Targets).first(routes.Count))
					if (existing.Producer == node.Producer && existing.Eye == eye) return false;
				routes.Targets[routes.Count++] = {
					node.Producer, eye, {requester, child.Producer, childEye}, root.Producer
				};
			}
			return RoutesCurrent(routes);
		}
		ProducedShadowRoutes *
		FindRoutes(const world::PresentationAddress &requester, const PortalExchangeKey &eye) {
			for (auto &routes : ShadowRoutes)
				if (routes && routes->Ready && routes->Requester == requester && routes->ParentEye == eye)
					return &*routes;
			return nullptr;
		}
		bool PinRoutes(ProducedShadowRoutes &routes, Time now) {
			if (now > Time::max() - SHADOW_TRANSFER_TIMEOUT) return false;
			if (!routes.PinnedUntil) routes.PinnedUntil = now + SHADOW_TRANSFER_TIMEOUT;
			if (now >= *routes.PinnedUntil) return false;
			routes.Deadline = *routes.PinnedUntil;
			for (auto &shadow : Shadows)
				if (shadow && shadow->Ready && shadow->Requester == routes.Requester &&
					shadow->Image.Snapshot.Eye == routes.ParentEye) {
					if (!shadow->PinnedUntil) shadow->PinnedUntil = routes.PinnedUntil;
					shadow->Deadline = *shadow->PinnedUntil;
				}
			return true;
		}
		bool TransferCurrent(const ShadowTransfer &transfer, bool checkAuthorization = true) {
			if (Universe.LookupPresentation(World, Requests.Channel) != Requests ||
				PublishedEndpoint(EndpointBindings, Borrow(Requests)) !=
					BorrowRouteEndpoint(transfer.Producer) ||
				Universe.LookupPresentation(
					Universe.Find(core::Name(transfer.Requester.World)), transfer.Requester.Channel
				) != transfer.Requester ||
				(checkAuthorization && !Authorized(transfer.Requester, transfer.RetainedBodyPlayer)))
				return false;
			if (transfer.Pull.Part == PORTAL_SHADOW_CANCEL_PART || transfer.RetainedBodyPlayer.empty())
				return true;
			const auto *routes = FindRoutes(transfer.Requester, transfer.Pull.ParentEye);
			return routes && RoutesCurrent(*routes, checkAuthorization);
		}
		size_t TransferBytes(const ShadowTransfer *except = nullptr) const {
			size_t bytes = 0;
			for (const auto &transfer : Transfers)
				if (transfer && &*transfer != except) bytes += transfer->Wire.capacity();
			return bytes;
		}
		bool StoreTransferWire(ShadowTransfer &transfer, std::vector<std::byte> wire) {
			for (auto &completed : Transfers) {
				if (wire.capacity() <= MAX_PORTAL_EXCHANGE_BYTES - TransferBytes(&transfer)) break;
				if (completed && &*completed != &transfer && completed->Phase == ShadowTransferPhase::Done)
					completed.reset();
			}
			if (wire.capacity() > MAX_PORTAL_EXCHANGE_BYTES - TransferBytes(&transfer)) return false;
			transfer.Wire = std::move(wire);
			return true;
		}
		bool ShadowPacket(
			ShadowTransfer &transfer, PortalImageStatus status, std::vector<std::byte> payload = {}
		) {
			std::string error;
			std::vector<std::byte> wire;
			if (!EncodePortalShadowPacket({transfer.Pull, status, std::move(payload)}, wire, error) ||
				!StoreTransferWire(transfer, std::move(wire)))
				return false;
			transfer.Phase = ShadowTransferPhase::SendParent;
			transfer.ResponseStatus = status;
			return true;
		}
		void StartShadowPull(
			const world::PresentationMessage &message,
			PortalShadowPull pull,
			Time now,
			PortalProducerProgress &progress
		) {
			if (message.To != Requests || message.Correlation == 0 || !ReplySlot(message.From.Channel) ||
				Universe.LookupPresentation(
					Universe.Find(core::Name(message.From.World)), message.From.Channel
				) != message.From) {
				++progress.Refused;
				return;
			}
			for (auto &transfer : Transfers)
				if (transfer && transfer->Requester == message.From &&
					transfer->Correlation == message.Correlation && transfer->Pull == pull) {
					if (transfer->Phase == ShadowTransferPhase::Done &&
						transfer->ResponseStatus == PortalImageStatus::BudgetExceeded) {
						transfer.reset();
						break;
					}
					if (transfer->Phase == ShadowTransferPhase::Done)
						transfer->Phase = ShadowTransferPhase::SendParent;
					if (transfer->Phase == ShadowTransferPhase::WaitChild && transfer->LastChildSend &&
						now - *transfer->LastChildSend >= std::chrono::milliseconds(100)) {
						std::vector<std::byte> wire;
						std::string error;
						if (EncodePortalShadowPull(transfer->Forwarded, wire, error) &&
							StoreTransferWire(*transfer, std::move(wire)))
							transfer->Phase = ShadowTransferPhase::SendChild;
					}
					return;
				}
			auto *routes = FindRoutes(message.From, pull.ParentEye);
			const ShadowRouteTarget *target = nullptr;
			if (routes)
				for (const auto &entry : std::span(routes->Targets).first(routes->Count))
					if (entry.Producer == pull.TargetProducer && entry.Eye == pull.TargetEye) target = &entry;
			const bool cancelling = target && pull.Part == PORTAL_SHADOW_CANCEL_PART;
			if (cancelling && RoutesCurrent(*routes))
				for (auto &transfer : Transfers)
					if (transfer && transfer->Requester == message.From &&
						transfer->Pull.ParentEye == pull.ParentEye)
						transfer.reset();
			const auto free = std::find_if(Transfers.begin(), Transfers.end(), [](const auto &transfer) {
				return !transfer || transfer->Phase == ShadowTransferPhase::Done;
			});
			if (free == Transfers.end() || NextShadowCorrelation == 0) {
				std::vector<std::byte> busy;
				std::string error;
				if (EncodePortalShadowPacket({pull, PortalImageStatus::BudgetExceeded, {}}, busy, error)) {
					const std::array<std::string_view, 1> references{pull.TargetProducer.World};
					(void)Universe.SendPresentation(
						World,
						Requests,
						message.From,
						message.Correlation,
						busy,
						world::PresentationPriority::Routine,
						references
					);
				}
				++progress.Refused;
				return;
			}
			auto &Transfer = *free;
			Transfer.reset();
			ShadowTransfer next;
			next.Requester = message.From;
			next.Pull = std::move(pull);
			next.Correlation = message.Correlation;
			next.ChildCorrelation = NextShadowCorrelation++;
			next.Deadline = now + CAPTURE_TIMEOUT;
			const auto published = PublishedEndpoint(EndpointBindings, Borrow(Requests));
			next.Producer = {
				std::string(published.World),
				std::string(published.Channel),
				published.Session,
				published.Generation
			};
			if (!target || !RoutesCurrent(*routes) || !PinRoutes(*routes, now)) {
				if (ShadowPacket(next, PortalImageStatus::Unavailable)) Transfer = std::move(next);
				return;
			}
			next.RetainedBodyPlayer = routes->RetainedBodyPlayer;
			next.Deadline = routes->Deadline;
			next.Hop = target->Hop;
			next.Forwarded = next.Pull;
			next.Forwarded.ParentEye = target->Hop.ParentEye;
			if (cancelling) {
				for (const auto &entry : std::span(routes->Targets).first(routes->Count).subspan(1)) {
					const auto duplicate = std::any_of(
						next.CancelHops.begin(),
						next.CancelHops.begin() + next.CancelCount,
						[&](const auto &held) { return held.Hop == entry.Hop; }
					);
					if (!duplicate) next.CancelHops[next.CancelCount++] = entry;
				}
				for (auto &shadow : Shadows)
					if (shadow && shadow->Requester == message.From &&
						shadow->Image.Snapshot.Eye == next.Pull.ParentEye)
						ReleaseProducedShadow(shadow);
				for (auto &held : ShadowRoutes)
					if (held && &*held == routes) {
						held.reset();
						break;
					}
				next.Phase = ShadowTransferPhase::CancelChildren;
			} else if (target == &routes->Targets[0]) {
				const PortalShadowImage *image = nullptr;
				ProducedShadow *held = nullptr;
				for (const auto &shadow : Shadows)
					if (shadow && shadow->Ready && shadow->Requester == message.From &&
						shadow->Image.Snapshot.Eye == next.Pull.TargetEye && ShadowCurrent(*shadow))
						image = &shadow->Image;
				for (auto &shadow : Shadows)
					if (shadow && shadow->Ready && shadow->Requester == message.From &&
						shadow->Image.Snapshot.Eye == next.Pull.TargetEye && ShadowCurrent(*shadow))
						held = &*shadow;
				if (held && next.Pull.BodyBounds && held->Revision == Render.ResourceRevision()) {
					const core::AABB sourceBounds{
						{held->Image.Snapshot.SourceBounds[0],
						 held->Image.Snapshot.SourceBounds[1],
						 held->Image.Snapshot.SourceBounds[2]},
						{held->Image.Snapshot.SourceBounds[3],
						 held->Image.Snapshot.SourceBounds[4],
						 held->Image.Snapshot.SourceBounds[5]}
					};
					const auto domain = held->Image.Snapshot.SourceEmpty
											? *next.Pull.BodyBounds
											: sourceBounds.Union(*next.Pull.BodyBounds);
					if (held->FitBounds && *held->FitBounds != domain) {
						Render.CancelResourceImage(held->FitToken);
						held->FitToken = 0;
						held->FitImage.reset();
						held->FitBounds.reset();
					}
					if (!held->FitBounds) held->FitBounds = domain;
					if (!held->FitToken && !held->FitImage) {
						RebindShadowSnapshot(*held);
						SceneTarget target{held->Width, held->Height};
						auto view = held->Snapshot;
						view.Target = &target;
						view.DirectionalShadowBounds = domain;
						held->FitToken =
							Render.QueueResourceImage(view.Pipeline, ShadowCaptureNode(), view.Slot);
						if (held->FitToken) {
							OverlayImage overlay;
							const auto previousDirection = Render.SunDirection();
							const auto previousAmbient = Render.SunAmbient();
							const auto previousDirect = Render.SunColor();
							Render.SetSun(
								view.Lighting.Direction, view.Lighting.Ambient, view.Lighting.Direct
							);
							(void)Render.RenderViews(std::span(&view, 1), overlay, nullptr, false);
							Render.SetSun(previousDirection, previousAmbient, previousDirect);
							if (held->Revision != Render.ResourceRevision()) {
								Render.CancelResourceImage(held->FitToken);
								held->FitToken = 0;
								held->FitBounds.reset();
							}
						}
					}
					if (held->FitToken) {
						auto captured = Render.TakeResourceImage(held->FitToken);
						if (captured) {
							held->FitToken = 0;
							if (captured->Status == ResourceImageStatus::Ok && captured->Shadow &&
								captured->Depth.size() == PORTAL_SHADOW_BYTES && held->FitBounds &&
								held->Revision == Render.ResourceRevision()) {
								PortalShadowImage fitted = held->Image;
								fitted.Depth = std::move(captured->Depth);
								fitted.Snapshot.DepthHash = assets::Hasher::Of(fitted.Depth);
								fitted.Snapshot.DomainBounds = {
									held->FitBounds->Minimum.X,
									held->FitBounds->Minimum.Y,
									held->FitBounds->Minimum.Z,
									held->FitBounds->Maximum.X,
									held->FitBounds->Maximum.Y,
									held->FitBounds->Maximum.Z
								};
								fitted.Snapshot.LightViewProjection = captured->Shadow->LightViewProjection;
								if (ValidPortalShadowImage(fitted)) held->FitImage = std::move(fitted);
							}
						}
					}
					if (held->FitImage && held->FitBounds && *held->FitBounds == domain)
						image = &*held->FitImage;
				}

				std::vector<std::byte> payload;
				std::string error;
				// A retained raster is valid only for its captured source fit. The producer
				// returns retryable pressure until the exact immutable-view prefit readback lands.
				const bool encoded =
					image && (!next.Pull.BodyBounds || (held && held->FitImage)) &&
					(next.Pull.Part == PORTAL_SHADOW_MANIFEST_PART
						 ? EncodePortalShadowManifest(image->Snapshot, payload, error)
						 : EncodePortalShadowTile(*image, next.Pull.Part - 1, payload, error));
				auto status = PortalImageStatus::Unavailable;
				if (encoded)
					status = PortalImageStatus::Ok;
				else if (held && next.Pull.BodyBounds && held->FitToken)
					status = PortalImageStatus::BudgetExceeded;
				if (!ShadowPacket(next, status, std::move(payload))) return;
			} else {
				std::string error;
				std::vector<std::byte> wire;
				if (!EncodePortalShadowPull(next.Forwarded, wire, error) ||
					!StoreTransferWire(next, std::move(wire)))
					return;
				next.Phase = ShadowTransferPhase::SendChild;
			}
			Transfer = std::move(next);
		}
		void CollectShadowReply(PortalImageSource &source) {
			const auto message = source.TakeShadowReply();
			if (!message) return;
			for (auto &Transfer : Transfers) {
				if (!Transfer || Transfer->Phase != ShadowTransferPhase::WaitChild ||
					message->From != Transfer->Hop.Producer || message->To != Transfer->Hop.Requester ||
					message->Correlation != Transfer->ChildCorrelation)
					continue;
				PortalShadowPacket packet;
				std::string error;
				if (!DecodePortalShadowPacket(message->Payload, packet, error) ||
					packet.Pull != Transfer->Forwarded)
					return;
				if (!ShadowPacket(*Transfer, packet.Status, std::move(packet.Payload))) {
					if (!ShadowPacket(*Transfer, PortalImageStatus::BudgetExceeded)) Transfer.reset();
				}
				return;
			}
		}
		void AdvanceShadowTransfer(
			std::optional<ShadowTransfer> &Transfer, Time now, PortalProducerProgress &progress
		) {
			if (!Transfer) return;
			if (now >= Transfer->Deadline || !TransferCurrent(*Transfer)) {
				Transfer.reset();
				return;
			}
			auto &transfer = *Transfer;
			if (transfer.Phase == ShadowTransferPhase::Done ||
				transfer.Phase == ShadowTransferPhase::WaitChild)
				return;
			if (transfer.Phase == ShadowTransferPhase::CancelChildren) {
				while (transfer.CancelNext < transfer.CancelCount) {
					const auto &entry = transfer.CancelHops[transfer.CancelNext];
					const auto &hop = entry.Hop;
					if (hop.Requester.World != OwnerName.Text() ||
						Universe.LookupPresentation(World, hop.Requester.Channel) != hop.Requester ||
						!CurrentEndpoint(Universe, Borrow(hop.Producer)) ||
						PublishedEndpoint(EndpointBindings, Borrow(hop.Producer)) !=
							BorrowRouteEndpoint(entry.HopProducer)) {
						++transfer.CancelNext;
						transfer.Wire.clear();
						continue;
					}
					if (transfer.Wire.empty()) {
						std::string error;
						std::vector<std::byte> wire;
						if (!EncodePortalShadowPull(
								{hop.ParentEye,
								 entry.HopProducer,
								 hop.ParentEye,
								 {},
								 PORTAL_SHADOW_CANCEL_PART},
								wire,
								error
							) ||
							!StoreTransferWire(transfer, std::move(wire))) {
							Transfer.reset();
							return;
						}
					}
					const std::array<std::string_view, 1> references{entry.HopProducer.World};
					const auto status = Universe.SendPresentation(
						World,
						hop.Requester,
						hop.Producer,
						transfer.ChildCorrelation,
						transfer.Wire,
						world::PresentationPriority::Routine,
						references
					);
					if (status == world::PresentationStatus::Full) return;
					if (status == world::PresentationStatus::Ok) {
						core::Metrics::Count("render.portal_shadow.sent_bytes", transfer.Wire.size());
						++progress.Sent;
					} else
						++progress.Refused;
					++transfer.CancelNext;
					transfer.Wire.clear();
				}
				if (!ShadowPacket(transfer, PortalImageStatus::Ok)) {
					Transfer.reset();
					return;
				}
			}
			const bool child = transfer.Phase == ShadowTransferPhase::SendChild;
			if (child && (transfer.Hop.Requester.World != OwnerName.Text() ||
						  Universe.LookupPresentation(World, transfer.Hop.Requester.Channel) !=
							  transfer.Hop.Requester ||
						  !CurrentEndpoint(Universe, Borrow(transfer.Hop.Producer)))) {
				if (!ShadowPacket(transfer, PortalImageStatus::Unavailable)) Transfer.reset();
				return;
			}
			const std::array<std::string_view, 1> references{
				child ? transfer.Forwarded.TargetProducer.World : transfer.Pull.TargetProducer.World
			};
			const auto status = Universe.SendPresentation(
				World,
				child ? transfer.Hop.Requester : Requests,
				child ? transfer.Hop.Producer : transfer.Requester,
				child ? transfer.ChildCorrelation : transfer.Correlation,
				transfer.Wire,
				world::PresentationPriority::Routine,
				references
			);
			if (status == world::PresentationStatus::Full) return;
			if (status != world::PresentationStatus::Ok) {
				if (!child || !ShadowPacket(transfer, PortalImageStatus::Unavailable)) Transfer.reset();
				++progress.Refused;
				return;
			}
			core::Metrics::Count("render.portal_shadow.sent_bytes", transfer.Wire.size());
			++progress.Sent;
			transfer.Phase = child ? ShadowTransferPhase::WaitChild : ShadowTransferPhase::Done;
			if (child) {
				transfer.LastChildSend = now;
				std::vector<std::byte>().swap(transfer.Wire);
			}
		}
		void AdvanceShadowTransfers(Time now, PortalProducerProgress &progress) {
			for (auto &transfer : Transfers)
				AdvanceShadowTransfer(transfer, now, progress);
		}
		void ExpireShadows(Time now) {
			for (auto &shadow : Shadows)
				if (shadow && shadow->Ready && (now >= shadow->Deadline || !ShadowCurrent(*shadow)))
					ReleaseProducedShadow(shadow);
			for (auto &routes : ShadowRoutes)
				if (routes && routes->Ready && (now >= routes->Deadline || !RoutesCurrent(*routes)))
					routes.reset();
			for (auto &transfer : Transfers)
				if (transfer && (now >= transfer->Deadline || !TransferCurrent(*transfer))) transfer.reset();
		}
		static size_t ShadowSnapshotBytes(const ProducedShadow &shadow) {
			return shadow.Rows.capacity() * sizeof(scene::DrawInstance) +
				   shadow.ForeignRows.capacity() * sizeof(scene::DrawInstance) +
				   shadow.Joints.capacity() * sizeof(core::CFrame) +
				   shadow.ForeignJoints.capacity() * sizeof(core::CFrame) +
				   shadow.Surfaces.capacity() * sizeof(SurfaceView) +
				   shadow.ContentOwners.capacity() * sizeof(WorldContentOwner) +
				   shadow.Particles.capacity() * sizeof(ParticleBatch) +
				   shadow.ParticleSeams.capacity() * sizeof(ParticleSeam) +
				   shadow.RibbonVertices.capacity() * sizeof(effects::RibbonVertex) +
				   shadow.RibbonRuns.capacity() * sizeof(effects::RibbonRun) +
				   shadow.Lights.capacity() * sizeof(SceneLight) +
				   shadow.Portals.capacity() * sizeof(PortalView) +
				   shadow.EyeHiddenRows.capacity() * sizeof(uint32_t);
		}
		static void RebindShadowSnapshot(ProducedShadow &shadow) {
			shadow.Snapshot.Instances = shadow.Rows;
			shadow.Snapshot.Foreign = shadow.ForeignRows;
			shadow.Snapshot.JointFrames = shadow.Joints;
			shadow.Snapshot.ForeignJointFrames = shadow.ForeignJoints;
			shadow.Snapshot.Surfaces = shadow.Surfaces;
			shadow.Snapshot.ForeignContentOwners = shadow.ContentOwners;
			shadow.Snapshot.Particles = shadow.Particles;
			shadow.Snapshot.ParticleSeams = shadow.ParticleSeams;
			shadow.Snapshot.RibbonVertices = shadow.RibbonVertices;
			shadow.Snapshot.RibbonRuns = shadow.RibbonRuns;
			shadow.Snapshot.Lights = shadow.Lights;
			shadow.Snapshot.Portals = shadow.Portals;
			shadow.Snapshot.EyeHiddenRows = shadow.EyeHiddenRows;
			shadow.Snapshot.Target = nullptr;
		}
		bool ReserveShadow(Capture &capture, const View &view) {
			for (const auto &shadow : Shadows)
				if (shadow && shadow->Requester == capture.ReplyTo &&
					shadow->Image.Snapshot.Eye == capture.Reply.Key)
					return false;
			for (size_t index = 0; index < Shadows.size(); ++index) {
				if (Shadows[index]) continue;
				ProducedShadow shadow;
				shadow.Requester = capture.ReplyTo;
				shadow.Deadline = capture.Deadline;
				shadow.Image.Snapshot.Producer = capture.Producer;
				shadow.Image.Snapshot.Eye = capture.Reply.Key;
				shadow.Image.Snapshot.ExcludedPlayer = capture.RetainedBodyPlayer;
				shadow.Snapshot = view;
				shadow.Rows.assign(view.Instances.begin(), view.Instances.end());
				shadow.ForeignRows.assign(view.Foreign.begin(), view.Foreign.end());
				shadow.Joints.assign(view.JointFrames.begin(), view.JointFrames.end());
				shadow.ForeignJoints.assign(view.ForeignJointFrames.begin(), view.ForeignJointFrames.end());
				shadow.Surfaces.assign(view.Surfaces.begin(), view.Surfaces.end());
				shadow.ContentOwners.assign(
					view.ForeignContentOwners.begin(), view.ForeignContentOwners.end()
				);
				shadow.Particles.assign(view.Particles.begin(), view.Particles.end());
				shadow.ParticleSeams.assign(view.ParticleSeams.begin(), view.ParticleSeams.end());
				shadow.RibbonVertices.assign(view.RibbonVertices.begin(), view.RibbonVertices.end());
				shadow.RibbonRuns.assign(view.RibbonRuns.begin(), view.RibbonRuns.end());
				shadow.Lights.assign(view.Lights.begin(), view.Lights.end());
				shadow.Portals.assign(view.Portals.begin(), view.Portals.end());
				shadow.EyeHiddenRows.assign(view.EyeHiddenRows.begin(), view.EyeHiddenRows.end());
				RebindShadowSnapshot(shadow);
				if (ShadowSnapshotBytes(shadow) > MAX_PORTAL_EXCHANGE_BYTES) return false;
				shadow.Width = view.Target->Width;
				shadow.Height = view.Target->Height;
				shadow.Revision = Render.ResourceRevision();
				if (!ShadowCurrent(shadow)) return false;
				Shadows[index] = std::move(shadow);
				capture.ShadowSlot = index;
				return true;
			}
			return false;
		}
		void DenyRetainedBody(Capture &capture) {
			Cancel(capture);
			const auto key = capture.Reply.Key;
			const auto scope = capture.Reply.Scope;
			capture.Reply = {};
			capture.Reply.Key = key;
			capture.Reply.Scope = scope;
			capture.Reply.Status = PortalImageStatus::Unavailable;
			capture.Reply.Diagnostic = "retained body exclusion is not authorized for requester";
			capture.Wire.clear();
			capture.Tree.reset();
		}

		world::PresentationStatus Send(Capture &capture, PortalProducerProgress &progress) {
			if (capture.Reply.Status == PortalImageStatus::Ok &&
				!Authorized(capture.ReplyTo, capture.RetainedBodyPlayer))
				DenyRetainedBody(capture);
			if (capture.Reply.Status == PortalImageStatus::Ok && capture.ShadowSlot &&
				(!Shadows[*capture.ShadowSlot] || !ShadowCurrent(*Shadows[*capture.ShadowSlot])))
				DenyRetainedBody(capture);
			if (capture.Reply.Status == PortalImageStatus::Ok && capture.RouteSlot &&
				(!ShadowRoutes[*capture.RouteSlot] || !RoutesCurrent(*ShadowRoutes[*capture.RouteSlot])))
				DenyRetainedBody(capture);
			if (capture.Reply.Status != PortalImageStatus::Ok) {
				ReleaseShadow(capture);
				ReleaseRoutes(capture);
			}
			if (capture.Wire.empty()) {
				if (capture.Reply.Status != PortalImageStatus::Ok) {
					capture.Reply.CaptureLighting.reset();
					capture.Tree.reset();
				}
				std::string error;
				if (!EncodePortalImageReply(capture.Reply, capture.Wire, error)) {
					++progress.Refused;
					return world::PresentationStatus::Invalid;
				}
				// Keep one canonical owned image while transport is full. Retrying
				// does not hash, encode, copy GPU pixels or redraw the world again.
				std::vector<std::byte>().swap(capture.Reply.Pixels);
				std::vector<std::byte>().swap(capture.Reply.Depth);
				std::vector<std::byte>().swap(capture.Reply.Normal);
				std::vector<std::byte>().swap(capture.Reply.AmbientResponse);
				std::vector<std::byte>().swap(capture.Reply.LightingBaseline);
				std::vector<std::byte>().swap(capture.Reply.DirectionalResponse);
			}
			const auto priority = capture.TransferEye ? world::PresentationPriority::TransferEye
													  : world::PresentationPriority::Routine;
			std::array<std::string_view, 1 + MAX_CAPTURE_WORLD_DEPENDENCIES + MAX_PORTAL_CAPTURE_TREE_NODES>
				references;
			std::optional<std::span<const std::string_view>> worldReferences;
			if (capture.WorldDependenciesKnown && (capture.WorldProvenanceComplete || capture.Tree) &&
				(!capture.Tree || capture.Tree->Nodes.size() <= MAX_PORTAL_CAPTURE_TREE_NODES)) {
				size_t count = 0;
				if (!capture.Producer.World.empty()) references[count++] = capture.Producer.World;
				for (const auto &dependency : capture.WorldDependencies)
					references[count++] = dependency;
				if (capture.Tree)
					for (const auto &node : capture.Tree->Nodes)
						references[count++] = node.Producer.World;
				worldReferences = std::span(references).first(count);
			}
			const auto status = Universe.SendPresentation(
				World,
				Requests,
				capture.ReplyTo,
				capture.Reply.Key.RequestId,
				capture.Wire,
				priority,
				worldReferences
			);
			if (EyeDiagnostic && capture.Reply.Key.PortalKey == "viewport-eye" && EyeDiagnosticCount++ < 64)
				ENGINE_WARN(
					"portal eye producer send world={} request={} reply_status={} transport={} bytes={} "
					"diagnostic={}",
					Requests.World,
					capture.Reply.Key.RequestId,
					static_cast<int>(capture.Reply.Status),
					static_cast<int>(status),
					capture.Wire.size(),
					capture.Reply.Diagnostic
				);
			if (status == world::PresentationStatus::Ok) {
				if (capture.ShadowSlot) {
					auto &shadow = *Shadows[*capture.ShadowSlot];
					shadow.Ready = true;
					shadow.Deadline = *LastTime + CAPTURE_TIMEOUT;
					capture.ShadowSlot.reset();
				}
				if (capture.RouteSlot) {
					auto &routes = *ShadowRoutes[*capture.RouteSlot];
					routes.Ready = true;
					routes.Deadline = *LastTime + CAPTURE_TIMEOUT;
					capture.RouteSlot.reset();
				}
				++progress.Sent;
				ENGINE_TRACE(
					"portal image sent: {} {} request {} bytes {} diagnostic {}",
					capture.ReplyTo.Channel,
					Requests.World,
					capture.Reply.Key.RequestId,
					capture.Wire.size(),
					capture.Reply.Diagnostic
				);
			} else {
				++progress.Refused;
			}
			return status;
		}
		void SendFailure(Capture &capture, PortalProducerProgress &progress) {
			// Failed requests need delivery too. Bound retries separately so a full
			// reply mailbox cannot consume the GPU capture slots.
			if (Send(capture, progress) == world::PresentationStatus::Full &&
				FailureReplies.size() < MAX_WAITING_CAPTURES) {
				FailureReplies.push_back(std::move(capture));
			}
		}
	};
}

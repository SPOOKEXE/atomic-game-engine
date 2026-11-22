// Private state shared by the PortalImageSource and PortalImageProducer sources: the
// endpoint, reply slot and capture pipeline helpers, and PortalImageSource::Impl.
#pragma once

#include "PortalRenderOperations.hpp"
#include "TransparentLayerWork.hpp"
#include "portal/PortalCaptureEntrance.hpp"
#include "portal/PortalCaptureTreeImport.hpp"
#include "portal/PortalImageBudget.hpp"
#include "portal/PortalPresentationClock.hpp"
#include "portal/PortalResidentImages.hpp"
#include "portal/PortalShadowTransport.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/interface/InterfacePass.hpp>
#include <engine/render/interface/Overlay.hpp>
#include <engine/render/portal/PortalGeometryDraw.hpp>
#include <engine/render/portal/PortalImageDemand.hpp>
#include <engine/render/portal/PortalImageRuntime.hpp>
#include <engine/render/presentation/WorldPresentation.hpp>
#include <engine/render/presentation/WorldView.hpp>
#include <engine/render/resources/EditableImages.hpp>
#include <engine/render/resources/EditableMeshes.hpp>
#include <engine/render/resources/ShaderLibrary.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/cameras/Visibility.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <new>

namespace engine::render {
	inline namespace portal_image_runtime {
		inline PortalEndpointView
		PublishedEndpoint(const world::PresentationBindings *bindings, PortalEndpointView local) {
			if (!bindings) return local;
			for (const auto *pairs : {&bindings->Exports, &bindings->Returns})
				for (const auto &pair : *pairs)
					if (PortalEndpointView{
							pair.Local.World, pair.Local.Channel, pair.Local.Session, pair.Local.Generation
						} == local)
						return {
							pair.Published.World,
							pair.Published.Channel,
							pair.Published.Session,
							pair.Published.Generation
						};
			return local;
		}
		inline bool CurrentEndpoint(world::Universe &universe, PortalEndpointView endpoint) {
			if (endpoint.Channel != PORTAL_REQUEST_CHANNEL) return false;
			for (const auto world : universe.Worlds()) {
				if (universe.NameOf(world).Text() != endpoint.World) continue;
				const auto current = universe.LookupPresentation(world, endpoint.Channel);
				return PortalEndpointView{
						   current.World, current.Channel, current.Session, current.Generation
					   } == endpoint;
			}
			return false;
		}
		inline bool CurrentPublishedEndpoint(
			world::Universe &universe,
			const world::PresentationBindings *bindings,
			PortalEndpointView endpoint
		) {
			bool mapped = false;
			if (bindings) {
				for (const auto *pairs : {&bindings->Exports, &bindings->Returns})
					for (const auto &pair : *pairs) {
						if (PortalEndpointView{
								pair.Published.World,
								pair.Published.Channel,
								pair.Published.Session,
								pair.Published.Generation
							} != endpoint)
							continue;
						mapped = true;
						if (CurrentEndpoint(
								universe,
								{pair.Local.World,
								 pair.Local.Channel,
								 pair.Local.Session,
								 pair.Local.Generation}
							))
							return true;
					}
			}
			return !mapped && PublishedEndpoint(bindings, endpoint) == endpoint &&
				   CurrentEndpoint(universe, endpoint);
		}
		inline bool KnownPublishedEndpoint(
			world::Universe &universe,
			const world::PresentationBindings *bindings,
			PortalEndpointView endpoint
		) {
			if (bindings)
				for (const auto *pairs : {&bindings->Exports, &bindings->Returns})
					for (const auto &pair : *pairs)
						if (pair.Published.World == endpoint.World &&
							pair.Published.Channel == endpoint.Channel)
							return true;
			for (const auto world : universe.Worlds())
				if (universe.NameOf(world).Text() == endpoint.World) return true;
			return false;
		}
		constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds(1);
		constexpr auto SHADOW_TRANSFER_TIMEOUT = std::chrono::seconds(10);
		constexpr size_t MAX_CAPTURES = 4;
		static_assert(MAX_PORTAL_TRANSPARENT_LAYERS + 2 == MAX_CAPTURES);
		constexpr size_t MAX_WAITING_CAPTURES = 16;
		constexpr std::string_view NESTED_REPLY_CHANNEL = "portal-image-replies/nested/";
		inline PortalEndpointView Borrow(const world::PresentationAddress &address) {
			return {address.World, address.Channel, address.Session, address.Generation};
		}
		inline bool
		Local(world::Universe &universe, world::WorldId world, const world::PresentationAddress &address) {
			return world.IsValid() && !universe.IsRemote(world) &&
				   universe.NameOf(world).Text() == address.World && address.Session != 0 &&
				   address.Generation != 0;
		}
		inline std::optional<size_t> ReplySlot(std::string_view channel) {
			if (channel.starts_with(NESTED_REPLY_CHANNEL)) {
				channel.remove_prefix(NESTED_REPLY_CHANNEL.size());
				size_t slot = 0;
				const auto [end, error] =
					std::from_chars(channel.data(), channel.data() + channel.size(), slot);
				if (channel.empty() || (channel.size() > 1 && channel.front() == '0') ||
					error != std::errc{} || end != channel.data() + channel.size() ||
					slot >= MAX_WAITING_CAPTURES)
					return {};
				// Captures run serially in renderer slot zero. The reply endpoint and
				// the prefixed portal key isolate each parent's imported images.
				return 0;
			}
			if (channel == PORTAL_REPLY_CHANNEL) {
				return 0;
			}
			if (!channel.starts_with(PORTAL_REPLY_CHANNEL)) {
				return {};
			}
			channel.remove_prefix(PORTAL_REPLY_CHANNEL.size());
			if (channel.size() < 2 || channel.front() != '/' || channel[1] == '0') {
				return {};
			}
			channel.remove_prefix(1);
			size_t slot = 0;
			const auto [end, error] = std::from_chars(channel.data(), channel.data() + channel.size(), slot);
			return error == std::errc{} && end == channel.data() + channel.size()
					   ? std::optional<size_t>(slot)
					   : std::nullopt;
		}
		inline bool Clock(
			std::optional<PortalImageInbox::Time> &previous,
			PortalImageInbox::Time now,
			std::chrono::steady_clock::duration timeout = CAPTURE_TIMEOUT
		) {
			if (timeout <= std::chrono::steady_clock::duration::zero() || timeout > std::chrono::hours(1) ||
				(previous && now < *previous) || now > PortalImageInbox::Time::max() - timeout) {
				return false;
			}
			previous = now;
			return true;
		}
		inline core::Name CapturePipeline(PortalImageScope scope, bool ordered = false) {
			if (ordered) return core::Name("portal-image-layers");
			if (scope == PortalImageScope::CompleteWorld) return core::Name("portal-image-world");
			return core::Name(
				scope == PortalImageScope::SeamRadiance ? "portal-image-seam-radiance" : "portal-image-opaque"
			);
		}
		inline core::Name CaptureNode() {
			return core::Name("portal-image-export");
		}
		inline core::Name ShadowCaptureNode() {
			return core::Name("portal-shadow-export");
		}
		inline core::Name LayerCaptureNode(size_t layer) {
			return core::Name("portal-layer-" + std::to_string(layer) + "-export");
		}
		inline size_t CaptureProfile(const PortalImageRequest &request) {
			return request.OrderedLayers ? 3 : static_cast<size_t>(request.Scope);
		}
		inline bool InstallCapture(PortalRenderOperations &renderer, PortalImageScope scope, bool ordered) {
			if (!renderer.HasDevice()) {
				return false;
			}
			const auto base = scope == PortalImageScope::CompleteWorld ? graph::DefaultWorldHdrDocument()
																	   : graph::DefaultPbrDocument();
			graph::PipelineDocument document;
			const bool opaque = scope != PortalImageScope::CompleteWorld;
			if (ordered)
				for (const char *name : {"lighting-baseline", "directional-response"})
					document.Record(
						{.Kind = graph::EditKind::AddResource,
						 .Name = core::Name(name),
						 .Resource = graph::ResourceKind::Colour,
						 .Format = graph::ResourceFormat::RGBA32F}
					);
			const core::Name depthBoundary(opaque ? "sky" : "present");
			for (const auto &edit : base.Edits()) {
				if (edit.Kind == graph::EditKind::AddNode && edit.Name == depthBoundary) {
					document.Record(
						{.Kind = graph::EditKind::AddResource,
						 .Name = core::Name("portal-depth"),
						 .Resource = graph::ResourceKind::Colour,
						 .Format = graph::ResourceFormat::R32F}
					);
					document.Record(
						{.Kind = graph::EditKind::AddNode,
						 .Name = core::Name("portal-depth-linearise"),
						 .NodeKind = core::Name("depth-linearise"),
						 .Scope = graph::NodeScope::View}
					);
					document.Record(
						{.Kind = graph::EditKind::Reads,
						 .Target = core::Name("depth"),
						 .Key = core::Name("depth")}
					);
					document.Record(
						{.Kind = graph::EditKind::Writes,
						 .Target = core::Name("portal-depth"),
						 .Key = core::Name("linear")}
					);
					document.Record(
						{.Kind = graph::EditKind::Set, .Key = core::Name("background"), .Value = "zero"}
					);
					// Opaque radiance and depth must share rays before later spatial effects.
					if (opaque) break;
				}
				if (ordered && edit.Kind == graph::EditKind::AddResource &&
					edit.Name == core::Name("depth")) {
					auto depth = edit;
					depth.Format = graph::ResourceFormat::D32F;
					document.Record(depth);
				} else
					document.Record(edit);
				if (ordered && edit.Kind == graph::EditKind::Writes && edit.Target == core::Name("lit"))
					for (const char *name : {"lighting-baseline", "directional-response"})
						document.Record(
							{.Kind = graph::EditKind::Writes,
							 .Target = core::Name(name),
							 .Key = core::Name(name)}
						);
			}
			if (ordered) {
				document.Record(
					{.Kind = graph::EditKind::AddResource,
					 .Name = core::Name("ambient-response"),
					 .Resource = graph::ResourceKind::Colour,
					 .Format = graph::ResourceFormat::RGBA32F}
				);
				document.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = core::Name("ambient-response"),
					 .NodeKind = core::Name("ambient-response"),
					 .Scope = graph::NodeScope::View}
				);
				for (const auto &[resource, port] : std::array{
						 std::pair{"albedo", "albedo"},
						 std::pair{"normal", "normal"},
						 std::pair{"material", "material"},
						 std::pair{"linear-depth", "depth"},
						 std::pair{"occlusion", "occlusion"}
					 })
					document.Record(
						{.Kind = graph::EditKind::Reads,
						 .Target = core::Name(resource),
						 .Key = core::Name(port)}
					);
				document.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("ambient-response"),
					 .Key = core::Name("response")}
				);
			}
			if (ordered) {
				const auto edge = [&](graph::EditKind kind, const std::string &target, const char *port) {
					document.Record({.Kind = kind, .Target = core::Name(target), .Key = core::Name(port)});
				};
				for (const bool depth : {false, true})
					document.Record(
						{.Kind = graph::EditKind::AddResource,
						 .Name = core::Name(depth ? "portal-overlay-z" : "portal-overlay-colour"),
						 .Resource = depth ? graph::ResourceKind::Depth : graph::ResourceKind::Colour,
						 .Format = depth ? graph::ResourceFormat::D32F : graph::ResourceFormat::RGBA16F}
					);
				document.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = core::Name("spatial-overlay"),
					 .NodeKind = core::Name("spatial-overlay"),
					 .Scope = graph::NodeScope::View}
				);
				edge(graph::EditKind::Writes, "portal-overlay-colour", "colour");
				edge(graph::EditKind::Writes, "portal-overlay-z", "z");

				for (size_t layer = 0; layer <= MAX_PORTAL_TRANSPARENT_LAYERS; ++layer) {
					const auto name = "portal-layer-" + std::to_string(layer);
					for (const auto &[suffix, format] : std::array{
							 std::pair{"", graph::ResourceFormat::RGBA16F},
							 std::pair{"-depth", graph::ResourceFormat::R32F},
							 std::pair{"-z", graph::ResourceFormat::D32F},
							 std::pair{"-interface", graph::ResourceFormat::RGBA16F},
							 std::pair{"-interface-z", graph::ResourceFormat::D32F}
						 }) {
						document.Record(
							{.Kind = graph::EditKind::AddResource,
							 .Name = core::Name(name + suffix),
							 .Resource = format == graph::ResourceFormat::D32F ? graph::ResourceKind::Depth
																			   : graph::ResourceKind::Colour,
							 .Format = format}
						);
					}
					document.Record(
						{.Kind = graph::EditKind::AddNode,
						 .Name = core::Name(name),
						 .NodeKind = core::Name("transparent-layer"),
						 .Scope = graph::NodeScope::View}
					);
					edge(graph::EditKind::Reads, "depth", "opaque-z");
					if (layer > 0)
						edge(
							graph::EditKind::Reads,
							"portal-layer-" + std::to_string(layer - 1) + "-z",
							"previous-z"
						);
					edge(graph::EditKind::Reads, "ordered-entities", "entities");
					edge(graph::EditKind::Reads, "view-instances", "instances");
					edge(graph::EditKind::Reads, "shadow", "shadow");
					edge(graph::EditKind::Writes, name, "colour");
					edge(graph::EditKind::Writes, name + "-depth", "depth");
					edge(graph::EditKind::Writes, name + "-z", "z");
					edge(graph::EditKind::Writes, name + "-interface", "interface-colour");
					edge(graph::EditKind::Writes, name + "-interface-z", "interface-z");
				}
				document.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = ShadowCaptureNode(),
					 .NodeKind = core::Name("shadow-capture"),
					 .Scope = graph::NodeScope::View}
				);
				edge(graph::EditKind::Reads, "shadow", "shadow");
				document.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = core::Name("portal-overlay-export"),
					 .NodeKind = core::Name("capture"),
					 .Scope = graph::NodeScope::Frame}
				);
				edge(graph::EditKind::Reads, "portal-overlay-colour", "source");
				for (size_t layer = 0; layer <= MAX_PORTAL_TRANSPARENT_LAYERS; ++layer) {
					const auto name = "portal-layer-" + std::to_string(layer);
					document.Record(
						{.Kind = graph::EditKind::AddNode,
						 .Name = LayerCaptureNode(layer),
						 .NodeKind = core::Name("capture"),
						 .Scope = graph::NodeScope::Frame}
					);
					edge(graph::EditKind::Reads, name, "source");
					edge(graph::EditKind::Reads, name + "-depth", "depth");
				}
			}
			// The capture backend is frame-scoped. Each bounded request renders
			// its own view, preserving its camera-dependent shadow setup as well.
			document.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = CaptureNode(),
				 .NodeKind = core::Name("capture"),
				 .Scope = graph::NodeScope::Frame}
			);
			document.Record(
				{.Kind = graph::EditKind::Reads,
				 .Target = core::Name(opaque ? "lit" : "lens-b"),
				 .Key = core::Name("source")}
			);
			document.Record(
				{.Kind = graph::EditKind::Reads,
				 .Target = core::Name("portal-depth"),
				 .Key = core::Name("depth")}
			);
			if (ordered)
				for (const char *name :
					 {"normal", "ambient-response", "lighting-baseline", "directional-response"})
					document.Record(
						{.Kind = graph::EditKind::Reads, .Target = core::Name(name), .Key = core::Name(name)}
					);
			graph::RenderGraph pipeline;
			core::Name offender;
			return graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok &&
				   renderer.SetPipeline(CapturePipeline(scope, ordered), pipeline);
		}

		inline bool CameraOf(const PortalCaptureCamera &request, PortalImageProjection kind, View &view) {
			const auto &p = request.Position;
			const auto &q = request.Orientation;
			view.CameraFrame =
				core::CFrame(core::Vector3(p[0], p[1], p[2]), glm::quat(q[3], q[0], q[1], q[2]));
			const auto &f = request.Frustum;
			const auto &c = request.ClipPlane;
			// A camera on the seam would make SurfaceProjection omit the plane.
			// Refuse it rather than render geometry outside the requested half-space.
			const float side = c[0] * p[0] + c[1] * p[1] + c[2] * p[2] + c[3];
			if (kind == PortalImageProjection::Seam && (!std::isfinite(side) || side >= -1e-4f)) {
				return false;
			}
			scene::SurfaceLens lens;
			lens.Left = f[0];
			lens.Right = f[1];
			lens.Bottom = f[2];
			lens.Top = f[3];
			lens.NearPlane = f[4];
			lens.FarPlane = f[5];
			lens.ClipNormal = {c[0], c[1], c[2]};
			lens.ClipDistance = -c[3];
			// Layout consumes a scalar vertical FOV even when rendering uses an
			// off-axis projection. Preserve that projection's vertical focal scale.
			view.Camera.FieldOfViewRadians =
				float(2.0 * std::atan((double(f[3]) - double(f[2])) / (2.0 * double(f[4]))));
			view.Camera.NearPlane = f[4];
			view.Camera.FarPlane = f[5];
			const auto projection = scene::SurfaceProjection(lens, view.CameraFrame);
			for (int column = 0; column < 4; ++column) {
				for (int row = 0; row < 4; ++row) {
					if (!std::isfinite(projection[column][row])) {
						return false;
					}
				}
			}
			const float determinant = glm::determinant(projection);
			if (!std::isfinite(determinant) || determinant == 0) {
				return false;
			}
			view.Projection = projection;
			return true;
		}
	}

	struct PortalImageSource::Impl {
		Impl(
			world::Universe &universe,
			Renderer &renderer,
			world::WorldId world,
			world::PresentationAddress replies,
			PortalInboxLimits limits,
			PortalResidentImages *resident,
			PortalImageSourceDelivery delivery
		)
			: Universe(universe), Render(renderer), World(world), Replies(std::move(replies)), Inbox(limits),
			  Limits(limits), Resident(resident), Delivery(delivery), ViewSlot(ReplySlot(Replies.Channel)) {
			PinnedCaptures.reserve(MAX_PORTAL_CAPTURE_TREE_LEASES);
		}

		world::Universe &Universe;
		PortalRenderOperations Render;
		world::WorldId World;
		world::PresentationAddress Replies;
		PortalImageInbox Inbox;
		PortalInboxLimits Limits;
		PortalResidentImages *Resident = nullptr;
		PortalImageSourceDelivery Delivery = PortalImageSourceDelivery::ImportedImages;
		const world::PresentationBindings *EndpointBindings = nullptr;
		std::optional<size_t> ViewSlot = ReplySlot(Replies.Channel);
		bool EyeDiagnostic = std::getenv("PORTAL_EYE_SOURCE_DIAGNOSTIC") != nullptr;
		size_t EyeDiagnosticCount = 0;
		std::optional<world::PresentationMessage> ShadowReply;
		PortalCaptureTreeEndpoint ShadowReplyTarget;
		bool ShadowReplyTargetWasCurrent = false;
		std::optional<Time> LastTime;
		bool EndpointCurrent(PortalEndpointView endpoint) const {
			return CurrentPublishedEndpoint(Universe, EndpointBindings, endpoint);
		}
		struct Demand {
			PortalImageRequest Request;
			PortalImageRequest Normalized;
			PortalImageBinding Binding;
			Time Deadline;
		};
		struct Preview {
			world::PresentationAddress Producer;
			PortalImageBinding Binding;
			uint64_t Pending = 0;
			uint64_t Handle = 0;
			Time PendingDeadline;
			Time ImageDeadline;
			Time AcceptedAt{};
			std::optional<Time> CapturePinDeadline;
			bool CapturePinned = false;
			uint64_t CaptureLease = 0;
			std::optional<PortalResidentReceipt> ImageVersion;
			std::optional<PortalImageVersion> OfferedVersion;
			std::string EyePlayer;
			PortalCaptureCamera PendingCamera{};
			PortalImageRequest PendingRequest{};
			std::optional<Demand> Deferred;
			PortalImageBinding CapturedBinding{};
			PortalCaptureCamera CapturedCamera{};
			world::PresentationAddress CapturedProducer;
			std::string CapturedEyePlayer{};
			std::string CapturedEntranceWorld{};
			bool CapturedWorldProvenanceKnown = true;
			std::string RetainedBodyPlayer{}, CapturedRetainedBodyPlayer{};
			bool OrderedLayers = false;
			bool PayloadReady = false;
			world::PresentationAddress ExpectedProducer;
			uint64_t Tree = 0, UploadTree = 0;
			std::vector<PortalCaptureTreeEndpoint> TreeProducers, UploadTreeProducers;
			std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS> TransparentImages{};
			uint64_t SpatialOverlayImage = 0;
			PortalCaptureLenses CapturedLenses;
			PortalCaptureLenses UploadLenses;
			uint64_t LensPrograms = 0;
			uint64_t UploadLensPrograms = 0;
			std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS + 2> UploadImages{};
			std::optional<PortalResidentReceipt> UploadVersion;
			void CaptureAccepted(Time now) {
				AcceptedAt = now;
				if (CaptureLease == 0) {
					CapturePinDeadline.reset();
					CapturePinned = false;
				}
				CapturedBinding = Binding;
				CapturedCamera = PendingCamera;
				CapturedProducer = Producer;
				CapturedEyePlayer = EyePlayer;
				if (PendingRequest.Entrance)
					CapturedEntranceWorld = std::move(PendingRequest.Entrance->SourceWorld);
				else
					CapturedEntranceWorld.clear();
				CapturedWorldProvenanceKnown = PendingRequest.RecursionDepth == 0 || Tree != 0;
				CapturedRetainedBodyPlayer = RetainedBodyPlayer;
			}
		};
		static bool SameRequestDemand(const PortalImageRequest &left, const PortalImageRequest &right) {
			PortalImageRequest normalizedLeft = left;
			normalizedLeft.Key.RequestId = 0;
			normalizedLeft.KnownImage.reset();
			PortalImageRequest normalizedRight = right;
			normalizedRight.Key.RequestId = 0;
			normalizedRight.KnownImage.reset();
			return normalizedLeft == normalizedRight;
		}
		static bool SameBindingProfile(const PortalImageBinding &left, const PortalImageBinding &right) {
			return left.World == right.World && left.WorldName == right.WorldName &&
				   left.ViewSlot == right.ViewSlot && left.Portal == right.Portal &&
				   left.Index == right.Index && left.Layer == right.Layer;
		}
		static bool SameBindingDemand(const PortalImageBinding &left, const PortalImageBinding &right) {
			if (!SameBindingProfile(left, right)) return false;
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					if (left.Sampling[column][row] != right.Sampling[column][row]) return false;
			return true;
		}
		std::vector<Preview> Previews;
		struct PinnedCapture {
			uint64_t Tree = 0, Lease = 0;
			Time Deadline;
			core::Name Portal;
			world::PresentationAddress Producer;
			std::vector<PortalCaptureTreeEndpoint> Producers;
			std::string EntranceWorld;
			bool WorldProvenanceKnown = true;
		};
		std::vector<PinnedCapture> PinnedCaptures;
		void PreserveCaptureLease(Preview &preview) {
			if (preview.CaptureLease == 0) return;
			if (!preview.CapturePinDeadline) {
				ReleaseCaptureLease(preview);
				return;
			}
			PinnedCaptures.push_back(
				{preview.Tree,
				 preview.CaptureLease,
				 *preview.CapturePinDeadline,
				 preview.Binding.Portal,
				 std::move(preview.CapturedProducer),
				 std::move(preview.TreeProducers),
				 std::move(preview.CapturedEntranceWorld),
				 preview.CapturedWorldProvenanceKnown}
			);
			preview.CaptureLease = 0;
			preview.CapturePinned = false;
			preview.CapturePinDeadline.reset();
		}
		void ReleaseCaptureLease(Preview &preview) {
			Render.ReleasePortalCaptureTreeLease(preview.CaptureLease);
			preview.CaptureLease = 0;
			preview.CapturePinned = false;
		}
		void RetirePinnedCaptures(Time now) {
			std::erase_if(PinnedCaptures, [&](PinnedCapture &capture) {
				const auto retired = [&](const auto &producer) {
					return !EndpointCurrent(
						{producer.World, producer.Channel, producer.Session, producer.Generation}
					);
				};
				const bool localRetired = !CurrentEndpoint(Universe, Borrow(capture.Producer));
				if (now < capture.Deadline && !localRetired &&
					!std::any_of(capture.Producers.begin(), capture.Producers.end(), retired))
					return false;
				if (localRetired || std::any_of(capture.Producers.begin(), capture.Producers.end(), retired))
					Render.DropPortalCaptureTree(capture.Tree);
				Render.ReleasePortalCaptureTreeLease(capture.Lease);
				return true;
			});
		}
		void CancelUpload(Preview &preview) {
			Render.DropPortalCaptureTree(preview.UploadTree);
			preview.UploadTree = 0;
			preview.UploadTreeProducers.clear();
			Render.ReleasePortalLensPrograms(preview.UploadLensPrograms);
			preview.UploadLensPrograms = 0;
			preview.UploadLenses = {};
			if (preview.UploadImages[0] != 0) Render.ReleaseImage(preview.UploadImages[0]);
			preview.UploadImages = {};
			preview.UploadVersion.reset();
		}
		void Drop(Preview &preview) {
			ReleaseCaptureLease(preview);
			preview.CapturePinDeadline.reset();
			CancelUpload(preview);
			Render.DropPortalCaptureTree(preview.Tree);
			preview.Tree = 0;
			Render.ReleasePortalLensPrograms(preview.LensPrograms);
			preview.LensPrograms = 0;
			if (Resident != nullptr) {
				Resident->Cancel(Replies, preview.Pending);
			}
			Inbox.CancelRequest(preview.Pending);
			if (preview.Handle != 0) {
				Render.ReleaseImage(preview.Handle);
			}
			preview.Deferred.reset();
		}
		void ReleaseImageOwner(Preview &preview) {
			PreserveCaptureLease(preview);
			if (preview.Tree != 0)
				Render.ReleasePortalCaptureTree(preview.Tree);
			else if (preview.Handle != 0)
				Render.ReleaseImage(preview.Handle);
			preview.Tree = 0;
			preview.Handle = 0;
			preview.CapturedProducer = {};
		}
		void RetireImage(Preview &preview) {
			Render.DropPortalCaptureTree(preview.Tree);
			ReleaseCaptureLease(preview);
			preview.CapturePinDeadline.reset();
			Render.ReleaseImage(preview.Handle);
			Render.ReleasePortalLensPrograms(preview.LensPrograms);
			preview.Handle = 0;
			preview.Tree = 0;
			preview.TreeProducers.clear();
			preview.TransparentImages = {};
			preview.SpatialOverlayImage = 0;
			preview.CapturedLenses = {};
			preview.LensPrograms = 0;
			preview.ImageVersion.reset();
		}
		void CancelPending(Preview &preview) {
			CancelUpload(preview);
			if (Resident) Resident->Cancel(Replies, preview.Pending);
			Inbox.CancelRequest(preview.Pending);
			preview.Pending = 0;
			preview.Deferred.reset();
		}
		void Expire(Time now) {
			Inbox.Expire(now);
			RetirePinnedCaptures(now);
			if (Resident != nullptr) {
				Resident->Expire(now);
			}
			std::erase_if(Previews, [&](Preview &preview) {
				if (preview.Deferred && now >= preview.Deferred->Deadline) preview.Deferred.reset();
				if (preview.CapturePinned &&
					(preview.Tree == 0 || Render.FindPortalCaptureTree(preview.Tree) == nullptr))
					RetireImage(preview);
				if (preview.CapturePinDeadline && now >= *preview.CapturePinDeadline) {
					ReleaseCaptureLease(preview);
				}
				const auto retired = [&](const auto &producer) {
					return !EndpointCurrent(
						{producer.World, producer.Channel, producer.Session, producer.Generation}
					);
				};
				if (std::any_of(preview.TreeProducers.begin(), preview.TreeProducers.end(), retired))
					if (EyeDiagnostic && ViewSlot && *ViewSlot == 5 && EyeDiagnosticCount++ < 256)
						ENGINE_WARN(
							"portal source retire slot=5 reason=tree-producer handle={}", preview.Handle
						);
				if (std::any_of(preview.TreeProducers.begin(), preview.TreeProducers.end(), retired))
					RetireImage(preview);
				if (preview.Handle != 0 && !CurrentEndpoint(Universe, Borrow(preview.CapturedProducer))) {
					if (EyeDiagnostic && ViewSlot && *ViewSlot == 5 && EyeDiagnosticCount++ < 256)
						ENGINE_WARN(
							"portal source retire slot=5 reason=capture-producer handle={}", preview.Handle
						);
					RetireImage(preview);
				}
				if (std::any_of(
						preview.UploadTreeProducers.begin(), preview.UploadTreeProducers.end(), retired
					))
					CancelPending(preview);
				const auto producerWorld = Universe.Find(core::Name(preview.Producer.World));
				if (Universe.LookupPresentation(producerWorld, preview.Producer.Channel) !=
						preview.Producer ||
					PublishedEndpoint(EndpointBindings, Borrow(preview.Producer)) !=
						Borrow(preview.ExpectedProducer)) {
					// A retained picture cannot authorize entry into a retired producer incarnation.
					if (EyeDiagnostic && ViewSlot && *ViewSlot == 5 && EyeDiagnosticCount++ < 256)
						ENGINE_WARN(
							"portal source drop slot=5 reason=current-producer handle={}", preview.Handle
						);
					Inbox.InvalidateEndpoint(Borrow(preview.Producer));
					Drop(preview);
					return true;
				}
				if (preview.Pending != 0 && now >= preview.PendingDeadline) {
					CancelUpload(preview);
					ENGINE_TRACE(
						"portal image request expired: {} {} request {}",
						Replies.Channel,
						preview.Producer.World,
						preview.Pending
					);
					if (Resident != nullptr) {
						Resident->Cancel(Replies, preview.Pending);
					}
					Inbox.CancelRequest(preview.Pending);
					preview.Pending = 0;
					preview.Deferred.reset();
				}
				if (preview.Handle != 0 && now >= preview.ImageDeadline && !preview.CapturePinned) {
					ENGINE_TRACE(
						"portal image expired: {} {} handle {}",
						Replies.Channel,
						preview.Producer.World,
						preview.Handle
					);
					ReleaseImageOwner(preview);
					preview.Tree = 0;
					preview.TreeProducers.clear();
					preview.Handle = 0;
					preview.TransparentImages = {};
					preview.SpatialOverlayImage = 0;
					preview.CapturedLenses = {};
					Render.ReleasePortalLensPrograms(preview.LensPrograms);
					preview.LensPrograms = 0;
					preview.ImageVersion.reset();
					preview.CapturePinDeadline.reset();
					if (preview.OfferedVersion) {
						// A renewal can no longer satisfy this request without its owned image.
						if (Resident != nullptr) {
							Resident->Cancel(Replies, preview.Pending);
						}
						Inbox.CancelRequest(preview.Pending);
						preview.Pending = 0;
						preview.OfferedVersion.reset();
						preview.Deferred.reset();
					}
				}
				if (preview.PayloadReady && now >= preview.ImageDeadline) {
					preview.PayloadReady = false;
					preview.Deferred.reset();
				}
				return preview.Pending == 0 && preview.Handle == 0 && !preview.PayloadReady &&
					   !preview.Deferred;
			});
		}
	};
}

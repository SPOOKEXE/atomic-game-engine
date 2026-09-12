#include "PortalCaptureEntrance.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/EditableImages.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/Overlay.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/render/PortalImageDemand.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/PortalShadowTransport.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>

namespace engine::render {
	namespace {
		PortalEndpointView
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
		bool CurrentEndpoint(world::Universe &universe, PortalEndpointView endpoint) {
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
		bool CurrentPublishedEndpoint(
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
		constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds(1);
		constexpr auto SHADOW_TRANSFER_TIMEOUT = std::chrono::seconds(10);
		constexpr size_t MAX_CAPTURES = 4;
		static_assert(MAX_PORTAL_TRANSPARENT_LAYERS + 2 == MAX_CAPTURES);
		constexpr size_t MAX_WAITING_CAPTURES = 16;
		constexpr std::string_view NESTED_REPLY_CHANNEL = "portal-image-replies/nested/";
		PortalEndpointView Borrow(const world::PresentationAddress &address) {
			return {address.World, address.Channel, address.Session, address.Generation};
		}
		bool
		Local(world::Universe &universe, world::WorldId world, const world::PresentationAddress &address) {
			return world.IsValid() && !universe.IsRemote(world) &&
				   universe.NameOf(world).Text() == address.World && address.Session != 0 &&
				   address.Generation != 0;
		}
		std::optional<size_t> ReplySlot(std::string_view channel) {
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
		bool Clock(
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
		core::Name CapturePipeline(PortalImageScope scope, bool ordered = false) {
			if (ordered) return core::Name("portal-image-layers");
			return core::Name(
				scope == PortalImageScope::CompleteWorld ? "portal-image-world" : "portal-image-opaque"
			);
		}
		core::Name CaptureNode() {
			return core::Name("portal-image-export");
		}
		core::Name ShadowCaptureNode() {
			return core::Name("portal-shadow-export");
		}
		core::Name LayerCaptureNode(size_t layer) {
			return core::Name("portal-layer-" + std::to_string(layer) + "-export");
		}
		size_t CaptureProfile(const PortalImageRequest &request) {
			return request.OrderedLayers ? 2 : static_cast<size_t>(request.Scope);
		}
		bool InstallCapture(Renderer &renderer, PortalImageScope scope, bool ordered) {
			if (renderer.Backend().Device == nullptr) {
				return false;
			}
			const auto base = scope == PortalImageScope::CompleteWorld ? graph::DefaultWorldHdrDocument()
																	   : graph::DefaultPbrDocument();
			graph::PipelineDocument document;
			const bool opaque = scope == PortalImageScope::OpaqueLighting;
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

		bool CameraOf(const PortalCaptureCamera &request, PortalImageProjection kind, View &view) {
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

	struct PortalImageSource::Impl {
		world::Universe &Universe;
		Renderer &Render;
		world::WorldId World;
		world::PresentationAddress Replies;
		PortalImageInbox Inbox;
		PortalInboxLimits Limits;
		PortalResidentImages *Resident = nullptr;
		PortalImageSourceDelivery Delivery = PortalImageSourceDelivery::ImportedImages;
		const world::PresentationBindings *EndpointBindings = nullptr;
		std::optional<size_t> ViewSlot = ReplySlot(Replies.Channel);
		std::optional<world::PresentationMessage> ShadowReply;
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
			std::string CapturedEyePlayer{};
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
			void CaptureAccepted() {
				if (CaptureLease == 0) {
					CapturePinDeadline.reset();
					CapturePinned = false;
				}
				CapturedBinding = Binding;
				CapturedCamera = PendingCamera;
				CapturedEyePlayer = EyePlayer;
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
			std::string Portal;
			std::vector<PortalCaptureTreeEndpoint> Producers;
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
				 std::string(preview.Binding.Portal.Text()),
				 std::move(preview.TreeProducers)}
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
				if (now < capture.Deadline &&
					!std::any_of(capture.Producers.begin(), capture.Producers.end(), retired))
					return false;
				if (std::any_of(capture.Producers.begin(), capture.Producers.end(), retired))
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
			if (preview.UploadImages[0] != 0) Render.DropPortalImage(preview.UploadImages[0]);
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
				Render.DropPortalImage(preview.Handle);
			}
			preview.Deferred.reset();
		}
		void ReleaseImageOwner(Preview &preview) {
			PreserveCaptureLease(preview);
			if (preview.Tree != 0)
				Render.ReleasePortalCaptureTree(preview.Tree);
			else if (preview.Handle != 0)
				Render.DropPortalImage(preview.Handle);
			preview.Tree = 0;
			preview.Handle = 0;
		}
		void RetireImage(Preview &preview) {
			Render.DropPortalCaptureTree(preview.Tree);
			ReleaseCaptureLease(preview);
			preview.CapturePinDeadline.reset();
			Render.DropPortalImage(preview.Handle);
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
					RetireImage(preview);
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
				  universe,
				  renderer,
				  world,
				  std::move(replies),
				  PortalImageInbox(limits),
				  limits,
				  resident,
				  delivery
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
			(state.Resident != nullptr && !state.Resident->Owns(state.Render)) ||
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
		result.Transport = state.Universe.SendPresentation(
			state.World, state.Replies, producer, result.RequestId, issued.Wire
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
			if (existing->Producer == producer && existing->Binding.World == preview.Binding.World &&
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
				preview.CapturePinDeadline = existing->CapturePinDeadline;
				preview.CapturePinned = existing->CapturePinned;
				preview.CaptureLease = existing->CaptureLease;
				existing->CaptureLease = 0;
				preview.ImageVersion = existing->ImageVersion;
				preview.CapturedBinding = std::move(existing->CapturedBinding);
				preview.CapturedCamera = existing->CapturedCamera;
				preview.CapturedEyePlayer = std::move(existing->CapturedEyePlayer);
				preview.CapturedRetainedBodyPlayer = std::move(existing->CapturedRetainedBodyPlayer);
			} else {
				state.Drop(*existing);
			}
			*existing = std::move(preview);
		}
		return result;
	}
	std::vector<PortalRuntimeCompletion> PortalImageSource::Poll(Time now) {
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
				preview.CaptureAccepted();
				preview.ImageDeadline = now + state.Limits.Timeout;
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
			preview.CaptureAccepted();
			preview.ImageDeadline = now + state.Limits.Timeout;
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
			if (IsPortalShadowPacket(message.Payload)) {
				PortalShadowPacket packet;
				std::string error;
				if (!state.ShadowReply && message.Payload.size() <= MAX_PORTAL_EXCHANGE_BYTES &&
					message.Payload.capacity() <= MAX_PORTAL_EXCHANGE_BYTES && message.To == state.Replies &&
					CurrentEndpoint(state.Universe, Borrow(message.From)) &&
					DecodePortalShadowPacket(message.Payload, packet, error))
					state.ShadowReply = std::move(message);
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
					preview.CaptureAccepted();
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
					for (auto &preview : state.Previews) {
						if (preview.OrderedLayers || preview.Pending != message.Correlation ||
							preview.Producer != message.From || message.To != state.Replies ||
							preview.Binding.Expected != receipt.Key ||
							preview.Binding.ExpectedScope != receipt.Scope) {
							continue;
						}
						const auto handle = state.Resident->Take(state.Replies, message.From, receipt, now);
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
						preview.CaptureAccepted();
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
			auto preview =
				std::find_if(state.Previews.begin(), state.Previews.end(), [&](const Impl::Preview &entry) {
					return entry.Pending == message.Correlation;
				});
			if (preview == state.Previews.end()) {
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
				preview->CaptureAccepted();
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
			preview->CaptureAccepted();
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
	std::optional<world::PresentationMessage> PortalImageSource::TakeShadowReply() {
		if (!State->ShadowReply) return {};
		if (State->Universe.LookupPresentation(State->World, State->Replies.Channel) != State->Replies ||
			!CurrentEndpoint(State->Universe, Borrow(State->ShadowReply->From))) {
			State->ShadowReply.reset();
			return {};
		}
		auto reply = std::move(State->ShadowReply);
		State->ShadowReply.reset();
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
				return preview.Handle;
			}
		}
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
	std::optional<PortalImageCapture> PortalImageSource::Capture(std::string_view portal) const {
		for (const auto &preview : State->Previews) {
			if (preview.Binding.Portal.Text() == portal && preview.Handle != 0 && preview.ImageVersion)
				return PortalImageCapture{
					preview.Handle,
					preview.Producer,
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
					preview.CapturedRetainedBodyPlayer
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
		if (State->ShadowReply &&
			(State->ShadowReply->From == endpoint || State->ShadowReply->To == endpoint))
			State->ShadowReply.reset();
		State->Inbox.InvalidateEndpoint(Borrow(endpoint));
		std::erase_if(State->PinnedCaptures, [&](Impl::PinnedCapture &capture) {
			const auto dependent = std::any_of(
				capture.Producers.begin(),
				capture.Producers.end(),
				[&](const PortalCaptureTreeEndpoint &producer) {
					return PortalEndpointView{
							   producer.World, producer.Channel, producer.Session, producer.Generation
						   } == Borrow(endpoint);
				}
			);
			if (!dependent) return false;
			State->Render.DropPortalCaptureTree(capture.Tree);
			State->Render.ReleasePortalCaptureTreeLease(capture.Lease);
			return true;
		});
		std::erase_if(State->Previews, [&](Impl::Preview &preview) {
			const auto dependent = [&](const auto &producers) {
				return std::any_of(producers.begin(), producers.end(), [&](const auto &producer) {
					return PortalEndpointView{
							   producer.World, producer.Channel, producer.Session, producer.Generation
						   } == Borrow(endpoint);
				});
			};
			if (State->Replies == endpoint || preview.Producer == endpoint) {
				State->Drop(preview);
				return true;
			}
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
			if (capture.Portal != portal) return false;
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

	struct PortalImageProducer::Impl {
		world::Universe &Universe;
		Renderer &Render;
		world::WorldId World;
		world::PresentationAddress Requests;
		core::Name OwnerName;
		PortalResidentImages *Resident = nullptr;
		const world::PresentationBindings *EndpointBindings = nullptr;
		core::Name ContentOwner = OwnerName;
		std::unique_ptr<ShaderLibrary> OwnedShaders;
		ShaderLibrary *Shaders = nullptr;
		bool PostProcessing = true;
		std::vector<core::Name> GuiShaders;
		uint64_t GuiShaderSignature = 0;
		std::vector<WorldContentOwner> ForeignContentOwners;
		EditableImageUploader EditableImages;
		EditableMeshUploader EditableMeshes;
		std::optional<Time> LastTime;
		std::array<bool, 3> PipelineReady{};
		double PresentationSeconds = 0;
		WorldViewFrame WorldFrame;
		RetainedBodyAuthorization AuthorizeRetainedBody;
		struct PresentationFrame {
			std::vector<scene::DrawInstance> EntranceRows;
			std::optional<int16_t> Entrance;
			WorldCameraFrame Camera;
			std::vector<SurfaceView> Surfaces;
			InterfacePass Interface;
			bool InterfaceReady = false;
		};
		std::array<PresentationFrame, MAX_WAITING_CAPTURES> Frames;
		struct Capture {
			world::PresentationAddress ReplyTo;
			PortalImageReply Reply;
			std::vector<std::byte> Wire;
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
		bool RoutesCurrent(const ProducedShadowRoutes &routes) const {
			if (Universe.LookupPresentation(World, Requests.Channel) != Requests ||
				PublishedEndpoint(EndpointBindings, Borrow(Requests)) !=
					BorrowRouteEndpoint(routes.Producer) ||
				!Authorized(routes.Requester, routes.RetainedBodyPlayer))
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
		bool TransferCurrent(const ShadowTransfer &transfer) {
			if (Universe.LookupPresentation(World, Requests.Channel) != Requests ||
				PublishedEndpoint(EndpointBindings, Borrow(Requests)) !=
					BorrowRouteEndpoint(transfer.Producer) ||
				Universe.LookupPresentation(
					Universe.Find(core::Name(transfer.Requester.World)), transfer.Requester.Channel
				) != transfer.Requester ||
				!Authorized(transfer.Requester, transfer.RetainedBodyPlayer))
				return false;
			if (transfer.Pull.Part == PORTAL_SHADOW_CANCEL_PART || transfer.RetainedBodyPlayer.empty())
				return true;
			const auto *routes = FindRoutes(transfer.Requester, transfer.Pull.ParentEye);
			return routes && RoutesCurrent(*routes);
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
				if (EncodePortalShadowPacket({pull, PortalImageStatus::BudgetExceeded, {}}, busy, error))
					(void)Universe.SendPresentation(World, Requests, message.From, message.Correlation, busy);
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
							(void)Render.Render(std::span(&view, 1), overlay, nullptr, false);
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
					const auto status = Universe.SendPresentation(
						World, hop.Requester, hop.Producer, transfer.ChildCorrelation, transfer.Wire
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
			const auto status = Universe.SendPresentation(
				World,
				child ? transfer.Hop.Requester : Requests,
				child ? transfer.Hop.Producer : transfer.Requester,
				child ? transfer.ChildCorrelation : transfer.Correlation,
				transfer.Wire
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
			const auto status = Universe.SendPresentation(
				World, Requests, capture.ReplyTo, capture.Reply.Key.RequestId, capture.Wire
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
	PortalImageProducer::PortalImageProducer(
		world::Universe &universe,
		Renderer &renderer,
		world::WorldId world,
		world::PresentationAddress requests,
		PortalResidentImages *resident,
		ShaderLibrary *shaders,
		bool postProcessing
	)
		: State(
			  std::make_unique<Impl>(
				  universe, renderer, world, std::move(requests), universe.NameOf(world), resident
			  )
		  ) {
		State->Shaders = shaders;
		State->PostProcessing = postProcessing;
	}
	PortalImageProducer::~PortalImageProducer() {
		Clear();
	}
	void PortalImageProducer::SetRetainedBodyAuthorization(RetainedBodyAuthorization authorize) {
		Clear();
		State->AuthorizeRetainedBody = std::move(authorize);
	}
	std::optional<PortalShadowImage> PortalImageProducer::TakeShadow(
		const world::PresentationAddress &requester, const PortalExchangeKey &eye, Time now
	) {
		auto &state = *State;
		if (!Clock(state.LastTime, now)) return {};
		state.ExpireShadows(now);
		for (auto &shadow : state.Shadows) {
			if (!shadow || !shadow->Ready || shadow->Requester != requester ||
				shadow->Image.Snapshot.Eye != eye)
				continue;
			core::Metrics::Count("render.portal_shadow.taken_bytes", shadow->Image.Depth.size());
			auto image = std::move(shadow->Image);
			state.ReleaseProducedShadow(shadow);
			return image;
		}
		return {};
	}
	PortalProducerShadowUsage PortalImageProducer::ShadowUsage() const {
		PortalProducerShadowUsage usage;
		for (const auto &shadow : State->Shadows) {
			if (!shadow) continue;
			++usage.Images;
			usage.Ready += shadow->Ready;
			usage.ReservedBytes += PORTAL_SHADOW_BYTES;
			usage.CpuBytes += shadow->Image.Depth.capacity() + Impl::ShadowSnapshotBytes(*shadow);
			if (shadow->FitImage) usage.CpuBytes += shadow->FitImage->Depth.capacity();
		}
		const auto addressBytes = [](const auto &address) {
			return address.World.capacity() + address.Channel.capacity();
		};
		for (const auto &routes : State->ShadowRoutes) {
			if (!routes) continue;
			++usage.Routes;
			usage.ReadyRoutes += routes->Ready;
			usage.RouteMetadataBytes += sizeof(Impl::ProducedShadowRoutes) + addressBytes(routes->Requester) +
										addressBytes(routes->Producer) +
										routes->ParentEye.PortalKey.capacity() +
										routes->RetainedBodyPlayer.capacity();
			for (const auto &target : std::span(routes->Targets).first(routes->Count))
				usage.RouteMetadataBytes +=
					addressBytes(target.Producer) + target.Eye.PortalKey.capacity() +
					addressBytes(target.Hop.Requester) + addressBytes(target.Hop.Producer) +
					target.Hop.ParentEye.PortalKey.capacity() + addressBytes(target.HopProducer);
		}
		for (const auto &transfer : State->Transfers)
			usage.Transfers += transfer.has_value();
		usage.TransferPacketBytes = State->TransferBytes();
		return usage;
	}
	std::optional<PortalShadowRoute> PortalImageProducer::ResolveShadowRoute(
		const world::PresentationAddress &parentRequester,
		const PortalExchangeKey &parentEye,
		const PortalCaptureTreeEndpoint &targetProducer,
		const PortalExchangeKey &targetEye,
		Time now
	) {
		auto &state = *State;
		if (!Clock(state.LastTime, now)) return {};
		state.ExpireShadows(now);
		for (const auto &routes : state.ShadowRoutes) {
			if (!routes || !routes->Ready || routes->Requester != parentRequester ||
				routes->ParentEye != parentEye)
				continue;
			for (const auto &target : std::span(routes->Targets).first(routes->Count))
				if (target.Producer == targetProducer && target.Eye == targetEye) return target.Hop;
		}
		return {};
	}
	void PortalImageProducer::SetEndpointBindings(const world::PresentationBindings *bindings) {
		if (State->EndpointBindings == bindings) return;
		Clear();
		State->EndpointBindings = bindings;
	}
	void PortalImageProducer::SetContentOwner(core::Name owner, std::span<const WorldContentOwner> foreign) {
		if (State->ContentOwner != owner) {
			State->EditableImages.ForgetOwner(State->ContentOwner);
			State->EditableMeshes.ForgetOwner(State->ContentOwner);
		}
		State->ContentOwner = owner;
		State->ForeignContentOwners.assign(foreign.begin(), foreign.end());
	}

	bool PortalImageProducer::HasPendingShadowFits() const {
		return std::any_of(State->Shadows.begin(), State->Shadows.end(), [](const auto &shadow) {
			return shadow && shadow->FitToken != 0;
		});
	}

	void PortalImageProducer::Clear() {
		for (auto &transfer : State->Transfers)
			transfer.reset();
		if (State->Resident != nullptr) {
			State->Resident->Invalidate(State->Requests);
		}
		for (auto &capture : State->Captures) {
			State->Cancel(capture);
		}
		State->Captures.clear();
		State->FailureReplies.clear();
		for (auto &shadow : State->Shadows)
			State->ReleaseProducedShadow(shadow);
		for (auto &routes : State->ShadowRoutes)
			routes.reset();
		for (auto &pending : State->Pending) {
			pending.Source.reset();
			if (pending.Work) State->Cancel(pending.Work->Output);
			pending.Work.reset();
			if (pending.Replies.Generation != 0) (void)State->Universe.ClosePresentation(pending.Replies);
			pending.Replies = {};
		}
		State->Render.ForgetWorld(State->World.Index, State->OwnerName);
		State->EditableImages.ForgetOwner(State->ContentOwner);
		State->EditableMeshes.ForgetOwner(State->ContentOwner);
		State->PipelineReady = {};
		for (auto &frame : State->Frames) {
			frame.Interface.Shutdown();
			frame.InterfaceReady = false;
			frame.Camera.Compiled.Invalidate();
		}
	}
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
			(state.Resident != nullptr && !state.Resident->Owns(state.Render)) ||
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
			auto &pending = state.Pending[slot];
			if (!pending.Work) {
				if (pending.Source) {
					(void)pending.Source->Poll(now);
					state.CollectShadowReply(*pending.Source);
				}
				continue;
			}
			auto &job = *pending.Work;
			if (now >= job.Output.Deadline ||
				state.Universe.LookupPresentation(
					state.Universe.Find(core::Name(job.Output.ReplyTo.World)), job.Output.ReplyTo.Channel
				) != job.Output.ReplyTo) {
				job.Output.Reply.Status = PortalImageStatus::Unavailable;
				job.Output.Reply.Diagnostic = "nested destination capture expired or requester retired";
				state.SendFailure(job.Output, progress);
				state.Finish(slot);
				continue;
			}
			job.Waiting = false;
			if (pending.Source) {
				for (const auto &completion : pending.Source->Poll(now)) {
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
				++progress.Refused;
				continue;
			}
			Impl::Capture capture;
			capture.ReplyTo = std::move(message.From);
			capture.Reply.Key = request.Key;
			capture.Reply.Scope = request.Scope;
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
				(request.Scope == PortalImageScope::OpaqueLighting && !request.OrderedLayers &&
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
			freeSlot->Work.emplace();
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
				frame.InterfaceReady || state.Render.Backend().Device == nullptr) {
				continue;
			}
			frame.InterfaceReady = frame.Interface.Initialise(
				state.Render.Backend().Device, state.Render.Backend().ColourFormat
			);
			frame.Interface.SetImageSource([&state](const core::Name &name) {
				InterfaceImage image;
				image.Texture = state.Render.TextureHandle(name, state.ContentOwner);
				image.Cell = state.Render.TextureCell(name, state.PresentationSeconds, state.ContentOwner);
				state.Render.TextureSize(name, image.Width, image.Height, state.ContentOwner);
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
			state.EditableImages.Refresh(store, state.Render, state.ContentOwner);
			state.EditableMeshes.Refresh(store, state.Render, state.ContentOwner);
			PrepareWorldShaders(
				store, state.ContentOwner, *state.Shaders, state.Render, nullptr, state.PostProcessing
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
			state.PresentationSeconds = state.WorldFrame.Seconds;
			const auto &seams = state.WorldFrame.Seams;

			for (size_t index = 0; index < jobs.size(); ++index) {
				auto &frame = state.Frames[index];
				auto &view = jobs[index]->Viewpoint;
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
								return candidate.Demand.Portal.Index == seam.Surface;
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
					if (matched != job.Children.size())
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
							state.Render,
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
				const size_t shares =
					job.Children.size() +
					(!job.Request.OrderedLayers && !state.Frames[index].Surfaces.empty() ? 1 : 0);
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
				const uint32_t childBudget = uint32_t(remaining / shares);
				job.LocalPixels = remaining - uint64_t(childBudget) * job.Children.size();
				for (auto &child : job.Children) {
					if (!job.Failure.empty()) break;
					auto &demand = child.Demand;
					const auto destination = state.Universe.Find(demand.DestinationWorld);
					const auto producer =
						state.Universe.LookupPresentation(destination, PORTAL_REQUEST_CHANNEL);
					if (child.RequestId == 0) {
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
						demand.Request.PixelBudget = childBudget;
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
			view.Lighting = lighting;
			view.OverrideLighting = true;

			FrameOverlayHook *interface = nullptr;
			if (job.Request.Scope == PortalImageScope::CompleteWorld) {
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
				state.Render.TextureAnimationSignature(state.PresentationSeconds)
			);
			const bool shaderClock =
				lighting.ShaderLensCount != 0 ||
				state.Render.PostProcessShaderName(state.ContentOwner).IsValid() ||
				std::any_of(view.Instances.begin(), view.Instances.end(), [](const auto &row) {
					return row.Shader.IsValid();
				});
			if (shaderClock) {
				signatureState.Animation = scene::MixSignature(
					signatureState.Animation, std::bit_cast<uint64_t>(state.PresentationSeconds)
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
			assets::Hasher lightingHash;
			lightingHash.Update(std::as_bytes(std::span(&lighting, 1)));
			lightingHash.Update(std::as_bytes(std::span(state.Frames[index].Camera.Lights)));
			const auto digest = lightingHash.Finish();
			for (size_t byte = 0; byte < 8; ++byte) {
				job.Output.Reply.LightingRevision |= uint64_t(digest.Digest[byte]) << (byte * 8);
			}
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
			if (job.Request.OrderedLayers && lighting.ShaderLensCount != 0) {
				static_assert(MAX_PORTAL_CAPTURE_LENSES == scene::MAX_SCENE_SHADER_LENSES);
				auto &capturedLenses = job.Output.Lenses;
				capturedLenses.TimeSeconds = static_cast<float>(state.PresentationSeconds);
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
			OverlayImage overlay;
			state.Render.SetAnimationTime(state.PresentationSeconds);
			const auto rendered = state.Render.Render(std::span(&view, 1), overlay, interface, false);
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

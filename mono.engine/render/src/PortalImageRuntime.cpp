#include "EnvironmentModes.hpp"
#include "PortalCaptureEntrance.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/Overlay.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/render/PortalImageDemand.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>

namespace engine::render {
	namespace {
		constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds(1);
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
			}
			if (ordered) {
				const auto edge = [&](graph::EditKind kind, const std::string &target, const char *port) {
					document.Record({.Kind = kind, .Target = core::Name(target), .Key = core::Name(port)});
				};
				for (size_t layer = 0; layer <= MAX_PORTAL_TRANSPARENT_LAYERS; ++layer) {
					const auto name = "portal-layer-" + std::to_string(layer);
					for (const auto &[suffix, format] : std::array{
							 std::pair{"", graph::ResourceFormat::RGBA16F},
							 std::pair{"-depth", graph::ResourceFormat::R32F},
							 std::pair{"-z", graph::ResourceFormat::D32F}
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
				}
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
		std::optional<size_t> ViewSlot = ReplySlot(Replies.Channel);
		std::optional<Time> LastTime;
		struct Preview {
			world::PresentationAddress Producer;
			PortalImageBinding Binding;
			uint64_t Pending = 0;
			uint64_t Handle = 0;
			Time PendingDeadline;
			Time ImageDeadline;
			std::optional<PortalResidentReceipt> ImageVersion;
			std::optional<PortalImageVersion> OfferedVersion;
			std::string EyePlayer;
			PortalCaptureCamera PendingCamera{};
			PortalImageBinding CapturedBinding{};
			PortalCaptureCamera CapturedCamera{};
			std::string CapturedEyePlayer{};
			bool OrderedLayers = false;
			std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS> TransparentImages{};
			std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS + 1> UploadImages{};
			std::optional<PortalResidentReceipt> UploadVersion;
			void CaptureAccepted() {
				CapturedBinding = Binding;
				CapturedCamera = PendingCamera;
				CapturedEyePlayer = EyePlayer;
			}
		};
		std::vector<Preview> Previews;
		void CancelUpload(Preview &preview) {
			if (preview.UploadImages[0] != 0) Render.DropPortalImage(preview.UploadImages[0]);
			preview.UploadImages = {};
			preview.UploadVersion.reset();
		}
		void Drop(Preview &preview) {
			CancelUpload(preview);
			if (Resident != nullptr) {
				Resident->Cancel(Replies, preview.Pending);
			}
			Inbox.CancelRequest(preview.Pending);
			if (preview.Handle != 0) {
				Render.DropPortalImage(preview.Handle);
			}
		}
		void Expire(Time now) {
			if (Resident != nullptr) {
				Resident->Expire(now);
			}
			std::erase_if(Previews, [&](Preview &preview) {
				const auto producerWorld = Universe.Find(core::Name(preview.Producer.World));
				if (Universe.LookupPresentation(producerWorld, preview.Producer.Channel) !=
					preview.Producer) {
					// A retained picture cannot authorize entry into a retired producer incarnation.
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
				}
				if (preview.Handle != 0 && now >= preview.ImageDeadline) {
					ENGINE_TRACE(
						"portal image expired: {} {} handle {}",
						Replies.Channel,
						preview.Producer.World,
						preview.Handle
					);
					Render.DropPortalImage(preview.Handle);
					preview.Handle = 0;
					preview.TransparentImages = {};
					preview.ImageVersion.reset();
					if (preview.OfferedVersion) {
						// A renewal can no longer satisfy this request without its owned image.
						if (Resident != nullptr) {
							Resident->Cancel(Replies, preview.Pending);
						}
						Inbox.CancelRequest(preview.Pending);
						preview.Pending = 0;
						preview.OfferedVersion.reset();
					}
				}
				return preview.Pending == 0 && preview.Handle == 0;
			});
		}
	};
	PortalImageSource::PortalImageSource(
		world::Universe &universe,
		Renderer &renderer,
		world::WorldId world,
		world::PresentationAddress replies,
		PortalInboxLimits limits,
		PortalResidentImages *resident
	)
		: State(
			  std::make_unique<Impl>(
				  universe, renderer, world, std::move(replies), PortalImageInbox(limits), limits, resident
			  )
		  ) {}
	PortalImageSource::~PortalImageSource() {
		Clear();
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
			binding.ViewSlot != *state.ViewSlot ||
			(state.Resident != nullptr && !state.Resident->Owns(state.Render)) ||
			!Local(state.Universe, state.World, state.Replies) ||
			producer.Channel != PORTAL_REQUEST_CHANNEL || binding.World != state.World.Index ||
			binding.WorldName.Text() != state.Replies.World ||
			binding.Portal.Text() != request.Key.PortalKey ||
			!Clock(state.LastTime, now, state.Limits.Timeout)) {
			return result;
		}
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
		auto existing =
			std::find_if(state.Previews.begin(), state.Previews.end(), [&](const Impl::Preview &preview) {
				return preview.Binding.Portal == binding.Portal;
			});
		// Keep camera motion from cancelling every reply before it arrives. The
		// caller retries its latest demand after Poll completes this request.
		if (existing != state.Previews.end() && existing->Pending != 0 && existing->Producer == producer &&
			existing->OrderedLayers == request.OrderedLayers && existing->EyePlayer == request.EyePlayer &&
			existing->Binding.Index == binding.Index &&
			existing->Binding.Expected.SeamRevision == request.Key.SeamRevision &&
			existing->Binding.ExpectedScope == request.Scope &&
			existing->Binding.ExpectedProjection == request.Projection) {
			result.Status = PortalInboxStatus::Busy;
			return result;
		}
		if (!request.Geometry.empty()) {
			const auto hash = assets::Hasher::Of(request.Geometry);
			for (const auto byte : hash.Digest) {
				request.Key.CameraRevision = scene::MixSignature(request.Key.CameraRevision, byte);
			}
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
		Impl::Preview preview{
			producer,
			std::move(binding),
			result.RequestId,
			0,
			now + state.Limits.Timeout,
			{},
			{},
			issued.Request.KnownImage,
			issued.Request.EyePlayer
		};
		preview.OrderedLayers = issued.Request.OrderedLayers;
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
				preview.TransparentImages = existing->TransparentImages;
				preview.ImageDeadline = existing->ImageDeadline;
				preview.ImageVersion = existing->ImageVersion;
				preview.CapturedBinding = std::move(existing->CapturedBinding);
				preview.CapturedCamera = existing->CapturedCamera;
				preview.CapturedEyePlayer = std::move(existing->CapturedEyePlayer);
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
			if (!preview.UploadVersion || !state.Render.PortalImageLayerSetReady(preview.UploadImages[0]))
				continue;
			if (preview.Handle != 0) state.Render.DropPortalImage(preview.Handle);
			preview.Handle = preview.UploadImages[0];
			std::copy(
				preview.UploadImages.begin() + 1,
				preview.UploadImages.end(),
				preview.TransparentImages.begin()
			);
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
						if (preview.TransparentImages[0] != 0) state.Render.DropPortalImage(preview.Handle);
						preview.TransparentImages = {};
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
				Borrow(message.From), Borrow(message.To), message.Correlation, message.Payload, now
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
					 std::move(accepted.Failure->Diagnostic)}
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
			if (preview->OrderedLayers) {
				auto layers = state.Inbox.TakeLayers(
					Borrow(state.Replies), Borrow(message.From), preview->Binding.Expected.PortalKey, now
				);
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
				if (!state.Render.QueuePortalImageLayerSet(
						preview->Binding, std::move(*layers), preview->UploadImages
					)) {
					preview->Pending = 0;
					completions.push_back(
						{message.Correlation,
						 PortalImageStatus::BudgetExceeded,
						 0,
						 "source layer import refused"}
					);
					continue;
				}
				preview->UploadVersion = version;
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
					{message.Correlation, PortalImageStatus::BudgetExceeded, 0, "source image import refused"}
				);
				continue;
			}
			if (preview->TransparentImages[0] != 0) state.Render.DropPortalImage(preview->Handle);
			preview->TransparentImages = {};
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
		return completions;
	}
	bool PortalImageSource::HasPendingUploads() const {
		return std::any_of(State->Previews.begin(), State->Previews.end(), [](const Impl::Preview &preview) {
			return preview.UploadImages[0] != 0;
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
					preview.TransparentImages
				};
		}
		return {};
	}
	void PortalImageSource::InvalidateEndpoint(const world::PresentationAddress &endpoint) {
		State->Inbox.InvalidateEndpoint(Borrow(endpoint));
		std::erase_if(State->Previews, [&](Impl::Preview &preview) {
			if (State->Replies != endpoint && preview.Producer != endpoint) {
				return false;
			}
			State->Drop(preview);
			return true;
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
		}
	}

	void PortalImageSource::InvalidatePortal(std::string_view portal) {
		State->Inbox.InvalidatePortal(Borrow(State->Replies), portal);
		std::erase_if(State->Previews, [&](Impl::Preview &preview) {
			if (preview.Binding.Portal.Text() != portal) {
				return false;
			}
			State->Drop(preview);
			return true;
		});
	}
	void PortalImageSource::Clear() {
		for (auto &preview : State->Previews) {
			State->Drop(preview);
		}
		State->Previews.clear();
		State->Inbox.Clear();
	}

	struct PortalImageProducer::Impl {
		world::Universe &Universe;
		Renderer &Render;
		world::WorldId World;
		world::PresentationAddress Requests;
		core::Name OwnerName;
		PortalResidentImages *Resident = nullptr;
		std::optional<Time> LastTime;
		std::array<bool, 3> PipelineReady{};
		double PresentationSeconds = 0;
		std::vector<scene::DrawInstance> Instances;
		std::vector<core::CFrame> Joints;
		std::vector<PortalView> Portals;
		std::vector<scene::SurfaceSlot> Slots;
		ParticleFrame Particles;
		struct PresentationFrame {
			std::vector<scene::DrawInstance> EntranceRows;
			std::optional<int16_t> Entrance;
			effects::RibbonBuffer Ribbons;
			std::vector<SceneLight> Lights;
			std::vector<SurfaceView> Surfaces;
			gui::Compiled Compiled;
			gui::DrawList SpatialCommands;
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
			Time Deadline;
		};
		std::vector<Capture> Captures;
		std::vector<Capture> FailureReplies;
		struct Child {
			PortalImageDemand Demand;
			world::PresentationAddress Producer;
			uint64_t RequestId = 0;
			PortalImageVersion Version;
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
		};
		std::array<PendingSlot, MAX_WAITING_CAPTURES> Pending;
		void Finish(size_t slot, bool keepChildren = false) {
			auto &pending = Pending[slot];
			if (pending.Source && !keepChildren) pending.Source->Clear();
			pending.Work.reset();
		}

		void Cancel(Capture &capture) {
			Render.CancelResourceImage(capture.Token);
			capture.Token = 0;
			for (auto &token : capture.LayerTokens) {
				Render.CancelResourceImage(token);
				token = 0;
			}
		}
		bool CollectLayers(Capture &capture, Time now) {
			const std::array tokens{
				capture.Token, capture.LayerTokens[0], capture.LayerTokens[1], capture.LayerTokens[2]
			};
			auto images = Render.TakeResourceImages(tokens);
			if (!images && now < capture.Deadline) return false;
			Cancel(capture);
			capture.Reply.Status = PortalImageStatus::Failed;
			capture.Reply.Diagnostic =
				images ? "destination layer capture failed" : "destination layer capture expired";
			if (!images) return true;
			const auto &opaque = images->front();
			for (const auto &image : *images) {
				if (image.Status != ResourceImageStatus::Ok || image.CaptureFrame == 0 ||
					image.CaptureFrame != opaque.CaptureFrame || image.Width != opaque.Width ||
					image.Height != opaque.Height ||
					image.Depth.size() != size_t(image.Width) * image.Height * 4)
					return true;
			}
			const auto &overflow = images->back();
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
			}
			std::string error;
			if (!EncodePortalImageLayerSet(layers, capture.Wire, error)) {
				capture.Reply.Status = PortalImageStatus::Failed;
				capture.Reply.Diagnostic = "destination layer encoding failed: " + error;
			}
			return true;
		}

		world::PresentationStatus Send(Capture &capture, PortalProducerProgress &progress) {
			if (capture.Wire.empty()) {
				if (capture.Reply.Status != PortalImageStatus::Ok) capture.Reply.CaptureLighting.reset();
				std::string error;
				if (!EncodePortalImageReply(capture.Reply, capture.Wire, error)) {
					++progress.Refused;
					return world::PresentationStatus::Invalid;
				}
				// Keep one canonical owned image while transport is full. Retrying
				// does not hash, encode, copy GPU pixels or redraw the world again.
				std::vector<std::byte>().swap(capture.Reply.Pixels);
				std::vector<std::byte>().swap(capture.Reply.Depth);
			}
			const auto status = Universe.SendPresentation(
				World, Requests, capture.ReplyTo, capture.Reply.Key.RequestId, capture.Wire
			);
			if (status == world::PresentationStatus::Ok) {
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
		PortalResidentImages *resident
	)
		: State(
			  std::make_unique<Impl>(
				  universe, renderer, world, std::move(requests), universe.NameOf(world), resident
			  )
		  ) {}
	PortalImageProducer::~PortalImageProducer() {
		Clear();
	}
	void PortalImageProducer::Clear() {
		if (State->Resident != nullptr) {
			State->Resident->Invalidate(State->Requests);
		}
		for (auto &capture : State->Captures) {
			State->Cancel(capture);
		}
		State->Captures.clear();
		State->FailureReplies.clear();
		for (auto &pending : State->Pending) {
			pending.Source.reset();
			pending.Work.reset();
			if (pending.Replies.Generation != 0) (void)State->Universe.ClosePresentation(pending.Replies);
			pending.Replies = {};
		}
		State->Render.ForgetWorld(State->World.Index, State->OwnerName);
		State->PipelineReady = {};
		for (auto &frame : State->Frames) {
			frame.Interface.Shutdown();
			frame.InterfaceReady = false;
			frame.Compiled.Invalidate();
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
		std::erase_if(state.FailureReplies, [&](Impl::Capture &capture) {
			if (now >= capture.Deadline) {
				++progress.Refused;
				return true;
			}
			return state.Send(capture, progress) != world::PresentationStatus::Full;
		});
		std::erase_if(state.Captures, [&](Impl::Capture &capture) {
			if (capture.Token == 0 && now >= capture.Deadline) {
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
			return status != world::PresentationStatus::Full || now >= capture.Deadline;
		});
		using Job = Impl::Job;
		std::vector<Job *> jobs;
		for (size_t slot = 0; slot < state.Pending.size(); ++slot) {
			auto &pending = state.Pending[slot];
			if (!pending.Work) {
				if (pending.Source) (void)pending.Source->Poll(now);
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
						if (completion.Status != PortalImageStatus::Ok) {
							job.Failure = "nested destination capture failed: " + completion.Diagnostic;
							job.FailureStatus = completion.Status;
						}
					}
				}
			}
			jobs.push_back(&job);
		}
		for (auto &message : state.Universe.TakePresentation(state.Requests)) {
			++progress.Requests;
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
			capture.Deadline = now + CAPTURE_TIMEOUT;
			ENGINE_TRACE(
				"portal image producer received: {} {} request {}",
				capture.ReplyTo.Channel,
				state.Requests.World,
				request.Key.RequestId
			);
			View view;
			if (request.RecursionDepth > MAX_PORTAL_DEPTH ||
				(request.Scope == PortalImageScope::OpaqueLighting && request.RecursionDepth != 0) ||
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
			jobs.push_back(&job);
		}
		if (jobs.empty()) {
			return progress;
		}
		for (auto *entry : jobs) {
			auto &job = *entry;
			const auto profile = CaptureProfile(job.Request);
			if (!state.PipelineReady[profile]) {
				state.PipelineReady[profile] =
					InstallCapture(state.Render, job.Request.Scope, job.Request.OrderedLayers);
			}
		}
		std::erase_if(jobs, [&](Job *entry) {
			auto &job = *entry;
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
		for (size_t index = 0; index < jobs.size(); ++index) {
			auto &frame = state.Frames[index];
			if (jobs[index]->Request.Scope != PortalImageScope::CompleteWorld || frame.InterfaceReady ||
				state.Render.Backend().Device == nullptr) {
				continue;
			}
			frame.InterfaceReady = frame.Interface.Initialise(
				state.Render.Backend().Device, state.Render.Backend().ColourFormat
			);
			frame.Interface.SetImageSource([&state](const core::Name &name) {
				InterfaceImage image;
				image.Texture = state.Render.TextureHandle(name);
				image.Cell = state.Render.TextureCell(name, state.PresentationSeconds);
				state.Render.TextureSize(name, image.Width, image.Height);
				return image;
			});
		}
		const world::Presentation presentation{state.World, frameSeconds, alpha};
		const bool presented =
			destinationPresented || state.Universe.PresentMany(std::span(&presentation, 1)) == 1;
		auto &instances = state.Instances;
		auto &joints = state.Joints;
		instances.clear();
		joints.clear();
		scene::WorldLighting lighting;
		uint64_t tick = 0, identity = 0;
		RegisterPresentationComponents();
		effects::RegisterEffectComponents();
		gui::RegisterGuiComponents();
		const bool copied = presented && state.Universe.Enter(state.World, [&](ecs::Store &store) {
			if (store.Resource<DrawList>() == nullptr) {
				store.SetResource(DrawList{});
			}
			// Replica presentation already sampled its received snapshots. Rebuilding
			// from ECS transforms here would replace that pose with the latest packet.
			if (!destinationPresented && !store.AdoptOnly()) {
				scene::SyncRendered(store);
				CollectInstances(store);
			}
			const auto &draw = *store.Resource<DrawList>();
			instances = draw.Instances;
			joints = draw.JointFrames;
			lighting = scene::LightingOf(store);
			// An inactive cloud clock cannot change the captured pixels.
			if (EnvironmentModesOf(lighting.EnvironmentState).Clouds == 0 ||
				lighting.EnvironmentState.CloudLayer.WindSpeed <= 0) {
				lighting.EnvironmentState.CloudTime = 0;
			}
			tick = store.Time().Tick;
			identity = store.Identity();
			state.PresentationSeconds = store.Time().Elapsed;
			scene::GatherSurfaceSlots(store, state.Slots);
			ApplySurfaceSlots(instances, state.Slots, state.OwnerName);
			std::vector<scene::PortalSeam> seams;
			scene::GatherPortalSeams(store, seams);
			for (auto &seam : seams) {
				for (const auto &slot : state.Slots) {
					if (slot.Camera == seam.Camera) seam.Surface = slot.Index;
				}
			}
			CollectPortalViews(store, state.Portals, state.Slots);
			CollectParticleBatches(store, state.Particles);
			state.Particles.Detach();

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
					job.Portals = state.Portals;
					job.LocalPixels = job.Request.PixelBudget - uint64_t(job.Request.Width) *
																	job.Request.Height *
																	(job.Request.OrderedLayers ? 4 : 1);
					if (job.Request.Scope == PortalImageScope::CompleteWorld &&
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
							store, view, settings, demands, job.Portals, state.Slots
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
							job.Children.push_back({std::move(demand), {}, 0});
						}
					}
				}
				if (revalidateChildren && job.Request.Scope == PortalImageScope::CompleteWorld &&
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

				CollectSurfaceViews(store, frame.Surfaces, job.Portals, &view, state.Slots);
				std::erase_if(frame.Surfaces, [&](const auto &surface) {
					return (frame.Entrance && surface.Index == *frame.Entrance) ||
						   std::any_of(job.Portals.begin(), job.Portals.end(), [&](const auto &portal) {
							   return portal.ExternalImage && portal.Index == surface.Index;
						   });
				});
				const auto &pending = state.Pending[job.Slot];
				const bool waitingForChildren =
					std::any_of(job.Children.begin(), job.Children.end(), [&](const auto &child) {
						return child.RequestId == 0 || !pending.Source ||
							   pending.Source->CurrentImage(child.Demand.Binding.Portal.Text()) == 0;
					});
				// Waiting jobs still validate seams and budget surfaces, but their
				// camera-dependent layers are sampled only when children can be drawn.
				if (waitingForChildren || !job.Failure.empty()) continue;
				job.ViewPrepared = true;
				core::Metrics::Count("render.portal_snapshot.prepared_views", 1);
				CollectLights(store, view.CameraFrame.Position, frame.Lights);
				effects::BuildRibbons(
					store, view.CameraFrame.Position, float(store.Time().Elapsed), frame.Ribbons
				);
				const core::Vector2 extent{
					float(jobs[index]->Request.Width), float(jobs[index]->Request.Height)
				};
				const gui::Screen screen{extent.X, extent.Y};
				ResolveSpatialCanvases(store, screen, &view.Camera, &view.CameraFrame);
				gui::CompileRequest compile;
				compile.Display = screen;
				compile.Seconds = store.Time().Elapsed;
				frame.Compiled.Rebuild(store, compile);
				frame.SpatialCommands = frame.Compiled.Commands();
				std::erase_if(frame.SpatialCommands.Commands, [&](const gui::DrawCommand &command) {
					return store.Get<gui::SpatialCanvas>(command.Collector) == nullptr;
				});
				frame.Interface.Submit(
					frame.SpatialCommands, extent, extent, store, frame.Compiled.Signature()
				);
			}
		}) == world::WorldStatus::Ok;
		size_t copiedBytes =
			instances.size() * sizeof(scene::DrawInstance) + joints.size() * sizeof(core::CFrame);
		for (size_t index = 0; index < jobs.size(); ++index) {
			const auto &frame = state.Frames[index];
			copiedBytes += frame.EntranceRows.size() * sizeof(scene::DrawInstance) +
						   frame.Surfaces.size() * sizeof(SurfaceView);
			if (jobs[index]->ViewPrepared)
				copiedBytes += frame.Lights.size() * sizeof(SceneLight) +
							   frame.Ribbons.Vertices.size() * sizeof(effects::RibbonVertex) +
							   frame.Ribbons.Runs.size() * sizeof(effects::RibbonRun);
		}
		copiedBytes += state.Portals.size() * sizeof(PortalView);
		copiedBytes += state.Particles.Blocks.size() * sizeof(effects::EmitterBlock) +
					   state.Particles.SpawnStates.size() * sizeof(effects::EmitterSpawnState) +
					   state.Particles.RuntimeStates.size() * sizeof(effects::EmitterRuntime);
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
							state.Resident
						);
				}
				if (!pending.Source) job.Failure = "nested reply endpoint unavailable";
				const size_t shares = job.Children.size() + (!state.Frames[index].Surfaces.empty() ? 1 : 0);
				const uint64_t remaining =
					job.Request.PixelBudget - uint64_t(job.Request.Width) * job.Request.Height;
				const uint32_t childBudget = uint32_t(remaining / shares);
				job.LocalPixels = remaining - uint64_t(childBudget) * job.Children.size();
				for (auto &child : job.Children) {
					if (!job.Failure.empty()) break;
					auto &demand = child.Demand;
					const auto destination = state.Universe.Find(demand.DestinationWorld);
					const auto producer =
						state.Universe.LookupPresentation(destination, PORTAL_REQUEST_CHANNEL);
					if (child.RequestId == 0) {
						if (uint64_t(demand.Request.Width) * demand.Request.Height > childBudget) {
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
						}
						job.Waiting = true;
						continue;
					}
					if (producer != child.Producer) {
						job.Failure = "nested producer endpoint retired";
						break;
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
			view.Pipeline = CapturePipeline(job.Request.Scope, job.Request.OrderedLayers);
			view.Instances = instances;
			if (state.Frames[index].Entrance) view.Instances = state.Frames[index].EntranceRows;
			view.JointFrames = joints;
			std::vector<scene::DrawInstance> joinedInstances;
			std::vector<core::CFrame> joinedJoints;
			PortalDrawSelection bodySelection{job.Request.EyePlayer, {}};
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
			view.Lights = state.Frames[index].Lights;
			view.Lighting = lighting;
			view.OverrideLighting = true;

			FrameOverlayHook *interface = nullptr;
			if (job.Request.Scope == PortalImageScope::CompleteWorld) {
				view.Surfaces = state.Frames[index].Surfaces;
				view.Portals = job.Portals;
				view.Particles = state.Particles.Batches;
				view.ParticleSeams = state.Particles.Seams;
				view.ParticleRevision = state.Particles.Revision;
				view.ParticleLayoutRevision = state.Particles.LayoutRevision;
				view.ParticleResidentRevision = state.Particles.ResidentRevision;
				view.ParticleBlocks = state.Particles.BlockCount;
				view.ParticlePool = state.Particles.Pool;
				view.ParticleDelta = index == 0 ? frameSeconds : 0;
				view.RibbonVertices = state.Frames[index].Ribbons.Vertices;
				view.RibbonRuns = state.Frames[index].Ribbons.Runs;
				view.SurfaceBudget = View::SurfaceCaptureBudget{job.Request.RecursionDepth, job.LocalPixels};
				auto &frame = state.Frames[index];
				if (!frame.SpatialCommands.Commands.empty()) {
					if (!frame.InterfaceReady) {
						job.Output.Reply.Status = PortalImageStatus::Unavailable;
						job.Output.Reply.Diagnostic = "destination spatial interface unavailable";
						state.SendFailure(job.Output, progress);
						continue;
					}
					interface = &frame.Interface;
				}
			}

			ScenePresentationState signatureState;
			signatureState.Lighting = lighting;
			signatureState.Resources = scene::MixSignature(identity, state.Render.ResourceRevision());
			signatureState.Animation = scene::MixSignature(
				state.Frames[index].Compiled.Signature(),
				state.Render.TextureAnimationSignature(state.PresentationSeconds)
			);
			const bool shaderClock =
				lighting.ShaderLensCount != 0 || state.Render.PostProcessShaderName().IsValid() ||
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
			lightingHash.Update(std::as_bytes(std::span(state.Frames[index].Lights)));
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
			if (!ResolvePortalCaptureLighting(capturedLighting, view, state.Frames[index].Lights)) {
				job.Output.Reply.Status = PortalImageStatus::Failed;
				job.Output.Reply.Diagnostic = "invalid destination capture lighting";
				state.SendFailure(job.Output, progress);
				continue;
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
				const std::array nodes{
					CaptureNode(), LayerCaptureNode(0), LayerCaptureNode(1), LayerCaptureNode(2)
				};
				std::array<uint64_t, 4> tokens{};
				if (state.Render.QueueResourceImages(
						view.Pipeline, nodes, view.Slot, ResourceImageDelivery::CopiedPixels, tokens
					)) {
					job.Output.Token = tokens[0];
					std::copy(tokens.begin() + 1, tokens.end(), job.Output.LayerTokens.begin());
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

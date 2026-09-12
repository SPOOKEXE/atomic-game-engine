#include "RendererState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/render/PortalImageRuntime.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {
	namespace {
		core::Vector3 Vector(const std::array<float, 3> &value) {
			return {value[0], value[1], value[2]};
		}

		scene::SeamTransform Mapping(const PortalCaptureTreeEdge &edge) {
			const auto &q = edge.Orientation;
			return {
				.Frame = core::CFrame(Vector(edge.Position), glm::quat(q[3], q[0], q[1], q[2])),
				.Origin = {},
				.Scale = edge.Scale
			};
		}

		PortalCaptureCamera CaptureCameraOf(const PortalCaptureTreeCamera &camera) {
			return {camera.Position, camera.Orientation, camera.Frustum, camera.ClipPlane};
		}

		bool FiniteVector(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		bool ApertureNormal(const PortalCaptureTreeEdge &edge, core::Vector3 &normal) {
			// Codec-valid float axes can overflow a float cross product before normalization.
			const auto &first = edge.First;
			const auto &second = edge.Second;
			const double x = double(first[1]) * second[2] - double(first[2]) * second[1];
			const double y = double(first[2]) * second[0] - double(first[0]) * second[2];
			const double z = double(first[0]) * second[1] - double(first[1]) * second[0];
			const double length = std::hypot(x, y, z);
			if (!std::isfinite(length) || length <= 0) return false;
			normal = {
				static_cast<float>(x / length), static_cast<float>(y / length), static_cast<float>(z / length)
			};
			return FiniteVector(normal);
		}

		bool Ordinary(const scene::DrawInstance &row, size_t joints) {
			return row.Transparency == 0 && row.Alpha != scene::AlphaMode::Transparency && row.Surface < 0 &&
				   !row.Shader.IsValid() && FiniteVector(row.Frame.Position) &&
				   FiniteVector(row.HalfExtent) && FiniteVector(row.SeamNormal) &&
				   std::isfinite(row.SeamOffset) && row.SkinFirst <= joints &&
				   row.SkinCount <= joints - row.SkinFirst;
		}

		// A child's oblique camera owns its entrance cut. The body's existing cut
		// remains available for an outer straddle and is mapped without replacement.
		bool EntranceCamera(
			const PortalCaptureTreeEdge &edge,
			const PortalCaptureTreeCamera &parent,
			const PortalCaptureTreeCamera &child,
			core::Vector3 normal
		) {
			if (child.Projection != PortalImageProjection::Seam) return false;
			const auto through = Mapping(edge);
			const float side = normal.Dot(Vector(parent.Position) - Vector(edge.Centre));
			const float mappedSide = through.Length(std::abs(side));
			if (!std::isfinite(side) || !std::isfinite(mappedSide)) return false;
			normal = through.Rotate(normal) * (side >= 0 ? -1.0f : 1.0f);
			const auto point =
				through.Point(Vector(edge.Centre)) - normal * scene::PortalClipBias(mappedSide);
			const auto &clip = child.ClipPlane;
			const auto clipNormal = core::Vector3(clip[0], clip[1], clip[2]);
			const float distance = normal.Dot(point);
			if (!FiniteVector(normal) || !FiniteVector(point) || !std::isfinite(distance)) return false;
			// Float wire poses and chained uniform scales accumulate a few ulps.
			const float tolerance = 1e-4f * std::max(1.0f, std::abs(distance));
			if ((normal - clipNormal).Magnitude() > 1e-4f || std::abs(distance + clip[3]) > tolerance)
				return false;
			const auto mappedEye = through.Point(Vector(parent.Position));
			if (!FiniteVector(mappedEye)) return false;
			const double eyeMagnitude =
				std::hypot(double(mappedEye.X), double(mappedEye.Y), double(mappedEye.Z));
			const double eyeError = std::hypot(
				double(mappedEye.X) - child.Position[0],
				double(mappedEye.Y) - child.Position[1],
				double(mappedEye.Z) - child.Position[2]
			);
			return std::isfinite(eyeError) && eyeError <= 1e-4 * std::max(1.0, eyeMagnitude);
		}
	}

	namespace {
		bool PrepareTree(
			const ImportedPortalCaptureTree *tree,
			const View &rootBody,
			std::array<PortalTreeNodeWork, MAX_PORTAL_CAPTURE_TREE_NODES> &work
		) {
			if (!tree || !rootBody.Target || rootBody.Instances.size() > MAX_PORTAL_GEOMETRY_ROWS ||
				rootBody.JointFrames.size() > MAX_PORTAL_GEOMETRY_JOINTS ||
				rootBody.World != tree->Nodes.front().Binding.World ||
				rootBody.WorldName != tree->Nodes.front().Binding.WorldName ||
				rootBody.Slot != tree->Nodes.front().Binding.ViewSlot ||
				std::any_of(rootBody.Instances.begin(), rootBody.Instances.end(), [&](const auto &row) {
					return !Ordinary(row, rootBody.JointFrames.size());
				}))
				return false;

			if (rootBody.EyeHiddenRows.size() > rootBody.Instances.size()) return false;
			for (size_t index = 0; index < rootBody.EyeHiddenRows.size(); ++index)
				if (rootBody.EyeHiddenRows[index] >= rootBody.Instances.size() ||
					(index && rootBody.EyeHiddenRows[index - 1] >= rootBody.EyeHiddenRows[index]))
					return false;
			work[0].Body.assign(rootBody.Instances.begin(), rootBody.Instances.end());
			work[0].Output = tree->Nodes.front().Binding;
			std::string error;
			// Parent indices precede children. Prepare all copied geometry and mappings
			// before submitting work, so malformed edges cannot publish partial results.
			for (size_t parent = 0; parent < tree->Nodes.size(); ++parent) {
				for (const auto &edge : tree->Edges) {
					if (edge.Parent != parent) continue;
					const auto &parentNode = tree->Nodes[parent];
					const auto &childNode = tree->Nodes[edge.Child];
					core::Vector3 normal;
					if (!ApertureNormal(edge, normal) ||
						!EntranceCamera(edge, parentNode.Camera, childNode.Camera, normal))
						return false;
					auto &near = work[parent];
					auto &far = work[edge.Child];
					const auto through = Mapping(edge);
					far.Body = near.Body;
					for (auto &row : far.Body) {
						row.Frame = through.Place(row.Frame);
						row.HalfExtent = row.HalfExtent * through.Scale;
						row.SeamNormal = through.Rotate(row.SeamNormal);
						row.SeamOffset =
							row.SeamOffset * through.Scale + row.SeamNormal.Dot(Vector(edge.Position));
						row.SeamLight = through.Rotate(row.SeamLight);
						row.TagMask = 0;
						if (!Ordinary(row, rootBody.JointFrames.size())) return false;
					}
					if (near.Apertures.size() >= scene::MAX_SURFACES) return false;
					const auto surface = static_cast<int16_t>(near.Apertures.size());
					const size_t first = near.ApertureRows.size();
					if (!AppendPortalDraws(
							edge.Geometry,
							core::Name(parentNode.Producer.World),
							near.ApertureRows,
							near.ApertureJoints,
							error
						) ||
						near.ApertureRows.size() == first ||
						near.ApertureRows.size() > MAX_PORTAL_GEOMETRY_ROWS - near.Body.size() ||
						near.ApertureJoints.size() > MAX_PORTAL_GEOMETRY_JOINTS - rootBody.JointFrames.size())
						return false;
					for (size_t row = first; row < near.ApertureRows.size(); ++row) {
						if (!Ordinary(near.ApertureRows[row], near.ApertureJoints.size())) return false;
						near.ApertureRows[row].Surface = surface;
					}
					near.Apertures.push_back(
						{.Index = surface,
						 .ExternalImage = true,
						 .ImagePortal = core::Name(edge.PortalKey),
						 .Centre = Vector(edge.Centre),
						 .Normal = normal,
						 .First = Vector(edge.First),
						 .Second = Vector(edge.Second),
						 .Warp = through}
					);
					far.Output = parentNode.Binding;
					far.Output.Index = surface;
					far.Output.Portal = core::Name(edge.PortalKey);
					far.Output.Expected = childNode.Binding.Expected;
					far.Output.Expected.PortalKey = edge.PortalKey;
					far.Output.ExpectedProjection = childNode.Binding.ExpectedProjection;
					View camera;
					if (!ResolvePortalCaptureCamera(
							CaptureCameraOf(childNode.Camera), childNode.Camera.Projection, camera
						))
						return false;
					auto matrix = through.Frame.ToMatrix();
					for (size_t axis = 0; axis < 3; ++axis)
						matrix[axis] *= through.Scale;
					far.Output.Sampling =
						*camera.Projection * camera.CameraFrame.Inverse().ToMatrix() * matrix;
					// Validate the derived binding before any child render allocates targets.
					for (int column = 0; column < 4; ++column)
						for (int row = 0; row < 4; ++row)
							if (!std::isfinite(far.Output.Sampling[column][row])) return false;
					const float determinant = glm::determinant(far.Output.Sampling);
					if (!std::isfinite(determinant) || determinant == 0) return false;
				}
			}
			return true;
		}
		PortalImageCapture CaptureOf(const ImportedPortalCaptureTreeNode &node) {
			PortalImageCapture capture;
			capture.Image = node.Images[0];
			capture.Producer = {
				node.Producer.World, node.Producer.Channel, node.Producer.Session, node.Producer.Generation
			};
			capture.Binding = node.Binding;
			capture.Camera = CaptureCameraOf(node.Camera);
			capture.RetainedBodyPlayer = node.RetainedBodyPlayer;
			capture.Width = node.Width;
			capture.Height = node.Height;
			capture.CaptureLighting = node.Lighting;
			capture.TransparentImages = {node.Images[1], node.Images[2]};
			capture.SpatialOverlayImage = node.Images[3];
			capture.Lenses = node.Lenses;
			capture.LensPrograms = node.LensPrograms;
			return capture;
		}
		void ConnectChildren(
			const ImportedPortalCaptureTree &tree,
			std::array<PortalTreeNodeWork, MAX_PORTAL_CAPTURE_TREE_NODES> &work,
			size_t index
		) {
			for (const auto &edge : tree.Edges)
				if (edge.Parent == index)
					work[index].Apertures[work[edge.Child].Output.Index].ImportedImage =
						work[edge.Child].Image;
		}
		size_t WorkBytes(const PortalTreeCompositionJob &job) {
			size_t bytes = job.Joints.capacity() * sizeof(core::CFrame) +
						   job.EyeHiddenRows.capacity() * sizeof(uint32_t) +
						   job.ContentOwners.capacity() * sizeof(WorldContentOwner);
			for (const auto &node : job.Work) {
				bytes += node.Body.capacity() * sizeof(scene::DrawInstance) +
						 node.ApertureRows.capacity() * sizeof(scene::DrawInstance) +
						 node.ApertureJoints.capacity() * sizeof(core::CFrame) +
						 node.Apertures.capacity() * sizeof(PortalView) +
						 node.Output.Expected.PortalKey.capacity();
			}
			return bytes;
		}
		std::optional<core::AABB> BodyBounds(const PortalTreeNodeWork &work) {
			std::optional<core::AABB> bounds;
			if (!work.Body.empty()) bounds = graph::BoundsOfAll(work.Body);
			if (!work.ApertureRows.empty()) {
				const auto aperture = graph::BoundsOfAll(work.ApertureRows);
				bounds = bounds ? bounds->Union(aperture) : aperture;
			}
			return bounds;
		}
		std::array<float, 6> BoundsArray(const core::AABB &bounds) {
			return {
				bounds.Minimum.X,
				bounds.Minimum.Y,
				bounds.Minimum.Z,
				bounds.Maximum.X,
				bounds.Maximum.Y,
				bounds.Maximum.Z
			};
		}
		core::AABB Box(const std::array<float, 6> &bounds) {
			return {{bounds[0], bounds[1], bounds[2]}, {bounds[3], bounds[4], bounds[5]}};
		}
	}

	namespace {
		template <class Imports>
		bool ShadowRequestOf(
			const ImportedPortalCaptureTree &tree,
			const Imports &images,
			uint64_t token,
			size_t index,
			PortalTreeShadowRequest &request
		) {
			if (index >= tree.Nodes.size()) return false;
			const auto &node = tree.Nodes[index];
			const auto found = std::find_if(images.begin(), images.end(), [&](const auto &image) {
				return image.Handle == node.Images[0];
			});
			if (found == images.end() || !found->Ready || !found->CaptureTick ||
				!found->DirectionalResponseTexture || found->PixelHash.IsZero())
				return false;
			request.Job = token;
			request.Node = static_cast<uint32_t>(index);
			request.Producer = node.Producer;
			request.Eye = node.Binding.Expected;
			request.CaptureTick = *found->CaptureTick;
			request.ContentRevision = found->ContentRevision;
			request.LightingRevision = found->LightingRevision;
			request.EyePixelHash = found->PixelHash;
			request.ExcludedPlayer = node.RetainedBodyPlayer;
			request.LightDirection = Vector(node.Lighting.Direction);
			return true;
		}
		bool CompletedPreparedFence(SDL_GPUDevice *device, SDL_GPUFence *&fence) {
			if (!fence) {
				auto *command = SDL_AcquireGPUCommandBuffer(device);
				if (command) fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
			}
			if (!fence || !SDL_QueryGPUFence(device, fence)) return false;
			SDL_ReleaseGPUFence(device, fence);
			fence = nullptr;
			return true;
		}
	}

	bool MatchesPortalTreeShadowRequest(
		const PortalTreeShadowRequest &expected, const PortalShadowSnapshot &snapshot
	) {
		if (!expected.Job || expected.Node >= MAX_PORTAL_CAPTURE_TREE_NODES ||
			!ValidPortalShadowSnapshot(snapshot) || snapshot.Producer != expected.Producer ||
			snapshot.Eye != expected.Eye || snapshot.CaptureTick != expected.CaptureTick ||
			snapshot.ContentRevision != expected.ContentRevision ||
			snapshot.LightingRevision != expected.LightingRevision ||
			snapshot.EyePixelHash != expected.EyePixelHash ||
			snapshot.ExcludedPlayer != expected.ExcludedPlayer || !FiniteVector(expected.LightDirection))
			return false;
		if (expected.BodyBounds) {
			const auto &bounds = *expected.BodyBounds;
			if (!FiniteVector(bounds.Minimum) || !FiniteVector(bounds.Maximum) ||
				bounds.Minimum.X > bounds.Maximum.X || bounds.Minimum.Y > bounds.Maximum.Y ||
				bounds.Minimum.Z > bounds.Maximum.Z)
				return false;
		}
		const auto sourceBounds = Box(snapshot.SourceBounds);
		const auto domain =
			snapshot.SourceEmpty
				? expected.BodyBounds.value_or(graph::BoundsOfAll(std::span<const scene::DrawInstance>{}))
			: expected.BodyBounds ? sourceBounds.Union(*expected.BodyBounds)
								  : sourceBounds;
		if (BoundsArray(domain) != snapshot.DomainBounds) return false;
		const auto magnitude = expected.LightDirection.Magnitude();
		if (!std::isfinite(magnitude) || magnitude <= 0) return false;
		const auto light = graph::FitDirectionalLight(domain, expected.LightDirection / magnitude);
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row)
				if (light[column][row] != snapshot.LightViewProjection[column * 4 + row]) return false;
		return true;
	}

	uint64_t Renderer::ComposePortalCaptureTree(uint64_t token, const View &rootBody) {
		RequireOwningThread("ComposePortalCaptureTree");
		ENGINE_PROFILE("compose current portal capture tree");
		const auto *tree = FindPortalCaptureTree(token);
		std::array<PortalTreeNodeWork, MAX_PORTAL_CAPTURE_TREE_NODES> work;
		if (!PrepareTree(tree, rootBody, work)) return 0;
		const auto rollback = [&] {
			for (auto &node : work)
				if (node.Image != 0) DropPortalImage(node.Image);
		};
		for (size_t index = tree->Nodes.size(); index-- > 0;) {
			const auto &node = tree->Nodes[index];
			auto &prepared = work[index];
			ConnectChildren(*tree, work, index);
			auto capture = CaptureOf(node);
			const SceneTarget target{node.Width, node.Height};
			auto view = rootBody;
			view.Target = &target;
			view.Instances = prepared.Body;
			if (index != 0) {
				view.EyeRig = 0;
				view.EyePlayer.reset();
				view.EyeHiddenRows = {};
			}
			view.Surfaces = {};
			view.Portals = {};
			prepared.Image = ComposePortalBodyImageInternal(
				capture,
				view,
				prepared.Output,
				prepared.ApertureRows,
				prepared.ApertureJoints,
				prepared.Apertures,
				true
			);
			if (prepared.Image == 0) {
				rollback();
				return 0;
			}
			for (const auto &edge : tree->Edges) {
				if (edge.Parent != index) continue;
				DropPortalImage(work[edge.Child].Image);
				work[edge.Child].Image = 0;
			}
		}
		return work[0].Image;
	}
	PortalTreeCompositionStatus
	Renderer::BeginPortalCaptureTreeComposition(uint64_t token, const View &body, uint64_t &jobToken) {
		RequireOwningThread("BeginPortalCaptureTreeComposition");
		ENGINE_PROFILE("prepare retained tree composition");
		jobToken = 0;
		auto &job = State->PortalTreeJob;
		if (job.Token && job.Cancelled) PollPortalCaptureTreeComposition(job.Token);
		if (job.Token) return PortalTreeCompositionStatus::BudgetExceeded;
		const auto *tree = FindPortalCaptureTree(token);
		if (!State->Device || !tree || !State->NextPortalTreeJob || !body.Surfaces.empty() ||
			!body.Portals.empty() || !body.Foreign.empty() || !body.ForeignJointFrames.empty() ||
			!body.Particles.empty() || !body.ParticleSeams.empty() || !body.RibbonVertices.empty() ||
			!body.RibbonRuns.empty() || body.ImportedDirectionalShadow || body.DirectionalShadowBounds ||
			body.ForeignContentOwners.size() > MAX_PORTAL_CAPTURE_TREE_NODES + 1)
			return PortalTreeCompositionStatus::Invalid;
		PortalTreeCompositionJob prepared;
		if (!PrepareTree(tree, body, prepared.Work)) return PortalTreeCompositionStatus::Invalid;
		prepared.Joints.assign(body.JointFrames.begin(), body.JointFrames.end());
		prepared.EyeHiddenRows.assign(body.EyeHiddenRows.begin(), body.EyeHiddenRows.end());
		prepared.EyeRig = body.EyeRig;
		prepared.EyePlayer = body.EyePlayer;
		prepared.ContentOwners.assign(body.ForeignContentOwners.begin(), body.ForeignContentOwners.end());
		prepared.ContentOwner = body.ContentOwner;
		prepared.OwnedBytes = WorkBytes(prepared);
		if (prepared.OwnedBytes > MAX_IMPORTED_PORTAL_TREE_METADATA_BYTES)
			return PortalTreeCompositionStatus::BudgetExceeded;
		prepared.Tree = token;
		prepared.ResourceEpoch = State->ResourceEpoch;
		prepared.NextNode = tree->Nodes.size();
		prepared.Token = State->NextPortalTreeJob++;
		jobToken = prepared.Token;
		job = std::move(prepared);
		return PortalTreeCompositionStatus::Pending;
	}

	PortalTreeCompositionProgress Renderer::PollPortalCaptureTreeComposition(uint64_t token) {
		RequireOwningThread("PollPortalCaptureTreeComposition");
		auto &job = State->PortalTreeJob;
		if (!token || job.Token != token) return {};
		if (job.ResourceEpoch != State->ResourceEpoch) job.Cancelled = true;
		if (job.Cancelled) State->ReleasePortalTreeShadowAssembly();
		if (job.Submitted) {
			if (!job.Fence) {
				auto *command = SDL_AcquireGPUCommandBuffer(State->Device);
				if (command) job.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
			}
			if (!job.Fence || !SDL_QueryGPUFence(State->Device, job.Fence))
				return {.Status = PortalTreeCompositionStatus::Pending};
			SDL_ReleaseGPUFence(State->Device, job.Fence);
			job.Fence = nullptr;
			job.Submitted = false;
			DropPortalShadowImage(job.Shadow);
			job.Shadow = 0;
		}
		if (job.Cancelled || job.AfterFence != PortalTreeCompositionStatus::Pending) {
			const auto status = job.AfterFence == PortalTreeCompositionStatus::Pending
									? PortalTreeCompositionStatus::Invalid
									: job.AfterFence;
			for (auto &node : job.Work)
				if (node.Image) DropPortalImage(node.Image);
			State->ReleasePortalTreeShadowAssembly();
			job = {};
			State->ReapPortalPreparedTrees(*this);
			return {.Status = status};
		}
		const auto *tree = FindPortalCaptureTree(job.Tree);
		if (!tree) {
			CancelPortalCaptureTreeComposition(token);
			return {};
		}
		// Children remain owned until the parent's submitted reads have finished.
		for (const auto &edge : tree->Edges) {
			if (edge.Parent != job.NextNode) continue;
			auto &child = job.Work[edge.Child];
			if (child.Image) DropPortalImage(child.Image);
			child.Image = 0;
		}
		if (job.NextNode == 0) {
			const uint64_t result = job.Work[0].Image;
			job.Work[0].Image = 0;
			if (job.Prepared)
				for (auto &node : job.Work)
					if (node.Image) DropPortalImage(node.Image);
			job = {};
			State->ReapPortalPreparedTrees(*this);
			return {.Status = PortalTreeCompositionStatus::Complete, .Image = result};
		}
		const auto &node = tree->Nodes[job.NextNode - 1];
		const auto found = std::find_if(
			State->ImportedPortals.begin(), State->ImportedPortals.end(), [&](const auto &image) {
				return image.Handle == node.Images[0];
			}
		);
		if (found == State->ImportedPortals.end() || !found->Ready || !found->CaptureTick ||
			!found->DirectionalResponseTexture || found->PixelHash.IsZero()) {
			CancelPortalCaptureTreeComposition(token);
			return {};
		}
		const size_t outputBytes = size_t(node.Width) * node.Height * 12;
		if (PORTAL_SHADOW_BYTES + outputBytes >
				MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes ||
			(job.ShadowAssemblyBytes ? 0 : PORTAL_SHADOW_BYTES) >
				MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes)
			return {.Status = PortalTreeCompositionStatus::BudgetExceeded};
		PortalTreeShadowRequest request;
		request.Job = token;
		request.Node = static_cast<uint32_t>(job.NextNode - 1);
		request.Producer = node.Producer;
		request.Eye = node.Binding.Expected;
		request.CaptureTick = *found->CaptureTick;
		request.ContentRevision = found->ContentRevision;
		request.LightingRevision = found->LightingRevision;
		request.EyePixelHash = found->PixelHash;
		request.ExcludedPlayer = node.RetainedBodyPlayer;
		request.BodyBounds = BodyBounds(job.Work[request.Node]);
		request.LightDirection = Vector(node.Lighting.Direction);
		return {.Status = PortalTreeCompositionStatus::Pending, .Request = std::move(request)};
	}

	PortalTreeCompositionStatus
	Renderer::AcceptPortalCaptureTreeShadow(uint64_t token, PortalShadowImage &&image) {
		RequireOwningThread("AcceptPortalCaptureTreeShadow");
		ENGINE_PROFILE("compose retained tree shadow");
		if (!token || State->PortalTreeJob.Token != token) return PortalTreeCompositionStatus::Invalid;
		if (State->PortalTreeJob.Submitted || State->PortalTreeJob.NextNode == 0)
			return PortalTreeCompositionStatus::Pending;
		const auto progress = PollPortalCaptureTreeComposition(token);
		if (progress.Status != PortalTreeCompositionStatus::Pending) return progress.Status;
		if (!progress.Request) return PortalTreeCompositionStatus::Pending;
		auto &job = State->PortalTreeJob;
		const bool assembled = job.AssembledShadow && &image == &*job.AssembledShadow;
		if (job.ShadowAssemblyBytes && !assembled) return PortalTreeCompositionStatus::Invalid;
		if (image.Depth.size() != PORTAL_SHADOW_BYTES) return PortalTreeCompositionStatus::Invalid;
		const size_t ownBytes = assembled ? job.ShadowAssemblyBytes : 0;
		if (image.Depth.capacity() >
			MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes + ownBytes)
			return PortalTreeCompositionStatus::BudgetExceeded;
		const auto &expected = *progress.Request;
		const auto &snapshot = image.Snapshot;
		if (!MatchesPortalTreeShadowRequest(expected, snapshot) || !ValidPortalShadowImage(image))
			return PortalTreeCompositionStatus::Invalid;
		const auto *tree = FindPortalCaptureTree(job.Tree);
		if (!tree) return PortalTreeCompositionStatus::Invalid;
		const auto &node = tree->Nodes[expected.Node];
		PortalShadowImageBinding binding{node.Binding.World, node.Binding.WorldName, snapshot};
		const auto domain = Box(snapshot.DomainBounds);
		// Transfer the existing charge on the owning thread, with no callback or yield between owners.
		State->PortalImportUsage.PendingCpuBytes -= ownBytes;
		job.Shadow = QueuePortalShadowImage(binding, std::move(image));
		if (!job.Shadow) {
			State->PortalImportUsage.PendingCpuBytes += ownBytes;
			return PortalTreeCompositionStatus::BudgetExceeded;
		}
		if (assembled) {
			job.ShadowAssemblyBytes = 0;
			State->ReleasePortalTreeShadowAssembly();
		}
		auto &work = job.Work[expected.Node];
		ConnectChildren(*tree, job.Work, expected.Node);
		auto capture = CaptureOf(node);
		const SceneTarget target{node.Width, node.Height};
		View view;
		view.World = node.Binding.World;
		view.WorldName = node.Binding.WorldName;
		view.Slot = node.Binding.ViewSlot;
		view.ContentOwner = job.ContentOwner;
		view.ForeignContentOwners = job.ContentOwners;
		view.Instances = work.Body;
		// Primary selection only changes colour draws. Full body rows still fit and cast shadows.
		if (expected.Node == 0) {
			view.EyeRig = job.EyeRig;
			view.EyePlayer = job.EyePlayer;
			view.EyeHiddenRows = job.EyeHiddenRows;
		}
		view.JointFrames = job.Joints;
		view.Target = &target;
		view.DirectionalShadowBounds = domain;
		view.ImportedDirectionalShadow = job.Shadow;
		work.Image = ComposePortalBodyImageInternal(
			capture, view, work.Output, work.ApertureRows, work.ApertureJoints, work.Apertures, true
		);
		// Even failed composition can have submitted an upload. A queue fence owns retirement.
		job.Submitted = true;
		job.AfterFence =
			work.Image ? PortalTreeCompositionStatus::Pending : PortalTreeCompositionStatus::Invalid;
		if (work.Image) --job.NextNode;
		auto *command = SDL_AcquireGPUCommandBuffer(State->Device);
		if (command) job.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		return PortalTreeCompositionStatus::Pending;
	}

	void Renderer::Impl::ReleasePortalTreeShadowAssembly() {
		auto &job = PortalTreeJob;
		PortalImportUsage.PendingCpuBytes -= job.ShadowAssemblyBytes;
		job.ShadowAssemblyBytes = 0;
		job.ShadowAssembly.reset();
		job.ShadowManifest.reset();
		job.AssembledShadow.reset();
		job.ShadowAssemblyNode = 0;
		ReportPortalImportUsage();
	}

	PortalTreeCompositionStatus
	Renderer::BeginPortalCaptureTreeShadowAssembly(uint64_t token, const PortalShadowSnapshot &manifest) {
		RequireOwningThread("BeginPortalCaptureTreeShadowAssembly");
		ENGINE_PROFILE("portal tree shadow assembly begin");
		auto &job = State->PortalTreeJob;
		if (!token || job.Token != token) return PortalTreeCompositionStatus::Invalid;
		if (job.Submitted || job.NextNode == 0) return PortalTreeCompositionStatus::Pending;
		const auto progress = PollPortalCaptureTreeComposition(token);
		if (progress.Status != PortalTreeCompositionStatus::Pending) return progress.Status;
		if (!progress.Request) return PortalTreeCompositionStatus::Pending;
		if (!MatchesPortalTreeShadowRequest(*progress.Request, manifest))
			return PortalTreeCompositionStatus::Invalid;
		if (job.ShadowManifest)
			return *job.ShadowManifest == manifest ? PortalTreeCompositionStatus::Pending
												   : PortalTreeCompositionStatus::Invalid;
		const size_t budget = MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes;
		if (budget < PORTAL_SHADOW_BYTES) return PortalTreeCompositionStatus::BudgetExceeded;
		auto assembly = std::make_unique<PortalShadowAssembly>();
		std::string error;
		if (!assembly->Begin(manifest, budget, error)) return PortalTreeCompositionStatus::BudgetExceeded;
		job.ShadowAssemblyBytes = assembly->Bytes();
		State->PortalImportUsage.PendingCpuBytes += job.ShadowAssemblyBytes;
		job.ShadowAssemblyNode = progress.Request->Node;
		job.ShadowManifest = manifest;
		job.ShadowAssembly = std::move(assembly);
		core::Metrics::Count("render.portal_shadow.assembly_bytes", job.ShadowAssemblyBytes);
		core::Metrics::Count("render.portal_shadow.assemblies", 1);
		State->ReportPortalImportUsage();
		return PortalTreeCompositionStatus::Pending;
	}

	PortalTreeCompositionStatus Renderer::AcceptPortalCaptureTreeShadowTile(
		uint64_t token, uint32_t node, std::span<const std::byte> packet
	) {
		RequireOwningThread("AcceptPortalCaptureTreeShadowTile");
		auto &job = State->PortalTreeJob;
		if (!token || job.Token != token || !job.ShadowAssembly || job.ShadowAssemblyNode != node ||
			job.NextNode != size_t(node) + 1)
			return PortalTreeCompositionStatus::Invalid;
		if (job.Submitted) return PortalTreeCompositionStatus::Pending;
		if (job.ResourceEpoch != State->ResourceEpoch || job.Cancelled) {
			CancelPortalCaptureTreeComposition(token);
			return PortalTreeCompositionStatus::Invalid;
		}
		if (job.AssembledShadow) return PortalTreeCompositionStatus::Invalid;
		std::string error;
		if (!job.ShadowAssembly->Accept(packet, error)) {
			// A final full-image hash failure cancels the codec's allocation.
			if (!job.ShadowAssembly->Bytes()) State->ReleasePortalTreeShadowAssembly();
			return PortalTreeCompositionStatus::Invalid;
		}
		job.AssembledShadow = job.ShadowAssembly->Take();
		core::Metrics::Count("render.portal_shadow.assembly_received_bytes", packet.size());
		if (!job.AssembledShadow) return PortalTreeCompositionStatus::Pending;
		return CommitPortalCaptureTreeShadowAssembly(token, node);
	}

	PortalTreeCompositionStatus
	Renderer::CommitPortalCaptureTreeShadowAssembly(uint64_t token, uint32_t node) {
		RequireOwningThread("CommitPortalCaptureTreeShadowAssembly");
		auto &job = State->PortalTreeJob;
		if (!token || job.Token != token) return PortalTreeCompositionStatus::Invalid;
		if (job.Submitted || job.NextNode == 0) return PortalTreeCompositionStatus::Pending;
		if (!job.ShadowManifest || job.ShadowAssemblyNode != node || job.NextNode != size_t(node) + 1)
			return PortalTreeCompositionStatus::Invalid;
		if (job.ResourceEpoch != State->ResourceEpoch || job.Cancelled) {
			CancelPortalCaptureTreeComposition(token);
			return PortalTreeCompositionStatus::Invalid;
		}
		if (!job.AssembledShadow) return PortalTreeCompositionStatus::Pending;
		return AcceptPortalCaptureTreeShadow(token, std::move(*job.AssembledShadow));
	}

	void Renderer::CancelPortalCaptureTreeComposition(uint64_t token) {
		RequireOwningThread("CancelPortalCaptureTreeComposition");
		auto &job = State->PortalTreeJob;
		if (!token || job.Token != token) return;
		job.Cancelled = true;
		State->ReleasePortalTreeShadowAssembly();
		if (job.Submitted) return;
		if (job.Shadow) DropPortalShadowImage(job.Shadow);
		for (auto &node : job.Work)
			if (node.Image) DropPortalImage(node.Image);
		job = {};
		State->ReapPortalPreparedTrees(*this);
	}

	PortalPreparedTree *Renderer::Impl::FindPortalPreparedTree(uint64_t token) {
		for (auto &prepared : PortalPreparedTrees)
			if (token && prepared.Token == token) return &prepared;
		return nullptr;
	}
	void Renderer::Impl::ReapPortalPreparedTrees(Renderer &renderer) {
		for (auto &prepared : PortalPreparedTrees) {
			if (!prepared.Token || !prepared.Cancelled || PortalTreeJob.Prepared == prepared.Token) continue;
			if (prepared.Command) {
				FinishPortalShadowImports(prepared.Command, false);
				SDL_CancelGPUCommandBuffer(prepared.Command);
				prepared.Command = nullptr;
			}
			if (prepared.Submitted && !CompletedPreparedFence(Device, prepared.Fence)) continue;
			const auto shadows = prepared.Shadows;
			const uint64_t lease = prepared.Lease;
			ReleasePortalPreparedShadowAssembly(prepared);
			prepared = {};
			for (const auto shadow : shadows)
				if (shadow) renderer.DropPortalShadowImage(shadow);
			renderer.ReleasePortalCaptureTreeLease(lease);
		}
	}
	void Renderer::Impl::ReleasePortalPreparedShadowAssembly(PortalPreparedTree &prepared) {
		PortalImportUsage.PendingCpuBytes -= prepared.ShadowAssemblyBytes;
		prepared.ShadowAssemblyBytes = 0;
		prepared.ShadowAssembly.reset();
		prepared.ShadowManifest.reset();
		prepared.AssembledShadow.reset();
		prepared.ShadowAssemblyNode = 0;
		ReportPortalImportUsage();
	}
	void Renderer::Impl::CancelPortalPreparedTrees(Renderer &renderer, uint64_t tree) {
		for (auto &prepared : PortalPreparedTrees) {
			if (!prepared.Token || (tree && prepared.Tree != tree)) continue;
			prepared.Cancelled = true;
			if (PortalTreeJob.Prepared == prepared.Token)
				renderer.CancelPortalCaptureTreeComposition(PortalTreeJob.Token);
		}
		ReapPortalPreparedTrees(renderer);
	}
	PortalTreeCompositionStatus Renderer::Impl::UploadPortalPreparedShadow(PortalPreparedTree &prepared) {
		if (prepared.Submitted) {
			if (!CompletedPreparedFence(Device, prepared.Fence)) return PortalTreeCompositionStatus::Pending;
			prepared.Submitted = false;
			if (!prepared.UploadConfirmed) return PortalTreeCompositionStatus::BudgetExceeded;
			return PortalTreeCompositionStatus::Pending;
		}
		if (!prepared.PendingUpload) return PortalTreeCompositionStatus::Pending;
		auto *shadow = FindPortalShadow(prepared.Shadows[prepared.NextNode - 1]);
		if (!shadow) return PortalTreeCompositionStatus::Invalid;
		if (!prepared.Command) prepared.Command = SDL_AcquireGPUCommandBuffer(Device);
		if (!prepared.Command) return PortalTreeCompositionStatus::BudgetExceeded;
		if (!RecordPortalShadowImport(prepared.Command, *shadow)) {
			FinishPortalShadowImports(prepared.Command, false);
			SDL_CancelGPUCommandBuffer(prepared.Command);
			prepared.Command = nullptr;
			return PortalTreeCompositionStatus::BudgetExceeded;
		}
		prepared.PendingUpload = false;
		--prepared.NextNode;
		if (prepared.NextNode) return PortalTreeCompositionStatus::Pending;
		prepared.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(prepared.Command);
		prepared.UploadConfirmed = prepared.Fence != nullptr;
		prepared.Submitted = true;
		FinishPortalShadowImports(prepared.Command, prepared.UploadConfirmed);
		prepared.Command = nullptr;
		return PortalTreeCompositionStatus::Pending;
	}
	PortalTreeCompositionStatus Renderer::BeginPortalCaptureTreePreparation(
		uint64_t treeToken, const View &referenceBody, uint64_t &token
	) {
		RequireOwningThread("BeginPortalCaptureTreePreparation");
		token = 0;
		State->ReapPortalPreparedTrees(*this);
		const auto *tree = FindPortalCaptureTree(treeToken);
		if (!State->Device || !tree || tree->Nodes.empty() || !State->NextPortalPreparation)
			return PortalTreeCompositionStatus::Invalid;
		const auto free = std::find_if(
			State->PortalPreparedTrees.begin(), State->PortalPreparedTrees.end(), [](const auto &prepared) {
				return !prepared.Token;
			}
		);
		if (free == State->PortalPreparedTrees.end()) return PortalTreeCompositionStatus::BudgetExceeded;
		std::array<PortalTreeNodeWork, MAX_PORTAL_CAPTURE_TREE_NODES> reference;
		if (!PrepareTree(tree, referenceBody, reference)) return PortalTreeCompositionStatus::Invalid;
		for (size_t index = 0; index < tree->Nodes.size(); ++index) {
			PortalTreeShadowRequest request;
			if (!ShadowRequestOf(*tree, State->ImportedPortals, State->NextPortalPreparation, index, request))
				return PortalTreeCompositionStatus::Invalid;
		}
		const auto lease = AcquirePortalCaptureTreeLease(treeToken);
		if (!lease) return PortalTreeCompositionStatus::BudgetExceeded;
		free->Token = State->NextPortalPreparation++;
		free->Tree = treeToken;
		free->Lease = lease;
		free->NextNode = tree->Nodes.size();
		for (size_t index = 0; index < tree->Nodes.size(); ++index)
			free->BodyBounds[index] = BodyBounds(reference[index]);
		token = free->Token;
		return PortalTreeCompositionStatus::Pending;
	}
	PortalTreeCompositionProgress Renderer::PollPortalCaptureTreePreparation(uint64_t token) {
		RequireOwningThread("PollPortalCaptureTreePreparation");
		State->ReapPortalPreparedTrees(*this);
		auto *prepared = State->FindPortalPreparedTree(token);
		if (!prepared || prepared->Cancelled) return {};
		const auto *tree = FindPortalCaptureTree(prepared->Tree);
		if (!tree) {
			CancelPortalCaptureTreePreparation(token);
			return {};
		}
		if (prepared->PendingUpload) {
			const auto status = State->UploadPortalPreparedShadow(*prepared);
			if (status == PortalTreeCompositionStatus::Invalid) {
				CancelPortalCaptureTreePreparation(token);
				return {};
			}
			if (status == PortalTreeCompositionStatus::BudgetExceeded || prepared->PendingUpload)
				return {.Status = status};
		}
		if (!prepared->NextNode) return {.Status = PortalTreeCompositionStatus::Complete};
		if (State->PortalTreeJob.Token && !State->PortalTreeJob.Prepared)
			return {.Status = PortalTreeCompositionStatus::Pending};
		PortalTreeShadowRequest request;
		if (!ShadowRequestOf(*tree, State->ImportedPortals, token, prepared->NextNode - 1, request)) {
			CancelPortalCaptureTreePreparation(token);
			return {};
		}
		request.BodyBounds = prepared->BodyBounds[request.Node];
		return {.Status = PortalTreeCompositionStatus::Pending, .Request = std::move(request)};
	}
	PortalTreeCompositionStatus
	Renderer::AcceptPortalPreparedShadow(uint64_t token, PortalShadowImage &&image) {
		RequireOwningThread("AcceptPortalPreparedShadow");
		ENGINE_PROFILE("prepare tree shadow");
		const auto progress = PollPortalCaptureTreePreparation(token);
		if (progress.Status != PortalTreeCompositionStatus::Pending || !progress.Request)
			return progress.Status;
		auto *prepared = State->FindPortalPreparedTree(token);
		if (!prepared) return PortalTreeCompositionStatus::Invalid;
		const bool assembled = prepared->AssembledShadow && &image == &*prepared->AssembledShadow;
		if (prepared->ShadowAssemblyBytes && !assembled) return PortalTreeCompositionStatus::Invalid;
		if (image.Depth.size() != PORTAL_SHADOW_BYTES) return PortalTreeCompositionStatus::Invalid;
		const size_t ownBytes = assembled ? prepared->ShadowAssemblyBytes : 0;
		if (image.Depth.capacity() >
			MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes + ownBytes)
			return PortalTreeCompositionStatus::BudgetExceeded;
		const auto &expected = *progress.Request;
		if (!MatchesPortalTreeShadowRequest(expected, image.Snapshot))
			return PortalTreeCompositionStatus::Invalid;
		const auto *tree = FindPortalCaptureTree(prepared->Tree);
		if (!tree) return PortalTreeCompositionStatus::Invalid;
		const auto &node = tree->Nodes[expected.Node];
		const PortalShadowImageBinding binding{node.Binding.World, node.Binding.WorldName, image.Snapshot};
		State->PortalImportUsage.PendingCpuBytes -= ownBytes;
		const auto shadow = QueuePackedPortalShadowImage(binding, std::move(image));
		if (!shadow) {
			State->PortalImportUsage.PendingCpuBytes += ownBytes;
			return PortalTreeCompositionStatus::BudgetExceeded;
		}
		if (assembled) {
			prepared->ShadowAssemblyBytes = 0;
			State->ReleasePortalPreparedShadowAssembly(*prepared);
		}
		State->FindPortalShadow(shadow)->DedicatedStaging = true;
		prepared->Shadows[expected.Node] = shadow;
		prepared->PendingUpload = true;
		return State->UploadPortalPreparedShadow(*prepared);
	}
	PortalTreeCompositionStatus Renderer::BeginPortalCaptureTreePreparationShadowAssembly(
		uint64_t token, const PortalShadowSnapshot &manifest
	) {
		RequireOwningThread("BeginPortalCaptureTreePreparationShadowAssembly");
		ENGINE_PROFILE("prepared portal shadow assembly begin");
		const auto progress = PollPortalCaptureTreePreparation(token);
		if (progress.Status != PortalTreeCompositionStatus::Pending) return progress.Status;
		if (!progress.Request || !MatchesPortalTreeShadowRequest(*progress.Request, manifest))
			return PortalTreeCompositionStatus::Invalid;
		auto *prepared = State->FindPortalPreparedTree(token);
		if (!prepared || prepared->Cancelled || prepared->PendingUpload || prepared->AssembledShadow)
			return PortalTreeCompositionStatus::Invalid;
		if (prepared->ShadowManifest)
			return *prepared->ShadowManifest == manifest ? PortalTreeCompositionStatus::Pending
														 : PortalTreeCompositionStatus::Invalid;
		const size_t budget = MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes;
		if (budget < PORTAL_SHADOW_BYTES) return PortalTreeCompositionStatus::BudgetExceeded;
		auto assembly = std::make_unique<PortalShadowAssembly>();
		std::string error;
		if (!assembly->Begin(manifest, budget, error)) return PortalTreeCompositionStatus::BudgetExceeded;
		prepared->ShadowAssemblyBytes = assembly->Bytes();
		State->PortalImportUsage.PendingCpuBytes += prepared->ShadowAssemblyBytes;
		prepared->ShadowAssemblyNode = progress.Request->Node;
		prepared->ShadowManifest = manifest;
		prepared->ShadowAssembly = std::move(assembly);
		core::Metrics::Count("render.portal_shadow.assembly_bytes", prepared->ShadowAssemblyBytes);
		core::Metrics::Count("render.portal_shadow.assemblies", 1);
		State->ReportPortalImportUsage();
		return PortalTreeCompositionStatus::Pending;
	}
	PortalTreeCompositionStatus Renderer::AcceptPortalPreparedShadowTile(
		uint64_t token, uint32_t node, std::span<const std::byte> packet
	) {
		RequireOwningThread("AcceptPortalPreparedShadowTile");
		auto *prepared = State->FindPortalPreparedTree(token);
		if (!prepared || prepared->Cancelled || !prepared->ShadowAssembly ||
			prepared->ShadowAssemblyNode != node || prepared->NextNode != size_t(node) + 1)
			return PortalTreeCompositionStatus::Invalid;
		if (prepared->PendingUpload || prepared->Submitted || prepared->AssembledShadow)
			return PortalTreeCompositionStatus::Pending;
		std::string error;
		if (!prepared->ShadowAssembly->Accept(packet, error)) {
			if (!prepared->ShadowAssembly->Bytes()) State->ReleasePortalPreparedShadowAssembly(*prepared);
			return PortalTreeCompositionStatus::Invalid;
		}
		prepared->AssembledShadow = prepared->ShadowAssembly->Take();
		core::Metrics::Count("render.portal_shadow.assembly_received_bytes", packet.size());
		if (!prepared->AssembledShadow) return PortalTreeCompositionStatus::Pending;
		return CommitPortalPreparedShadowAssembly(token, node);
	}
	PortalTreeCompositionStatus Renderer::CommitPortalPreparedShadowAssembly(uint64_t token, uint32_t node) {
		RequireOwningThread("CommitPortalPreparedShadowAssembly");
		auto *prepared = State->FindPortalPreparedTree(token);
		if (!prepared || prepared->Cancelled || prepared->PendingUpload || prepared->Submitted ||
			!prepared->ShadowManifest || prepared->ShadowAssemblyNode != node ||
			prepared->NextNode != size_t(node) + 1)
			return PortalTreeCompositionStatus::Invalid;
		if (!prepared->AssembledShadow) return PortalTreeCompositionStatus::Pending;
		return AcceptPortalPreparedShadow(token, std::move(*prepared->AssembledShadow));
	}
	void Renderer::CancelPortalCaptureTreePreparation(uint64_t token) {
		RequireOwningThread("CancelPortalCaptureTreePreparation");
		auto *prepared = State->FindPortalPreparedTree(token);
		if (!prepared) return;
		prepared->Cancelled = true;
		if (State->PortalTreeJob.Prepared == token)
			CancelPortalCaptureTreeComposition(State->PortalTreeJob.Token);
		State->ReapPortalPreparedTrees(*this);
	}
	PortalTreeCompositionStatus Renderer::BeginPreparedPortalCaptureTreeComposition(
		uint64_t token, const View &body, uint64_t &jobToken
	) {
		RequireOwningThread("BeginPreparedPortalCaptureTreeComposition");
		ENGINE_PROFILE("compose prepared tree pose");
		jobToken = 0;
		const auto readiness = PollPortalCaptureTreePreparation(token);
		if (readiness.Status != PortalTreeCompositionStatus::Complete) return readiness.Status;
		auto *prepared = State->FindPortalPreparedTree(token);
		const auto status = BeginPortalCaptureTreeComposition(prepared->Tree, body, jobToken);
		if (status != PortalTreeCompositionStatus::Pending) return status;
		auto &job = State->PortalTreeJob;
		job.Prepared = token;
		const auto *tree = FindPortalCaptureTree(prepared->Tree);
		size_t outputBytes = 0;
		for (size_t index = 0; index < tree->Nodes.size(); ++index) {
			const auto &node = tree->Nodes[index];
			auto *shadow = State->FindPortalShadow(prepared->Shadows[index]);
			PortalTreeShadowRequest request;
			if (!shadow || !shadow->Ready ||
				!ShadowRequestOf(*tree, State->ImportedPortals, jobToken, index, request)) {
				CancelPortalCaptureTreeComposition(jobToken);
				jobToken = 0;
				return PortalTreeCompositionStatus::Invalid;
			}
			request.BodyBounds = BodyBounds(job.Work[index]);
			if (!MatchesPortalTreeShadowRequest(request, shadow->Binding.ExpectedSnapshot)) {
				CancelPortalCaptureTreeComposition(jobToken);
				jobToken = 0;
				return PortalTreeCompositionStatus::Invalid;
			}
			outputBytes += size_t(node.Width) * node.Height * 12;
		}
		const auto freeImages = std::count_if(
			State->ImportedPortals.begin(), State->ImportedPortals.end(), [](const auto &image) {
				return !image.Handle;
			}
		);
		if (outputBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes ||
			static_cast<size_t>(freeImages) < tree->Nodes.size()) {
			CancelPortalCaptureTreeComposition(jobToken);
			jobToken = 0;
			return PortalTreeCompositionStatus::BudgetExceeded;
		}
		// Every node records from this one owned pose. Source buffers are immutable and uploaded;
		// child outputs and the prepared lease remain charged until the final queue fence.
		for (size_t index = tree->Nodes.size(); index-- > 0;) {
			const auto &node = tree->Nodes[index];
			auto &work = job.Work[index];
			ConnectChildren(*tree, job.Work, index);
			const auto *shadow = State->FindPortalShadow(prepared->Shadows[index]);
			const SceneTarget target{node.Width, node.Height};
			auto view = body;
			view.World = node.Binding.World;
			view.WorldName = node.Binding.WorldName;
			view.Slot = node.Binding.ViewSlot;
			view.ContentOwner = job.ContentOwner;
			view.ForeignContentOwners = job.ContentOwners;
			view.Instances = work.Body;
			view.JointFrames = job.Joints;
			if (index == 0) {
				view.EyeRig = job.EyeRig;
				view.EyePlayer = job.EyePlayer;
				view.EyeHiddenRows = job.EyeHiddenRows;
			} else {
				view.EyeRig = 0;
				view.EyePlayer.reset();
				view.EyeHiddenRows = {};
			}
			view.Target = &target;
			view.DirectionalShadowBounds = Box(shadow->Binding.ExpectedSnapshot.DomainBounds);
			view.ImportedDirectionalShadow = shadow->Handle;
			work.Image = ComposePortalBodyImageInternal(
				CaptureOf(node),
				view,
				work.Output,
				work.ApertureRows,
				work.ApertureJoints,
				work.Apertures,
				true
			);
			job.Submitted = true;
			if (!work.Image) {
				job.AfterFence = PortalTreeCompositionStatus::Invalid;
				break;
			}
			job.NextNode = index;
		}
		auto *command = SDL_AcquireGPUCommandBuffer(State->Device);
		if (command) job.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		return PortalTreeCompositionStatus::Pending;
	}

}

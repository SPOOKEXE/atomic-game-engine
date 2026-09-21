#include "RendererState.hpp"

#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalImageRuntime.hpp>

#include <atomic>
#include <limits>

namespace engine::render {
	namespace {
		std::atomic<uint64_t> NextTreeToken{1};
		uint64_t TakeTreeToken() {
			auto token = NextTreeToken.load(std::memory_order_relaxed);
			while (token != 0) {
				const auto next = token == std::numeric_limits<uint64_t>::max() ? 0 : token + 1;
				if (NextTreeToken.compare_exchange_weak(token, next, std::memory_order_relaxed)) return token;
			}
			return 0;
		}
		size_t MetadataBytes(const ImportedPortalCaptureTree &tree) {
			size_t bytes = sizeof(tree) + tree.Nodes.capacity() * sizeof(ImportedPortalCaptureTreeNode) +
						   tree.Edges.capacity() * sizeof(PortalCaptureTreeEdge);
			for (const auto &node : tree.Nodes) {
				bytes += node.Producer.World.capacity() + node.Producer.Channel.capacity() +
						 node.Binding.Expected.PortalKey.capacity() + node.RetainedBodyPlayer.capacity() +
						 node.Lenses.Entries.capacity() * sizeof(PortalCaptureLens);
				for (const auto &lens : node.Lenses.Entries)
					bytes += lens.Shader.capacity();
			}
			for (const auto &edge : tree.Edges)
				bytes += edge.PortalKey.capacity() + edge.Geometry.capacity();
			return bytes;
		}
	}

	uint64_t Renderer::QueuePortalCaptureTree(const PortalImageBinding &root, PortalCaptureTree &&tree) {
		RequireOwningThread("QueuePortalCaptureTree");
		ENGINE_PROFILE("portal capture tree import");
		if (!State->Device || !ValidPortalCaptureTree(tree) || root.Layer != 0 ||
			root.Expected != tree.Nodes.front().Layers.Opaque.Key ||
			root.Portal.Text() != root.Expected.PortalKey ||
			root.ExpectedScope != PortalImageScope::OpaqueLighting ||
			root.ExpectedProjection != tree.Nodes.front().Camera.Projection)
			return 0;
		auto free = std::find_if(
			State->ImportedPortalTrees.begin(), State->ImportedPortalTrees.end(), [](const auto &candidate) {
				return candidate.Token == 0;
			}
		);
		if (free == State->ImportedPortalTrees.end()) return 0;
		ImportedPortalCaptureTree prepared;
		prepared.Nodes.resize(tree.Nodes.size());
		prepared.Edges = tree.Edges;
		size_t images = 0;
		for (size_t index = 0; index < tree.Nodes.size(); ++index) {
			const auto &node = tree.Nodes[index];
			auto &imported = prepared.Nodes[index];
			imported.Producer = node.Producer;
			imported.RetainedBodyPlayer = node.RetainedBodyPlayer;
			imported.Camera = node.Camera;
			imported.Binding = root;
			imported.Binding.Expected = node.Layers.Opaque.Key;
			imported.Binding.Portal = core::Name(node.Layers.Opaque.Key.PortalKey);
			imported.Binding.ExpectedProjection = node.Camera.Projection;
			imported.Lighting = *node.Layers.Opaque.CaptureLighting;
			imported.Lenses.TimeSeconds = node.Layers.Lenses.TimeSeconds;
			imported.Lenses.Entries = node.Layers.Lenses.Entries;
			imported.Width = node.Layers.Opaque.Width;
			imported.Height = node.Layers.Opaque.Height;
			images += 1 + node.Layers.Transparent.size() + node.Layers.SpatialOverlay.has_value();
			if (index != 0) {
				View cameraView;
				const PortalCaptureCamera camera{
					node.Camera.Position, node.Camera.Orientation, node.Camera.Frustum, node.Camera.ClipPlane
				};
				if (!ResolvePortalCaptureCamera(camera, node.Camera.Projection, cameraView) ||
					!cameraView.Projection)
					return 0;
				imported.Binding.Sampling =
					*cameraView.Projection * cameraView.CameraFrame.Inverse().ToMatrix();
			}
		}
		if (images > MAX_IMPORTED_PORTAL_IMAGES - State->PortalImportUsage.Images) return 0;
		size_t textureBytes = 0, cpuBytes = 0;
		const auto charge = [&](const PortalImageReply &image) {
			textureBytes += image.Pixels.size() + image.Depth.size() + image.Normal.size() +
							image.AmbientResponse.size() + image.LightingBaseline.size() +
							image.DirectionalResponse.size();
			cpuBytes += image.Pixels.capacity() + image.Depth.capacity() + image.Normal.capacity() +
						image.AmbientResponse.capacity() + image.LightingBaseline.capacity() +
						image.DirectionalResponse.capacity();
		};
		for (const auto &node : tree.Nodes) {
			charge(node.Layers.Opaque);
			for (const auto &layer : node.Layers.Transparent)
				charge(layer);
			if (node.Layers.SpatialOverlay) charge(*node.Layers.SpatialOverlay);
		}
		if (textureBytes > MAX_IMPORTED_PORTAL_TEXTURE_BYTES - State->PortalImportUsage.TextureBytes ||
			cpuBytes > MAX_IMPORTED_PORTAL_CPU_BYTES - State->PortalImportUsage.PendingCpuBytes)
			return 0;
		prepared.MetadataBytes = MetadataBytes(prepared);
		size_t heldBytes = 0;
		for (const auto &held : State->ImportedPortalTrees)
			heldBytes += held.MetadataBytes;
		if (prepared.MetadataBytes > MAX_IMPORTED_PORTAL_TREE_METADATA_BYTES - heldBytes) return 0;
		const auto rollback = [&] {
			for (const auto &node : prepared.Nodes) {
				DropPortalImage(node.Images[0]);
				ReleasePortalLensPrograms(node.LensPrograms);
			}
		};
		for (size_t index = 0; index < tree.Nodes.size(); ++index) {
			auto &layers = tree.Nodes[index].Layers;
			auto &node = prepared.Nodes[index];
			if (!layers.Lenses.Entries.empty()) {
				node.LensPrograms = RetainPortalLensPrograms(layers.Lenses);
				if (node.LensPrograms == 0) {
					rollback();
					return 0;
				}
			}
			const size_t count = 1 + layers.Transparent.size() + layers.SpatialOverlay.has_value();
			const size_t physical = layers.Transparent.size();
			const bool top = layers.SpatialOverlay.has_value();
			std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS + 2> handles{};
			if (!QueuePortalImageLayerSet(node.Binding, std::move(layers), std::span(handles).first(count))) {
				rollback();
				return 0;
			}
			std::copy_n(handles.begin(), physical + 1, node.Images.begin());
			if (top) node.Images.back() = handles[count - 1];
		}
		prepared.Token = TakeTreeToken();
		if (prepared.Token == 0) {
			rollback();
			return 0;
		}
		*free = std::move(prepared);
		return free->Token;
	}

	bool Renderer::PortalCaptureTreeReady(uint64_t token) const {
		RequireOwningThread("PortalCaptureTreeReady");
		if (token == 0) return false;
		for (const auto &tree : State->ImportedPortalTrees) {
			if (tree.Token != token || tree.Revoked) continue;
			for (const auto &node : tree.Nodes)
				if (!PortalImageLayerSetReady(node.Images[0])) return false;
			return !tree.Nodes.empty();
		}
		return false;
	}

	const ImportedPortalCaptureTree *Renderer::FindPortalCaptureTree(uint64_t token) const {
		if (!PortalCaptureTreeReady(token)) return nullptr;
		for (const auto &tree : State->ImportedPortalTrees)
			if (tree.Token == token) return &tree;
		return nullptr;
	}

	uint64_t Renderer::AcquirePortalCaptureTreeLease(uint64_t token) {
		RequireOwningThread("AcquirePortalCaptureTreeLease");
		if (!PortalCaptureTreeReady(token)) return 0;
		size_t held = 0;
		for (const auto &tree : State->ImportedPortalTrees)
			for (const auto lease : tree.Leases)
				held += lease != 0;
		if (held >= MAX_PORTAL_CAPTURE_TREE_LEASES) return 0;
		for (auto &tree : State->ImportedPortalTrees) {
			if (tree.Token != token || !tree.Owner || tree.Revoked) continue;
			for (auto &lease : tree.Leases) {
				if (lease != 0) continue;
				lease = TakeTreeToken();
				return lease;
			}
		}
		return 0;
	}

	void Renderer::ReleasePortalCaptureTreeLease(uint64_t lease) {
		RequireOwningThread("ReleasePortalCaptureTreeLease");
		if (lease == 0) return;
		for (auto &tree : State->ImportedPortalTrees) {
			for (auto &held : tree.Leases) {
				if (held != lease) continue;
				held = 0;
				if (!tree.Owner && std::none_of(tree.Leases.begin(), tree.Leases.end(), [](auto value) {
						return value != 0;
					}))
					DropPortalCaptureTree(tree.Token);
				return;
			}
		}
	}

	void Renderer::ReleasePortalCaptureTree(uint64_t token) {
		RequireOwningThread("ReleasePortalCaptureTree");
		if (token == 0) return;
		for (auto &tree : State->ImportedPortalTrees) {
			if (tree.Token != token) continue;
			tree.Owner = false;
			if (std::none_of(tree.Leases.begin(), tree.Leases.end(), [](auto lease) { return lease != 0; }))
				DropPortalCaptureTree(token);
			return;
		}
	}

	void Renderer::DropPortalCaptureTree(uint64_t token) {
		RequireOwningThread("DropPortalCaptureTree");
		if (token == 0) return;
		for (auto &tree : State->ImportedPortalTrees) {
			if (tree.Token != token) continue;
			tree.Owner = false;
			tree.Revoked = true;
			break;
		}
		if (State->PortalTreeJob.Tree == token)
			CancelPortalCaptureTreeComposition(State->PortalTreeJob.Token);
		State->CancelPortalPreparedTrees(*this, token);
		for (auto &tree : State->ImportedPortalTrees) {
			if (tree.Token != token) continue;
			const bool leased =
				std::any_of(tree.Leases.begin(), tree.Leases.end(), [](auto lease) { return lease != 0; });
			if (State->PortalTreeJob.Tree == token || leased) return;
			// Remove membership before dropping groups: member retirement routes here.
			auto retired = std::move(tree);
			tree = {};
			for (const auto &node : retired.Nodes) {
				DropPortalImage(node.Images[0]);
				ReleasePortalLensPrograms(node.LensPrograms);
			}
			return;
		}
	}
}

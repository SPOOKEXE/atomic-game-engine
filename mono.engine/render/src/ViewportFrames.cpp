#include "PresentationSource.hpp"
#include "ViewportFrameScene.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ViewportFrames.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {

	namespace {
		constexpr uint32_t MAX_VIEWPORT_EDGE = 2048;
		constexpr size_t MAX_VIEWPORT_FRAMES = 16;
		constexpr uint64_t MAX_VIEWPORT_TARGET_BYTES = 128ull * 1024ull * 1024ull;
		constexpr uint64_t VIEWPORT_BYTES_PER_PIXEL = 8;
		constexpr int MAX_TREE_DEPTH = 256;

		void CollectViewportDescendants(
			const ecs::Store &store, ecs::Entity parent, int depth, std::vector<scene::DrawInstance> &out
		) {
			if (depth > MAX_TREE_DEPTH) {
				return;
			}

			store.EachChild(parent, [&](ecs::Entity child) {
				// A nested ViewportFrame owns a second miniature world. This pass only
				// collects the root frame's world, so descending would merge two target
				// ownership domains and make its cost unbounded.
				if (store.Get<gui::Viewport>(child) != nullptr) return;
				const auto *transform = store.Get<scene::Transform>(child);
				if (transform != nullptr && PresentationSource::IsViewportDrawable(store, child)) {
					out.push_back(
						PresentationSource::MakeDrawInstance(
							store, child, transform->Frame, PresentationSource::LocalTransparencyMode::Ignore
						)
					);
				}

				CollectViewportDescendants(store, child, depth + 1, out);
			});
		}

		uint32_t ScaledEdge(uint32_t edge, float scale, uint32_t maximum) {
			const float validScale = std::isfinite(scale) ? std::clamp(scale, 0.25f, 2.0f) : 1.0f;
			const float requested = std::ceil(static_cast<float>(edge) * validScale);
			return std::clamp(
				static_cast<uint32_t>(
					std::min(requested, static_cast<float>(std::numeric_limits<uint32_t>::max()))
				),
				1u,
				maximum
			);
		}

		uint64_t TargetBytes(uint32_t width, uint32_t height) {
			return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * VIEWPORT_BYTES_PER_PIXEL;
		}
	}

	void CollectViewportInstances(
		const ecs::Store &store, ecs::Entity viewport, std::vector<scene::DrawInstance> &out
	) {
		CollectViewportDescendants(store, viewport, 0, out);
	}

	size_t ViewportFrames::Render(
		Renderer &renderer, ecs::Store &store, const gui::DrawList &list, size_t firstSlot, core::Name owner
	) {
		Entries.clear();
		std::vector<std::vector<scene::DrawInstance>> instances;
		std::vector<SceneTarget> targets;
		std::vector<View> views;
		struct PendingCacheCommit {
			size_t Index = 0;
			size_t Slot = 0;
			ecs::Entity Instance;
			uint64_t Signature = 0;
			uint32_t InvalidationRevision = 0;
		};
		std::vector<PendingCacheCommit> pending;
		instances.reserve(std::min(list.Commands.size(), MAX_VIEWPORT_FRAMES));
		targets.reserve(std::min(list.Commands.size(), MAX_VIEWPORT_FRAMES));
		views.reserve(std::min(list.Commands.size(), MAX_VIEWPORT_FRAMES));
		pending.reserve(std::min(list.Commands.size(), MAX_VIEWPORT_FRAMES));
		Entries.reserve(std::min(list.Commands.size(), MAX_VIEWPORT_FRAMES));
		const scene::WorldLighting baseLighting = renderer.CurrentLighting();
		const uint64_t generation = renderer.RenderGeneration() + 1;
		uint64_t targetBytes = 0;

		for (const gui::DrawCommand &command : list.Commands) {
			if (command.Kind != gui::DrawKind::Viewport || command.Transparency >= 1.0f ||
				command.Bounds.Width() <= 0.0f || command.Bounds.Height() <= 0.0f ||
				Entries.size() >= MAX_VIEWPORT_FRAMES ||
				std::any_of(Entries.begin(), Entries.end(), [&](const Entry &entry) {
					return entry.Instance == command.Source;
				})) {
				continue;
			}

			const auto *viewport = store.Get<gui::Viewport>(command.Source);
			const auto *placement =
				viewport != nullptr ? store.Get<scene::Transform>(viewport->CurrentCamera) : nullptr;
			const auto *lens =
				viewport != nullptr ? store.Get<scene::Camera>(viewport->CurrentCamera) : nullptr;
			if (viewport == nullptr || placement == nullptr || lens == nullptr) {
				continue;
			}

			const uint32_t width = std::clamp(
				static_cast<uint32_t>(std::ceil(std::max(command.Bounds.Width(), 1.0f))),
				1u,
				MAX_VIEWPORT_EDGE
			);
			const uint32_t height = std::clamp(
				static_cast<uint32_t>(std::ceil(std::max(command.Bounds.Height(), 1.0f))),
				1u,
				MAX_VIEWPORT_EDGE
			);
			const uint32_t rawImageWidth =
				lens->ImageWidth > 0 && lens->ImageHeight > 0 ? lens->ImageWidth : width;
			const uint32_t rawImageHeight =
				lens->ImageWidth > 0 && lens->ImageHeight > 0 ? lens->ImageHeight : height;
			const uint32_t maxWidth = lens->MaxImageWidth == 0 ? MAX_VIEWPORT_EDGE : lens->MaxImageWidth;
			const uint32_t maxHeight = lens->MaxImageHeight == 0 ? MAX_VIEWPORT_EDGE : lens->MaxImageHeight;
			const uint32_t imageWidth =
				ScaledEdge(rawImageWidth, viewport->ResolutionScale, std::min(maxWidth, MAX_VIEWPORT_EDGE));
			const uint32_t imageHeight =
				ScaledEdge(rawImageHeight, viewport->ResolutionScale, std::min(maxHeight, MAX_VIEWPORT_EDGE));
			if (TargetBytes(imageWidth, imageHeight) > MAX_VIEWPORT_TARGET_BYTES - targetBytes) continue;

			const size_t slot = firstSlot + Entries.size();
			instances.emplace_back();
			CollectViewportInstances(store, command.Source, instances.back());
			targets.push_back({std::clamp(imageWidth, 1u, maxWidth), std::clamp(imageHeight, 1u, maxHeight)});

			View view;
			view.CameraFrame = placement->Frame;
			view.Camera = *lens;
			view.Instances = instances.back();
			view.Target = &targets.back();
			view.Slot = slot;
			// Each ViewportFrame owns a miniature scene rooted at itself. Two
			// frames in the same Store are not two cameras on one world, so their
			// world-scoped shadow work must not be shared.
			view.World = command.Source.Id;
			view.WorldName = core::Name("render.viewport-frame");
			view.ContentOwner = owner;
			view.Lighting = baseLighting;
			view.Lighting.Direction = viewport->LightDirection;
			view.Lighting.Ambient = viewport->Ambient;
			view.Lighting.Direct = viewport->LightColor;
			view.OverrideLighting = true;
			ScenePresentationState presentation;
			presentation.Lighting = baseLighting;
			presentation.Resources = renderer.ResourceRevision();
			uint64_t signature = ScenePresentationSignature(view, presentation);
			signature =
				scene::MixSignature(signature, ViewportPresentationSignature(imageWidth, imageHeight));
			signature = scene::MixSignature(signature, slot);

			auto cached = std::find_if(Cached.begin(), Cached.end(), [&](const CachedViewport &entry) {
				return entry.Instance == command.Source;
			});
			if (cached == Cached.end()) {
				if (Cached.size() >= MAX_VIEWPORT_FRAMES) {
					cached = std::min_element(
						Cached.begin(),
						Cached.end(),
						[](const CachedViewport &left, const CachedViewport &right) {
							return left.LastSeenGeneration < right.LastSeenGeneration;
						}
					);
					*cached =
						CachedViewport{.Instance = command.Source, .Cache = {}, .LastSeenGeneration = 0};
				} else {
					Cached.push_back(
						CachedViewport{.Instance = command.Source, .Cache = {}, .LastSeenGeneration = 0}
					);
					cached = std::prev(Cached.end());
				}
			}
			auto target = std::find_if(Targets.begin(), Targets.end(), [&](const CachedTarget &entry) {
				return entry.Slot == slot;
			});
			const bool ownsTarget = target != Targets.end() && target->Owner.Owns(command.Source.Id);
			if (ownsTarget) target->LastSeenGeneration = generation;
			const bool update = !ownsTarget || cached->Cache.NeedsRender(
												   viewport->UpdateMode,
												   viewport->UpdateEveryFrames,
												   viewport->InvalidationRevision,
												   signature,
												   generation
											   );
			cached->LastSeenGeneration = generation;
			const size_t cachedIndex = static_cast<size_t>(cached - Cached.begin());
			if (update)
				pending.push_back(
					{cachedIndex, slot, command.Source, signature, viewport->InvalidationRevision}
				);
			view.Damage.Scene = update;
			view.Damage.Objects = update;
			view.Damage.Environment = update;
			view.Damage.Particles = update;
			view.Damage.Portals = update;
			views.push_back(view);
			targetBytes += TargetBytes(imageWidth, imageHeight);

			Entries.push_back(
				Entry{
					command.Source,
					nullptr,
					core::Vector2{1.0f, 1.0f},
					width,
					height,
				}
			);
		}

		const FrameResult result =
			views.empty() ? FrameResult{} : renderer.Render(views, EmptyOverlay, nullptr, false);
		if (result.Submitted) {
			for (const PendingCacheCommit &commit : pending) {
				Cached[commit.Index].Cache.Commit(commit.Signature, generation, commit.InvalidationRevision);
				auto target = std::find_if(Targets.begin(), Targets.end(), [&](const CachedTarget &entry) {
					return entry.Slot == commit.Slot;
				});
				if (target == Targets.end()) {
					if (Targets.size() >= MAX_VIEWPORT_FRAMES) {
						target = std::min_element(
							Targets.begin(),
							Targets.end(),
							[](const CachedTarget &left, const CachedTarget &right) {
								return left.LastSeenGeneration < right.LastSeenGeneration;
							}
						);
					} else {
						Targets.push_back(
							CachedTarget{
								.Slot = commit.Slot,
								.Owner = {},
								.LastSeenGeneration = 0,
							}
						);
						target = std::prev(Targets.end());
					}
				}
				target->Owner.Commit(commit.Instance.Id, generation);
				target->LastSeenGeneration = generation;
			}
		}
		for (size_t index = 0; index < Entries.size(); index++) {
			const size_t slot = firstSlot + index;
			const auto owner = std::find_if(Targets.begin(), Targets.end(), [&](const CachedTarget &entry) {
				return entry.Slot == slot;
			});
			// A failed submit may leave another frame's image in this slot.
			// Present only the last successfully committed owner's pixels.
			if (owner == Targets.end() || !owner->Owner.Owns(Entries[index].Instance.Id)) continue;
			const SceneExtent extent = renderer.SceneTextureExtent(slot);
			Entries[index].Texture = renderer.SceneTexture(slot);
			Entries[index].UVMax = core::Vector2{extent.U, extent.V};
		}
		return static_cast<size_t>(std::count_if(Entries.begin(), Entries.end(), [](const Entry &entry) {
			return entry.Texture != nullptr;
		}));
	}

	InterfaceImage ViewportFrames::Resolve(ecs::Entity instance) const {
		const auto found = std::find_if(Entries.begin(), Entries.end(), [&](const Entry &entry) {
			return entry.Instance == instance;
		});
		if (found == Entries.end()) {
			return {};
		}

		InterfaceImage image;
		image.Texture = found->Texture;
		image.UVMax = found->UVMax;
		image.Width = found->Width;
		image.Height = found->Height;
		return image;
	}
}

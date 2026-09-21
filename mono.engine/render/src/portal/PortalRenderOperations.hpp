#pragma once

#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/Renderer.hpp>

#include <memory>

namespace engine::render {
	// The portal coordinator only needs this bounded set of renderer operations.
	// Keeping it here stops transport and retirement state from depending on the
	// renderer's general frame and resource APIs.
	class PortalRenderOperations {
	  public:
		explicit PortalRenderOperations(Renderer &renderer) : Render(renderer) {}

		std::unique_ptr<PortalImageProducer> CreateProducer(
			world::Universe &universe,
			world::WorldId world,
			world::PresentationAddress requests,
			PortalResidentImages *resident,
			ShaderLibrary *shaders,
			bool postProcessing
		) {
			return std::make_unique<PortalImageProducer>(
				universe, Render, world, std::move(requests), resident, shaders, postProcessing
			);
		}
		std::unique_ptr<PortalImageSource> CreateSource(
			world::Universe &universe,
			world::WorldId world,
			world::PresentationAddress replies,
			PortalInboxLimits limits,
			PortalResidentImages *resident
		) {
			return std::make_unique<PortalImageSource>(
				universe, Render, world, std::move(replies), limits, resident
			);
		}
		bool InstallPipeline(core::Name name, const graph::RenderGraph &pipeline) {
			return Render.SetPipeline(name, pipeline);
		}
		const ImportedPortalCaptureTree *FindCaptureTree(uint64_t token) const {
			return Render.FindPortalCaptureTree(token);
		}
		uint64_t ComposeBodyImage(const PortalImageCapture &capture, const View &body) {
			return Render.ComposePortalBodyImage(capture, body);
		}
		PortalTreeCompositionStatus BeginComposition(uint64_t tree, const View &body, uint64_t &job) {
			return Render.BeginPortalCaptureTreeComposition(tree, body, job);
		}
		PortalTreeCompositionStatus BeginPreparation(uint64_t tree, const View &body, uint64_t &preparation) {
			return Render.BeginPortalCaptureTreePreparation(tree, body, preparation);
		}
		PortalTreeCompositionStatus
		BeginPreparedComposition(uint64_t preparation, const View &body, uint64_t &job) {
			return Render.BeginPreparedPortalCaptureTreeComposition(preparation, body, job);
		}
		PortalTreeCompositionStatus
		BeginShadowAssembly(uint64_t job, const PortalShadowSnapshot &manifest, bool preparation) {
			return preparation ? Render.BeginPortalCaptureTreePreparationShadowAssembly(job, manifest)
							   : Render.BeginPortalCaptureTreeShadowAssembly(job, manifest);
		}
		PortalTreeCompositionStatus
		UploadShadowTile(uint64_t job, uint32_t node, std::span<const std::byte> packet, bool preparation) {
			return preparation ? Render.AcceptPortalPreparedShadowTile(job, node, packet)
							   : Render.AcceptPortalCaptureTreeShadowTile(job, node, packet);
		}
		PortalTreeCompositionStatus CommitShadowAssembly(uint64_t job, uint32_t node, bool preparation) {
			return preparation ? Render.CommitPortalPreparedShadowAssembly(job, node)
							   : Render.CommitPortalCaptureTreeShadowAssembly(job, node);
		}
		PortalTreeCompositionProgress Poll(uint64_t job, bool preparation) {
			return preparation ? Render.PollPortalCaptureTreePreparation(job)
							   : Render.PollPortalCaptureTreeComposition(job);
		}
		void CancelComposition(uint64_t job) {
			Render.CancelPortalCaptureTreeComposition(job);
		}
		void CancelPreparation(uint64_t preparation) {
			Render.CancelPortalCaptureTreePreparation(preparation);
		}
		void ReleaseImage(uint64_t image) {
			Render.DropPortalImage(image);
		}

	  private:
		Renderer &Render;
	};
}

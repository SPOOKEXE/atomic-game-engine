#pragma once

#include <engine/render/InterfacePass.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/Renderer.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace engine::render {
#if ENGINE_ASSERTS_ENABLED
	namespace test_support {
		enum class PortalTerminalKind { ReleaseImage, CancelComposition, CancelPreparation };
		struct PortalTerminalCall {
			const Renderer *Owner = nullptr;
			PortalTerminalKind Kind = PortalTerminalKind::ReleaseImage;
			uint64_t Token = 0;
			bool Released = false;
		};
		struct PortalTerminalObserver {
			void *Context = nullptr;
			void (*Record)(void *, PortalTerminalCall) = nullptr;
		};
		inline thread_local PortalTerminalObserver PortalTerminalObserverForTests;
	}
#endif
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
		bool SetPipeline(core::Name name, const graph::RenderGraph &pipeline) {
			return InstallPipeline(name, pipeline);
		}
		Renderer &RendererRef() {
			return Render;
		}
		const Renderer &RendererRef() const {
			return Render;
		}
		bool OwnsResident(const PortalResidentImages &resident) const {
			return resident.Owns(Render);
		}
		// Source and producer state own portal work. These are the complete GPU
		// operations that state may request, so neither role reaches the renderer's
		// general frame state directly.
		bool HasDevice() const {
			return Render.Backend().Device != nullptr;
		}
		bool InitialiseInterface(InterfacePass &interface) const {
			const auto backend = Render.Backend();
			return backend.Device != nullptr && interface.Initialise(backend.Device, backend.ColourFormat);
		}

		template <typename... Args> decltype(auto) QueuePortalImage(Args &&...args) {
			return Render.QueuePortalImage(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) QueuePortalImageLayerSet(Args &&...args) {
			return Render.QueuePortalImageLayerSet(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) PortalImageLayerSetReady(Args &&...args) const {
			return Render.PortalImageLayerSetReady(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) QueuePortalCaptureTree(Args &&...args) {
			return Render.QueuePortalCaptureTree(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) FindPortalCaptureTree(Args &&...args) const {
			return Render.FindPortalCaptureTree(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) AcquirePortalCaptureTreeLease(Args &&...args) {
			return Render.AcquirePortalCaptureTreeLease(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) ReleasePortalCaptureTreeLease(Args &&...args) {
			return Render.ReleasePortalCaptureTreeLease(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) ReleasePortalCaptureTree(Args &&...args) {
			return Render.ReleasePortalCaptureTree(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) DropPortalCaptureTree(Args &&...args) {
			return Render.DropPortalCaptureTree(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) RetainPortalLensPrograms(Args &&...args) {
			return Render.RetainPortalLensPrograms(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) ReleasePortalLensPrograms(Args &&...args) {
			return Render.ReleasePortalLensPrograms(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) QueueResourceImage(Args &&...args) {
			return Render.QueueResourceImage(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) QueueResourceImages(Args &&...args) {
			return Render.QueueResourceImages(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) TakeResourceImage(Args &&...args) {
			return Render.TakeResourceImage(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) TakeResourceImages(Args &&...args) {
			return Render.TakeResourceImages(std::forward<Args>(args)...);
		}
		bool CancelResourceImage(uint64_t token) {
			return Render.CancelResourceImage(token);
		}
		uint64_t ResourceRevision() const {
			return Render.ResourceRevision();
		}
		template <typename... Args> decltype(auto) ForgetWorld(Args &&...args) {
			return Render.ForgetWorld(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) MeshExtentOf(Args &&...args) const {
			return Render.MeshExtentOf(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) TextureHandle(Args &&...args) const {
			return Render.TextureHandle(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) TextureCell(Args &&...args) const {
			return Render.TextureCell(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) TextureSize(Args &&...args) const {
			return Render.TextureSize(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) TextureAnimationSignature(Args &&...args) const {
			return Render.TextureAnimationSignature(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) PostProcessShaderName(Args &&...args) const {
			return Render.PostProcessShaderName(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) LensShaderHash(Args &&...args) const {
			return Render.LensShaderHash(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) SetAnimationTime(Args &&...args) {
			return Render.SetAnimationTime(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) RenderViews(Args &&...args) {
			return Render.Render(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) SunDirection(Args &&...args) const {
			return Render.SunDirection(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) SunAmbient(Args &&...args) const {
			return Render.SunAmbient(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) SunColor(Args &&...args) const {
			return Render.SunColor(std::forward<Args>(args)...);
		}
		template <typename... Args> decltype(auto) SetSun(Args &&...args) {
			return Render.SetSun(std::forward<Args>(args)...);
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
#if ENGINE_ASSERTS_ENABLED
			const auto observer = test_support::PortalTerminalObserverForTests;
			if (observer.Record)
				observer.Record(
					observer.Context, {&Render, test_support::PortalTerminalKind::CancelComposition, job}
				);
#endif
			Render.CancelPortalCaptureTreeComposition(job);
		}
		void CancelPreparation(uint64_t preparation) {
#if ENGINE_ASSERTS_ENABLED
			const auto observer = test_support::PortalTerminalObserverForTests;
			if (observer.Record)
				observer.Record(
					observer.Context,
					{&Render, test_support::PortalTerminalKind::CancelPreparation, preparation}
				);
#endif
			Render.CancelPortalCaptureTreePreparation(preparation);
		}
		void ReleaseImage(uint64_t image) {
#if ENGINE_ASSERTS_ENABLED
			const bool released = Render.DropPortalImage(image);
			const auto observer = test_support::PortalTerminalObserverForTests;
			if (observer.Record)
				observer.Record(
					observer.Context,
					{&Render, test_support::PortalTerminalKind::ReleaseImage, image, released}
				);
#else
			Render.DropPortalImage(image);
#endif
		}

	  private:
		Renderer &Render;
	};
}

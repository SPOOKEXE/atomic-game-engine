#pragma once
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/render/Renderer.hpp>
namespace client::detail {
	// Stack-owned composition keeps Lua/file capability ownership with its caller.
	class ComposerProvider final : public engine::imagegraph::HostNodeProvider {
	  public:
		ComposerProvider(
			engine::render::Renderer &renderer,
			engine::core::Name owner,
			engine::imagegraph::HostNodeProvider *fallback,
			engine::core::Name captureNamespace = {},
			std::vector<engine::core::Name> *captures = nullptr,
			engine::imagegraph::PendingHostObservations *observations = nullptr
		)
			: Render(renderer), Owner(owner), Fallback(fallback), CaptureNamespace(captureNamespace),
			  Captures(captures), Observations(observations) {}
		bool Pending = false;
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &,
			engine::imagegraph::HostNodeCapture &,
			std::string &
		) override;
		bool PcxMessages(
			std::string_view, std::span<const engine::imagegraph::PcxMessage>, std::string &
		) override;

	  private:
		bool CaptureDevice(
			const engine::imagegraph::HostNodeInvocation &,
			engine::imagegraph::HostNodeCapture &,
			std::string &
		);
		engine::render::Renderer &Render;
		engine::core::Name Owner;
		engine::imagegraph::HostNodeProvider *Fallback;
		engine::core::Name CaptureNamespace;
		std::vector<engine::core::Name> *Captures = nullptr;
		engine::imagegraph::PendingHostObservations *Observations = nullptr;
	};
	bool BuildComposerRequest(
		const engine::imagegraph::Document &,
		const engine::imagegraph::Plan &,
		const engine::imagegraph::Node &,
		engine::render::Renderer &,
		engine::core::Name owner,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::hlsl::SurfaceRequest &,
		engine::imagegraph::Diagnostic &,
		engine::imagegraph::HostNodeProvider * = nullptr,
		engine::imagegraph::CapturedFeedbackHost * = nullptr,
		uint64_t authoringRevision = 1
	);
	engine::core::Name ComposerAsset(const engine::imagegraph::Node &);
}

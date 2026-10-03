#include "ImageGraphComposerAdapter.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
namespace client::detail {
	bool ComposerProvider::Capture(
		const engine::imagegraph::HostNodeInvocation &invocation,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	) {
		if (invocation.Authored.Type == "pc.hlsl")
			return Render.CaptureComposerSurface(invocation, Owner, output, failure);
		if (Fallback) return Fallback->Capture(invocation, output, failure);
		failure = "Client has no provider for this host node";
		return false;
	}
	bool ComposerProvider::PcxMessages(
		std::string_view node, std::span<const engine::imagegraph::PcxMessage> messages, std::string &failure
	) {
		if (Fallback) return Fallback->PcxMessages(node, messages, failure);
		failure = "Client has no PCX message provider";
		return false;
	}

	engine::core::Name ComposerAsset(const engine::imagegraph::Node &node) {
		for (const auto &property : node.SourceProperties)
			if (property.Port == engine::render::hlsl::COOKED_SELECTOR) {
				const auto *name = std::get_if<std::string>(&property.Data);
				if (name && !name->empty() && name->size() <= engine::assets::Shader::MAXIMUM_NAME)
					return engine::core::Name(*name);
			}
		return {};
	}
	bool BuildComposerRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		engine::render::Renderer &renderer,
		engine::core::Name resourceOwner,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::hlsl::SurfaceRequest &output,
		engine::imagegraph::Diagnostic &diagnostic,
		engine::imagegraph::HostNodeProvider *provider,
		engine::imagegraph::CapturedFeedbackHost *replay,
		uint64_t revision
	) {
		ENGINE_PROFILE("imagegraph cooked composer request");
		using namespace engine;
		diagnostic = {};
		imagegraph::CapturedFeedbackHost localOwner;
		auto &owner = replay ? *replay : localOwner;
		imagegraph::EvaluationSnapshot localSnapshot;
		imagegraphphysics::RigidProvider rigid;
		ComposerProvider composer(renderer, resourceOwner, provider);
		imagegraph::EvaluationRequest clock{.Tick = tick, .Seed = seed, .HostProvider = &composer};
		clock.RigidProvider = &rigid;
		clock.RigidPlaying = true;
		clock.RigidFrameProgress = true;
		if (!owner.PrepareNodeInputs(
				document,
				plan,
				revision,
				seed,
				node.Id,
				clock,
				diagnostic,
				render::hlsl::MAXIMUM_SURFACE_JOB_BYTES
			))
			return false;
		if (!owner.Active() && imagegraph::EvaluateNodeInputs(
								   document,
								   plan,
								   node.Id,
								   clock,
								   localSnapshot,
								   diagnostic,
								   render::hlsl::MAXIMUM_SURFACE_JOB_BYTES
							   ) != imagegraph::Status::Ok)
			return false;
		const auto &snapshot = owner.Active() ? owner.Snapshot() : localSnapshot;
		if (const auto failure =
				renderer.BuildComposerSurface(node, snapshot, resourceOwner, displayColorSpace, output)) {
			diagnostic = {imagegraph::Status::UnsupportedExecution, node.Id, "surface", *failure};
			return false;
		}
		return true;
	}
}

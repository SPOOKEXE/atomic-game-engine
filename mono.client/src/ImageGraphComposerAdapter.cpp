#include "ImageGraphComposerAdapter.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
namespace client::detail {
	bool ComposerProvider::Capture(
		const engine::imagegraph::HostNodeInvocation &invocation,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	) {
		engine::imagegraph::HostNodeInvocation rendererInvocation = invocation;
		if (Observations && ((invocation.Authored.Type == "pc.3_d_transform_image" ||
							  invocation.Authored.Type == "image.transform_3d") ||
							 invocation.Authored.Type == "pc.hlsl")) {
			const uint64_t bytes =
				sizeof(*Observations) + Observations->Bytes +
				Observations->Captures.capacity() * sizeof(engine::imagegraph::HostNodeCapture);
			if (bytes >= rendererInvocation.MaximumOperationBytes) {
				failure = "Pending host observations leave no renderer capture budget";
				return false;
			}
			rendererInvocation.MaximumOperationBytes -= bytes;
		}
		if ((invocation.Authored.Type == "pc.3_d_transform_image" ||
			 invocation.Authored.Type == "image.transform_3d")) {
			if (!CaptureNamespace.IsValid() && Fallback)
				return Fallback->Capture(invocation, output, failure);
			if (!CaptureNamespace.IsValid()) {
				failure = "Transform host requires a live binding capture namespace";
				return false;
			}
			const engine::core::Name name(
				std::string(CaptureNamespace.Text()) + "/" + invocation.Authored.Id
			);
			if (Captures && std::find(Captures->begin(), Captures->end(), name) == Captures->end()) {
				if (Captures->size() >= 16) {
					failure = "Transform binding exceeds capture limit";
					return false;
				}
				Captures->push_back(name);
			}
			bool pending = false;
			const bool captured = Render.CaptureTransformImage3DAsync(
				rendererInvocation, Owner, name, output, failure, &pending
			);
			Pending = Pending || pending;
			return captured;
		}
		if (invocation.Authored.Type == "pc.hlsl")
			return Render.CaptureComposerSurface(rendererInvocation, Owner, output, failure);
		if (Fallback && Observations && invocation.Authored.Type.starts_with("pc.lua_"))
			return Observations->Capture(invocation, *Fallback, output, failure);
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

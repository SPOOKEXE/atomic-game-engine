#include "ImageGraphComposerAdapter.hpp"

#include "ImageGraphFontInputs.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
namespace client::detail {
	bool ComposerProvider::Capture(
		const engine::imagegraph::HostNodeInvocation &invocation,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	) {
		const bool raster = invocation.Authored.Type == "pc.3_d_camera" ||
							invocation.Authored.Type == "pc.3_d_camera_set" ||
							invocation.Authored.Type == "pc.3_d_transform_image" ||
							invocation.Authored.Type == "image.transform_3d" ||
							engine::render::imagegraph::IsSourceSdfRenderNode(invocation.Authored.Type);
		if (raster && Observations && CaptureNamespace.IsValid()) {
			struct DeviceProvider final : engine::imagegraph::HostNodeProvider {
				ComposerProvider &Host;
				explicit DeviceProvider(ComposerProvider &host) : Host(host) {}
				bool Capture(
					const engine::imagegraph::HostNodeInvocation &input,
					engine::imagegraph::HostNodeCapture &result,
					std::string &error
				) override {
					return Host.CaptureDevice(input, result, error);
				}
			} device(*this);
			// Earlier processor rows replay while the device retains the current pending row.
			return Observations->CaptureSequenced(invocation, device, output, failure);
		}
		return CaptureDevice(invocation, output, failure);
	}
	bool ComposerProvider::CaptureDevice(
		const engine::imagegraph::HostNodeInvocation &invocation,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	) {
		const bool sdf = engine::render::imagegraph::IsSourceSdfRenderNode(invocation.Authored.Type);
		const bool camera =
			invocation.Authored.Type == "pc.3_d_camera" || invocation.Authored.Type == "pc.3_d_camera_set";
		engine::imagegraph::HostNodeInvocation rendererInvocation = invocation;
		if (Observations && invocation.Authored.Type == "pc.hlsl") {
			const auto held = Observations->RetainedPayloadBytes();
			if (!held) {
				failure = "Pending host observation residency is invalid";
				return false;
			}
			const uint64_t bytes = *held;
			if (bytes >= rendererInvocation.MaximumOperationBytes) {
				failure = "Pending host observations leave no renderer capture budget";
				return false;
			}
			rendererInvocation.MaximumOperationBytes -= bytes;
		}
		if (sdf || camera ||
			(invocation.Authored.Type == "pc.3_d_transform_image" ||
			 invocation.Authored.Type == "image.transform_3d")) {
			if (sdf && !CaptureNamespace.IsValid())
				return Render.CaptureSourceSdf(rendererInvocation, Owner, output, failure);
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
			const bool captured =
				sdf ? Render.CaptureSourceSdfAsync(rendererInvocation, Owner, name, output, failure, &pending)
				: camera ? Render.CaptureSourceCamera3DAsync(
							   rendererInvocation, Owner, name, output, failure, &pending
						   )
						 : Render.CaptureTransformImage3DAsync(
							   rendererInvocation, Owner, name, output, failure, &pending
						   );
			Pending = Pending || pending;
			return captured;
		}
		if (invocation.Authored.Type == "pc.hlsl")
			return Render.CaptureComposerSurface(rendererInvocation, Owner, output, failure);
		if (Fallback && Observations && invocation.Authored.Type.starts_with("pc.lua_"))
			return Observations->CaptureSequenced(invocation, *Fallback, output, failure);
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
		uint64_t revision,
		const engine::imagegraphfont::GraphFontInputs *fonts,
		const engine::imagegraph::EvaluationRequest *fontInputs
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
		imagegraph::SourceFontContext heldFontContext;
		if (!BindFontInputs(
				fonts, fontInputs, heldFontContext, clock, diagnostic, render::hlsl::MAXIMUM_SURFACE_JOB_BYTES
			))
			return false;
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

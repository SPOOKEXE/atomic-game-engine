#include "ImageGraphCameraAdapter.hpp"

#include "ImageGraphFontInputs.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCamera3D.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

namespace client::detail {
	namespace {
		using namespace engine::imagegraph;
		constexpr uint64_t CAMERA_HOST_BYTES = 64ull * 1024 * 1024;

	}

	bool BuildCameraRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		std::string_view outputPort,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::imagegraph::SourceCamera3DRequest &outputRequest,
		engine::imagegraph::Diagnostic &diagnostic,
		engine::imagegraph::HostNodeProvider *hostProvider,
		engine::imagegraph::CapturedFeedbackHost *replayOwner,
		uint64_t authoringRevision,
		const engine::imagegraphfont::GraphFontInputs *fonts,
		const engine::imagegraph::EvaluationRequest *fontInputs
	) {
		ENGINE_PROFILE("imagegraph source camera request");
		using namespace engine;
		diagnostic = {};
		imagegraph::EvaluationSnapshot localSnapshot;
		imagegraph::CapturedFeedbackHost localReplay;
		auto &owner = replayOwner ? *replayOwner : localReplay;
		imagegraphphysics::RigidProvider rigid;
		imagegraph::EvaluationRequest clock{.Tick = tick, .Seed = seed, .HostProvider = hostProvider};
		clock.RigidProvider = &rigid;
		// Native client snapshots sample played frames, including fixed seeks.
		clock.RigidPlaying = true;
		clock.SourceCachePlayback = imagegraph::SourceCachePlaybackObservation{
			true, imagegraph::SourceCacheSampling::NativePlayedPrefix, true
		};
		clock.RigidFrameProgress = true;
		imagegraph::SourceFontContext heldFontContext;
		if (!BindFontInputs(fonts, fontInputs, heldFontContext, clock, diagnostic, CAMERA_HOST_BYTES))
			return false;
		if (!owner.PrepareNodeInputs(
				document, plan, authoringRevision, seed, node.Id, clock, diagnostic, CAMERA_HOST_BYTES
			))
			return false;
		if (!owner.Active() &&
			imagegraph::EvaluateNodeInputs(
				document, plan, node.Id, clock, localSnapshot, diagnostic, CAMERA_HOST_BYTES
			) != imagegraph::Status::Ok)
			return false;
		const auto &snapshot = owner.Active() ? owner.Snapshot() : localSnapshot;
		imagegraph::SourceCameraEvaluationPolicy policy;
		policy.InheritedSurfaceFormat = snapshot.InheritedSurfaceFormat();
		if (document.Project) {
			policy.ProjectWidth = document.Project->SurfaceWidth;
			policy.ProjectHeight = document.Project->SurfaceHeight;
			policy.ProjectColorDepth = document.Project->ColorDepth;
			policy.ProjectShader3D = document.Project->Shader3D;
		}
		for (const auto &value : snapshot.Values())
			if (value.Port == "dimension") policy.DimensionLinked = value.Linked;
		if (!render::imagegraph::BuildSourceCamera3DRequest(
				node,
				snapshot,
				policy,
				outputPort,
				displayColorSpace,
				outputRequest,
				diagnostic,
				CAMERA_HOST_BYTES
			)) {
			return false;
		}
		return true;
	}
}

#include "../NodeExecutors.hpp"

#include <engine/imagegraph/SliceStackReplay.hpp>
namespace engine::imagegraph::detail {
	bool SourceMeshSliceStack(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.slice_stack");
		ImageArray output;
		if (const auto *state = context.Request.SliceStackReplay) {
			Diagnostic diagnostic;
			if (ValidateSliceStackReplay(*state, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, "outputs");
			const auto found =
				std::find_if(state->Entries.begin(), state->Entries.end(), [&](const auto &entry) {
					return entry.NodeId == context.Authored.Id;
				});
			if (found != state->Entries.end()) {
				uint64_t bytes = found->Images.size() * (sizeof(Image) + sizeof(ImageArrayItem));
				for (const auto &image : found->Images)
					bytes = MeshAddBytes(bytes, image.Pixels.size());
				if (!context.ReserveOutput(bytes + 64, "outputs")) return false;
				output.Images = found->Images;
				output.Items.reserve(output.Images.size());
				for (size_t i = 0; i < output.Images.size(); ++i)
					output.Items.push_back({i});
			}
		}
		if (!context.ReserveOutput(64, "outputs")) return false;
		context.OutputImageArrays.emplace_back("outputs", std::move(output));
		return context.FailureCode == Status::Ok;
	}
}

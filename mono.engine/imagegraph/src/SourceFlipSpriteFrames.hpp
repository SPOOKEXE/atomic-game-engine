#pragma once
#include "NodeExecutors.hpp"
namespace engine::imagegraph::detail {
	struct SourceFlipSpriteFrames {
		const Image *Single = nullptr;
		const ImageArray *Sequence = nullptr;
		size_t Count = 0;
		const Image *At(size_t index) const {
			if (Single) return Single;
			if (!Sequence || !Count) return nullptr;
			index %= Count;
			if (Sequence->Items.empty()) return &Sequence->Images[index];
			const auto *slot = std::get_if<size_t>(&Sequence->Items[index].Data);
			return slot && *slot < Sequence->Images.size() ? &Sequence->Images[*slot] : nullptr;
		}
	};
	inline bool ResolveSourceFlipSpriteFrames(NodeContext &context, SourceFlipSpriteFrames &frames) {
		frames = {};
		frames.Single = context.Input("fluid_particle");
		if (frames.Single) {
			frames.Count = 1;
			return true;
		}
		for (const auto &[port, array] : context.ImageArrays)
			if (port == "fluid_particle") frames.Sequence = array;
		if (!frames.Sequence) return true;
		frames.Count =
			frames.Sequence->Items.empty() ? frames.Sequence->Images.size() : frames.Sequence->Items.size();
		if (frames.Count > Limits::MaximumArrayElements)
			return context.Fail(
				Status::LimitExceeded, "FLIP sprite sequence exceeds bounded frames", "fluid_particle"
			);
		if (!frames.Count) return true;
		// A source array whose first entry is not a surface chooses droplet rendering.
		if (!frames.At(0)) {
			frames.Count = 0;
			return true;
		}
		for (size_t index = 0; index < frames.Count; ++index)
			if (!frames.At(index))
				return context.Fail(
					Status::UnsupportedExecution,
					"FLIP sprite sequence contains an undefined surface draw",
					"fluid_particle"
				);
		return true;
	}
}

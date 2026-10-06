#include "GroupReplayInternal.hpp"
#include "PixelBuilderPayload.hpp"

#include <new>

namespace engine::imagegraph::detail {
	uint64_t PixelBuilderGroupCloneBytes(const GroupReplayState &source) {
		return std::max(source.RetainedBytes(), uint64_t(sizeof(GroupReplayAccess::Owner)));
	}
	Status OverridePixelBuilderGroups(
		const GroupReplayState &source,
		std::string_view nodeId,
		PixelBuilderGroupState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		auto owner = CloneGroupReplay(source, 0, source.AuthoringRevision(), maximumBytes, 0, diagnostic);
		if (!owner) return diagnostic.Code;
		std::erase_if(owner->Bindings, [&](const auto &binding) {
			return binding.NodeId == nodeId &&
				   (binding.Port == "dimension" || binding.Port == "dimension_unit");
		});
		std::erase_if(owner->SharedSubtypes, [&](const auto &overlay) {
			return overlay.NodeId == nodeId &&
				   (overlay.Port == "dimension" || overlay.Port == "dimension_unit");
		});
		GroupReplayAccess::Install(result.Replay, std::move(owner));
		return Status::Ok;
	}
	Status FreezePixelBuilderGroups(
		const GroupReplayState &source,
		PixelBuilderGroupState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		auto owner = CloneGroupReplay(source, 0, source.AuthoringRevision(), maximumBytes, 0, diagnostic);
		if (!owner) return diagnostic.Code;
		GroupReplayAccess::Install(result.Replay, std::move(owner));
		return Status::Ok;
	}
}
namespace engine::imagegraph {
	PixelBuilderGroupState::PixelBuilderGroupState(const PixelBuilderGroupState &other) {
		Diagnostic diagnostic;
		if (detail::FreezePixelBuilderGroups(
				other.Replay, *this, diagnostic, Limits::MaximumEvaluationBytes
			) != Status::Ok)
			throw std::bad_alloc{};
	}
	PixelBuilderGroupState &PixelBuilderGroupState::operator=(const PixelBuilderGroupState &other) {
		if (this != &other) {
			PixelBuilderGroupState copy(other);
			Replay = std::move(copy.Replay);
		}
		return *this;
	}
	bool PixelBuilderGroupState::operator==(const PixelBuilderGroupState &other) const {
		const auto equal = [](auto left, auto right, auto compare) {
			return left.size() == right.size() &&
				   std::equal(left.begin(), left.end(), right.begin(), compare);
		};
		return Replay.AuthoringRevision() == other.Replay.AuthoringRevision() &&
			   Replay.InstancesBound() == other.Replay.InstancesBound() &&
			   equal(
				   Replay.DetachedAnimators(),
				   other.Replay.DetachedAnimators(),
				   [](const auto &a, const auto &b) { return a == b; }
			   ) &&
			   equal(
				   Replay.Entries(),
				   other.Replay.Entries(),
				   [](const auto &a, const auto &b) {
					   return a.NodeId == b.NodeId && a.InputType == b.InputType && a.Subtype == b.Subtype &&
							  a.VectorSize == b.VectorSize && a.Domain == b.Domain &&
							  a.ParentReset == b.ParentReset && a.SubtypeStatic == b.SubtypeStatic &&
							  a.SubtypeKeys == b.SubtypeKeys && a.ParentKeys == b.ParentKeys;
				   }
			   ) &&
			   equal(
				   Replay.Bindings(),
				   other.Replay.Bindings(),
				   [](const auto &a, const auto &b) {
					   return a.NodeId == b.NodeId && a.OwnerId == b.OwnerId && a.Getter == b.Getter &&
							  a.Writer == b.Writer && a.Port == b.Port && a.AnimatorPort == b.AnimatorPort &&
							  a.Axes == b.Axes;
				   }
			   ) &&
			   equal(
				   Replay.SharedSubtypes(), other.Replay.SharedSubtypes(), [](const auto &a, const auto &b) {
					   return a.NodeId == b.NodeId && a.Fixed == b.Fixed && a.Keys == b.Keys &&
							  a.Port == b.Port && a.SeparatedVec2 == b.SeparatedVec2;
				   }
			   );
	}
}

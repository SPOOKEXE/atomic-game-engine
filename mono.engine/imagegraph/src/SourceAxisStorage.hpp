#pragma once

#include "SourceAnimatorIdentity.hpp"

namespace engine::imagegraph::detail {
	struct SourceAxisStorageView {
		const Node *Owner = nullptr;
		std::string_view Port;
		const SourceSeparatedVec2Animator *Axes = nullptr;
		const AnimationTrack *Track = nullptr;
		const DetachedSourceAnimator *Detached = nullptr;
		bool WriterAnimated = false;
		bool Separated = false;
		Status Code = Status::Ok;
		std::string_view Message;
	};
	inline bool AdmitSourceAxisWork(uint64_t &work, uint64_t visits) {
		if (work > 64'000'000 || visits > 64'000'000 - work) return false;
		work += visits;
		return true;
	}
	// reads pass the delegated getter; edits pass the selected local property.
	inline SourceAxisStorageView ResolveLocalSourceAxes(
		const Document &document,
		const Node &property,
		std::string_view port,
		std::span<const GroupSubtypeBinding> bindings,
		std::span<const GroupSubtypeOverlay> overlays,
		std::span<const DetachedSourceAnimator> detached,
		std::string_view legacyOwner,
		std::string_view legacyPort,
		bool legacyAnimated,
		uint64_t &work,
		bool requireSeparated = true
	) {
		SourceAxisStorageView result;
		const auto fail = [&](Status code, std::string_view message) {
			result.Code = code;
			result.Message = message;
			return result;
		};
		if (!AdmitSourceAxisWork(
				work,
				overlays.size() + (property.SourceSeparatedVec2Animators
									   ? property.SourceSeparatedVec2Animators->Inputs.size()
									   : 0)
			))
			return fail(Status::LimitExceeded, "source axis flag lookup exceeds work bounds");
		const auto localOverlay = std::find_if(overlays.begin(), overlays.end(), [&](const auto &item) {
			return item.NodeId == property.Id && item.Port == port;
		});
		const auto *local = localOverlay != overlays.end() && localOverlay->SeparatedVec2
								? &*localOverlay->SeparatedVec2
								: FindSeparatedVec2(property, port);
		if (requireSeparated && (!local || !local->Separated)) return result;
		result.Separated = local && local->Separated;
		result.Owner = &property;
		result.Port = port;
		if (!AdmitSourceAxisWork(work, bindings.size()))
			return fail(Status::LimitExceeded, "source axis binding lookup exceeds work bounds");
		const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto &item) {
			return item.NodeId == property.Id && item.Port == port;
		});
		std::string_view ownerId = property.Id;
		result.Port = port;
		if (!AdmitSourceAxisWork(work, property.SourceAnimatedInputs.size()))
			return fail(Status::LimitExceeded, "source local axis mode lookup exceeds work bounds");
		result.WriterAnimated =
			property.Id == legacyOwner && port == legacyPort
				? legacyAnimated
				: std::find(
					  property.SourceAnimatedInputs.begin(), property.SourceAnimatedInputs.end(), port
				  ) != property.SourceAnimatedInputs.end();
		if (binding != bindings.end()) {
			if (binding->Axes.Storage == GroupAxisStorage::Uninitialized)
				return fail(
					Status::SourceAxisInitializationRequired,
					"source axis storage requires retained initialization"
				);
			if (binding->Axes.Storage != GroupAxisStorage::None) {
				ownerId = binding->Axes.OwnerId;
				result.Port = binding->Axes.Port;
				result.WriterAnimated = binding->Axes.Writer == GroupSubtypeAnimator::Animated;
			} else {
				ownerId = binding->OwnerId;
				result.Port = BindingAnimatorPort(*binding);
				result.WriterAnimated = binding->Writer == GroupSubtypeAnimator::Animated;
			}
		}
		if (!AdmitSourceAxisWork(
				work, document.Nodes.size() + overlays.size() + detached.size() + document.Tracks.size()
			))
			return fail(Status::LimitExceeded, "source axis storage lookup exceeds work bounds");
		const auto owner = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
			return item.Id == ownerId;
		});
		if (owner == document.Nodes.end())
			return fail(Status::UnknownNode, "source scalar axis owner is absent");
		result.Owner = &*owner;
		const auto retained = std::find_if(detached.begin(), detached.end(), [&](const auto &item) {
			return item.OwnerId == ownerId && item.Id == result.Port;
		});
		if (retained != detached.end()) {
			result.Detached = &*retained;
			if (retained->Type != ValueType::Vector2)
				return fail(Status::TypeMismatch, "retained source axes need a two-axis property");
			result.WriterAnimated = retained->Writer == GroupSubtypeAnimator::Animated;
			if (retained->Track) result.Track = &*retained->Track;
		}
		const auto overlay = std::find_if(overlays.begin(), overlays.end(), [&](const auto &item) {
			return item.NodeId == ownerId && item.Port == result.Port;
		});
		if (overlay != overlays.end() && overlay->SeparatedVec2)
			result.Axes = &*overlay->SeparatedVec2;
		else {
			if (!AdmitSourceAxisWork(
					work,
					owner->SourceSeparatedVec2Animators ? owner->SourceSeparatedVec2Animators->Inputs.size()
														: 0
				))
				return fail(Status::LimitExceeded, "source authored axis lookup exceeds work bounds");
			result.Axes = FindSeparatedVec2(*owner, result.Port);
		}
		if (!result.Axes) return fail(Status::UnsupportedExecution, "source scalar axis storage is absent");
		if (!result.Axes->Initialized) {
			if (binding != bindings.end())
				return fail(Status::UnsupportedExecution, "captured warm scalar storage is uninitialized");
			return fail(
				Status::SourceAxisInitializationRequired,
				"source scalar axis storage requires retained initialization"
			);
		}
		if (!result.Track)
			for (const auto &track : document.Tracks)
				if (track.NodeId == ownerId && track.Port == result.Port) {
					result.Track = &track;
					break;
				}
		return result;
	}
	inline SourceAxisStorageView ResolveSourceGetterAxes(
		const Document &document,
		const Node &node,
		std::string_view port,
		const GroupReplayState *replay,
		std::string_view legacyOwner,
		std::string_view legacyPort,
		bool legacyAnimated,
		uint64_t &work
	) {
		bool workRefused = false;
		const auto *selected = SourcePropertyGetterNode(document, node, port, &work, &workRefused);
		if (!selected) {
			SourceAxisStorageView result;
			result.Code = workRefused ? Status::LimitExceeded : Status::InvalidGroup;
			result.Message = workRefused ? "source axis getter lookup exceeds work bounds"
										 : "source axis getter property is absent";
			return result;
		}
		return ResolveLocalSourceAxes(
			document,
			*selected,
			port,
			replay && replay->InstancesBound() ? replay->Bindings() : std::span<const GroupSubtypeBinding>{},
			replay ? replay->SharedSubtypes() : std::span<const GroupSubtypeOverlay>{},
			replay ? replay->DetachedAnimators() : std::span<const DetachedSourceAnimator>{},
			legacyOwner,
			legacyPort,
			legacyAnimated,
			work
		);
	}
}

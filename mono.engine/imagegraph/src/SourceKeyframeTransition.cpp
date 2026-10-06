#include "EvaluationAllocator.hpp"
#include "SourceAnimatorPersistence.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceKeyframeTransition.hpp>

#include <algorithm>
#include <tuple>

namespace engine::imagegraph {
	namespace {
		struct KeyEditWorkExceeded {};
		struct KeyView {
			const Keyframe *Data = nullptr;
			const Keyframe *Identity = nullptr;
			bool Copy = false;
		};
		struct WriterEdit {
			std::string_view Node, Port;
			size_t Detached = SIZE_MAX;
			detail::EvaluationVector<KeyView> Keys;
			explicit WriterEdit(detail::EvaluationBudget &budget)
				: Keys(detail::EvaluationAllocator<KeyView>(budget)) {}
		};
		struct ResolvedEdit {
			size_t Writer = 0;
			const Keyframe *Original = nullptr;
			const SourceKeyframeEdit *Edit = nullptr;
			bool Duplicate = false;
		};
		auto KeyPayload(const Keyframe &key) {
			return std::tie(
				key.Tick,
				key.Data,
				key.Interpolation,
				key.Ease,
				key.SineDriver,
				key.SourceDriver,
				key.Subframe,
				key.NegativeFrame,
				key.Kind
			);
		}
	}
	Status ApplySourceKeyframeEdits(
		const Document &document,
		std::span<const SourceKeyframeEdit> edits,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_keyframe_transition");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || edits.empty() ||
			edits.size() > Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "source key edit count or byte bound is invalid");
		const auto oldBytes = DocumentRetainedPayloadBytes(document);
		const auto resultBytes =
			&document == &result ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		uint64_t borrowed = edits.size() * sizeof(SourceKeyframeEdit);
		if (oldBytes && !detail::SourceAnimatorAdd(borrowed, *oldBytes))
			return fail(Status::LimitExceeded, "source key borrowed records overflow");
		if (!oldBytes || !resultBytes || !detail::SourceAnimatorAdd(borrowed, *resultBytes))
			return fail(Status::LimitExceeded, "source key document payload exceeds bounds");
		for (const auto &edit : edits) {
			if (!edit.Original || (edit.Copy && !edit.Replacement))
				return fail(Status::InvalidValue, "source key edit has no original or copy destination");
			for (const auto *key : {edit.Original, edit.Replacement}) {
				if (!key) continue;
				uint64_t bytes = sizeof(Keyframe);
				size_t count = 0;
				if (!detail::SourceAnimatorKey(bytes, *key, false, count) ||
					!detail::SourceAnimatorAdd(borrowed, bytes))
					return fail(Status::LimitExceeded, "source key edit payload exceeds bounds");
			}
			if (edit.Replacement && (edit.Replacement->NodeId != edit.Original->NodeId ||
									 edit.Replacement->Port != edit.Original->Port))
				return fail(Status::InvalidValue, "source key replacement changes its logical socket");
		}
		detail::EvaluationBudget budget(maximumBytes);
		auto held = budget.Reserve(borrowed);
		if (!held) return fail(Status::LimitExceeded, "source key borrowed overlap exceeds bounds");
		if (detail::ValidateSourceAnimatorState(document, diagnostic) != Status::Ok) return diagnostic.Code;
		uint64_t work = 0;
		const auto admit = [&](uint64_t amount) {
			if (amount > 64'000'000 - work) throw KeyEditWorkExceeded{};
			work += amount;
		};
		const auto same = [&](std::string_view a, std::string_view b) {
			admit(1 + std::min(a.size(), b.size()));
			return a == b;
		};
		const auto equivalent = [&](const Keyframe &a, const Keyframe &b) {
			const auto aBytes = KeyframePayloadBytes(a), bBytes = KeyframePayloadBytes(b);
			if (!aBytes || !bBytes) return false;
			admit(*aBytes + *bBytes);
			return KeyPayload(a) == KeyPayload(b);
		};
		detail::EvaluationVector<WriterEdit> writers{detail::EvaluationAllocator<WriterEdit>(budget)};
		detail::EvaluationVector<ResolvedEdit> resolved{detail::EvaluationAllocator<ResolvedEdit>(budget)};
		for (const auto &edit : edits) {
			const auto &original = *edit.Original;
			if (!edit.Copy) {
				bool pinned = false;
				for (const auto &key : document.Keyframes)
					if (same(key.NodeId, original.NodeId) && same(key.Port, original.Port) &&
						equivalent(key, original) && same(key.SourceKeyId, original.SourceKeyId))
						pinned = true;
				if (!pinned) return fail(Status::InvalidValue, "source key changed during its gesture");
			}
			std::string_view owner = original.NodeId, port = original.Port;
			if (document.SourceAnimators)
				for (const auto &binding : document.SourceAnimators->Bindings)
					if (same(binding.NodeId, owner) && same(binding.Port, port)) {
						owner = binding.OwnerId;
						port = binding.AnimatorPort.empty() ? binding.Port : binding.AnimatorPort;
						break;
					}
			size_t writer = 0;
			for (; writer < writers.size(); ++writer)
				if (same(writers[writer].Node, owner) && same(writers[writer].Port, port)) break;
			if (writer == writers.size()) {
				WriterEdit next(budget);
				next.Node = owner;
				next.Port = port;
				const std::vector<Keyframe> *keys = &document.Keyframes;
				if (document.SourceAnimators)
					for (size_t index = 0; index < document.SourceAnimators->DetachedValues.size(); ++index) {
						const auto &record = document.SourceAnimators->DetachedValues[index];
						if (same(record.NodeId, owner) && same(record.Port, port)) {
							next.Detached = index;
							keys = &record.Keys;
							break;
						}
					}
				for (const auto &key : *keys)
					if (same(key.NodeId, owner) && same(key.Port, port))
						next.Keys.push_back({&key, &key, false});
				writers.push_back(std::move(next));
			}
			const Keyframe *physical = nullptr;
			if (!edit.Copy) {
				for (const auto &key : writers[writer].Keys)
					if (equivalent(*key.Data, original)) physical = key.Data;
				if (!physical)
					return fail(Status::InvalidValue, "logical key disagrees with captured writer");
			}
			bool duplicate = false;
			for (const auto &prior : resolved) {
				admit(1);
				if (prior.Writer != writer || edit.Copy || prior.Edit->Copy || prior.Original != physical)
					continue;
				const auto *a = prior.Edit->Replacement, *b = edit.Replacement;
				if (bool(a) != bool(b) || (a && !equivalent(*a, *b)))
					return fail(Status::InvalidValue, "shared key selections disagree on their edit");
				duplicate = true;
			}
			resolved.push_back({writer, physical, &edit, duplicate});
		}
		for (const auto &edit : resolved)
			if (!edit.Duplicate && !edit.Edit->Copy)
				std::erase_if(writers[edit.Writer].Keys, [&](const auto &key) {
					admit(1);
					return key.Data == edit.Original;
				});
		for (size_t index = 0; index < resolved.size(); ++index) {
			const auto &edit = resolved[index];
			if (edit.Duplicate || !edit.Edit->Replacement) continue;
			const auto *replacement = edit.Edit->Replacement;
			bool collision = false;
			if (!edit.Edit->Copy)
				for (size_t prior = 0; prior < index; ++prior) {
					admit(1);
					const auto &other = resolved[prior];
					if (!other.Duplicate && !other.Edit->Copy && other.Writer == edit.Writer &&
						other.Edit->Replacement &&
						GetFrameTime(*other.Edit->Replacement) == GetFrameTime(*replacement))
						collision = true;
				}
			if (collision) continue;
			auto &keys = writers[edit.Writer].Keys;
			std::erase_if(keys, [&](const auto &key) {
				admit(1);
				return GetFrameTime(*key.Data) == GetFrameTime(*replacement);
			});
			keys.push_back({replacement, edit.Edit->Copy ? nullptr : edit.Original, edit.Edit->Copy});
		}
		for (auto &writer : writers)
			std::sort(writer.Keys.begin(), writer.Keys.end(), [&](const auto &a, const auto &b) {
				admit(1);
				return CompareFrameTime(GetFrameTime(*a.Data), GetFrameTime(*b.Data)) < 0;
			});
		const auto writerFor = [&](std::string_view node, std::string_view port) -> const WriterEdit * {
			for (const auto &writer : writers)
				if (writer.Detached == SIZE_MAX && same(writer.Node, node) && same(writer.Port, port))
					return &writer;
			if (document.SourceAnimators)
				for (const auto &binding : document.SourceAnimators->Bindings)
					if (same(binding.NodeId, node) && same(binding.Port, port))
						for (const auto &writer : writers)
							if (same(binding.OwnerId, writer.Node) &&
								same(
									binding.AnimatorPort.empty() ? binding.Port : binding.AnimatorPort,
									writer.Port
								))
								return &writer;
			return nullptr;
		};
		// Count the final vector before cloning values. Alias fanout is part of this bound.
		detail::EvaluationVector<std::pair<std::string_view, std::string_view>> targets{
			detail::EvaluationAllocator<std::pair<std::string_view, std::string_view>>(budget)
		};
		for (const auto &writer : writers) {
			if (writer.Detached == SIZE_MAX) targets.emplace_back(writer.Node, writer.Port);
			if (document.SourceAnimators)
				for (const auto &binding : document.SourceAnimators->Bindings)
					if (same(binding.OwnerId, writer.Node) &&
						same(binding.AnimatorPort.empty() ? binding.Port : binding.AnimatorPort, writer.Port))
						targets.emplace_back(binding.NodeId, binding.Port);
		}
		uint64_t finalBytes = 0;
		size_t finalCount = 0;
		const auto chargeKey = [&](const KeyView &key, std::string_view node, std::string_view port) {
			const auto bytes = KeyframePayloadBytes(*key.Data);
			const uint64_t extra =
				node.size() + port.size() + (key.Identity ? key.Identity->SourceKeyId.size() : 0) + 64;
			if (!bytes || extra > UINT64_MAX / 2 || *bytes > UINT64_MAX / 2 - extra ||
				!detail::SourceAnimatorAdd(finalBytes, 2 * (*bytes + extra)))
				throw std::bad_alloc{};
		};
		for (const auto &key : document.Keyframes)
			if (!writerFor(key.NodeId, key.Port)) {
				++finalCount;
				chargeKey({&key, &key}, key.NodeId, key.Port);
			}
		for (const auto &[node, port] : targets) {
			const auto *writer = writerFor(node, port);
			if (writer->Keys.size() > Limits::MaximumKeyframes - finalCount)
				return fail(Status::LimitExceeded, "source key fanout exceeds key count bounds");
			finalCount += writer->Keys.size();
			for (const auto &key : writer->Keys)
				chargeKey(key, node, port);
		}
		for (const auto &writer : writers)
			if (writer.Detached != SIZE_MAX)
				for (const auto &key : writer.Keys)
					chargeKey(key, writer.Node, writer.Port);
		detail::EvaluationVector<std::pair<std::string_view, std::string_view>> newTracks{
			detail::EvaluationAllocator<std::pair<std::string_view, std::string_view>>(budget)
		};
		for (const auto &[node, port] : targets) {
			const auto *writer = writerFor(node, port);
			bool source = false, present = false;
			for (const auto &key : writer->Keys) {
				admit(1);
				if (same(key.Data->Interpolation, "source")) source = true;
			}
			for (const auto &track : document.Tracks)
				if (same(track.NodeId, node) && same(track.Port, port)) present = true;
			if (source && !present) {
				if (newTracks.size() >= Limits::MaximumTracks - document.Tracks.size())
					return fail(Status::LimitExceeded, "source key track fanout exceeds count bounds");
				newTracks.emplace_back(node, port);
				if (!detail::SourceAnimatorAdd(
						finalBytes, sizeof(AnimationTrack) + node.size() + port.size() + 64
					))
					throw std::bad_alloc{};
			}
		}
		if (!detail::SourceAnimatorAdd(
				finalBytes, (document.Tracks.size() + newTracks.size()) * sizeof(AnimationTrack)
			))
			throw std::bad_alloc{};
		if (*oldBytes > UINT64_MAX / 2 || !detail::SourceAnimatorAdd(finalBytes, 2 * *oldBytes))
			return fail(Status::LimitExceeded, "source key clone overlap overflows");
		auto owned = budget.Reserve(finalBytes);
		if (!owned) return fail(Status::LimitExceeded, "source key clone and fanout exceed byte bounds");
		Document candidate = document;
		candidate.Tracks.reserve(candidate.Tracks.size() + newTracks.size());
		for (const auto &[node, port] : newTracks)
			candidate.Tracks.push_back({std::string(node), std::string(port), "hold", -1});
		const auto clone = [](const KeyView &view, std::string_view node, std::string_view port, bool alias) {
			Keyframe key = *view.Data;
			key.NodeId = node;
			key.Port = port;
			key.SourceKeyId = alias || !view.Identity ? std::string{} : view.Identity->SourceKeyId;
			if (view.Copy) {
				key.SourceDriver.reset();
				key.SineDriver.reset();
			}
			return key;
		};
		std::vector<Keyframe> finalKeys;
		finalKeys.reserve(finalCount);
		for (const auto &key : document.Keyframes)
			if (!writerFor(key.NodeId, key.Port)) finalKeys.push_back(key);
		for (const auto &[node, port] : targets) {
			const auto *writer = writerFor(node, port);
			const bool alias = node != writer->Node || port != writer->Port;
			for (const auto &key : writer->Keys)
				finalKeys.push_back(clone(key, node, port, alias));
		}
		for (const auto &writer : writers)
			if (writer.Detached != SIZE_MAX) {
				auto &stored = candidate.SourceAnimators->DetachedValues[writer.Detached];
				std::vector<Keyframe> keys;
				keys.reserve(writer.Keys.size());
				for (const auto &key : writer.Keys)
					keys.push_back(clone(key, writer.Node, writer.Port, false));
				stored.Keys = std::move(keys);
				stored.Fixed.reset();
			}
		std::sort(finalKeys.begin(), finalKeys.end(), [&](const auto &a, const auto &b) {
			admit(1);
			const auto aSocket = std::tie(a.NodeId, a.Port), bSocket = std::tie(b.NodeId, b.Port);
			admit(a.NodeId.size() + a.Port.size() + b.NodeId.size() + b.Port.size());
			if (aSocket != bSocket) return aSocket < bSocket;
			return CompareFrameTime(GetFrameTime(a), GetFrameTime(b)) < 0;
		});
		candidate.Keyframes = std::move(finalKeys);
		if (detail::ValidateSourceAnimatorState(candidate, diagnostic) != Status::Ok) return diagnostic.Code;
		Plan plan;
		if (Compile(candidate, plan, diagnostic, budget.Available()) != Status::Ok) return diagnostic.Code;
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const KeyEditWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source key edit comparison work exceeds bounds"};
		return diagnostic.Code;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source key edit allocation exceeds bounds"};
		return diagnostic.Code;
	}
}

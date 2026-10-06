#include "EvaluationAllocator.hpp"
#include "SourceAnimatorPersistence.hpp"
#include "SourceAxisStorage.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceKeyframeTransition.hpp>

#include <algorithm>
#include <cmath>
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
			int8_t Axis = -1;
			detail::EvaluationVector<KeyView> Keys;
			explicit WriterEdit(detail::EvaluationBudget &budget)
				: Keys(detail::EvaluationAllocator<KeyView>(budget)) {}
		};
		struct AxisTarget {
			size_t Writer = 0, Node = SIZE_MAX, Input = SIZE_MAX, Overlay = SIZE_MAX;
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
		struct AxisLookupBound {
			uint64_t Records = 0, NameBytes = 0;
		};
		void AdmitKeyWork(uint64_t &work, uint64_t amount) {
			if (amount > 64'000'000 - work) throw KeyEditWorkExceeded{};
			work += amount;
		}
		AxisLookupBound BoundAxisLookup(const Document &document, uint64_t &work) {
			AxisLookupBound bound;
			const auto admit = [&](uint64_t amount) { AdmitKeyWork(work, amount); };

			bound.Records = document.Nodes.size() + document.Tracks.size();
			const auto name = [&](std::string_view text) {
				admit(1);
				bound.NameBytes = std::max(bound.NameBytes, uint64_t(text.size()));
			};
			for (const auto &node : document.Nodes) {
				name(node.Id);
				bound.Records += node.SourceAnimatedInputs.size();
				for (const auto &port : node.SourceAnimatedInputs)
					name(port);
				if (node.SourceSeparatedVec2Animators) {
					bound.Records += 2 * node.SourceSeparatedVec2Animators->Inputs.size();
					for (const auto &axes : node.SourceSeparatedVec2Animators->Inputs)
						name(axes.Port);
				}
			}
			for (const auto &track : document.Tracks) {
				name(track.NodeId);
				name(track.Port);
			}
			if (document.SourceAnimators) {
				const auto &state = *document.SourceAnimators;
				bound.Records +=
					state.Bindings.size() + 2 * state.DetachedValues.size() + state.Detached.size();
				for (const auto &binding : state.Bindings) {
					name(binding.NodeId);
					name(binding.OwnerId);
					name(binding.Port);
					name(binding.AnimatorPort);
					name(binding.Axes.OwnerId);
					name(binding.Axes.Port);
				}
				for (const auto &overlay : state.DetachedValues) {
					name(overlay.NodeId);
					name(overlay.Port);
				}
				for (const auto &retired : state.Detached) {
					name(retired.OwnerId);
					name(retired.Id);
				}
			}
			return bound;
		}
		detail::SourceAxisStorageView ReadKeyAxes(
			const Document &document,
			const Node &node,
			std::string_view port,
			const AxisLookupBound &bound,
			uint64_t &work
		) {
			const uint64_t nameBytes =
				2 * (1 + std::max({bound.NameBytes, uint64_t(node.Id.size()), uint64_t(port.size())}));
			if (bound.Records > (64'000'000 - work) / nameBytes) throw KeyEditWorkExceeded{};
			AdmitKeyWork(work, bound.Records * nameBytes);
			uint64_t visits = 0;
			return detail::ResolveLocalSourceAxes(
				document,
				node,
				port,
				document.SourceAnimators
					? std::span<const GroupSubtypeBinding>{document.SourceAnimators->Bindings}
					: std::span<const GroupSubtypeBinding>{},
				document.SourceAnimators
					? std::span<const GroupSubtypeOverlay>{document.SourceAnimators->DetachedValues}
					: std::span<const GroupSubtypeOverlay>{},
				document.SourceAnimators
					? std::span<const DetachedSourceAnimator>{document.SourceAnimators->Detached}
					: std::span<const DetachedSourceAnimator>{},
				{},
				{},
				false,
				visits,
				false
			);
		}

	}
	Status ObserveSourceKeyframeAxes(
		const Document &document,
		std::vector<SourceAxisObservation> &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_axis_observation");
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			result.capacity() > Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "source axis observation byte or count bound is invalid");
		const auto resident = DocumentRetainedPayloadBytes(document);
		uint64_t borrowed = result.capacity() * sizeof(SourceAxisObservation);
		if (!resident || !detail::SourceAnimatorAdd(borrowed, *resident))
			return fail(Status::LimitExceeded, "source axis observation retained payload exceeds bounds");
		detail::EvaluationBudget budget(maximumBytes);
		auto held = budget.Reserve(borrowed);
		if (!held) return fail(Status::LimitExceeded, "source axis observation overlap exceeds bounds");
		uint64_t work = 0, slots = 0;
		for (const auto &node : document.Nodes) {
			AdmitKeyWork(work, 1);
			if (node.SourceSeparatedVec2Animators) slots += node.SourceSeparatedVec2Animators->Inputs.size();
		}
		if (document.SourceAnimators)
			for (const auto &binding : document.SourceAnimators->Bindings) {
				AdmitKeyWork(work, 1);
				if (binding.Axes.Storage == GroupAxisStorage::Local ||
					binding.Axes.Storage == GroupAxisStorage::Shared)
					++slots;
			}
		if (slots > 2 * Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "source axis observation channel count exceeds bounds");
		if (slots) {
			Plan plan;
			if (Compile(document, plan, diagnostic, budget.Available()) != Status::Ok) return diagnostic.Code;
		}
		const auto bound = slots ? BoundAxisLookup(document, work) : AxisLookupBound{};
		const auto same = [&](std::string_view a, std::string_view b) {
			AdmitKeyWork(work, 1 + std::min(a.size(), b.size()));
			return a == b;
		};
		slots = std::min(slots, uint64_t(Limits::MaximumKeyframes));
		auto backing = budget.Reserve(slots * sizeof(SourceAxisObservation));
		if (!backing) return fail(Status::LimitExceeded, "source axis observation backing exceeds bounds");
		std::vector<SourceAxisObservation> candidate;
		candidate.reserve(size_t(slots));
		auto spare = budget.Reserve((candidate.capacity() - slots) * sizeof(SourceAxisObservation));
		if (!spare) return fail(Status::LimitExceeded, "source axis observation spare slots exceed bounds");
		uint64_t keys = 0;
		const auto observe = [&](const Node &node, std::string_view port) {
			for (const auto &view : candidate)
				if (same(view.NodeId, node.Id) && same(view.Port, port)) return Status::Ok;
			if (!node.InstanceBase.empty()) {
				bool captured = false;
				if (document.SourceAnimators)
					for (const auto &binding : document.SourceAnimators->Bindings)
						if (same(binding.NodeId, node.Id) && same(binding.Port, port)) {
							captured = true;
							break;
						}
				if (!captured)
					return fail(Status::InvalidValue, "source axis alias requires captured bindings");
			}
			const auto axes = ReadKeyAxes(document, node, port, bound, work);
			if (axes.Code == Status::SourceAxisInitializationRequired) return Status::Ok;
			if (axes.Code != Status::Ok) return fail(axes.Code, axes.Message);
			const uint64_t count = axes.Axes->Axes[0].Keys.size() + axes.Axes->Axes[1].Keys.size();
			if (candidate.size() >= Limits::MaximumKeyframes || count > Limits::MaximumKeyframes - keys)
				return fail(
					Status::LimitExceeded, "source axis observation logical key fanout exceeds bounds"
				);
			keys += count;
			candidate.push_back(
				{node.Id, port, axes.Axes, !same(axes.Owner->Id, node.Id) || !same(axes.Port, port)}
			);
			return Status::Ok;
		};
		for (const auto &node : document.Nodes) {
			AdmitKeyWork(work, 1);
			if (!node.SourceSeparatedVec2Animators) continue;
			for (const auto &input : node.SourceSeparatedVec2Animators->Inputs) {
				AdmitKeyWork(work, 1);
				if (input.Initialized && observe(node, input.Port) != Status::Ok) return diagnostic.Code;
			}
		}
		if (document.SourceAnimators)
			for (const auto &binding : document.SourceAnimators->Bindings) {
				AdmitKeyWork(work, 1);
				if (binding.Axes.Storage != GroupAxisStorage::Local &&
					binding.Axes.Storage != GroupAxisStorage::Shared)
					continue;
				const Node *node = nullptr;
				for (const auto &entry : document.Nodes)
					if (same(entry.Id, binding.NodeId)) {
						node = &entry;
						break;
					}
				if (!node) return fail(Status::UnknownNode, "source axis observation node is absent");
				if (observe(*node, binding.Port) != Status::Ok) return diagnostic.Code;
			}
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const KeyEditWorkExceeded &) {
		diagnostic = {
			Status::LimitExceeded, {}, {}, "source axis observation comparison work exceeds bounds"
		};
		return diagnostic.Code;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source axis observation allocation exceeds bounds"};
		return diagnostic.Code;
	}

	Status CaptureSourceKeyframes(
		const Document &document,
		std::span<const SourceKeyframeIdentity> selection,
		std::vector<Keyframe> &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_keyframe_capture");
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || selection.empty() ||
			selection.size() > Limits::MaximumKeyframes || result.size() > Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "source key capture count or byte bound is invalid");
		const auto resident = DocumentRetainedPayloadBytes(document);
		uint64_t borrowed = uint64_t(selection.size()) * sizeof(SourceKeyframeIdentity);
		if (!resident || !detail::SourceAnimatorAdd(borrowed, *resident) ||
			!detail::SourceAnimatorAdd(borrowed, uint64_t(result.capacity()) * sizeof(Keyframe)))
			return fail(Status::LimitExceeded, "source key capture retained payload exceeds bounds");
		size_t retainedKeys = 0;
		for (const auto &key : result)
			if (!detail::SourceAnimatorKey(borrowed, key, false, retainedKeys))
				return fail(Status::LimitExceeded, "source key capture old snapshot exceeds bounds");
		for (const auto &identity : selection) {
			if (identity.Axis < -1 || identity.Axis > 1 || !ValidFrameTime(identity.Time))
				return fail(Status::InvalidValue, "source key capture identity is invalid");
			if (identity.NodeId.size() > Limits::MaximumTextBytes ||
				identity.Port.size() > Limits::MaximumTextBytes ||
				!detail::SourceAnimatorAdd(borrowed, identity.NodeId.size() + identity.Port.size()))
				return fail(Status::LimitExceeded, "source key capture identity exceeds bounds");
		}
		detail::EvaluationBudget budget(maximumBytes);
		auto held = budget.Reserve(borrowed);
		if (!held) return fail(Status::LimitExceeded, "source key capture borrowed overlap exceeds bounds");
		{
			Plan plan;
			if (Compile(document, plan, diagnostic, budget.Available()) != Status::Ok) return diagnostic.Code;
		}
		uint64_t work = 0;
		const auto same = [&](std::string_view a, std::string_view b) {
			AdmitKeyWork(work, 1 + std::min(a.size(), b.size()));
			return a == b;
		};
		const auto bound =
			std::any_of(selection.begin(), selection.end(), [](const auto &id) { return id.Axis >= 0; })
				? BoundAxisLookup(document, work)
				: AxisLookupBound{};
		struct CaptureView {
			const Keyframe *Key;
			bool Alias;
		};
		detail::EvaluationVector<CaptureView> pins{detail::EvaluationAllocator<CaptureView>(budget)};
		pins.reserve(selection.size());
		uint64_t cloneBytes = uint64_t(selection.size()) * sizeof(Keyframe);
		size_t cloneKeys = 0;
		for (size_t index = 0; index < selection.size(); ++index) {
			const auto &identity = selection[index];
			for (size_t previous = 0; previous < index; ++previous) {
				AdmitKeyWork(work, 1);
				const auto &other = selection[previous];
				if (identity.Axis == other.Axis && identity.Time == other.Time &&
					same(identity.NodeId, other.NodeId) && same(identity.Port, other.Port))
					return fail(Status::InvalidValue, "source key capture repeats an identity");
			}
			const Keyframe *found = nullptr;
			bool alias = false;
			if (identity.Axis < 0) {
				for (const auto &key : document.Keyframes) {
					AdmitKeyWork(work, 1);
					if (GetFrameTime(key) == identity.Time && same(key.NodeId, identity.NodeId) &&
						same(key.Port, identity.Port)) {
						found = &key;
						break;
					}
				}
			} else {
				const Node *node = nullptr;
				for (const auto &entry : document.Nodes)
					if (same(entry.Id, identity.NodeId)) {
						node = &entry;
						break;
					}
				if (!node) return fail(Status::UnknownNode, "source key capture axis node is absent");
				if (!node->InstanceBase.empty()) {
					bool captured = false;
					if (document.SourceAnimators)
						for (const auto &binding : document.SourceAnimators->Bindings)
							if (same(binding.NodeId, identity.NodeId) && same(binding.Port, identity.Port)) {
								captured = true;
								break;
							}
					if (!captured)
						return fail(Status::InvalidValue, "source axis alias requires captured bindings");
				}
				const auto axes = ReadKeyAxes(document, *node, identity.Port, bound, work);
				if (axes.Code != Status::Ok) return fail(axes.Code, axes.Message);
				alias = !same(axes.Owner->Id, identity.NodeId) || !same(axes.Port, identity.Port);
				for (const auto &key : axes.Axes->Axes[size_t(identity.Axis)].Keys) {
					AdmitKeyWork(work, 1);
					if (GetFrameTime(key) == identity.Time) {
						found = &key;
						break;
					}
				}
			}
			if (!found) return fail(Status::InvalidValue, "selected source key no longer exists");
			if (!detail::SourceAnimatorKey(cloneBytes, *found, true, cloneKeys) ||
				!detail::SourceAnimatorAdd(cloneBytes, 2 * (identity.NodeId.size() + identity.Port.size())))
				return fail(Status::LimitExceeded, "source key capture clones exceed bounds");
			pins.push_back({found, alias});
		}
		// grug reserve clone and logical-name replacement overlap before copying any key payload.
		if (cloneBytes > UINT64_MAX / 2)
			return fail(Status::LimitExceeded, "source key capture clone overlap overflows");
		auto clones = budget.Reserve(2 * cloneBytes);
		if (!clones) return fail(Status::LimitExceeded, "source key capture clone overlap exceeds bounds");
		std::vector<Keyframe> candidate;
		candidate.reserve(selection.size());
		if (candidate.capacity() > UINT64_MAX / sizeof(Keyframe) ||
			uint64_t(candidate.capacity() - selection.size()) * sizeof(Keyframe) > budget.Available())
			return fail(Status::LimitExceeded, "source key capture spare slots exceed bounds");
		for (size_t index = 0; index < selection.size(); ++index) {
			candidate.push_back(*pins[index].Key);
			candidate.back().NodeId = selection[index].NodeId;
			candidate.back().Port = selection[index].Port;
			if (pins[index].Alias) candidate.back().SourceKeyId.clear();
		}
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const KeyEditWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source key capture comparison work exceeds bounds"};
		return diagnostic.Code;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source key capture allocation exceeds bounds"};
		return diagnostic.Code;
	}

	Status ApplySourceKeyframeEdits(
		const Document &document,
		std::span<const SourceKeyframeEdit> edits,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_keyframe_transition");
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
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
			if (!edit.Original || (edit.Copy && !edit.Replacement) || edit.Axis < -1 || edit.Axis > 1)
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
		const auto axisBound =
			std::any_of(edits.begin(), edits.end(), [](const auto &edit) { return edit.Axis >= 0; })
				? BoundAxisLookup(document, work)
				: AxisLookupBound{};
		detail::EvaluationVector<WriterEdit> writers{detail::EvaluationAllocator<WriterEdit>(budget)};
		detail::EvaluationVector<ResolvedEdit> resolved{detail::EvaluationAllocator<ResolvedEdit>(budget)};
		for (const auto &edit : edits) {
			const auto &original = *edit.Original;
			std::string_view owner = original.NodeId, port = original.Port;
			const SourceSeparatedVec2Animator *axes = nullptr;
			if (edit.Axis >= 0) {
				const Node *selected = nullptr;
				for (const auto &node : document.Nodes)
					if (same(node.Id, original.NodeId)) selected = &node;
				if (!selected) return fail(Status::UnknownNode, "source axis target is absent");
				const auto bindings =
					document.SourceAnimators
						? std::span<const GroupSubtypeBinding>{document.SourceAnimators->Bindings}
						: std::span<const GroupSubtypeBinding>{};
				if (!selected->InstanceBase.empty()) {
					bool captured = false;
					for (const auto &binding : bindings)
						if (same(binding.NodeId, original.NodeId) && same(binding.Port, original.Port))
							captured = true;
					if (!captured)
						return fail(Status::InvalidValue, "source axis alias requires captured bindings");
				}
				const auto view = ReadKeyAxes(document, *selected, original.Port, axisBound, work);
				if (view.Code != Status::Ok) return fail(view.Code, view.Message);
				owner = view.Owner->Id;
				port = view.Port;
				axes = view.Axes;
				for (const auto *key : {edit.Original, edit.Replacement}) {
					if (!key) continue;
					const auto *scalar = std::get_if<double>(&key->Data);
					if ((!scalar || !std::isfinite(*scalar)) && !std::holds_alternative<int64_t>(key->Data))
						return fail(Status::TypeMismatch, "source axis key must contain a finite scalar");
				}
				if (!edit.Copy) {
					bool pinned = false;
					const bool alias = owner != original.NodeId || port != original.Port;
					for (const auto &key : axes->Axes[size_t(edit.Axis)].Keys)
						if (GetFrameTime(key) == GetFrameTime(original) && equivalent(key, original) &&
							same(
								alias ? std::string_view{} : std::string_view(key.SourceKeyId),
								original.SourceKeyId
							))
							pinned = true;
					if (!pinned)
						return fail(Status::InvalidValue, "source axis key changed during its gesture");
				}
			} else {
				if (!edit.Copy) {
					bool pinned = false;
					for (const auto &key : document.Keyframes)
						if (same(key.NodeId, original.NodeId) && same(key.Port, original.Port) &&
							equivalent(key, original) && same(key.SourceKeyId, original.SourceKeyId))
							pinned = true;
					if (!pinned) return fail(Status::InvalidValue, "source key changed during its gesture");
				}
				if (document.SourceAnimators)
					for (const auto &binding : document.SourceAnimators->Bindings)
						if (same(binding.NodeId, owner) && same(binding.Port, port)) {
							owner = binding.OwnerId;
							port = binding.AnimatorPort.empty() ? binding.Port : binding.AnimatorPort;
							break;
						}
			}

			size_t writer = 0;
			for (; writer < writers.size(); ++writer)
				if (writers[writer].Axis == edit.Axis && same(writers[writer].Node, owner) &&
					same(writers[writer].Port, port))
					break;
			if (writer == writers.size()) {
				WriterEdit next(budget);
				next.Node = owner;
				next.Port = port;
				next.Axis = edit.Axis;
				const std::vector<Keyframe> *keys =
					axes ? &axes->Axes[size_t(edit.Axis)].Keys : &document.Keyframes;
				if (document.SourceAnimators)
					for (size_t index = 0; index < document.SourceAnimators->DetachedValues.size(); ++index) {
						const auto &record = document.SourceAnimators->DetachedValues[index];
						if (same(record.NodeId, owner) && same(record.Port, port)) {
							next.Detached = index;
							if (!axes) keys = &record.Keys;
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
				if (writer.Axis < 0 && writer.Detached == SIZE_MAX && same(writer.Node, node) &&
					same(writer.Port, port))
					return &writer;
			if (document.SourceAnimators)
				for (const auto &binding : document.SourceAnimators->Bindings)
					if (same(binding.NodeId, node) && same(binding.Port, port))
						for (const auto &writer : writers)
							if (writer.Axis < 0 && same(binding.OwnerId, writer.Node) &&
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
			if (writer.Axis >= 0) continue;
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
		detail::EvaluationVector<AxisTarget> axisTargets{detail::EvaluationAllocator<AxisTarget>(budget)};
		const auto addAxisTarget = [&](size_t writer, size_t node, size_t input, size_t overlay) {
			for (const auto &target : axisTargets) {
				admit(1);
				if (target.Writer == writer && target.Node == node && target.Input == input &&
					target.Overlay == overlay)
					return;
			}
			axisTargets.push_back({writer, node, input, overlay});
		};
		const auto addNodeAxisTarget = [&](size_t writer, std::string_view nodeId, std::string_view port) {
			for (size_t node = 0; node < document.Nodes.size(); ++node) {
				const auto &owner = document.Nodes[node];
				if (!same(owner.Id, nodeId) || !owner.SourceSeparatedVec2Animators) continue;
				for (size_t input = 0; input < owner.SourceSeparatedVec2Animators->Inputs.size(); ++input) {
					const auto &axes = owner.SourceSeparatedVec2Animators->Inputs[input];
					if (same(axes.Port, port) && axes.Initialized)
						addAxisTarget(writer, node, input, SIZE_MAX);
				}
			}
		};
		for (size_t index = 0; index < writers.size(); ++index) {
			const auto &writer = writers[index];
			if (writer.Axis < 0) continue;
			if (writer.Detached != SIZE_MAX)
				addAxisTarget(index, SIZE_MAX, SIZE_MAX, writer.Detached);
			else
				addNodeAxisTarget(index, writer.Node, writer.Port);
			if (document.SourceAnimators)
				for (const auto &binding : document.SourceAnimators->Bindings) {
					const auto &axes = binding.Axes;
					if ((axes.Storage == GroupAxisStorage::Local ||
						 axes.Storage == GroupAxisStorage::Shared) &&
						same(axes.OwnerId, writer.Node) && same(axes.Port, writer.Port))
						addNodeAxisTarget(index, binding.NodeId, binding.Port);
				}
		}
		for (const auto &target : axisTargets) {
			const auto &writer = writers[target.Writer];
			const auto node =
				target.Overlay != SIZE_MAX ? writer.Node : std::string_view(document.Nodes[target.Node].Id);
			const auto port =
				target.Overlay != SIZE_MAX
					? writer.Port
					: std::string_view(
						  document.Nodes[target.Node].SourceSeparatedVec2Animators->Inputs[target.Input].Port
					  );
			for (const auto &key : writer.Keys)
				chargeKey(key, node, port);
		}
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
			if (writer.Axis < 0 && writer.Detached != SIZE_MAX)
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
		detail::EvaluationVector<size_t> newDetachedTracks{detail::EvaluationAllocator<size_t>(budget)};
		if (document.SourceAnimators)
			for (const auto &writer : writers) {
				if (writer.Axis >= 0 || writer.Detached == SIZE_MAX) continue;
				bool source = false;
				for (const auto &key : writer.Keys)
					if (same(key.Data->Interpolation, "source")) source = true;
				if (!source) continue;
				for (size_t index = 0; index < document.SourceAnimators->Detached.size(); ++index) {
					const auto &metadata = document.SourceAnimators->Detached[index];
					if (metadata.Track || !same(metadata.OwnerId, writer.Node) ||
						!same(metadata.Id, writer.Port))
						continue;
					newDetachedTracks.push_back(index);
					if (!detail::SourceAnimatorAdd(
							finalBytes, sizeof(AnimationTrack) + writer.Node.size() + writer.Port.size() + 64
						))
						throw std::bad_alloc{};
				}
			}
		size_t aggregateKeys = finalCount;
		const auto axisCount =
			[&](size_t node, size_t input, size_t overlay, const SourceSeparatedVec2Animator &axes) {
				for (int axis = 0; axis < 2; ++axis) {
					size_t count = axes.Axes[size_t(axis)].Keys.size();
					for (const auto &target : axisTargets) {
						admit(1);
						if (target.Node == node && target.Input == input && target.Overlay == overlay &&
							writers[target.Writer].Axis == axis)
							count = writers[target.Writer].Keys.size();
					}
					if (count > Limits::MaximumKeyframes - aggregateKeys) return false;
					aggregateKeys += count;
				}
				return true;
			};
		for (size_t node = 0; node < document.Nodes.size(); ++node)
			if (document.Nodes[node].SourceSeparatedVec2Animators)
				for (size_t input = 0;
					 input < document.Nodes[node].SourceSeparatedVec2Animators->Inputs.size();
					 ++input)
					if (!axisCount(
							node,
							input,
							SIZE_MAX,
							document.Nodes[node].SourceSeparatedVec2Animators->Inputs[input]
						))
						return fail(
							Status::LimitExceeded, "source axis fanout exceeds aggregate key count bounds"
						);
		// grug keep authored rows and retained generations inside their separate public count limits.
		aggregateKeys = 0;
		if (document.SourceAnimators)
			for (size_t overlay = 0; overlay < document.SourceAnimators->DetachedValues.size(); ++overlay) {
				const auto &stored = document.SourceAnimators->DetachedValues[overlay];
				size_t count = stored.Keys.size();
				for (const auto &writer : writers) {
					admit(1);
					if (writer.Axis < 0 && writer.Detached == overlay) count = writer.Keys.size();
				}
				if (count > Limits::MaximumKeyframes - aggregateKeys)
					return fail(Status::LimitExceeded, "source retained key fanout exceeds aggregate bounds");
				aggregateKeys += count;
				if (stored.SeparatedVec2 && !axisCount(SIZE_MAX, SIZE_MAX, overlay, *stored.SeparatedVec2))
					return fail(
						Status::LimitExceeded, "source retained axis fanout exceeds aggregate bounds"
					);
			}
		if (*oldBytes > UINT64_MAX / 2 || !detail::SourceAnimatorAdd(finalBytes, 2 * *oldBytes))
			return fail(Status::LimitExceeded, "source key clone overlap overflows");
		auto owned = budget.Reserve(finalBytes);
		if (!owned) return fail(Status::LimitExceeded, "source key clone and fanout exceed byte bounds");
		Document candidate = document;
		candidate.Tracks.reserve(candidate.Tracks.size() + newTracks.size());
		for (const auto &[node, port] : newTracks)
			candidate.Tracks.push_back({std::string(node), std::string(port), "hold", -1});
		for (const auto index : newDetachedTracks) {
			auto &metadata = candidate.SourceAnimators->Detached[index];
			metadata.Track = AnimationTrack{metadata.OwnerId, metadata.Id, "hold", -1};
		}
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
		const bool combinedWrites =
			std::any_of(writers.begin(), writers.end(), [](const auto &writer) { return writer.Axis < 0; });
		std::vector<Keyframe> finalKeys;
		if (combinedWrites) {
			finalKeys.reserve(finalCount);
			for (const auto &key : document.Keyframes)
				if (!writerFor(key.NodeId, key.Port)) finalKeys.push_back(key);
			for (const auto &[node, port] : targets) {
				const auto *writer = writerFor(node, port);
				const bool alias = node != writer->Node || port != writer->Port;
				for (const auto &key : writer->Keys)
					finalKeys.push_back(clone(key, node, port, alias));
			}
		}
		for (const auto &writer : writers)
			if (writer.Axis < 0 && writer.Detached != SIZE_MAX) {
				auto &stored = candidate.SourceAnimators->DetachedValues[writer.Detached];
				std::vector<Keyframe> keys;
				keys.reserve(writer.Keys.size());
				for (const auto &key : writer.Keys)
					keys.push_back(clone(key, writer.Node, writer.Port, false));
				stored.Keys = std::move(keys);
				stored.Fixed.reset();
			}
		for (const auto &target : axisTargets) {
			const auto &writer = writers[target.Writer];
			auto &axes =
				target.Overlay != SIZE_MAX
					? *candidate.SourceAnimators->DetachedValues[target.Overlay].SeparatedVec2
					: candidate.Nodes[target.Node].SourceSeparatedVec2Animators->Inputs[target.Input];
			const auto node =
				target.Overlay != SIZE_MAX ? writer.Node : std::string_view(candidate.Nodes[target.Node].Id);
			const bool alias = node != writer.Node || axes.Port != writer.Port;
			std::vector<Keyframe> keys;
			keys.reserve(writer.Keys.size());
			for (const auto &key : writer.Keys)
				keys.push_back(clone(key, node, axes.Port, alias));
			axes.Axes[size_t(writer.Axis)].Keys = std::move(keys);
		}
		if (combinedWrites) {
			std::sort(finalKeys.begin(), finalKeys.end(), [&](const auto &a, const auto &b) {
				admit(1);
				const auto aSocket = std::tie(a.NodeId, a.Port), bSocket = std::tie(b.NodeId, b.Port);
				admit(a.NodeId.size() + a.Port.size() + b.NodeId.size() + b.Port.size());
				if (aSocket != bSocket) return aSocket < bSocket;
				return CompareFrameTime(GetFrameTime(a), GetFrameTime(b)) < 0;
			});
			candidate.Keyframes = std::move(finalKeys);
		}
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

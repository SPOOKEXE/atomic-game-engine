#pragma once

#include <engine/imagegraph/Document.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace engine::imagegraph {
	namespace detail {
		struct GroupReplayAccess;
	}
	enum class GroupRefreshReason { Load, Edit, Connect, ParentEdit, Restore };
	enum class GroupSubtypeAnimator { Static, Animated };
	// One explicit source refresh callback, in caller event order. Sampling alone
	// is not an event.
	struct GroupRefreshEvent {
		std::string NodeId;
		GroupRefreshReason Reason = GroupRefreshReason::Load;
		EvaluationRequest At;
		GroupSubtypeAnimator SubtypeAnimator = GroupSubtypeAnimator::Static;
		// Source key-override preference matters only when an animated subtype has an
		// exact-time key.
		bool ReplaceExistingKey = true;
		// The actual edited local property. Linked inputs still win when its value is
		// consumed.
		std::string_view EditedPort;
		const Value *LocalValue = nullptr;
		bool LocalAnimated = false;
	};
	struct GroupReplayEntry {
		std::string NodeId;
		int64_t InputType = 11;
		int64_t Subtype = 0;
		int64_t VectorSize = 0;
		SourceSocketDomain Domain;
		// Replaces the local parent animator and suppresses its original keys. Links
		// keep priority.
		std::optional<Value> ParentReset;
		std::optional<Value> SubtypeStatic;
		std::vector<Keyframe> SubtypeKeys;
		std::vector<Keyframe> ParentKeys;
	};
	enum class GroupAxisStorage : uint8_t { None, Uninitialized, Local, Shared };
	// group binding copies the current scalar array, independently from the combined animator.
	// an uninitialized array keeps its local constructor owner even if the base creates axes later.
	struct GroupAxisBinding {
		GroupAxisStorage Storage = GroupAxisStorage::None;
		std::string OwnerId;
		std::string Port;
		std::string InstanceBase;
		GroupSubtypeAnimator Writer = GroupSubtypeAnimator::Static;
		bool operator==(const GroupAxisBinding &) const = default;
	};
	struct GroupSubtypeBinding {
		std::string NodeId;
		std::string OwnerId;
		// Getter mode belongs to the target when overridden, otherwise the delegated owner.
		GroupSubtypeAnimator Getter = GroupSubtypeAnimator::Static;
		// Animator.prop remains the original property after its animator is aliased.
		GroupSubtypeAnimator Writer = GroupSubtypeAnimator::Static;
		// Actual source child input. Group.inputs parent_value remains local.
		std::string Port = "subtype";
		// The retained animator can keep its original socket after a physical input move.
		// Empty uses Port. An admitted detached animator uses its native Id.
		// Inherited getters still resolve their current input index.
		std::string AnimatorPort{};
		GroupAxisBinding Axes{};
	};
	// Metadata for one retained animator whose original physical input was removed.
	// The shared overlay owns its values and keys; this record never duplicates them.
	struct DetachedSourceAnimator {
		std::string Id;
		std::string OwnerId;
		std::string OriginalPort;
		GroupSubtypeAnimator Writer = GroupSubtypeAnimator::Static;
		ValueType Type = ValueType::Any;
		std::optional<bool> ArrayClassification;
		std::optional<AnimationTrack> Track;
		bool operator==(const DetachedSourceAnimator &) const = default;
	};
	struct GroupSubtypeOverlay {
		std::string NodeId;
		std::optional<Value> Fixed;
		std::vector<Keyframe> Keys;
		std::string Port = "subtype";
		// scalar storage may have a different owner from the combined animator.
		OwnedPayload3D<SourceSeparatedVec2Animator> SeparatedVec2{};
	};
	// The host owns this immutable replay result. It never mutates the authored
	// document.
	class GroupReplayState {
	  public:
		GroupReplayState();
		~GroupReplayState();
		GroupReplayState(GroupReplayState &&) noexcept;
		GroupReplayState &operator=(GroupReplayState &&) noexcept;
		GroupReplayState(const GroupReplayState &) = delete;
		GroupReplayState &operator=(const GroupReplayState &) = delete;
		const GroupReplayEntry *Find(std::string_view nodeId) const noexcept;
		std::span<const GroupReplayEntry> Entries() const noexcept;
		std::span<const GroupSubtypeBinding> Bindings() const noexcept;
		std::span<const GroupSubtypeOverlay> SharedSubtypes() const noexcept;
		std::span<const DetachedSourceAnimator> DetachedAnimators() const noexcept;
		const DetachedSourceAnimator *
		DetachedAnimator(std::string_view ownerId, std::string_view id) const noexcept;
		const GroupSubtypeBinding *
		Binding(std::string_view nodeId, std::string_view port = "subtype") const noexcept;
		const GroupSubtypeOverlay *
		SharedSubtype(std::string_view ownerId, std::string_view port = "subtype") const noexcept;
		bool InstancesBound() const noexcept;
		uint64_t RetainedBytes() const noexcept;
		uint64_t AuthoringRevision() const noexcept;

	  private:
		struct Storage;
		std::unique_ptr<Storage> Data;
		friend struct detail::GroupReplayAccess;
	};
	// installs or reconciles source aliases without running refresh callbacks.
	// unchanged immediate bases keep captured axes; new bindings resolve parent before child.
	// repeated source setInstance axis recapture needs a distinct explicit transition.
	// Local subtype effects on targets are retired when their input animator alias is installed.
	// The byte bound includes the borrowed document and old/new owner overlap.
	// Replaces instance bindings at the same revision, preserving frozen callback declarations.
	Status BindGroupReplay(
		const Document &document,
		std::span<const GroupSubtypeBinding> bindings,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	struct SourceAxisInitialization {
		std::string_view NodeId;
		std::string_view Port;
	};
	// explicit local getAnimators events create frame-zero scalar storage from constructor defaults.
	// combined aliases and local separation flags stay unchanged; warm arrays retain their contents.
	// existing cold descendants stay cold; new bindings can share the created local array.
	// failed batches preserve both states, including an aliased result.
	Status InitializeSourceVec2Axes(
		const Document &document,
		std::span<const SourceAxisInitialization> targets,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Rebind unchanged callback effects after an unrelated authored edit. This
	// performs no refresh and removes effects owned by deleted boundary nodes.
	// The byte bound includes the borrowed document and old/new owner overlap.
	// Failed replacement preserves both states.
	Status RebindGroupReplay(
		const Document &document,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Surviving source input objects retain their animator identity when renamed.
	// Empty NewPort retires the selected physical input. Moves are simultaneous.
	struct SourceInputMove {
		std::string_view NodeId;
		std::string_view OldPort;
		std::string_view NewPort;
	};
	// The caller has already staged physical input records, keys and links.
	// This validates old/new membership and moves retained animator effects only.
	// Failed replacement preserves all states, including an aliased result.
	Status RebindGroupReplayWithInputMoves(
		const Document &original,
		const Document &staged,
		std::span<const SourceInputMove> moves,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// The caller has staged a fresh frame-zero source animator on this physical socket.
	// Replacement never redirects through a delegated animator writer.
	struct SourceAnimatorReplacement {
		std::string_view NodeId;
		std::string_view Port;
	};
	// Rebinds the staged document while retiring only matching physical SharedSubtype effects.
	// Dormant unsplit replacement preserves separate-axis edits.
	// Boundary Entry animators use their existing distinct refresh operations.
	// Failed replacement preserves previous and result, including when they alias.
	Status RebindGroupReplayWithAnimatorReplacements(
		const Document &document,
		std::span<const SourceAnimatorReplacement> replacements,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Ordered bootstrap callbacks. Source APPEND supplies appended-list order;
	// native hosts choose their own explicit order. The current signed clock
	// belongs to the caller, not the saved archive.
	struct GroupBootstrapTarget {
		std::string_view NodeId;
		GroupSubtypeAnimator SubtypeAnimator = GroupSubtypeAnimator::Static;
	};
	Status ReplayGroupBootstrap(
		const Document &document,
		const Plan &plan,
		std::span<const GroupBootstrapTarget> order,
		const EvaluationRequest &clock,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Native host restore policy: derive declarations at the caller clock without
	// changing serialized animators. This is not a source load callback.
	Status RestoreGroupDeclarations(
		const Document &document,
		const Plan &plan,
		std::span<const GroupBootstrapTarget> order,
		const EvaluationRequest &clock,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Resolve actual linked/timeline controls at each event, then apply their
	// ordered refresh effects. Failed operations preserve both states. Old and new
	// state overlap under maximumBytes.
	Status ReplayGroupRefresh(
		const Document &document,
		const Plan &plan,
		std::span<const GroupRefreshEvent> events,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Edits source child input animators through their original owner and loaded
	// Group parent animators locally without a refresh callback. Getter mode,
	// override flags and links do not change Animator.prop's writer mode. Events
	// borrow their LocalValue and EditedPort; this performs no Group refresh.
	Status ReplayGroupAnimatorEdits(
		const Document &document,
		std::span<const GroupRefreshEvent> edits,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Project serialized animator effects into a replacement authored document.
	// Cached declarations and socket domains remain replay-only. Admission includes
	// source, replay, prior result and candidate/scratch overlap. Failure preserves result.
	Status ProjectGroupReplay(
		const Document &authored,
		const GroupReplayState &replay,
		uint64_t authoringRevision,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	// Accept a successful authored projection, retaining derived declarations and
	// bindings while releasing effects already represented by serialized animators.
	// The document must contain every prior effect. Failure preserves both states.
	Status RebindProjectedGroupReplay(
		const Document &document,
		const GroupReplayState &previous,
		uint64_t authoringRevision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
} // namespace engine::imagegraph

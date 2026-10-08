#pragma once

#include "Utf8TextOps.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	inline bool CommonOwnerUtf8(std::string_view text) noexcept {
		size_t characters = 0;
		return CountText(text, characters) == TextOpStatus::Ok;
	}
	// Authored owners reference one native location and one retained local Update animator.
	struct SourceCommonOwnerWorkExceeded {};
	inline Status ValidateSourceCommonOwners(const Document &document, Diagnostic &diagnostic) try {
		const auto fail = [&](Status status, const char *message, std::string_view owner = {}) {
			diagnostic = {status, std::string(owner), "pxcx.update_in_trigger", message};
			return status;
		};
		if (document.Nodes.size() > Limits::MaximumNodes || document.Groups.size() > Limits::MaximumGroups)
			return fail(Status::LimitExceeded, "source common native owner count exceeds limits");
		for (const auto &group : document.Groups)
			if (!std::isfinite(group.SourcePosition.X) || !std::isfinite(group.SourcePosition.Y) ||
				group.SourceInternalName.size() > Limits::MaximumTextBytes ||
				group.SourceInternalName.find('\0') != std::string::npos ||
				!CommonOwnerUtf8(group.SourceInternalName))
				return fail(Status::InvalidValue, "source group metadata is invalid", group.Id);
		for (const auto &group : document.Groups)
			if (document.FormatVersion < 11 &&
				(group.SourcePosition != Vector2{} || !group.SourceInternalName.empty()))
				return fail(Status::UnsupportedVersion, "source group metadata requires format 11", group.Id);
		if (document.SourceCommonOwners.empty()) return Status::Ok;
		if (document.FormatVersion < 11)
			return fail(Status::UnsupportedVersion, "source common owners require format 11");
		if (document.SourceCommonOwners.size() > Limits::MaximumSourceCommonOwners)
			return fail(Status::LimitExceeded, "source common owner count exceeds its limit");
		uint64_t work = 0;
		const auto spend = [&](uint64_t visits) {
			if (visits > 64'000'000 - work) throw SourceCommonOwnerWorkExceeded{};
			work += visits;
		};
		const auto same = [&](std::string_view first, std::string_view second) {
			spend(1 + std::min(first.size(), second.size()));
			return first == second;
		};
		const auto text = [](const std::string &value, bool required = false) {
			return (!required || !value.empty()) && value.size() <= Limits::MaximumTextBytes &&
				   value.find('\0') == std::string::npos && CommonOwnerUtf8(value);
		};
		for (size_t index = 0; index < document.SourceCommonOwners.size(); ++index) {
			const auto &owner = document.SourceCommonOwners[index];
			if (!text(owner.SourceOwnerId, true) || !text(owner.SourceType, true) ||
				!text(owner.NativeOwnerId, true) || !text(owner.InstanceBase) ||
				!text(owner.UpdateAnimatorOwnerId, true) || !text(owner.UpdateAnimatorPort, true))
				return fail(Status::InvalidValue, "source common owner text is invalid", owner.SourceOwnerId);
			spend(
				owner.SourceOwnerId.size() + owner.SourceType.size() + owner.NativeOwnerId.size() +
				owner.InstanceBase.size() + owner.UpdateAnimatorOwnerId.size() +
				owner.UpdateAnimatorPort.size() + index + document.Nodes.size() + document.Groups.size()
			);
			for (size_t prior = 0; prior < index; ++prior) {
				const auto &other = document.SourceCommonOwners[prior];
				spend(
					std::min(other.SourceOwnerId.size(), owner.SourceOwnerId.size()) +
					std::min(other.NativeOwnerId.size(), owner.NativeOwnerId.size())
				);
				if (other.SourceOwnerId == owner.SourceOwnerId ||
					(other.NativeOwnerKind == owner.NativeOwnerKind &&
					 other.NativeOwnerId == owner.NativeOwnerId))
					return fail(
						Status::DuplicateId, "source common owner identity repeats", owner.SourceOwnerId
					);
			}
			if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &value) {
						return same(value.Id, owner.NativeOwnerId);
					});
				if (node != document.Nodes.end()) {
					spend(node->SourceDisplayName.size());
					for (const auto &property : node->SourceProperties) {
						if (same(property.Port, "update_graph"))
							return fail(
								Status::InvalidValue,
								"source update_graph has duplicate authored storage",
								owner.SourceOwnerId
							);
					}
				}
				if (node == document.Nodes.end() || !std::isfinite(node->Position.X) ||
					!std::isfinite(node->Position.Y) || !text(node->SourceDisplayName))
					return fail(
						Status::InvalidGroup, "source common node binding is invalid", owner.SourceOwnerId
					);
			} else if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group) {
				const auto group =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &value) {
						return same(value.Id, owner.NativeOwnerId);
					});
				if (group != document.Groups.end())
					spend(group->Name.size() + group->SourceInternalName.size());
				if (group == document.Groups.end() || !std::isfinite(group->SourcePosition.X) ||
					!std::isfinite(group->SourcePosition.Y) || !text(group->Name) ||
					!text(group->SourceInternalName))
					return fail(
						Status::InvalidGroup, "source common group binding is invalid", owner.SourceOwnerId
					);
			} else
				return fail(
					Status::InvalidValue, "source common native owner kind is invalid", owner.SourceOwnerId
				);
			if (owner.UpdateExpression)
				spend(owner.UpdateExpression->Code.size() + owner.UpdateExpression->Port.size());
			if (owner.UpdateExpression && (owner.UpdateExpression->Port != "pxcx.update_in_trigger" ||
										   !text(owner.UpdateExpression->Code)))
				return fail(
					Status::InvalidValue, "source common Update expression is invalid", owner.SourceOwnerId
				);
			if (owner.UpdateAnimatorOwnerId != owner.SourceOwnerId)
				return fail(
					Status::InvalidGroup,
					"source common local writer belongs to another owner",
					owner.SourceOwnerId
				);
			if (!document.SourceAnimators)
				return fail(
					Status::InvalidGroup, "source common Update animator is absent", owner.SourceOwnerId
				);
			const auto &state = *document.SourceAnimators;
			spend(state.Detached.size() + state.DetachedValues.size());
			const auto animator =
				std::find_if(state.Detached.begin(), state.Detached.end(), [&](const auto &value) {
					return same(value.OwnerId, owner.UpdateAnimatorOwnerId) &&
						   same(value.Id, owner.UpdateAnimatorPort);
				});
			const auto payload = std::find_if(
				state.DetachedValues.begin(), state.DetachedValues.end(), [&](const auto &value) {
					return same(value.NodeId, owner.UpdateAnimatorOwnerId) &&
						   same(value.Port, owner.UpdateAnimatorPort);
				}
			);
			if (animator == state.Detached.end() || payload == state.DetachedValues.end() ||
				animator->OriginalPort != "pxcx.update_in_trigger" || animator->Type != ValueType::Boolean ||
				payload->SeparatedVec2)
				return fail(
					Status::InvalidGroup,
					"source common Update animator binding is invalid",
					owner.SourceOwnerId
				);
			const auto *current = &owner;
			for (size_t hop = 0; !current->InstanceBase.empty(); ++hop) {
				if (hop >= document.SourceCommonOwners.size())
					return fail(
						Status::InvalidGroup, "source common instance ancestry is cyclic", owner.SourceOwnerId
					);
				spend(document.SourceCommonOwners.size());
				const auto base = std::find_if(
					document.SourceCommonOwners.begin(),
					document.SourceCommonOwners.end(),
					[&](const auto &value) { return same(value.SourceOwnerId, current->InstanceBase); }
				);
				if (base == document.SourceCommonOwners.end() || base->SourceType != owner.SourceType)
					return fail(
						Status::InvalidGroup, "source common instance base is invalid", owner.SourceOwnerId
					);
				current = &*base;
			}
		}
		return Status::Ok;
	} catch (const SourceCommonOwnerWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source common owner validation exceeds work limits"};
		return diagnostic.Code;
	}
}

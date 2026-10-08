using CommonLookupKey = std::pair<std::string_view, std::string_view>;
using CommonLookupIndex = std::vector<std::pair<CommonLookupKey, size_t>>;
struct CommonLookupWorkExceeded {};
int CompareSourceCommonKey(CommonLookupKey left, CommonLookupKey right, uint64_t &workRemaining) {
	const auto compare = [&](std::string_view a, std::string_view b) {
		for (size_t position = 0; position < std::min(a.size(), b.size()); ++position) {
			if (!workRemaining) throw CommonLookupWorkExceeded{};
			--workRemaining;
			const auto first = static_cast<unsigned char>(a[position]),
					   second = static_cast<unsigned char>(b[position]);
			if (first != second) return first < second ? -1 : 1;
		}
		if (!workRemaining) throw CommonLookupWorkExceeded{};
		--workRemaining;
		return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
	};
	const int owner = compare(left.first, right.first);
	return owner ? owner : compare(left.second, right.second);
}

template <class Rows, class Key>
std::optional<CommonLookupIndex>
IndexSourceCommon(const Rows &rows, Key key, size_t &payloadBudget, uint64_t &workRemaining) {
	const uint64_t work = uint64_t(rows.size()) * 2;
	if (work > workRemaining) return {};
	workRemaining -= work;
	CommonLookupIndex index;
	if (!ReserveEmptyProjectionSlots(index, rows.size(), payloadBudget)) return {};
	for (size_t position = 0; position < rows.size(); ++position) {
		const auto identity = key(rows[position]);
		if (identity.first.size() > Limits::MaximumTextBytes ||
			identity.second.size() > Limits::MaximumTextBytes)
			return {};
		index.push_back({identity, position});
	}
	std::sort(index.begin(), index.end(), [&](const auto &left, const auto &right) {
		return CompareSourceCommonKey(left.first, right.first, workRemaining) < 0;
	});
	for (size_t position = 1; position < index.size(); ++position)
		if (CompareSourceCommonKey(index[position - 1].first, index[position].first, workRemaining) == 0)
			return {};
	return index;
}

template <class Rows>
auto FindSourceCommon(
	const CommonLookupIndex &index, Rows &rows, CommonLookupKey key, uint64_t &workRemaining
) {
	const auto found =
		std::lower_bound(index.begin(), index.end(), key, [&](const auto &entry, auto identity) {
			return CompareSourceCommonKey(entry.first, identity, workRemaining) < 0;
		});
	return found == index.end() || CompareSourceCommonKey(found->first, key, workRemaining) != 0
			   ? rows.end()
			   : rows.begin() + found->second;
}

// Bound the additional common writer storage without applying runtime array rules to unrelated source values.
bool BoundSourceCommonWriter(
	const SourceCommonOwnerRecord &owner,
	const DetachedSourceAnimator &metadata,
	const GroupSubtypeOverlay &payload,
	size_t &budget
) {
	const auto text = [&](const std::string &value) {
		return value.size() <= Limits::MaximumTextBytes && Spend(value.size(), budget);
	};
	if (!Spend(
			sizeof(SourceCommonOwnerRecord) + sizeof(DetachedSourceAnimator) + sizeof(GroupSubtypeOverlay),
			budget
		) ||
		!text(owner.SourceOwnerId) || !text(owner.SourceType) || !text(owner.NativeOwnerId) ||
		!text(owner.InstanceBase) || !text(owner.UpdateAnimatorOwnerId) || !text(owner.UpdateAnimatorPort) ||
		!text(metadata.Id) || !text(metadata.OwnerId) || !text(metadata.OriginalPort) ||
		(owner.UpdateExpression &&
		 (!text(owner.UpdateExpression->Port) || !text(owner.UpdateExpression->Code))) ||
		payload.SeparatedVec2 || !text(payload.NodeId) || !text(payload.Port) ||
		(payload.Fixed && !BoundedValue(*payload.Fixed, budget)) ||
		payload.Keys.size() > Limits::MaximumKeyframes ||
		!Spend(payload.Keys.size() * sizeof(Keyframe), budget))
		return false;
	if (metadata.Track && (!Spend(sizeof(AnimationTrack), budget) || !text(metadata.Track->NodeId) ||
						   !text(metadata.Track->Port) || !text(metadata.Track->End)))
		return false;
	for (const auto &key : payload.Keys) {
		if (key.SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes || !text(key.SourceKeyId) ||
			!text(key.NodeId) || !text(key.Port) || !text(key.Interpolation) ||
			!BoundedValue(key.Data, budget) ||
			(key.Ease &&
			 (!Spend(sizeof(KeyframeEase), budget) || !text(key.Ease->InType) || !text(key.Ease->OutType))))
			return false;
		if (key.SourceDriver && (!ValidKeyframeSourceDriver(*key.SourceDriver) ||
								 !std::visit(
									 [&](const auto &driver) {
										 using D = std::decay_t<decltype(driver)>;
										 if constexpr (std::is_same_v<D, KeyframeCurveDriver>)
											 return Bounded(driver.Data, budget);
										 else if constexpr (std::is_same_v<D, KeyframeAudioDriver>)
											 return text(driver.SourceId) && text(driver.Metric);
										 else
											 return Spend(sizeof(D), budget);
									 },
									 *key.SourceDriver
								 )))
			return false;
	}
	return true;
}

// Common authoring owns its local writer independently of inherited or linked getters.
bool WriteSourceCommonOwners(
	Json &root, Document &desired, const Document &previous, Diagnostic &diagnostic, size_t &payloadBudget
) try {
	if (desired.SourceCommonOwners.size() > Limits::MaximumNodes + Limits::MaximumGroups)
		return Reject(diagnostic, "PXC common owner count exceeds its limit");
	if (desired.SourceCommonOwners.empty()) return true;
	if (!desired.SourceAnimators) return Reject(diagnostic, "PXC common writer state is absent");
	auto &sources = root["nodes"].get_ref<Json::array_t &>();
	const auto &state = *desired.SourceAnimators;
	const SourceAnimatorState empty;
	const auto &before = previous.SourceAnimators ? *previous.SourceAnimators : empty;
	uint64_t workRemaining = bake::PxcxLimits::MaximumLinks * 16;
	const auto single = [](const auto &row) { return CommonLookupKey{row.Id, {}}; };
	const auto ownerKey = [](const auto &row) { return CommonLookupKey{row.SourceOwnerId, {}}; };
	const auto animatorKey = [](const auto &row) { return CommonLookupKey{row.OwnerId, row.Id}; };
	const auto payloadKey = [](const auto &row) { return CommonLookupKey{row.NodeId, row.Port}; };
	const auto sourceIndex = IndexSourceCommon(
		sources,
		[](const auto &row) {
			return CommonLookupKey{row.at("id").template get_ref<const std::string &>(), {}};
		},
		payloadBudget,
		workRemaining
	);
	const auto ownerIndex =
		IndexSourceCommon(previous.SourceCommonOwners, ownerKey, payloadBudget, workRemaining);
	const auto nodeIndex = IndexSourceCommon(desired.Nodes, single, payloadBudget, workRemaining);
	const auto oldNodeIndex = IndexSourceCommon(previous.Nodes, single, payloadBudget, workRemaining);
	const auto groupIndex = IndexSourceCommon(desired.Groups, single, payloadBudget, workRemaining);
	const auto oldGroupIndex = IndexSourceCommon(previous.Groups, single, payloadBudget, workRemaining);
	const auto metadataIndex = IndexSourceCommon(state.Detached, animatorKey, payloadBudget, workRemaining);
	const auto payloadIndex =
		IndexSourceCommon(state.DetachedValues, payloadKey, payloadBudget, workRemaining);
	const auto oldMetadataIndex =
		IndexSourceCommon(before.Detached, animatorKey, payloadBudget, workRemaining);
	const auto oldPayloadIndex =
		IndexSourceCommon(before.DetachedValues, payloadKey, payloadBudget, workRemaining);
	if (!sourceIndex || !ownerIndex || !nodeIndex || !oldNodeIndex || !groupIndex || !oldGroupIndex ||
		!metadataIndex || !payloadIndex || !oldMetadataIndex || !oldPayloadIndex)
		return Reject(diagnostic, "PXC common authoring indices exceed transaction bounds");
	// ordered_json can copy object members when fields grow. Resolve borrowed IDs before editing.
	std::vector<size_t> positions;
	std::vector<uint8_t> selected;
	if (!ReserveEmptyProjectionSlots(positions, desired.SourceCommonOwners.size(), payloadBudget) ||
		!ReserveEmptyProjectionSlots(selected, sources.size(), payloadBudget))
		return Reject(diagnostic, "PXC common owner order exceeds payload bounds");
	selected.resize(sources.size(), 0);
	for (const auto &owner : desired.SourceCommonOwners) {
		const auto found = FindSourceCommon(*sourceIndex, sources, {owner.SourceOwnerId, {}}, workRemaining);
		if (found == sources.end())
			return Reject(
				diagnostic, "PXC common owner order references a missing constructor", owner.NativeOwnerId
			);
		const size_t position = static_cast<size_t>(found - sources.begin());
		if (selected[position])
			return Reject(diagnostic, "PXC common owner order repeats a constructor", owner.NativeOwnerId);
		selected[position] = 1;
		positions.push_back(position);
	}
	size_t ownerPosition = 0;
	for (auto &owner : desired.SourceCommonOwners) {
		Json *source = &sources[positions[ownerPosition++]];
		if (source->value("type", "") != owner.SourceType || !owner.Active)
			return Reject(
				diagnostic, "PXC common owner requires its active source constructor", owner.NativeOwnerId
			);
		const auto old = FindSourceCommon(
			*ownerIndex, previous.SourceCommonOwners, {owner.SourceOwnerId, {}}, workRemaining
		);
		const auto nodeRow =
			FindSourceCommon(*nodeIndex, desired.Nodes, {owner.NativeOwnerId, {}}, workRemaining);
		Node *node =
			owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node && nodeRow != desired.Nodes.end()
				? &*nodeRow
				: nullptr;
		auto group = FindSourceCommon(*groupIndex, desired.Groups, {owner.NativeOwnerId, {}}, workRemaining);
		if ((!node && owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) ||
			(owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group && group == desired.Groups.end()))
			return Reject(diagnostic, "PXC common owner has no native authoring target", owner.NativeOwnerId);
		const std::string &name = node ? node->SourceDisplayName : group->Name;
		const auto oldNodeRow =
			FindSourceCommon(*oldNodeIndex, previous.Nodes, {owner.NativeOwnerId, {}}, workRemaining);
		const Node *oldNode = node && oldNodeRow != previous.Nodes.end() ? &*oldNodeRow : nullptr;
		const auto oldGroup =
			FindSourceCommon(*oldGroupIndex, previous.Groups, {owner.NativeOwnerId, {}}, workRemaining);
		if (old != previous.SourceCommonOwners.end() && owner.DisplayNamePresent == old->DisplayNamePresent &&
			((oldNode && name != oldNode->SourceDisplayName) ||
			 (!node && oldGroup != previous.Groups.end() && name != oldGroup->Name)))
			owner.DisplayNamePresent = true;
		if (owner.DisplayNamePresent)
			(*source)["name"] = name;
		else
			source->erase("name");
		if (!node &&
			(oldGroup == previous.Groups.end() || group->SourcePosition != oldGroup->SourcePosition)) {
			(*source)["x"] = group->SourcePosition.X;
			(*source)["y"] = group->SourcePosition.Y;
		}
		if (!node && (oldGroup == previous.Groups.end() ||
					  group->SourceInternalName != oldGroup->SourceInternalName)) {
			if (group->SourceInternalName.empty())
				source->erase("iname");
			else
				(*source)["iname"] = group->SourceInternalName;
		}
		if (old == previous.SourceCommonOwners.end() || owner.InstanceBase != old->InstanceBase) {
			if (owner.InstanceBase.empty())
				source->erase("instanceBase");
			else
				(*source)["instanceBase"] = owner.InstanceBase;
		}
		const auto flag = [&](const char *key, bool value, bool prior) {
			if (old != previous.SourceCommonOwners.end() && value == prior) return true;
			auto &attributes = (*source)["attri"];
			if (attributes.is_null()) {
				attributes = Json::object();
				// Adding common flags must retain group sampling values across source default shapes.
				if (!node) {
					attributes["color_depth"] = group->ColorDepth;
					attributes["interpolate"] = group->Interpolation;
					attributes["oversample"] = group->Oversample;
				}
			}
			if (!attributes.is_object())
				return Reject(diagnostic, "PXC common attributes are malformed", owner.NativeOwnerId);
			attributes[key] = value;
			return true;
		};
		if (!flag(
				"show_update_trigger",
				owner.ShowUpdateTrigger,
				old == previous.SourceCommonOwners.end() ? false : old->ShowUpdateTrigger
			) ||
			!flag(
				"outp_meta", owner.OutMeta, old == previous.SourceCommonOwners.end() ? false : old->OutMeta
			) ||
			!flag(
				"update_graph",
				owner.UpdateGraph,
				old == previous.SourceCommonOwners.end() ? true : old->UpdateGraph
			))
			return false;

		const auto metadata = FindSourceCommon(
			*metadataIndex,
			state.Detached,
			{owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort},
			workRemaining
		);
		const auto payload = FindSourceCommon(
			*payloadIndex,
			state.DetachedValues,
			{owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort},
			workRemaining
		);
		if (metadata == state.Detached.end() || payload == state.DetachedValues.end() ||
			metadata->OriginalPort != "pxcx.update_in_trigger" || metadata->Type != ValueType::Boolean ||
			owner.UpdateAnimatorOwnerId != owner.SourceOwnerId || (payload->Fixed && !payload->Keys.empty()))
			return Reject(
				diagnostic, "PXC common writer reference is not a local Trigger animator", owner.NativeOwnerId
			);
		if (!BoundSourceCommonWriter(owner, *metadata, *payload, payloadBudget))
			return Reject(
				diagnostic, "PXC common authoring exceeds its transaction payload limit", owner.NativeOwnerId
			);
		if (old != previous.SourceCommonOwners.end() && previous.SourceAnimators &&
			owner.UpdateOverrideInstance == old->UpdateOverrideInstance &&
			owner.UpdateExpression == old->UpdateExpression) {
			const auto oldMetadata = FindSourceCommon(
				*oldMetadataIndex,
				before.Detached,
				{old->UpdateAnimatorOwnerId, old->UpdateAnimatorPort},
				workRemaining
			);
			const auto oldPayload = FindSourceCommon(
				*oldPayloadIndex,
				before.DetachedValues,
				{old->UpdateAnimatorOwnerId, old->UpdateAnimatorPort},
				workRemaining
			);
			if (oldMetadata != before.Detached.end() && oldPayload != before.DetachedValues.end() &&
				*metadata == *oldMetadata && *payload == *oldPayload)
				continue;
		}
		auto &inspect = (*source)["inspectInputs"];
		if (inspect.is_null()) inspect = Json::array();
		if (!inspect.is_array())
			return Reject(diagnostic, "PXC common inspector inputs are malformed", owner.NativeOwnerId);
		while (inspect.size() < 3)
			inspect.push_back(Json::object());
		auto &update = inspect[2];
		if (!update.is_object())
			return Reject(diagnostic, "PXC common Update record is malformed", owner.NativeOwnerId);
		Json local = update;
		local.erase("from_node");
		local.erase("from_index");
		local.erase("from_tag");
		local["global_use"] = false;
		Document animator;
		Node cursor;
		cursor.Id = owner.UpdateAnimatorOwnerId;
		if (metadata->Writer == GroupSubtypeAnimator::Animated)
			cursor.SourceAnimatedInputs.push_back(owner.UpdateAnimatorPort);
		else if (metadata->Writer != GroupSubtypeAnimator::Static)
			return Reject(diagnostic, "PXC common writer mode is unsupported", owner.NativeOwnerId);
		if (payload->Fixed) cursor.Values.push_back({owner.UpdateAnimatorPort, *payload->Fixed});
		animator.Keyframes = payload->Keys;
		if (metadata->Track) animator.Tracks.push_back(*metadata->Track);
		if (payload->Keys.empty() && !payload->Fixed) {
			local["r"] = Json::array();
			local["anim"] = metadata->Writer == GroupSubtypeAnimator::Animated;
		} else if (!AuthoredKeyRecord(animator, cursor, owner.UpdateAnimatorPort, local, diagnostic))
			return false;
		for (const auto field : {"r", "anim", "on_end", "loop_range"}) {
			if (local.contains(field))
				update[field] = local[field];
			else
				update.erase(field);
		}
		if (metadata->Track && payload->Keys.empty() && !payload->Fixed) {
			constexpr std::string_view ends[]{"hold", "loop", "ping", "wrap"};
			const auto end = std::find(std::begin(ends), std::end(ends), metadata->Track->End);
			if (end == std::end(ends))
				return Reject(diagnostic, "PXC common writer end mode is unsupported", owner.NativeOwnerId);
			update["on_end"] = size_t(end - std::begin(ends));
			update["loop_range"] = metadata->Track->LoopRange;
		}
		if (!update.contains("attri")) update["attri"] = Json::object();
		if (!update["attri"].is_object())
			return Reject(diagnostic, "PXC common Update attributes are malformed", owner.NativeOwnerId);
		update["attri"]["override_instance"] = owner.UpdateOverrideInstance;
		if (owner.UpdateExpression) {
			if (owner.UpdateExpression->Port != "pxcx.update_in_trigger")
				return Reject(diagnostic, "PXC common expression port is invalid", owner.NativeOwnerId);
			update["global_key"] = owner.UpdateExpression->Code;
			update["global_use"] = owner.UpdateExpression->Enabled;
		} else {
			update.erase("global_key");
			update.erase("global_use");
		}
	}
	// Preserve the single source dispatch order across native node/group storage partitions.
	auto &rows = root["nodes"].get_ref<Json::array_t &>();
	Json::array_t ordered;
	if (!ReserveEmptyProjectionSlots(ordered, rows.size(), payloadBudget))
		return Reject(diagnostic, "PXC common owner order exceeds payload bounds");
	// Source identities were resolved before editing; row movement uses only their positions.
	for (const auto position : positions)
		ordered.push_back(std::move(rows[position]));
	for (auto &row : rows)
		if (!row.is_null()) ordered.push_back(std::move(row));
	rows = std::move(ordered);
	return true;
} catch (const CommonLookupWorkExceeded &) {
	return Reject(diagnostic, "PXC common authoring exceeds its transaction work limit");
}

bool HasOnlyCommonDetached(const Document &document) {
	if (!document.SourceAnimators) return true;
	const auto &state = *document.SourceAnimators;
	if (state.Detached.size() != document.SourceCommonOwners.size() ||
		state.DetachedValues.size() != state.Detached.size())
		return false;
	for (const auto &metadata : state.Detached) {
		if (metadata.OriginalPort != "pxcx.update_in_trigger" ||
			std::none_of(
				document.SourceCommonOwners.begin(),
				document.SourceCommonOwners.end(),
				[&](const auto &owner) {
					return owner.UpdateAnimatorOwnerId == metadata.OwnerId &&
						   owner.UpdateAnimatorPort == metadata.Id;
				}
			))
			return false;
	}
	for (const auto &payload : state.DetachedValues)
		if (std::none_of(state.Detached.begin(), state.Detached.end(), [&](const auto &metadata) {
				return metadata.OwnerId == payload.NodeId && metadata.Id == payload.Port;
			}))
			return false;
	return true;
}

// Fresh PXC imports allocate physical animator IDs. Compare common writers by source owner only.
bool RebaseSourceCommonComparison(Document &desired, Document &foreign, Diagnostic &diagnostic) {
	if (!HasOnlyCommonDetached(desired) || !HasOnlyCommonDetached(foreign))
		return Reject(
			diagnostic, "PXC cannot reconstruct captured animator generations; save the native document"
		);
	if (desired.SourceCommonOwners.empty() && foreign.SourceCommonOwners.empty()) {
		desired.SourceAnimators = {};
		foreign.SourceAnimators = {};
		return true;
	}
	if (!desired.SourceAnimators || !foreign.SourceAnimators)
		return Reject(diagnostic, "PXC common writer state is absent after reconstruction");
	auto &wanted = *desired.SourceAnimators;
	auto &actual = *foreign.SourceAnimators;
	for (auto &owner : desired.SourceCommonOwners) {
		const auto saved = std::find_if(
			foreign.SourceCommonOwners.begin(), foreign.SourceCommonOwners.end(), [&](const auto &record) {
				return record.SourceOwnerId == owner.SourceOwnerId;
			}
		);
		if (saved == foreign.SourceCommonOwners.end())
			return Reject(
				diagnostic, "PXC common constructor is absent after reconstruction", owner.NativeOwnerId
			);
		const auto metadata =
			std::find_if(wanted.Detached.begin(), wanted.Detached.end(), [&](const auto &record) {
				return record.OwnerId == owner.UpdateAnimatorOwnerId && record.Id == owner.UpdateAnimatorPort;
			});
		const auto payload =
			std::find_if(wanted.DetachedValues.begin(), wanted.DetachedValues.end(), [&](const auto &record) {
				return record.NodeId == owner.UpdateAnimatorOwnerId &&
					   record.Port == owner.UpdateAnimatorPort;
			});
		const auto savedPayload =
			std::find_if(actual.DetachedValues.begin(), actual.DetachedValues.end(), [&](const auto &record) {
				return record.NodeId == saved->UpdateAnimatorOwnerId &&
					   record.Port == saved->UpdateAnimatorPort;
			});
		if (metadata == wanted.Detached.end() || payload == wanted.DetachedValues.end() ||
			savedPayload == actual.DetachedValues.end() || payload->Keys.size() != savedPayload->Keys.size())
			return Reject(
				diagnostic, "PXC common writer shape changes during reconstruction", owner.NativeOwnerId
			);
		metadata->Id = saved->UpdateAnimatorPort;
		payload->Port = saved->UpdateAnimatorPort;
		if (metadata->Track) metadata->Track->Port = saved->UpdateAnimatorPort;
		for (size_t index = 0; index < payload->Keys.size(); ++index) {
			auto &key = payload->Keys[index];
			key.Port = saved->UpdateAnimatorPort;
			// The inverse codec checked each nonempty source identity against this owner's raw key rows.
			key.SourceKeyId = savedPayload->Keys[index].SourceKeyId;
		}
		owner.UpdateAnimatorPort = saved->UpdateAnimatorPort;
	}
	wanted.Bindings.clear();
	actual.Bindings.clear();
	std::sort(wanted.Detached.begin(), wanted.Detached.end(), [](const auto &a, const auto &b) {
		return a.Id < b.Id;
	});
	std::sort(actual.Detached.begin(), actual.Detached.end(), [](const auto &a, const auto &b) {
		return a.Id < b.Id;
	});
	std::sort(wanted.DetachedValues.begin(), wanted.DetachedValues.end(), [](const auto &a, const auto &b) {
		return std::tie(a.NodeId, a.Port) < std::tie(b.NodeId, b.Port);
	});
	std::sort(actual.DetachedValues.begin(), actual.DetachedValues.end(), [](const auto &a, const auto &b) {
		return std::tie(a.NodeId, a.Port) < std::tie(b.NodeId, b.Port);
	});
	return true;
}

// An actual newly exported source constructor acquires its cold local animator on reopen.
bool AdoptNewSourceCommonOwners(
	Document &desired,
	const Document &foreign,
	const Document &previous,
	Diagnostic &diagnostic,
	size_t &payloadBudget
) try {
	if (foreign.SourceCommonOwners.size() > Limits::MaximumSourceCommonOwners ||
		desired.SourceCommonOwners.size() > Limits::MaximumSourceCommonOwners)
		return Reject(diagnostic, "PXC new common constructor count exceeds bounds");
	uint64_t workRemaining = bake::PxcxLimits::MaximumLinks * 16;
	const auto ownerKey = [](const auto &owner) { return CommonLookupKey{owner.SourceOwnerId, {}}; };
	auto existing = IndexSourceCommon(desired.SourceCommonOwners, ownerKey, payloadBudget, workRemaining);
	if (!existing) return Reject(diagnostic, "PXC new common constructor comparison exceeds work bounds");
	size_t newCount = 0;
	for (const auto &owner : foreign.SourceCommonOwners)
		newCount += FindSourceCommon(
						*existing, desired.SourceCommonOwners, {owner.SourceOwnerId, {}}, workRemaining
					) == desired.SourceCommonOwners.end();
	if (!newCount) return true;
	if (newCount > Limits::MaximumSourceCommonOwners - desired.SourceCommonOwners.size())
		return Reject(diagnostic, "PXC new common constructor count exceeds bounds");
	const size_t capacity = desired.SourceCommonOwners.size() + newCount;
	if (desired.SourceCommonOwners.capacity() < capacity) {
		if (!Spend(capacity * sizeof(SourceCommonOwnerRecord), payloadBudget))
			return Reject(diagnostic, "PXC new common constructor storage exceeds payload bounds");
		// Refresh borrowed identity views after relocating native owner rows.
		existing.reset();
		desired.SourceCommonOwners.reserve(capacity);
		if (!Spend(
				(desired.SourceCommonOwners.capacity() - capacity) * sizeof(SourceCommonOwnerRecord),
				payloadBudget
			))
			return Reject(diagnostic, "PXC new common constructor storage exceeds payload bounds");
		existing = IndexSourceCommon(desired.SourceCommonOwners, ownerKey, payloadBudget, workRemaining);
		if (!existing) return Reject(diagnostic, "PXC new common constructor indices exceed bounds");
	}
	for (const auto &owner : foreign.SourceCommonOwners) {
		if (FindSourceCommon(
				*existing, desired.SourceCommonOwners, {owner.SourceOwnerId, {}}, workRemaining
			) != desired.SourceCommonOwners.end())
			continue;
		const uint64_t newWork = previous.Nodes.size() + previous.Groups.size() +
								 (foreign.SourceAnimators ? foreign.SourceAnimators->Detached.size() +
																foreign.SourceAnimators->DetachedValues.size()
														  : 0);
		if (newWork > workRemaining)
			return Reject(diagnostic, "PXC new common constructor comparison exceeds work bounds");
		workRemaining -= newWork;
		const bool oldNode = std::any_of(previous.Nodes.begin(), previous.Nodes.end(), [&](const auto &node) {
			return CompareSourceCommonKey({node.Id, {}}, {owner.NativeOwnerId, {}}, workRemaining) == 0;
		});
		const bool oldGroup =
			std::any_of(previous.Groups.begin(), previous.Groups.end(), [&](const auto &group) {
				return CompareSourceCommonKey({group.Id, {}}, {owner.NativeOwnerId, {}}, workRemaining) == 0;
			});
		if (oldNode || oldGroup)
			return Reject(
				diagnostic, "PXC existing common owner registry cannot be omitted", owner.NativeOwnerId
			);
		if (!foreign.SourceAnimators)
			return Reject(diagnostic, "PXC new common constructor writer is absent", owner.NativeOwnerId);
		const auto &saved = *foreign.SourceAnimators;
		const auto metadata =
			std::find_if(saved.Detached.begin(), saved.Detached.end(), [&](const auto &row) {
				return CompareSourceCommonKey(
						   {row.OwnerId, row.Id},
						   {owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort},
						   workRemaining
					   ) == 0;
			});
		const auto payload =
			std::find_if(saved.DetachedValues.begin(), saved.DetachedValues.end(), [&](const auto &row) {
				return CompareSourceCommonKey(
						   {row.NodeId, row.Port},
						   {owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort},
						   workRemaining
					   ) == 0;
			});
		if (metadata == saved.Detached.end() || payload == saved.DetachedValues.end() ||
			!payload->Keys.empty() || payload->Fixed || metadata->Writer != GroupSubtypeAnimator::Static)
			return Reject(
				diagnostic,
				"PXC new common constructor must retain its cold Trigger writer",
				owner.NativeOwnerId
			);
		if (!BoundSourceCommonWriter(owner, *metadata, *payload, payloadBudget))
			return Reject(
				diagnostic,
				"PXC new common constructor comparison exceeds payload bounds",
				owner.NativeOwnerId
			);
		if (!desired.SourceAnimators) desired.SourceAnimators.emplace();
		// Renumber the fresh reference if it collides with an existing native physical animator.
		DetachedSourceAnimator localMetadata = *metadata;
		GroupSubtypeOverlay localPayload = *payload;
		SourceCommonOwnerRecord localOwner = owner;
		uint64_t fresh = 0;
		for (;;) {
			if (desired.SourceAnimators->Detached.size() > workRemaining)
				return Reject(
					diagnostic,
					"PXC new common constructor identity search exceeds work bounds",
					owner.NativeOwnerId
				);
			workRemaining -= desired.SourceAnimators->Detached.size();
			if (!std::any_of(
					desired.SourceAnimators->Detached.begin(),
					desired.SourceAnimators->Detached.end(),
					[&](const auto &row) {
						return CompareSourceCommonKey({row.Id, {}}, {localMetadata.Id, {}}, workRemaining) ==
							   0;
					}
				))
				break;
			if (fresh > desired.SourceAnimators->Detached.size())
				return Reject(
					diagnostic, "PXC new common constructor writer identity collides", owner.NativeOwnerId
				);
			localMetadata.Id = "native:animator:" + std::to_string(fresh++);
		}
		if (localMetadata.Track) localMetadata.Track->Port = localMetadata.Id;
		localPayload.Port = localMetadata.Id;
		localOwner.UpdateAnimatorPort = localMetadata.Id;
		desired.SourceAnimators->Detached.push_back(std::move(localMetadata));
		desired.SourceAnimators->DetachedValues.push_back(std::move(localPayload));
		desired.SourceCommonOwners.push_back(std::move(localOwner));
	}
	return true;
} catch (const CommonLookupWorkExceeded &) {
	return Reject(diagnostic, "PXC new common constructor comparison exceeds work bounds");
}

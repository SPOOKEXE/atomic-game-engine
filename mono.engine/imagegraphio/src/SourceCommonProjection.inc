// Import owns the unpublished candidate; a refusal discards all partial common owner records.
bool ProjectSourceCommonOwners(
	const Json &root, imagegraph::Document &document, detail::ImportBudget &budget, std::string &failure
) {
	using namespace imagegraph;
	constexpr std::string_view updatePort = "pxcx.update_in_trigger";
	struct Binding {
		Node *NativeNode = nullptr;
		Group *NativeGroup = nullptr;
	};
	using Pair = std::pair<const std::string_view, Binding>;
	std::map<std::string_view, Binding, std::less<>, detail::ImportAllocator<Pair>> bindings(
		std::less<>{}, detail::ImportAllocator<Pair>{budget}
	);
	for (auto &node : document.Nodes)
		bindings[node.Id].NativeNode = &node;
	for (auto &group : document.Groups)
		bindings[group.Id].NativeGroup = &group;
	const auto fail = [&](std::string reason) {
		failure = std::move(reason);
		return false;
	};
	const auto boolean = [&](const Json &record, std::string_view key, bool &out) {
		const auto value = record.find(std::string(key));
		if (value == record.end()) return true;
		if (!value->is_boolean()) return fail("common source flag is not boolean: " + std::string(key));
		out = value->get<bool>();
		return true;
	};
	const auto text = [&](const Json &value, std::string &out) {
		if (!value.is_string()) return fail("common source identity or name is not text");
		const auto &name = value.get_ref<const std::string &>();
		if (name.size() > Limits::MaximumTextBytes || !AdmitNativeText(name, &budget))
			return fail("common source identity or name exceeds operation bounds");
		out = name;
		return true;
	};
	const auto &sources = root.at("nodes");
	if (!document.SourceCommonOwners.empty() || sources.size() > Limits::MaximumNodes)
		return fail("common source owner projection is not a fresh bounded candidate");
	std::set<uint64_t, std::less<>, detail::ImportAllocator<uint64_t>> usedAnimators(
		std::less<>{}, detail::ImportAllocator<uint64_t>{budget}
	);
	if (document.SourceAnimators) {
		for (const auto &metadata : document.SourceAnimators->Detached) {
			constexpr std::string_view prefix = "native:animator:";
			const std::string_view identity = metadata.Id;
			uint64_t index = 0;
			if (!identity.starts_with(prefix))
				return fail("existing physical animator identity is not native");
			const auto parsed =
				std::from_chars(identity.data() + prefix.size(), identity.data() + identity.size(), index);
			if (parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size())
				return fail("existing physical animator identity is malformed");
			usedAnimators.insert(index);
		}
	}
	uint64_t nextAnimator = 0;
	uint64_t keyCount = document.Keyframes.size();
	for (const auto &node : document.Nodes)
		if (node.SourceSeparatedVec2Animators)
			for (const auto &input : node.SourceSeparatedVec2Animators->Inputs)
				for (const auto &axis : input.Axes)
					keyCount += axis.Keys.size();
	if (document.SourceAnimators)
		for (const auto &value : document.SourceAnimators->DetachedValues) {
			keyCount += value.Keys.size();
			if (value.SeparatedVec2)
				for (const auto &axis : value.SeparatedVec2->Axes)
					keyCount += axis.Keys.size();
		}
	if (!sources.empty()) {
		if (!document.SourceAnimators) {
			if (!budget.Hold(sizeof(SourceAnimatorState)))
				return fail("common Update state exceeds operation bounds");
			document.SourceAnimators.emplace();
		}
		auto &state = *document.SourceAnimators;
		if (state.Detached.size() > Limits::MaximumArrayElements ||
			state.DetachedValues.size() > Limits::MaximumArrayElements ||
			sources.size() > Limits::MaximumArrayElements - state.Detached.size() ||
			sources.size() > Limits::MaximumArrayElements - state.DetachedValues.size())
			return fail("common source animator count exceeds bounds");
		if (!AdmitNativeSlots(state.Detached, state.Detached.size() + sources.size(), &budget) ||
			!AdmitNativeSlots(state.DetachedValues, state.DetachedValues.size() + sources.size(), &budget) ||
			!AdmitNativeSlots(document.SourceCommonOwners, sources.size(), &budget))
			return fail("common source owner storage exceeds operation bounds");
	}
	for (const auto &source : sources) {
		const auto &sourceId = source.at("id").get_ref<const std::string &>();
		const auto binding = bindings.find(sourceId);
		if (binding == bindings.end())
			return fail("common source owner has no native projection: " + sourceId);
		SourceCommonOwnerRecord owner;
		if (!text(source.at("id"), owner.SourceOwnerId) || !text(source.at("type"), owner.SourceType) ||
			!text(source.at("id"), owner.NativeOwnerId) ||
			!text(source.at("id"), owner.UpdateAnimatorOwnerId) || !AdmitNativeText(updatePort, &budget))
			return false;
		std::string animatorId;
		while (usedAnimators.contains(nextAnimator)) {
			if (nextAnimator == UINT64_MAX) return fail("physical animator identity space is exhausted");
			++nextAnimator;
		}
		animatorId = "native:animator:" + std::to_string(nextAnimator);
		usedAnimators.insert(nextAnimator);
		if (!AdmitNativeText(animatorId, &budget))
			return fail("common Update animator identity exceeds operation bounds");
		owner.UpdateAnimatorPort = animatorId;
		owner.NativeOwnerKind = binding->second.NativeNode ? SourceCommonNativeOwnerKind::Node
														   : SourceCommonNativeOwnerKind::Group;
		// The pinned serializer emits only active owners; loading has no saved active-field override.
		owner.Active = true;
		if (const auto base = source.find("instanceBase");
			base != source.end() && !text(*base, owner.InstanceBase))
			return false;
		const auto attributes = source.find("attri");
		if (attributes != source.end()) {
			if (!attributes->is_object()) return fail("common source attributes are not an object");
			if (!boolean(*attributes, "show_update_trigger", owner.ShowUpdateTrigger) ||
				!boolean(*attributes, "outp_meta", owner.OutMeta) ||
				!boolean(*attributes, "update_graph", owner.UpdateGraph))
				return false;
		}
		const auto name = source.find("name");
		owner.DisplayNamePresent = name != source.end();
		if (name != source.end()) {
			std::string display;
			if (!text(*name, display)) return false;
			if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node)
				binding->second.NativeNode->SourceDisplayName = std::move(display);
			else
				binding->second.NativeGroup->Name = std::move(display);
		}
		if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group)
			binding->second.NativeGroup->SourcePosition = {
				source.at("x").get<double>(), source.at("y").get<double>()
			};
		if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group) {
			if (const auto internal = source.find("iname");
				internal != source.end() && !text(*internal, binding->second.NativeGroup->SourceInternalName))
				return false;
		}
		const Json *update = nullptr;
		if (const auto inspect = source.find("inspectInputs"); inspect != source.end()) {
			if (!inspect->is_array()) return fail("common source inspector inputs are not an array");
			if (inspect->size() > 2) {
				update = &(*inspect)[2];
				if (!update->is_object()) return fail("common Update input is not an object");
			}
		}
		bool animated = false;
		if (update) {
			if (!boolean(*update, "anim", animated)) return false;
			if (const auto attr = update->find("attri"); attr != update->end()) {
				if (!attr->is_object()) return fail("common Update attributes are not an object");
				if (!boolean(*attr, "override_instance", owner.UpdateOverrideInstance)) return false;
			}
		}
		Node local;
		Document animation;
		CatalogueEntry entry{};
		entry.Type = "pc.group_input";
		CatalogueInput input{"parent_value", "Update", -1, "Trigger", ValueType::Any, "b 0", ""};
		if (update &&
			!CatalogueInputValue(*update, entry, input, sourceId, local, animation, failure, true, &budget))
			return false;
		if (local.Values.size() > 1 || local.SourceInputExpressions.size() > 1 || animation.Tracks.size() > 1)
			return fail("common Update input produced conflicting animator payloads");
		if (!local.SourceInputExpressions.empty()) {
			local.SourceInputExpressions.front().Port = updatePort;
			owner.UpdateExpression = std::move(local.SourceInputExpressions.front());
		}
		if (animation.Keyframes.size() > Limits::MaximumKeyframes ||
			keyCount > Limits::MaximumKeyframes - animation.Keyframes.size())
			return fail("common Update keys exceed the aggregate source key bound");
		keyCount += animation.Keyframes.size();
		DetachedSourceAnimator metadata;
		GroupSubtypeOverlay payload;
		if (!AdmitNativeText(animatorId, &budget) || !AdmitNativeText(sourceId, &budget) ||
			!AdmitNativeText(updatePort, &budget) || !AdmitNativeText(sourceId, &budget) ||
			!AdmitNativeText(animatorId, &budget))
			return fail("common Update animator identities exceed operation bounds");
		metadata.Id = animatorId;
		metadata.OwnerId = sourceId;
		metadata.OriginalPort = updatePort;
		metadata.Writer = animated ? GroupSubtypeAnimator::Animated : GroupSubtypeAnimator::Static;
		metadata.Type = ValueType::Boolean;
		metadata.ArrayClassification = false;
		if (!animation.Tracks.empty()) {
			if (!AdmitNativeText(animatorId, &budget))
				return fail("common Update track identity exceeds operation bounds");
			animation.Tracks.front().Port = animatorId;
			metadata.Track = std::move(animation.Tracks.front());
		}
		payload.NodeId = sourceId;
		payload.Port = animatorId;
		// Compact records also produce a physical frame-zero key; keep only that animator payload.
		const bool emptyTrigger =
			!update || !update->contains("r") || (update->at("r").is_array() && update->at("r").empty());
		if (!emptyTrigger && animation.Keyframes.empty() && !local.Values.empty())
			payload.Fixed = std::move(local.Values.front().Data);
		for (auto &key : animation.Keyframes) {
			if (!AdmitNativeText(animatorId, &budget))
				return fail("common Update key identity exceeds operation bounds");
			key.Port = animatorId;
		}
		payload.Keys = std::move(animation.Keyframes);
		auto &state = *document.SourceAnimators;
		state.Detached.push_back(std::move(metadata));
		state.DetachedValues.push_back(std::move(payload));
		document.SourceCommonOwners.push_back(std::move(owner));
	}
	document.FormatVersion = std::max(document.FormatVersion, 11u);
	return true;
}

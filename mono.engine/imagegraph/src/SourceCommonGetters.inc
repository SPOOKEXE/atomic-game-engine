namespace detail {
	SourceSocketDomain SourceCommonGetterDomain(SourceCommonSelector selector);
	inline std::optional<SourceCommonSelector> CommonSelector(std::string_view port) {
		if (port == "pxcx.update_in_trigger") return SourceCommonSelector::Update;
		if (port == "pxcx.updated_out_trigger") return SourceCommonSelector::Updated;
		if (port == "pxcx.metadata.0") return SourceCommonSelector::Name;
		if (port == "pxcx.metadata.1") return SourceCommonSelector::Position;
		return std::nullopt;
	}
	inline ValueType CommonSelectorType(SourceCommonSelector selector) {
		return selector == SourceCommonSelector::Name		? ValueType::Text
			   : selector == SourceCommonSelector::Position ? ValueType::Vector2
															: ValueType::Boolean;
	}
	inline SourceSocketDomain CommonSelectorDomain(SourceCommonSelector selector) {
		if (selector == SourceCommonSelector::Name)
			return {ValueType::Text, SourceValueDisplay::Default, SourceSocketKind::Text};
		if (selector == SourceCommonSelector::Position)
			return {ValueType::Vector2, SourceValueDisplay::Vector, SourceSocketKind::Float};
		return {ValueType::Boolean, SourceValueDisplay::Default, SourceSocketKind::Trigger};
	}
	inline const SourceCommonOwnerRecord *CommonOwner(const Document &document, std::string_view id) {
		const auto found = std::find_if(
			document.SourceCommonOwners.begin(), document.SourceCommonOwners.end(), [&](const auto &owner) {
				return owner.NativeOwnerId == id;
			}
		);
		return found == document.SourceCommonOwners.end() ? nullptr : &*found;
	}
	inline std::string_view CommonOwnerScope(const Document &document, const SourceCommonOwnerRecord &owner) {
		if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
			const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
				return n.Id == owner.NativeOwnerId;
			});
			return node == document.Nodes.end() ? std::string_view{} : node->GroupId;
		}
		const auto group = std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &g) {
			return g.Id == owner.NativeOwnerId;
		});
		return group == document.Groups.end() ? std::string_view{} : group->ParentId;
	}
	struct CommonNamedReference {
		const SourceCommonOwnerRecord *Owner;
		SourceCommonSelector Selector;
	};
	inline std::optional<CommonNamedReference> CommonNamed(const Document &document, std::string_view name) {
		const auto first = name.find('.');
		const auto second = first == std::string_view::npos ? first : name.find('.', first + 1);
		if (second == std::string_view::npos) return std::nullopt;
		const auto equal = [](std::string_view a, std::string_view b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i) {
				char c = a[i];
				if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
				if (c != b[i]) return false;
			}
			return true;
		};
		const auto direction = name.substr(first + 1, second - first - 1);
		const auto slot = name.substr(second + 1);
		std::optional<SourceCommonSelector> selector;
		if ((equal(direction, "input") || equal(direction, "inputs")) && equal(slot, "update"))
			selector = SourceCommonSelector::Update;
		if (equal(direction, "output") || equal(direction, "outputs")) {
			if (equal(slot, "updated"))
				selector = SourceCommonSelector::Updated;
			else if (equal(slot, "name"))
				selector = SourceCommonSelector::Name;
			else if (equal(slot, "position"))
				selector = SourceCommonSelector::Position;
		}
		if (!selector) return std::nullopt;
		const auto internal = name.substr(0, first);
		if (internal.empty()) return std::nullopt;
		for (auto owner = document.SourceCommonOwners.rbegin(); owner != document.SourceCommonOwners.rend();
			 ++owner) {
			std::string_view actual;
			if (owner->NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
				for (const auto &n : document.Nodes)
					if (n.Id == owner->NativeOwnerId) actual = n.SourceInternalName;
			} else
				for (const auto &g : document.Groups)
					if (g.Id == owner->NativeOwnerId) actual = g.SourceInternalName;
			if (actual == internal) return CommonNamedReference{&*owner, *selector};
		}
		return std::nullopt;
	}
	inline const SourceCommonRoute *CommonInputRoute(
		const Document &document, const Plan &plan, std::string_view nodeId, std::string_view port
	) {
		const auto direct = [&](std::string_view id) -> const SourceCommonRoute * {
			const auto found = std::find_if(
				plan.SourceCommonRoutes.begin(), plan.SourceCommonRoutes.end(), [&](const auto &route) {
					return !route.DestinationUpdate && route.NodeId == id && route.Port == port;
				}
			);
			return found == plan.SourceCommonRoutes.end() ? nullptr : &*found;
		};
		if (const auto *route = direct(nodeId)) return route;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
			return n.Id == nodeId;
		});
		if (node == document.Nodes.end()) return nullptr;
		const auto *owner = EffectiveInputOwner(document, *node, port);
		return owner && owner->Id != nodeId ? direct(owner->Id) : nullptr;
	}
	class CommonGetterWalk {
		const Document &Doc;
		const Plan &Compiled;
		const EvaluationRequest &Request;
		EvaluationBudget &Budget;
		EvaluationVector<std::string_view> Active;
		uint64_t Work = 0;
		Status Refuse(Diagnostic &diagnostic, Status code, std::string_view id, const char *message) {
			diagnostic = {code, std::string(id), "pxcx.update_in_trigger", message};
			return code;
		}
		bool Spend(uint64_t count, Diagnostic &diagnostic, std::string_view id = {}) {
			if (count > 64'000'000 - Work) {
				Refuse(diagnostic, Status::LimitExceeded, id, "common getter scans exceed work bounds");
				return false;
			}
			Work += count;
			return true;
		}
		Status
		Copy(const Value &source, Value &value, AllocationReservation &charge, Diagnostic &diagnostic) {
			if (!ValidRuntimeValue(source))
				return Refuse(diagnostic, Status::InvalidValue, {}, "common getter payload is invalid");
			const uint64_t bytes = RetainedPayloadBytes(source);
			if (!Spend(bytes + 1, diagnostic)) return diagnostic.Code;
			auto reservation = Budget.Reserve(bytes);
			if (!reservation)
				return Refuse(
					diagnostic, Status::LimitExceeded, {}, "common getter payload exceeds live bytes"
				);
			Value candidate = source;
			if (!reservation->Resize(RetainedPayloadBytes(candidate)))
				return Refuse(diagnostic, Status::LimitExceeded, {}, "common getter copy exceeds live bytes");
			value = std::move(candidate);
			charge = std::move(*reservation);
			return Status::Ok;
		}
		Status Held(
			std::string_view id,
			std::string_view port,
			Value &value,
			AllocationReservation &charge,
			Diagnostic &diagnostic
		) {
			const auto *outputs =
				Request.GroupRender ? &Request.GroupRender->Outputs : Request.SourceTunnelPreviousOutputs;
			if (outputs && !Spend(outputs->Nodes.size() + id.size() + port.size(), diagnostic, id))
				return diagnostic.Code;
			if (!outputs)
				return Refuse(
					diagnostic, Status::UnsupportedExecution, id, "common Update needs held producer outputs"
				);
			const auto node = std::find_if(outputs->Nodes.begin(), outputs->Nodes.end(), [&](const auto &n) {
				return n.NodeId == id;
			});
			if (node == outputs->Nodes.end())
				return Refuse(
					diagnostic,
					Status::UnsupportedExecution,
					id,
					"common Update producer has no constructor output"
				);
			if (!Spend(node->Outputs.size(), diagnostic, id)) return diagnostic.Code;
			const auto output = std::find_if(node->Outputs.begin(), node->Outputs.end(), [&](const auto &p) {
				return p.Port == port;
			});
			if (output == node->Outputs.end())
				return Refuse(
					diagnostic,
					Status::UnsupportedExecution,
					id,
					"common Update producer getter is unavailable"
				);
			if (output->Refusal) {
				diagnostic = *output->Refusal;
				return diagnostic.Code;
			}
			if (!output->Data)
				return Refuse(
					diagnostic,
					Status::UnsupportedExecution,
					id,
					"common Update producer getter is unavailable"
				);
			return Copy(*output->Data, value, charge, diagnostic);
		}
		class Names final : public PcxNameResolver {
			CommonGetterWalk &Walk;
			mutable AllocationReservation ValuesCharge;

		  public:
			explicit Names(CommonGetterWalk &walk) : Walk(walk) {}
			bool Resolve(std::string_view name, Value &value, Diagnostic &diagnostic) const override {
				if (!Walk.Spend(
						Walk.Doc.SourceCommonOwners.size() *
								uint64_t(Walk.Doc.Nodes.size() + Walk.Doc.Groups.size() + 1) +
							name.size(),
						diagnostic
					))
					return false;
				if (const auto common = CommonNamed(Walk.Doc, name)) {
					AllocationReservation charge;
					const auto status =
						Walk.Read(common->Owner->SourceOwnerId, common->Selector, value, charge, diagnostic);
					if (status == Status::Ok && !ValuesCharge.Merge(std::move(charge))) std::terminate();
					return status == Status::Ok;
				}
				const auto first = name.find('.');
				if (first == std::string_view::npos) return false;
				const auto second = name.find('.', first + 1);
				if (second == std::string_view::npos) return false;
				const auto internal = name.substr(0, first);
				const auto direction = name.substr(first + 1, second - first - 1);
				const auto slot = name.substr(second + 1);
				for (auto owner = Walk.Doc.SourceCommonOwners.rbegin();
					 owner != Walk.Doc.SourceCommonOwners.rend();
					 ++owner) {
					std::string_view actual;
					if (owner->NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
						for (const auto &n : Walk.Doc.Nodes)
							if (n.Id == owner->NativeOwnerId) actual = n.SourceInternalName;
					} else {
						for (const auto &g : Walk.Doc.Groups)
							if (g.Id == owner->NativeOwnerId) actual = g.SourceInternalName;
					}
					if (actual.empty() || actual != internal) continue;
					std::optional<SourceCommonSelector> selector;
					if ((direction == "input" || direction == "inputs") && slot == "update")
						selector = SourceCommonSelector::Update;
					if (direction == "output" || direction == "outputs") {
						if (slot == "updated")
							selector = SourceCommonSelector::Updated;
						else if (slot == "name")
							selector = SourceCommonSelector::Name;
						else if (slot == "position")
							selector = SourceCommonSelector::Position;
					}
					AllocationReservation charge;
					const auto finish = [&](Status status) {
						if (status == Status::Ok && !ValuesCharge.Merge(std::move(charge))) std::terminate();
						return status == Status::Ok;
					};
					if (selector)
						return finish(Walk.Read(owner->SourceOwnerId, *selector, value, charge, diagnostic));
					if (direction == "output" || direction == "outputs")
						return finish(Walk.Held(owner->NativeOwnerId, slot, value, charge, diagnostic));
					if ((direction == "input" || direction == "inputs") &&
						owner->NativeOwnerKind == SourceCommonNativeOwnerKind::Node)
						return finish(ReadHeldSourceCommonInput(
							Walk.Doc,
							Walk.Compiled,
							owner->NativeOwnerId,
							slot,
							Walk.Request,
							Walk.Budget,
							value,
							charge,
							diagnostic
						));
					break;
				}
				return false;
			}
		};
		Status Expression(
			const SourceCommonOwnerRecord &owner,
			bool trigger,
			Value &value,
			AllocationReservation &charge,
			Diagnostic &diagnostic
		) {
			const auto &source = *owner.UpdateExpression;
			if (!Spend(
					source.Code.size() + 4096 +
						(Request.GroupRender ? Request.GroupRender->SourceCommonInputs.size() : 0) +
						Doc.Nodes.size() + Doc.Groups.size(),
					diagnostic,
					owner.SourceOwnerId
				))
				return diagnostic.Code;
			auto workspace = Budget.Reserve(
				source.Code.size() * 8 + 4096 * sizeof(PcxInstruction) + 4 * Limits::MaximumArrayBytes
			);
			if (!workspace)
				return Refuse(
					diagnostic,
					Status::LimitExceeded,
					owner.SourceOwnerId,
					"common expression workspace exceeds live bytes"
				);
			PcxExpressionValue program;
			if (CompilePcxProgram(source.Code, program, diagnostic) != Status::Ok) return diagnostic.Code;
			bool needsMap = false, needsDisplayName = false;
			for (const auto &instruction : program.Data->Instructions)
				if (instruction.Operation == "name") {
					const auto *name = std::get_if<std::string>(&instruction.Literal);
					needsMap |= name && (*name == "self" || name->starts_with("self.") ||
										 *name == "node_values" || name->starts_with("node_values."));
					needsDisplayName |= name && *name == "node_name";
				}
			const SourceCommonInputMap *held = nullptr;
			if (Request.GroupRender) {
				const auto found = std::find_if(
					Request.GroupRender->SourceCommonInputs.begin(),
					Request.GroupRender->SourceCommonInputs.end(),
					[&](const auto &map) {
						return map.OwnerId == owner.SourceOwnerId && map.OwnerType == owner.SourceType;
					}
				);
				if (found != Request.GroupRender->SourceCommonInputs.end()) held = &*found;
			}
			if (needsMap && (!held || !held->Inputs))
				return Refuse(
					diagnostic,
					Status::UnsupportedExecution,
					owner.SourceOwnerId,
					"common expression needs an actual retained input_value_map capture"
				);
			if (needsMap && !ValidStructPayload(*held->Inputs))
				return Refuse(
					diagnostic,
					Status::InvalidValue,
					owner.SourceOwnerId,
					"retained input_value_map is invalid"
				);
			std::string_view displayName;
			if (owner.DisplayNamePresent) {
				if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
					for (const auto &node : Doc.Nodes)
						if (node.Id == owner.NativeOwnerId) displayName = node.SourceDisplayName;
				} else
					for (const auto &group : Doc.Groups)
						if (group.Id == owner.NativeOwnerId) displayName = group.Name;
			} else if (needsDisplayName)
				return Refuse(
					diagnostic,
					Status::UnsupportedExecution,
					owner.SourceOwnerId,
					"common expression needs an observed runtime display name"
				);
			auto parameterCharge = Budget.Reserve(
				4 * sizeof(AuthoredValue) + 128 + displayName.size() +
				(needsMap ? RetainedPayloadBytes(*held->Inputs) : 0)
			);
			if (!parameterCharge)
				return Refuse(
					diagnostic,
					Status::LimitExceeded,
					owner.SourceOwnerId,
					"common expression parameters exceed live bytes"
				);
			StructValue map;
			if (needsMap) map = *held->Inputs;
			const std::array parameters{
				AuthoredValue{"name", std::string{"Update"}},
				AuthoredValue{"node_name", std::string{displayName}},
				AuthoredValue{"value", trigger},
				AuthoredValue{"node_values", std::move(map)}
			};
			Names names(*this);
			PcxExecutionContext context{Request, parameters};
			context.Names = &names;
			context.Timeline = Doc.Timeline ? &*Doc.Timeline : nullptr;
			if (Doc.Project)
				context.ProjectDimension = {
					double(Doc.Project->SurfaceWidth), double(Doc.Project->SurfaceHeight)
				};
			context.MaximumBytes = std::min(Budget.Available(), Limits::MaximumArrayBytes);
			PcxExecutionResult result;
			if (ExecutePcxExpression(program, context, result, diagnostic) != Status::Ok)
				return diagnostic.Code;
			return Copy(result.Data, value, charge, diagnostic);
		}

	  public:
		CommonGetterWalk(
			const Document &doc, const Plan &plan, const EvaluationRequest &request, EvaluationBudget &budget
		)
			: Doc(doc), Compiled(plan), Request(request), Budget(budget),
			  Active(EvaluationAllocator<std::string_view>{budget}) {}
		Status Read(
			std::string_view id,
			SourceCommonSelector selector,
			Value &value,
			AllocationReservation &charge,
			Diagnostic &diagnostic
		) {
			if (!Spend(Doc.SourceCommonOwners.size() + id.size() + 1, diagnostic, id)) return diagnostic.Code;
			const auto foundOwner = std::find_if(
				Doc.SourceCommonOwners.begin(), Doc.SourceCommonOwners.end(), [&](const auto &record) {
					return record.SourceOwnerId == id;
				}
			);
			const auto *owner = foundOwner == Doc.SourceCommonOwners.end() ? nullptr : &*foundOwner;
			if (!owner)
				return Refuse(diagnostic, Status::UnknownNode, id, "common getter owner is not registered");
			if (selector != SourceCommonSelector::Update) {
				if (!Request.SourceCommon)
					return Refuse(
						diagnostic,
						Status::UnsupportedExecution,
						id,
						"common output needs an initialized socket session"
					);
				if (!Spend(Request.SourceCommon->Owners.size(), diagnostic, id)) return diagnostic.Code;
				const auto state = std::find_if(
					Request.SourceCommon->Owners.begin(),
					Request.SourceCommon->Owners.end(),
					[&](const auto &s) {
						return s.OwnerId == owner->SourceOwnerId && s.OwnerType == owner->SourceType;
					}
				);
				if (state == Request.SourceCommon->Owners.end())
					return Refuse(
						diagnostic,
						Status::InvalidValue,
						id,
						"common output owner identity changed without reconstruction"
					);
				if (selector == SourceCommonSelector::Updated)
					return Copy(Value{state->Updated}, value, charge, diagnostic);
				if (selector == SourceCommonSelector::Name) {
					if (!Spend(state->Name.size(), diagnostic, id)) return diagnostic.Code;
					auto nameStorage = Budget.Reserve(std::max(state->Name.size(), std::string{}.capacity()));
					if (!nameStorage)
						return Refuse(
							diagnostic, Status::LimitExceeded, id, "common Name getter exceeds live bytes"
						);
					Value named{state->Name};
					if (!nameStorage->Resize(RetainedPayloadBytes(named)))
						return Refuse(
							diagnostic,
							Status::LimitExceeded,
							id,
							"common Name getter copy exceeds live bytes"
						);
					value = std::move(named);
					charge = std::move(*nameStorage);
					return Status::Ok;
				}
				if (selector == SourceCommonSelector::Position)
					return Copy(Value{state->Position}, value, charge, diagnostic);
				return Refuse(diagnostic, Status::UnknownPort, id, "common output selector is invalid");
			}
			if (!Spend(
					Active.size() + Compiled.SourceCommonRoutes.size() + Compiled.ResolvedInputs.size(),
					diagnostic,
					id
				))
				return diagnostic.Code;
			if (std::find(Active.begin(), Active.end(), owner->SourceOwnerId) != Active.end())
				return Refuse(diagnostic, Status::Cycle, id, "common Update getters form a cycle");
			Active.push_back(owner->SourceOwnerId);
			const auto leave = [&](Status status) {
				Active.pop_back();
				return status;
			};
			for (const auto &route : Compiled.SourceCommonRoutes)
				if (route.DestinationUpdate && route.NodeId == owner->NativeOwnerId) {
					if (route.Selector == SourceCommonSelector::HeldOutput)
						return leave(Held(route.OwnerId, route.SourcePort, value, charge, diagnostic));
					return leave(Read(route.OwnerId, route.Selector, value, charge, diagnostic));
				}
			for (const auto &input : Compiled.ResolvedInputs)
				if (input.NodeId == owner->NativeOwnerId && input.Port == "pxcx.update_in_trigger")
					return leave(Copy(input.Data, value, charge, diagnostic));
			if (!owner->UpdateOverrideInstance && !owner->InstanceBase.empty())
				return leave(Read(owner->InstanceBase, selector, value, charge, diagnostic));
			const SourceAnimatorState *animators = Request.SourceCommonAnimators
													   ? Request.SourceCommonAnimators
												   : Doc.SourceAnimators ? &*Doc.SourceAnimators
																		 : nullptr;
			if (!animators)
				return leave(
					Refuse(diagnostic, Status::InvalidValue, id, "common Update has no local animator owner")
				);
			if (!Spend(animators->Detached.size() + animators->DetachedValues.size(), diagnostic, id))
				return leave(diagnostic.Code);
			const auto metadata =
				std::find_if(animators->Detached.begin(), animators->Detached.end(), [&](const auto &a) {
					return a.OwnerId == owner->UpdateAnimatorOwnerId && a.Id == owner->UpdateAnimatorPort;
				});
			const auto payload = std::find_if(
				animators->DetachedValues.begin(), animators->DetachedValues.end(), [&](const auto &a) {
					return a.NodeId == owner->UpdateAnimatorOwnerId && a.Port == owner->UpdateAnimatorPort;
				}
			);
			if (metadata == animators->Detached.end() || payload == animators->DetachedValues.end())
				return leave(
					Refuse(diagnostic, Status::InvalidValue, id, "common Update animator identity is missing")
				);
			bool trigger = false;
			if (metadata->Writer == GroupSubtypeAnimator::Animated && !payload->Keys.empty()) {
				if (Request.Subframe != 0 || Request.NegativeFrame)
					return leave(Refuse(
						diagnostic,
						Status::UnsupportedExecution,
						id,
						"common Trigger getter needs a nonnegative integer frame"
					));
				if (!Spend(payload->Keys.size(), diagnostic, id)) return leave(diagnostic.Code);
				trigger = std::any_of(payload->Keys.begin(), payload->Keys.end(), [&](const auto &key) {
					return !key.NegativeFrame && key.Subframe == 0 && key.Tick == Request.Tick;
				});
			}
			if (owner->UpdateExpression && owner->UpdateExpression->Enabled &&
				!owner->UpdateExpression->Code.empty())
				return leave(Expression(*owner, trigger, value, charge, diagnostic));
			return leave(Copy(Value{trigger}, value, charge, diagnostic));
		}
	};
	Status ReadSourceCommonGetter(
		const Document &document,
		const Plan &plan,
		std::string_view ownerId,
		SourceCommonSelector selector,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		Value &value,
		AllocationReservation &charge,
		Diagnostic &diagnostic
	) {
		try {
			CommonGetterWalk walk(document, plan, request, budget);
			return walk.Read(ownerId, selector, value, charge, diagnostic);
		} catch (const std::bad_alloc &) {
			diagnostic = {
				Status::LimitExceeded, std::string(ownerId), {}, "common getter allocation refused"
			};
			return diagnostic.Code;
		}
	}
}

namespace detail {
	SourceSocketDomain SourceCommonGetterDomain(SourceCommonSelector selector) {
		return CommonSelectorDomain(selector);
	}
}

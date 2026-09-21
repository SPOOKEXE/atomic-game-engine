// Transactional ownership for control-surface hook rows.

#include <engine/control/HookRegistry.hpp>
#include <engine/control/Surface.hpp>

#include <algorithm>
#include <exception>
#include <unordered_map>
#include <utility>

namespace engine::control {

	struct HookRegistration::Impl {
		std::vector<Tool> Tools;
		std::vector<Resource> Resources;
		std::vector<Prompt> Prompts;
		std::string Failure;
		std::function<bool()> Drain;
	};

	struct HookRegistryState {
		struct Activation {
			HookStatus Status;
			bool Builtin = false;
			size_t Guards = 0;
			std::function<bool()> Drain;
		};

		Surface *Owner = nullptr;
		uint64_t NextGeneration = 1;
		uint64_t ControlGeneration = 0;
		bool Reaping = false;
		std::vector<Activation> Activations;
		std::unordered_map<std::string, std::pair<std::string, uint64_t>> ToolOwners;
		std::unordered_map<std::string, std::pair<std::string, uint64_t>> ResourceOwners;
		std::unordered_map<std::string, std::pair<std::string, uint64_t>> PromptOwners;
	};

	namespace {
		HookRegistryState::Activation *
		Find(HookRegistryState &state, std::string_view id, uint64_t generation) {
			const auto found = std::find_if(
				state.Activations.begin(), state.Activations.end(), [id, generation](const auto &activation) {
					return activation.Status.Descriptor.Id == id &&
						   activation.Status.Generation == generation;
				}
			);
			return found == state.Activations.end() ? nullptr : &*found;
		}

		const HookRegistryState::Activation *FindActive(const HookRegistryState &state, std::string_view id) {
			const auto found = std::find_if(
				state.Activations.begin(), state.Activations.end(), [id](const auto &activation) {
					return activation.Status.Descriptor.Id == id &&
						   activation.Status.State == HookState::Active;
				}
			);
			return found == state.Activations.end() ? nullptr : &*found;
		}

		const HookRegistryState::Activation *FindNamed(const HookRegistryState &state, std::string_view id) {
			const auto found = std::find_if(
				state.Activations.begin(), state.Activations.end(), [id](const auto &activation) {
					return activation.Status.Descriptor.Id == id;
				}
			);
			return found == state.Activations.end() ? nullptr : &*found;
		}

		bool HasActiveDependent(const HookRegistryState &state, std::string_view id) {
			return std::any_of(
				state.Activations.begin(), state.Activations.end(), [id](const auto &candidate) {
					return candidate.Status.State != HookState::Inactive &&
						   std::find(
							   candidate.Status.Descriptor.Dependencies.begin(),
							   candidate.Status.Descriptor.Dependencies.end(),
							   id
						   ) != candidate.Status.Descriptor.Dependencies.end();
				}
			);
		}

		bool Drained(const HookRegistryState::Activation &activation) {
			if (!activation.Drain) return true;
			try {
				return activation.Drain();
			} catch (...) {
				return false;
			}
		}

		bool Has(std::span<const Tool> rows, std::string_view name) {
			return std::any_of(rows.begin(), rows.end(), [name](const Tool &row) {
				return row.Name == name;
			});
		}

		bool Has(std::span<const Resource> rows, std::string_view uri) {
			return std::any_of(rows.begin(), rows.end(), [uri](const Resource &row) {
				return row.Uri == uri;
			});
		}

		bool Has(std::span<const Prompt> rows, std::string_view name) {
			return std::any_of(rows.begin(), rows.end(), [name](const Prompt &row) {
				return row.Name == name;
			});
		}
	}

	HookLease::HookLease(std::weak_ptr<HookRegistryState> state, std::string id, uint64_t generation)
		: State(std::move(state)), Id_(std::move(id)), Generation_(generation) {}

	HookLease::~HookLease() {
		Close();
	}

	HookLease::HookLease(HookLease &&other) noexcept
		: State(std::move(other.State)), Id_(std::move(other.Id_)),
		  Generation_(std::exchange(other.Generation_, 0)) {}

	HookLease &HookLease::operator=(HookLease &&other) noexcept {
		if (this != &other) {
			Close();
			State = std::move(other.State);
			Id_ = std::move(other.Id_);
			Generation_ = std::exchange(other.Generation_, 0);
		}
		return *this;
	}

	void HookLease::Close() {
		const std::shared_ptr<HookRegistryState> state = State.lock();
		if (state == nullptr || state->Owner == nullptr || Generation_ == 0) return;
		state->Owner->Hooks().Close(Id_, Generation_);
	}

	bool HookLease::IsValid() const {
		const std::shared_ptr<HookRegistryState> state = State.lock();
		return state != nullptr && state->Owner != nullptr && Find(*state, Id_, Generation_) != nullptr;
	}

	std::string_view HookLease::Id() const {
		return Id_;
	}

	uint64_t HookLease::Generation() const {
		return Generation_;
	}

	HookRegistration::HookRegistration() : State(std::make_unique<Impl>()) {}

	HookRegistration::HookRegistration(HookRegistration &&) noexcept = default;
	HookRegistration &HookRegistration::operator=(HookRegistration &&) noexcept = default;
	HookRegistration::~HookRegistration() = default;

	bool HookRegistration::Add(Tool tool) {
		if (!State->Failure.empty()) return false;
		if (tool.Name.empty() || Has(State->Tools, tool.Name)) {
			State->Failure = "hook activation has a duplicate or empty tool name";
			return false;
		}
		State->Tools.push_back(std::move(tool));
		return true;
	}

	bool HookRegistration::Add(Resource resource) {
		if (!State->Failure.empty()) return false;
		if (resource.Uri.empty() || Has(State->Resources, resource.Uri)) {
			State->Failure = "hook activation has a duplicate or empty resource URI";
			return false;
		}
		State->Resources.push_back(std::move(resource));
		return true;
	}

	bool HookRegistration::Add(Prompt prompt) {
		if (!State->Failure.empty()) return false;
		if (prompt.Name.empty() || Has(State->Prompts, prompt.Name)) {
			State->Failure = "hook activation has a duplicate or empty prompt name";
			return false;
		}
		State->Prompts.push_back(std::move(prompt));
		return true;
	}

	std::string_view HookRegistration::Failure() const {
		return State->Failure;
	}

	void HookRegistration::SetDrain(std::function<bool()> drained) {
		State->Drain = std::move(drained);
	}

	HookRegistry::HookRegistry(Surface &surface) : State(std::make_shared<HookRegistryState>()) {
		State->Owner = &surface;
	}

	HookRegistry::~HookRegistry() {
		State->Owner = nullptr;
	}

	HookLease
	HookRegistry::Activate(HookDescriptor descriptor, const HookInstaller &installer, std::string &failure) {
		return ActivateImpl(std::move(descriptor), installer, failure, false);
	}

	HookLease HookRegistry::ActivateBuiltin(
		HookDescriptor descriptor, const HookInstaller &installer, std::string &failure
	) {
		return ActivateImpl(std::move(descriptor), installer, failure, true);
	}

	HookLease HookRegistry::ActivateImpl(
		HookDescriptor descriptor, const HookInstaller &installer, std::string &failure, bool builtin
	) {
		failure.clear();
		if (descriptor.Id.empty() || descriptor.Revision.empty()) {
			failure = "hook id and revision must not be empty";
			return {};
		}
		if (FindNamed(*State, descriptor.Id) != nullptr) {
			failure = "hook is already active or draining: " + descriptor.Id;
			return {};
		}
		for (const std::string &dependency : descriptor.Dependencies) {
			if (FindActive(*State, dependency) == nullptr) {
				failure = "hook dependency is not active: " + dependency;
				return {};
			}
		}

		HookRegistration registration;
		try {
			if (installer) installer(registration);
		} catch (const std::exception &exception) {
			failure = exception.what();
			return {};
		}
		if (!registration.Failure().empty()) {
			failure = std::string(registration.Failure());
			return {};
		}

		const auto collides = [&](std::string_view name, const auto &rows, const auto &owned) {
			(void)owned;
			return Has(rows, name);
		};
		for (const Tool &tool : registration.State->Tools) {
			if (collides(tool.Name, State->Owner->Registered(), State->ToolOwners)) {
				const auto owner = State->ToolOwners.find(tool.Name);
				failure =
					"hook " + descriptor.Id + " cannot publish tool name collision: " + tool.Name +
					" is owned by " +
					(owner == State->ToolOwners.end() ? std::string("the surface") : owner->second.first);
				return {};
			}
		}
		for (const Resource &resource : registration.State->Resources) {
			if (collides(resource.Uri, State->Owner->Readable(), State->ResourceOwners)) {
				const auto owner = State->ResourceOwners.find(resource.Uri);
				failure =
					"hook " + descriptor.Id + " cannot publish resource URI collision: " + resource.Uri +
					" is owned by " +
					(owner == State->ResourceOwners.end() ? std::string("the surface") : owner->second.first);
				return {};
			}
		}
		for (const Prompt &prompt : registration.State->Prompts) {
			if (collides(prompt.Name, State->Owner->Prompted(), State->PromptOwners)) {
				const auto owner = State->PromptOwners.find(prompt.Name);
				failure =
					"hook " + descriptor.Id + " cannot publish prompt name collision: " + prompt.Name +
					" is owned by " +
					(owner == State->PromptOwners.end() ? std::string("the surface") : owner->second.first);
				return {};
			}
		}

		const uint64_t generation = State->NextGeneration++;
		HookRegistryState::Activation activation;
		activation.Status = {
			.Descriptor = std::move(descriptor),
			.State = HookState::Active,
			.Generation = generation,
			.Tools = {},
			.Resources = {},
			.Prompts = {},
		};
		activation.Builtin = builtin;
		activation.Drain = std::move(registration.State->Drain);
		HookLease lease(State, activation.Status.Descriptor.Id, generation);
		std::vector<std::string> tools, resources, prompts;
		try {
			tools.reserve(registration.State->Tools.size());
			resources.reserve(registration.State->Resources.size());
			prompts.reserve(registration.State->Prompts.size());
			for (const Tool &tool : registration.State->Tools)
				tools.push_back(tool.Name);
			for (const Resource &resource : registration.State->Resources)
				resources.push_back(resource.Uri);
			for (const Prompt &prompt : registration.State->Prompts)
				prompts.push_back(prompt.Name);
			activation.Status.Tools = tools;
			activation.Status.Resources = resources;
			activation.Status.Prompts = prompts;
			State->Activations.reserve(State->Activations.size() + 1);

			size_t installedTools = 0, installedResources = 0, installedPrompts = 0;
			for (Tool &tool : registration.State->Tools) {
				State->ToolOwners.emplace(
					tools[installedTools], std::pair{activation.Status.Descriptor.Id, generation}
				);
				State->Owner->InstallHookTool(
					std::move(tool), activation.Status.Descriptor.Id, generation, true
				);
				++installedTools;
			}
			for (Resource &resource : registration.State->Resources) {
				State->ResourceOwners.emplace(
					resources[installedResources], std::pair{activation.Status.Descriptor.Id, generation}
				);
				State->Owner->InstallHookResource(
					std::move(resource), activation.Status.Descriptor.Id, generation, true
				);
				++installedResources;
			}
			for (Prompt &prompt : registration.State->Prompts) {
				State->PromptOwners.emplace(
					prompts[installedPrompts], std::pair{activation.Status.Descriptor.Id, generation}
				);
				State->Owner->InstallHookPrompt(
					std::move(prompt), activation.Status.Descriptor.Id, generation, true
				);
				++installedPrompts;
			}
			State->Activations.push_back(std::move(activation));
		} catch (const std::exception &exception) {
			for (const std::string &name : tools)
				if (State->Owner->Registered().end() !=
					std::find_if(
						State->Owner->Registered().begin(),
						State->Owner->Registered().end(),
						[&name](const Tool &tool) { return tool.Name == name; }
					))
					State->Owner->RemoveHookTool(name, activation.Status.Descriptor.Id, generation);
			for (const std::string &uri : resources)
				if (State->Owner->Readable().end() !=
					std::find_if(
						State->Owner->Readable().begin(),
						State->Owner->Readable().end(),
						[&uri](const Resource &resource) { return resource.Uri == uri; }
					))
					State->Owner->RemoveHookResource(uri);
			for (const std::string &name : prompts)
				if (State->Owner->Prompted().end() !=
					std::find_if(
						State->Owner->Prompted().begin(),
						State->Owner->Prompted().end(),
						[&name](const Prompt &prompt) { return prompt.Name == name; }
					))
					State->Owner->RemoveHookPrompt(name);
			const std::pair owner{activation.Status.Descriptor.Id, generation};
			std::erase_if(State->ToolOwners, [&owner](const auto &entry) { return entry.second == owner; });
			std::erase_if(State->ResourceOwners, [&owner](const auto &entry) {
				return entry.second == owner;
			});
			std::erase_if(State->PromptOwners, [&owner](const auto &entry) { return entry.second == owner; });
			failure = exception.what();
			return {};
		}
		State->ControlGeneration++;
		return lease;
	}

	std::vector<HookStatus> HookRegistry::Active() const {
		std::vector<HookStatus> active;
		for (const HookRegistryState::Activation &activation : State->Activations) {
			active.push_back(activation.Status);
		}
		return active;
	}

	uint64_t HookRegistry::ControlGeneration() const {
		return State->ControlGeneration;
	}

	bool HookRegistry::OwnsTool(std::string_view name) const {
		return State->ToolOwners.contains(std::string(name));
	}

	bool HookRegistry::OwnsResource(std::string_view uri) const {
		return State->ResourceOwners.contains(std::string(uri));
	}

	bool HookRegistry::OwnsPrompt(std::string_view name) const {
		return State->PromptOwners.contains(std::string(name));
	}

	bool HookRegistry::VisibleTool(std::string_view name) const {
		const auto owner = State->ToolOwners.find(std::string(name));
		return owner == State->ToolOwners.end() ||
			   (Find(*State, owner->second.first, owner->second.second) != nullptr &&
				Find(*State, owner->second.first, owner->second.second)->Status.State == HookState::Active);
	}
	bool HookRegistry::VisibleResource(std::string_view uri) const {
		const auto owner = State->ResourceOwners.find(std::string(uri));
		return owner == State->ResourceOwners.end() ||
			   (Find(*State, owner->second.first, owner->second.second) != nullptr &&
				Find(*State, owner->second.first, owner->second.second)->Status.State == HookState::Active);
	}
	bool HookRegistry::VisiblePrompt(std::string_view name) const {
		const auto owner = State->PromptOwners.find(std::string(name));
		return owner == State->PromptOwners.end() ||
			   (Find(*State, owner->second.first, owner->second.second) != nullptr &&
				Find(*State, owner->second.first, owner->second.second)->Status.State == HookState::Active);
	}

	void HookRegistry::Close(std::string_view id, uint64_t generation) {
		HookRegistryState::Activation *activation = Find(*State, id, generation);
		if (activation == nullptr || activation->Status.State == HookState::Draining) return;
		// A close makes callbacks unavailable immediately. Dependants delay removal,
		// not the state change, so a provider cannot accept new calls while retiring.
		activation->Status.State = HookState::Draining;
		if (HasActiveDependent(*State, id)) {
			return;
		}
		if (activation->Guards == 0 && Drained(*activation)) Release(id, generation);
	}

	bool
	HookRegistry::Holds(std::string_view tool, std::string_view expectedId, uint64_t expectedGeneration) {
		const auto owner = State->ToolOwners.find(std::string(tool));
		if (owner == State->ToolOwners.end()) return false;
		HookRegistryState::Activation *activation = Find(*State, owner->second.first, owner->second.second);
		if (activation == nullptr || activation->Status.State != HookState::Active ||
			owner->second != std::pair{std::string(expectedId), expectedGeneration})
			return false;
		activation->Guards++;
		return true;
	}

	bool HookRegistry::HoldsResource(
		std::string_view uri, std::string_view expectedId, uint64_t expectedGeneration
	) {
		const auto owner = State->ResourceOwners.find(std::string(uri));
		if (owner == State->ResourceOwners.end()) return false;
		HookRegistryState::Activation *activation = Find(*State, owner->second.first, owner->second.second);
		if (activation == nullptr || activation->Status.State != HookState::Active ||
			owner->second != std::pair{std::string(expectedId), expectedGeneration})
			return false;
		activation->Guards++;
		return true;
	}

	bool HookRegistry::HoldsPrompt(
		std::string_view name, std::string_view expectedId, uint64_t expectedGeneration
	) {
		const auto owner = State->PromptOwners.find(std::string(name));
		if (owner == State->PromptOwners.end()) return false;
		HookRegistryState::Activation *activation = Find(*State, owner->second.first, owner->second.second);
		if (activation == nullptr || activation->Status.State != HookState::Active ||
			owner->second != std::pair{std::string(expectedId), expectedGeneration})
			return false;
		activation->Guards++;
		return true;
	}

	void HookRegistry::Release(std::string_view id, uint64_t generation) {
		HookRegistryState::Activation *activation = Find(*State, id, generation);
		if (activation == nullptr) return;
		if (activation->Guards > 0) {
			activation->Guards--;
			return;
		}
		if (activation->Status.State != HookState::Draining) return;
		if (HasActiveDependent(*State, id)) return;
		if (!Drained(*activation)) return;

		for (const std::string &name : activation->Status.Tools) {
			const auto owner = State->ToolOwners.find(name);
			if (owner != State->ToolOwners.end() && owner->second == std::pair{std::string(id), generation}) {
				State->Owner->RemoveHookTool(name, id, generation);
				State->ToolOwners.erase(owner);
			}
		}
		for (const std::string &uri : activation->Status.Resources) {
			const auto owner = State->ResourceOwners.find(uri);
			if (owner != State->ResourceOwners.end() &&
				owner->second == std::pair{std::string(id), generation}) {
				State->Owner->RemoveHookResource(uri);
				State->ResourceOwners.erase(owner);
			}
		}
		for (const std::string &name : activation->Status.Prompts) {
			const auto owner = State->PromptOwners.find(name);
			if (owner != State->PromptOwners.end() &&
				owner->second == std::pair{std::string(id), generation}) {
				State->Owner->RemoveHookPrompt(name);
				State->PromptOwners.erase(owner);
			}
		}
		State->Activations.erase(
			std::remove_if(
				State->Activations.begin(),
				State->Activations.end(),
				[id, generation](const auto &candidate) {
					return candidate.Status.Descriptor.Id == id && candidate.Status.Generation == generation;
				}
			),
			State->Activations.end()
		);
		State->ControlGeneration++;
		Reap();
	}

	void HookRegistry::Reap() {
		if (State->Reaping) return;
		State->Reaping = true;
		for (;;) {
			const auto found = std::find_if(
				State->Activations.begin(), State->Activations.end(), [this](const auto &activation) {
					return activation.Status.State == HookState::Draining && activation.Guards == 0 &&
						   !HasActiveDependent(*State, activation.Status.Descriptor.Id) &&
						   Drained(activation);
				}
			);
			if (found == State->Activations.end()) break;
			const std::string id = found->Status.Descriptor.Id;
			const uint64_t generation = found->Status.Generation;
			Release(id, generation);
		}
		State->Reaping = false;
	}
}

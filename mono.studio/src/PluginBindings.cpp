#include "PluginSurfaceInternal.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <studio/Plugins.hpp>
#include <utility>

namespace studio {
	namespace {
		bool Supports(PluginBindingLanguages languages, engine::script::Language language) {
			const PluginBindingLanguages wanted =
				language == engine::script::Language::JavaScript
					? static_cast<PluginBindingLanguages>(PluginBindingLanguage::JavaScript)
					: static_cast<PluginBindingLanguages>(PluginBindingLanguage::Luau);
			return (languages & wanted) != 0;
		}

		bool ValidBindingName(std::string_view name) {
			if (name.empty() || name.front() == '.' || name.back() == '.') {
				return false;
			}
			return name.find("..") == std::string_view::npos;
		}

		struct RegisteredCppPlugin {
			uint64_t Id = 0;
			CppPluginDefinition Definition;
		};

		struct CppRegistry {
			std::mutex Lock;
			std::vector<RegisteredCppPlugin> Definitions;
			uint64_t NextId = 1;
			std::atomic<uint64_t> Revision = 1;
		};

		CppRegistry &CppPlugins() {
			// Registrations may be destroyed during static teardown. Keeping the
			// registry alive avoids a shutdown-order dependency between libraries.
			static CppRegistry *registry = new CppRegistry;
			return *registry;
		}
	}

	struct PluginBindingScope::State {
		struct Row {
			uint64_t Owner = 0;
			std::string Name;
			PluginBindingLanguages Languages = 0;
			PluginBindingFunction Function;
		};

		explicit State(PluginRunTarget target) : Target(target) {}

		std::mutex Lock;
		std::vector<Row> Rows;
		std::function<void()> Changed;
		uint64_t NextOwner = 1;
		uint64_t Revision = 1;
		PluginRunTarget Target = PluginRunTarget::Studio;
	};

	PluginBindingScope::PluginBindingScope(std::shared_ptr<State> state, uint64_t owner)
		: Shared(std::move(state)), Owner(owner) {}

	PluginBindingScope::~PluginBindingScope() {
		Close();
	}

	PluginBindingScope::PluginBindingScope(PluginBindingScope &&other) noexcept
		: Shared(std::move(other.Shared)), Owner(std::exchange(other.Owner, 0)) {}

	PluginBindingScope &PluginBindingScope::operator=(PluginBindingScope &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		Close();
		Shared = std::move(other.Shared);
		Owner = std::exchange(other.Owner, 0);
		return *this;
	}

	bool PluginBindingScope::Add(
		std::string name, PluginBindingFunction function, std::string &error, PluginBindingLanguages languages
	) {
		if (Shared == nullptr || Owner == 0) {
			error = "the plugin binding scope is closed";
			return false;
		}
		if (!ValidBindingName(name)) {
			error = "a plugin binding needs a non-empty name with no empty segment";
			return false;
		}
		if (!function) {
			error = "plugin binding '" + name + "' has no function";
			return false;
		}
		if (languages == 0) {
			error = "plugin binding '" + name + "' has no script language";
			return false;
		}
		const PluginBindingLanguages supportedLanguages =
			static_cast<PluginBindingLanguages>(PluginBindingLanguage::Both);
		if ((languages & supportedLanguages) != languages) {
			error = "plugin binding '" + name + "' has an unknown script language";
			return false;
		}
		if (Shared->Target == PluginRunTarget::Studio && IsStudioPluginHostName(name)) {
			error = "plugin binding '" + name + "' conflicts with a built-in Studio host function";
			return false;
		}

		std::function<void()> changed;
		{
			std::scoped_lock lock(Shared->Lock);
			const auto duplicate =
				std::find_if(Shared->Rows.begin(), Shared->Rows.end(), [&](const auto &row) {
					return row.Name == name && (row.Languages & languages) != 0;
				});
			if (duplicate != Shared->Rows.end()) {
				error = "plugin binding '" + name + "' is already registered";
				return false;
			}
			Shared->Rows.push_back(State::Row{Owner, std::move(name), languages, std::move(function)});
			Shared->Revision++;
			changed = Shared->Changed;
		}

		error.clear();
		if (changed) {
			changed();
		}
		return true;
	}

	void PluginBindingScope::Close() {
		if (Shared == nullptr || Owner == 0) {
			return;
		}

		std::function<void()> changed;
		bool removed = false;
		{
			std::scoped_lock lock(Shared->Lock);
			const size_t before = Shared->Rows.size();
			std::erase_if(Shared->Rows, [&](const State::Row &row) { return row.Owner == Owner; });
			removed = Shared->Rows.size() != before;
			if (removed) {
				Shared->Revision++;
				changed = Shared->Changed;
			}
		}

		Owner = 0;
		Shared.reset();
		if (removed && changed) {
			changed();
		}
	}

	bool PluginBindingScope::IsOpen() const {
		return Shared != nullptr && Owner != 0;
	}

	PluginBindingRegistry::PluginBindingRegistry(PluginRunTarget target)
		: Shared(std::make_shared<PluginBindingScope::State>(target)) {}

	PluginBindingScope PluginBindingRegistry::OpenScope() {
		std::scoped_lock lock(Shared->Lock);
		return PluginBindingScope(Shared, Shared->NextOwner++);
	}

	std::vector<std::string> PluginBindingRegistry::Names(engine::script::Language language) const {
		std::vector<std::string> names;
		std::scoped_lock lock(Shared->Lock);
		for (const PluginBindingScope::State::Row &row : Shared->Rows) {
			if (Supports(row.Languages, language)) {
				names.push_back(row.Name);
			}
		}
		return names;
	}

	bool PluginBindingRegistry::Call(
		engine::script::Language language,
		std::string_view name,
		engine::script::HostArguments arguments,
		engine::script::HostValue &result,
		std::string &failure
	) const {
		PluginBindingFunction function;
		{
			std::scoped_lock lock(Shared->Lock);
			const auto found = std::find_if(Shared->Rows.begin(), Shared->Rows.end(), [&](const auto &row) {
				return row.Name == name && Supports(row.Languages, language);
			});
			if (found == Shared->Rows.end()) {
				failure = "plugin binding '" + std::string(name) + "' is not loaded";
				return false;
			}
			function = found->Function;
		}
		return function(arguments, result, failure);
	}

	void PluginBindingRegistry::OnChanged(std::function<void()> changed) {
		std::scoped_lock lock(Shared->Lock);
		Shared->Changed = std::move(changed);
	}

	uint64_t PluginBindingRegistry::Revision() const {
		std::scoped_lock lock(Shared->Lock);
		return Shared->Revision;
	}

	CppPluginRegistration::~CppPluginRegistration() {
		Close();
	}

	CppPluginRegistration::CppPluginRegistration(CppPluginRegistration &&other) noexcept
		: Id(std::exchange(other.Id, 0)) {}

	CppPluginRegistration &CppPluginRegistration::operator=(CppPluginRegistration &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		Close();
		Id = std::exchange(other.Id, 0);
		return *this;
	}

	void CppPluginRegistration::Close() {
		if (Id == 0) {
			return;
		}
		CppRegistry &registry = CppPlugins();
		std::scoped_lock lock(registry.Lock);
		const size_t before = registry.Definitions.size();
		std::erase_if(registry.Definitions, [&](const RegisteredCppPlugin &row) { return row.Id == Id; });
		if (registry.Definitions.size() != before) {
			registry.Revision++;
		}
		Id = 0;
	}

	bool CppPluginRegistration::IsOpen() const {
		return Id != 0;
	}

	CppPluginRegistration RegisterCppPlugin(CppPluginDefinition definition, std::string &error) {
		if (definition.Manifest.Name.empty()) {
			error = "a C++ plugin needs a name";
			return {};
		}
		if (definition.Manifest.Id.empty()) {
			error = "C++ plugin '" + definition.Manifest.Name + "' needs a stable id";
			return {};
		}
		if (!definition.Open) {
			error = "C++ plugin '" + definition.Manifest.Name + "' has no open function";
			return {};
		}
		if (definition.Manifest.Runs == 0) {
			error = "C++ plugin '" + definition.Manifest.Name + "' has no run target";
			return {};
		}
		const PluginRunTargets supportedTargets = PluginTarget(PluginRunTarget::Studio) |
												  PluginTarget(PluginRunTarget::PlaytestServer) |
												  PluginTarget(PluginRunTarget::PlaytestClient);
		if ((definition.Manifest.Runs & supportedTargets) != definition.Manifest.Runs) {
			error = "C++ plugin '" + definition.Manifest.Name + "' has an unknown run target";
			return {};
		}
		if (definition.Manifest.Id == "atomic.default-studio") {
			error = "plugin id 'atomic.default-studio' is reserved";
			return {};
		}

		CppRegistry &registry = CppPlugins();
		std::scoped_lock lock(registry.Lock);
		const auto duplicate =
			std::find_if(registry.Definitions.begin(), registry.Definitions.end(), [&](const auto &row) {
				return row.Definition.Manifest.Id == definition.Manifest.Id;
			});
		if (duplicate != registry.Definitions.end()) {
			error = "duplicate C++ plugin id '" + definition.Manifest.Id + "'";
			return {};
		}

		const uint64_t id = registry.NextId++;
		registry.Definitions.push_back(RegisteredCppPlugin{id, std::move(definition)});
		registry.Revision++;
		error.clear();
		return CppPluginRegistration(id);
	}

	std::vector<CppPluginDefinition> RegisteredCppPlugins() {
		CppRegistry &registry = CppPlugins();
		std::scoped_lock lock(registry.Lock);
		std::vector<CppPluginDefinition> definitions;
		definitions.reserve(registry.Definitions.size());
		for (const RegisteredCppPlugin &row : registry.Definitions) {
			definitions.push_back(row.Definition);
		}
		return definitions;
	}

	uint64_t CppPluginRegistryRevision() {
		CppRegistry &registry = CppPlugins();
		return registry.Revision.load(std::memory_order_relaxed);
	}
}

#include <engine/core/Paths.hpp>

#include <Luau/Ast.h>
#include <Luau/AstQuery.h>
#include <Luau/Autocomplete.h>
#include <Luau/BuiltinDefinitions.h>
#include <Luau/Config.h>
#include <Luau/ConstraintSolver.h>
#include <Luau/Frontend.h>
#include <Luau/ToString.h>
#include <Luau/Type.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <scriptcheck/Check.hpp>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace scriptcheck {

	namespace fs = std::filesystem;

	namespace {

		// Module names are absolute, normalised, `/`-separated file paths.
		// These prefixes name the requires that do not reach a file, so the
		// reason survives into the diagnostic.
		constexpr std::string_view INSTANCE = "@instance:";
		constexpr std::string_view MISSING = "@missing:";
		constexpr std::string_view ESCAPE = "@escape:";

		std::string Name(const fs::path &path) {
			return fs::absolute(path).lexically_normal().generic_string();
		}

		std::optional<std::string> Read(const fs::path &path) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return std::nullopt;
			}
			std::ostringstream contents;
			contents << file.rdbuf();
			return contents.str();
		}

		// `Main.server.luau` -> `Main`, `Counter.luau` -> `Counter`.
		std::string InstanceName(const fs::path &file) {
			std::string stem = file.stem().string();
			for (const std::string_view suffix : {".server", ".client"}) {
				if (stem.ends_with(suffix)) {
					stem.resize(stem.size() - suffix.size());
				}
			}
			return stem;
		}

		bool IsScript(const fs::path &file) {
			const std::string stem = file.stem().string();
			return stem.ends_with(".server") || stem.ends_with(".client");
		}

		// Where a file sits in the instance tree: `init.luau` is its folder,
		// anything else is a child of its folder named after the file.
		fs::path InstanceOf(const fs::path &file) {
			const std::string name = InstanceName(file);
			return name == "init" ? file.parent_path() : file.parent_path() / name;
		}

		// The file an instance path is, or nothing when no file backs it.
		std::optional<fs::path> FileOf(const fs::path &instance) {
			const std::string base = instance.generic_string();
			for (const char *suffix :
				 {".luau",
				  ".lua",
				  "/init.luau",
				  "/init.lua",
				  ".server.luau",
				  ".client.luau",
				  "/init.server.luau",
				  "/init.client.luau"}) {
				const fs::path candidate = base + suffix;
				std::error_code ignored;
				if (fs::is_regular_file(candidate, ignored)) {
					return candidate;
				}
			}
			return std::nullopt;
		}

		bool Inside(const fs::path &path, const fs::path &root) {
			const fs::path relative = path.lexically_relative(root);
			return !relative.empty() && *relative.begin() != "..";
		}

		bool SkippedFolder(const fs::path &folder) {
			const std::string name = folder.filename().string();
			return (name.starts_with('.') && name != "." && name != "..") || name == "node_modules";
		}

		bool IsLuau(const fs::path &file) {
			return file.extension() == ".luau" || file.extension() == ".lua";
		}

		Diagnostic
		Make(const std::string &module, const Luau::Location &at, std::string code, std::string message) {
			Diagnostic out;
			out.File = module;
			out.Line = static_cast<int>(at.begin.line) + 1;
			out.Column = static_cast<int>(at.begin.column) + 1;
			out.EndLine = static_cast<int>(at.end.line) + 1;
			out.EndColumn = static_cast<int>(at.end.column) + 1;
			out.Code = std::move(code);
			out.Message = std::move(message);
			return out;
		}

		// Luau's `require` magic, reattached because the declarations replace
		// the builtin `require` (so an unresolved instance argument types as
		// the declared `any`) and that drops the magic with it.
		struct Require final : Luau::MagicFunction {
			Luau::MagicRequire Inner;

			std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(
				Luau::TypeChecker &checker,
				const std::shared_ptr<Luau::Scope> &scope,
				const Luau::AstExprCall &call,
				Luau::WithPredicate<Luau::TypePackId> arguments
			) override {
				return Inner.handleOldSolver(checker, scope, call, std::move(arguments));
			}

			// The tracer records an empty module for an argument it could not
			// follow (`require(Modules.X)` through `script :: any`); that is
			// left to the declared signature rather than reported.
			bool infer(const Luau::MagicFunctionCallContext &context) override {
				const Luau::ModuleName &from = context.constraint->moduleName
												   ? *context.constraint->moduleName
												   : context.solver->module->name;
				const auto info = context.solver->moduleResolver->resolveModuleInfo(from, *context.callSite);
				return info && !info->name.empty() && Inner.infer(context);
			}
		};
	}

	struct Checker::State : Luau::FileResolver, Luau::ConfigResolver {
		std::vector<fs::path> Roots;
		std::unordered_map<std::string, std::string> Overlay;
		bool ReportMissing = true;
		bool Loaded = false;

		// Strict for every file, and `.luaurc` is not read: a per-folder answer
		// would let `--!nonstrict` or a config turn the check off from inside
		// the thing being checked.
		Luau::Config Strict;
		std::unique_ptr<Luau::Frontend> Frontend;

		State() {
			Strict.mode = Luau::Mode::Strict;

			// Lints are style, and this gates correctness. Full type graphs are
			// kept so hover and completion can read the checked module.
			Luau::FrontendOptions options;
			options.runLintChecks = false;
			options.retainFullTypeGraphs = true;
			Frontend = std::make_unique<Luau::Frontend>(Luau::SolverMode::New, this, this, options);
		}

		const Luau::Config &
		getConfig(const Luau::ModuleName &, const Luau::TypeCheckLimits &) const override {
			return Strict;
		}

		std::optional<Luau::SourceCode> readSource(const Luau::ModuleName &name) override {
			if (name.starts_with('@')) {
				return std::nullopt;
			}
			const auto type = IsScript(name) ? Luau::SourceCode::Script : Luau::SourceCode::Module;
			if (const auto found = Overlay.find(name); found != Overlay.end()) {
				return Luau::SourceCode{found->second, type};
			}
			std::optional<std::string> source = Read(name);
			if (!source) {
				return std::nullopt;
			}
			return Luau::SourceCode{std::move(*source), type};
		}

		std::string getHumanReadableModuleName(const Luau::ModuleName &name) const override {
			for (const std::string_view prefix : {INSTANCE, MISSING, ESCAPE}) {
				if (name.starts_with(prefix)) {
					return std::string(name.substr(prefix.size()));
				}
			}
			return Display(name);
		}

		// The root a module may not leave, or nothing for a loose file.
		std::optional<fs::path> RootOf(const fs::path &file) const {
			for (const fs::path &root : Roots) {
				if (Inside(file, root)) {
					return root;
				}
			}
			return std::nullopt;
		}

		// Names an instance path: its file when one backs it, else a marker
		// so a later `.Child` can continue from it.
		Luau::ModuleInfo Instance(const fs::path &requirer, const fs::path &instance) const {
			Luau::ModuleInfo info;
			const fs::path normal = instance.lexically_normal();
			const std::optional<fs::path> root = RootOf(requirer);
			if (root && !Inside(normal, *root) && normal != *root) {
				info.name = std::string(ESCAPE) + normal.generic_string();
			} else if (const std::optional<fs::path> file = FileOf(normal)) {
				info.name = Name(*file);
			} else {
				info.name = std::string(INSTANCE) + normal.generic_string();
			}
			return info;
		}

		// The instance a context names, and the file it was reached from.
		std::optional<fs::path> InstanceFrom(const Luau::ModuleInfo &context) const {
			const std::string &name = context.name;
			if (name.starts_with(ESCAPE) || name.starts_with(MISSING)) {
				return std::nullopt;
			}
			if (name.starts_with(INSTANCE)) {
				return fs::path(name.substr(INSTANCE.size()));
			}
			return InstanceOf(name);
		}

		// Called by Luau's require tracer for the argument of a `require` and
		// for each expression it depends on, innermost first, with the parent
		// expression's answer as `context`.
		std::optional<Luau::ModuleInfo> resolveModule(
			const Luau::ModuleInfo *context, Luau::AstExpr *expr, const Luau::TypeCheckLimits &
		) override {
			if (context == nullptr || expr == nullptr) {
				return std::nullopt;
			}

			if (const auto *text = expr->as<Luau::AstExprConstantString>()) {
				return ResolveString(context->name, std::string_view(text->value.data, text->value.size));
			}

			if (const auto *global = expr->as<Luau::AstExprGlobal>()) {
				if (global->name != "script" || context->name.starts_with('@')) {
					return std::nullopt;
				}
				return Instance(context->name, InstanceOf(context->name));
			}

			std::optional<fs::path> base = InstanceFrom(*context);
			if (!base) {
				if (context->name.starts_with(ESCAPE)) {
					return *context;
				}
				return std::nullopt;
			}

			const fs::path requirer = RequirerOf(*context);
			if (const auto *index = expr->as<Luau::AstExprIndexName>()) {
				const std::string child = index->index.value;
				return Instance(requirer, child == "Parent" ? base->parent_path() : *base / child);
			}

			if (const auto *call = expr->as<Luau::AstExprCall>(); call != nullptr && call->self) {
				const auto *method = call->func->as<Luau::AstExprIndexName>();
				const std::string_view name = method != nullptr ? method->index.value : "";
				const bool finds = name == "FindFirstChild" || name == "WaitForChild";
				const auto *child =
					call->args.size >= 1 ? call->args.data[0]->as<Luau::AstExprConstantString>() : nullptr;
				if (finds && child != nullptr) {
					return Instance(requirer, *base / std::string(child->value.data, child->value.size));
				}
			}
			return std::nullopt;
		}

		// A marker carries no requirer, so the escape rule uses the instance
		// path itself: it is inside the same root its chain started in.
		fs::path RequirerOf(const Luau::ModuleInfo &context) const {
			if (context.name.starts_with(INSTANCE)) {
				return fs::path(context.name.substr(INSTANCE.size())) / "_";
			}
			return context.name;
		}

		// Luau's require-by-string rule, which Studio's plugin host follows:
		// `./` and `../` start from the requiring module's folder, `@self/`
		// from the module itself.
		std::optional<Luau::ModuleInfo>
		ResolveString(const std::string &requirer, std::string_view path) const {
			if (requirer.starts_with('@')) {
				return std::nullopt;
			}
			const fs::path module = InstanceOf(requirer);
			fs::path base;
			std::string_view rest = path;
			if (path.starts_with("@self/")) {
				base = module;
				rest.remove_prefix(6);
			} else if (path.starts_with("./") || path.starts_with("../")) {
				base = module.parent_path();
			} else {
				// An alias (`@lune/fs`) names nothing this checker knows.
				return std::nullopt;
			}

			const fs::path target = (base / std::string(rest)).lexically_normal();
			Luau::ModuleInfo info;
			const std::optional<fs::path> root = RootOf(requirer);
			if (root && !Inside(target, *root)) {
				info.name = std::string(ESCAPE) + std::string(path);
			} else if (const std::optional<fs::path> file = FileOf(target)) {
				info.name = Name(*file);
			} else {
				info.name = std::string(MISSING) + std::string(path);
			}
			return info;
		}

		Diagnostic Convert(const Luau::TypeError &error) const {
			std::string code = "TypeError";
			if (Luau::get_if<Luau::SyntaxError>(&error.data) != nullptr) {
				code = "SyntaxError";
			} else if (Luau::get_if<Luau::UnknownRequire>(&error.data) != nullptr) {
				code = "UnknownRequire";
			}
			return Make(
				error.moduleName,
				error.location,
				code,
				Luau::toString(error, Luau::TypeErrorToStringOptions{Frontend->fileResolver})
			);
		}

		// Finds which marker a failed require carried, from the source text
		// the frontend traced. Escapes become `RequireEscape` with their own
		// sentence; a missing file is dropped when drafts asked for that.
		bool Classify(const std::string &module, Diagnostic &diagnostic) const {
			if (diagnostic.Code != "UnknownRequire") {
				return true;
			}
			const auto trace = Frontend->requireTrace.find(module);
			if (trace == Frontend->requireTrace.end()) {
				return true;
			}
			for (const auto &[required, at] : trace->second.requireList) {
				if (static_cast<int>(at.begin.line) + 1 != diagnostic.Line) {
					continue;
				}
				if (required.starts_with(ESCAPE)) {
					const std::optional<fs::path> root = RootOf(module);
					diagnostic.Code = "RequireEscape";
					diagnostic.Message = "require leaves " +
										 (root ? Display(*root) : std::string("the root")) + ": " +
										 required.substr(ESCAPE.size()) +
										 ". A plugin or project may only require its own files.";
					return true;
				}
				if (!ReportMissing && (required.starts_with(MISSING) || required.starts_with(INSTANCE))) {
					return false;
				}
			}
			return true;
		}
	};

	Checker::Checker() : Inner(std::make_unique<State>()) {}
	Checker::~Checker() = default;

	bool Checker::Load(std::string_view declarations, std::vector<Diagnostic> &failures) {
		Luau::Frontend &frontend = *Inner->Frontend;
		Luau::registerBuiltinGlobals(frontend, frontend.globals);

		// Into the global scope: `declare` gives a vocabulary, a required
		// module would only give a table.
		const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
			frontend.globals, frontend.globals.globalScope, std::string(declarations), "@engine", false
		);
		if (!loaded.success) {
			for (const Luau::ParseError &error : loaded.parseResult.errors) {
				failures.push_back(Make("@engine", error.getLocation(), "SyntaxError", error.what()));
			}
			if (loaded.module != nullptr) {
				for (const Luau::TypeError &error : loaded.module->errors) {
					failures.push_back(Make("@engine", error.location, "TypeError", Luau::toString(error)));
				}
			}
			return false;
		}

		// `Enum.NormalId` as a type. A definitions file can only declare the
		// flat `Enum_NormalId`; a dotted type resolves through
		// `importedTypeBindings`, which only the host can fill. luau-lsp does
		// the same through `mono.vendor/patches/luau-lsp/dotted-enum-types.patch`
		// and `just typecheck-editor` keeps the two agreeing. Before `freeze`.
		auto &bindings = frontend.globals.globalScope->importedTypeBindings["Enum"];
		for (const auto &[name, binding] : frontend.globals.globalScope->exportedTypeBindings) {
			constexpr std::string_view PREFIX = "Enum_";
			if (name.size() <= PREFIX.size() || !name.starts_with(PREFIX)) {
				continue;
			}
			const std::string shortName = name.substr(PREFIX.size());
			if (auto *externType = Luau::getMutable<Luau::ExternType>(binding.type)) {
				externType->name = "Enum." + shortName;
			}
			bindings[shortName] = binding;
		}

		const Luau::TypeId require = Luau::getGlobalBinding(frontend.globals, "require");
		Luau::attachTag(require, Luau::kRequireTagName);
		Luau::attachMagicFunction(require, std::make_shared<Require>());

		// Frozen so a script cannot add to the vocabulary it is checked
		// against, which is what the sandbox does at run time too.
		Luau::freeze(frontend.globals.globalTypes);
		Inner->Loaded = true;
		return true;
	}

	void Checker::SetRoots(std::vector<fs::path> roots) {
		Inner->Roots.clear();
		for (const fs::path &root : roots) {
			Inner->Roots.emplace_back(Name(root));
		}
		Inner->Frontend->clear();
	}

	void Checker::SetReportMissingRequires(bool report) {
		Inner->ReportMissing = report;
	}

	void Checker::SetText(const fs::path &file, std::string text) {
		const std::string name = Name(file);
		Inner->Overlay[name] = std::move(text);
		Inner->Frontend->markDirty(name);
	}

	void Checker::Forget(const fs::path &file) {
		const std::string name = Name(file);
		Inner->Overlay.erase(name);
		Inner->Frontend->markDirty(name);
	}

	std::vector<Diagnostic> Checker::Check(const fs::path &file) {
		std::vector<Diagnostic> out;
		const Luau::CheckResult result = Inner->Frontend->check(Name(file));
		for (const Luau::TypeError &error : result.errors) {
			// A marker module (`@instance:`, `@missing:`) has no file of its
			// own; its failure is already reported at the require.
			if (error.moduleName.starts_with('@')) {
				continue;
			}
			Diagnostic diagnostic = Inner->Convert(error);
			if (Inner->Classify(error.moduleName, diagnostic)) {
				out.push_back(std::move(diagnostic));
			}
		}
		Sort(out);
		return out;
	}

	void Sort(std::vector<Diagnostic> &diagnostics) {
		std::sort(diagnostics.begin(), diagnostics.end(), [](const Diagnostic &a, const Diagnostic &b) {
			return std::tie(a.File, a.Line, a.Column, a.Message) <
				   std::tie(b.File, b.Line, b.Column, b.Message);
		});
		// The solver can report one error once per constraint that reached
		// it, so a single `..` over a generic shows up a dozen times.
		const auto same = [](const Diagnostic &a, const Diagnostic &b) {
			return a.File == b.File && a.Line == b.Line && a.Column == b.Column && a.Message == b.Message;
		};
		diagnostics.erase(std::unique(diagnostics.begin(), diagnostics.end(), same), diagnostics.end());
	}

	std::optional<std::string> Checker::TypeAt(const fs::path &file, int line, int column) {
		const std::string name = Name(file);
		Inner->Frontend->check(name);
		const Luau::SourceModule *source = Inner->Frontend->getSourceModule(name);
		const Luau::ModulePtr module = Inner->Frontend->moduleResolver.getModule(name);
		if (source == nullptr || module == nullptr) {
			return std::nullopt;
		}
		const Luau::Position at{static_cast<unsigned>(line - 1), static_cast<unsigned>(column - 1)};
		// An expression's type, else a local's (a declaration is not an
		// expression).
		std::optional<Luau::TypeId> type = Luau::findTypeAtPosition(*module, *source, at);
		if (!type) {
			if (const std::optional<Luau::Binding> binding =
					Luau::findBindingAtPosition(*module, *source, at)) {
				type = binding->typeId;
			} else if (Luau::AstLocal *local = Luau::findExprOrLocalAtPosition(*source, at).getLocal()) {
				// The name in its own `local` statement, which the lookup
				// above skips so `local x = x` reads the outer one.
				for (Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, at); scope && !type;
					 scope = scope->parent) {
					if (const auto found = scope->bindings.find(Luau::Symbol(local));
						found != scope->bindings.end()) {
						type = found->second.typeId;
					}
				}
			}
		}
		if (!type) {
			return std::nullopt;
		}
		Luau::ToStringOptions options;
		options.exhaustive = false;
		options.maxTypeLength = 400;
		return Luau::toString(*type, options);
	}

	std::vector<Completion> Checker::Complete(const fs::path &file, int line, int column) {
		const std::string name = Name(file);
		Inner->Frontend->check(name);
		const Luau::Position at{static_cast<unsigned>(line - 1), static_cast<unsigned>(column - 1)};
		const Luau::AutocompleteResult result =
			Luau::autocomplete(*Inner->Frontend, name, at, [](auto, auto, auto) { return std::nullopt; });

		std::vector<Completion> out;
		for (const auto &[label, entry] : result.entryMap) {
			Completion completion;
			completion.Label = label;
			switch (entry.kind) {
			case Luau::AutocompleteEntryKind::Keyword:
				completion.Kind = "keyword";
				break;
			case Luau::AutocompleteEntryKind::String:
			case Luau::AutocompleteEntryKind::RequirePath:
				completion.Kind = "string";
				break;
			case Luau::AutocompleteEntryKind::Type:
				completion.Kind = "type";
				break;
			default:
				completion.Kind = "property";
				break;
			}
			if (entry.type) {
				completion.Detail = Luau::toString(*entry.type);
			}
			out.push_back(std::move(completion));
		}
		std::sort(out.begin(), out.end(), [](const Completion &a, const Completion &b) {
			return a.Label < b.Label;
		});
		return out;
	}

	Target Discover(const fs::path &path) {
		Target target;
		std::error_code error;
		if (!fs::is_directory(path, error)) {
			target.Files.push_back(path);
			return target;
		}

		std::vector<fs::path> plugins;
		std::vector<fs::path> files;
		for (auto walk = fs::recursive_directory_iterator(path, error);
			 walk != fs::recursive_directory_iterator();
			 walk.increment(error)) {
			if (error) {
				break;
			}
			if (walk->is_directory() && SkippedFolder(walk->path())) {
				walk.disable_recursion_pending();
				continue;
			}
			if (walk->path().filename() == "plugin.json") {
				plugins.push_back(walk->path().parent_path().lexically_normal());
			} else if (walk->is_regular_file() && IsLuau(walk->path())) {
				files.push_back(walk->path().lexically_normal());
			}
		}

		if (plugins.empty()) {
			target.Roots.push_back(path);
			target.Files = std::move(files);
		} else {
			target.Roots = plugins;
			for (fs::path &file : files) {
				const bool inPlugin =
					std::any_of(plugins.begin(), plugins.end(), [&](const fs::path &plugin) {
						return Inside(fs::absolute(file), fs::absolute(plugin));
					});
				if (inPlugin) {
					target.Files.push_back(std::move(file));
				}
			}
		}
		std::sort(target.Roots.begin(), target.Roots.end());
		std::sort(target.Files.begin(), target.Files.end());
		return target;
	}

	std::optional<fs::path> FindDefinitions(std::optional<std::string_view> flag) {
		if (flag) {
			return fs::path(*flag);
		}
		if (const char *environment = std::getenv("ATOMIC_LUAU_DEFINITIONS"); environment && *environment) {
			return fs::path(environment);
		}

		std::error_code error;
		const fs::path program = engine::core::Paths::Base();
		if (fs::is_regular_file(program / "engine.d.luau", error)) {
			return program / "engine.d.luau";
		}

		const fs::path relative = fs::path("mono.engine") / "script" / "bindings" / "engine.d.luau";
		for (fs::path start : {fs::current_path(error), program}) {
			for (fs::path at = fs::absolute(start); !at.empty(); at = at.parent_path()) {
				if (fs::is_regular_file(at / relative, error)) {
					return at / relative;
				}
				if (at == at.parent_path()) {
					break;
				}
			}
		}
		return std::nullopt;
	}

	std::string Display(const fs::path &file) {
		std::error_code error;
		const fs::path here = fs::current_path(error);
		const fs::path absolute = fs::absolute(file).lexically_normal();
		if (!error && Inside(absolute, here)) {
			return absolute.lexically_relative(here).generic_string();
		}
		return absolute.generic_string();
	}

	std::string FormatText(const Diagnostic &diagnostic) {
		return Display(diagnostic.File) + "(" + std::to_string(diagnostic.Line) + "," +
			   std::to_string(diagnostic.Column) + "): " + diagnostic.Code + ": " + diagnostic.Message;
	}

	std::string FormatJson(const std::vector<Diagnostic> &diagnostics) {
		nlohmann::json out = nlohmann::json::array();
		for (const Diagnostic &diagnostic : diagnostics) {
			out.push_back({
				{"file", Display(diagnostic.File)},
				{"line", diagnostic.Line},
				{"column", diagnostic.Column},
				{"endLine", diagnostic.EndLine},
				{"endColumn", diagnostic.EndColumn},
				{"code", diagnostic.Code},
				{"message", diagnostic.Message},
			});
		}
		return out.dump(1, '\t');
	}
}

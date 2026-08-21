// The component catalogue, generated from the registry rather than written out.
//
// **A hand-maintained list of 136 components is wrong within a month, and wrong
// quietly.** Everything mechanical about a component - its registered name, its
// size, whether it is a tag, whether it can be serialised, whether it has a
// compact wire form, whether it has padding a raw writer would leak - is already
// in `ecs::Components`. This walks that table and writes it out.
//
// The one thing the table cannot know is what a component is *for*, so that
// lives in `purposes.md` beside this file, one line per component, and the two
// are merged here. Run with `--check` the tool regenerates and compares instead
// of writing, and it fails when a registered component has no purpose written -
// which is what makes adding a component to the engine also add it to the
// catalogue, rather than leaving the catalogue to drift.
//
// The pattern is `expected_graph.json`'s and `bindings`': a checked-in
// expectation, a tool that compares, and a rule that says if the two disagree
// the question is which one is wrong.
//
// Worth knowing, and the same caveat `bindings` carries: `expected_graph.json`
// does not track a bare `add_executable`, so this link row is reviewed here and
// nowhere else. Keep it short. A component registered by a *program* rather than
// by the engine - `mono.server` has two, `mono.client` one - is deliberately not
// covered, because linking a program library into a build tool to document three
// rows is a worse trade than saying so in the output.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

	using engine::ecs::ComponentId;
	using engine::ecs::ComponentKind;
	using engine::ecs::Components;
	using engine::ecs::TypeDescriptor;

	// One row of the catalogue: what the registry knows, plus what it cannot.
	struct Row {
		std::string Name;
		std::string Module;
		uint32_t Size = 0;
		uint32_t Alignment = 1;
		bool Tag = false;
		bool Serialisable = false;
		bool RawSerialisation = false;
		bool Padded = false;
		uint32_t WireSize = 0;
		std::string Purpose;
	};

	// The module a component belongs to is the part of its name before the
	// first dot. That is a convention rather than a rule the build checks, so a
	// name with no dot is reported as `(unprefixed)` rather than silently
	// filed somewhere - an unprefixed component is a finding, not a module.
	std::string ModuleOf(std::string_view name) {
		const size_t dot = name.find('.');
		return dot == std::string_view::npos ? std::string("(unprefixed)") : std::string(name.substr(0, dot));
	}

	std::string Trim(std::string_view text) {
		size_t from = 0;
		size_t to = text.size();
		while (from < to && (text[from] == ' ' || text[from] == '\t')) {
			from++;
		}
		while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\r')) {
			to--;
		}
		return std::string(text.substr(from, to - from));
	}

	// `purposes.md` is one component per line, `name | purpose`.
	//
	// **A data line is one whose name part is non-empty and holds no
	// whitespace.** Everything else is prose, which is what lets the file be a
	// real document with headings and paragraphs rather than a table wearing
	// `#` on every explanatory line. The rule is deliberately mechanical: a
	// component name never contains a space, so "prose" and "typo" cannot be
	// confused with each other - a misspelled name still parses as a data line
	// and is reported as naming nothing, which is the failure a reader can act
	// on. A markdown table row starts with `|`, so its name part is empty and
	// it is prose.
	std::map<std::string, std::string> ReadPurposes(const std::filesystem::path &path, std::string &error) {
		std::map<std::string, std::string> purposes;
		std::ifstream file(path);
		if (!file) {
			error = "cannot read " + path.string();
			return purposes;
		}

		std::string line;
		int number = 0;
		while (std::getline(file, line)) {
			number++;
			const std::string trimmed = Trim(line);
			const size_t bar = trimmed.find('|');
			if (bar == std::string::npos) {
				continue;
			}

			const std::string name = Trim(std::string_view(trimmed).substr(0, bar));
			if (name.empty() || name.find_first_of(" \t") != std::string::npos) {
				continue;
			}

			const auto [entry, fresh] =
				purposes.emplace(name, Trim(std::string_view(trimmed).substr(bar + 1)));
			if (!fresh) {
				error = path.string() + ":" + std::to_string(number) + ": '" + name +
						"' has a second purpose line. One component, one line.";
				return purposes;
			}
		}
		return purposes;
	}

	// Registration order depends on which translation unit ran first, so the
	// output sorts by name. A document that reordered itself when a link line
	// moved would fail its own drift check for a reason nobody could act on -
	// the same argument `bindings::ComponentNames` makes.
	std::vector<Row> Collect(const std::map<std::string, std::string> &purposes) {
		std::vector<Row> rows;
		rows.reserve(Components::Count());

		for (uint32_t index = 0; index < static_cast<uint32_t>(Components::Count()); index++) {
			const TypeDescriptor &type = Components::Describe(ComponentId(index));

			Row row;
			row.Name = std::string(type.Name.Text());
			row.Module = ModuleOf(row.Name);
			row.Size = type.Size;
			row.Alignment = type.Alignment;
			row.Tag = type.Kind == ComponentKind::Tag;
			row.Serialisable = type.Serialisable;
			row.RawSerialisation = type.RawSerialisation;
			row.Padded = type.Padded;
			row.WireSize = type.Wire.Present() ? type.Wire.Size : 0;

			const auto found = purposes.find(row.Name);
			row.Purpose = found == purposes.end() ? std::string() : found->second;
			rows.push_back(std::move(row));
		}

		std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
			return a.Module == b.Module ? a.Name < b.Name : a.Module < b.Module;
		});
		return rows;
	}

	void Registrations() {
		// **A `Store` first, because three of `ecs`'s own components are
		// registered lazily.** `Hierarchy`, `InstanceClass` and `InstanceName`
		// come from `RegisterInstanceComponents`, which is declared in
		// `ecs/src/` and so cannot be called from here; `Store`'s constructor
		// calls it, which is how every real program gets them. A catalogue
		// built without this is short by three and says nothing about it.
		const engine::ecs::Store store("componentdoc");
		(void)store;

		// Every engine-side entry point that registers components. Missing one
		// produces a catalogue that passes its own drift check while describing
		// less than the tree holds, which is the single failure mode this tool
		// exists to make impossible.
		engine::ecs::RegisterAttributeComponents();
		engine::world::RegisterMailboxTypes();
		engine::scene::RegisterSceneComponents();
		engine::gui::RegisterGuiComponents();
		engine::physics::RegisterPhysicsComponents();
		engine::effects::RegisterEffectComponents();
		engine::graph::RegisterPipelineComponents();
		engine::script::RegisterScriptComponents();
		engine::replication::RegisterReplicationComponents();
		engine::examples::RegisterExampleComponents();

		// **The classes too, because a class registration registers the
		// components it names.** `bindings` makes the same call for the same
		// reason: a table built from the component entry points alone describes
		// what a system reads and not what a world can hold.
		engine::scene::RegisterSceneClasses();
		engine::effects::RegisterEffectClasses();
	}

	std::string Yes(bool value) {
		return value ? "yes" : ".";
	}

	std::string Render(const std::vector<Row> &rows) {
		std::ostringstream out;

		out << "# ECS components\n\n";
		out << "**Generated by `just components`. Do not edit this file.** The mechanical\n";
		out << "columns come from `ecs::Components` at runtime; the purpose column comes from\n";
		out << "`mono.tools/componentdoc/purposes.md`, which is where to write.\n\n";
		out << "**Components a *program* registers rather than the engine are absent**:\n";
		out << "`mono.server` has two and `mono.client` one, and linking a program library into\n";
		out << "a build tool to document three rows is the worse trade.\n\n";
		out << "A component registered by `Components::Of<T>()` rather than under an explicit\n";
		out << "name would appear here under the compiler's spelling of its type, in an\n";
		out << "`(unprefixed)` section. **That section being absent is the check**: such a name\n";
		out << "is stable within one build and nothing wider, and decision 21 says a name that\n";
		out << "crosses a save file is a string somebody chose. Four components were in that\n";
		out << "state until v0.19.\n\n";
		out << "| Column | Meaning |\n|---|---|\n";
		out << "| size | `sizeof`, in bytes. `0` is a tag: a type with no data that exists to be matched by "
			   "a query |\n";
		out << "| align | `alignof` |\n";
		out << "| save | has `Write`/`Read`, so a snapshot can carry it. A component without this refuses to "
			   "be saved rather than writing bytes that cannot be read back |\n";
		out << "| raw | `Write` copies the object representation. Padding and any process-local id inside it "
			   "reach the file, which is why `pad` below only matters here |\n";
		out << "| pad | has bytes no member occupies |\n";
		out << "| wire | bytes in the compact replication form. Blank means the wire carries `Write`'s bytes "
			   "unchanged |\n\n";

		std::string module;
		size_t undocumented = 0;
		for (const Row &row : rows) {
			if (row.Module != module) {
				module = row.Module;
				out << "\n## `" << module << "`\n\n";
				out << "| component | size | align | save | raw | pad | wire | what it is for |\n";
				out << "|---|---|---|---|---|---|---|---|\n";
			}

			out << "| `" << row.Name << "` | " << row.Size << " | " << row.Alignment << " | "
				<< Yes(row.Serialisable) << " | " << Yes(row.RawSerialisation) << " | " << Yes(row.Padded)
				<< " | ";
			if (row.WireSize > 0) {
				out << row.WireSize;
			} else {
				out << ".";
			}
			out << " | " << (row.Purpose.empty() ? "**undocumented**" : row.Purpose) << " |\n";
			if (row.Purpose.empty()) {
				undocumented++;
			}
		}

		out << "\n---\n\n";
		out << rows.size() << " components registered by the engine, " << undocumented
			<< " without a purpose line.\n";
		return out.str();
	}

	bool WriteFile(const std::filesystem::path &path, const std::string &text) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file) {
			return false;
		}
		file << text;
		return file.good();
	}

	std::string ReadFile(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return {};
		}
		return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

} // namespace

int main(int argc, char **argv) {
	engine::core::Log::Initialise("componentdoc");

	engine::core::Arguments arguments(
		"componentdoc", "atomic - generates the ECS component catalogue from the registry."
	);
	arguments.Flag("check", "Compare against the checked-in catalogue instead of writing it");
	arguments.Value("out", "FILE", "Where the catalogue is written");
	arguments.Value("purposes", "FILE", "The checked-in purpose lines");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (parsed.Ok && parsed.VersionRequested) {
		std::cout << arguments.VersionLine();
		return 0;
	}
	if (!parsed.Ok || parsed.HelpRequested) {
		return parsed.Ok ? 0 : 2;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
		return 0;
	}

	// `Get` hands back an optional, so the default lives in the `value_or`
	// rather than in a `Has` test that would read the option twice.
	const std::filesystem::path out{std::string(arguments.Get("out").value_or("docs/ECS_COMPONENTS.md"))};
	const std::filesystem::path purposesPath{
		std::string(arguments.Get("purposes").value_or("mono.tools/componentdoc/purposes.md"))
	};

	std::string error;
	const std::map<std::string, std::string> purposes = ReadPurposes(purposesPath, error);
	if (!error.empty()) {
		std::cerr << error << "\n";
		return 2;
	}

	Registrations();
	const std::vector<Row> rows = Collect(purposes);
	if (rows.empty()) {
		std::cerr << "no components registered - the catalogue would describe nothing.\n";
		return 2;
	}

	const std::string rendered = Render(rows);

	// A purpose line for a name nothing registers is the other half of the
	// drift: a component that was renamed or deleted leaves its line behind,
	// and nothing else would notice.
	std::vector<std::string> orphans;
	for (const auto &[name, purpose] : purposes) {
		const auto found =
			std::find_if(rows.begin(), rows.end(), [&](const Row &row) { return row.Name == name; });
		if (found == rows.end()) {
			orphans.push_back(name);
		}
	}

	size_t undocumented = 0;
	for (const Row &row : rows) {
		if (row.Purpose.empty()) {
			undocumented++;
		}
	}

	if (!arguments.Has("check")) {
		if (!WriteFile(out, rendered)) {
			std::cerr << "cannot write " << out << "\n";
			return 2;
		}
		std::cout << "componentdoc - " << rows.size() << " component(s) written to " << out << ", "
				  << undocumented << " undocumented\n";
		return 0;
	}

	int status = 0;
	if (ReadFile(out) != rendered) {
		std::cerr << out.string() << " is out of date. Run `just components`.\n";
		status = 1;
	}
	for (const std::string &orphan : orphans) {
		std::cerr << purposesPath.string() << ": '" << orphan
				  << "' has a purpose line and is not registered. Renamed, or removed?\n";
		status = 1;
	}
	if (undocumented > 0) {
		for (const Row &row : rows) {
			if (row.Purpose.empty()) {
				std::cerr << purposesPath.string() << ": '" << row.Name
						  << "' is registered and has no purpose line.\n";
			}
		}
		status = 1;
	}
	if (status == 0) {
		std::cout << "componentdoc ok - " << rows.size() << " component(s), all documented\n";
	}
	return status;
}

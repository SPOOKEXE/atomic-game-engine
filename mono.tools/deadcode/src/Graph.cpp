#include <deadcode/Graph.hpp>

#include <algorithm>
#include <cstdlib>
#include <cxxabi.h>
#include <numeric>
#include <string_view>
#include <unordered_map>

namespace deadcode {

	namespace {

		constexpr uint32_t R_X86_64_PC32 = 2;
		constexpr uint32_t R_X86_64_PLT32 = 4;

		// Sections whose relocations are bookkeeping rather than references.
		// `.eh_frame` names every function, so following it would keep all of
		// them alive.
		bool Ignored(std::string_view name) {
			return name.starts_with(".debug") || name.starts_with(".eh_frame") || name.starts_with(".note") ||
				name.starts_with(".comment") || name.starts_with(".gcc_except_table") ||
				name.starts_with(".rela") || name.starts_with(".llvm");
		}

		// Sections the loader runs on its own. Whatever they name is a root.
		bool RunsAtStartup(std::string_view name) {
			return name.starts_with(".init_array") || name.starts_with(".fini_array") ||
				name.starts_with(".preinit_array") || name.starts_with(".ctors") || name.starts_with(".dtors");
		}

		struct Node {
			size_t Object = 0;
			std::string Name;
			uint64_t Size = 0;
			bool Function = false;
			bool Weak = false;
		};

		// One defined symbol inside a section, for finding which one covers a
		// relocation's offset.
		struct Span {
			uint64_t Start = 0;
			uint64_t End = 0;
			size_t Node = 0;
		};

		// The far end of an edge: a node, or a name that is joined once every
		// object has been read.
		struct Target {
			size_t Node = SIZE_MAX;
			std::string Name;
		};

		struct Builder {
			std::vector<Node> Nodes;
			std::vector<size_t> Parent;
			std::unordered_map<std::string, size_t> Globals;
			std::vector<std::pair<size_t, Target>> Edges;
			std::vector<std::pair<Role, Target>> Roots;

			size_t Add(Node node) {
				Nodes.push_back(std::move(node));
				Parent.push_back(Parent.size());
				return Nodes.size() - 1;
			}

			size_t Find(size_t node) {
				while (Parent[node] != node) {
					Parent[node] = Parent[Parent[node]];
					node = Parent[node];
				}
				return node;
			}

			void Join(size_t a, size_t b) {
				a = Find(a);
				b = Find(b);
				if (a != b) {
					Parent[std::max(a, b)] = std::min(a, b);
				}
			}

			size_t Resolve(const Target &target) {
				if (target.Node != SIZE_MAX) {
					return Find(target.Node);
				}
				const auto found = Globals.find(target.Name);
				return found == Globals.end() ? SIZE_MAX : Find(found->second);
			}
		};

		// Every node one first-party object contributes, indexed by section.
		struct ObjectNodes {
			std::vector<std::vector<Span>> BySection;
			std::vector<size_t> SectionRest;
			std::vector<size_t> BySymbol;
		};

		size_t Cover(Builder &graph, ObjectNodes &nodes, size_t object, uint32_t section, uint64_t offset) {
			if (section >= nodes.BySection.size()) {
				return SIZE_MAX;
			}
			const std::vector<Span> &spans = nodes.BySection[section];
			auto after = std::upper_bound(spans.begin(), spans.end(), offset, [](uint64_t value, const Span &span) {
				return value < span.Start;
			});
			if (after != spans.begin()) {
				const Span &span = *(after - 1);
				if (offset < span.End || (span.Start == span.End && offset == span.Start)) {
					return span.Node;
				}
			}
			// Bytes no symbol claims: jump tables, string pools, anonymous
			// constants. One node per section stands for all of them.
			if (nodes.SectionRest[section] == SIZE_MAX) {
				nodes.SectionRest[section] = graph.Add({object, {}, 0, false, false});
			}
			return nodes.SectionRest[section];
		}

		void ReadFirstParty(Builder &graph, const Input &input, size_t object) {
			const ObjectFile &file = input.Object;
			ObjectNodes nodes;
			nodes.BySection.resize(file.Sections.size());
			nodes.SectionRest.assign(file.Sections.size(), SIZE_MAX);
			nodes.BySymbol.assign(file.Symbols.size(), SIZE_MAX);

			// Aliases share an address (GCC emits the C1 and C2 constructors
			// as one body), so a node is an address, not a name.
			std::map<std::pair<uint16_t, uint64_t>, size_t> byAddress;
			for (size_t i = 0; i < file.Symbols.size(); ++i) {
				const Symbol &symbol = file.Symbols[i];
				const bool defined = symbol.Section != SECTION_UNDEFINED && symbol.Section < SECTION_RESERVED &&
					symbol.Section < file.Sections.size();
				if (!defined || (symbol.Type != SYMBOL_FUNC && symbol.Type != SYMBOL_OBJECT)) {
					continue;
				}
				const auto key = std::make_pair(symbol.Section, symbol.Value);
				auto found = byAddress.find(key);
				size_t node = 0;
				if (found == byAddress.end()) {
					node = graph.Add({object, symbol.Name, symbol.Size, symbol.Type == SYMBOL_FUNC, symbol.Bind == 2});
					byAddress.emplace(key, node);
					nodes.BySection[symbol.Section].push_back({symbol.Value, symbol.Value + symbol.Size, node});
				} else {
					node = found->second;
					// Prefer the global spelling, which is the one a reader
					// can search for.
					if (symbol.Bind != BIND_LOCAL && graph.Nodes[node].Name.empty()) {
						graph.Nodes[node].Name = symbol.Name;
					}
				}
				nodes.BySymbol[i] = node;
				if (symbol.Bind != BIND_LOCAL) {
					auto [slot, inserted] = graph.Globals.emplace(symbol.Name, node);
					if (!inserted) {
						graph.Join(slot->second, node);
					}
					if (symbol.Name == "main") {
						graph.Roots.push_back({input.Kind, {node, {}}});
					}
				}
			}
			for (size_t section = 0; section < nodes.BySection.size(); ++section) {
				std::vector<Span> &spans = nodes.BySection[section];
				std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) {
					return a.Start < b.Start;
				});
				// A virtual-call thunk shares its target's section and jumps to
				// it without a relocation. Under `-ffunction-sections` that is
				// the only way two functions share one, so they live together.
				if (file.Sections[section].Name.starts_with(".text.")) {
					for (size_t i = 1; i < spans.size(); ++i) {
						graph.Join(spans[0].Node, spans[i].Node);
					}
				}
			}

			for (const RelocationSet &set : file.Relocations) {
				if (set.Target >= file.Sections.size()) {
					continue;
				}
				const std::string &sectionName = file.Sections[set.Target].Name;
				if (Ignored(sectionName)) {
					continue;
				}
				const bool startup = RunsAtStartup(sectionName);
				for (const Relocation &relocation : set.Entries) {
					if (relocation.Symbol == 0 || relocation.Symbol >= file.Symbols.size()) {
						continue;
					}
					const Symbol &symbol = file.Symbols[relocation.Symbol];
					Target target;
					if (symbol.Section == SECTION_UNDEFINED) {
						target.Name = symbol.Name;
					} else if (symbol.Type == SYMBOL_SECTION) {
						// A section-relative reference. PC-relative forms carry
						// the distance to the end of the 4-byte field, so the
						// place they mean is 4 bytes on.
						const bool relative = relocation.Type == R_X86_64_PC32 || relocation.Type == R_X86_64_PLT32;
						const int64_t place = relocation.Addend + (relative ? 4 : 0);
						target.Node = Cover(graph, nodes, object, symbol.Section, place < 0 ? 0 : uint64_t(place));
					} else if (symbol.Bind != BIND_LOCAL) {
						target.Name = symbol.Name;
					} else if (nodes.BySymbol[relocation.Symbol] != SIZE_MAX) {
						target.Node = nodes.BySymbol[relocation.Symbol];
					} else {
						target.Node = Cover(graph, nodes, object, symbol.Section, symbol.Value);
					}
					if (target.Node == SIZE_MAX && target.Name.empty()) {
						continue;
					}

					if (startup) {
						graph.Roots.push_back({input.Kind, std::move(target)});
					} else {
						const size_t from = Cover(graph, nodes, object, set.Target, relocation.Offset);
						if (from != SIZE_MAX) {
							graph.Edges.push_back({from, std::move(target)});
						}
					}
				}
			}
		}

		// A vendor object counts only for the first-party names it asks for.
		void ReadVendor(Builder &graph, const Input &input) {
			for (const Symbol &symbol : input.Object.Symbols) {
				if (symbol.Section == SECTION_UNDEFINED && !symbol.Name.empty()) {
					graph.Roots.push_back({Role::Ship, {SIZE_MAX, symbol.Name}});
				}
			}
		}

		std::vector<bool> Walk(Builder &graph, const std::vector<std::vector<size_t>> &out, bool withTests) {
			std::vector<bool> live(graph.Nodes.size(), false);
			std::vector<size_t> stack;
			for (const auto &[role, target] : graph.Roots) {
				if (role == Role::Test && !withTests) {
					continue;
				}
				const size_t node = graph.Resolve(target);
				if (node != SIZE_MAX && !live[node]) {
					live[node] = true;
					stack.push_back(node);
				}
			}
			while (!stack.empty()) {
				const size_t node = stack.back();
				stack.pop_back();
				for (const size_t next : out[node]) {
					if (!live[next]) {
						live[next] = true;
						stack.push_back(next);
					}
				}
			}
			return live;
		}
	}

	namespace {

		// Code that belongs to a library and only got emitted here: the
		// standard library and vendor headers compiled inside a first-party
		// object, and the replaceable allocation functions the runtime calls
		// by itself. When one of these is dead it is because a first-party
		// caller is, and that caller is already reported.
		//
		// An unmangled name is C. First-party program code is C++ and never
		// `extern "C"` (only fuzz and fault fixtures are), so a C function
		// in a program object came from a vendor implementation header.
		bool Library(std::string_view name) {
			if (name.starts_with("_Znw") || name.starts_with("_Zna") || name.starts_with("_Zdl") ||
				name.starts_with("_Zda")) {
				return true;
			}
			if (!name.starts_with("_Z")) {
				return name != "main";
			}
			name.remove_prefix(2);
			// `L` marks internal linkage; the name that follows is the same.
			if (name.starts_with("L")) {
				name.remove_prefix(1);
			}
			if (name.starts_with("N")) {
				name.remove_prefix(1);
				while (!name.empty() && (name[0] == 'K' || name[0] == 'V' || name[0] == 'r' || name[0] == 'R' ||
					name[0] == 'O')) {
					name.remove_prefix(1);
				}
			}
			if (name.starts_with("St") || name.starts_with("Sa") || name.starts_with("Sb") ||
				name.starts_with("Ss") || name.starts_with("Si") || name.starts_with("So") ||
				name.starts_with("Sd")) {
				return true;
			}
			size_t length = 0;
			size_t digits = 0;
			while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9') {
				length = length * 10 + size_t(name[digits] - '0');
				digits += 1;
			}
			if (digits == 0 || digits + length > name.size()) {
				return false;
			}
			const std::string_view first = name.substr(digits, length);
			static constexpr std::string_view VENDORS[] = {"nlohmann", "__gnu_cxx", "__cxxabiv1", "CryptoPP", "asio"};
			for (const std::string_view vendor : VENDORS) {
				if (first == vendor) {
					return true;
				}
			}
			// stb's implementation headers, compiled as C++ with internal
			// linkage, all prefix their names.
			return first.starts_with("stb") || first == "ttLONG" || first == "ttULONG" || first == "ttSHORT" ||
				first == "ttUSHORT" || first == "ttBYTE" || first == "ttCHAR" || first == "ttFixed";
		}
	}

	bool IsDestructor(const std::string &name) {
		// The Itanium ABI spells the last name of a destructor D0, D1 or D2,
		// closing the nested name with `E` and taking no arguments (`v`).
		return name.starts_with("_Z") &&
			(name.ends_with("D0Ev") || name.ends_with("D1Ev") || name.ends_with("D2Ev"));
	}

	std::string Demangle(const std::string &name) {
		int status = 0;
		char *readable = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
		if (status != 0 || readable == nullptr) {
			std::free(readable);
			return name;
		}
		std::string result(readable);
		std::free(readable);
		return result;
	}

	bool SharesCodeSection(const ObjectFile &object) {
		// The first function's address in each section. An alias at the same
		// address is the same body, not a second function.
		std::vector<uint64_t> first(object.Sections.size(), UINT64_MAX);
		for (const Symbol &symbol : object.Symbols) {
			// Thunks sit in their target's section on purpose; see Analyse.
			const bool thunk = symbol.Name.starts_with("_ZTh") || symbol.Name.starts_with("_ZTv") ||
				symbol.Name.starts_with("_ZTc");
			if (thunk || symbol.Type != SYMBOL_FUNC || symbol.Size == 0 || symbol.Section == SECTION_UNDEFINED ||
				symbol.Section >= first.size()) {
				continue;
			}
			uint64_t &address = first[symbol.Section];
			if (address == UINT64_MAX) {
				address = symbol.Value;
			} else if (address != symbol.Value) {
				return true;
			}
		}
		return false;
	}

	Analysis Analyse(const std::vector<Input> &inputs) {
		Builder graph;
		for (size_t i = 0; i < inputs.size(); ++i) {
			if (inputs[i].Kind == Role::Vendor) {
				ReadVendor(graph, inputs[i]);
			} else {
				ReadFirstParty(graph, inputs[i], i);
			}
		}

		std::vector<std::vector<size_t>> out(graph.Nodes.size());
		for (const auto &[from, target] : graph.Edges) {
			const size_t to = graph.Resolve(target);
			if (to != SIZE_MAX) {
				out[graph.Find(from)].push_back(to);
			}
		}

		const std::vector<bool> shipped = Walk(graph, out, false);
		const std::vector<bool> tested = Walk(graph, out, true);

		Analysis analysis;
		for (size_t i = 0; i < graph.Nodes.size(); ++i) {
			const Node &node = graph.Nodes[i];
			if (!node.Function || graph.Find(i) != i || inputs[node.Object].Kind != Role::Ship) {
				continue;
			}
			const std::string &source = inputs[node.Object].Source;
			if (!node.Weak) {
				analysis.FunctionsPerSource[source] += 1;
			}
			if (shipped[i] || Library(node.Name)) {
				continue;
			}
			Function function{source, node.Name, node.Size, node.Weak};
			(tested[i] ? analysis.TestOnly : analysis.Dead).push_back(std::move(function));
		}
		return analysis;
	}

}

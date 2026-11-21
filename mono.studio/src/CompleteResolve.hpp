#pragma once

// What an expression in a script holds, read off the text, the API dump and the
// edited world. `Complete.cpp` asks this for the subject before a dot or colon,
// for the callee of a signature and for a hovered local. `Complete.hpp` says
// which shapes are followed and why the rest answer nothing.

#include "ApiDump.hpp"

#include <string>
#include <string_view>
#include <studio/Complete.hpp>
#include <vector>

namespace studio {

	// Identifier characters in either language. `$` is JavaScript's alone.
	bool IsCompletionWordCharacter(char character);

	// A name, with no separator and no digit in front of it.
	bool IsCompletionIdentifier(std::string_view value);

	std::string_view TrimCompletion(std::string_view value);

	// An expression with a trailing `--` or `//` comment cut off.
	std::string_view WithoutComment(std::string_view value);

	// The first argument of a call when it is a string literal.
	std::string_view FirstStringArgument(std::string_view arguments);

	// Where the expression ending at `end` begins, walking left over
	// identifiers, dots, colons and balanced bracket groups.
	size_t ExpressionStart(std::string_view text, size_t end);

	// What an expression holds.
	struct ResolvedValue {
		// The declared type, as the dump writes it (`Part`, `Vector3?`,
		// `{ Instance }`). Empty when unknown.
		std::string Type;

		// The node in `ScriptResolver::Tree`, or -1.
		int Node = -1;

		// A local whose last value was destroyed above the caret.
		bool Destroyed = false;
	};

	// One instance in the tree a script is resolved against: the edited world,
	// plus what code above the caret parents into it, minus what it destroys.
	struct ResolvedNode {
		std::string Name;
		std::string ClassName;
		int Parent = -1;
		bool Removed = false;
	};

	class ScriptResolver {
	  public:
		// Reads the code above `caret` for what it creates and destroys.
		ScriptResolver(std::string_view text, size_t caret, const CompletionSources &sources);

		// What `expression` holds as of byte `before`.
		ResolvedValue Evaluate(std::string_view expression, size_t before);

		// The type key a value walks as: its node's class when it has one,
		// otherwise its declared type's key.
		std::string KeyOf(const ResolvedValue &value) const;

		// Live children of a node.
		std::vector<int> ChildrenOf(int node) const;

		const ApiDump &Api() const {
			return Dump;
		}

		const std::vector<ResolvedNode> &Tree() const {
			return Nodes;
		}

	  private:
		// A local assigned from `Instance.new`, and the node it was parented
		// into once code did so.
		struct Binding {
			std::string Name;
			size_t At = 0;
			std::string ClassName;
			std::string PendingName;
			int Node = -1;
		};

		struct Destruction {
			std::string Name;
			size_t At = 0;
		};

		ResolvedValue Evaluate(std::string_view expression, size_t before, int depth);
		ResolvedValue Root(std::string_view name, size_t before, int depth);
		ResolvedValue Local(std::string_view name, size_t before, int depth, bool &found);
		ResolvedValue Index(const ResolvedValue &value, std::string_view name);
		ResolvedValue Call(const ResolvedValue &value, std::string_view name, std::string_view arguments);

		int ChildNamed(int node, std::string_view name) const;
		int AddNode(const std::string &name, const std::string &className, int parent);
		void Reparent(int node, int parent);
		bool Live(int node) const;
		void ReadEffects(size_t caret);
		void ReadStatement(std::string_view statement, size_t at);
		Binding *BindingFor(std::string_view name, size_t before);

		std::string_view Text;
		const ApiDump &Dump;
		std::vector<ResolvedNode> Nodes;
		std::vector<std::vector<int>> Children;
		int ScriptNode = -1;
		std::vector<Binding> Bindings;
		std::vector<Destruction> Destructions;
	};

}

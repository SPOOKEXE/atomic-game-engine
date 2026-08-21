#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <studio/Hierarchy.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Hierarchy;
	using engine::ecs::InstanceClass;
	using engine::ecs::InstanceName;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;

	namespace {
		// The odd 64-bit constant everything here multiplies by.
		constexpr uint64_t GOLDEN = 0x9E3779B97F4A7C15ull;

		// splitmix64's finaliser: the cheapest mix that avalanches every input
		// bit across all sixty-four output ones.
		//
		// **Not `std::hash`**, for the reason `core/Random.hpp` gives about it:
		// the standard says nothing about what it produces, so two builds of
		// this program may disagree. Nothing here crosses a process - the
		// comparison is this frame against the last - but a hash whose value is
		// a property of the compiler is one nobody can reason about or write a
		// test for.
		//
		// @param value The value to mix.
		// @return The mixed value.
		constexpr uint64_t Scramble(uint64_t value) {
			value ^= value >> 30;
			value *= 0xBF58476D1CE4E5B9ull;
			value ^= value >> 27;
			value *= 0x94D049BB133111EBull;
			value ^= value >> 31;
			return value;
		}

		// Folds one term into a running signature, order included.
		//
		// **Order-dependent deliberately.** Two archetypes that swap their rows
		// hold the same instances and would flatten identically, so an
		// order-independent fold would be the more accurate answer - and would
		// also be one where two different worlds can agree by accident, since
		// commutative folds collide far more readily. A reshuffle costs one
		// rebuild nobody sees; a collision is a panel showing a tree that is no
		// longer there.
		//
		// @param running The signature so far.
		// @param term    What to fold in.
		// @return The new signature.
		constexpr uint64_t Fold(uint64_t running, uint64_t term) {
			return (running ^ Scramble(term)) * GOLDEN;
		}

		// A rotate, so a row's four handles do not simply add into each other.
		//
		// @param value The value to rotate.
		// @param by    How far left, 1 to 63.
		// @return The rotated value.
		constexpr uint64_t Rotate(uint64_t value, int by) {
			return (value << by) | (value >> (64 - by));
		}

		// Folds text in, a byte at a time.
		//
		// The filter box holds a handful of characters and this runs once per
		// frame, so the plain loop is the right shape.
		//
		// @param running The signature so far.
		// @param text    The text to fold in.
		// @return The new signature.
		uint64_t FoldText(uint64_t running, std::string_view text) {
			uint64_t accumulated = text.size();
			for (const char character : text) {
				accumulated = accumulated * GOLDEN + static_cast<unsigned char>(character);
			}
			return Fold(running, accumulated);
		}
	}

	bool HierarchyView::Rebuild(Store &store, const HierarchyRequest &request) {
		// Sorted here rather than inside the compile, because the signature has
		// to fold in what the compile will read and the sorted form is what it
		// reads. Hashing the caller's order instead would re-compile whenever
		// its vector happened to shuffle - safe, but a rebuild for nothing.
		OpenSorted.clear();
		OpenSorted.reserve(request.Open.size());
		for (const Entity open : request.Open) {
			OpenSorted.push_back(open.Id);
		}
		std::sort(OpenSorted.begin(), OpenSorted.end());
		OpenSorted.erase(std::unique(OpenSorted.begin(), OpenSorted.end()), OpenSorted.end());

		// --- the scan, which on almost every frame is all that happens -------
		//
		// A pure read: no writes, no allocation, and one linear pass over the
		// three columns every instance carries. What it produces is a number to
		// compare against the last one.
		// **Which world, before what is in it.** Two worlds built the same way
		// allocate the same entity ids and hold the same names, so their
		// contents hash identically - correct arithmetic and the wrong answer
		// for a view that has been pointed at the other one. The store's
		// address is the cheapest thing that tells them apart, and folding it
		// in costs one term rather than a branch.
		uint64_t stamp = Fold(0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&store)));
		size_t instances = 0;

		store.Each<const Hierarchy, const InstanceName, const InstanceClass>(
			[&](
				Entity entity, const Hierarchy &node, const InstanceName &label, const InstanceClass &declared
			) {
				instances++;

				// The five fields `Flatten` reads, and nothing else. See the
				// table in the header: a field added there has to be added
				// here, and the failure if it is not is a row one edit stale.
				uint64_t row = entity.Id;
				row = row * GOLDEN + node.Parent.Id;
				row = Rotate(row, 23) + node.FirstChild.Id;
				row = Rotate(row, 23) + node.NextSibling.Id;
				row = row * GOLDEN + (static_cast<uint64_t>(label.Value.Id()) << 32 |
									  static_cast<uint64_t>(declared.Class.Index));

				stamp = Fold(stamp, row);
			}
		);

		stamp = Fold(stamp, instances);
		stamp = FoldText(stamp, request.Filter);
		for (const uint64_t open : OpenSorted) {
			stamp = Fold(stamp, open);
		}
		for (const Entity reveal : request.Reveal) {
			stamp = Fold(stamp, reveal.Id);
		}

		if (Stamped && stamp == Stamp) {
			return false;
		}

		Stamp = stamp;
		Stamped = true;
		Compile(store, request, instances);
		return true;
	}

	void HierarchyView::Compile(Store &store, const HierarchyRequest &request, size_t instances) {
		Nodes.clear();
		Nodes.reserve(instances);
		RowList.clear();
		RootList.clear();
		Narrowed = !request.Filter.empty();
		Matches = 0;

		// --- the second pass, which only a frame that changed something pays --
		store.Each<const Hierarchy, const InstanceName, const InstanceClass>(
			[&](
				Entity entity, const Hierarchy &node, const InstanceName &label, const InstanceClass &declared
			) {
				Node copied;
				copied.Id = entity.Id;
				copied.Parent = node.Parent;
				copied.FirstChild = node.FirstChild;
				copied.NextSibling = node.NextSibling;
				copied.Name = label.Value;
				copied.Class = declared.Class;
				Nodes.push_back(copied);
			}
		);

		// Sorted so `Find` is a binary chop. Everything below this line reaches
		// the tree through `Find` rather than through the store, which is what
		// makes the flatten a walk over one contiguous array.
		std::sort(Nodes.begin(), Nodes.end(), [](const Node &left, const Node &right) {
			return left.Id < right.Id;
		});

		for (const Node &node : Nodes) {
			if (node.Parent == NULL_ENTITY) {
				RootList.push_back(Entity{node.Id});
			}
		}

		// **`Store::EachRoot`'s order, deliberately.** A world's roots are
		// ordered by creation rather than by insertion - see that function for
		// why - and a panel sorting them its own way would be a second answer
		// to "what order are the roots in". `Nodes` is already sorted by id, so
		// this list came out sorted; the assertion is that it is the same key.
		//
		// (No sort call: the loop above walked `Nodes` in id order.)

		if (Narrowed) {
			for (Node &node : Nodes) {
				int score = 0;
				if (node.Name.IsValid() && FuzzyMatch(request.Filter, Label(node.Name), score)) {
					node.Flags |= MATCH | KEEP;
					Matches++;
				}
			}

			// Ancestors second, and in their own pass: the loop above is
			// walking `Nodes` while this writes to arbitrary entries of it, and
			// doing both at once is a reference invalidated by nothing except
			// the reader's assumption that it is still looking at what it was.
			for (size_t index = 0; index < Nodes.size(); index++) {
				if ((Nodes[index].Flags & MATCH) != 0) {
					MarkAncestors(Entity{Nodes[index].Id}, KEEP | OPEN);
				}
			}
		}

		// **After the filter's propagation, never before it.** The filter's
		// walk stops at the first ancestor already carrying `OPEN` and relies
		// on `OPEN` implying `KEEP`; this one sets `OPEN` alone, because
		// revealing a selection must not smuggle rows past a filter the author
		// typed. Running it first would break the other walk's early exit.
		for (const Entity reveal : request.Reveal) {
			MarkAncestors(reveal, OPEN);
		}

		for (const Entity root : RootList) {
			Flatten(root, 0);
		}
	}

	void HierarchyView::MarkAncestors(Entity from, uint8_t flags) {
		const Node *start = Find(from);
		if (start == nullptr) {
			return;
		}

		Entity walk = start->Parent;
		for (size_t step = 0; step < Nodes.size() && walk != NULL_ENTITY; step++) {
			Node *node = Find(walk);
			if (node == nullptr) {
				return;
			}
			if ((node->Flags & OPEN) != 0) {
				return;
			}
			node->Flags |= flags;
			walk = node->Parent;
		}
	}

	void HierarchyView::Flatten(Entity root, uint16_t depth) {
		// **An explicit stack rather than recursion.** A tree's depth is
		// authored, so a file with ten thousand nested folders in it is a stack
		// overflow in a function that recursed - a crash on open, from data,
		// with nothing to point at. The store's own `DestroyInstance` recurses
		// and inherits that; a panel that draws whatever it is handed should
		// not.
		Stack.clear();
		Stack.push_back(Pending{root, depth});

		while (!Stack.empty()) {
			const Pending at = Stack.back();
			Stack.pop_back();

			Node *node = Find(at.Instance);
			if (node == nullptr) {
				continue;
			}
			if (Narrowed && (node->Flags & KEEP) == 0) {
				continue;
			}

			// **"Has a child this view would show", not "has a child".** While
			// a filter is narrowing the tree, an arrow onto nothing is an arrow
			// the author has to try before learning it is empty.
			bool hasChildren = false;
			for (Entity child = node->FirstChild; child != NULL_ENTITY;) {
				const Node *link = Find(child);
				if (link == nullptr) {
					// A link naming a freed row. `Store::Destroy` releases an
					// entity without touching what points at it, so this is
					// reachable - and the walk stops rather than stepping over
					// it, because the links *out of* that row went with it.
					break;
				}
				if (!Narrowed || (link->Flags & KEEP) != 0) {
					hasChildren = true;
					break;
				}
				child = link->NextSibling;
			}

			const bool open =
				hasChildren && ((node->Flags & OPEN) != 0 ||
								std::binary_search(OpenSorted.begin(), OpenSorted.end(), at.Instance.Id));

			node->Row = RowList.size();

			HierarchyRow row;
			row.Instance = at.Instance;
			row.Parent = node->Parent;
			row.Name = node->Name;
			row.Class = node->Class;
			row.Depth = at.Depth;
			row.HasChildren = hasChildren;
			row.Open = open;
			row.Matched = (node->Flags & MATCH) != 0;

			// The two mutex acquisitions, spent here rather than per frame. See
			// `HierarchyRow::Text`.
			row.Text = node->Name.IsValid() ? Label(node->Name) : "(unnamed)";
			row.ClassText =
				node->Class.IsValid() ? Label(engine::ecs::Classes::Describe(node->Class).Name) : "Entity";

			RowList.push_back(row);

			if (!open) {
				continue;
			}

			// Children pushed in reverse, so they pop in insertion order - the
			// order `EachChild` yields and the order `GetChildren()` returns.
			// Collected first because the sibling list only runs forwards here:
			// `PreviousSibling` is one of the two links this view does not copy,
			// on the grounds that nothing reads it, and walking backwards would
			// make that false.
			const size_t mark = Fringe.size();
			for (Entity child = node->FirstChild; child != NULL_ENTITY;) {
				const Node *link = Find(child);
				if (link == nullptr) {
					break;
				}
				Fringe.push_back(child);
				child = link->NextSibling;
			}

			for (size_t index = Fringe.size(); index > mark; index--) {
				Stack.push_back(Pending{Fringe[index - 1], static_cast<uint16_t>(at.Depth + 1)});
			}
			Fringe.resize(mark);
		}
	}

	HierarchyView::Node *HierarchyView::Find(Entity instance) {
		const auto found =
			std::lower_bound(Nodes.begin(), Nodes.end(), instance.Id, [](const Node &node, uint64_t id) {
				return node.Id < id;
			});
		if (found == Nodes.end() || found->Id != instance.Id) {
			return nullptr;
		}
		return &*found;
	}

	const HierarchyView::Node *HierarchyView::Find(Entity instance) const {
		return const_cast<HierarchyView *>(this)->Find(instance);
	}

	std::span<const HierarchyRow> HierarchyView::Rows() const {
		return RowList;
	}

	size_t HierarchyView::RowOf(Entity instance) const {
		const Node *node = Find(instance);
		return node == nullptr ? NO_ROW : node->Row;
	}

	bool HierarchyView::Holds(Entity instance) const {
		return Find(instance) != nullptr;
	}

	bool HierarchyView::Filtering() const {
		return Narrowed;
	}

	size_t HierarchyView::Count() const {
		return Nodes.size();
	}

	size_t HierarchyView::MatchCount() const {
		return Matches;
	}

	void HierarchyView::Forget() {
		Stamped = false;
	}

	bool HierarchyView::IsUnder(Entity instance, Entity ancestor) const {
		if (ancestor == NULL_ENTITY) {
			// Not everything is inside nothing. The walk ends at the null
			// handle rather than matching it, exactly as `IsDescendantOf` does.
			return false;
		}

		Entity walk = instance;
		for (size_t step = 0; step <= Nodes.size() && walk != NULL_ENTITY; step++) {
			if (walk == ancestor) {
				return true;
			}

			const Node *node = Find(walk);
			if (node == nullptr) {
				return false;
			}
			walk = node->Parent;
		}
		return false;
	}

	std::span<const HierarchyRow> RowsBetween(const HierarchyView &view, Entity anchor, Entity to) {
		const size_t from = view.RowOf(anchor);
		const size_t until = view.RowOf(to);
		if (from == HierarchyView::NO_ROW || until == HierarchyView::NO_ROW) {
			return {};
		}

		const std::span<const HierarchyRow> rows = view.Rows();
		const size_t first = from < until ? from : until;
		const size_t last = from < until ? until : from;
		if (last >= rows.size()) {
			return {};
		}

		return rows.subspan(first, last - first + 1);
	}

	void TopMost(const HierarchyView &view, std::span<const Entity> moving, std::vector<Entity> &out) {
		out.clear();
		out.reserve(moving.size());

		for (const Entity candidate : moving) {
			bool nested = false;
			for (const Entity other : moving) {
				if (other != candidate && view.IsUnder(candidate, other)) {
					nested = true;
					break;
				}
			}

			if (!nested) {
				out.push_back(candidate);
			}
		}
	}

}

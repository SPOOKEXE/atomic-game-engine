#include <engine/graph/PipelineDiagnostics.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::graph {

	namespace {
		// A node's position in the graph, its handle, and the row itself, so a
		// walk does not have to re-`Find` every time.
		struct Row {
			NodeId Id;
			const Node *Body = nullptr;
		};

		// Every enabled node, in declaration order.
		//
		// **Declaration order and not compiled order**, deliberately. These
		// checks have to work on a graph that does not compile — a pipeline
		// mid-edit usually does not — and `Compile` is what refuses those. The
		// only check that cares about order is `WastedWrite`, and declaration
		// order is what an author sees on the canvas.
		std::vector<Row> Enabled(const RenderGraph &graph) {
			std::vector<Row> rows;
			for (size_t index = 0; index < graph.Count(); index++) {
				const NodeId id{static_cast<uint32_t>(index + 1)};
				const Node *body = graph.Find(id);
				if (body != nullptr && body->Enabled) {
					rows.push_back(Row{id, body});
				}
			}
			return rows;
		}

		// Whether a node's whole job is to put a frame somewhere outside the
		// graph.
		//
		// **The exemption every "nothing reads this" check needs.** A `present`
		// writes the swapchain and nothing in the graph reads it, which is not a
		// dead resource — it is the point of the frame. Asked of the catalogue
		// rather than of a name list, so a game that adds its own output kind
		// gets the exemption without editing this file.
		bool IsTerminal(const Node &node) {
			const NodeKindSpec *spec = NodeCatalogue::Find(node.Kind);
			if (spec == nullptr) {
				// **Unknown kinds are treated as terminal, which is the
				// forgiving direction.** A false "this is dead" on a kind we
				// know nothing about is a warning somebody has to learn to
				// ignore, and a warning people ignore is worse than none.
				return true;
			}
			return spec->Category == NodeCategory::Output || spec->Category == NodeCategory::Interface;
		}

		// The format a node's slot declares, by position in its reads or writes.
		//
		// Returns false for an unregistered kind or a position past the end,
		// which is a graph the catalogue cannot describe rather than a fault.
		bool SlotFormat(const Node &node, size_t slot, bool reading, ResourceFormat &out) {
			const NodeKindSpec *spec = NodeCatalogue::Find(node.Kind);
			if (spec == nullptr) {
				return false;
			}
			const std::vector<PortSpec> &side = reading ? spec->Inputs : spec->Outputs;
			if (slot >= side.size()) {
				return false;
			}
			out = side[slot].Format;
			return true;
		}

		std::string NameOf(const RenderGraph &graph, ResourceId resource) {
			const ResourceDesc *desc = graph.FindResource(resource);
			return desc == nullptr ? std::string("?") : std::string(desc->Name.Text());
		}

		bool Touches(const std::vector<ResourceId> &list, ResourceId resource) {
			return std::find(list.begin(), list.end(), resource) != list.end();
		}

		// Whether a resource lives outside the graph — the swapchain, a history
		// buffer, a target the renderer presents itself.
		//
		// **Three checks defer to this**, and all for one reason: the graph is
		// not the whole story for such a resource. Something outside it reads
		// what a pass wrote, so "nothing reads this" is not a fault; something
		// outside it may write between two passes, so "overwritten before a
		// read" is not one either; and a pass writing one is producing the frame
		// rather than feeding a later pass.
		bool IsExternal(const RenderGraph &graph, ResourceId resource) {
			const ResourceDesc *desc = graph.FindResource(resource);
			return desc != nullptr && desc->External;
		}
	}

	const char *Describe(DiagnosticKind kind) {
		switch (kind) {
		case DiagnosticKind::DeadResource:
			return "dead-resource";
		case DiagnosticKind::WastedWrite:
			return "wasted-write";
		case DiagnosticKind::DeadNode:
			return "dead-node";
		case DiagnosticKind::Disconnected:
			return "disconnected";
		case DiagnosticKind::UnwrittenRead:
			return "unwritten-read";
		case DiagnosticKind::FormatOverspend:
			return "format-overspend";
		case DiagnosticKind::LossyWire:
			return "lossy-wire";
		case DiagnosticKind::OutOfOrder:
			return "out-of-order";
		case DiagnosticKind::UnusedAlpha:
			return "unused-alpha";
		}
		return "?";
	}

	const char *Describe(DiagnosticSeverity severity) {
		switch (severity) {
		case DiagnosticSeverity::Warning:
			return "warning";
		case DiagnosticSeverity::Hint:
			return "hint";
		}
		return "?";
	}

	std::vector<Diagnostic> Diagnose(const RenderGraph &graph) {
		std::vector<Diagnostic> found;
		const std::vector<Row> rows = Enabled(graph);

		// Who reads and who writes each resource, once, because every check
		// below asks.
		std::unordered_map<uint32_t, std::vector<size_t>> readers;
		std::unordered_map<uint32_t, std::vector<size_t>> writers;
		for (size_t at = 0; at < rows.size(); at++) {
			for (const ResourceId resource : rows[at].Body->Reads) {
				readers[resource.Value].push_back(at);
			}
			for (const ResourceId resource : rows[at].Body->Writes) {
				writers[resource.Value].push_back(at);
			}
		}

		const auto readCount = [&readers](ResourceId resource) {
			const auto at = readers.find(resource.Value);
			return at == readers.end() ? size_t{0} : at->second.size();
		};

		// --- dead resources, and the nodes that are dead because of them -------

		for (const auto &[value, wrote] : writers) {
			const ResourceId resource{value};
			if (readCount(resource) > 0 || IsExternal(graph, resource)) {
				continue;
			}

			// Written by something whose job is to leave the graph. Not dead.
			const bool terminal = std::any_of(wrote.begin(), wrote.end(), [&rows](size_t at) {
				return IsTerminal(*rows[at].Body);
			});
			if (terminal) {
				continue;
			}

			// **Every writer, not just the first.** Two passes writing a target
			// nobody reads is two mistakes, and marking only one of them leaves
			// the other looking fine.
			for (const size_t writer : wrote) {
				found.push_back(
					Diagnostic{
						DiagnosticKind::DeadResource,
						DiagnosticSeverity::Warning,
						rows[writer].Body->Name,
						graph.FindResource(resource) == nullptr ? core::Name{}
																: graph.FindResource(resource)->Name,
						"'" + NameOf(graph, resource) + "' is written here and read by nothing",
					}
				);
			}
		}

		std::unordered_set<uint32_t> deadNodes;
		for (size_t at = 0; at < rows.size(); at++) {
			const Node &node = *rows[at].Body;
			if (node.Writes.empty() || IsTerminal(node)) {
				continue;
			}

			const bool anyRead = std::any_of(node.Writes.begin(), node.Writes.end(), [&](ResourceId r) {
				return readCount(r) > 0 || IsExternal(graph, r);
			});
			if (anyRead) {
				continue;
			}

			deadNodes.insert(rows[at].Id.Value);
			found.push_back(
				Diagnostic{
					DiagnosticKind::DeadNode,
					DiagnosticSeverity::Warning,
					node.Name,
					{},
					"nothing reads anything this writes — the pass could be deleted",
				}
			);
		}

		// --- writes overwritten before anybody read them -----------------------
		//
		// **The clearest case is a clear followed by a full copy**, which is what
		// `PIPELINE_NODES.md` §1.5 fault 2 is: three targets cleared at the top
		// of the frame, two of them wholly overwritten a few draws later. Stated
		// generally, because a clear is not the only pass that can be wasted.

		for (const auto &[value, wrote] : writers) {
			if (wrote.size() < 2) {
				continue;
			}
			const ResourceId resource{value};
			if (IsExternal(graph, resource)) {
				continue;
			}

			const auto &read = readers.find(value);
			const std::vector<size_t> empty;
			const std::vector<size_t> &reads = read == readers.end() ? empty : read->second;

			for (size_t index = 0; index + 1 < wrote.size(); index++) {
				const size_t first = wrote[index];
				const size_t next = wrote[index + 1];

				// A pass that reads what it writes is refining it, not
				// replacing it — `transparent` blending over `colour` is the
				// ordinary case and must not be reported.
				if (Touches(rows[next].Body->Reads, resource)) {
					continue;
				}

				// Somebody read it in between, so the first write was used.
				const bool readBetween = std::any_of(reads.begin(), reads.end(), [first, next](size_t at) {
					return at > first && at < next;
				});
				if (readBetween) {
					continue;
				}

				found.push_back(
					Diagnostic{
						DiagnosticKind::WastedWrite,
						DiagnosticSeverity::Warning,
						rows[first].Body->Name,
						graph.FindResource(resource)->Name,
						"'" + NameOf(graph, resource) + "' is overwritten by '" +
							std::string(rows[next].Body->Name.Text()) + "' before anything reads it",
					}
				);
			}
		}

		// --- reads of things nothing writes ------------------------------------

		for (const Row &row : rows) {
			for (const ResourceId resource : row.Body->Reads) {
				if (writers.find(resource.Value) != writers.end() || IsExternal(graph, resource)) {
					continue;
				}
				found.push_back(
					Diagnostic{
						DiagnosticKind::UnwrittenRead,
						DiagnosticSeverity::Warning,
						row.Body->Name,
						graph.FindResource(resource) == nullptr ? core::Name{}
																: graph.FindResource(resource)->Name,
						"'" + NameOf(graph, resource) + "' is read here and written by nothing",
					}
				);
			}
		}

		// --- subgraphs that go nowhere ------------------------------------------
		//
		// Reachability *backwards* from every terminal node: a node is connected
		// when something it writes is read, directly or transitively, by a pass
		// that leaves the graph. A whole cluster wired to itself and to nothing
		// else fails this while every individual node in it looks fine — which
		// is precisely fault 8.

		std::unordered_set<size_t> reaches;
		{
			std::vector<size_t> frontier;
			for (size_t at = 0; at < rows.size(); at++) {
				// **Terminal by kind, or terminal by what it writes.** A
				// `present` leaves the graph because that is its job; an
				// `opaque` writing the target the renderer presents leaves it
				// too, and only the resource knows that.
				const bool leaves = std::any_of(
					rows[at].Body->Writes.begin(), rows[at].Body->Writes.end(), [&](ResourceId r) {
						return IsExternal(graph, r);
					}
				);
				if (IsTerminal(*rows[at].Body) || leaves) {
					reaches.insert(at);
					frontier.push_back(at);
				}
			}

			while (!frontier.empty()) {
				const size_t at = frontier.back();
				frontier.pop_back();

				for (const ResourceId resource : rows[at].Body->Reads) {
					const auto wrote = writers.find(resource.Value);
					if (wrote == writers.end()) {
						continue;
					}
					for (const size_t writer : wrote->second) {
						if (reaches.insert(writer).second) {
							frontier.push_back(writer);
						}
					}
				}
			}
		}

		for (size_t at = 0; at < rows.size(); at++) {
			if (reaches.count(at) > 0 || deadNodes.count(rows[at].Id.Value) > 0) {
				continue;
			}
			found.push_back(
				Diagnostic{
					DiagnosticKind::Disconnected,
					DiagnosticSeverity::Warning,
					rows[at].Body->Name,
					{},
					"nothing this pass produces reaches the frame",
				}
			);
		}

		// --- declared order against dependency order ------------------------------
		//
		// **`Compile` refuses this, and refusing is not the same as explaining.**
		// It answers `ReadsBeforeWrite` with the resource's name and stops at
		// the first one it meets. What somebody staring at a canvas needs is
		// which box to drag and what it depends on, for every place the graph
		// has the problem — so this walks all of them and names both ends.

		for (size_t at = 0; at < rows.size(); at++) {
			for (const ResourceId resource : rows[at].Body->Reads) {
				const auto wrote = writers.find(resource.Value);
				if (wrote == writers.end() || IsExternal(graph, resource)) {
					continue;
				}

				// The earliest writer. A resource written by several passes is
				// in order as long as *one* of them comes first — the reader is
				// then reading something, whichever refinement it gets.
				const size_t earliest = wrote->second.front();
				if (earliest <= at) {
					continue;
				}

				// **A pass that reads and writes the same resource is not out of
				// order with itself.** `transparent` blending over `colour` is
				// the ordinary case and appears in both lists.
				if (earliest == at) {
					continue;
				}

				found.push_back(
					Diagnostic{
						DiagnosticKind::OutOfOrder,
						DiagnosticSeverity::Warning,
						rows[at].Body->Name,
						graph.FindResource(resource) == nullptr ? core::Name{}
																: graph.FindResource(resource)->Name,
						"declared before '" + std::string(rows[earliest].Body->Name.Text()) +
							"', which writes the '" + NameOf(graph, resource) +
							"' it reads — the frame runs in the right order and the list does not",
					}
				);
			}
		}

		// --- formats -------------------------------------------------------------

		for (const Row &row : rows) {
			for (size_t slot = 0; slot < row.Body->Writes.size(); slot++) {
				const ResourceId resource = row.Body->Writes[slot];

				// **A list of instances has no pixels, so it has no bit depth to
				// overspend.** `ResourceKind::Entities` carries indices into the
				// view's draw list; its slots declare a format only because
				// every slot does, and reading that as "eight bits a pixel"
				// made every colour target in a pipeline with a filter wired
				// into it report as overspent. Skipped rather than given a
				// sentinel format, because a format that means "not an image"
				// would have to be understood everywhere a format is.
				const ResourceDesc *described = graph.FindResource(resource);
				if (described != nullptr && described->Kind == ResourceKind::Entities) {
					continue;
				}

				ResourceFormat produced{};
				if (!SlotFormat(*row.Body, slot, false, produced)) {
					continue;
				}

				// What the widest reader actually asks for.
				uint32_t widest = 0;
				uint32_t mostChannels = 0;
				bool anyReader = false;
				bool anyLossy = false;

				const auto read = readers.find(resource.Value);
				if (read != readers.end()) {
					for (const size_t at : read->second) {
						const Node &reader = *rows[at].Body;
						for (size_t in = 0; in < reader.Reads.size(); in++) {
							if (reader.Reads[in] != resource) {
								continue;
							}
							ResourceFormat wanted{};
							if (!SlotFormat(reader, in, true, wanted)) {
								continue;
							}
							if (described != nullptr && described->Kind == ResourceKind::Entities) {
								continue;
							}
							anyReader = true;
							widest = std::max(widest, BitsPerPixel(wanted));
							mostChannels = std::max(mostChannels, ChannelCount(wanted));
							anyLossy = anyLossy || IsLossy(produced, wanted);
						}
					}
				}

				if (!anyReader) {
					continue;
				}

				if (widest < BitsPerPixel(produced)) {
					found.push_back(
						Diagnostic{
							DiagnosticKind::FormatOverspend,
							DiagnosticSeverity::Warning,
							row.Body->Name,
							graph.FindResource(resource)->Name,
							"'" + NameOf(graph, resource) + "' is written as " +
								std::string(Describe(produced)) + " and no reader takes more than " +
								std::to_string(widest) + " bits a pixel",
						}
					);
				}

				if (anyLossy) {
					found.push_back(
						Diagnostic{
							DiagnosticKind::LossyWire,
							DiagnosticSeverity::Hint,
							row.Body->Name,
							graph.FindResource(resource)->Name,
							"'" + NameOf(graph, resource) +
								"' is read as a narrower format than it is written",
						}
					);
				}

				if (HasAlpha(produced) && mostChannels < 4) {
					found.push_back(
						Diagnostic{
							DiagnosticKind::UnusedAlpha,
							DiagnosticSeverity::Hint,
							row.Body->Name,
							graph.FindResource(resource)->Name,
							"'" + NameOf(graph, resource) + "' has an alpha channel no reader looks at",
						}
					);
				}
			}
		}

		// **Warnings first, then by kind, then by node.** A panel redraws this
		// every frame and a list that reordered itself would be unreadable — the
		// same argument `NodeCatalogue::All` makes about the add menu.
		std::stable_sort(found.begin(), found.end(), [](const Diagnostic &a, const Diagnostic &b) {
			if (a.Severity != b.Severity) {
				return a.Severity < b.Severity;
			}
			if (a.Kind != b.Kind) {
				return a.Kind < b.Kind;
			}
			return a.Node.Text() < b.Node.Text();
		});

		return found;
	}
}

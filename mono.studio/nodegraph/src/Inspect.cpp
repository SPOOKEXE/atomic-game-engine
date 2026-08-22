// Which inspector handler a node gets. The handlers themselves draw with ImGui
// and live in `Inspectors.cpp`; this is the table and the fallback, which are
// the halves a test can reach.

#include <nodegraph/Inspect.hpp>
#include <nodegraph/Layout.hpp>
#include <string>
#include <unordered_map>

namespace nodegraph {

	namespace {
		std::unordered_map<std::string, InspectorFn> &Handlers() {
			static std::unordered_map<std::string, InspectorFn> table;
			return table;
		}
	}

	void Inspectors::Register(const std::string &id, InspectorFn draw) {
		Handlers()[id] = std::move(draw);
	}

	const InspectorFn *Inspectors::Find(const std::string &id) {
		const auto found = Handlers().find(id);
		return found == Handlers().end() ? nullptr : &found->second;
	}

	const InspectorFn *Inspectors::For(const Inspection &what) {
		RegisterInspectors();

		if (what.Type == nullptr) {
			return Find("empty");
		}
		if (!what.Type->Inspector.empty()) {
			if (const InspectorFn *asked = Find(what.Type->Inspector); asked != nullptr) {
				return asked;
			}
		}

		// A staged type is about its run whether or not it produced anything: the
		// stages are the interesting part and they exist before the result
		// does.
		if (!what.Type->Steps.empty()) {
			return Find("run");
		}

		// **Inferred from the payload and not from the declaration.** A node
		// that has not run yet has nothing to draw, and saying so is better than
		// an empty picture frame that looks like a broken preview.
		if (what.Runner != nullptr && what.Node != nullptr && what.Graph != nullptr) {
			// **Asked of the node's own interface, resolved to where the payload
			// is.** A compressed node's ports are proxies, and reading them off
			// its type would find none at all.
			const auto payloadOn = [&](const PortSpec &port, bool inputs) -> const std::any * {
				NodeId held = NO_NODE;
				std::string heldPort;
				if (!Actual(*what.Graph, what.Node->Id, port.Name, inputs, held, heldPort)) {
					return nullptr;
				}
				if (!inputs) {
					return what.Runner->Output(held, heldPort);
				}
				const Link *link = what.Graph->LinkInto(held, heldPort);
				return link == nullptr ? nullptr : what.Runner->Output(link->From, link->FromPort);
			};

			const auto drawable = [&](const std::vector<PortSpec> &ports, bool inputs) {
				for (const PortSpec &port : ports) {
					const std::any *payload = payloadOn(port, inputs);
					if (payload == nullptr) {
						continue;
					}
					PreviewImage image;
					if (PictureOf(what.Type, port.Type, *payload, image) && image.Valid()) {
						return true;
					}
				}
				return false;
			};

			const std::vector<PortSpec> outputs = OutputsOf(*what.Node);
			if (drawable(outputs, false) || drawable(InputsOf(*what.Node), true)) {
				return Find("field");
			}

			for (const PortSpec &port : outputs) {
				if (payloadOn(port, false) != nullptr) {
					return Find("value");
				}
			}
		}

		return Find("empty");
	}
}

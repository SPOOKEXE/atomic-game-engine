#include <engine/graph/PipelineCatalogue.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace engine::graph {

	namespace {
		// The table, and the lock that lets a kind be registered from wherever a
		// module's registration function happens to run.
		//
		// **A mutex rather than a "call this first" rule**, matching
		// `ecs::Components`: registration happens during static setup and from
		// test fixtures, and a data race there is a corrupt table rather than a
		// diagnostic.
		struct Table {
			std::mutex Guard;

			// Sorted by `Kind`'s text, so `All` hands back a span rather than
			// building one per call. A menu asks every frame.
			std::vector<NodeKindSpec> Specs;

			// `Name::Id` to the position in `Specs`, rebuilt whenever the sort
			// moves anything. A linear scan would be fine at six kinds and
			// wrong at sixty.
			std::unordered_map<uint32_t, size_t> Index;
		};

		Table &Kinds() {
			static Table table;
			return table;
		}

		void Reindex(Table &table) {
			table.Index.clear();
			for (size_t position = 0; position < table.Specs.size(); position++) {
				table.Index[table.Specs[position].Kind.Id()] = position;
			}
		}
	}

	const char *Describe(PortDirection direction) {
		switch (direction) {
		case PortDirection::Input:
			return "input";
		case PortDirection::Output:
			return "output";
		}
		return "?";
	}

	const char *Describe(NodeCategory category) {
		switch (category) {
		case NodeCategory::Draw:
			return "draw";
		case NodeCategory::Composite:
			return "composite";
		case NodeCategory::Interface:
			return "interface";
		case NodeCategory::Output:
			return "output";
		}
		return "?";
	}

	bool PortsCompatible(ResourceKind from, ResourceKind to) {
		if (from == to) {
			return true;
		}

		// The one narrowing rule, and the whole of it: something that was
		// rendered into may be sampled. A shadow map is a `Depth` a pass wrote
		// and every lit pass samples; a mirror is a `Colour` in the same
		// position.
		return to == ResourceKind::Texture && (from == ResourceKind::Colour || from == ResourceKind::Depth);
	}

	std::string_view WhyIncompatible(ResourceKind from, ResourceKind to) {
		if (PortsCompatible(from, to)) {
			return {};
		}

		// **Named in the direction the author is dragging.** "a depth buffer is
		// not a colour attachment" reads as a fact about the world; "cannot
		// connect depth to colour" reads as a fact about the tool, and only one
		// of those tells somebody what to do instead.
		if (from == ResourceKind::Texture) {
			return "a sampled texture is not something a pass can render into";
		}
		if (from == ResourceKind::Depth && to == ResourceKind::Colour) {
			return "a depth buffer is not a colour attachment";
		}
		if (from == ResourceKind::Colour && to == ResourceKind::Depth) {
			return "a colour attachment is not a depth buffer";
		}
		return "these two slots hold different sorts of resource";
	}

	bool NodeCatalogue::Register(NodeKindSpec spec) {
		if (!spec.Kind.IsValid()) {
			return false;
		}

		Table &table = Kinds();
		const std::lock_guard<std::mutex> held(table.Guard);

		const auto found = table.Index.find(spec.Kind.Id());
		if (found != table.Index.end()) {
			table.Specs[found->second] = std::move(spec);
			return true;
		}

		table.Specs.push_back(std::move(spec));
		std::sort(table.Specs.begin(), table.Specs.end(), [](const NodeKindSpec &a, const NodeKindSpec &b) {
			return a.Kind.Text() < b.Kind.Text();
		});
		Reindex(table);
		return true;
	}

	const NodeKindSpec *NodeCatalogue::Find(core::Name kind) {
		Table &table = Kinds();
		const std::lock_guard<std::mutex> held(table.Guard);

		const auto found = table.Index.find(kind.Id());
		return found == table.Index.end() ? nullptr : &table.Specs[found->second];
	}

	std::span<const NodeKindSpec> NodeCatalogue::All() {
		Table &table = Kinds();
		const std::lock_guard<std::mutex> held(table.Guard);
		return table.Specs;
	}

	void NodeCatalogue::Reset() {
		Table &table = Kinds();
		const std::lock_guard<std::mutex> held(table.Guard);
		table.Specs.clear();
		table.Index.clear();
	}

	void RegisterStandardNodeKinds() {
		const auto port = [](const char *name, ResourceKind kind, bool required, const char *summary) {
			PortSpec spec;
			spec.Name = core::Name(name);
			spec.Kind = kind;
			spec.Required = required;
			spec.Summary = summary;
			return spec;
		};

		{
			NodeKindSpec shadow;
			shadow.Kind = core::Name("shadow");
			shadow.Label = "Shadow";
			shadow.Summary = "Draws the casters from the light, into a depth map every view samples.";
			shadow.Category = NodeCategory::Draw;
			shadow.PerView = false;
			shadow.Outputs.push_back(port("shadow", ResourceKind::Depth, true, "The light's depth map."));
			NodeCatalogue::Register(std::move(shadow));
		}

		{
			NodeKindSpec surface;
			surface.Kind = core::Name("surface");
			surface.Label = "Surface";
			surface.Summary = "Renders each mirror's own view into the texture its pane samples.";
			surface.Category = NodeCategory::Draw;
			surface.Inputs.push_back(port("shadow", ResourceKind::Texture, false, "Shadows, if any."));
			surface.Outputs.push_back(
				port("surface", ResourceKind::Colour, true, "One image per surface index.")
			);
			NodeCatalogue::Register(std::move(surface));
		}

		{
			NodeKindSpec opaque;
			opaque.Kind = core::Name("opaque");
			opaque.Label = "Opaque";
			opaque.Summary = "The solid geometry, front to back against the depth buffer.";
			opaque.Category = NodeCategory::Draw;
			opaque.Inputs.push_back(port("shadow", ResourceKind::Texture, false, "Shadows, if any."));
			opaque.Inputs.push_back(port("surface", ResourceKind::Texture, false, "Mirror images."));
			opaque.Outputs.push_back(port("colour", ResourceKind::Colour, true, "The lit frame."));
			opaque.Outputs.push_back(port("depth", ResourceKind::Depth, true, "What it wrote."));
			NodeCatalogue::Register(std::move(opaque));
		}

		{
			NodeKindSpec transparent;
			transparent.Kind = core::Name("transparent");
			transparent.Label = "Transparent";
			transparent.Summary = "The blended tail, back to front, tested against the opaque depth.";
			transparent.Category = NodeCategory::Draw;
			transparent.Inputs.push_back(port("colour", ResourceKind::Colour, true, "What to blend over."));
			transparent.Inputs.push_back(port("depth", ResourceKind::Depth, true, "The opaque depth."));
			transparent.Outputs.push_back(port("colour", ResourceKind::Colour, true, "The blended frame."));
			NodeCatalogue::Register(std::move(transparent));
		}

		{
			NodeKindSpec overlay;
			overlay.Kind = core::Name("overlay");
			overlay.Label = "Overlay";
			overlay.Summary = "The debug panels, drawn once over the whole frame rather than per view.";
			overlay.Category = NodeCategory::Interface;
			overlay.PerView = false;
			overlay.Outputs.push_back(port("window", ResourceKind::Colour, true, "The swapchain."));
			NodeCatalogue::Register(std::move(overlay));
		}

		{
			NodeKindSpec interface;
			interface.Kind = core::Name("interface");
			interface.Label = "Interface";
			interface.Summary = "The retained widget tree, last, over everything.";
			interface.Category = NodeCategory::Interface;
			interface.PerView = false;
			interface.Outputs.push_back(port("window", ResourceKind::Colour, true, "The swapchain."));
			NodeCatalogue::Register(std::move(interface));
		}

		{
			// **Not in `StandardGraph`, and it is the reason a catalogue is not
			// just a description of the frame that exists.** A menu with only
			// the passes already on the canvas offers nothing worth opening;
			// this is the first kind somebody can *add*, and it is the shape
			// every later post pass takes.
			NodeKindSpec tonemap;
			tonemap.Kind = core::Name("tonemap");
			tonemap.Label = "Tone map";
			tonemap.Summary = "Reads one image and writes another. The shape every post pass has.";
			tonemap.Category = NodeCategory::Composite;
			tonemap.Inputs.push_back(port("source", ResourceKind::Texture, true, "What to read."));
			tonemap.Outputs.push_back(port("colour", ResourceKind::Colour, true, "What it wrote."));
			NodeCatalogue::Register(std::move(tonemap));
		}

		{
			NodeKindSpec present;
			present.Kind = core::Name("present");
			present.Label = "Present";
			present.Summary = "Puts a finished image on the window.";
			present.Category = NodeCategory::Output;
			present.PerView = false;
			present.Inputs.push_back(port("colour", ResourceKind::Texture, true, "The frame to show."));
			present.Outputs.push_back(port("window", ResourceKind::Colour, true, "The swapchain."));
			NodeCatalogue::Register(std::move(present));
		}
	}
}

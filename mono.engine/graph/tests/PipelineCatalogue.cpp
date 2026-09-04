// What sorts of node a pipeline can hold, and which slots may be joined.
//
// **Written after two of them turned out to be unreachable.** `overlay` and
// `interface` were registered with outputs and no inputs, so they sat on the
// canvas as islands: no wire could land on them, and no amount of dragging
// would ever change that. Nothing said so - the catalogue was internally
// consistent, the frame still compiled, and the two boxes just never joined
// anything.
//
// So the cases below are mostly about the catalogue as a whole rather than
// about one entry: a kind that cannot be wired is a kind that cannot be used,
// and that is a property of the *set*.

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.graph.pipelinecatalogue")

using engine::core::Name;
using engine::graph::NodeCatalogue;
using engine::graph::NodeCategory;
using engine::graph::NodeKindSpec;
using engine::graph::ParameterWidget;
using engine::graph::PortsCompatible;
using engine::graph::PortSpec;
using engine::graph::ResourceKind;
using engine::graph::WhyIncompatible;

namespace {
	void Kinds() {
		engine::graph::RegisterRenderNodeKinds();
	}
}

// --- the type rule --------------------------------------------------------------

TEST_CASE("a rendered target may be sampled and not rendered into", "[graph][catalogue]") {
	// Exact matches, which is most connections.
	CHECK(PortsCompatible(ResourceKind::Colour, ResourceKind::Colour));
	CHECK(PortsCompatible(ResourceKind::Depth, ResourceKind::Depth));

	// **The one narrowing rule, and it is the one the hardware has.** A shadow
	// map is a `Depth` a pass wrote and every lit pass samples; a mirror is a
	// `Colour` in the same position. Without this the standard frame cannot be
	// wired at all.
	CHECK(PortsCompatible(ResourceKind::Colour, ResourceKind::Texture));
	CHECK(PortsCompatible(ResourceKind::Depth, ResourceKind::Texture));

	// And not the other way: a pass cannot render into something declared as a
	// sampled texture.
	CHECK_FALSE(PortsCompatible(ResourceKind::Texture, ResourceKind::Colour));
	CHECK_FALSE(PortsCompatible(ResourceKind::Texture, ResourceKind::Depth));

	CHECK_FALSE(PortsCompatible(ResourceKind::Depth, ResourceKind::Colour));
	CHECK_FALSE(PortsCompatible(ResourceKind::Colour, ResourceKind::Depth));
}

TEST_CASE("every refusal has a reason and every acceptance has none", "[graph][catalogue]") {
	constexpr ResourceKind EVERY[] = {ResourceKind::Colour, ResourceKind::Depth, ResourceKind::Texture};

	for (const ResourceKind from : EVERY) {
		for (const ResourceKind to : EVERY) {
			// **A panel shows this while the wire is still in the air**, so a
			// refusal with nothing to say would be a drag that silently does
			// nothing - which reads as the editor being broken.
			if (PortsCompatible(from, to)) {
				CHECK(WhyIncompatible(from, to).empty());
			} else {
				CHECK_FALSE(WhyIncompatible(from, to).empty());
			}
		}
	}
}

// --- the catalogue as a set ------------------------------------------------------

// **The case that would have caught the bug.** A pass whose job is to draw over
// what came before has to have something to draw over; one with no inputs is a
// box on the canvas that no wire can reach, and it looks exactly like a
// correctly-registered node until somebody tries to connect it.
//
// **The exemption is `Source` and not `Draw`.** It was `Draw`, and `clear` broke
// it: a pass can be a compositing kind and still produce from nothing, and the
// category is about where a menu puts it rather than about what it reads.
TEST_CASE("every pass that is not a source takes an input", "[graph][catalogue]") {
	Kinds();

	for (const NodeKindSpec &spec : NodeCatalogue::All()) {
		if (spec.Source) {
			continue;
		}

		INFO("kind: " << spec.Kind.Text());
		CHECK_FALSE(spec.Inputs.empty());
	}
}

// And the converse, so `Source` cannot be used to wave a mistake through: a kind
// that declares itself a source and then declares an *image* input is confused
// about which it is.
//
// **`Entities` is exempt, and that is a narrowing rather than a hole.** A source
// is a kind that can start a chain - it needs no picture from anywhere. When
// what a pass draws became a wire, `shadow` and `depth-prepass` gained an
// optional entity input: they still need no image, and they still start a chain,
// but they will now take a list if one is offered. Reading the rule as "reads
// nothing at all" would have forced them to stop being sources, which is the
// opposite of true.
TEST_CASE("a source reads no image", "[graph][catalogue]") {
	Kinds();

	for (const NodeKindSpec &spec : NodeCatalogue::All()) {
		if (!spec.Source) {
			continue;
		}
		INFO("kind: " << spec.Kind.Text());

		for (const PortSpec &input : spec.Inputs) {
			INFO("input: " << input.Name.Text());
			CHECK(input.Kind == engine::graph::ResourceKind::Entities);

			// **And it has to be optional.** A source with a required input is
			// a kind that cannot start a chain, whatever it carries.
			CHECK_FALSE(input.Required);
		}
	}
}

// The other half: a kind whose output nothing accepts is one that can be added
// and then never joined to the frame.
TEST_CASE("every output can land somewhere", "[graph][catalogue]") {
	Kinds();

	for (const NodeKindSpec &spec : NodeCatalogue::All()) {
		for (const PortSpec &output : spec.Outputs) {
			bool accepted = false;
			for (const NodeKindSpec &other : NodeCatalogue::All()) {
				if (other.Kind == spec.Kind) {
					continue;
				}
				for (const PortSpec &input : other.Inputs) {
					accepted = accepted || PortsCompatible(output.Kind, input.Kind);
				}
			}

			INFO("kind: " << spec.Kind.Text() << ", output: " << output.Name.Text());
			CHECK(accepted);
		}
	}
}

TEST_CASE("the default PBR frame's kinds and material ports are registered", "[graph][catalogue]") {
	Kinds();
	for (const char *name :
		 {"world",			"shadow",	  "camera",			  "last-frame",		"entities",
		  "cull-frustum",	"order-draw", "upload-instances", "mirror-capture", "portal-capture",
		  "portal-tonemap", "gbuffer",	  "depth-linearise",  "ssao",			"deferred-lighting",
		  "shader-lenses",	"tonemap",	  "portal-overlay",	  "mirror-overlay", "transparent",
		  "present",		"overlay",	  "interface",		  "output-image"}) {
		INFO("kind: " << name);
		CHECK(NodeCatalogue::Find(Name(name)) != nullptr);
	}

	const NodeKindSpec *gbuffer = NodeCatalogue::Find(Name("gbuffer"));
	const NodeKindSpec *lighting = NodeCatalogue::Find(Name("deferred-lighting"));
	REQUIRE(gbuffer != nullptr);
	REQUIRE(lighting != nullptr);
	CHECK(std::any_of(gbuffer->Outputs.begin(), gbuffer->Outputs.end(), [](const PortSpec &port) {
		return port.Name == Name("emissive");
	}));
	CHECK(std::any_of(lighting->Inputs.begin(), lighting->Inputs.end(), [](const PortSpec &port) {
		return port.Name == Name("emissive");
	}));
	CHECK(std::any_of(lighting->Inputs.begin(), lighting->Inputs.end(), [](const PortSpec &port) {
		return port.Name == Name("occlusion");
	}));
}

TEST_CASE("a kind's slot count matches what the default frame binds", "[graph][catalogue]") {
	Kinds();

	// **Position in the document's `reads`/`writes` list is the slot index**, so
	// a kind that declares fewer ports than the frame binds silently drops the
	// bindings past the end. `DefaultPbrDocument` is the caller that would.
	const auto ports = [](const char *kind, size_t inputs, size_t outputs) {
		const NodeKindSpec *spec = NodeCatalogue::Find(Name(kind));
		REQUIRE(spec != nullptr);
		INFO("kind: " << kind);
		CHECK(spec->Inputs.size() >= inputs);
		CHECK(spec->Outputs.size() >= outputs);
	};

	ports("world", 0, 1);
	ports("shadow", 1, 1);
	ports("camera", 0, 1);
	ports("last-frame", 0, 1);
	ports("entities", 0, 1);
	ports("cull-frustum", 2, 1);
	ports("order-draw", 2, 1);
	ports("upload-instances", 1, 1);
	ports("mirror-capture", 5, 1);
	ports("portal-capture", 3, 2);
	ports("portal-tonemap", 1, 1);
	ports("gbuffer", 3, 5);
	ports("depth-linearise", 1, 1);
	ports("ssao", 2, 1);
	ports("deferred-lighting", 8, 1);
	ports("tonemap", 1, 1);
	ports("portal-overlay", 5, 1);
	ports("mirror-overlay", 5, 1);
	ports("transparent", 4, 1);
	ports("present", 1, 1);
	ports("overlay", 2, 1);
	ports("interface", 0, 1);
	ports("output-image", 1, 0);
}

TEST_CASE("the composed frame ends at one output image sink", "[graph][catalogue][output]") {
	Kinds();

	const NodeKindSpec *final = NodeCatalogue::Find(Name("output-image"));
	REQUIRE(final != nullptr);
	CHECK(NodeCatalogue::Find(Name("output")) == nullptr);
	CHECK(final->Label == "Output Image");
	CHECK(final->Scope == engine::graph::NodeScope::Frame);
	CHECK(final->Inputs.size() == 1);
	CHECK(final->Outputs.empty());
}

TEST_CASE("execution and parameter metadata live on the catalogue row", "[graph][catalogue]") {
	Kinds();

	const NodeKindSpec *world = NodeCatalogue::Find(Name("world"));
	const NodeKindSpec *hzb = NodeCatalogue::Find(Name("hzb"));
	const NodeKindSpec *capture = NodeCatalogue::Find(Name("capture"));
	const NodeKindSpec *dispatch = NodeCatalogue::Find(Name("dispatch"));
	REQUIRE(world != nullptr);
	REQUIRE(hzb != nullptr);
	REQUIRE(capture != nullptr);
	REQUIRE(dispatch != nullptr);

	CHECK(world->Queue == engine::graph::ExecutionQueue::Cpu);
	CHECK(hzb->Queue == engine::graph::ExecutionQueue::Compute);
	CHECK(capture->Queue == engine::graph::ExecutionQueue::Transfer);
	CHECK(hzb->Needs.Compute);
	CHECK(hzb->Needs.StorageTextures);
	CHECK(dispatch->FlexibleScope);
	CHECK(dispatch->Repeatable);

	const auto mode = std::find_if(
		dispatch->Params.begin(), dispatch->Params.end(), [](const engine::graph::ParameterSpec &param) {
			return param.Name == Name("dispatch.mode");
		}
	);
	REQUIRE(mode != dispatch->Params.end());
	CHECK(mode->Widget == ParameterWidget::Select);
	CHECK(mode->Default == "target");
	CHECK(mode->Options == std::vector<std::string>{"target", "groups"});
}

TEST_CASE("parameter schemas have unique names and valid defaults", "[graph][catalogue]") {
	Kinds();

	for (const NodeKindSpec &kind : NodeCatalogue::All()) {
		std::vector<std::string_view> names;
		for (const engine::graph::ParameterSpec &parameter : kind.Params) {
			INFO("kind: " << kind.Kind.Text() << ", parameter: " << parameter.Name.Text());
			CHECK(parameter.Name.IsValid());
			CHECK_FALSE(parameter.Label.empty());
			CHECK(std::find(names.begin(), names.end(), parameter.Name.Text()) == names.end());
			names.push_back(parameter.Name.Text());

			if (parameter.Widget == ParameterWidget::Select) {
				CHECK_FALSE(parameter.Options.empty());
				CHECK(
					std::find(parameter.Options.begin(), parameter.Options.end(), parameter.Default) !=
					parameter.Options.end()
				);
			}
		}
	}
}

TEST_CASE("registering a kind twice replaces it", "[graph][catalogue]") {
	Kinds();

	const size_t before = NodeCatalogue::All().size();

	NodeKindSpec replacement;
	replacement.Kind = Name("gbuffer");
	replacement.Label = "Replaced";
	replacement.Outputs.push_back(
		PortSpec{
			.Name = Name("colour"),
			.Kind = ResourceKind::Colour,
			.Format = engine::graph::ResourceFormat::RGBA8,
			.Required = true,
			.Summary = {},
		}
	);
	REQUIRE(NodeCatalogue::Register(replacement));

	CHECK(NodeCatalogue::All().size() == before);
	CHECK(NodeCatalogue::Find(Name("gbuffer"))->Label == "Replaced");

	// Put it back, because the catalogue is process-wide and the next case in
	// whatever order the runner picked is entitled to the real one.
	Kinds();
	CHECK(NodeCatalogue::Find(Name("gbuffer"))->Label == "G-buffer");

	CHECK_FALSE(NodeCatalogue::Register(NodeKindSpec{}));
}

TEST_CASE("the catalogue lists in a stable order", "[graph][catalogue]") {
	Kinds();

	// **Sorted rather than in registration order**, so the add menu reads the
	// same way whatever order the registrations happened to run in.
	std::string previous;
	for (const NodeKindSpec &spec : NodeCatalogue::All()) {
		const std::string name(spec.Kind.Text());
		CHECK(previous < name);
		previous = name;
	}
}

TEST_CASE("a later Register moves what an earlier Find named", "[graph][catalogue]") {
	Kinds();

	// The documented lifetime on `Find` and `All`, demonstrated by index rather
	// than by pointer. Reading a stale pointer is the bug the contract exists to
	// forbid, so a case that read one to prove the point would be the same
	// mistake wearing a `CHECK`.
	//
	// `Specs` is sorted by `Kind`'s text, so registering a name that sorts
	// before an existing one shifts every later entry by a slot. A pointer taken
	// before this now names the neighbour, and nothing about it looks wrong.
	const auto slotOf = [](std::string_view kind) -> size_t {
		const std::span<const NodeKindSpec> specs = NodeCatalogue::All();
		for (size_t index = 0; index < specs.size(); ++index) {
			if (specs[index].Kind.Text() == kind) {
				return index;
			}
		}
		return specs.size();
	};

	const size_t before = slotOf("gbuffer");
	REQUIRE(before < NodeCatalogue::All().size());

	NodeKindSpec earlier;
	earlier.Kind = Name("aaa-sorts-first");
	earlier.Label = "Sorts first";
	earlier.Outputs.push_back(
		PortSpec{
			.Name = Name("colour"),
			.Kind = ResourceKind::Colour,
			.Format = engine::graph::ResourceFormat::RGBA8,
			.Required = true,
			.Summary = {},
		}
	);
	REQUIRE(NodeCatalogue::Register(earlier));

	CHECK(slotOf("gbuffer") == before + 1);

	// Process-wide table, so hand the next case the real one back. `Kinds()`
	// registers and does not clear, so this is the one case that has to
	// `Reset` - every other case here only ever replaces a built-in.
	NodeCatalogue::Reset();
	Kinds();
	CHECK(NodeCatalogue::Find(Name("aaa-sorts-first")) == nullptr);
	CHECK(NodeCatalogue::Find(Name("gbuffer")) != nullptr);
}

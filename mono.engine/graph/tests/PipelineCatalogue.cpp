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

#include <algorithm>
#include <atomic>
#include <barrier>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
		 {"world",
		  "shadow",
		  "camera",
		  "last-frame",
		  "entities",
		  "cull-frustum",
		  "order-draw",
		  "mesh-residency",
		  "delta-upload",
		  "select-lod",
		  "mirror-capture",
		  "portal-capture",
		  "portal-tonemap",
		  "gbuffer",
		  "depth-linearise",
		  "ssao",
		  "deferred-lighting",
		  "shader-lenses",
		  "bloom",
		  "tonemap",
		  "portal-overlay",
		  "mirror-overlay",
		  "transparent",
		  "present",
		  "overlay",
		  "interface",
		  "output-image"}) {
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
	CHECK(std::any_of(gbuffer->Outputs.begin(), gbuffer->Outputs.end(), [](const PortSpec &port) {
		return port.Name == Name("mesh-uv") && port.Format == engine::graph::ResourceFormat::RG16F;
	}));
	CHECK(gbuffer->Needs.ColourTargets == 8);
	REQUIRE(gbuffer->Outputs.size() == 9);
	CHECK(gbuffer->Outputs[4].Name == Name("mesh-uv"));
	CHECK(gbuffer->Outputs[5].Name == Name("object-ids"));
	CHECK(gbuffer->Outputs[6].Name == Name("semantic-ids"));
	CHECK(gbuffer->Outputs[7].Name == Name("part-ids"));
	CHECK(gbuffer->Outputs[8].Name == Name("depth"));
	CHECK(std::any_of(lighting->Inputs.begin(), lighting->Inputs.end(), [](const PortSpec &port) {
		return port.Name == Name("emissive");
	}));
	CHECK(std::any_of(lighting->Inputs.begin(), lighting->Inputs.end(), [](const PortSpec &port) {
		return port.Name == Name("occlusion");
	}));
	const NodeKindSpec *history = NodeCatalogue::Find(Name("last-frame"));
	REQUIRE(history != nullptr);
	CHECK(history->Lifetime == engine::graph::ResourceLifetime::History);
	CHECK(history->HistoryReads == 1);
}

TEST_CASE(
	"hard-render nodes declare compute contracts and renderer backends", "[graph][catalogue][tracing]"
) {
	Kinds();
	for (const char *name : {"tessellate", "global-illumination", "raytrace", "pathtrace"}) {
		const NodeKindSpec *spec = NodeCatalogue::Find(Name(name));
		REQUIRE(spec != nullptr);
		CHECK(spec->Queue == engine::graph::ExecutionQueue::Compute);
		CHECK(spec->Needs.Compute);
		CHECK(spec->BuiltInBackend);
		if (std::string_view(name) == "tessellate") {
			REQUIRE(spec->Outputs.size() == 3);
			for (const PortSpec &port : spec->Outputs)
				CHECK(port.Kind == engine::graph::ResourceKind::Buffer);
		} else {
			REQUIRE(spec->Outputs.size() == 1);
			CHECK(spec->Outputs.front().Kind == engine::graph::ResourceKind::Storage);
		}
		CHECK(spec->DefaultShader == std::string(name) + ".comp");
	}
	for (const char *name : {"global-illumination", "raytrace", "pathtrace"}) {
		REQUIRE(NodeCatalogue::Find(Name(name)) != nullptr);
		CHECK(NodeCatalogue::Find(Name(name))->Needs.StorageTextures);
	}

	const NodeKindSpec *raytrace = NodeCatalogue::Find(Name("raytrace"));
	REQUIRE(raytrace != nullptr);
	const auto input = [raytrace](std::string_view name) -> const PortSpec * {
		for (const PortSpec &port : raytrace->Inputs)
			if (port.Name.Text() == name) return &port;
		return nullptr;
	};
	REQUIRE(input("scene") != nullptr);
	REQUIRE(input("indirect") != nullptr);
	CHECK_FALSE(input("indirect")->Required);
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
	ports("shadow", 2, 1);
	ports("camera", 0, 1);
	ports("last-frame", 0, 1);
	ports("entities", 0, 1);
	ports("cull-frustum", 2, 1);
	ports("order-draw", 2, 1);
	ports("mesh-residency", 0, 1);
	ports("delta-upload", 2, 1);
	ports("select-lod", 2, 1);
	ports("mirror-capture", 5, 1);
	ports("surface-capture", 4, 3);
	ports("portal-capture", 3, 2);
	ports("portal-tonemap", 1, 1);
	ports("gbuffer", 3, 8);
	ports("depth-peel", 3, 3);
	ports("depth-linearise", 1, 1);
	ports("depth-validity", 1, 1);
	ports("ssao", 2, 1);
	ports("deferred-lighting", 8, 1);
	ports("bloom", 1, 1);
	ports("tonemap", 2, 1);
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
	for (const char *kind : {"mesh-residency", "delta-upload"}) {
		const NodeKindSpec *upload = NodeCatalogue::Find(Name(kind));
		REQUIRE(upload != nullptr);
		CHECK(upload->Queue == engine::graph::ExecutionQueue::Graphics);
		CHECK(upload->TransferOnlyFallback);
		CHECK_FALSE(upload->Needs.Compute);
	}
	CHECK_FALSE(capture->TransferOnlyFallback);
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

TEST_CASE("antialiasing choices are executable graph nodes", "[graph][catalogue][antialiasing]") {
	Kinds();

	for (const char *name : {"fxaa", "taa", "smaa-edges", "smaa-blend", "smaa-resolve"}) {
		const NodeKindSpec *spec = NodeCatalogue::Find(Name(name));
		INFO("kind: " << name);
		REQUIRE(spec != nullptr);
		CHECK(spec->BuiltInBackend);
		CHECK(spec->Queue == engine::graph::ExecutionQueue::Graphics);
		CHECK_FALSE(spec->DefaultShader.empty());
	}

	const NodeKindSpec *taa = NodeCatalogue::Find(Name("taa"));
	REQUIRE(taa != nullptr);
	CHECK(taa->Inputs.size() == 3);
	CHECK(taa->Outputs.size() == 2);
}

TEST_CASE(
	"compositor basics have executable shaders and authored controls", "[graph][catalogue][compositor]"
) {
	Kinds();

	for (const char *name : {"exposure-grade", "hsv", "mix", "transform-crop", "blur"}) {
		const NodeKindSpec *spec = NodeCatalogue::Find(Name(name));
		INFO("kind: " << name);
		REQUIRE(spec != nullptr);
		CHECK(spec->BuiltInBackend);
		CHECK(spec->Repeatable);
		CHECK(spec->Queue == engine::graph::ExecutionQueue::Graphics);
		CHECK_FALSE(spec->DefaultShader.empty());
		CHECK_FALSE(spec->Params.empty());
	}

	const NodeKindSpec *mix = NodeCatalogue::Find(Name("mix"));
	REQUIRE(mix != nullptr);
	REQUIRE(mix->Inputs.size() == 2);
	const auto operation = std::find_if(mix->Params.begin(), mix->Params.end(), [](const auto &parameter) {
		return parameter.Name == Name("operation");
	});
	REQUIRE(operation != mix->Params.end());
	CHECK(operation->Default == "alpha-over");
	CHECK(operation->Options.back() == "alpha-over");

	const NodeKindSpec *transform = NodeCatalogue::Find(Name("transform-crop"));
	REQUIRE(transform != nullptr);
	CHECK(transform->Params.size() == 10);
	const NodeKindSpec *blur = NodeCatalogue::Find(Name("blur"));
	REQUIRE(blur != nullptr);
	CHECK(blur->Inputs.size() == 1);
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

TEST_CASE("builtin registration restores replacements and keeps custom kinds", "[graph][catalogue]") {
	struct Restore {
		~Restore() {
			NodeCatalogue::Reset();
			Kinds();
		}
	} restore;
	NodeCatalogue::Reset();
	Kinds();
	const size_t builtins = NodeCatalogue::All().size();
	REQUIRE(builtins > 0);
	Kinds();
	CHECK(NodeCatalogue::All().size() == builtins);

	NodeKindSpec custom;
	custom.Kind = Name("catalogue-custom-kind");
	custom.Label = "Custom";
	REQUIRE(NodeCatalogue::Register(custom));
	Kinds();
	REQUIRE(NodeCatalogue::Find(custom.Kind) != nullptr);
	CHECK(NodeCatalogue::Find(custom.Kind)->Label == "Custom");
	CHECK(NodeCatalogue::All().size() == builtins + 1);

	NodeKindSpec replacement;
	replacement.Kind = Name("gbuffer");
	replacement.Label = "Replacement";
	REQUIRE(NodeCatalogue::Register(replacement));
	CHECK(NodeCatalogue::Find(replacement.Kind)->Label == "Replacement");
	Kinds();
	CHECK(NodeCatalogue::Find(replacement.Kind)->Label == "G-buffer");
	REQUIRE(NodeCatalogue::Find(custom.Kind) != nullptr);
	CHECK(NodeCatalogue::Find(custom.Kind)->Label == "Custom");

	replacement.Inputs.resize(NodeCatalogue::MAXIMUM_SCHEMA_PORTS + 1);
	CHECK_FALSE(NodeCatalogue::Register(replacement));
	Kinds();
	CHECK(NodeCatalogue::Find(replacement.Kind)->Label == "G-buffer");
	CHECK(NodeCatalogue::Find(custom.Kind) != nullptr);

	NodeCatalogue::Reset();
	CHECK(NodeCatalogue::All().empty());
	Kinds();
	CHECK(NodeCatalogue::All().size() == builtins);
	CHECK(NodeCatalogue::Find(custom.Kind) == nullptr);
	CHECK(NodeCatalogue::Find(replacement.Kind)->Label == "G-buffer");
}

TEST_CASE("concurrent builtin registration does not certify a changed catalogue", "[graph][catalogue]") {
	struct Restore {
		~Restore() {
			NodeCatalogue::Reset();
			Kinds();
		}
	} restore;
	NodeCatalogue::Reset();
	Kinds();
	const size_t builtins = NodeCatalogue::All().size();
	REQUIRE(builtins > 0);
	std::vector<std::pair<Name, std::string>> expected;
	for (const auto &spec : NodeCatalogue::All())
		expected.emplace_back(spec.Kind, spec.Label);

	std::barrier start(3);
	std::atomic<bool> accepted{true};
	auto registerBuiltins = [&] {
		start.arrive_and_wait();
		for (int iteration = 0; iteration < 12; ++iteration)
			Kinds();
	};
	std::thread first(registerBuiltins);
	std::thread second(registerBuiltins);
	start.arrive_and_wait();
	for (int iteration = 0; iteration < 12; ++iteration) {
		NodeCatalogue::Reset();
		NodeKindSpec replacement;
		replacement.Kind = Name("gbuffer");
		replacement.Label = "Concurrent replacement";
		if (!NodeCatalogue::Register(std::move(replacement))) accepted = false;
	}
	first.join();
	second.join();
	REQUIRE(accepted.load());

	// This must repair any reset or replacement that raced with registration.
	Kinds();
	CHECK(NodeCatalogue::All().size() == builtins);
	for (const auto &[kind, label] : expected) {
		const auto *spec = NodeCatalogue::Find(kind);
		REQUIRE(spec != nullptr);
		CHECK(spec->Label == label);
	}
	const auto *gbuffer = NodeCatalogue::Find(Name("gbuffer"));
	REQUIRE(gbuffer != nullptr);
	CHECK(gbuffer->Label == "G-buffer");
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

TEST_CASE("registered render declarations drive named and positional port lookup", "[graph][catalogue]") {
	struct Restore {
		~Restore() {
			NodeCatalogue::Reset();
			engine::graph::RegisterRenderNodeKinds();
		}
	} restore;
	NodeCatalogue::Reset();
	Kinds();
	REQUIRE_FALSE(NodeCatalogue::All().empty());
	size_t maximumPorts = 0;
	size_t maximumBytes = 0;
	for (const auto &spec : NodeCatalogue::All()) {
		CHECK(spec.Declaration.Id == spec.Kind.Text());
		CHECK(spec.Declaration.Ports.size() == spec.Inputs.size() + spec.Outputs.size());
		size_t bytes = spec.Declaration.Id.size();
		for (const auto direction :
			 {engine::graph::PortDirection::Input, engine::graph::PortDirection::Output}) {
			const auto &side = direction == engine::graph::PortDirection::Input ? spec.Inputs : spec.Outputs;
			for (size_t index = 0; index < side.size(); ++index) {
				const auto *port = spec.DeclaredPort(direction, index);
				REQUIRE(port);
				CHECK(port->Id == side[index].Name.Text());
				CHECK(port->ValueType == engine::graph::ResourcePortType(side[index].Kind));
				CHECK(port->Optional);
				CHECK(spec.DeclaredPort(direction, port->Id) == port);
				bytes += port->Id.size() + port->ValueType.size();
			}
			CHECK(spec.DeclaredPort(direction, side.size()) == nullptr);
			CHECK(spec.DeclaredPort(direction, "missing-port") == nullptr);
		}
		CHECK(spec.DeclaredPort(static_cast<engine::graph::PortDirection>(255), size_t{0}) == nullptr);
		CHECK(spec.DeclaredPort(static_cast<engine::graph::PortDirection>(255), "colour") == nullptr);
		maximumPorts = std::max(maximumPorts, spec.Declaration.Ports.size());
		maximumBytes = std::max(maximumBytes, bytes);
	}
	CAPTURE(maximumPorts, maximumBytes);
	CHECK(maximumPorts < NodeCatalogue::MAXIMUM_SCHEMA_PORTS);
	CHECK(maximumBytes < NodeCatalogue::MAXIMUM_SCHEMA_IDENTITY_BYTES);
}

TEST_CASE("render resource role tokens retain their closed directional domain policy", "[graph][catalogue]") {
	using namespace engine::graph;
	for (const auto kind :
		 {ResourceKind::Colour,
		  ResourceKind::Depth,
		  ResourceKind::Texture,
		  ResourceKind::Storage,
		  ResourceKind::Buffer,
		  ResourceKind::Camera,
		  ResourceKind::Entities}) {
		ResourceKind decoded = ResourceKind::Colour;
		CHECK_FALSE(ResourcePortType(kind).empty());
		REQUIRE(ResourceKindFromPortType(ResourcePortType(kind), decoded));
		CHECK(decoded == kind);
	}
	ResourceKind unchanged = ResourceKind::Depth;
	CHECK_FALSE(ResourceKindFromPortType("any", unchanged));
	CHECK(unchanged == ResourceKind::Depth);
	CHECK(ResourcePortType(static_cast<ResourceKind>(255)).empty());
	CHECK(PortsCompatible(ResourceKind::Storage, ResourceKind::Texture));
	CHECK_FALSE(PortsCompatible(ResourceKind::Texture, ResourceKind::Storage));
}

TEST_CASE(
	"render declaration replacement rebuilds derived ports and refuses malformed candidates",
	"[graph][catalogue]"
) {
	using namespace engine::graph;
	struct Restore {
		~Restore() {
			NodeCatalogue::Reset();
			RegisterRenderNodeKinds();
		}
	} restore;
	NodeCatalogue::Reset();
	NodeKindSpec spec;
	spec.Kind = Name("checked");
	spec.Label = "accepted";
	spec.Inputs = {{.Name = Name("image"), .Kind = ResourceKind::Texture}};
	spec.Declaration.Id = "forged";
	spec.Declaration.Ports = {{.Id = "forged", .ValueType = "unrelated"}};
	REQUIRE(NodeCatalogue::Register(spec));
	CHECK(NodeCatalogue::Find(spec.Kind)->Declaration.Id == "checked");
	SECTION("duplicate directional port") {
		spec.Inputs.push_back(spec.Inputs.front());
	}
	SECTION("unknown resource role") {
		spec.Inputs.front().Kind = static_cast<ResourceKind>(255);
	}
	SECTION("empty stable port identity") {
		spec.Inputs.front().Name = {};
	}
	SECTION("over budget count") {
		spec.Inputs.resize(NodeCatalogue::MAXIMUM_SCHEMA_PORTS + 1);
	}
	spec.Label = "refused";
	CHECK_FALSE(NodeCatalogue::Register(spec));
	const auto *accepted = NodeCatalogue::Find(Name("checked"));
	REQUIRE(accepted);
	CHECK(accepted->Label == "accepted");
	REQUIRE(accepted->DeclaredPort(PortDirection::Input, "image"));
	CHECK(accepted->DeclaredPort(PortDirection::Input, "image")->ValueType == "render.texture");
	spec.Inputs.clear();
	spec.Outputs = {{.Name = Name("replacement"), .Kind = ResourceKind::Buffer}};
	REQUIRE(NodeCatalogue::Register(spec));
	const auto *replacement = NodeCatalogue::Find(spec.Kind);
	CHECK(replacement->DeclaredPort(PortDirection::Input, "image") == nullptr);
	REQUIRE(replacement->DeclaredPort(PortDirection::Output, "replacement"));
	CHECK(replacement->DeclaredPort(PortDirection::Output, "replacement")->ValueType == "render.buffer");
}

TEST_CASE(
	"render declaration budgets accept their exact count and identity boundaries", "[graph][catalogue]"
) {
	using namespace engine::graph;
	struct Restore {
		~Restore() {
			NodeCatalogue::Reset();
			RegisterRenderNodeKinds();
		}
	} restore;
	NodeCatalogue::Reset();
	NodeKindSpec spec;
	spec.Kind = Name("boundary");
	SECTION("port count") {
		for (size_t index = 0; index < NodeCatalogue::MAXIMUM_SCHEMA_PORTS; ++index)
			spec.Inputs.push_back(
				{.Name = Name("port-" + std::to_string(index)), .Kind = ResourceKind::Texture}
			);
		REQUIRE(NodeCatalogue::Register(spec));
		CHECK(
			NodeCatalogue::Find(spec.Kind)->Declaration.Ports.size() == NodeCatalogue::MAXIMUM_SCHEMA_PORTS
		);
		spec.Outputs.push_back({.Name = Name("extra")});
		CHECK_FALSE(NodeCatalogue::Register(spec));
		CHECK(
			NodeCatalogue::Find(spec.Kind)->Declaration.Ports.size() == NodeCatalogue::MAXIMUM_SCHEMA_PORTS
		);
	}
	SECTION("identity bytes") {
		const size_t length = NodeCatalogue::MAXIMUM_SCHEMA_IDENTITY_BYTES - spec.Kind.Text().size() -
							  ResourcePortType(ResourceKind::Texture).size();
		std::string name(length, 'x');
		spec.Inputs = {{.Name = Name(name), .Kind = ResourceKind::Texture}};
		REQUIRE(NodeCatalogue::Register(spec));
		CHECK(NodeCatalogue::Find(spec.Kind)->Declaration.Ports.front().Id.size() == length);
		name.push_back('x');
		spec.Inputs.front().Name = Name(name);
		CHECK_FALSE(NodeCatalogue::Register(spec));
		CHECK(NodeCatalogue::Find(spec.Kind)->Declaration.Ports.front().Id.size() == length);
	}
}

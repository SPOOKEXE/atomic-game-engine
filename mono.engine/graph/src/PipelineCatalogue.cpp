#include <engine/graph/PipelineCatalogue.hpp>

#include <algorithm>
#include <initializer_list>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

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

		// The one narrowing rule, and the whole of it: **something that was
		// written may be sampled.** A shadow map is a `Depth` a pass wrote and
		// every lit pass samples; a mirror is a `Colour` in the same position;
		// a compute shader's `Storage` output is exactly the same relationship,
		// and leaving it out made every compute node in the catalogue an island
		// - `engine.graph.pipelinecatalogue`'s "every output can land
		// somewhere" is what found it.
		return to == ResourceKind::Texture &&
			   (from == ResourceKind::Colour || from == ResourceKind::Depth || from == ResourceKind::Storage);
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

	bool IsLossy(ResourceFormat from, ResourceFormat to) {
		if (from == to) {
			return false;
		}

		// **A block-compressed destination is always lossy**, and there is no
		// such thing as a block-compressed render target - so this is really a
		// statement that nothing should be declaring one as an output.
		if (to == ResourceFormat::BC1_SRGB || to == ResourceFormat::BC3 || to == ResourceFormat::BC5 ||
			to == ResourceFormat::BC7_SRGB) {
			return true;
		}

		return BitsPerPixel(to) < BitsPerPixel(from) || ChannelCount(to) < ChannelCount(from);
	}

	std::string_view WhatIsLost(ResourceFormat from, ResourceFormat to) {
		if (!IsLossy(from, to)) {
			return {};
		}
		if (ChannelCount(to) < ChannelCount(from)) {
			return "channels are dropped - what the writer put in them is gone";
		}
		return "precision is dropped - an HDR range lands in fewer bits than it needs";
	}

	namespace {
		bool Named(core::Name kind, std::initializer_list<std::string_view> names) {
			return std::find(names.begin(), names.end(), kind.Text()) != names.end();
		}

		ParameterSpec TextParam(const char *name, const char *label, const char *fallback) {
			return ParameterSpec{
				.Name = core::Name(name),
				.Label = label,
				.Widget = ParameterWidget::Text,
				.Default = fallback,
				.Options = {},
			};
		}

		ParameterSpec NumberParam(
			const char *name, const char *label, const char *fallback, double minimum, double maximum
		) {
			return ParameterSpec{
				.Name = core::Name(name),
				.Label = label,
				.Widget = ParameterWidget::Number,
				.Default = fallback,
				.Options = {},
				.Minimum = minimum,
				.Maximum = maximum,
				.HasRange = true,
			};
		}

		ParameterSpec SelectParam(
			const char *name,
			const char *label,
			const char *fallback,
			std::initializer_list<const char *> options
		) {
			ParameterSpec spec{
				.Name = core::Name(name),
				.Label = label,
				.Widget = ParameterWidget::Select,
				.Default = fallback,
				.Options = {},
			};
			for (const char *option : options) {
				spec.Options.emplace_back(option);
			}
			return spec;
		}

		ExecutionQueue QueueFor(const NodeKindSpec &spec) {
			if (Named(
					spec.Kind,
					{"viewer",
					 "capture",
					 "shadow-capture",
					 "mesh-residency",
					 "delta-upload",
					 "output-image",
					 "blit"}
				)) {
				return ExecutionQueue::Transfer;
			}
			if (Named(
					spec.Kind,
					{"world",
					 "camera",
					 "entities",
					 "cull-frustum",
					 "cull-distance",
					 "filter-tag",
					 "order-draw"}
				)) {
				return ExecutionQueue::Cpu;
			}
			if (Named(
					spec.Kind,
					{"hzb",
					 "select-lod",
					 "skybox-compute",
					 "clouds-compute",
					 "tessellate",
					 "global-illumination",
					 "raytrace",
					 "pathtrace",
					 "dispatch"}
				)) {
				return ExecutionQueue::Compute;
			}
			return ExecutionQueue::Graphics;
		}

		void AddParameters(NodeKindSpec &spec) {
			if (spec.Kind == core::Name("cull-frustum")) {
				spec.Params.push_back(
					SelectParam("culling", "Culling", "inherit", {"inherit", "none", "frustum", "occlusion"})
				);
			}
			if (spec.Kind == core::Name("cull-distance")) {
				spec.Params.push_back(TextParam("radius", "Radius", "0"));
			}
			if (spec.Kind == core::Name("filter-tag")) {
				spec.Params.push_back(TextParam("mask", "Tag mask", "0"));
			}
			if (spec.Kind == core::Name("mirror-capture")) {
				spec.Params.push_back(
					SelectParam("feedback", "Feedback", "last-frame", {"last-frame", "flat"})
				);
				spec.Params.push_back(NumberParam("max-recursion", "Max recursion", "3", 1.0, 32.0));
			}
			if (spec.Kind == core::Name("tessellate")) {
				spec.Params.push_back(NumberParam("target-pixels", "Target edge pixels", "12", 1.0, 128.0));
				spec.Params.push_back(NumberParam("max-subdivisions", "Max subdivisions", "4", 1.0, 8.0));
			}
			if (spec.Kind == core::Name("global-illumination")) {
				spec.Params.push_back(NumberParam("rays-per-pixel", "Rays per pixel", "1", 1.0, 64.0));
				spec.Params.push_back(NumberParam("max-distance", "Max distance", "24", 0.1, 10'000.0));
			}
			if (spec.Kind == core::Name("raytrace")) {
				spec.Params.push_back(NumberParam("steps", "March steps", "32", 1.0, 512.0));
				spec.Params.push_back(NumberParam("max-distance", "Max distance", "100", 0.1, 100'000.0));
				spec.Params.push_back(NumberParam("thickness", "Depth thickness", "0.1", 0.001, 100.0));
			}
			if (spec.Kind == core::Name("pathtrace")) {
				spec.Params.push_back(
					NumberParam("samples-per-frame", "Samples per frame", "1", 1.0, 4096.0)
				);
				spec.Params.push_back(NumberParam("max-bounces", "Max bounces", "3", 1.0, 64.0));
			}
			if (Named(spec.Kind, {"raster", "dispatch"})) {
				spec.Params.push_back(TextParam("shader", "Shader", ""));
				spec.Params.push_back(TextParam("source", "GLSL source", ""));
				spec.Params.push_back(
					SelectParam("attachment", "Visual attachment", "none", {"none", "visual"})
				);
			}
			if (spec.Kind == core::Name("raster")) {
				spec.Params.push_back(SelectParam("load", "Load", "clear", {"clear", "load"}));
			}
			if (Named(
					spec.Kind, {"dispatch", "tessellate", "global-illumination", "raytrace", "pathtrace"}
				)) {
				spec.Params.push_back(
					SelectParam("dispatch.mode", "Dispatch", "target", {"target", "groups"})
				);
				spec.Params.push_back(SelectParam("uniforms", "Uniforms", "view", {"none", "view"}));
				spec.Params.push_back(
					SelectParam("instances", "Instance rows", "none", {"none", "resident"})
				);
				for (const auto &[name, label, fallback] :
					 std::initializer_list<std::tuple<const char *, const char *, const char *>>{
						 {"local.x", "Threads X", "8"},
						 {"local.y", "Threads Y", "8"},
						 {"local.z", "Threads Z", "1"},
						 {"dispatch.x", "Groups X", "1"},
						 {"dispatch.y", "Groups Y", "1"},
						 {"dispatch.z", "Groups Z", "1"},
					 }) {
					spec.Params.push_back(NumberParam(name, label, fallback, 1.0, 65'535.0));
				}
			}
			if (Named(spec.Kind, {"viewer", "capture"})) {
				spec.Params.push_back(NumberParam("view", "Viewport slot", "0", 0.0, 65'535.0));
			}
			if (spec.Kind == core::Name("depth-compose")) {
				spec.Params.push_back(SelectParam(
					"mode", "Foreground mode", "opaque", {"opaque", "transparent", "premultiplied"}
				));
			}
			if (spec.Kind == core::Name("eye-image")) {
				spec.Params.push_back(SelectParam(
					"layer",
					"Image layer",
					"base",
					{"base", "transparent-0", "transparent-1", "spatial-overlay"}
				));
				spec.Params.push_back(SelectParam(
					"scope", "Capture scope", "complete-world", {"complete-world", "opaque-lighting"}
				));
				spec.Params.push_back(
					SelectParam("projection", "Capture projection", "eye", {"eye", "seam"})
				);
			}
			if (spec.Kind == core::Name("depth-linearise"))
				spec.Params.push_back(SelectParam("background", "Background depth", "far", {"far", "zero"}));
			if (spec.Kind == core::Name("capture")) {
				spec.Params.push_back(TextParam("path", "BMP path", ""));
				spec.Params.push_back(
					SelectParam("capture.mode", "Capture", "once", {"once", "every-frame"})
				);
			}
			if (spec.Kind == core::Name("blit")) {
				spec.Params.push_back(SelectParam(
					"format",
					"Target format",
					"RGBA16F",
					{"R8",
					 "RG8",
					 "RGBA8",
					 "RGBA8_SRGB",
					 "RGB10A2",
					 "RG11B10F",
					 "R16F",
					 "RG16F",
					 "RGBA16F",
					 "RGBA32F",
					 "R32F",
					 "RG32F"}
				));
			}
		}

		void AddExecutionMetadata(NodeKindSpec &spec) {
			spec.Queue = QueueFor(spec);
			spec.BuiltInBackend = Named(
				spec.Kind,
				{"world",
				 "shadow",
				 "camera",
				 "entities",
				 "cull-frustum",
				 "cull-distance",
				 "filter-tag",
				 "order-draw",
				 "mesh-residency",
				 "delta-upload",
				 "select-lod",
				 "last-frame",
				 "mirror-capture",
				 "surface-capture",
				 "portal-capture",
				 "portal-tonemap",
				 "forward",
				 "gbuffer",
				 "depth-linearise",
				 "depth-compose",
				 "ambient-response",
				 "ambient-merge",
				 "ambient-correct",
				 "colour-compose",
				 "spatial-overlay",
				 "transparent-layer",
				 "fxaa",
				 "taa",
				 "smaa-edges",
				 "smaa-blend",
				 "smaa-resolve",
				 "hzb",
				 "skybox-compute",
				 "clouds-compute",
				 "ssao",
				 "deferred-lighting",
				 "sky",
				 "fog",
				 "shader-lenses",
				 "tonemap",
				 "eye-image",
				 "portal-overlay",
				 "mirror-overlay",
				 "transparent",
				 "mix",
				 "blit",
				 "raster",
				 "dispatch",
				 "present",
				 "viewer",
				 "capture",
				 "shadow-capture",
				 "overlay",
				 "interface",
				 "output-image",
				 "tessellate",
				 "global-illumination",
				 "raytrace",
				 "pathtrace"}
			);
			spec.Repeatable = Named(
				spec.Kind,
				{"depth-compose",
				 "ambient-response",
				 "ambient-merge",
				 "ambient-correct",
				 "colour-compose",
				 "spatial-overlay",
				 "shader-lenses",
				 "eye-image",
				 "transparent-layer",
				 "fxaa",
				 "taa",
				 "smaa-edges",
				 "smaa-blend",
				 "smaa-resolve",
				 "cull-frustum",
				 "cull-distance",
				 "filter-tag",
				 "order-draw",
				 "blit",
				 "raster",
				 "dispatch",
				 "viewer",
				 "capture",
				 "shadow-capture"}
			);
			spec.FlexibleScope = spec.Kind == core::Name("dispatch");
			spec.Needs.Compute = spec.Queue == ExecutionQueue::Compute;
			spec.Needs.StorageTextures =
				std::any_of(
					spec.Outputs.begin(),
					spec.Outputs.end(),
					[](const PortSpec &port) { return port.Kind == ResourceKind::Storage; }
				) ||
				Named(spec.Kind, {"global-illumination", "raytrace", "pathtrace"});
			spec.Needs.IndirectDraws = Named(
				spec.Kind,
				{"shadow",
				 "mirror-capture",
				 "surface-capture",
				 "portal-capture",
				 "select-lod",
				 "gbuffer",
				 "transparent"}
			);
			for (const PortSpec &output : spec.Outputs) {
				// Ordinary deferred views do not allocate or require the optional capture target.
				if (spec.Kind == core::Name("deferred-lighting") &&
					(output.Name == core::Name("lighting-baseline") ||
					 output.Name == core::Name("directional-response")))
					continue;
				if (output.Kind != ResourceKind::Colour && output.Kind != ResourceKind::Depth &&
					output.Kind != ResourceKind::Texture && output.Kind != ResourceKind::Storage) {
					continue;
				}
				if (std::find(spec.Needs.Formats.begin(), spec.Needs.Formats.end(), output.Format) ==
					spec.Needs.Formats.end()) {
					spec.Needs.Formats.push_back(output.Format);
				}
			}
			spec.Needs.TimestampsUseful = spec.Queue != ExecutionQueue::Cpu;
			AddParameters(spec);
		}
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

	bool RegisterNodeKind(NodeKindSpec spec) {
		return NodeCatalogue::Register(std::move(spec));
	}

	void RegisterRenderNodeKinds() {
		using K = ResourceKind;
		using F = ResourceFormat;
		using C = NodeCategory;
		using S = NodeScope;

		// **A table rather than forty blocks.** The vocabulary is the point; the
		// registration is not, and forty near-identical paragraphs would bury
		// what differs between two kinds under what does not.
		struct Port {
			const char *Name;
			K Kind;
			F Format;
			bool Required;
			const char *Summary;
			bool InternalUse = false;
		};

		struct Kind {
			const char *Name;
			const char *Label;
			C Category;
			NodeScope Scope;
			std::vector<Port> Inputs;
			std::vector<Port> Outputs;
			const char *Summary;

			// Whether it reads nothing from the frame. See `NodeKindSpec::Source`
			// - the four that set it are the four that legitimately have no
			// inputs.
			bool Source = false;

			// The shader the engine ships for this kind, if it is a fullscreen
			// effect with one correct implementation. See
			// `NodeKindSpec::DefaultShader`.
			const char *Shader = "";
		};

		// Shorthands, so a row fits on a line and the table reads as a table.
		constexpr F RGBA8 = F::RGBA8;
		constexpr F RGBA16 = F::RGBA16F;
		constexpr F RG16 = F::RG16F;
		constexpr F R32 = F::R32F;
		constexpr F D24 = F::D24S8;
		constexpr F D32 = F::D32F;
		constexpr F HDR = F::RG11B10F;
		constexpr F LDR = F::RGB10A2;

		const std::vector<Kind> table = {
			// --- geometry: passes that rasterise the world ---------------------
			{"depth-prepass",
			 "Depth pre-pass",
			 C::Draw,
			 S::View,
			 {{"entities", K::Entities, F::R8, false, "What to lay depth for."}},
			 {{"depth", K::Depth, D24, true, "Scene depth, before any shading."}},
			 "Depth only, so the base pass shades each pixel once. Worth about 7% "
			 "on a frame that lacks it.",
			 true},

			{"shadow",
			 "Shadow",
			 C::Draw,
			 // **Per world, and this is the distinction the boolean could not
			 // make.** Four split-screen views of one world sample one atlas;
			 // two worlds need two. `RenderGraph::Execute` was already running
			 // the shared block once per distinct world - the vocabulary is
			 // only now catching up with it.
			 S::World,
			 {{"entities",
			   K::Entities,
			   F::R8,
			   false,
			   "The casters. Deliberately not frustum culled - one off screen still shadows in."},
			  {"meshes", K::Buffer, F::R8, true, "Resident ranges and world upload completion."}},
			 {{"shadow", K::Depth, D32, true, "The light's depth atlas."}},
			 "Draws the casters from the light, into a depth map every view samples.",
			 false},

			{"last-frame",
			 "Last Frame",
			 C::Composite,
			 S::View,
			 {},
			 {{"image", K::Colour, LDR, true, "The previous completed image for this viewport."}},
			 "Exposes the viewport's previous completed image without creating a same-frame cycle.",
			 true},

			{"mirror-capture",
			 "Mirror Capture",
			 C::Draw,
			 S::View,
			 {{"last-frame", K::Texture, LDR, true, "The previous completed viewport image."},
			  {"world-state", K::Entities, F::R8, true, "The complete world state mirrors may reveal."},
			  {"shadow", K::Texture, D32, false, "Shadows, if any."},
			  {"entities", K::Entities, F::R8, false, "What each mirror draws."},
			  {"instances", K::Buffer, F::R8, false, "The uploaded instance attributes."}},
			 {{"surface", K::Colour, RGBA8, true, "One image per surface index."}},
			 "Renders each mirror's own view. Deeper panes resolve at most three levels and use the "
			 "previous frame when that bound is reached."},

			{"surface-capture",
			 "Surface Capture",
			 C::Draw,
			 S::View,
			 {{"world-state", K::Entities, F::R8, true, "The complete world revealed by child cameras."},
			  {"shadow", K::Texture, D32, false, "The owning world's shadows."},
			  {"entities", K::Entities, F::R8, false, "The ordered world draw ranges."},
			  {"instances", K::Buffer, F::R8, false, "Uploaded instance attributes."}},
			 {{"surface", K::Colour, HDR, true, "Root mirror radiance."},
			  {"portal", K::Texture, HDR, true, "Root and child portal radiance."},
			  {"light", K::Texture, HDR, true, "Each mouth's light field."}},
			 "Captures a bounded mixed mirror and portal tree in child-first order, before composition."},

			{"portal-capture",
			 "Portal Capture",
			 C::Draw,
			 S::View,
			 {{"shadow", K::Texture, D32, false, "Shadows, if any."},
			  {"entities", K::Entities, F::R8, false, "What each portal view draws."},
			  {"instances", K::Buffer, F::R8, false, "The uploaded instance attributes."}},
			 {{"portal", K::Texture, LDR, true, "One lit image per portal and recursion level."},
			  {"light",
			   K::Texture,
			   LDR,
			   false,
			   "Each mouth's room rendered against a lit void: the seam's light field."}},
			 "Captures recursively warped portal views with the lighting of the world being shown, and "
			 "each seam's room against a lit void for the light-spill projection."},

			{"portal-tonemap",
			 "Portal Tone Map",
			 C::Composite,
			 S::View,
			 {{"portal", K::Texture, LDR, true, "The linear-lit portal captures."}},
			 {{"portal", K::Texture, LDR, true, "Portal captures in the scene display transform."}},
			 "Applies the scene tone curve to the top portal level before it is projected over the "
			 "tone-mapped scene."},

			{"gbuffer",
			 "G-buffer",
			 C::Draw,
			 S::View,
			 {{"shadow", K::Texture, D32, false, "Shadows, if any."},
			  {"entities", K::Entities, F::R8, false, "What to fill the buffers from."},
			  {"instances", K::Buffer, F::R8, false, "The uploaded instance attributes."}},
			 {{"albedo", K::Colour, RGBA8, true, "Base colour. Alpha is opacity."},
			  {"normal", K::Colour, LDR, true, "World normals. Ten bits an axis is enough."},
			  {"material", K::Colour, RGBA8, true, "Roughness, metalness, and material tags."},
			  {"emissive", K::Colour, RGBA16, true, "Light emitted by the surface before exposure."},
			  {"depth", K::Depth, D24, true, "Scene depth."}},
			 "The deferred split of the opaque pass: surface properties, not light. "
			 "**Built** - three targets, and the engine ships the shader. Declare "
			 "albedo, normal and material in that order; a node declaring fewer is "
			 "refused rather than drawn short."},

			{"forward",
			 "Forward",
			 C::Draw,
			 S::View,
			 {{"entities", K::Entities, F::R8, false, "What to draw."},
			  {"instances", K::Buffer, F::R8, false, "The uploaded instance attributes."}},
			 {{"colour", K::Colour, LDR, true, "The directly lit scene."},
			  {"depth", K::Depth, D24, true, "Depth for the directly lit scene."}},
			 "A reduced direct-lighting path for devices that cannot allocate the deferred frame."},

			{"velocity",
			 "Velocity",
			 C::Draw,
			 S::View,
			 {},
			 {{"velocity", K::Colour, RG16, true, "Per-pixel screen motion."}},
			 "Where every pixel was last frame. Feeds temporal resolve and motion blur.",
			 true},

			{"transparent",
			 "Transparent",
			 C::Draw,
			 S::View,
			 {{"colour", K::Colour, RGBA16, true, "What to blend over."},
			  {"depth", K::Depth, D24, true, "The opaque depth."},
			  {"surface", K::Texture, RGBA16, false, "Mirror and portal images to project."},
			  {"entities", K::Entities, F::R8, false, "The blended tail, already ordered back to front."},
			  {"instances", K::Buffer, F::R8, false, "The uploaded instance attributes."}},
			 {{"colour", K::Colour, RGBA16, true, "The blended frame."}},
			 "The blended tail, back to front, tested against the opaque depth."},

			{"sky",
			 "Sky",
			 C::Draw,
			 S::View,
			 {{"colour", K::Colour, RGBA16, true, "The lit scene to preserve."},
			  {"depth", K::Depth, D24, true, "So it fills only what nothing covered."},
			  {"environment", K::Texture, RGBA16, false, "The completed world environment."}},
			 {{"colour", K::Colour, RGBA16, true, "The background."}},
			 "The background, drawn where the depth buffer is still far."},

			{"skybox-compute",
			 "Skybox compute",
			 C::Composite,
			 S::World,
			 {},
			 {{"sky", K::Storage, RGBA16, true, "The fixed-resolution linear sky history."}},
			 "Generates the sky and atmosphere once per changed world environment.",
			 true},

			{"clouds-compute",
			 "Clouds compute",
			 C::Composite,
			 S::World,
			 {{"sky", K::Texture, RGBA16, true, "The generated sky and atmosphere."}},
			 {{"clouds", K::Storage, RGBA16, true, "The completed world environment history."}},
			 "Composites clouds into the stable world environment image."},

			{"particles",
			 "Particles",
			 C::Draw,
			 S::View,
			 {{"colour", K::Colour, RGBA16, true, "What to blend over."},
			  {"depth", K::Depth, D24, false, "For soft particles."}},
			 {{"colour", K::Colour, RGBA16, true, "The frame with sprites on it."}},
			 "The emitter pool's live particles, as camera-facing sprites."},

			{"decals",
			 "Decals",
			 C::Draw,
			 S::View,
			 {{"depth", K::Texture, R32, true, "Linear depth, to project against."},
			  {"normal", K::Texture, LDR, false, "For box-projection decals."}},
			 {{"albedo", K::Colour, RGBA8, true, "The modified surface."},
			  {"material", K::Colour, RGBA8, true, "And its properties."}},
			 "Projected modifications to the G-buffer, after it is filled."},

			// --- depth derivatives ---------------------------------------------
			{"depth-linearise",
			 "Linearise depth",
			 C::Composite,
			 S::View,
			 {{"depth", K::Texture, D24, true, "Hardware depth."},
			  {"after-colour",
			   K::Texture,
			   RGBA16,
			   false,
			   "Wait for a colour pass that also writes the shared hardware depth."}},
			 {{"linear", K::Colour, R32, true, "Linear view-space depth."}},
			 "Hardware depth to linear float. Cheap as a blit; expensive as a "
			 "full-screen triangle, which is how most engines do it.",
			 false,
			 "depth-linearise.frag"},

			{"hzb",
			 "Hierarchical Z",
			 C::Composite,
			 S::View,
			 {{"depth", K::Texture, R32, true, "Linear depth."}},
			 {{"hzb", K::Storage, R32, true, "A min pyramid, for occlusion and SSR."}},
			 "The depth pyramid occlusion culling and screen-space tracing walk."},

			{"depth-discontinuity",
			 "Depth edges",
			 C::Composite,
			 S::View,
			 {{"depth", K::Texture, R32, true, "Linear depth."}},
			 {{"edges", K::Colour, RGBA16, true, "Min, max and edge strength per 2x2."}},
			 "Where depth jumps, packed per 2x2. What light culling and upscaling "
			 "both want and neither should recompute.",
			 false,
			 "depth-discontinuity.frag"},

			// --- lighting -------------------------------------------------------
			{"light-bounds",
			 "Light bounds",
			 C::Composite,
			 S::View,
			 {{"depth", K::Texture, D24, true, "A downscaled depth to test against."},
			  {"edges", K::Texture, RGBA16, false, "Depth discontinuities."}},
			 {{"lights", K::Buffer, RGBA8, true, "The per-tile light list."}},
			 "Rasterises each light's volume to build the tile lists. Measured "
			 "faster than compute culling in at least one shipping engine."},

			{"deferred-lighting",
			 "Deferred lighting",
			 C::Composite,
			 S::View,
			 {{"albedo", K::Texture, RGBA8, true, "Base colour."},
			  {"normal", K::Texture, LDR, true, "World normals."},
			  {"material", K::Texture, RGBA8, true, "Roughness and metalness."},
			  {"emissive", K::Texture, RGBA16, true, "Surface emission in linear HDR."},
			  {"depth", K::Texture, R32, true, "Linear depth."},
			  {"occlusion", K::Texture, F::R8, false, "Ambient visibility, one when open."},
			  {"lights", K::Buffer, RGBA8, false, "The tile lists."},
			  {"shadow", K::Texture, D32, false, "The shadow atlas."},
			  {"portal-light",
			   K::Texture,
			   LDR,
			   false,
			   "Seam light-field captures, projected out of each portal entrance."}},
			 {{"colour", K::Colour, RGBA16, true, "The lit frame."},
			  {"lighting-baseline",
			   K::Colour,
			   F::RGBA32F,
			   false,
			   "The same lighting before half-float storage."},
			  {"directional-response",
			   K::Colour,
			   F::RGBA32F,
			   false,
			   "Directional radiance per visibility and original visibility. Requires lighting-baseline."}},
			 "Shades the G-buffer. One pass over the screen, however much geometry "
			 "went into it.",
			 false,
			 "deferred-lighting.frag"},

			{"shadow-project",
			 "Shadow projection",
			 C::Composite,
			 S::View,
			 {{"shadow", K::Texture, D32, true, "The atlas."},
			  {"depth", K::Texture, R32, true, "Linear scene depth."}},
			 {{"mask", K::Colour, F::R8, true, "A screen-space shadow mask."}},
			 "Resolves the shadow atlas into a screen mask. Restrict it with the "
			 "stencil or it shades pixels no light reaches."},

			{"ssao",
			 "Ambient occlusion",
			 C::Composite,
			 S::View,
			 {{"depth", K::Texture, R32, true, "Linear depth."},
			  {"normal", K::Texture, LDR, true, "World normals."}},
			 {{"occlusion", K::Colour, F::R8, true, "How shut in each pixel is."}},
			 "Screen-space ambient occlusion. Noisy by nature; wants a depth-aware "
			 "blur after it. **The engine ships the shader** - drop the node and "
			 "it works; name your own and yours wins.",
			 false,
			 "ssao.frag"},

			{"ssr",
			 "Screen-space reflections",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, RGBA16, true, "Last frame's lit colour."},
			  {"depth", K::Texture, R32, true, "Linear depth."},
			  {"normal", K::Texture, LDR, true, "World normals."},
			  {"hzb", K::Texture, R32, false, "To march the ray cheaply."}},
			 {{"reflection", K::Colour, RGBA16, true, "Reflected colour and confidence."}},
			 "Marches rays through the depth buffer. Should cost less than tracing "
			 "the real geometry, and often does not.",
			 false,
			 "ssr.frag"},

			{"raytrace",
			 "Ray trace",
			 C::Composite,
			 S::View,
			 {{"scene", K::Texture, RGBA16, true, "Radiance to reflect."},
			  {"depth", K::Texture, R32, true, "Linear depth."},
			  {"normal", K::Texture, LDR, true, "World normals and material tags."},
			  {"material", K::Texture, RGBA8, true, "Roughness, to pick which pixels trace."},
			  {"indirect", K::Texture, RGBA16, false, "Optional indirect-light estimate."}},
			 {{"reflection", K::Storage, RGBA16, true, "Screen-space reflected radiance."}},
			 "Marches low-roughness screen-space rays through the current depth and radiance. "
			 "It is not hardware ray tracing and has no off-screen hit guarantee.",
			 false,
			 "raytrace.comp"},

			{"tessellate",
			 "Tessellation factor field",
			 C::Draw,
			 S::View,
			 {{"instances", K::Buffer, F::R8, true, "LOD-selected source instances."},
			  {"camera", K::Camera, F::R8, true, "The projection that sets edge density."}},
			 {{"factors", K::Storage, F::R16F, true, "Screen-space tessellation factor field."}},
			 "Builds a screen-space factor field for a future geometry tessellation pass. No draw node "
			 "consumes it yet.",
			 false,
			 "tessellate.comp"},

			{"global-illumination",
			 "Global illumination",
			 C::Composite,
			 S::View,
			 {{"albedo", K::Texture, RGBA8, true, "Surface base colour."},
			  {"normal", K::Texture, LDR, true, "Surface normal."},
			  {"material", K::Texture, RGBA8, true, "Surface roughness and metalness."},
			  {"depth", K::Texture, R32, true, "Linear view depth."},
			  {"occlusion", K::Texture, F::R8, false, "Ambient visibility, when available."}},
			 {{"indirect", K::Storage, RGBA16, true, "Estimated indirect radiance."}},
			 "Estimates one-bounce indirect radiance from the visible G-buffer, composed with direct "
			 "lighting.",
			 false,
			 "global-illumination.comp"},

			{"pathtrace",
			 "Path trace",
			 C::Composite,
			 S::View,
			 {{"camera", K::Camera, F::R8, true, "The camera that owns ray generation."},
			  {"entities", K::Entities, F::R8, true, "Visible scene identity and material lookup."},
			  {"instances", K::Buffer, F::R8, true, "Tessellated or LOD-selected geometry rows."},
			  {"albedo", K::Texture, RGBA8, true, "Visible surface base colour."},
			  {"normal", K::Texture, LDR, true, "Visible surface normal."},
			  {"material", K::Texture, RGBA8, true, "Visible surface material."},
			  {"depth", K::Texture, R32, true, "Visible hit distance."},
			  {"indirect", K::Texture, RGBA16, false, "Optional one-bounce guide."}},
			 {{"radiance", K::Storage, RGBA16, true, "One-sample screen-space transport estimate."}},
			 "Builds a bounded, stochastic one-bounce estimate from the visible G-buffer. It has no "
			 "accumulation, acceleration structure, or off-screen hits.",
			 false,
			 "pathtrace.comp"},

			{"fog",
			 "Volumetric fog",
			 C::Composite,
			 S::View,
			 {{"colour", K::Colour, RGBA16, true, "The lit frame."},
			  {"depth", K::Texture, R32, true, "Linear depth."}},
			 {{"colour", K::Colour, RGBA16, true, "The frame with fog in it."}},
			 "Froxel fog, composited over the frame. Use blue noise, not an "
			 "interleaved gradient, or the pattern shows."},

			{"shader-lenses",
			 "Shader lenses",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, RGBA16, true, "The HDR scene behind every lens."},
			  {"depth", K::Texture, R32, true, "Linear depth for spatial occlusion."}},
			 {{"colour", K::Colour, RGBA16, true, "The lensed HDR scene."},
			  {"scratch",
			   K::Colour,
			   RGBA16,
			   true,
			   "Intermediate HDR image for the ordered lens chain.",
			   true}},
			 "Composes bounded world-space lens shader runs in priority order before tone mapping."},

			// --- composite -------------------------------------------------------
			{"temporal-reconstruct",
			 "Temporal reconstruct",
			 C::Composite,
			 S::View,
			 {{"current", K::Texture, RGBA16, true, "This frame, at trace resolution."},
			  {"history", K::Texture, RGBA16, true, "Last frame's accumulation."},
			  {"velocity", K::Texture, RG16, true, "Where each pixel came from."}},
			 {{"resolved", K::Colour, RGBA16, true, "Accumulated, at full resolution."},
			  {"history", K::Colour, RGBA16, true, "Kept for next frame."}},
			 "Accumulates a sub-resolution buffer against history. The output that "
			 "feeds next frame is why history resources exist."},

			{"mix",
			 "Mix",
			 C::Composite,
			 S::View,
			 {{"a", K::Texture, RGBA16, true, "The bottom image."},
			  {"b", K::Texture, RGBA16, true, "The top image."},
			  {"factor", K::Texture, F::R8, false, "Per-pixel blend, if any."}},
			 {{"colour", K::Colour, RGBA16, true, "The blend."}},
			 "Two images and a blend mode. The most-used node in any compositor.",
			 false,
			 "mix.frag"},

			{"blur",
			 "Blur",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, true, "What to blur."},
			  {"depth", K::Texture, R32, false, "For a depth-aware blur."}},
			 {{"colour", K::Colour, RGBA16, true, "The blurred image."}},
			 "Gaussian, box or directional. Depth-aware when given a depth input.",
			 false,
			 "blur.frag"},

			{"bloom",
			 "Bloom",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, true, "The lit frame."}},
			 {{"bloom", K::Colour, F::RGBA16F, true, "The bright parts, spread."}},
			 "Downsample, blur, upsample. Reuses its own chain, so it is one node "
			 "rather than a dozen.",
			 false,
			 "bloom.frag"},

			{"dof",
			 "Depth of field",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, RGBA16, true, "The frame."},
			  {"depth", K::Texture, R32, true, "To decide the circle of confusion."}},
			 {{"colour", K::Colour, RGBA16, true, "Near and far blurred, composited."}},
			 "Near and far fields at half resolution, composited back.",
			 false,
			 "dof.frag"},

			{"motion-blur",
			 "Motion blur",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, RGBA16, true, "The frame."},
			  {"velocity", K::Texture, RG16, true, "Screen motion."}},
			 {{"colour", K::Colour, RGBA16, true, "Smeared along motion."}},
			 "Smears each pixel along its velocity.",
			 false,
			 "motion-blur.frag"},

			{"exposure",
			 "Exposure",
			 C::Composite,
			 S::Frame,
			 {{"luminance", K::Texture, RG16, true, "A 1x1 average, from a reduce chain."}},
			 {{"exposure", K::Buffer, RG16, true, "The scale the tone mapper applies."}},
			 "Turns an average luminance into an exposure scale, with adaptation "
			 "over time."},

			{"transparent-layer",
			 "Capture transparent depth layer",
			 C::Composite,
			 S::View,
			 {{"opaque-z", K::Depth, D32, true, "Opaque device depth from the same projection."},
			  {"previous-z", K::Depth, D32, false, "Previous peeled device depth; one is empty."},
			  {"entities", K::Buffer, RG16, true, "Ordered entity slots."},
			  {"instances", K::Buffer, RG16, true, "Uploaded instance data."},
			  {"shadow", K::Texture, D32, false, "Current shadow atlas, if used by the surface lighting."}},
			 {{"colour",
			   K::Colour,
			   RGBA16,
			   true,
			   "Premultiplied radiance of all fragments at the selected depth."},
			  {"depth", K::Colour, R32, true, "Layer camera-forward distance; zero is empty."},
			  {"z", K::Depth, D32, true, "Private nearest-fragment depth attachment."},
			  {"interface-colour", K::Colour, RGBA16, false, "Per-batch spatial interface scratch colour."},
			  {"interface-z", K::Depth, D32, false, "Per-batch spatial interface scratch depth."}},
			 "Peels ordinary transparent geometry and depth-tested spatial interface batches before "
			 "blending. "
			 "Unsupported surface or custom-shader geometry rows "
			 "refuse capture. "
			 "Capture an extra layer to detect overflow before publishing a bounded set.",
			 false,
			 "transparent-layer.frag"},

			{"spatial-overlay",
			 "Spatial UI overlay",
			 C::Composite,
			 S::View,
			 {},
			 {{"colour", K::Colour, RGBA16, true, "Premultiplied always-on-top spatial UI."},
			  {"z", K::Depth, F::D32F, true, "Scratch attachment required by spatial UI pipelines."}},
			 "Captures top-only world-space UI without physical depth ordering. Screen UI is excluded.",
			 true},

			{"colour-compose",
			 "Compose colour layers",
			 C::Composite,
			 S::View,
			 {{"foreground", K::Colour, RGBA16, true, "Premultiplied overlay."},
			  {"background", K::Colour, RGBA16, true, "Background radiance."}},
			 {{"colour", K::Colour, RGBA16, true, "Premultiplied source-over result."}},
			 "Blends a depth-free overlay over background radiance. Depth remains a separate graph resource.",
			 false,
			 "colour-compose.frag"},

			{"ambient-response",
			 "Retain ambient response",
			 C::Composite,
			 S::View,
			 {{"albedo", K::Texture, RGBA8, true, "Native base colour."},
			  {"normal", K::Texture, LDR, true, "Native normal and validity."},
			  {"material", K::Texture, RGBA8, true, "Native material including AO."},
			  {"depth", K::Texture, R32, true, "Linear camera depth."},
			  {"occlusion", K::Texture, F::R8, true, "Original screen-space ambient visibility."}},
			 {{"response",
			   K::Colour,
			   F::RGBA32F,
			   true,
			   "Fog-attenuated ambient response and original sampled AO."}},
			 "Retains the ambient-only response used to recompute AO without changing other radiance.",
			 false,
			 "ambient-response.frag"},
			{"ambient-merge",
			 "Merge ambient geometry",
			 C::Composite,
			 S::View,
			 {{"body-depth", K::Texture, R32, true, "Current body depth, far for background."},
			  {"body-normal", K::Texture, LDR, true, "Current body normal and validity."},
			  {"room-depth", K::Texture, R32, true, "Retained room depth, zero for background."},
			  {"room-normal", K::Texture, LDR, true, "Retained room normal and validity."}},
			 {{"depth", K::Colour, R32, true, "Merged native AO depth, far for background."},
			  {"normal", K::Colour, LDR, true, "Visible surface normal and validity."}},
			 "Selects room or body geometry in one room's depth domain before SSAO and aperture composition.",
			 false,
			 "ambient-merge.frag"},
			{"ambient-correct",
			 "Correct retained ambient",
			 C::Composite,
			 S::View,
			 {{"lighting-baseline", K::Texture, F::RGBA32F, true, "Unrounded retained room radiance."},
			  {"response", K::Texture, F::RGBA32F, true, "Ambient response and original sampled AO."},
			  {"occlusion", K::Texture, F::R8, true, "Merged ambient visibility."},
			  {"directional-response",
			   K::Texture,
			   F::RGBA32F,
			   false,
			   "Directional response and original visibility; requires depth, normal and shadow."},
			  {"room-depth", K::Texture, R32, false, "Retained linear depth in the current camera domain."},
			  {"room-normal", K::Texture, LDR, false, "Retained normal in the current world domain."},
			  {"shadow",
			   K::Texture,
			   D32,
			   false,
			   "Combined directional map in the current light projection."}},
			 {{"colour", K::Colour, RGBA16, true, "Room radiance with current ambient occlusion."}},
			 "Corrects retained ambient visibility and optionally directional visibility with a complete "
			 "shadow input group.",
			 false,
			 "ambient-correct.frag"},

			{"depth-compose",
			 "Compose depth layers",
			 C::Composite,
			 S::View,
			 {{"foreground",
			   K::Colour,
			   RGBA16,
			   true,
			   "Foreground radiance; straight or premultiplied alpha as selected by mode."},
			  {"foreground-depth", K::Colour, R32, true, "Camera-forward distance, zero for no surface."},
			  {"background",
			   K::Colour,
			   RGBA16,
			   true,
			   "Background radiance; premultiplied in transparent mode."},
			  {"background-depth", K::Colour, R32, true, "Distance in the same camera and world units."}},
			 {{"colour", K::Colour, RGBA16, true, "Composed radiance; premultiplied in transparent mode."},
			  {"depth", K::Colour, R32, true, "Opaque surface camera-forward distance."}},
			 "Composes opaque layers sharing one capture projection, with matched extents within each pair. "
			 "Equal depths retain the background. "
			 "Transparent mode blends straight-alpha foreground in front of opaque depth and preserves that "
			 "depth. "
			 "Apply transparent layers back to front, then spatial lenses.",
			 false,
			 "depth-compose.frag"},

			{"eye-image",
			 "Remote eye image",
			 C::Composite,
			 S::View,
			 {},
			 {{"colour", K::Colour, RGBA16, true, "Owned destination HDR radiance."},
			  {"depth", K::Colour, R32, false, "Paired camera-forward depth; zero means no surface."},
			  {"normal", K::Colour, LDR, false, "Retained native normal and validity."},
			  {"ambient-response",
			   K::Colour,
			   F::RGBA32F,
			   false,
			   "Retained ambient response and original AO."},
			  {"lighting-baseline", K::Colour, F::RGBA32F, false, "Retained unrounded room lighting."},
			  {"directional-response",
			   K::Colour,
			   F::RGBA32F,
			   false,
			   "Retained directional response and shadow visibility."}},
			 "Copies the viewport's owned image before local tone mapping and interface composition. "
			 "The selected scope and projection must match the capture. Seam projection supplies an "
			 "intermediate for mapped body composition; opaque lighting precedes later scene effects.",
			 true},

			{"tonemap",
			 "Tone map",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, RGBA16, true, "The HDR frame."},
			  {"bloom", K::Texture, F::RGBA16F, false, "Composited while we are here."},
			  {"exposure", K::Buffer, RG16, false, "How much to scale by."},
			  {"lut", K::Texture, RGBA8, false, "A colour grade."}},
			 {{"colour", K::Colour, LDR, true, "Display range."}},
			 "HDR to display, with bloom and grading folded in so the frame is read "
			 "once.",
			 false,
			 "tonemap.frag"},

			{"portal-overlay",
			 "Portal Overlay",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, RGBA16, true, "The HDR scene below the portals."},
			  {"depth", K::Depth, D24, true, "The opaque scene depth."},
			  {"portal", K::Texture, RGBA16, true, "The HDR portal captures."},
			  {"entities", K::Entities, F::R8, false, "The ordered portal panes."},
			  {"instances", K::Buffer, F::R8, false, "The uploaded instance attributes."}},
			 {{"colour", K::Colour, RGBA16, true, "The scene with portal openings composed."}},
			 "Projects portal captures onto their entrance geometry before lenses, tone mapping and "
			 "transparent geometry."},

			{"mirror-overlay",
			 "Mirror Overlay",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, RGBA16, true, "The scene below the mirror panes."},
			  {"depth", K::Depth, D24, true, "The opaque scene depth."},
			  {"surface", K::Texture, RGBA16, false, "The captured mirror views."},
			  {"entities", K::Entities, F::R8, false, "The ordered mirror panes."},
			  {"instances", K::Buffer, F::R8, false, "The uploaded instance attributes."}},
			 {{"colour", K::Colour, RGBA16, true, "The scene with mirror panes composed."}},
			 "Projects each captured mirror view onto its pane before ordinary transparent geometry."},

			{"taa",
			 "Temporal AA",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, LDR, true, "This frame."},
			  {"history", K::Texture, LDR, true, "Last frame's output."},
			  {"velocity", K::Texture, RG16, true, "Screen motion."}},
			 {{"colour", K::Colour, LDR, true, "Resolved."},
			  {"history", K::Colour, LDR, true, "Kept for next frame."}},
			 "Jittered accumulation. The more competent it is, the less it can be "
			 "abused to hide undersampling.",
			 false,
			 "taa.frag"},

			{"fxaa",
			 "FXAA",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, LDR, true, "The tone-mapped frame."}},
			 {{"colour", K::Colour, LDR, true, "Edge-smoothed."}},
			 "Single-pass luminance edge search for a low-cost spatial antialiasing choice.",
			 false,
			 "fxaa.frag"},

			{"smaa-edges",
			 "SMAA edges",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, LDR, true, "The tone-mapped frame."}},
			 {{"edges", K::Colour, F::RG8, true, "A two-channel edge mask."}},
			 "First of three. Writes the stencil so the blend pass can skip "
			 "everything it did not touch.",
			 false,
			 "smaa-edges.frag"},

			{"smaa-blend",
			 "SMAA blend",
			 C::Composite,
			 S::View,
			 {{"edges", K::Texture, F::RG8, true, "The edge mask."}},
			 {{"weights", K::Colour, RGBA8, true, "Blending weights."}},
			 "Second of three.",
			 false,
			 "smaa-blend.frag"},

			{"smaa-resolve",
			 "SMAA resolve",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, LDR, true, "The tone-mapped frame."},
			  {"weights", K::Texture, RGBA8, true, "Blending weights."}},
			 {{"colour", K::Colour, LDR, true, "Edge-smoothed."}},
			 "Third of three.",
			 false,
			 "smaa-resolve.frag"},

			{"sharpen",
			 "Sharpen",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, LDR, true, "The frame."}},
			 {{"colour", K::Colour, LDR, true, "Sharpened."}},
			 "Contrast-adaptive sharpening, to claw back what a temporal resolve "
			 "blurred.",
			 false,
			 "sharpen.frag"},

			{"grade",
			 "Grade",
			 C::Composite,
			 S::View,
			 {{"colour", K::Texture, LDR, true, "The frame."},
			  {"lut", K::Texture, RGBA8, false, "A lookup table."}},
			 {{"colour", K::Colour, LDR, true, "Graded."}},
			 "Vignette, grain, chromatic aberration and a LUT. One pass, because "
			 "each of them alone is a full-screen read.",
			 false,
			 "grade.frag"},

			{"separate",
			 "Separate channels",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, true, "Any image."}},
			 {{"r", K::Colour, F::R16F, true, "Red."},
			  {"g", K::Colour, F::R16F, true, "Green."},
			  {"b", K::Colour, F::R16F, true, "Blue."},
			  {"a", K::Colour, F::R16F, true, "Alpha."}},
			 "Splits a target into channels. How an empty alpha becomes visible "
			 "without a capture tool."},

			{"combine",
			 "Combine channels",
			 C::Composite,
			 S::View,
			 {{"r", K::Texture, F::R16F, true, "Red."},
			  {"g", K::Texture, F::R16F, true, "Green."},
			  {"b", K::Texture, F::R16F, true, "Blue."},
			  {"a", K::Texture, F::R16F, false, "Alpha."}},
			 {{"colour", K::Colour, RGBA16, true, "The packed image."}},
			 "The other half of Separate."},

			// --- resample --------------------------------------------------------
			{"scale",
			 "Scale",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, true, "What to resample."}},
			 {{"colour", K::Colour, RGBA16, true, "At the declared size."}},
			 "To a fraction or an absolute size. The node that makes resolution "
			 "something an author sets."},

			{"reduce-chain",
			 "Reduce",
			 C::Composite,
			 S::Frame,
			 {{"source", K::Texture, RGBA16, true, "What to reduce."}},
			 {{"result", K::Storage, RG16, true, "Down to 1x1, if asked."}},
			 "Repeated halving to a fixed size. One node, not seven, because "
			 "nobody wires the intermediate steps by hand."},

			{"upscale",
			 "Upscale",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, true, "The low-resolution frame."},
			  {"depth", K::Texture, R32, false, "For a depth-aware upscale."},
			  {"edges", K::Texture, RGBA16, false, "Depth discontinuities."}},
			 {{"colour", K::Colour, RGBA16, true, "At full resolution."}},
			 "Spatial upscale. Depth-aware when given depth, which is the "
			 "difference between usable and pixelated."},

			{"blit",
			 "Blit",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, true, "What to copy."}},
			 {{"colour", K::Colour, RGBA16, true, "The copy."}},
			 "A straight copy. Its own kind so that a copy done as a full-screen "
			 "triangle shows up as the mistake it is.",
			 false,
			 "blit.frag"},

			{"clear",
			 "Clear",
			 C::Composite,
			 S::View,
			 {},
			 {{"target", K::Colour, RGBA16, true, "Cleared to a constant."}},
			 "Explicit, because an implicit clear is a cost nobody sees and "
			 "sometimes a cost nobody needs.",
			 true},

			{"dispatch",
			 "Compute",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, false, "Whatever it reads."}},
			 {{"result", K::Storage, RGBA16, true, "Whatever it writes."}},
			 "A named or live GLSL compute shader. Sampled inputs use set 0, storage "
			 "outputs use set 1, and `local.x/y/z` must match the shader. Target mode "
			 "covers the output; groups mode uses `dispatch.x/y/z`."},

			// --- interface and output ---------------------------------------------
			{"overlay",
			 "Image Overlay",
			 C::Interface,
			 S::Frame,
			 {{"scene", K::Texture, LDR, true, "The image below."},
			  {"interface", K::Texture, LDR, false, "The image above."}},
			 {{"image", K::Colour, LDR, true, "The composed image."}},
			 "Composes an in-game interface image over a scene image. Debug drawing joins the "
			 "same composition instead of bypassing the graph."},

			{"interface",
			 "Interface Image",
			 C::Interface,
			 S::Frame,
			 {},
			 {{"image", K::Colour, LDR, true, "The in-game widget tree on transparency."}},
			 "Renders the in-game widget tree to a transparent image for a compositor. "
			 "Studio chrome is a host overlay outside the universe graph.",
			 true},

			{"present",
			 "Scene Image",
			 C::Output,
			 S::Frame,
			 {{"image", K::Texture, LDR, true, "Any image produced by the view graph."}},
			 {{"image", K::Colour, LDR, true, "The scene image at frame resolution."}},
			 "Turns any view image, including depth and intermediate targets, into the "
			 "scene image consumed by frame compositors."},

			// --- the entity flow ---------------------------------------------
			//
			// **What a pass draws, as wires.** Everything above describes what is
			// drawn *into*; these describe what goes in. They carry
			// `ResourceKind::Entities` - a list of indices into the view's draw
			// list - and every one of them is list-in, list-out, so they compose
			// in any order somebody wires them.
			{"raster",
			 "Custom raster",
			 C::Composite,
			 S::View,
			 {{"source", K::Texture, RGBA16, false, "What it samples, in slot order."},
			  {"second", K::Texture, RGBA16, false, "A second input, if the shader takes one."}},
			 {{"colour", K::Colour, RGBA16, true, "Whatever it drew."}},
			 "A fullscreen pass running a named or live GLSL fragment shader. Inputs "
			 "are samplers in set 2 and slot order. Set 3 binding 0 is the engine's "
			 "camera, target and time block. The renderer builds and caches the "
			 "pipeline, then draws one fullscreen triangle. "
			 "Two of these are one kind and two pipelines, which is what node "
			 "parameters are for."},

			{"world",
			 "World",
			 C::Draw,
			 S::World,
			 {},
			 {{"entities", K::Entities, F::R8, true, "Everything this world put in the frame."}},
			 "The world this view belongs to, as a source of geometry. **Where a "
			 "frame of several worlds starts**: each world's shared work runs "
			 "once for it, and what comes out of here is that world's draw list "
			 "rather than the frame's.",
			 true},

			{"camera",
			 "Camera",
			 C::Draw,
			 S::View,
			 {},
			 {{"camera", K::Camera, F::R8, true, "Where this view looks from."}},
			 "The view's own eye, as a value. What a filter culls against and "
			 "what a colour pass projects with, when nothing says otherwise.",
			 true},

			{"light-camera",
			 "Light camera",
			 C::Draw,
			 S::World,
			 {{"entities", K::Entities, F::R8, false, "What the light must cover."}},
			 {{"camera", K::Camera, F::R8, true, "The light's fitted viewpoint."}},
			 "The sun, fitted to the bound of what it is given. **This is what "
			 "makes 'do not frustum cull the shadow pass' sayable as something "
			 "better** - cull against the light's box instead, which drops what "
			 "casts into nothing rather than keeping everything.",
			 true},

			{"entities",
			 "Entities",
			 C::Draw,
			 S::View,
			 {},
			 {{"entities", K::Entities, F::R8, true, "Every instance this view was given."}},
			 "The source: the whole draw list, before anything has filtered it. "
			 "Everything that draws geometry ultimately reads from one of these.",
			 true},

			{"cull-frustum",
			 "Frustum cull",
			 C::Draw,
			 S::View,
			 {{"entities", K::Entities, F::R8, true, "What to consider."},
			  {"camera", K::Camera, F::R8, false, "Whose frustum. Unwired, the view's own eye."}},
			 {{"entities", K::Entities, F::R8, true, "What the camera can see."}},
			 "Drops what the view frustum does not contain. The single most "
			 "valuable filter, and the one a shadow pass must not have - a caster "
			 "off screen still shadows into it."},

			{"cull-distance",
			 "Distance cull",
			 C::Draw,
			 S::View,
			 {{"entities", K::Entities, F::R8, true, "What to consider."},
			  {"camera", K::Camera, F::R8, false, "Where to measure from. Unwired, the view's own eye."}},
			 {{"entities", K::Entities, F::R8, true, "What is near enough."}},
			 "Drops what is further than a radius. Unconfigured it keeps "
			 "everything, because a node somebody has not set up should be a "
			 "no-op rather than an empty frame."},

			{"filter-tag",
			 "Tag filter",
			 C::Draw,
			 S::View,
			 {{"entities", K::Entities, F::R8, true, "What to consider."}},
			 {{"entities", K::Entities, F::R8, true, "What carries the tag."}},
			 "Keeps what matches a tag mask. How a pass draws one layer of a "
			 "scene - the casters, the mirrors, the terrain - without the scene "
			 "having to be split up to say so."},

			{"order-draw",
			 "Order for drawing",
			 C::Draw,
			 S::View,
			 {{"entities", K::Entities, F::R8, true, "What to order."},
			  {"camera",
			   K::Camera,
			   F::R8,
			   false,
			   "Who the back-to-front sort is for. Unwired, the view's own eye."}},
			 {{"entities", K::Entities, F::R8, true, "Opaque first, then blended back to front."}},
			 "Sorts a list the way a colour pass needs it. A depth-only pass does "
			 "not need this and should not pay for it."},

			{"mesh-residency",
			 "Mesh residency",
			 C::Draw,
			 S::World,
			 {},
			 {{"meshes", K::Buffer, F::R8, true, "The resident mesh vertex and index ranges."}},
			 "Admits pending mesh ranges and records their delta into this frame's command buffer. "
			 "The graph owns the transfer, so a draw cannot observe a newly-resolved range before its bytes.",
			 true},

			{"delta-upload",
			 "Upload draw deltas",
			 C::Draw,
			 S::View,
			 {{"meshes", K::Buffer, F::R8, true, "Resident mesh ranges required by the draw list."},
			  {"entities", K::Entities, F::R8, true, "What to put in the buffer."}},
			 {{"instances", K::Buffer, F::R8, true, "The per-instance attributes, on the GPU."}},
			 "Puts a list's changed instance, skin, indirect, ribbon, and overlay rows where the GPU can "
			 "read them. **The join "
			 "between what a pipeline decided and what it draws** - a filter "
			 "upstream of one of these changes the frame; a filter with none "
			 "downstream changes a list nobody reads."},

			{"select-lod",
			 "Select authored LOD",
			 C::Draw,
			 S::View,
			 {{"instances", K::Buffer, F::R8, true, "Packed level rows and zeroed indirect draws."},
			  {"camera", K::Camera, F::R8, false, "The view used to measure projected pixel area."}},
			 {{"instances", K::Buffer, F::R8, true, "The same rows with one mesh level enabled."}},
			 "Chooses one of four authored meshes per visual on the GPU from projected pixels per "
			 "triangle. No selected level returns to the CPU."},

			{"output-image",
			 "Output Image",
			 C::Output,
			 S::Frame,
			 {{"image", K::Texture, LDR, true, "Any image to put on the display."}},
			 {},
			 "The one terminal display sink. Colour, depth, storage and intermediate "
			 "images can all end here; image conversion happens while presenting it.",
			 false},

			{"overdraw",
			 "Overdraw",
			 C::Draw,
			 S::View,
			 {},
			 {{"overdraw", K::Colour, F::R8, true, "Fragments per pixel, one step of 1/255 each."}},
			 "Counts how many times each pixel would be shaded, by drawing the "
			 "camera's whole list with the depth test off. Fault 9 of the eleven, "
			 "and the only one no reading of the graph can answer - overdraw is a "
			 "property of what the geometry happens to overlap.",
			 true},

			{"viewer",
			 "Viewer",
			 C::Output,
			 S::Frame,
			 {{"source", K::Texture, RGBA16, true, "Anything at all."}},
			 {},
			 "Shows whatever is wired into it. Attach one to any wire to see what "
			 "is on it - inspection as a node, which is Blender's idea and a good "
			 "one."},

			{"shadow-capture",
			 "Capture directional shadow",
			 C::Output,
			 S::View,
			 {{"shadow", K::Texture, D32, true, "Exact native directional shadow depth."}},
			 {},
			 "Copies native D32 depth and its producing light domain before another view overwrites it."},

			{"capture",
			 "Capture",
			 C::Output,
			 S::Frame,
			 {{"source", K::Texture, LDR, true, "The frame to write out."},
			  {"depth", K::Colour, F::R32F, false, "Optional camera-forward depth paired with the frame."},
			  {"normal", K::Colour, F::RGB10A2, false, "Native room normal and validity paired with depth."},
			  {"ambient-response",
			   K::Colour,
			   F::RGBA32F,
			   false,
			   "Ambient response and original sampled SSAO."},
			  {"lighting-baseline",
			   K::Colour,
			   F::RGBA32F,
			   false,
			   "Unrounded lighting paired with the ambient planes."},
			  {"directional-response",
			   K::Colour,
			   F::RGBA32F,
			   false,
			   "Optional directional response and shadow visibility paired with the ambient planes."}},
			 {},
			 "Writes a frame to a file. What --capture does today, as a node."},
		};

		for (const Kind &row : table) {
			NodeKindSpec spec;
			spec.Kind = core::Name(row.Name);
			spec.Label = row.Label;
			spec.Summary = row.Summary;
			spec.Category = row.Category;
			spec.Scope = row.Scope;
			spec.Source = row.Source;
			spec.DefaultShader = row.Shader;
			if (spec.Kind == core::Name("last-frame")) {
				spec.Lifetime = ResourceLifetime::History;
				spec.HistoryReads = 1;
			}

			const auto fill = [](const std::vector<Port> &from, std::vector<PortSpec> &into) {
				for (const Port &port : from) {
					PortSpec made;
					made.Name = core::Name(port.Name);
					made.Kind = port.Kind;
					made.Format = port.Format;
					made.Required = port.Required;
					made.Summary = port.Summary;
					made.InternalUse = port.InternalUse;
					into.push_back(std::move(made));
				}
			};
			fill(row.Inputs, spec.Inputs);
			fill(row.Outputs, spec.Outputs);
			AddExecutionMetadata(spec);

			NodeCatalogue::Register(std::move(spec));
		}
	}
}

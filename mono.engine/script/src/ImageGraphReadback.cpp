#include "ImageGraphReadback.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Content.hpp>
#include <engine/imagegraph/Reference.hpp>
#include <engine/scene/editable/EditableImage.hpp>
#include <engine/scene/editable/EditableMesh.hpp>
#include <engine/scene/editable/ImageGraph.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace engine::script {
	namespace {
		// Nested instance sources deeper than this are refused, which also ends cycles.
		constexpr int MAXIMUM_NESTING = 8;

		ecs::Entity ReferencedEntity(std::string_view reference, std::string_view prefix) {
			reference.remove_prefix(prefix.size());
			uint64_t id = 0;
			std::from_chars(reference.data(), reference.data() + reference.size(), id);
			return ecs::Entity(id);
		}
		uint8_t EncodeSrgb(uint8_t linear) {
			const double value = linear / 255.0;
			const double encoded =
				value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
			return static_cast<uint8_t>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
		}
		ecs::Entity FindInstance(ecs::Store &store, std::string_view key) {
			ecs::Entity found{};
			store.Each<const scene::ImageGraph>([&](ecs::Entity entity, const scene::ImageGraph &graph) {
				if (graph.InstanceKey.Text() == key) found = entity;
			});
			return found;
		}
		bool Evaluate(
			ecs::Store &store,
			ecs::Entity instance,
			std::string_view output,
			int depth,
			imagegraph::Image &out,
			imagegraph::Diagnostic &diagnostic
		);

		// Sources mirror the GPU host: a colour source holding linear bytes is encoded to
		// sRGB, the same as an RGBA8 texture sampled through an sRGB view.
		imagegraph::TypedSourceResolver Sources(ecs::Store &store, int depth) {
			return [&store,
					depth](const imagegraph::Source &source, imagegraph::Image &image, std::string &failure) {
				if (imagegraph::IsEditableImageReference(source.Path)) {
					const ecs::Entity entity = ReferencedEntity(source.Path, "editable-image://");
					const auto *editable = store.Get<scene::EditableImage>(entity);
					if (editable == nullptr) {
						failure = "source names no editable image in this world";
						return false;
					}
					image = {editable->Width, editable->Height, scene::EditableImageToBuffer(store, entity)};
					image.Space = editable->Space == scene::EditableImageSpace::SRGB
									  ? imagegraph::OutputSpace::SRGB
									  : imagegraph::OutputSpace::Linear;
					if (source.Interpretation == imagegraph::SourceInterpretation::Colour &&
						image.Space == imagegraph::OutputSpace::Linear)
						for (size_t offset = 0; offset < image.Pixels.size(); ++offset)
							if (offset % 4 != 3)
								image.Pixels[offset] =
									std::byte(EncodeSrgb(std::to_integer<uint8_t>(image.Pixels[offset])));
					return true;
				}
				imagegraph::Reference reference;
				if (imagegraph::ParseReference(source.Path, reference) &&
					reference.Kind == imagegraph::ReferenceKind::Instance) {
					const ecs::Entity entity = FindInstance(store, reference.Name);
					if (!entity) {
						failure = "source names no image graph instance in this world";
						return false;
					}
					if (depth >= MAXIMUM_NESTING) {
						failure = "image graph sources nest too deeply";
						return false;
					}
					imagegraph::Diagnostic nested;
					if (!Evaluate(store, entity, reference.Output, depth + 1, image, nested)) {
						failure = "nested graph refused: " + nested.Message;
						return false;
					}
					if (image.Format != imagegraph::OutputFormat::RGBA8) {
						failure = "nested graph output must be rgba8 to feed a source";
						return false;
					}
					return true;
				}
				failure = "readback resolves editable images and graph instances only";
				return false;
			};
		}
		imagegraph::MeshResolver Meshes(ecs::Store &store) {
			return [&store](
					   const imagegraph::MeshAttribute &node,
					   imagegraph::MeshAttributeData &out,
					   std::string &failure
				   ) {
				const auto *mesh =
					imagegraph::IsEditableMeshReference(node.Mesh)
						? store.Get<scene::EditableMesh>(ReferencedEntity(node.Mesh, "editable-mesh://"))
						: nullptr;
				if (mesh == nullptr) {
					failure = "mesh attribute names no editable mesh in this world";
					return false;
				}
				if (node.Attribute >= scene::EDITABLE_ATTRIBUTE_CHANNELS ||
					mesh->Attributes[node.Attribute].empty()) {
					failure = "editable mesh has no values in the selected attribute channel";
					return false;
				}
				out.UVs.clear();
				for (const auto &uv : mesh->UVs)
					out.UVs.push_back({uv.X, uv.Y});
				out.Values.assign(
					mesh->Attributes[node.Attribute].begin(), mesh->Attributes[node.Attribute].end()
				);
				out.Indices = mesh->Indices;
				return true;
			};
		}
		bool Evaluate(
			ecs::Store &store,
			ecs::Entity instance,
			std::string_view output,
			int depth,
			imagegraph::Image &out,
			imagegraph::Diagnostic &diagnostic
		) {
			const auto *graph = store.Get<scene::ImageGraph>(instance);
			if (graph == nullptr) {
				diagnostic = {{}, "instance is not an ImageGraph"};
				return false;
			}
			if (graph->Document.empty()) {
				diagnostic = {{}, "readback needs a document set with SetGraphDocument"};
				return false;
			}
			imagegraph::Document authored;
			if (!imagegraph::AdmitRuntimeDocument(graph->Document, authored, diagnostic)) return false;
			std::vector<imagegraph::InputOverride> inputs;
			for (const auto &input : graph->Inputs) {
				imagegraph::InputValue value;
				switch (input.Kind) {
				case scene::ImageGraphInputKind::Number:
					value = input.Number;
					break;
				case scene::ImageGraphInputKind::Boolean:
					value = input.Boolean;
					break;
				case scene::ImageGraphInputKind::Colour:
					value = input.Colour;
					break;
				case scene::ImageGraphInputKind::String:
					value = input.String;
					break;
				}
				inputs.push_back({std::string(input.Name.Text()), std::move(value)});
			}
			imagegraph::Document resolved;
			imagegraph::Plan plan;
			if (!imagegraph::ResolveInputs(authored, inputs, resolved, diagnostic) ||
				!imagegraph::Compile(resolved, plan, diagnostic))
				return false;
			const std::string selected(output.empty() ? graph->Output.Text() : output);
			return imagegraph::EvaluateWith(
				resolved, plan, selected, {Sources(store, depth), Meshes(store)}, out, diagnostic
			);
		}
	}

	bool ReadImageGraph(
		ecs::Store &store,
		ecs::Entity instance,
		std::string_view output,
		imagegraph::Image &out,
		imagegraph::Diagnostic &diagnostic
	) {
		return Evaluate(store, instance, output, 0, out, diagnostic);
	}
}

#include <engine/core/Name.hpp>
#include <engine/examples/RenderFeaturesDemo.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace engine::examples {

	namespace {
		using core::Name;
		using graph::Edit;
		using graph::EditKind;
		using graph::NodeScope;
		using graph::PipelineDocument;
		using graph::ResourceFormat;
		using graph::ResourceKind;

		void Resource(
			PipelineDocument &document,
			std::string_view name,
			ResourceKind kind,
			ResourceFormat format,
			bool external = false
		) {
			document.Record({
				.Kind = EditKind::AddResource,
				.Name = Name(name),
				.Resource = kind,
				.Format = format,
				.External = external,
			});
		}

		void Node(
			PipelineDocument &document,
			std::string_view name,
			std::string_view kind,
			NodeScope scope,
			std::initializer_list<std::pair<std::string_view, std::string_view>> reads,
			std::initializer_list<std::pair<std::string_view, std::string_view>> writes,
			std::initializer_list<std::pair<std::string_view, std::string_view>> settings = {}
		) {
			document.Record({
				.Kind = EditKind::AddNode,
				.Name = Name(name),
				.NodeKind = Name(kind),
				.Scope = scope,
			});
			for (const auto &[resource, port] : reads) {
				document.Record({
					.Kind = EditKind::Reads,
					.Target = Name(resource),
					.Key = Name(port),
				});
			}
			for (const auto &[resource, port] : writes) {
				document.Record({
					.Kind = EditKind::Writes,
					.Target = Name(resource),
					.Key = Name(port),
				});
			}
			for (const auto &[key, value] : settings) {
				document.Record({
					.Kind = EditKind::Set,
					.Key = Name(key),
					.Value = std::string(value),
				});
			}
		}
	}

	PipelineDocument RenderFeaturesDemoPipeline() {
		PipelineDocument document = graph::DefaultPbrDocument();

		for (const char *node : {"present", "interface", "overlay", "output-image"}) {
			document.Record({.Kind = EditKind::Enable, .Name = Name(node), .Enabled = false});
		}

		Resource(document, "demo-computed", ResourceKind::Storage, ResourceFormat::RGBA16F);
		Resource(document, "demo-post", ResourceKind::Colour, ResourceFormat::RGBA16F);
		Resource(document, "demo-antialiased", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		Resource(document, "demo-scene", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		Resource(document, "demo-interface", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB, true);
		Resource(document, "demo-composed", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);

		Node(
			document,
			"demo-compute",
			"dispatch",
			NodeScope::View,
			{{"tonemapped", "source"}},
			{{"demo-computed", "target"}},
			{{"shader", "attachment-copy.comp"}, {"attachment", "visual"}, {"uniforms", "view"}}
		);
		Node(
			document,
			"demo-post",
			"fxaa",
			NodeScope::View,
			{{"demo-computed", "source"}},
			{{"demo-post", "target"}},
			{{"attachment", "visual"}}
		);
		Node(
			document,
			"demo-fxaa",
			"fxaa",
			NodeScope::View,
			{{"demo-post", "source"}},
			{{"demo-antialiased", "target"}}
		);
		Node(
			document,
			"demo-present",
			"present",
			NodeScope::Frame,
			{{"demo-antialiased", "image"}},
			{{"demo-scene", "image"}}
		);
		Node(
			document, "demo-interface-pass", "interface", NodeScope::Frame, {}, {{"demo-interface", "image"}}
		);
		Node(
			document,
			"demo-overlay",
			"overlay",
			NodeScope::Frame,
			{{"demo-scene", "scene"}, {"demo-interface", "interface"}},
			{{"demo-composed", "image"}}
		);
		Node(document, "demo-output", "output-image", NodeScope::Frame, {{"demo-composed", "image"}}, {});

		return document;
	}
}

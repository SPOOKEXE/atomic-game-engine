#include "SourceVfxParticles.hpp"
#include "SourceVfxRaster.hpp"
#include "VfxNodes.hpp"
#include "nodes/Processor.hpp"
#include "nodes/VerletNodes.hpp"
namespace engine::imagegraph::detail {
	bool VfxInlineScope(NodeContext &context) {
		if (!context.Boolean("loop", true)) return context.FailureCode == Status::Ok;
		if (!context.EvaluationDocument)
			return context.Fail(
				Status::UnsupportedExecution, "VFX pre-render requires captured collection membership"
			);
		const auto &doc = *context.EvaluationDocument;
		uint64_t visits = 0;
		for (const auto &node : doc.Nodes) {
			std::string_view scope = node.GroupId;
			for (size_t depth = 0; !scope.empty() && depth < doc.Groups.size(); ++depth) {
				if (++visits > 16777216)
					return context.Fail(
						Status::LimitExceeded, "VFX collection membership exceeds bounded traversal"
					);
				const auto group = std::find_if(doc.Groups.begin(), doc.Groups.end(), [&](const auto &g) {
					return g.Id == scope;
				});
				if (group == doc.Groups.end()) break;
				if (group->OwnerNodeId == context.Authored.Id) {
					if (node.Type != "pc.vfx_renderer")
						return context.Fail(
							Status::UnsupportedExecution,
							"VFX loop pre-render needs owned particle simulation replay for this member",
							"loop"
						);
					break;
				}
				scope = group->ParentId;
			}
		}
		return true;
	}
	bool VfxRenderer(NodeContext &context) {
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"VFX CPU raster profile requires captured source GPU point and texture coverage"
			);
		if (context.InlineOwnerType != "pc.vfx_group_inline")
			return context.Fail(
				Status::UnsupportedExecution,
				"source VFX renderer requires its nearest VFX collection dimension",
				"output_dimension"
			);
		const auto dimension = VerletScopeDimension(context);
		if (context.FailureCode != Status::Ok) return false;
		if (!MeshFinite(dimension) || dimension.X < 1 || dimension.Y < 1 ||
			dimension.X != std::trunc(dimension.X) || dimension.Y != std::trunc(dimension.Y) ||
			dimension.X > Limits::MaximumDimension || dimension.Y > Limits::MaximumDimension)
			return context.Fail(
				Status::InvalidValue,
				"VFX output dimension requires bounded integral pixels",
				"output_dimension"
			);
		const auto type = context.Integer("render_type", 0);
		if (type < 0 || type > 1)
			return context.Fail(Status::InvalidValue, "VFX render type is invalid", "render_type");
		const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
		if (!format) return false;
		auto *image = context.NewImage("surface_out", uint32_t(dimension.X), uint32_t(dimension.Y), *format);
		if (!image) return false;
		struct Group {
			size_t Index;
			std::string_view Blend, Particles;
		};
		auto charge = context.ReserveWorkspace(context.Authored.DynamicInputs.size() * sizeof(Group));
		if (!charge) return false;
		std::vector<Group> groups;
		groups.reserve(context.Authored.DynamicInputs.size());
		for (const auto &port : context.Authored.DynamicInputs) {
			size_t index = 0;
			const auto *source = FindDynamicTemplate(context.Entry, port.Id, index);
			if (!source)
				return context.Fail(
					Status::InvalidValue, "VFX dynamic input has no source template", port.Id
				);
			auto group =
				std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.Index == index; });
			if (group == groups.end()) {
				groups.push_back({index, {}, {}});
				group = groups.end() - 1;
			}
			if (source->Id == "blend_mode")
				group->Blend = port.Id;
			else if (source->Id == "input_1")
				group->Particles = port.Id;
		}
		std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) {
			return a.Index < b.Index;
		});
		uint64_t work = 0;
		const bool filtered = Filtered(ReadSampler(context));
		for (const auto &group : groups) {
			if (group.Blend.empty() || group.Particles.empty())
				return context.Fail(
					Status::InvalidValue, "VFX renderer requires complete blend and particle input groups"
				);
			const auto mode = context.Integer(group.Blend, 0);
			if (mode < 0 || mode > 2)
				return context.Fail(Status::InvalidValue, "VFX blend mode is invalid", group.Blend);
			if (!VisitSourceVfxParticles(
					context, context.Find(group.Particles), [&](const ParticleData2D &particle) {
						return DrawSourceVfxBaseParticle(
							context, *image, particle, mode, filtered, work, type == 1
						);
					}
				))
				return false;
		}
		return context.FailureCode == Status::Ok;
	}
}

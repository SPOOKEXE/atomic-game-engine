#pragma once

#include "NodeExecutors.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph::testing {
	// grug known concave points and zero-normal Octahedron check geometry without another renderer.
	struct SourceShapeTriangulateFixture {
		enum class Kind { Shape3D, Triangulate };
		using Triangle = std::array<std::array<double, 3>, 3>;
		static constexpr uint32_t Side = 64;
		Kind Workload;
		Document Authored;
		Plan Compiled;
		std::array<Image, 2> Images;
		EvaluatedValue Triangles;
		uint64_t ExpectedHash = 0;
		explicit SourceShapeTriangulateFixture(Kind kind) : Workload(kind) {
			Authored.FormatVersion = 9;
			if (kind == Kind::Shape3D) {
				Authored.Nodes = {
					{"shape",
					 "pc.shape_3_d",
					 "",
					 {},
					 {{"shape", std::string{"Octahedron"}},
					  {"dimension", Vector2{Side, Side}},
					  {"dimension_unit", EnumValue{0}},
					  {"attribute_color_depth", EnumValue{5}}}}
				};
				Authored.Outputs = {{"surface", "shape", "surface_out"}, {"rim", "shape", "rim_normal"}};
			} else {
				ArrayValue points{
					ValueType::Vector2,
					{Vector2{0, 0}, Vector2{4, 0}, Vector2{1, 1}, Vector2{4, 4}, Vector2{0, 4}}
				};
				Authored.Nodes = {
					{"triangulate", "pc.points_triangulate", "", {}, {{"points", std::move(points)}}}
				};
				Authored.Outputs = {{"out", "triangulate", "triangles"}};
			}
			Diagnostic diagnostic;
			Check(Compile(Authored, Compiled, diagnostic), diagnostic);
			Run();
			ExpectedHash = Verify();
			Run();
			if (Verify() != ExpectedHash) Fail("shape/triangulation repeatability");
			CheckBudget();
		}
		[[noreturn]] static void Fail(std::string_view message) {
			throw std::runtime_error(std::string(message));
		}
		static void Check(Status status, const Diagnostic &diagnostic) {
			if (status != Status::Ok)
				Fail(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		}
		uint32_t OutputSide() const {
			return Workload == Kind::Shape3D ? Side : 0;
		}
		const char *OutputFormat() const {
			if (Workload == Kind::Triangulate) return "WeightedTriangles";
			const auto format = DescribeSurfaceFormat(Images[0].Format);
			if (!format) Fail("shape profiling surface format");
			return format->Name.data();
		}
		bool IsPalette() const {
			return false;
		}
		unsigned ProfileKind() const {
			return Workload == Kind::Shape3D ? 200 : 201;
		}
		size_t ProfileEvaluations() const {
			return Workload == Kind::Shape3D ? 2 : 1;
		}
		bool ProfileProcessors() const {
			return true;
		}
		std::string_view ProfileWorkScope() const {
			return Workload == Kind::Shape3D ? "imagegraph.shape3d.executor"
											 : "imagegraph.source.points_triangulate";
		}
		double ProfileSeed() const {
			return 0;
		}
		unsigned ProfileIterations() const {
			return 0;
		}
		void Run() {
			Diagnostic diagnostic;
			if (Workload == Kind::Shape3D) {
				Check(Evaluate(Authored, Compiled, "surface", {}, Images[0], diagnostic), diagnostic);
				Check(Evaluate(Authored, Compiled, "rim", {}, Images[1], diagnostic), diagnostic);
			} else
				Check(EvaluateValue(Authored, Compiled, "out", {}, Triangles, diagnostic), diagnostic);
		}
		static Triangle Sorted(Triangle triangle) {
			std::sort(triangle.begin(), triangle.end());
			return triangle;
		}
		uint64_t Verify() const {
			uint64_t hash = 14695981039346656037ull;
			const auto number = [&](double value) {
				const auto bits = std::bit_cast<uint64_t>(value);
				for (unsigned shift = 0; shift < 64; shift += 8)
					hash = (hash ^ uint8_t(bits >> shift)) * 1099511628211ull;
			};
			if (Workload == Kind::Shape3D) {
				for (size_t output = 0; output < Images.size(); ++output) {
					const auto &image = Images[output];
					if (image.Width != Side || image.Height != Side || !FiniteSurfaceSamples(image))
						Fail("shape attachment layout");
					bool covered = false;
					for (uint32_t y = 0; y < Side; ++y)
						for (uint32_t x = 0; x < Side; ++x) {
							SurfacePixel pixel{};
							if (!LoadSurfacePixel(image, x, y, pixel)) Fail("shape attachment pixel");
							covered |= pixel[3] > 0;
							if (output == 1 && (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0))
								Fail("Octahedron zero-normal rim oracle");
							for (double channel : pixel)
								number(channel);
						}
					if (!covered) Fail("shape attachment coverage");
				}
				return hash;
			}
			const auto *array = std::get_if<ArrayValue>(&Triangles.Data);
			if (!array || array->ElementType != ValueType::Any || array->Items.size() != 4)
				Fail("triangulation output shape");
			std::set<Triangle> actual;
			for (const auto &item : array->Items) {
				const auto *corners = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
				if (!corners || corners->size() != 3) Fail("triangle corner count");
				Triangle triangle{};
				for (size_t corner = 0; corner < 3; ++corner) {
					const auto *coordinates =
						std::get_if<std::vector<SourceArrayItem>>(&(*corners)[corner].Data);
					if (!coordinates || coordinates->size() != 3) Fail("weighted coordinate shape");
					for (size_t axis = 0; axis < 3; ++axis) {
						const auto *leaf = std::get_if<ElementValue>(&(*coordinates)[axis].Data);
						const auto *value = leaf ? std::get_if<double>(leaf) : nullptr;
						if (!value || !std::isfinite(*value)) Fail("weighted coordinate value");
						triangle[corner][axis] = *value;
					}
				}
				actual.insert(Sorted(triangle));
			}
			const std::set<Triangle> expected{
				Sorted({{{0, 0, 1}, {4, 0, 1}, {1, 1, 1}}}),
				Sorted({{{4, 0, 1}, {4, 4, 1}, {1, 1, 1}}}),
				Sorted({{{4, 4, 1}, {0, 4, 1}, {1, 1, 1}}}),
				Sorted({{{0, 4, 1}, {0, 0, 1}, {1, 1, 1}}})
			};
			if (actual != expected) Fail("concave-input complete hull oracle");
			if (!Triangles.Domain || Triangles.Domain->Type != ValueType::Scalar ||
				Triangles.Domain->Kind != SourceSocketKind::Float ||
				Triangles.Domain->Display != SourceValueDisplay::Vector)
				Fail("weighted triangulation domain");
			for (const auto &triangle : actual)
				for (const auto &corner : triangle)
					for (double value : corner)
						number(value);
			return hash;
		}
		void CheckBudget() {
			const uint64_t before = Verify();
			if (Workload == Kind::Shape3D) {
				Diagnostic diagnostic;
				if (Evaluate(Authored, Compiled, "surface", {}, Images[0], diagnostic, 1) !=
					Status::LimitExceeded)
					Fail("shape byte preflight");
			} else {
				const auto &node = Authored.Nodes[0];
				EvaluationRequest request;
				for (bool wholeBatch : {false, true}) {
					detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
					detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request, budget);
					context.ByteBudget = wholeBatch ? Limits::MaximumEvaluationBytes : 1;
					context.ProcessorCount = wholeBatch ? 1000000 : 1;
					context.Values = {{"points", node.Values[0].Data}};
					if (detail::FindExecutor(node.Type)(context) ||
						context.FailureCode != Status::LimitExceeded || !context.OutputValues.empty() ||
						budget.Used() != 0)
						Fail("triangulation batch preflight");
				}
			}
			if (Verify() != before) Fail("refused shape/triangulation changed retained output");
		}
	};
}

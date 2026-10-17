// The host work behind device-resident instance rows.
//
// A frame can upload almost nothing and still spend time proving that nothing
// changed. These rows separate the transform packing from the stable-key table
// lookup so a profile of `resident.scene` has an answer more precise than
// "residency is slow". No device is involved; this is all CPU work completed
// before an upload is staged.
//
// On the 24-thread development machine in the `bench` preset, ten thousand
// steady rows measured 25 ns each to pack and 17 ns each to upsert after
// packing across three samples. Exact source reuse measured 16 ns each.
// The whole-row `memcmp` is 3 ns, which is
// why the cache compares exact bytes instead of maintaining a second hash.

#include <engine/core/Name.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Bench.hpp>

#include <InstanceResidency.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.render.bench.instances")

using engine::core::Name;
using engine::render::GpuInstance;
using engine::render::InstanceKey;
using engine::render::InstanceResidency;
using engine::render::MeshEntry;
using engine::render::ToGpu;
using engine::scene::DrawInstance;
using engine::testing::Consume;

namespace instance_bench {
	// Frozen packing oracle makes the candidate and its baseline share fixtures.
	GpuInstance LegacyToGpu(const DrawInstance &instance, const MeshEntry &mesh) {
		const auto stretch = [](float half, float extent) {
			return extent > 1e-6f ? half / extent : half * 2.0f;
		};
		const glm::vec3 scale{
			stretch(instance.HalfExtent.X, mesh.Extent.X),
			stretch(instance.HalfExtent.Y, mesh.Extent.Y),
			stretch(instance.HalfExtent.Z, mesh.Extent.Z),
		};
		GpuInstance gpu;
		gpu.Rotation = engine::render::PackRotation(instance.Frame.Rotation());
		gpu.Scale = scale;
		gpu.Position =
			glm::vec3{instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z} -
			engine::render::UnpackRotation(gpu.Rotation) *
				(scale * glm::vec3{mesh.Centre.X, mesh.Centre.Y, mesh.Centre.Z});
		gpu.Colour = engine::render::PackColour(
			glm::vec4{instance.Tint.R, instance.Tint.G, instance.Tint.B, 1.0f - instance.Transparency}
		);
		gpu.Appearance =
			engine::render::PackAppearance(instance.Alpha, instance.AlphaCutoff, instance.Resample);
		gpu.SurfaceColour = engine::render::PackColour(
			glm::vec4{instance.SurfaceColour.R, instance.SurfaceColour.G, instance.SurfaceColour.B, 1.0f}
		);
		gpu.Emission = engine::render::PackEmission(instance.EmissiveTint, instance.EmissiveStrength);
		gpu.FeatureEnable = instance.RenderFeatures.Enable & engine::scene::ALL_RENDER_FEATURES;
		gpu.FeatureDisable = instance.RenderFeatures.Disable & engine::scene::ALL_RENDER_FEATURES;
		gpu.ObjectLabel = instance.ObjectLabel;
		gpu.SemanticLabel = instance.SemanticLabel;
		gpu.PartLabel = instance.PartLabel;
		return gpu;
	}

	struct Rows {
		std::vector<DrawInstance> Source;
		std::vector<DrawInstance> Previous;
		std::vector<GpuInstance> Packed;
		std::vector<GpuInstance> Staged;
		std::vector<InstanceKey> Keys;
		std::vector<uint32_t> Slots;
		MeshEntry Mesh;
		InstanceResidency Residency;

		explicit Rows(size_t count) {
			Source.resize(count);
			Previous.resize(count);
			Packed.resize(count);
			Staged.resize(count);
			Keys.resize(count);
			Slots.resize(count);
			const Name world("bench.world");
			for (size_t index = 0; index < count; index++) {
				DrawInstance &instance = Source[index];
				instance.Frame.Position = {
					static_cast<float>(index % 100),
					static_cast<float>((index / 100) % 100),
					static_cast<float>(index / 10'000),
				};
				instance.Source = index + 1;
				Packed[index] = ToGpu(instance, Mesh);
				Keys[index] = InstanceKey{world, instance.Source, 0, 0};
			}
			Previous = Source;

			Residency.BeginFrame();
			for (size_t index = 0; index < count; index++) {
				Slots[index] = Residency.Upsert(Keys[index], Packed[index], Source[index], Mesh);
			}
			Residency.EndFrame();
			Residency.AcknowledgeDirty();
		}
	};

	Rows &RowsOf(size_t count) {
		static Rows thousand(1'000);
		static Rows tenThousand(10'000);
		return count == 1'000 ? thousand : tenThousand;
	}

	Rows &RotatedRows(bool centred) {
		static Rows centredRows(10'000);
		static Rows offsetRows(10'000);
		static const bool prepared = [] {
			offsetRows.Mesh.Centre = {6.0f, -2.0f, 3.0f};
			for (Rows *rows : {&centredRows, &offsetRows}) {
				for (size_t index = 0; index < rows->Source.size(); ++index) {
					rows->Source[index].Frame = engine::core::CFrame(
						engine::core::Vector3{
							1.0f + static_cast<float>(index % 100),
							-1.0f - static_cast<float>((index / 100) % 100),
							3.0f,
						},
						glm::quat{0.5f, 0.5f, -0.5f, 0.5f}
					);
					rows->Source[index].HalfExtent = {1.0f, 2.0f, 3.0f};
				}
			}
			return true;
		}();
		(void)prepared;
		return centred ? centredRows : offsetRows;
	}
}

using namespace instance_bench;

BENCH("ToGpu · 1,000 unchanged instance rows", 1'000) {
	Rows &rows = RowsOf(1'000);
	for (size_t index = 0; index < rows.Source.size(); index++) {
		Consume(ToGpu(rows.Source[index], rows.Mesh));
	}
}

BENCH("Signature · 1,000 unchanged instance rows", 1'000) {
	Rows &rows = RowsOf(1'000);
	for (const DrawInstance &instance : rows.Source) {
		Consume(engine::scene::SignatureOf(std::span(&instance, 1)));
	}
}

BENCH("memcmp · 1,000 unchanged instance rows", 1'000) {
	Rows &rows = RowsOf(1'000);
	for (size_t index = 0; index < rows.Source.size(); index++) {
		Consume(std::memcmp(&rows.Source[index], &rows.Previous[index], sizeof(DrawInstance)));
	}
}

BENCH("Residency · 1,000 unchanged prepacked rows", 1'000) {
	Rows &rows = RowsOf(1'000);
	rows.Residency.BeginFrame();
	for (size_t index = 0; index < rows.Packed.size(); index++) {
		Consume(rows.Residency.Upsert(rows.Keys[index], rows.Packed[index]));
	}
	rows.Residency.EndFrame();
}

BENCH("Reuse · 1,000 unchanged source rows", 1'000) {
	Rows &rows = RowsOf(1'000);
	rows.Residency.BeginFrame();
	for (size_t index = 0; index < rows.Source.size(); index++) {
		uint32_t slot = 0;
		Consume(rows.Residency.Reuse(rows.Keys[index], rows.Source[index], rows.Mesh, slot));
	}
	rows.Residency.EndFrame();
}

BENCH("ToGpu · 10,000 unchanged instance rows", 10'000) {
	Rows &rows = RowsOf(10'000);
	for (size_t index = 0; index < rows.Source.size(); index++) {
		Consume(ToGpu(rows.Source[index], rows.Mesh));
	}
}

BENCH("ToGpu · 10,000 rotated centred instance rows", 10'000) {
	Rows &rows = RotatedRows(true);
	for (const DrawInstance &instance : rows.Source)
		Consume(ToGpu(instance, rows.Mesh));
}

BENCH("ToGpu · 10,000 rotated off-centre instance rows", 10'000) {
	Rows &rows = RotatedRows(false);
	for (const DrawInstance &instance : rows.Source)
		Consume(ToGpu(instance, rows.Mesh));
}

BENCH("Legacy ToGpu · 10,000 rotated centred instance rows", 10'000) {
	Rows &rows = RotatedRows(true);
	for (const DrawInstance &instance : rows.Source)
		Consume(LegacyToGpu(instance, rows.Mesh));
}

BENCH("Legacy ToGpu · 10,000 rotated off-centre instance rows", 10'000) {
	Rows &rows = RotatedRows(false);
	for (const DrawInstance &instance : rows.Source)
		Consume(LegacyToGpu(instance, rows.Mesh));
}

BENCH("Signature · 10,000 unchanged instance rows", 10'000) {
	Rows &rows = RowsOf(10'000);
	for (const DrawInstance &instance : rows.Source) {
		Consume(engine::scene::SignatureOf(std::span(&instance, 1)));
	}
}

BENCH("memcmp · 10,000 unchanged instance rows", 10'000) {
	Rows &rows = RowsOf(10'000);
	for (size_t index = 0; index < rows.Source.size(); index++) {
		Consume(std::memcmp(&rows.Source[index], &rows.Previous[index], sizeof(DrawInstance)));
	}
}

BENCH("Residency · 10,000 unchanged prepacked rows", 10'000) {
	Rows &rows = RowsOf(10'000);
	rows.Residency.BeginFrame();
	for (size_t index = 0; index < rows.Packed.size(); index++) {
		Consume(rows.Residency.Upsert(rows.Keys[index], rows.Packed[index]));
	}
	rows.Residency.EndFrame();
}

BENCH("Reuse · 10,000 unchanged source rows", 10'000) {
	Rows &rows = RowsOf(10'000);
	rows.Residency.BeginFrame();
	for (size_t index = 0; index < rows.Source.size(); index++) {
		uint32_t slot = 0;
		Consume(rows.Residency.Reuse(rows.Keys[index], rows.Source[index], rows.Mesh, slot));
	}
	rows.Residency.EndFrame();
}

BENCH("Known slot · 10,000 unchanged source rows", 10'000) {
	Rows &rows = RowsOf(10'000);
	rows.Residency.BeginFrame();
	for (size_t index = 0; index < rows.Source.size(); index++) {
		Consume(rows.Residency.ProbeSlot(rows.Slots[index], rows.Keys[index], rows.Source[index], rows.Mesh));
		rows.Residency.Touch(rows.Slots[index]);
	}
	rows.Residency.EndFrame();
}

BENCH("Stage scalar · whole 10,000-row pool", 1) {
	Rows &rows = RowsOf(10'000);
	for (uint32_t slot = 0; slot < rows.Residency.SlotCount(); slot++) {
		rows.Staged[slot] = rows.Residency.Row(slot);
	}
	Consume(rows.Staged.data());
}

BENCH("Stage contiguous · whole 10,000-row pool", 1) {
	Rows &rows = RowsOf(10'000);
	const std::span<const GpuInstance> packed = rows.Residency.PackedRows();
	std::memcpy(rows.Staged.data(), packed.data(), packed.size_bytes());
	Consume(rows.Staged.data());
}

BENCH("DirtyRanges · 10,000 dirty rows · repeated metric stage upload reads", 300) {
	static Rows rows(10'000);
	rows.Residency.MarkAllDirty();
	for (size_t sync = 0; sync < 100; ++sync) {
		for (size_t reader = 0; reader < 3; ++reader) {
			const auto ranges = rows.Residency.DirtyRanges();
			if (ranges.size() != 1 || ranges.front().First != 0 || ranges.front().Count != 10'000)
				throw std::runtime_error("dirty residency range did not cover the full pool");
			Consume(ranges.size());
			Consume(ranges.data());
		}
	}
	rows.Residency.AcknowledgeDirty();
}

BENCH("DirtyRanges · 10,000 resident rows · repeated resync", 300) {
	static Rows rows(10'000);
	for (size_t sync = 0; sync < 100; ++sync) {
		rows.Residency.MarkAllDirty();
		for (size_t reader = 0; reader < 3; ++reader) {
			const auto ranges = rows.Residency.DirtyRanges();
			if (ranges.size() != 1 || ranges.front().First != 0 || ranges.front().Count != 10'000)
				throw std::runtime_error("resynced residency range did not cover the full pool");
			Consume(ranges.size());
			Consume(ranges.data());
		}
		rows.Residency.AcknowledgeDirty();
	}
}

BENCH("DirtyRanges · later camera adds a dirty row between reads", 600) {
	static Rows rows(10'000);
	for (size_t sync = 0; sync < 100; ++sync) {
		rows.Residency.AcknowledgeDirty();
		for (const size_t index : {size_t{255}, size_t{513}}) {
			rows.Source[index].Frame.Position.X = rows.Source[index].Frame.Position.X == 0.0f ? 1.0f : 0.0f;
			rows.Packed[index] = ToGpu(rows.Source[index], rows.Mesh);
			Consume(rows.Residency.Upsert(rows.Keys[index], rows.Packed[index]));
			for (size_t reader = 0; reader < 3; ++reader) {
				const auto ranges = rows.Residency.DirtyRanges();
				const size_t expected = index == 255 ? 1 : 2;
				if (ranges.size() != expected || ranges.back().First != index || ranges.back().Count != 1)
					throw std::runtime_error("later camera dirty row missing from residency ranges");
				Consume(ranges.size());
				Consume(ranges.data());
			}
		}
	}
	rows.Residency.AcknowledgeDirty();
}

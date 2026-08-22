// The host work behind device-resident instance rows.
//
// A frame can upload almost nothing and still spend time proving that nothing
// changed. These rows separate the transform packing from the stable-key table
// lookup so a profile of `resident.scene` has an answer more precise than
// "residency is slow". No device is involved; this is all CPU work completed
// before an upload is staged.
//
// On the 24-thread development machine in the `bench` preset, ten thousand
// steady rows measured 37 ns each to pack and 17 ns each to upsert after
// packing. Exact source reuse measured 13 ns each, removing about 76 per cent
// of the former unchanged-row path. The whole-row `memcmp` is 3 ns, which is
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
	struct Rows {
		std::vector<DrawInstance> Source;
		std::vector<DrawInstance> Previous;
		std::vector<GpuInstance> Packed;
		std::vector<InstanceKey> Keys;
		MeshEntry Mesh;
		InstanceResidency Residency;

		explicit Rows(size_t count) {
			Source.resize(count);
			Previous.resize(count);
			Packed.resize(count);
			Keys.resize(count);
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
				Residency.Upsert(Keys[index], Packed[index], Source[index], Mesh);
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

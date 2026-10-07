#include "../src/nodes/SourceMeshFlatten.hpp"
#include "../src/nodes/SourceParticle3DReplay.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_particle_3d_executor")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
namespace source_particle3d_executor_test {
	struct Run {
		Node Authored{"particles", "pc.3_d_particle", "", {}, {}};
		EvaluationRequest Request;
		NodeContext Context{Authored, *FindCatalogueEntry(Authored.Type), Request};
		Run() {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			for (const auto &input : Context.Entry.Inputs)
				if (auto value = CatalogueDefault(input)) Context.Values.emplace_back(input.Id, *value);
			Set("loop", false);
			Set("spawn_span", Vector3{});
			Set("lifespan", Vector2{2, 2});
			Set("velocity", ArrayValue{ValueType::Scalar, {0., 0., 0., 0., 1., 1.}});
		}
		void Set(std::string_view port, Value value) {
			for (auto &[id, data] : Context.Values)
				if (id == port) {
					data = std::move(value);
					return;
				}
			Context.Values.emplace_back(port, std::move(value));
		}
		bool Execute() {
			return FindExecutor(Authored.Type)(Context);
		}
		const MeshValue3D &Mesh() const {
			return std::get<MeshValue3D>(Context.OutputValues.front().Data);
		}
	};
	MeshValue3D Base() {
		MeshValue3D mesh;
		auto &data = mesh.Data.emplace();
		data.LocalTransforms.emplace_back();
		data.Materials.emplace_back();
		data.Parts.emplace_back();
		data.Parts[0].Vertices = {
			{{-1, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {.5, 1}}
		};
		return mesh;
	}
}
using namespace source_particle3d_executor_test;
TEST_CASE("Particle 3D publishes source draw count and advances owned replay without rereading base mesh") {
	Run first;
	first.Set("mesh", Base());
	REQUIRE(first.Execute());
	REQUIRE(first.Mesh().Data);
	CHECK(first.Mesh().Data->ParticleInstanced);
	REQUIRE(first.Mesh().Data->Instances.size() == 1);
	CHECK(first.Mesh().Data->ParticleRecords[0].LifeTime == 1);
	CHECK(first.Mesh().Data->Instances[0].Fields[2] == 1);
	DataReplayState replay;
	replay.Entries = std::move(first.Context.DataUpdates);
	Run second;
	second.Request.Tick = 1;
	second.Context.CurrentData = &replay;
	second.Set("mesh", MeshValue3D{});
	REQUIRE(second.Execute());
	REQUIRE(second.Mesh().Data->Instances.size() == 1);
	CHECK(second.Mesh().Data->Instances[0].Fields[2] == 2);
	CHECK(second.Mesh().Data->ParticleRecords[0].LifeTime == 2);
	CHECK(second.Mesh().Data->Parts == first.Mesh().Data->Parts);
	Run repeat;
	repeat.Request.Tick = 1;
	DataReplayState next;
	next.Entries = std::move(second.Context.DataUpdates);
	repeat.Context.CurrentData = &next;
	REQUIRE(repeat.Execute());
	CHECK(repeat.Mesh() == second.Mesh());
	Run gap;
	gap.Request.Tick = 3;
	gap.Context.CurrentData = &next;
	CHECK_FALSE(gap.Execute());
	CHECK(gap.Context.FailureCode == Status::UnsupportedExecution);
	CHECK(gap.Context.OutputValues.empty());
	CHECK(gap.Context.DataUpdates.empty());
}
TEST_CASE("Particle 3D receipt round trips every ping pong word and rejects malformed binary atomically") {
	Run run;
	SourceParticle3DControls controls;
	controls.PoolCapacity = 6;
	controls.Loop = false;
	controls.SpawnSpan = {};
	SourceParticle3DState state;
	REQUIRE(BeginSourceParticle3DState(run.Context, controls, 0, state));
	StructValue receipt;
	AllocationReservation charge;
	auto base = Base();
	REQUIRE(EncodeSourceParticle3DReceipt(run.Context, state, base, receipt, charge));
	SourceParticle3DReceipt decoded;
	REQUIRE(DecodeSourceParticle3DReceipt(run.Context, receipt, decoded));
	CHECK(decoded.State.Buffers == state.Buffers);
	CHECK(decoded.State.CurveMaps == state.CurveMaps);
	CHECK(decoded.BaseMesh == base);
	auto malformed = receipt;
	std::get<BufferValue>(malformed.Data->Fields[11].second).Bytes.pop_back();
	Run bad;
	CHECK_FALSE(DecodeSourceParticle3DReceipt(bad.Context, malformed, decoded));
	CHECK(bad.Context.FailureCode == Status::InvalidValue);
	CHECK(decoded.State.Buffers == state.Buffers);
}
TEST_CASE("Shared source mesh flatten preserves ordinary and particle base geometry") {
	const auto base = Base();
	SourceMeshFlatCost quote;
	std::array<const MeshTransform3D *, 128> chain{};
	REQUIRE(SourceMeshFlatten(base, chain, 0, quote, nullptr));
	MeshData3D ordinary, particles;
	SourceMeshFlatCost a, b;
	REQUIRE(SourceMeshFlatten(base, chain, 0, a, &ordinary));
	REQUIRE(SourceMeshFlatten(base, chain, 0, b, &particles));
	CHECK(a.Bytes == quote.Bytes);
	CHECK(a.Vertices == 3);
	CHECK(ordinary.Parts == particles.Parts);
	CHECK(ordinary.Materials == particles.Materials);
	auto instance = imagegraph_test::RunNode("pc.3_d_instancer", {}, {{"mesh", base}});
	INFO(instance.Message);
	REQUIRE(instance.Ok);
	const auto *output = std::get_if<MeshValue3D>(instance.OutputValue("mesh"));
	REQUIRE(output);
	CHECK(output->Data->Parts == ordinary.Parts);
	Run particle;
	particle.Set("mesh", base);
	REQUIRE(particle.Execute());
	CHECK(particle.Mesh().Data->Parts == ordinary.Parts);
}
TEST_CASE("Particle 3D failure publishes neither mesh nor replay") {
	Run invalid;
	invalid.Set("mesh", Base());
	invalid.Set("lifespan", Vector2{0, 0});
	CHECK_FALSE(invalid.Execute());
	CHECK(invalid.Context.FailureCode == Status::InvalidValue);
	CHECK(invalid.Context.OutputValues.empty());
	CHECK(invalid.Context.DataUpdates.empty());
	Run malformed;
	DataReplayState replay;
	DataReplayEntry entry;
	entry.NodeId = malformed.Authored.Id;
	entry.Initialized = true;
	entry.Values.push_back({0, double{0}});
	replay.Entries.push_back(std::move(entry));
	malformed.Context.CurrentData = &replay;
	CHECK_FALSE(malformed.Execute());
	CHECK(malformed.Context.OutputValues.empty());
	CHECK(malformed.Context.DataUpdates.empty());
}
TEST_CASE("Particle 3D admits whole processor and exact ledger before publication") {
	Run batch;
	batch.Set("mesh", Base());
	batch.Context.ProcessorCount = 1000;
	CHECK_FALSE(batch.Execute());
	CHECK(batch.Context.FailureCode == Status::LimitExceeded);
	CHECK(batch.Context.OutputValues.empty());
	CHECK(batch.Context.DataUpdates.empty());
	Run storage;
	storage.Set("mesh", Base());
	EvaluationBudget tiny(1);
	NodeContext limited{storage.Authored, *FindCatalogueEntry(storage.Authored.Type), storage.Request, tiny};
	limited.ByteBudget = Limits::MaximumEvaluationBytes;
	limited.Values = storage.Context.Values;
	CHECK_FALSE(FindExecutor(storage.Authored.Type)(limited));
	CHECK(limited.FailureCode == Status::LimitExceeded);
	CHECK(limited.OutputValues.empty());
	CHECK(limited.DataUpdates.empty());
	CHECK(tiny.Used() == 0);
}
TEST_CASE("Particle 3D receipt rejects mismatched outer and value frame identities") {
	Run first;
	first.Set("mesh", Base());
	REQUIRE(first.Execute());
	DataReplayState replay;
	replay.Entries = std::move(first.Context.DataUpdates);
	replay.Entries[0].Values[0].Frame = 1;
	Run invalid;
	invalid.Context.CurrentData = &replay;
	CHECK_FALSE(invalid.Execute());
	CHECK(invalid.Context.FailureCode == Status::InvalidValue);
	CHECK(invalid.Context.OutputValues.empty());
	CHECK(invalid.Context.DataUpdates.empty());
}

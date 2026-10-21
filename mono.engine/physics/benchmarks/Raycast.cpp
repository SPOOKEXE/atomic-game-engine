// Public collider ray queries, with analytic answers independent of the query path.
#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.raycast")
TEST_DEPENDS("engine.physics.query")
TEST_DEPENDS("engine.physics.bakedshapes")

namespace {
	using namespace engine;
	using core::CFrame;
	using core::Vector3;
	using Clock = std::chrono::steady_clock;
	constexpr size_t RAYS = 64, MAX_TRIALS = 512;
	constexpr float TOLERANCE = .004f;
	constexpr std::array<scene::ShapeKind, 6> SHAPES{
		scene::ShapeKind::Box,
		scene::ShapeKind::Sphere,
		scene::ShapeKind::Cylinder,
		scene::ShapeKind::Capsule,
		scene::ShapeKind::Hull,
		scene::ShapeKind::Mesh
	};
	void Require(bool okay, const char *message) {
		if (!okay) throw std::runtime_error(message);
	}
	bool Flag(const char *name) {
		const char *setting = std::getenv(name);
		if (!setting) return false;
		const std::string_view value(setting);
		Require(value == "0" || value == "1", "raycast benchmark flags must be exactly 0 or 1");
		return value == "1";
	}
	size_t Option(const char *name, size_t fallback, size_t minimum, size_t maximum) {
		const char *setting = std::getenv(name);
		if (!setting) return fallback;
		const std::string_view value(setting);
		size_t result = 0;
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
		Require(
			parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result >= minimum &&
				result <= maximum,
			"raycast benchmark option outside its bounded range"
		);
		return result;
	}
	uint64_t Elapsed(Clock::time_point begin) {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
	}
	bool Near(float actual, float expected) {
		return std::isfinite(actual) && std::abs(actual - expected) <= TOLERANCE;
	}
	bool Near(const Vector3 &actual, const Vector3 &expected) {
		return Near(actual.X, expected.X) && Near(actual.Y, expected.Y) && Near(actual.Z, expected.Z);
	}
	collision::TriangleMesh Ring(size_t divisions) {
		std::vector<Vector3> points;
		std::vector<uint32_t> indices;
		points.reserve(16 * divisions * divisions);
		indices.reserve(24 * divisions * divisions);
		const auto rectangle = [&](float x0, float x1, float z0, float z1) {
			for (size_t z = 0; z < divisions; ++z)
				for (size_t x = 0; x < divisions; ++x) {
					const float left = x0 + (x1 - x0) * float(x) / float(divisions);
					const float right = x0 + (x1 - x0) * float(x + 1) / float(divisions);
					const float back = z0 + (z1 - z0) * float(z) / float(divisions);
					const float front = z0 + (z1 - z0) * float(z + 1) / float(divisions);
					const uint32_t first = static_cast<uint32_t>(points.size());
					points.insert(
						points.end(), {{left, 0, back}, {right, 0, back}, {right, 0, front}, {left, 0, front}}
					);
					indices.insert(indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
				}
		};
		rectangle(-4, -.5f, -4, 4);
		rectangle(.5f, 4, -4, 4);
		rectangle(-.5f, .5f, .5f, 4);
		rectangle(-.5f, .5f, -4, -.5f);
		return collision::BuildTriangleMesh(points, indices);
	}
	struct Fixture {
		std::unique_ptr<ecs::Store> Store;
		std::array<ecs::Entity, 6> First{};
		std::array<CFrame, 6> Frames{};
		ecs::Entity Competitor;
		size_t Base = 0, Triangles = 0;
		Vector3 MeshOrigin;
	};
	Fixture MakeFixture(size_t count, size_t divisions) {
		Fixture fixture;
		fixture.Base = count;
		fixture.Store = std::make_unique<ecs::Store>("physics.bench.raycast");
		auto &store = *fixture.Store;
		physics::PreparePhysicsWorld(store, 4);
		if (!count) {
			physics::SyncBroadphase(store);
			return fixture;
		}
		scene::CollisionShapes shapes;
		std::vector<Vector3> corners;
		for (unsigned corner = 0; corner < 8; ++corner)
			corners.push_back(
				{corner & 1 ? .75f : -.75f, corner & 2 ? .75f : -.75f, corner & 4 ? .75f : -.75f}
			);
		shapes.SetHull(core::Name("raycast.hull"), collision::BuildConvexHull(corners));
		shapes.SetMesh(core::Name("raycast.ring"), Ring(divisions));
		fixture.Triangles = shapes.FindMesh(core::Name("raycast.ring"))->TriangleCount();
		Require(fixture.Triangles == 8 * divisions * divisions, "raycast mesh triangle count differs");
		store.SetResource(shapes);
		for (size_t index = 0; index < count; ++index) {
			const auto entity = store.Create();
			auto frame = CFrame::Angles(.23f, .41f, -.17f);
			frame.Position = {float(index % 64) * 24, 0, float(index / 64) * 24};
			scene::Collider collider;
			collider.Shape = SHAPES[index % SHAPES.size()];
			collider.Extent = collider.Shape == scene::ShapeKind::Box		? Vector3{1, .5f, 2}
							  : collider.Shape == scene::ShapeKind::Capsule ? Vector3{.5f, 1, .5f}
																			: Vector3{1, 1, 1};
			if (collider.Shape == scene::ShapeKind::Hull) collider.Geometry = core::Name("raycast.hull");
			if (collider.Shape == scene::ShapeKind::Mesh) {
				collider.Geometry = core::Name("raycast.ring");
				// Mesh broadphase bounds use the authored extent, not the baked triangle bounds.
				collider.Extent = {4, .5f, 4};
			}
			store.Set(entity, scene::Transform{frame});
			store.Set(entity, collider);
			if (index % 8 == 0 && collider.Shape != scene::ShapeKind::Mesh)
				store.Set(entity, scene::Motion{});
			if (index < SHAPES.size()) {
				fixture.First[index] = entity;
				fixture.Frames[index] = frame;
			}
		}
		fixture.MeshOrigin = fixture.Frames[5].Position;
		fixture.Competitor = store.Create();
		auto far = fixture.Frames[5];
		far.Position = far.PointToWorldSpace({0, -8, 0});
		store.Set(fixture.Competitor, scene::Transform{far});
		scene::Collider collider;
		collider.Extent = {4, .5f, 4};
		store.Set(fixture.Competitor, collider);
		physics::SyncBroadphase(store);
		const auto *world = store.Resource<physics::PhysicsWorld>();
		Require(
			world->StaticColliders() + world->DynamicColliders() == count + 1,
			"raycast collider count differs"
		);
		return fixture;
	}
	enum class Row { Empty, Front, Back, Nearest, Hole, Broad, Sync };
	const char *RowName(Row row) {
		switch (row) {
		case Row::Empty:
			return "empty";
		case Row::Front:
			return "mixed_front_hits";
		case Row::Back:
			return "mixed_back_hits";
		case Row::Nearest:
			return "mesh_nearest_competitor";
		case Row::Hole:
			return "mesh_hole_narrow_miss";
		case Row::Broad:
			return "broad_miss";
		case Row::Sync:
			return "sync_moved_mesh_and_nearest";
		}
		throw std::runtime_error("unknown raycast benchmark row");
	}
	struct Queries {
		std::array<core::Ray, RAYS> Rays;
		std::array<std::optional<physics::ColliderHit>, RAYS> Expected{}, Actual{};
		float Reach = 6;
	};
	Queries MakeQueries(const Fixture &fixture, Row row) {
		Queries queries;
		queries.Reach = row == Row::Nearest || row == Row::Sync || row == Row::Broad ? 20 : 6;
		constexpr std::array<float, 6> HEIGHT{.5f, 1, 1, 1.5f, .75f, 0};
		for (size_t index = 0; index < RAYS; ++index) {
			if (row == Row::Empty || row == Row::Broad) {
				queries.Rays[index] = {{-64 - float(index), 100, -64}, {0, 1, 0}};
				continue;
			}
			const bool mixed = row == Row::Front || row == Row::Back;
			const size_t shape = mixed ? index % SHAPES.size() : 5;
			const bool back = row == Row::Back;
			const float sign = back ? -1.0f : 1.0f;
			const auto &frame = fixture.Frames[shape];
			const bool hole = row == Row::Hole;
			const float x = shape == 5 && !hole ? -2.123f - float(index % 4) * .031f : 0;
			const float z = shape == 5 && !hole ? .317f + float(index % 7) * .027f : 0;
			queries.Rays[index] = {
				frame.PointToWorldSpace({x, sign * 5, z}), frame.VectorToWorldSpace({0, -sign, 0})
			};
			if (!hole)
				queries.Expected[index] = physics::ColliderHit{
					fixture.First[shape],
					5 - HEIGHT[shape],
					frame.PointToWorldSpace({x, sign * HEIGHT[shape], z}),
					frame.VectorToWorldSpace({0, sign, 0})
				};
		}
		return queries;
	}
	void Query(Fixture &fixture, Queries &queries) {
		for (size_t index = 0; index < RAYS; ++index)
			queries.Actual[index] = physics::Raycast(*fixture.Store, queries.Rays[index], queries.Reach);
	}
	void Verify(const Queries &queries, const Fixture &fixture, Row row, const char *phase) {
		for (size_t index = 0; index < RAYS; ++index) {
			const auto &expected = queries.Expected[index];
			const auto &actual = queries.Actual[index];
			const auto check = [&](bool okay, const char *reason) {
				if (okay) return;
				throw std::runtime_error(
					std::string(reason) + " base=" + std::to_string(fixture.Base) + " row=" + RowName(row) +
					" phase=" + phase + " index=" + std::to_string(index) +
					" expected_owner=" + (expected ? std::to_string(expected->Owner.Id) : "miss") +
					" actual_owner=" + (actual ? std::to_string(actual->Owner.Id) : "miss") +
					" expected_distance=" + (expected ? std::to_string(expected->Distance) : "none") +
					" actual_distance=" + (actual ? std::to_string(actual->Distance) : "none")
				);
			};
			check(bool(actual) == bool(expected), "raycast analytic hit/miss differs");
			if (!actual) continue;
			check(actual->Owner == expected->Owner, "raycast analytic owner differs");
			check(Near(actual->Distance, expected->Distance), "raycast analytic distance differs");
			check(Near(actual->Position, expected->Position), "raycast analytic position differs");
			check(
				Near(actual->Normal, expected->Normal) && Near(actual->Normal.Dot(actual->Normal), 1),
				"raycast analytic unit surface normal differs"
			);
		}
	}
	void VerifyCompetitor(Fixture &fixture, const Queries &queries) {
		const auto far = physics::Raycast(
			*fixture.Store, queries.Rays[0], 20, spatial::LayerMask::All(), fixture.First[5]
		);
		Require(
			far && far->Owner == fixture.Competitor && Near(far->Distance, 12.5f) &&
				Near(far->Position, queries.Rays[0].PointAt(12.5f)) &&
				Near(far->Normal, fixture.Frames[5].VectorToWorldSpace({0, 1, 0})),
			"raycast nearest row has no analytic farther competitor"
		);
	}
	void Move(Fixture &fixture, size_t sample) {
		auto frame = sample % 2 ? CFrame::Angles(-.31f, -.27f, .29f) : CFrame::Angles(.23f, .41f, -.17f);
		frame.Position = fixture.MeshOrigin + Vector3{sample % 2 ? 4096.0f : 0.0f, 0, 0};
		fixture.Frames[5] = frame;
		fixture.Store->Set(fixture.First[5], scene::Transform{frame});
		auto far = frame;
		far.Position = frame.PointToWorldSpace({0, -8, 0});
		fixture.Store->Set(fixture.Competitor, scene::Transform{far});
	}
	struct Report {
		size_t Base = 0, Triangles = 0, Samples = 0, Warmups = 0, Dynamic = 0, Static = 0;
		Row Kind = Row::Empty;
		bool Memory = false, Heap = false;
		std::array<uint64_t, MAX_TRIALS> Times{};
		core::HeapTotals HeapBefore{}, HeapAfter{};
		physics::PhysicsMemoryStats PhysicsBefore{}, PhysicsAfter{};
		uint64_t StaticRebuilds = 0;
		uint64_t AllocatedBytes = 0, AllocatedBlocks = 0;
		uint64_t MeasuredQueries = 0, MeasuredHits = 0;
	};
	std::vector<Report> Latest;
	void Print() {
		for (const auto &report : Latest) {
			std::cout
				<< "raycast-cohort base_colliders=" << report.Base
				<< " total_colliders=" << report.Static + report.Dynamic << " static=" << report.Static
				<< " dynamic=" << report.Dynamic << " row=" << RowName(report.Kind)
				<< " samples=" << report.Samples << " warmups=" << report.Warmups
				<< " calls_per_batch=" << RAYS << " mesh_triangles=" << report.Triangles
				<< " unique_meshes=" << (report.Base ? 1 : 0)
				<< " oracle=all64_owner_distance_position_unit_normal_before_after tolerance=" << TOLERANCE
				<< " setup_and_expected=excluded outer_body_includes_setup_oracles=true static_rebuilds="
				<< report.StaticRebuilds << " static_rebuild_scope=all_row_syncs_including_warmups_and_proofs"
				<< " actual_measured_queries=" << report.MeasuredQueries
				<< " actual_measured_hits=" << report.MeasuredHits
				<< " query_count_scope=measured_batches_excludes_warmups_and_proofs"
				<< " heap_compiled=" << report.Heap << " timing_valid=" << (!report.Memory && !report.Heap)
				<< " measurement_mode="
				<< (report.Memory ? "memory_diagnostic"
					: report.Heap ? "heap_instrumented_latency"
								  : "heap_off_latency")
				<< '\n';
			if (report.Memory) {
				std::cout << "raycast-memory row=" << RowName(report.Kind)
						  << " base_colliders=" << report.Base;
				if (report.Heap)
					std::cout << " allocated_bytes=" << report.AllocatedBytes
							  << " allocated_blocks=" << report.AllocatedBlocks
							  << " process_live_bytes=" << report.HeapAfter.LiveBytes
							  << " process_live_blocks=" << report.HeapAfter.LiveBlocks
							  << " process_lifetime_peak_bytes=" << report.HeapAfter.PeakBytes
							  << " profiler_overhead_bytes=" << report.HeapAfter.OverheadBytes
							  << " heap_dropped_scopes="
							  << report.HeapAfter.DroppedScopes - report.HeapBefore.DroppedScopes;
				else
					std::cout << " allocated_bytes=unavailable allocated_blocks=unavailable "
								 "process_live_bytes=unavailable process_live_blocks=unavailable "
								 "process_lifetime_peak_bytes=unavailable "
								 "profiler_overhead_bytes=unavailable heap_dropped_scopes=unavailable";
				const auto before = report.PhysicsBefore.Total(), after = report.PhysicsAfter.Total();
				std::cout << " physics_live_before=" << before.LiveBytes
						  << " physics_live_after=" << after.LiveBytes
						  << " physics_retained_before=" << before.RetainedBytes
						  << " physics_retained_after=" << after.RetainedBytes
						  << " physics_scope=world_owned_containers_excludes_mesh_geometry_and_ecs "
							 "heap_scope=sum_measured_query_windows_excludes_oracles timing_valid=0\n";
				continue;
			}
			auto times = report.Times;
			std::sort(times.begin(), times.begin() + report.Samples);
			const auto percentile = [&](size_t percent) {
				return times[(report.Samples * percent + 99) / 100 - 1];
			};
			double sum = 0;
			for (size_t index = 0; index < report.Samples; ++index)
				sum += double(times[index]);
			std::cout << "raycast-latency row=" << RowName(report.Kind) << " base_colliders=" << report.Base
					  << " batch_p50_ns=" << percentile(50) << " batch_p95_ns=" << percentile(95)
					  << " batch_p99_ns=" << percentile(99) << " batch_max_ns=" << times[report.Samples - 1]
					  << " derived_mean_ns_per_query=" << sum / double(report.Samples * RAYS)
					  << " quantile_rule=nearest_rank query_tail=not_measured tail_status="
					  << (report.Samples >= 100 ? "at_least100" : "insufficient_below100") << '\n';
		}
	}
	Report Measure(Fixture &fixture, Row row, size_t trials, size_t warmups, bool memory) {
		Report report;
		report.Base = fixture.Base;
		report.Triangles = fixture.Triangles;
		report.Kind = row;
		report.Samples = trials;
		report.Warmups = warmups;
		report.Memory = memory;
		report.Heap = core::HeapProfile::IsCompiledIn();
		auto *world = fixture.Store->Resource<physics::PhysicsWorld>();
		report.Dynamic = world->DynamicColliders();
		report.Static = world->StaticColliders();
		const auto rebuilds = world->StaticRebuilds();
		for (size_t sample = 0; sample < warmups + trials; ++sample) {
			if (row == Row::Sync) {
				Move(fixture, sample);
				physics::SyncBroadphase(*fixture.Store);
			}
			auto queries = MakeQueries(fixture, row);
			Query(fixture, queries);
			Verify(queries, fixture, row, "before");
			if (row == Row::Nearest || row == Row::Sync) VerifyCompetitor(fixture, queries);
			// Move to the other chart after the untimed proof, so sync cannot hit a cached pose.
			if (row == Row::Sync) {
				Move(fixture, sample + 1);
				queries = MakeQueries(fixture, row);
			}
			core::HeapTotals before;
			if (memory && report.Heap) before = core::HeapProfile::Totals();
			if (sample == warmups) {
				report.HeapBefore = before;
				report.PhysicsBefore = world->MemoryStats();
			}
			const auto begin = Clock::now();
			if (row == Row::Sync) physics::SyncBroadphase(*fixture.Store);
			Query(fixture, queries);
			const auto nanoseconds = Elapsed(begin);
			if (sample >= warmups) report.Times[sample - warmups] = nanoseconds;
			if (memory && report.Heap) {
				report.HeapAfter = core::HeapProfile::Totals();
				if (sample >= warmups) {
					report.AllocatedBytes += report.HeapAfter.TotalBytes - before.TotalBytes;
					report.AllocatedBlocks += report.HeapAfter.TotalBlocks - before.TotalBlocks;
				}
				Require(
					report.HeapAfter.DroppedScopes == before.DroppedScopes,
					"raycast heap attribution dropped scopes"
				);
			}
			if (sample >= warmups) {
				report.MeasuredQueries += RAYS;
				report.MeasuredHits += static_cast<uint64_t>(std::count_if(
					queries.Actual.begin(), queries.Actual.end(), [](const auto &hit) { return bool(hit); }
				));
			}
			Verify(queries, fixture, row, "after");
			if (row == Row::Nearest || row == Row::Sync) VerifyCompetitor(fixture, queries);
		}
		report.PhysicsAfter = world->MemoryStats();
		report.StaticRebuilds = world->StaticRebuilds() - rebuilds;
		return report;
	}
	void Run() {
		const bool enabled = Flag("MONO_PHYSICS_RAYCAST_BENCH");
		const bool memory = Flag("MONO_PHYSICS_RAYCAST_MEMORY");
		if (!enabled) {
			std::cout << "raycast skipped=opt_in_required env=MONO_PHYSICS_RAYCAST_BENCH\n";
			return;
		}
		const size_t trials = Option("MONO_PHYSICS_RAYCAST_TRIALS", 101, 1, MAX_TRIALS);
		const size_t warmups = Option("MONO_PHYSICS_RAYCAST_WARMUPS", 3, 0, 32);
		static const bool registered = [] {
			Require(std::atexit(Print) == 0, "raycast summary registration failed");
			return true;
		}();
		(void)registered;
		Latest.clear();
		Latest.reserve(17);
		for (size_t cohort = 0; cohort < 4; ++cohort) {
			constexpr std::array<size_t, 4> counts{0, 6, 1024, 4096}, divisions{0, 1, 8, 128};
			auto fixture = MakeFixture(counts[cohort], divisions[cohort]);
			if (!cohort)
				Latest.push_back(Measure(fixture, Row::Empty, trials, warmups, memory));
			else {
				for (const auto row : {Row::Front, Row::Back, Row::Nearest, Row::Hole, Row::Broad})
					Latest.push_back(Measure(fixture, row, trials, warmups, memory));
				if (cohort == 3) Latest.push_back(Measure(fixture, Row::Sync, trials, warmups, memory));
			}
		}
	}
}

BENCH("Public physics raycast | analytic mixed shapes and resolved ring mesh", 1) {
	Run();
}

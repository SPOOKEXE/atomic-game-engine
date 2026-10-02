#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Bench.hpp>

#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.imagegraphphysics.bench.replay")
using namespace engine::imagegraph;
namespace {
	struct Workload {
		SourceRigidHistory History;
		SourceRigidSnapshot Expected;
		uint64_t Tick;
		Workload(uint32_t count, uint64_t tick) : Tick(tick) {
			History.World.Dimension = {320, 320};
			History.World.Scale = 20;
			History.World.Walls = 15;
			History.World.WallRestitution = 0;
			History.Frames.resize(tick + 1);
			for (uint32_t index = 0; index < count; ++index) {
				SourceRigidBody body;
				body.Id = "body/" + std::to_string(index);
				body.Position = {80. + (index % 8) * 20., 100. + (index / 8) * 20.};
				body.Size = {18, 18};
				body.Restitution = 0;
				History.Frames[0].Events.push_back({{body.Id, 0}, std::move(body)});
			}
			for (auto &frame : History.Frames)
				frame.Events.push_back({{"render", 0}, SourceRigidStep{}});
			Diagnostic diagnostic;
			if (engine::imagegraphphysics::ReplayRigid(History, Tick, Expected, diagnostic) != Status::Ok ||
				Expected.Bodies.size() != count || Expected.Contacts.empty())
				throw std::runtime_error(
					"Rigid benchmark requires real settled contacts: " + diagnostic.Message
				);
			const auto recorded = History;
			SourceRigidSnapshot earlier, repeated;
			if (engine::imagegraphphysics::ReplayRigid(History, Tick / 2, earlier, diagnostic) !=
					Status::Ok ||
				engine::imagegraphphysics::ReplayRigid(History, Tick, repeated, diagnostic) != Status::Ok ||
				repeated != Expected || History != recorded)
				throw std::runtime_error("Rigid benchmark seek/reset or copied-history parity failed");
		}
		void Run() const {
			SourceRigidSnapshot snapshot;
			Diagnostic diagnostic;
			if (engine::imagegraphphysics::ReplayRigid(History, Tick, snapshot, diagnostic) != Status::Ok ||
				snapshot != Expected)
				throw std::runtime_error("Rigid benchmark replay parity failed: " + diagnostic.Message);
			engine::testing::Consume(snapshot.Contacts.size());
		}
	};
} // namespace
BENCH("16 bodies, 181 frames, contacts and exact replay snapshot", 3) {
	static const Workload workload{16, 180};
	for (int pass = 0; pass < 3; ++pass)
		workload.Run();
}
BENCH("64 bodies, 601 frames, contacts and exact replay snapshot", 1) {
	static const Workload workload{64, 600};
	workload.Run();
}

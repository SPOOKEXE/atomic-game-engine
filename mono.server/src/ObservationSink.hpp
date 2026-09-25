#pragma once

// Writes completed physics and portal observation records to a JSONL file, one
// file per process, so a test or a person can read one run across every host.
//
// Records are copied from each local world after its tick, the same value copies
// the MCP control features read; this only chooses a different reader. Each
// world keeps a cursor, so a record is written once even though the rings hold
// it for many ticks.

#include <engine/world/Universe.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace server {

	class ObservationSink {
	  public:
		// Opens `<directory>/<process>.jsonl` for appending. `trace` becomes every
		// local world's default portal trace id.
		bool Open(const std::filesystem::path &directory, std::string process, uint64_t trace);

		// Writes records produced since the last call for every local world.
		void Collect(engine::world::Universe &worlds);

	  private:
		struct Cursor {
			uint64_t Portal = 0;
			uint64_t PhysicsTick = 0;
			uint64_t PhysicsStep = 0;
			int PhysicsBoundary = -1;
			bool Traced = false;
		};

		std::ofstream Out;
		std::string Process;
		uint64_t Trace = 0;
		std::unordered_map<std::string, Cursor> Cursors;
	};
}

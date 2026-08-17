#include <engine/core/Flags.hpp>

#include <loadtest/Options.hpp>

namespace loadtest {

	namespace {
		// **Built from a default-constructed `Options`**, so the numbers here are
		// the member initialisers in `Options.hpp` and not a second copy of them.
		// The same shape `server::Settings` uses, for the same reason.
		const engine::core::FlagTableBuilder &Table() {
			static const engine::core::FlagTableBuilder table = [] {
				const Options defaults;
				engine::core::FlagTableBuilder built;

				built.Integer("loadtest.clients", defaults.Clients, "How many virtual clients to open");
				built.Text("loadtest.address", defaults.Address, "The server's address");
				built.Integer("loadtest.port", defaults.Port, "The server's UDP port");
				built.Number(
					"loadtest.tick-rate", defaults.TickRate, "How fast the harness ticks its clients"
				);
				built.Number(
					"loadtest.seconds", defaults.Seconds, "Run for this long, or zero for a tick budget"
				);
				built.Integer(
					"loadtest.ticks", defaults.Ticks, "Run this many ticks, or zero for a time budget"
				);
				built.Integer(
					"loadtest.connects-per-tick",
					defaults.ConnectsPerTick,
					"How many sessions may start dialling on one tick"
				);
				built.Integer(
					"loadtest.input-every-ticks",
					defaults.InputEveryTicks,
					"How often a client submits an input, in harness ticks"
				);
				built.Number(
					"loadtest.stall-seconds",
					defaults.StallSeconds,
					"How long a session may make no progress before it is written off"
				);
				built.Text(
					"loadtest.profile-out",
					defaults.ProfilePath.string(),
					"Fold this run's frame graph into a .folded file for scripts/flamegraph.py"
				);

				return built;
			}();
			return table;
		}
	}

	bool DeclareFlags() {
		return engine::core::Flags::Declare(Table().Rows());
	}

	Options OptionsFromFlags() {
		Options options;
		if (!engine::core::Flags::Has("loadtest.clients")) {
			return options;
		}

		using engine::core::Flag;

		options.Clients = static_cast<uint32_t>(Flag("loadtest.clients").Integer());
		options.Address = std::string(Flag("loadtest.address").Text());
		options.Port = static_cast<uint16_t>(Flag("loadtest.port").Integer());
		options.TickRate = Flag("loadtest.tick-rate").Number();
		options.Seconds = Flag("loadtest.seconds").Number();
		options.Ticks = Flag("loadtest.ticks").Integer();
		options.ConnectsPerTick = static_cast<uint32_t>(Flag("loadtest.connects-per-tick").Integer());
		options.InputEveryTicks = static_cast<uint32_t>(Flag("loadtest.input-every-ticks").Integer());
		options.StallSeconds = Flag("loadtest.stall-seconds").Number();
		options.ProfilePath = std::filesystem::path(Flag("loadtest.profile-out").Text());

		return options;
	}
}

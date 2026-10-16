#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralService.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace engine::script {
	namespace procedural_surface_detail {
		using V = ScriptValue;
		V Number(double number) {
			V out(ValueTag::Number);
			out.Number = number;
			return out;
		}
		V Text(std::string text) {
			V out(ValueTag::String);
			out.Text = std::move(text);
			return out;
		}
		V Record(std::initializer_list<std::pair<std::string, V>> entries) {
			V out(ValueTag::Map);
			out.Entries = entries;
			return out;
		}
		V Read(ScriptCall &call, size_t index, size_t owned = 256 * 1024) {
			V value;
			CodecStatus why{};
			if (!call.ReadBoundedValue(index, value, why, owned))
				call.Raise("procedural input exceeds owned-byte limit");
			std::vector<std::byte> encoded;
			if (Encode(value, encoded) != CodecStatus::Ok)
				call.Raise("procedural input exceeds 64 KiB record limit");
			return value;
		}
		const V &Field(ScriptCall &call, const V &record, std::string_view key, bool required = true) {
			static const V empty;
			if (record.Tag != ValueTag::Map && !(record.Tag == ValueTag::Array && record.Items.empty()))
				call.Raise("procedural record expected");
			const V *found = nullptr;
			for (const auto &[name, value] : record.Entries)
				if (name == key) {
					if (found) call.Raise("duplicate procedural field");
					found = &value;
				}
			if (found) return *found;
			if (required) call.Raise("missing procedural field");
			return empty;
		}
		void Keys(ScriptCall &call, const V &record, std::initializer_list<std::string_view> allowed) {
			(void)Field(call, record, "", false);
			for (const auto &[name, value] : record.Entries) {
				(void)value;
				if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
					call.Raise("unknown procedural field");
			}
		}
		const std::vector<V> &Items(ScriptCall &call, const V &array, size_t maximum) {
			if ((array.Tag != ValueTag::Array && !(array.Tag == ValueTag::Map && array.Entries.empty())) ||
				array.Items.size() > maximum)
				call.Raise("bounded procedural array expected");
			return array.Items;
		}
		std::string String(ScriptCall &call, const V &value, bool empty = false) {
			if (value.Tag != ValueTag::String || value.Text.size() > 128 || (!empty && value.Text.empty()) ||
				value.Text.find('\0') != std::string::npos)
				call.Raise("bounded procedural string expected");
			return value.Text;
		}
		int64_t Integer(ScriptCall &call, const V &value, int64_t minimum, int64_t maximum) {
			if (value.Tag != ValueTag::Number || !std::isfinite(value.Number) ||
				std::floor(value.Number) != value.Number || value.Number < static_cast<double>(minimum) ||
				value.Number > static_cast<double>(maximum))
				call.Raise("procedural integer out of range");
			return static_cast<int64_t>(value.Number);
		}
		uint64_t Identity(ScriptCall &call, const V &value) {
			const auto text = String(call, value);
			uint64_t number = 0;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || text.size() > 20)
				call.Raise("procedural uint64 decimal string expected");
			return number;
		}
		int64_t Coordinate(ScriptCall &call, const V &value) {
			if (value.Tag != ValueTag::Number || !std::isfinite(value.Number))
				call.Raise("procedural coordinate expected");
			const double scaled = value.Number * procedural::SUBCELLS_PER_CELL;
			if (std::abs(scaled) > 9007199254740991.0 || std::floor(scaled) != scaled)
				call.Raise("coordinate must be exactly representable in fixed subcells");
			return static_cast<int64_t>(scaled);
		}
		procedural::RegionScope Scope(ScriptCall &call, const V &value) {
			Keys(call, value, {"X", "Z", "Cells"});
			return {
				static_cast<int32_t>(Integer(call, Field(call, value, "X"), INT32_MIN, INT32_MAX)),
				static_cast<int32_t>(Integer(call, Field(call, value, "Z"), INT32_MIN, INT32_MAX)),
				static_cast<uint16_t>(Integer(call, Field(call, value, "Cells"), 1, 64))
			};
		}
		procedural::SourceKind Kind(ScriptCall &call, const V &value) {
			const auto name = String(call, value);
			if (name == "Village") return procedural::SourceKind::Village;
			if (name == "Entrance") return procedural::SourceKind::Entrance;
			call.Raise("source Kind must be Village or Entrance");
			return procedural::SourceKind::Village;
		}
		ProceduralSource Source(ScriptCall &call, const V &value) {
			Keys(call, value, {"Kind", "Name", "X", "Z", "TargetVillage"});
			ProceduralSource out;
			out.Kind = Kind(call, Field(call, value, "Kind"));
			out.Name = String(call, Field(call, value, "Name"));
			out.Anchor = {
				Coordinate(call, Field(call, value, "X")), Coordinate(call, Field(call, value, "Z"))
			};
			const auto &target = Field(call, value, "TargetVillage", false);
			if (target.Tag != ValueTag::Nil) out.TargetVillage = String(call, target, true);
			return out;
		}
		std::shared_ptr<ProceduralEndpoint> Endpoint(ScriptCall &call) {
			auto endpoint = call.Procedural();
			if (!endpoint || !endpoint->BelongsTo(call.World(), call.RuntimeIdentity()))
				call.Raise("procedural host unavailable");
			return endpoint;
		}
		void Result(ScriptCall &call, bool ok, const procedural::Diagnostic &diagnostic) {
			if (!ok)
				call.Raise(
					diagnostic.Field.empty() ? "procedural operation refused" : diagnostic.Field.c_str()
				);
			call.ReturnBoolean(true);
		}
		void Configure(ScriptCall &call) {
			const auto config = Read(call, 0);
			Keys(call, config, {"Name", "Seed", "Regions", "Sources", "Parameters"});
			auto recipe = procedural::ForestVillageDefaults(
				String(call, Field(call, config, "Name")), Identity(call, Field(call, config, "Seed"))
			);
			const auto &parameters = Field(call, config, "Parameters", false);
			std::vector<std::pair<std::string, std::string>> changed;
			changed.reserve(13);
			if (parameters.Tag != ValueTag::Nil)
				for (const auto &entry : Items(call, parameters, 13)) {
					Keys(call, entry, {"Node", "Name", "Value"});
					const auto nodeId = String(call, Field(call, entry, "Node"));
					const auto name = String(call, Field(call, entry, "Name"));
					for (const auto &prior : changed)
						if (prior.first == nodeId && prior.second == name)
							call.Raise("duplicate procedural parameter");
					changed.emplace_back(nodeId, name);
					auto node = std::find_if(recipe.Nodes.begin(), recipe.Nodes.end(), [&](const auto &row) {
						return row.Id == nodeId;
					});
					if (node == recipe.Nodes.end()) call.Raise("unknown procedural node");
					auto parameter =
						std::find_if(node->Parameters.begin(), node->Parameters.end(), [&](const auto &row) {
							return row.Name == name;
						});
					if (parameter == node->Parameters.end()) call.Raise("unknown procedural parameter");
					parameter->Value =
						static_cast<uint64_t>(Integer(call, Field(call, entry, "Value"), 0, 1000000));
				}
			std::vector<procedural::RegionScope> scopes;
			std::vector<ProceduralSource> sources;
			for (const auto &scope :
				 Items(call, Field(call, config, "Regions"), ProceduralHostLimits::MaximumRegions))
				scopes.push_back(Scope(call, scope));
			for (const auto &source :
				 Items(call, Field(call, config, "Sources"), ProceduralHostLimits::MaximumSources))
				sources.push_back(Source(call, source));
			procedural::Diagnostic diagnostic;
			Result(
				call, Endpoint(call)->Configure(call.World(), recipe, scopes, sources, diagnostic), diagnostic
			);
		}
		void SetSource(ScriptCall &call) {
			const auto value = Source(call, Read(call, 0, 8192));
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->SetSource(call.World(), value, diagnostic), diagnostic);
		}
		void RemoveSource(ScriptCall &call) {
			const auto value = Read(call, 0, 4096);
			Keys(call, value, {"Kind", "Name"});
			const auto kind = Kind(call, Field(call, value, "Kind"));
			const auto name = String(call, Field(call, value, "Name"));
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->RemoveSource(call.World(), kind, name, diagnostic), diagnostic);
		}
		void SetSpline(ScriptCall &call) {
			const auto value = Read(call, 0, 8192);
			Keys(call, value, {"Name", "EntranceName", "ControlX", "ControlZ"});
			procedural::AuthoredSpline spline{
				String(call, Field(call, value, "Name")),
				String(call, Field(call, value, "EntranceName")),
				{Coordinate(call, Field(call, value, "ControlX")),
				 Coordinate(call, Field(call, value, "ControlZ"))}
			};
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->SetSpline(call.World(), spline, diagnostic), diagnostic);
		}
		void RemoveSpline(ScriptCall &call) {
			const auto name = String(call, Read(call, 0, 4096));
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->RemoveSpline(call.World(), name, diagnostic), diagnostic);
		}
		void SetOverride(ScriptCall &call) {
			const auto value = Read(call, 0, 8192);
			procedural::PlacementOverride edit;
			edit.TargetId = String(call, Field(call, value, "TargetId"));
			const auto action = String(call, Field(call, value, "Action"));
			if (action == "Delete")
				Keys(call, value, {"TargetId", "Action"});
			else if (action == "Replace") {
				Keys(
					call,
					value,
					{"TargetId",
					 "Action",
					 "X",
					 "Z",
					 "RotationQuarterTurns",
					 "Variant",
					 "Material",
					 "SourceName"}
				);
				edit.Action = procedural::OverrideAction::Replace;
				edit.Position = {
					Coordinate(call, Field(call, value, "X")), Coordinate(call, Field(call, value, "Z"))
				};
				edit.RotationQuarterTurns =
					static_cast<uint8_t>(Integer(call, Field(call, value, "RotationQuarterTurns"), 0, 3));
				edit.Variant = static_cast<uint16_t>(Integer(call, Field(call, value, "Variant"), 0, 63));
				edit.Material = static_cast<uint16_t>(Integer(call, Field(call, value, "Material"), 0, 63));
				edit.SourceName = String(call, Field(call, value, "SourceName"), true);
			} else
				call.Raise("override Action must be Delete or Replace");
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->SetOverride(call.World(), edit, diagnostic), diagnostic);
		}
		void RemoveOverride(ScriptCall &call) {
			const auto name = String(call, Read(call, 0, 4096));
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->RemoveOverride(call.World(), name, diagnostic), diagnostic);
		}
		void Generate(ScriptCall &call) {
			const auto scope = Scope(call, Read(call, 0, 4096));
			const auto mesh = call.AsInstance(1);
			const auto part = call.AsInstance(2);
			uint64_t ticket = 0;
			procedural::Diagnostic diagnostic;
			if (!Endpoint(call)->Generate(call.World(), scope, mesh, part, ticket, diagnostic))
				call.Raise(diagnostic.Field.c_str());
			call.ReturnString(std::to_string(ticket));
		}
		void UseBaked(ScriptCall &call) {
			(void)Endpoint(call);
			const auto scope = Scope(call, Read(call, 0, 4096));
			const auto mesh = call.AsInstance(1);
			const auto part = call.AsInstance(2);
			const auto name = Read(call, 3, 4096);
			if (name.Tag != ValueTag::String || name.Text.size() > 512)
				call.Raise("bounded procedural description name expected");
			procedural::Diagnostic diagnostic;
			Result(
				call,
				SetBakedProceduralReference(call.World(), scope, mesh, part, name.Text, diagnostic),
				diagnostic
			);
		}
		void Progress(ScriptCall &call) {
			const auto ticket = Identity(call, Read(call, 0, 4096));
			ProceduralServiceProgress progress;
			procedural::Diagnostic diagnostic;
			if (!Endpoint(call)->Progress(call.World(), ticket, progress, diagnostic))
				call.Raise(diagnostic.Field.c_str());
			const char *status = "Pending";
			switch (progress.Work.Status) {
			case ProceduralJobStatus::Pending:
				break;
			case ProceduralJobStatus::Ready:
				status = "Ready";
				break;
			case ProceduralJobStatus::Canceled:
				status = "Canceled";
				break;
			case ProceduralJobStatus::Failed:
				status = "Failed";
				break;
			case ProceduralJobStatus::Published:
				status = "Published";
				break;
			}
			constexpr char hex[] = "0123456789abcdef";
			std::string digest(64, '0');
			for (size_t i = 0; i < 32; ++i) {
				digest[i * 2] = hex[progress.ArtifactDigest.Bytes[i] >> 4];
				digest[i * 2 + 1] = hex[progress.ArtifactDigest.Bytes[i] & 15];
			}
			call.ReturnValue(Record(
				{{"Status", Text(status)},
				 {"CandidateCells", Number(progress.Work.CandidateCells)},
				 {"WorkUnits", Number(progress.Work.WorkUnits)},
				 {"SpatialTests", Number(progress.Work.SpatialTests)},
				 {"Placements", Number(progress.Work.Placements)},
				 {"Roads", Number(progress.Work.Roads)},
				 {"ArtifactDigest", Text(std::move(digest))},
				 {"Diagnostic", Text(progress.Diagnostic.Field)}}
			));
		}
		void Cancel(ScriptCall &call) {
			const auto ticket = Identity(call, Read(call, 0, 4096));
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->Cancel(call.World(), ticket, diagnostic), diagnostic);
		}
		void Release(ScriptCall &call) {
			const auto ticket = Identity(call, Read(call, 0, 4096));
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->Release(call.World(), ticket, diagnostic), diagnostic);
		}
		void Publish(ScriptCall &call) {
			const auto values = Read(call, 0, 8192);
			std::vector<uint64_t> tickets;
			for (const auto &value : Items(call, values, ProceduralHostLimits::MaximumRetainedRequests))
				tickets.push_back(Identity(call, value));
			procedural::Diagnostic diagnostic;
			Result(call, Endpoint(call)->Publish(call.World(), tickets, diagnostic), diagnostic);
		}
		constexpr std::array<ServiceMethod, 13> METHODS{
			{{"ConfigureForestVillage", Configure},
			 {"SetSource", SetSource},
			 {"RemoveSource", RemoveSource},
			 {"SetSpline", SetSpline},
			 {"RemoveSpline", RemoveSpline},
			 {"SetOverride", SetOverride},
			 {"RemoveOverride", RemoveOverride},
			 {"Generate", Generate},
			 {"UseBaked", UseBaked},
			 {"Progress", Progress},
			 {"Cancel", Cancel},
			 {"Publish", Publish},
			 {"Release", Release}}
		};
	} // namespace procedural_surface_detail
	const ServiceSurface &ProceduralServiceSurface() {
		static const ServiceSurface surface = [] {
			ServiceSurface value;
			value.Name = "ProceduralService";
			value.Methods = procedural_surface_detail::METHODS;
			return value;
		}();
		return surface;
	}
} // namespace engine::script

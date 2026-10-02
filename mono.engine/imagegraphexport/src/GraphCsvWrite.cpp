#include "GraphCsvWrite.hpp"

#include "SourceCsvNumber.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		bool Fail(std::string &failure, const char *reason) {
			failure = reason;
			return false;
		}
		struct Writer {
			std::string Text;
			uint64_t Maximum = 0, RemainingEntries = Limits::MaximumArrayElements;
			bool Append(std::string_view value) {
				if (Text.size() > Maximum || value.size() > Maximum - Text.size()) return false;
				Text += value;
				return true;
			}
			template <class Variant> bool Primitive(const Variant &value, bool quote) {
				if (!RemainingEntries) return false;
				--RemainingEntries;
				return std::visit(
					[&](const auto &v) -> bool {
						using T = std::decay_t<decltype(v)>;
						if constexpr (std::is_same_v<T, std::string>)
							return (!quote || Append("\"")) && Append(v) && (!quote || Append("\""));
						else if constexpr (std::is_same_v<T, bool>)
							return Append(v ? "1" : "0");
						else if constexpr (std::is_same_v<T, int64_t>) {
							char text[32];
							const auto result = std::to_chars(text, text + sizeof(text), v);
							return result.ec == std::errc{} && Append({text, size_t(result.ptr - text)});
						} else if constexpr (std::is_same_v<T, double>)
							return std::isfinite(v) && Append(detail::CsvNumber(v));
						else if constexpr (std::is_same_v<T, UndefinedValue>)
							return Append("undefined");
						else
							return false;
					},
					value
				);
			}
			bool Item(const SourceArrayItem &item, bool quote, size_t depth) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) return Primitive(*leaf, quote);
				const auto &children = std::get<std::vector<SourceArrayItem>>(item.Data);
				return Items(children, true, depth + 1);
			}
			bool Items(const std::vector<SourceArrayItem> &items, bool brackets, size_t depth) {
				if (depth > Limits::MaximumArrayDepth || !RemainingEntries) return false;
				--RemainingEntries;
				if (brackets && !Append("[ ")) return false;
				for (size_t i = 0; i < items.size(); ++i)
					if ((i && !Append(brackets ? "," : ", ")) || !Item(items[i], brackets, depth))
						return false;
				return !brackets || Append(" ]");
			}
			bool Row(const std::vector<ElementValue> &row) {
				for (size_t j = 0; j < row.size(); ++j)
					if ((j && !Append(", ")) || !Primitive(row[j], false)) return false;
				return Append("\n");
			}
			bool Content(const Value &value) {
				const auto *array = std::get_if<ArrayValue>(&value);
				if (!array) return Primitive(value, false);
				const unsigned forms = unsigned(!array->Elements.empty()) + unsigned(!array->Items.empty()) +
									   unsigned(!array->Nested.empty());
				if (forms > 1) return false;
				if (!array->Nested.empty()) {
					for (const auto &row : array->Nested)
						if (!Row(row)) return false;
					return true;
				}
				if (!array->Items.empty()) {
					for (size_t i = 0; i < array->Items.size(); ++i) {
						const auto &item = array->Items[i];
						if (const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
							if (!Items(*row, false, 1) || !Append("\n")) return false;
						} else if ((i && !Append(", ")) || !Item(item, false, 0))
							return false;
					}
					return true;
				}
				for (size_t i = 0; i < array->Elements.size(); ++i)
					if ((i && !Append(", ")) || !Primitive(array->Elements[i], false)) return false;
				return true;
			}
		};
		bool Capture(
			std::span<const GraphFileGrant> grants,
			const engine::assets::ContentPolicy &policy,
			const HostNodeInvocation &in,
			HostNodeCapture &output,
			std::string &failure
		) {
			ENGINE_PROFILE_CAT("image composer CSV file export", engine::core::ProfileCategory::Engine);
			if (in.Authored.Type != "pc.csv_file_write") return Fail(failure, "not a source CSV writer");
			const Value *content = nullptr;
			const std::string *path = nullptr;
			for (const auto &input : in.Inputs) {
				if (input.Port == "path")
					path = std::get_if<std::string>(&input.Data);
				else if (input.Port == "content")
					content = &input.Data;
			}
			if (!path || path->empty() || path->size() > 4096 || !content)
				return Fail(failure, "CSV writer requires a bounded path and source content");
			auto destination = std::filesystem::path(*path);
			if (destination.extension() != ".csv") destination += ".csv";
			if (!destination.is_absolute() || destination.lexically_normal() != destination)
				return Fail(failure, "CSV writer destination must be an absolute normalized path");
			const GraphFileGrant *grant = nullptr;
			for (const auto &candidate : grants)
				if (candidate.NodeId == in.Authored.Id && candidate.Resource.empty()) {
					if (grant) return Fail(failure, "CSV write grant is duplicated");
					grant = &candidate;
				}
			if (!grant || !grant->Write || grant->File != destination ||
				!policy.AllowsName(destination.string()))
				return Fail(failure, "CSV writer requires the exact suffixed destination write grant");
			const auto maximum = std::min<uint64_t>(in.MaximumOperationBytes, Limits::MaximumEvaluationBytes);
			uint64_t retained = in.Inputs.size() * sizeof(AuthoredValue);
			const auto authored = NodeClonePayloadBytes(in.Authored);
			if (!authored || *authored > maximum / 4 || retained > maximum / 4 - *authored)
				return Fail(failure, "CSV recording exceeds its operation budget");
			retained += *authored;
			for (const auto &input : in.Inputs) {
				const auto bytes = ValueClonePayloadBytes(input.Data);
				if (!bytes || input.Port.size() + 1 > maximum / 4 - retained ||
					*bytes > maximum / 4 - retained - input.Port.size() - 1)
					return Fail(failure, "CSV resolved controls exceed their recording budget");
				retained += *bytes + input.Port.size() + 1;
			}
			HostNodeCapture candidate;
			candidate.Authored = in.Authored;
			candidate.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
			candidate.Tick = in.Request.Tick;
			candidate.Subframe = in.Request.Subframe;
			candidate.NegativeFrame = in.Request.NegativeFrame;
			Writer writer;
			writer.Maximum = std::min<uint64_t>(Limits::MaximumTextBytes, maximum / 4);
			if (!writer.Content(*content))
				return Fail(
					failure,
					"CSV source content needs unsupported runtime string conversion or exceeds its limits"
				);
			engine::core::Metrics::Count(
				"image composer CSV file bytes prepared", static_cast<double>(writer.Text.size())
			);
			engine::core::Metrics::Count("image composer CSV file publication attempts", 1);
			if (!PublishGraphHostFile(
					*grant,
					policy,
					{reinterpret_cast<const std::byte *>(writer.Text.data()), writer.Text.size()},
					maximum,
					failure
				))
				return false;
			output = std::move(candidate);
			failure.clear();
			return true;
		}
	}
	bool CaptureGraphCsvWrite(
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		const engine::imagegraph::HostNodeInvocation &in,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	) {
		try {
			return Capture(grants, policy, in, output, failure);
		} catch (const std::bad_alloc &) {
			return Fail(failure, "CSV writer allocation exceeds its operation budget");
		} catch (const std::filesystem::filesystem_error &) {
			return Fail(failure, "CSV writer filesystem operation failed");
		}
	}
}

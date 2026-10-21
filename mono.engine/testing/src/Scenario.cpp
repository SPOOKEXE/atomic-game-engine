#include <engine/testing/Scenario.hpp>

#include <limits>

namespace engine::testing {

	void CheckResult::Record(std::string_view property, bool passed, std::string_view message) {
		Passed = Passed && passed;
		const size_t bytes = detail::ScenarioSum(property.size(), message.size());
		if (Findings.size() >= MaximumFindings || bytes > MaximumFindingBytes - RetainedFindingBytes) {
			Passed = false;
			DroppedFindings = detail::ScenarioSum(DroppedFindings, 1);
			DroppedFindingBytes = detail::ScenarioSum(DroppedFindingBytes, bytes);
			return;
		}
		Findings.push_back({std::string(property), std::string(message), passed});
		RetainedFindingBytes += bytes;
	}

	size_t CheckResult::FindingBytes() const {
		return RetainedFindingBytes;
	}

	void
	ScenarioReport::RecordFailure(std::string_view message, size_t maximumBytes, std::string_view prefix) {
		constexpr size_t MAXIMUM_FAILURES = 8;
		const size_t messageBytes = detail::ScenarioSum(prefix.size(), message.size());
		size_t retainedBytes = 0;
		for (const auto &failure : Failures)
			retainedBytes = detail::ScenarioSum(retainedBytes, failure.size());
		if (Failures.size() >= MAXIMUM_FAILURES || retainedBytes > maximumBytes ||
			messageBytes > maximumBytes - retainedBytes) {
			DroppedFailures = detail::ScenarioSum(DroppedFailures, 1);
			DroppedFailureBytes = detail::ScenarioSum(DroppedFailureBytes, messageBytes);
			return;
		}
		Failures.emplace_back(prefix);
		Failures.back().append(message);
	}

	bool ScenarioReport::Passed() const {
		return Failures.empty() && DroppedFailures == 0 && !Checks.empty() &&
			   std::all_of(Checks.begin(), Checks.end(), [](const CheckResult &check) {
				   return check.Passed && check.DroppedFindings == 0;
			   });
	}

	namespace detail {
		std::string_view ScenarioException() {
			try {
				throw;
			} catch (const std::exception &failure) {
				return failure.what();
			} catch (...) {
				// Nonstandard exceptions still need a stable attributed diagnosis.
				return "unknown scenario exception";
			}
		}

		size_t ScenarioSum(size_t left, size_t right) {
			return right > std::numeric_limits<size_t>::max() - left ? std::numeric_limits<size_t>::max()
																	 : left + right;
		}

		size_t ScenarioKeyBytes(const ScenarioKey &key) {
			size_t bytes = sizeof(ScenarioKey);
			for (const auto *text :
				 {&key.Scene,
				  &key.ContentRevision,
				  &key.Configuration,
				  &key.TickSchedule,
				  &key.FrameSchedule,
				  &key.Actions,
				  &key.Transport,
				  &key.Faults,
				  &key.CaptureConfiguration,
				  &key.ObservationSchema,
				  &key.CheckSet}) {
				bytes = ScenarioSum(bytes, text->size());
			}
			return bytes;
		}

		size_t ScenarioReportBytes(const ScenarioReport &report) {
			size_t bytes =
				ScenarioSum(sizeof(ScenarioReport) - sizeof(ScenarioKey), ScenarioKeyBytes(report.Key));
			for (const auto &check : report.Checks) {
				bytes = ScenarioSum(bytes, sizeof(CheckResult));
				bytes = ScenarioSum(bytes, check.Name.size());
				bytes = ScenarioSum(bytes, check.SourceSuite.size());
				for (const auto &finding : check.Findings) {
					bytes = ScenarioSum(bytes, sizeof(Finding));
					bytes = ScenarioSum(bytes, finding.Property.size());
					bytes = ScenarioSum(bytes, finding.Message.size());
				}
			}
			for (const auto &failure : report.Failures) {
				bytes = ScenarioSum(bytes, sizeof(std::string));
				bytes = ScenarioSum(bytes, failure.size());
			}
			return bytes;
		}
	}
}

#include <algorithm>
#include <cstdio>
#include <loadtest/Statistics.hpp>

namespace loadtest {

	namespace {
		// One row of the report. Kept to a helper so the block lines up whatever
		// the numbers are - a report an operator diffs between two runs has to
		// diff cleanly.
		void Line(std::string &into, const char *label, const char *format, double value) {
			char text[256];
			std::snprintf(text, sizeof(text), format, value);
			char row[320];
			std::snprintf(row, sizeof(row), "  %-28s %s\n", label, text);
			into += row;
		}

		void Count(std::string &into, const char *label, uint64_t value) {
			char row[320];
			std::snprintf(row, sizeof(row), "  %-28s %llu\n", label, static_cast<unsigned long long>(value));
			into += row;
		}
	}

	double Percentile(std::vector<double> readings, double fraction) {
		if (readings.empty()) {
			return 0.0;
		}
		std::sort(readings.begin(), readings.end());
		const auto rank = static_cast<size_t>(fraction * static_cast<double>(readings.size() - 1) + 0.5);
		return readings[std::min(rank, readings.size() - 1)];
	}

	Summary Summarise(std::span<const SessionReport> reports, double seconds) {
		Summary summary;
		summary.Sessions = reports.size();
		summary.Seconds = seconds;

		std::vector<double> admitted;
		std::vector<double> joined;
		double admitTotal = 0.0;
		double joinTotal = 0.0;

		bool first = true;
		for (const SessionReport &report : reports) {
			switch (report.Final) {
			case Stage::Dialling:
				summary.Dialling++;
				break;
			case Stage::Streaming:
				summary.Streaming++;
				break;
			case Stage::Playing:
				summary.Playing++;
				break;
			case Stage::Refused:
				summary.Refused++;
				break;
			case Stage::TimedOut:
				summary.TimedOut++;
				break;
			}

			// The time rather than the stage, because a session that was admitted
			// and then timed out was still admitted - and a run where that is
			// happening is exactly the one an operator is looking at.
			if (report.AdmitSeconds > 0.0) {
				summary.Admitted++;
				admitted.push_back(report.AdmitSeconds);
				admitTotal += report.AdmitSeconds;
			}
			if (report.JoinSeconds > 0.0) {
				joined.push_back(report.JoinSeconds);
				joinTotal += report.JoinSeconds;
			}

			summary.BytesSent += report.BytesSent;
			summary.BytesReceived += report.BytesReceived;
			summary.PacketsSent += report.PacketsSent;
			summary.PacketsReceived += report.PacketsReceived;
			summary.PacketsLost += report.PacketsLost;

			summary.InputsSent += report.InputsSent;
			summary.InputsRefused += report.InputsRefused;
			summary.Applied += report.Applied;
			summary.Deltas += report.Deltas;
			summary.Incomplete += report.Incomplete;
			summary.Refusals += report.Refusals;

			summary.ApplyTotalMilliseconds += report.ApplyMicroseconds / 1000.0;
			summary.Polls += report.Polls;

			summary.SmallestReplica =
				first ? report.Entities : std::min(summary.SmallestReplica, report.Entities);
			summary.LargestReplica = std::max(summary.LargestReplica, report.Entities);
			first = false;
		}

		if (!admitted.empty()) {
			summary.AdmitMeanSeconds = admitTotal / static_cast<double>(admitted.size());
			summary.AdmitP50Seconds = Percentile(admitted, 0.50);
			summary.AdmitP99Seconds = Percentile(admitted, 0.99);
		}
		if (!joined.empty()) {
			summary.JoinMeanSeconds = joinTotal / static_cast<double>(joined.size());
			summary.JoinP50Seconds = Percentile(joined, 0.50);
			summary.JoinP95Seconds = Percentile(joined, 0.95);
			summary.JoinP99Seconds = Percentile(joined, 0.99);
		}

		if (summary.Polls > 0) {
			summary.ApplyMeanMicroseconds =
				summary.ApplyTotalMilliseconds * 1000.0 / static_cast<double>(summary.Polls);
		}
		if (seconds > 0.0) {
			summary.SentBytesPerSecond = static_cast<double>(summary.BytesSent) / seconds;
			summary.ReceivedBytesPerSecond = static_cast<double>(summary.BytesReceived) / seconds;
		}

		return summary;
	}

	std::string Describe(const Summary &summary) {
		std::string text;
		text += "load test\n";

		Count(text, "sessions", summary.Sessions);
		Count(text, "admitted", summary.Admitted);
		Count(text, "playing at the end", summary.Playing);
		Count(text, "still streaming", summary.Streaming);
		Count(text, "never admitted", summary.Dialling);
		Count(text, "refused", summary.Refused);
		Count(text, "timed out", summary.TimedOut);

		Line(text, "run seconds", "%.2f", summary.Seconds);

		text += "\nadmission and join, seconds\n";
		Line(text, "admit mean", "%.3f", summary.AdmitMeanSeconds);
		Line(text, "admit p50", "%.3f", summary.AdmitP50Seconds);
		Line(text, "admit p99", "%.3f", summary.AdmitP99Seconds);
		Line(text, "join mean", "%.3f", summary.JoinMeanSeconds);
		Line(text, "join p50", "%.3f", summary.JoinP50Seconds);
		Line(text, "join p95", "%.3f", summary.JoinP95Seconds);
		Line(text, "join p99", "%.3f", summary.JoinP99Seconds);

		text += "\ntraffic, measured at the sockets\n";
		Count(text, "bytes sent", summary.BytesSent);
		Count(text, "bytes received", summary.BytesReceived);
		Line(text, "bytes sent per second", "%.0f", summary.SentBytesPerSecond);
		Line(text, "bytes received per second", "%.0f", summary.ReceivedBytesPerSecond);
		Count(text, "packets sent", summary.PacketsSent);
		Count(text, "packets received", summary.PacketsReceived);
		Count(text, "packets lost", summary.PacketsLost);

		text += "\nwhat the replicas did\n";
		Count(text, "inputs sent", summary.InputsSent);
		Count(text, "inputs refused", summary.InputsRefused);
		Count(text, "deltas applied", summary.Deltas);
		Count(text, "incomplete ticks", summary.Incomplete);
		Count(text, "datagrams refused", summary.Refusals);
		Count(text, "smallest replica", summary.SmallestReplica);
		Count(text, "largest replica", summary.LargestReplica);

		text += "\nclient-side cost\n";
		Count(text, "polls", summary.Polls);
		Line(text, "apply mean microseconds", "%.2f", summary.ApplyMeanMicroseconds);
		Line(text, "apply total ms", "%.1f", summary.ApplyTotalMilliseconds);

		return text;
	}
}

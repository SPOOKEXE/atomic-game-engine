#pragma once

#include <engine/imagegraph/FrameTime.hpp>

#include <array>
#include <ctime>
#include <string>

namespace studio::detail {
	// A seek uses the current captured session observation for every replay step. It does not invent
	// historical calendar readings. Repeated requests at the same revision/frame share the snapshot.
	struct ImageGraphObservations {
		uint64_t Revision = 0;
		engine::imagegraph::FrameTime Frame{};
		std::string Project;
		std::array<engine::imagegraph::AuthoredValue, 8> Values{};
		bool Captured = false;

		bool Matches(uint64_t revision, engine::imagegraph::FrameTime frame, std::string_view project) const {
			return Captured && Revision == revision && Frame == frame && Project == project;
		}

		void Capture(
			uint64_t revision,
			engine::imagegraph::FrameTime frame,
			std::string project,
			double elapsedSeconds,
			const std::tm &calendar
		) {
			Revision = revision;
			Frame = frame;
			Project = std::move(project);
			Values = {
				{{"Program.time", elapsedSeconds},
				 {"Device.timeSecond", double(calendar.tm_sec)},
				 {"Device.timeMinute", double(calendar.tm_min)},
				 {"Device.timeHour", double(calendar.tm_hour)},
				 {"Device.timeDay", double(calendar.tm_mday)},
				 {"Device.timeDayInWeek", double(calendar.tm_wday)},
				 {"Device.timeMonth", double(calendar.tm_mon + 1)},
				 {"Device.timeYear", double(calendar.tm_year + 1900)}}
			};
			Captured = true;
		}

		void Bind(engine::imagegraph::EvaluationRequest &request) const {
			request.PcxObservations = Values;
			request.ProjectName = Project;
		}
	};
}

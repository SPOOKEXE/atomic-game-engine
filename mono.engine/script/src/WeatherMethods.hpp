#pragma once

namespace engine::script {
	class ScriptCall;
	// Samples the accepted canonical atmosphere and returns an owned, physical-unit dictionary.
	void SampleWeather(ScriptCall &call);
}

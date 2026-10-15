#ifndef WEATHER_CLOCK_GLSL
#define WEATHER_CLOCK_GLSL

layout(set = 0, binding = WEATHER_CLOCK_BINDING, std430) readonly buffer WeatherClockState {
	uvec4 Phase;
	uvec4 Time;
} weatherClock;

WeatherField WeatherClockField(WeatherField field) {
	if (field.Phase.z == 1u) {
		field.Phase.xy = weatherClock.Phase.xy;
		field.Time = weatherClock.Time;
	}
	return field;
}

#endif

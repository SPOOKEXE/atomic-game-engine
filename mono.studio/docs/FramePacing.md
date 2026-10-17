# Studio frame pacing

Fresh Studio preferences default to uncapped presentation. The fixed simulation
rate remains 60 Hz. Existing saved pacing preferences keep their chosen values.

In Preferences, turn off **Uncapped presentation** to use the active, idle,
focused and unfocused rate limits. The lowest applicable limit sets the
presentation cadence. **Vertical sync** instead lets the display pace frames.

`--uncapped` overrides saved pacing for one run and locks those controls without
writing the override into preferences. Saved rate limits remain available when
uncapped presentation is turned off.

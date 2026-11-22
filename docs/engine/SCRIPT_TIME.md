# Time in scripts

A script can read three clocks. None of them reads the machine while a world
runs, so a recording replays and two runs from one snapshot agree.

| Call | Returns | Source |
|---|---|---|
| `time()`, `elapsedTime()` | Simulated seconds since the world started | `store.Time().Elapsed` |
| `tick()` | Completed ticks | `store.Time().Tick` |
| `os.time()`, `DateTime.now()`, `DateTime:GetUTCNow()` | Unix time | The world's anchor plus simulated time |
| `workspace:GetServerTimeNow()` | The server's Unix time, in seconds | The same, read from the server on a client |
| `os.clock()` | A real timer, in seconds | Sampled once per driver tick |
| `os.date(format, time)` | A formatted date or a `*t` table | The anchored clock |

## The anchored clock

The server reads the machine's Unix time once, when it creates a world, and
stores it in the `world.WallClock` resource with the local UTC offset. A date is
that anchor plus the world's simulated elapsed time. The anchor is in the world
snapshot, so a replay and a restarted world read the same dates.

The authority keeps a `world.ServerClock` resource: the Unix time of its tick
zero and its tick delta. It changes only when the tick rate changes, so it costs
no replication traffic on a quiet world. A client's replica notes the server
tick of each update it applies, with its own simulated time at that moment.
On a client, `os.time()`, `DateTime.now()` and `workspace:GetServerTimeNow()`
are the server's time at that tick plus the client's own simulated time since.
So a client and its server name the same second, and the client's value keeps
moving when no update arrives. A client keeps its own UTC offset, so local time
on a client is the client's time zone.

## `os.clock()`

The universe reads a real timer once per driver tick and gives the value to
every world before any world ticks. Every script in one tick reads the same
`os.clock()`, so two scripts that compare it agree. A recording keeps the
sample of each barrier, and a replay gives the recorded value back.

Use the difference between two `os.clock()` reads from different ticks to
measure time. Two reads in the same tick are equal.

A Studio plugin is the exception: it is not recorded, so its `os.clock()` reads
a monotonic clock on every call. See [STUDIO_PLUGINS.md](STUDIO_PLUGINS.md).

## `DateTime`

`DateTime` matches Roblox:

- Constructors: `now`, `fromUnixTimestamp`, `fromUnixTimestampMillis`,
  `fromUniversalTime`, `fromLocalTime`, `fromIsoDate` (`nil` on bad text).
- Members: `UnixTimestamp`, `UnixTimestampMillis`, `ToUniversalTime`,
  `ToLocalTime`, `ToIsoDate`, `FormatUniversalTime`, `FormatLocalTime`.
- `==`, `<` and `<=` compare two values. `typeof` is `"DateTime"`.
- The range is the years 1400 through 9999. A value outside it raises.

Differences from Roblox:

- `DateTime:GetUTCNow()` is extra. It returns the same value as `now`.
- `DateTime.fromSimulated()` is extra. It returns the world's simulated seconds
  as a date in 1970.
- The format functions use en-us names whatever locale is given.
- In JavaScript, `Equals` compares two values and `valueOf` returns the
  milliseconds, so `<` and `>` work.

## `os`

`os.time`, `os.date`, `os.clock` and `os.difftime` follow Luau. `os.time(t)`
reads a date table as UTC, as Luau does. `os.date` uses local time unless the
format starts with `!`. The `strftime` conversions are the ones Luau accepts,
with en-us names.

## Pinning the clock

`server --wall-clock <unix>` pins every world's anchor to that second with a UTC
offset of zero. `os.clock()` then reads the sum of the frame times the server
was given. `just determinism` and `just replay-check` pass `--wall-clock 0`, so
two separate runs write identical recordings. In C++ the setting is
`world::UniverseSettings::WallClockEpoch`.

## Where the clock is different

- A Studio plugin reads the machine's real time once per Studio frame. See
  [STUDIO_PLUGINS.md](STUDIO_PLUGINS.md).
- A data package has no `os`, and its `DateTime.now()` raises. Its output must
  depend on its seed and parameters only.
- The local UTC offset is read once with the anchor. A world that runs across a
  daylight saving change keeps the old offset.

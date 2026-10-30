# Pass 2 decisions

- CPU oracle pairs used verified `9de02761` controls. Query, catalogue,
  procedural and delivery outputs matched before the final weather guard;
  weather and representative cadence were rechecked afterward. Fixture drift
  limits graph comparison. Shared-host timing and GPU checks remain open.
- Build turnaround: remove the script runtime include from the game header.
  Valid Ninja dependency records fell from 266 to 254; wall time was not measured.
  Removing it from Studio headers reduced the same count from 254 to 199.
  Forward-declaring TeamCreate reduced its recorded dependent objects from 71 to 6.
  Splitting compute bridge declarations reduced recorded dependent objects from 429 to 72.
  Source checker: scan backward only at record keywords. Three repository pairs
  matched diagnostics and exit status, as did all 13 fixtures. On the active
  host, control runs took 49.3 to 52.9 seconds and candidate runs 5.4 to 5.6.
- Procedural baked scene: reject moving placement names into temporary geometry.
  Three heap comparisons against `6bd15f10` showed negligible savings with
  unchanged output and cache identities. Investigate the larger geometry buffers.
  Latency was not compared.
- Procedural entrances: build posts in the destination geometry. Three CPU heap
  pairs matched fixture and canonical output hashes and retained-byte deltas.
  Complex serial publication saved 363,397,120 tracked bytes and 983,040
  allocations over 896 operations; cook saved 72,679,424 bytes and 196,608
  allocations over 128 operations. Shared-host timings remain unqualified.
- Delivery cache: reject unbuffered whole-file reads. Three CPU comparisons
  preserved outputs and reduced allocation churn, but the complex delivery path
  was repeatedly slower. Three retained-control pairs matched cold and warm
  cache identities and work. Three verified `9de02761` pairs also matched
  hashes and work before the later duplicate-read fix. Three final pairs matched
  hashes; cold bundle reads fell from two to one. Measurements used a shared host.
- Compute completion: accept validated ownership transfer for lower C++
  allocation churn. Historical native and VM oracle pairs passed; latency was
  mixed and VM residency differed. Combined GPU matrix validation remains open.
- Single-chunk hash roots: skip building a proof tree. Three CPU delivery
  pairs matched; complex cache allocation churn fell. Timings were mixed.
- Procedural baked scene: accept borrowed mesh views for shared packing. They
  reduce temporary allocations and peak tracked payload while retaining output,
  admission limits, and rollback behavior. Delivery and live timings were mixed
  on a shared host, with some live medians slower. No latency gain is claimed.
  Three verified `9de02761` pairs matched all fixtures and canonical hashes. The
  procedural scene suite also passed under ASan and UBSan.
- Cadence animation: accept call-local track-vector reuse. Native heap runs
  reduced allocation operations without retaining the buffer across calls.
  Native and town work matched; timings were mixed on a shared host. Three
  full CPU correctness pairs matched `9de02761` across all 19 native
  and town fixtures, including promotions, after the weather guard. Three heap
  pairs also matched work at `50a663e3`; JS maximum-batch VM live bytes varied
  by -64 to +112. Timing remains open.
  Busy native full and authored heap pairs also matched after the weather guard.
- Studio viewport queries: three `9de02761` CPU pairs matched all 34 work rows
  and internal oracles. Shared-host timings are not accepted as performance proof.
- Character, portal, mixed-shape and weather ground queries: three `9de02761` CPU
  pairs matched all work rows. Shared-host timings remain unqualified.
- Weather broadphase: skip budget-only counts when no budget is active. Three
  heap pairs matched work; the guard removed 46,056 bytes and 808 allocations
  from 101 impact preparations. The remaining 129,280 bytes and 101 allocations
  are resource-table growth when installing `WeatherGroundQuery`.
- Portal crossing: omit unused seam paths. Three CPU oracle pairs matched;
  steady query allocation fell to zero in all five fixtures.
- Engine graph: three pairs against the `9de02761` executor passed all 12
  current fixture cohorts with common dependencies. Three heap pairs preserved
  retained bytes; complex prepared execution fell from 63 allocations and
  1,008 bytes to one allocation and 16 bytes. Three `9de02761` catalogue pairs
  matched startup and warm oracles. Shared-host timing remains unqualified.
- Catalogue registration: retain canonical built-ins. Three CPU oracle pairs
  matched; the warm heap probe fell from 2,215 allocations to zero. Process live
  bytes were 21,304 higher in the comparison.
- Schedule compilation: reuse writer storage within a call. Three CPU oracle
  pairs matched; PBR and stress allocation counts fell. Timings were mixed.
- Pipeline admission: replay documents before the existing compiler validation.
  Three CPU oracle pairs matched; warm PBR and stress allocations fell.
- Graph validation: use bounded node-name storage. Three CPU oracle pairs
  matched; PBR and stress allocations fell, with mixed shared-host timings.
- CPU image evaluation: three pairs against the `9de02761` evaluator passed all
  five full-pixel cohorts using the current common harness. Three heap pairs
  matched output and retained bytes; busy and complex saved one allocation and
  256 KiB and 4 MiB of tracked payload per call, respectively. Shared-host
  timings remain unqualified. Image graph and bake suites passed ASan and UBSan.
- CPU audio and domain compilers: three control pairs passed 11 sample/trace
  cohorts and 23 compiler cohorts. Controls were rebuilt from `9de02761`.

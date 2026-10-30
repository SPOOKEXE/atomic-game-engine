# Pass 2 decisions

- Build turnaround: remove the script runtime include from the game header.
  Valid Ninja dependency records fell from 266 to 254; wall time was not measured.
- Procedural baked scene: reject moving placement names into temporary geometry.
  Three heap comparisons against `6bd15f10` showed negligible savings with
  unchanged output and cache identities. Investigate the larger geometry buffers.
  Latency was not compared.
- Delivery cache: reject unbuffered whole-file reads. Three CPU comparisons
  preserved outputs and reduced allocation churn, but the complex delivery path
  was repeatedly slower. Three retained-control pairs matched cold and warm
  cache identities and work. Three verified `9de02761` pairs also matched
  hashes and work. Measurements used a shared host.
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
  and town fixtures, including promotions. The timing matrix remains open.
- Studio viewport queries: three `9de02761` CPU pairs matched all 34 work rows
  and internal oracles. Shared-host timings are not accepted as performance proof.
- Character, portal, mixed-shape and weather ground queries: three `9de02761` CPU
  pairs matched all work rows. Shared-host timings remain unqualified.
- Portal crossing: omit unused seam paths. Three CPU oracle pairs matched;
  steady query allocation fell to zero in all five fixtures.
- Engine graph: three retained-control pairs passed all 12 fixture cohorts.
  Three `9de02761` pairs matched catalogue startup and warm oracles; graph
  fixture changes prevent a direct full-matrix timing comparison.
- Catalogue registration: retain canonical built-ins. Three CPU oracle pairs
  matched; the warm heap probe fell from 2,215 allocations to zero.
- Schedule compilation: reuse writer storage within a call. Three CPU oracle
  pairs matched; PBR and stress allocation counts fell. Timings were mixed.
- Pipeline admission: replay documents before the existing compiler validation.
  Three CPU oracle pairs matched; warm PBR and stress allocations fell.
- Graph validation: use bounded node-name storage. Three CPU oracle pairs
  matched; PBR and stress allocations fell, with mixed shared-host timings.
- CPU image evaluation: three control pairs passed all five full-pixel cohorts.
  Busy and complex medians were lower; shared-host timings remain unqualified.
  Image graph and bake suites passed under ASan and UBSan.
- CPU audio and domain compilers: three control pairs passed 11 sample/trace
  cohorts and 23 compiler cohorts. Controls were rebuilt from `9de02761`.

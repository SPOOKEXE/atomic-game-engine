# Version performance impact

Memory byte rows describe tracked payload. CPU allocation overhead is included separately. Render, physics, replication, script and imagegraph timings are CPU self milliseconds per frame; Wall timing rows sum outermost inclusive owner-thread scopes for each subsystem, including nested waits. replication includes Network category work. GPU memory is logical payload, not driver residency. Timings are gated per iteration and per wall-clock second. Allocation churn is gated per wall-clock second. Per-iteration allocation values, raw totals and throughput are diagnostic. External peak process RSS covers the whole process lifetime, including startup and shutdown. cpu_peak_bytes is sampled at frame boundaries; cpu_process_peak_bytes and GPU peaks cover process lifetime. CPU timings cover the owning thread only; background workers are excluded. Off-thread rejected spans are visible as diagnostics. Owner-thread drops invalidate measurements. ImageGraph diagnostic frames include pending GPU captures and errors; published texture counts verify active graph output.

Run `just demo-bench` to collect three five-second samples per available demo. Run `just demo-impact` to compare with this reusable baseline, `just demo-impact-report <collection-directory>` to refresh this document from saved samples, and `just regression-check` for the regression process. New workloads appear as Added because an older revision has no measurement to compare.

### v0.24.1

Source: `4b41df33893e0ea2b903f9e8b8cf7c009f245f58` (clean). Source diff SHA-256: `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`. Three independent five-second runs.

```json
{
  "compiler": "c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0\nCopyright (C) 2023 Free Software Foundation, Inc.\nThis is free software; see the source for copying conditions.  There is NO\nwarranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.",
  "cpu": "AMD Ryzen 9 9900X 12-Core Processor",
  "fingerprint": "5675a8d8002198eeac91db34876ea236c284ac3be38542a446379a7671fa438a",
  "gpu": "NVIDIA GeForce RTX 4090, 580.173.02",
  "os": "Linux 7.0.0-38-generic #38~24.04.4-Ubuntu SMP PREEMPT_DYNAMIC Mon Sep 14 16:37:11 UTC 2 x86_64 GNU/Linux"
}
{
  "compute": "serial",
  "height": 540,
  "max_fps": 0,
  "preset": "profile",
  "replica_viewer_max_fps": 60,
  "runs": 3,
  "seconds": 5,
  "width": 960
}
```

| Demo | Metric | Mean | Population stddev | Minimum | Maximum |
|---|---|---:|---:|---:|---:|
| Cube | cpu_allocated_bytes | 2151880285.000 | 105398013.894 | 2002903222.000 | 2230550685.000 |
| Cube | cpu_allocated_bytes_per_frame | 7283550.954 | 84549.612 | 7178864.595 | 7385929.421 |
| Cube | cpu_allocated_bytes_per_second | 426334371.656 | 19618611.935 | 398589468.945 | 440220297.833 |
| Cube | cpu_allocations | 843250.667 | 33352.386 | 796397.000 | 871381.000 |
| Cube | cpu_allocations_per_frame | 2855.327 | 24.185 | 2826.144 | 2885.368 |
| Cube | cpu_allocations_per_second | 167070.506 | 6089.612 | 158487.666 | 171975.291 |
| Cube | cpu_live_blocks | 47668.000 | 1.633 | 47666.000 | 47670.000 |
| Cube | cpu_live_bytes | 140577582.000 | 527368.367 | 139836114.000 | 141017922.000 |
| Cube | cpu_peak_bytes | 140577582.000 | 527368.367 | 139836114.000 | 141017922.000 |
| Cube | cpu_process_peak_bytes | 146874520.667 | 714228.484 | 145871046.000 | 147476070.000 |
| Cube | cpu_profiler_overhead_bytes | 1525376.000 | 52.256 | 1525312.000 | 1525440.000 |
| Cube | draw_calls_per_frame | 16.000 | 0.000 | 16.000 | 16.000 |
| Cube | duration_seconds | 5.047 | 0.017 | 5.025 | 5.067 |
| Cube | frame_count | 295.333 | 11.614 | 279.000 | 305.000 |
| Cube | frame_ms_per_frame | 17.062 | 0.638 | 16.503 | 17.956 |
| Cube | frame_ms_per_second | 997.008 | 0.059 | 996.947 | 997.087 |
| Cube | framegraph_dropped_spans | 472.333 | 1.700 | 470.000 | 474.000 |
| Cube | framegraph_off_thread_dropped_spans | 472.333 | 1.700 | 470.000 | 474.000 |
| Cube | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | gpu_allocated_bytes | 108442688.000 | 0.000 | 108442688.000 | 108442688.000 |
| Cube | gpu_allocated_bytes_per_frame | 367771.672 | 14857.011 | 355549.797 | 388683.470 |
| Cube | gpu_allocated_bytes_per_second | 21488116.536 | 73041.343 | 21402191.275 | 21580729.886 |
| Cube | gpu_live_bytes | 103872768.000 | 0.000 | 103872768.000 | 103872768.000 |
| Cube | gpu_peak_bytes | 103872768.000 | 0.000 | 103872768.000 | 103872768.000 |
| Cube | gpu_resources_created | 45.000 | 0.000 | 45.000 | 45.000 |
| Cube | gpu_resources_created_per_frame | 0.153 | 0.006 | 0.148 | 0.161 |
| Cube | gpu_resources_created_per_second | 8.917 | 0.030 | 8.881 | 8.955 |
| Cube | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | physics_ms_per_frame | 11.309 | 0.095 | 11.241 | 11.443 |
| Cube | physics_ms_per_second | 661.819 | 26.558 | 624.298 | 682.026 |
| Cube | physics_wall_ms_per_frame | 11.309 | 0.095 | 11.241 | 11.443 |
| Cube | physics_wall_ms_per_second | 661.819 | 26.558 | 624.298 | 682.026 |
| Cube | process_peak_rss_bytes | 321452714.667 | 2251874.693 | 318312448.000 | 323481600.000 |
| Cube | render_ms_per_frame | 3.143 | 0.121 | 3.049 | 3.314 |
| Cube | render_ms_per_second | 183.671 | 0.623 | 182.796 | 184.199 |
| Cube | render_wall_ms_per_frame | 3.152 | 0.122 | 3.057 | 3.324 |
| Cube | render_wall_ms_per_second | 184.148 | 0.647 | 183.236 | 184.674 |
| Cube | replication_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| Cube | replication_ms_per_second | 0.066 | 0.003 | 0.063 | 0.070 |
| Cube | replication_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| Cube | replication_wall_ms_per_second | 0.066 | 0.003 | 0.063 | 0.070 |
| Cube | script_ms_per_frame | 0.009 | 0.001 | 0.008 | 0.010 |
| Cube | script_ms_per_second | 0.541 | 0.031 | 0.506 | 0.581 |
| Cube | submitted_frames | 295.333 | 11.614 | 279.000 | 305.000 |
| Cube | uploaded_bytes | 237329593.333 | 9317003.537 | 224223988.000 | 245062296.000 |
| Cube | uploaded_bytes_per_frame | 803601.265 | 84.050 | 803482.938 | 803670.208 |
| Cube | uploaded_bytes_per_second | 47021983.131 | 1717417.989 | 44621886.529 | 48544427.231 |
| ImageGraph3D | Unavailable: unavailable on this source revision | | | | |
| ImageGraphFeedback | Unavailable: unavailable on this source revision | | | | |
| ImageGraphFlipbook | Unavailable: unavailable on this source revision | | | | |
| ImageGraphGui | Unavailable: unavailable on this source revision | | | | |
| ImageGraphMaterials | Unavailable: unavailable on this source revision | | | | |
| ImageGraphSkybox | Unavailable: unavailable on this source revision | | | | |
| ImageGraphVerlet | Unavailable: unavailable on this source revision | | | | |
| Interface | cpu_allocated_bytes | 3241679910.667 | 110708419.551 | 3085794266.000 | 3332243374.000 |
| Interface | cpu_allocated_bytes_per_frame | 124989.822 | 62.469 | 124938.824 | 125077.794 |
| Interface | cpu_allocated_bytes_per_second | 648321176.881 | 22142989.853 | 617143624.687 | 666447309.649 |
| Interface | cpu_allocations | 9234021.000 | 317839.164 | 8786439.000 | 9493664.000 |
| Interface | cpu_allocations_per_frame | 356.034 | 0.081 | 355.955 | 356.144 |
| Interface | cpu_allocations_per_second | 1846762.027 | 63571.511 | 1757244.439 | 1898728.911 |
| Interface | cpu_live_blocks | 16710.000 | 0.816 | 16709.000 | 16711.000 |
| Interface | cpu_live_bytes | 32525052.333 | 213.700 | 32524863.000 | 32525351.000 |
| Interface | cpu_peak_bytes | 32525068.333 | 202.526 | 32524887.000 | 32525351.000 |
| Interface | cpu_process_peak_bytes | 32655988.333 | 37.712 | 32655935.000 | 32656015.000 |
| Interface | cpu_profiler_overhead_bytes | 534720.000 | 26.128 | 534688.000 | 534752.000 |
| Interface | draw_calls_per_frame | 15.000 | 0.000 | 15.000 | 15.000 |
| Interface | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Interface | frame_count | 25936.000 | 898.397 | 24671.000 | 26671.000 |
| Interface | frame_ms_per_frame | 0.186 | 0.006 | 0.180 | 0.195 |
| Interface | frame_ms_per_second | 961.469 | 0.888 | 960.227 | 962.246 |
| Interface | framegraph_dropped_spans | 456.000 | 2.828 | 454.000 | 460.000 |
| Interface | framegraph_off_thread_dropped_spans | 456.000 | 2.828 | 454.000 | 460.000 |
| Interface | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | gpu_allocated_bytes | 88035376.000 | 0.000 | 88035376.000 | 88035376.000 |
| Interface | gpu_allocated_bytes_per_frame | 3398.508 | 120.567 | 3300.790 | 3568.375 |
| Interface | gpu_allocated_bytes_per_second | 17606671.476 | 288.465 | 17606334.551 | 17607039.134 |
| Interface | gpu_live_bytes | 83596528.000 | 0.000 | 83596528.000 | 83596528.000 |
| Interface | gpu_peak_bytes | 83596528.000 | 0.000 | 83596528.000 | 83596528.000 |
| Interface | gpu_resources_created | 33.000 | 0.000 | 33.000 | 33.000 |
| Interface | gpu_resources_created_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| Interface | gpu_resources_created_per_second | 6.600 | 0.000 | 6.600 | 6.600 |
| Interface | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | physics_ms_per_second | 1.563 | 0.070 | 1.500 | 1.661 |
| Interface | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | physics_wall_ms_per_second | 1.563 | 0.070 | 1.500 | 1.661 |
| Interface | process_peak_rss_bytes | 211376810.667 | 2391480.233 | 209428480.000 | 214745088.000 |
| Interface | render_ms_per_frame | 0.107 | 0.003 | 0.104 | 0.111 |
| Interface | render_ms_per_second | 553.349 | 2.991 | 549.158 | 555.940 |
| Interface | render_wall_ms_per_frame | 0.143 | 0.004 | 0.140 | 0.148 |
| Interface | render_wall_ms_per_second | 742.986 | 7.335 | 732.614 | 748.293 |
| Interface | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | replication_ms_per_second | 1.323 | 0.005 | 1.319 | 1.329 |
| Interface | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | replication_wall_ms_per_second | 1.323 | 0.005 | 1.319 | 1.329 |
| Interface | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | script_ms_per_second | 1.181 | 0.056 | 1.123 | 1.257 |
| Interface | submitted_frames | 25936.000 | 898.397 | 24671.000 | 26671.000 |
| Interface | uploaded_bytes | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | uploaded_bytes_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | uploaded_bytes_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | cpu_allocated_bytes | 931392720.333 | 26310722.451 | 895267733.000 | 957176622.000 |
| Meshes | cpu_allocated_bytes_per_frame | 31170.465 | 48.472 | 31124.658 | 31237.534 |
| Meshes | cpu_allocated_bytes_per_second | 186275617.604 | 5262635.835 | 179049742.652 | 191432268.873 |
| Meshes | cpu_allocations | 6641629.333 | 193260.275 | 6376606.000 | 6831988.000 |
| Meshes | cpu_allocations_per_frame | 222.266 | 0.159 | 222.151 | 222.491 |
| Meshes | cpu_allocations_per_second | 1328305.001 | 38655.516 | 1275294.106 | 1366375.791 |
| Meshes | cpu_live_blocks | 16390.000 | 0.000 | 16390.000 | 16390.000 |
| Meshes | cpu_live_bytes | 32508777.000 | 0.000 | 32508777.000 | 32508777.000 |
| Meshes | cpu_peak_bytes | 32508777.000 | 0.000 | 32508777.000 | 32508777.000 |
| Meshes | cpu_process_peak_bytes | 32639601.667 | 482.718 | 32638919.000 | 32639943.000 |
| Meshes | cpu_profiler_overhead_bytes | 524480.000 | 0.000 | 524480.000 | 524480.000 |
| Meshes | draw_calls_per_frame | 37.093 | 0.003 | 37.090 | 37.097 |
| Meshes | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Meshes | frame_count | 29882.000 | 889.780 | 28660.000 | 30753.000 |
| Meshes | frame_ms_per_frame | 0.159 | 0.005 | 0.155 | 0.166 |
| Meshes | frame_ms_per_second | 951.146 | 1.185 | 949.853 | 952.716 |
| Meshes | framegraph_dropped_spans | 450.667 | 2.055 | 448.000 | 453.000 |
| Meshes | framegraph_off_thread_dropped_spans | 450.667 | 2.055 | 448.000 | 453.000 |
| Meshes | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | gpu_allocated_bytes | 119985024.000 | 0.000 | 119985024.000 | 119985024.000 |
| Meshes | gpu_allocated_bytes_per_frame | 4018.915 | 121.624 | 3901.571 | 4186.498 |
| Meshes | gpu_allocated_bytes_per_second | 23996625.652 | 108.301 | 23996494.990 | 23996760.187 |
| Meshes | gpu_live_bytes | 115546176.000 | 0.000 | 115546176.000 | 115546176.000 |
| Meshes | gpu_peak_bytes | 115546176.000 | 0.000 | 115546176.000 | 115546176.000 |
| Meshes | gpu_resources_created | 51.000 | 0.000 | 51.000 | 51.000 |
| Meshes | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Meshes | gpu_resources_created_per_second | 10.200 | 0.000 | 10.200 | 10.200 |
| Meshes | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | physics_ms_per_second | 1.773 | 0.188 | 1.555 | 2.014 |
| Meshes | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | physics_wall_ms_per_second | 1.773 | 0.188 | 1.555 | 2.014 |
| Meshes | process_peak_rss_bytes | 237214378.667 | 12536948.198 | 219619328.000 | 247902208.000 |
| Meshes | render_ms_per_frame | 0.130 | 0.004 | 0.126 | 0.135 |
| Meshes | render_ms_per_second | 775.475 | 1.750 | 773.769 | 777.881 |
| Meshes | render_wall_ms_per_frame | 0.132 | 0.004 | 0.128 | 0.138 |
| Meshes | render_wall_ms_per_second | 789.890 | 1.692 | 788.158 | 792.186 |
| Meshes | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | replication_ms_per_second | 1.593 | 0.031 | 1.549 | 1.618 |
| Meshes | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | replication_wall_ms_per_second | 1.593 | 0.031 | 1.549 | 1.618 |
| Meshes | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | script_ms_per_second | 1.635 | 0.162 | 1.450 | 1.845 |
| Meshes | submitted_frames | 29882.000 | 889.780 | 28660.000 | 30753.000 |
| Meshes | uploaded_bytes | 1324.000 | 0.000 | 1324.000 | 1324.000 |
| Meshes | uploaded_bytes_per_frame | 0.044 | 0.001 | 0.043 | 0.046 |
| Meshes | uploaded_bytes_per_second | 264.796 | 0.001 | 264.794 | 264.797 |
| Particles | cpu_allocated_bytes | 20832077209.000 | 1337277046.392 | 19469472493.000 | 22649137525.000 |
| Particles | cpu_allocated_bytes_per_frame | 859915.448 | 150.389 | 859712.945 | 860073.000 |
| Particles | cpu_allocated_bytes_per_second | 4166321991.052 | 267405358.578 | 3893860674.190 | 4529669495.633 |
| Particles | cpu_allocations | 5889143.000 | 375027.364 | 5506972.000 | 6398705.000 |
| Particles | cpu_allocations_per_frame | 243.102 | 0.164 | 242.881 | 243.273 |
| Particles | cpu_allocations_per_second | 1177802.188 | 74991.339 | 1101384.833 | 1279696.360 |
| Particles | cpu_live_blocks | 16898.000 | 0.000 | 16898.000 | 16898.000 |
| Particles | cpu_live_bytes | 40585337.000 | 0.000 | 40585337.000 | 40585337.000 |
| Particles | cpu_peak_bytes | 40585337.000 | 0.000 | 40585337.000 | 40585337.000 |
| Particles | cpu_process_peak_bytes | 40716509.000 | 0.000 | 40716509.000 | 40716509.000 |
| Particles | cpu_profiler_overhead_bytes | 540736.000 | 0.000 | 540736.000 | 540736.000 |
| Particles | draw_calls_per_frame | 26.000 | 0.000 | 26.000 | 26.000 |
| Particles | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Particles | frame_count | 24226.000 | 1559.485 | 22637.000 | 26345.000 |
| Particles | frame_ms_per_frame | 0.199 | 0.012 | 0.182 | 0.212 |
| Particles | frame_ms_per_second | 958.936 | 1.093 | 958.104 | 960.480 |
| Particles | framegraph_dropped_spans | 449.000 | 2.160 | 447.000 | 452.000 |
| Particles | framegraph_off_thread_dropped_spans | 449.000 | 2.160 | 447.000 | 452.000 |
| Particles | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | gpu_allocated_bytes | 109632576.000 | 0.000 | 109632576.000 | 109632576.000 |
| Particles | gpu_allocated_bytes_per_frame | 4543.706 | 284.393 | 4161.419 | 4843.070 |
| Particles | gpu_allocated_bytes_per_second | 21926038.020 | 234.489 | 21925750.360 | 21926324.735 |
| Particles | gpu_live_bytes | 105193728.000 | 0.000 | 105193728.000 | 105193728.000 |
| Particles | gpu_peak_bytes | 105193728.000 | 0.000 | 105193728.000 | 105193728.000 |
| Particles | gpu_resources_created | 60.000 | 0.000 | 60.000 | 60.000 |
| Particles | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.003 |
| Particles | gpu_resources_created_per_second | 12.000 | 0.000 | 12.000 | 12.000 |
| Particles | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | physics_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| Particles | physics_ms_per_second | 6.151 | 0.463 | 5.570 | 6.704 |
| Particles | physics_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| Particles | physics_wall_ms_per_second | 6.151 | 0.463 | 5.570 | 6.704 |
| Particles | process_peak_rss_bytes | 232947712.000 | 3025300.165 | 229810176.000 | 237035520.000 |
| Particles | render_ms_per_frame | 0.159 | 0.010 | 0.145 | 0.170 |
| Particles | render_ms_per_second | 767.616 | 2.041 | 764.730 | 769.078 |
| Particles | render_wall_ms_per_frame | 0.163 | 0.010 | 0.149 | 0.174 |
| Particles | render_wall_ms_per_second | 785.425 | 1.337 | 783.553 | 786.588 |
| Particles | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | replication_ms_per_second | 1.393 | 0.052 | 1.329 | 1.455 |
| Particles | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | replication_wall_ms_per_second | 1.393 | 0.052 | 1.329 | 1.455 |
| Particles | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | script_ms_per_second | 1.818 | 0.213 | 1.535 | 2.049 |
| Particles | submitted_frames | 24226.000 | 1559.485 | 22637.000 | 26345.000 |
| Particles | uploaded_bytes | 64009912.000 | 4142453.935 | 59764104.000 | 69628488.000 |
| Particles | uploaded_bytes_per_frame | 2642.140 | 1.447 | 2640.107 | 2643.364 |
| Particles | uploaded_bytes_per_second | 12801695.175 | 828336.700 | 11952716.972 | 13925211.844 |
| RenderFeatures | cpu_allocated_bytes | 658872302.333 | 9916473.004 | 648222885.000 | 672099358.000 |
| RenderFeatures | cpu_allocated_bytes_per_frame | 24749.958 | 29.713 | 24709.535 | 24780.110 |
| RenderFeatures | cpu_allocated_bytes_per_second | 131770350.089 | 1983273.224 | 129640280.954 | 134415626.136 |
| RenderFeatures | cpu_allocations | 6838264.667 | 107924.814 | 6720894.000 | 6981447.000 |
| RenderFeatures | cpu_allocations_per_frame | 256.870 | 0.146 | 256.671 | 257.016 |
| RenderFeatures | cpu_allocations_per_second | 1367610.274 | 21584.743 | 1344134.258 | 1396245.300 |
| RenderFeatures | cpu_live_blocks | 16575.667 | 0.471 | 16575.000 | 16576.000 |
| RenderFeatures | cpu_live_bytes | 32501921.667 | 104.171 | 32501775.000 | 32502007.000 |
| RenderFeatures | cpu_peak_bytes | 32501929.667 | 109.366 | 32501775.000 | 32502007.000 |
| RenderFeatures | cpu_process_peak_bytes | 32633129.667 | 109.366 | 32632975.000 | 32633207.000 |
| RenderFeatures | cpu_profiler_overhead_bytes | 530421.333 | 15.085 | 530400.000 | 530432.000 |
| RenderFeatures | draw_calls_per_frame | 26.000 | 0.000 | 26.000 | 26.000 |
| RenderFeatures | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| RenderFeatures | frame_count | 26621.667 | 432.785 | 26159.000 | 27200.000 |
| RenderFeatures | frame_ms_per_frame | 0.179 | 0.003 | 0.176 | 0.182 |
| RenderFeatures | frame_ms_per_second | 954.588 | 0.705 | 953.776 | 955.496 |
| RenderFeatures | framegraph_dropped_spans | 442.333 | 6.128 | 435.000 | 450.000 |
| RenderFeatures | framegraph_off_thread_dropped_spans | 442.333 | 6.128 | 435.000 | 450.000 |
| RenderFeatures | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | gpu_allocated_bytes | 108986176.000 | 0.000 | 108986176.000 | 108986176.000 |
| RenderFeatures | gpu_allocated_bytes_per_frame | 4094.966 | 66.170 | 4006.845 | 4166.297 |
| RenderFeatures | gpu_allocated_bytes_per_second | 21796555.184 | 38.423 | 21796512.903 | 21796605.884 |
| RenderFeatures | gpu_live_bytes | 104547328.000 | 0.000 | 104547328.000 | 104547328.000 |
| RenderFeatures | gpu_peak_bytes | 104547328.000 | 0.000 | 104547328.000 | 104547328.000 |
| RenderFeatures | gpu_resources_created | 51.000 | 0.000 | 51.000 | 51.000 |
| RenderFeatures | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| RenderFeatures | gpu_resources_created_per_second | 10.200 | 0.000 | 10.200 | 10.200 |
| RenderFeatures | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | physics_ms_per_second | 1.645 | 0.292 | 1.416 | 2.057 |
| RenderFeatures | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | physics_wall_ms_per_second | 1.645 | 0.292 | 1.416 | 2.057 |
| RenderFeatures | process_peak_rss_bytes | 328052736.000 | 25156097.009 | 304476160.000 | 362913792.000 |
| RenderFeatures | render_ms_per_frame | 0.151 | 0.002 | 0.149 | 0.153 |
| RenderFeatures | render_ms_per_second | 801.193 | 6.811 | 792.604 | 809.262 |
| RenderFeatures | render_wall_ms_per_frame | 0.152 | 0.002 | 0.150 | 0.155 |
| RenderFeatures | render_wall_ms_per_second | 810.526 | 6.561 | 802.256 | 818.305 |
| RenderFeatures | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | replication_ms_per_second | 1.491 | 0.074 | 1.426 | 1.595 |
| RenderFeatures | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | replication_wall_ms_per_second | 1.491 | 0.074 | 1.426 | 1.595 |
| RenderFeatures | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | script_ms_per_second | 0.761 | 0.135 | 0.645 | 0.949 |
| RenderFeatures | submitted_frames | 26621.667 | 432.785 | 26159.000 | 27200.000 |
| RenderFeatures | uploaded_bytes | 41104205.333 | 668220.098 | 40389848.000 | 41997152.000 |
| RenderFeatures | uploaded_bytes_per_frame | 1544.013 | 0.000 | 1544.013 | 1544.013 |
| RenderFeatures | uploaded_bytes_per_second | 8220584.640 | 133642.294 | 8077701.919 | 8399165.116 |
| ReplicationRings | cpu_allocated_bytes | 1917331132.333 | 98054706.465 | 1782053479.000 | 2011371769.000 |
| ReplicationRings | cpu_allocated_bytes_per_frame | 247292.612 | 231.352 | 247066.917 | 247610.599 |
| ReplicationRings | cpu_allocated_bytes_per_second | 383433411.297 | 19613492.655 | 356370856.247 | 402232252.472 |
| ReplicationRings | cpu_allocations | 2567178.333 | 119778.692 | 2400609.000 | 2677135.000 |
| ReplicationRings | cpu_allocations_per_frame | 331.188 | 1.923 | 328.846 | 333.557 |
| ReplicationRings | cpu_allocations_per_second | 513391.728 | 23959.990 | 480068.132 | 535370.963 |
| ReplicationRings | cpu_live_blocks | 29101.000 | 0.816 | 29100.000 | 29102.000 |
| ReplicationRings | cpu_live_bytes | 38339640.000 | 87.132 | 38339518.000 | 38339716.000 |
| ReplicationRings | cpu_peak_bytes | 38341197.667 | 1771.068 | 38339910.000 | 38343702.000 |
| ReplicationRings | cpu_process_peak_bytes | 43133419.333 | 37.712 | 43133366.000 | 43133446.000 |
| ReplicationRings | cpu_profiler_overhead_bytes | 931232.000 | 26.128 | 931200.000 | 931264.000 |
| ReplicationRings | draw_calls_per_frame | 27.000 | 0.000 | 27.000 | 27.000 |
| ReplicationRings | duration_seconds | 5.000 | 0.000 | 5.000 | 5.001 |
| ReplicationRings | frame_count | 7753.667 | 403.559 | 7197.000 | 8141.000 |
| ReplicationRings | frame_ms_per_frame | 0.622 | 0.032 | 0.591 | 0.666 |
| ReplicationRings | frame_ms_per_second | 961.680 | 1.906 | 959.137 | 963.725 |
| ReplicationRings | framegraph_dropped_spans | 452.000 | 3.559 | 449.000 | 457.000 |
| ReplicationRings | framegraph_off_thread_dropped_spans | 452.000 | 3.559 | 449.000 | 457.000 |
| ReplicationRings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | gpu_allocated_bytes | 15796736.000 | 0.000 | 15796736.000 | 15796736.000 |
| ReplicationRings | gpu_allocated_bytes_per_frame | 2043.027 | 109.584 | 1940.393 | 2194.906 |
| ReplicationRings | gpu_allocated_bytes_per_second | 3159074.893 | 98.850 | 3158994.048 | 3159214.083 |
| ReplicationRings | gpu_live_bytes | 199632960.000 | 0.000 | 199632960.000 | 199632960.000 |
| ReplicationRings | gpu_peak_bytes | 200699200.000 | 0.000 | 200699200.000 | 200699200.000 |
| ReplicationRings | gpu_resources_created | 12.000 | 0.000 | 12.000 | 12.000 |
| ReplicationRings | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.001 | 0.002 |
| ReplicationRings | gpu_resources_created_per_second | 2.400 | 0.000 | 2.400 | 2.400 |
| ReplicationRings | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | physics_ms_per_frame | 0.004 | 0.001 | 0.004 | 0.005 |
| ReplicationRings | physics_ms_per_second | 6.749 | 0.641 | 6.121 | 7.630 |
| ReplicationRings | physics_wall_ms_per_frame | 0.004 | 0.001 | 0.004 | 0.005 |
| ReplicationRings | physics_wall_ms_per_second | 6.749 | 0.641 | 6.121 | 7.630 |
| ReplicationRings | process_peak_rss_bytes | 223673002.667 | 960843.572 | 222887936.000 | 225026048.000 |
| ReplicationRings | render_ms_per_frame | 0.374 | 0.016 | 0.358 | 0.396 |
| ReplicationRings | render_ms_per_second | 579.091 | 6.450 | 569.970 | 583.767 |
| ReplicationRings | render_wall_ms_per_frame | 0.378 | 0.016 | 0.362 | 0.400 |
| ReplicationRings | render_wall_ms_per_second | 584.344 | 6.419 | 575.270 | 589.115 |
| ReplicationRings | replication_ms_per_frame | 0.013 | 0.002 | 0.011 | 0.016 |
| ReplicationRings | replication_ms_per_second | 20.474 | 1.816 | 18.646 | 22.951 |
| ReplicationRings | replication_wall_ms_per_frame | 0.015 | 0.002 | 0.013 | 0.018 |
| ReplicationRings | replication_wall_ms_per_second | 22.452 | 2.045 | 20.397 | 25.241 |
| ReplicationRings | script_ms_per_frame | 0.023 | 0.003 | 0.021 | 0.027 |
| ReplicationRings | script_ms_per_second | 35.638 | 2.399 | 33.476 | 38.983 |
| ReplicationRings | submitted_frames | 7753.667 | 403.559 | 7197.000 | 8141.000 |
| ReplicationRings | uploaded_bytes | 558137888.000 | 29047625.662 | 517342920.000 | 582715656.000 |
| ReplicationRings | uploaded_bytes_per_frame | 71984.385 | 380.036 | 71577.897 | 72492.123 |
| ReplicationRings | uploaded_bytes_per_second | 111618043.289 | 5810599.524 | 103457018.292 | 116530934.000 |
| Rings | cpu_allocated_bytes | 3669822233.667 | 78893983.385 | 3571202376.000 | 3764320476.000 |
| Rings | cpu_allocated_bytes_per_frame | 232460.236 | 79.815 | 232365.461 | 232560.717 |
| Rings | cpu_allocated_bytes_per_second | 733920552.125 | 15782734.273 | 714191212.718 | 752824410.212 |
| Rings | cpu_allocations | 3275118.333 | 70470.075 | 3186986.000 | 3359478.000 |
| Rings | cpu_allocations_per_frame | 207.458 | 0.067 | 207.375 | 207.540 |
| Rings | cpu_allocations_per_second | 654984.493 | 14097.529 | 637353.238 | 671860.183 |
| Rings | cpu_live_blocks | 24595.000 | 0.816 | 24594.000 | 24596.000 |
| Rings | cpu_live_bytes | 34768235.333 | 346.277 | 34767806.000 | 34768654.000 |
| Rings | cpu_peak_bytes | 34768323.333 | 397.986 | 34767806.000 | 34768774.000 |
| Rings | cpu_process_peak_bytes | 34899163.333 | 218.732 | 34898854.000 | 34899318.000 |
| Rings | cpu_profiler_overhead_bytes | 787040.000 | 26.128 | 787008.000 | 787072.000 |
| Rings | draw_calls_per_frame | 16.000 | 0.000 | 16.000 | 16.000 |
| Rings | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Rings | frame_count | 15787.000 | 344.797 | 15356.000 | 16200.000 |
| Rings | frame_ms_per_frame | 0.305 | 0.007 | 0.298 | 0.314 |
| Rings | frame_ms_per_second | 963.728 | 1.179 | 962.150 | 964.983 |
| Rings | framegraph_dropped_spans | 456.667 | 0.943 | 456.000 | 458.000 |
| Rings | framegraph_off_thread_dropped_spans | 456.667 | 0.943 | 456.000 | 458.000 |
| Rings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | gpu_allocated_bytes | 104883264.000 | 0.000 | 104883264.000 | 104883264.000 |
| Rings | gpu_allocated_bytes_per_frame | 6646.824 | 145.470 | 6474.276 | 6830.116 |
| Rings | gpu_allocated_bytes_per_second | 20975395.283 | 141.744 | 20975206.002 | 20975547.078 |
| Rings | gpu_live_bytes | 100444416.000 | 0.000 | 100444416.000 | 100444416.000 |
| Rings | gpu_peak_bytes | 100444416.000 | 0.000 | 100444416.000 | 100444416.000 |
| Rings | gpu_resources_created | 43.000 | 0.000 | 43.000 | 43.000 |
| Rings | gpu_resources_created_per_frame | 0.003 | 0.000 | 0.003 | 0.003 |
| Rings | gpu_resources_created_per_second | 8.599 | 0.000 | 8.599 | 8.600 |
| Rings | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | physics_ms_per_frame | 0.002 | 0.000 | 0.001 | 0.002 |
| Rings | physics_ms_per_second | 4.782 | 0.035 | 4.734 | 4.812 |
| Rings | physics_wall_ms_per_frame | 0.002 | 0.000 | 0.001 | 0.002 |
| Rings | physics_wall_ms_per_second | 4.782 | 0.035 | 4.734 | 4.812 |
| Rings | process_peak_rss_bytes | 212930560.000 | 1026144.059 | 211513344.000 | 213909504.000 |
| Rings | render_ms_per_frame | 0.210 | 0.004 | 0.205 | 0.216 |
| Rings | render_ms_per_second | 663.418 | 1.311 | 662.415 | 665.270 |
| Rings | render_wall_ms_per_frame | 0.212 | 0.004 | 0.207 | 0.217 |
| Rings | render_wall_ms_per_second | 668.779 | 1.236 | 667.825 | 670.525 |
| Rings | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | replication_ms_per_second | 0.854 | 0.037 | 0.810 | 0.901 |
| Rings | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | replication_wall_ms_per_second | 0.854 | 0.037 | 0.810 | 0.901 |
| Rings | script_ms_per_frame | 0.011 | 0.000 | 0.010 | 0.011 |
| Rings | script_ms_per_second | 34.202 | 0.184 | 33.979 | 34.431 |
| Rings | submitted_frames | 15787.000 | 344.797 | 15356.000 | 16200.000 |
| Rings | uploaded_bytes | 646650460.000 | 14122867.031 | 628996700.000 | 663566940.000 |
| Rings | uploaded_bytes_per_frame | 40960.947 | 0.021 | 40960.922 | 40960.973 |
| Rings | uploaded_bytes_per_second | 129322357.733 | 2825264.531 | 125790663.388 | 132706392.409 |
| ServerPhysics | cpu_allocated_bytes | 734747175.333 | 1132183.551 | 733146026.000 | 735547750.000 |
| ServerPhysics | cpu_allocated_bytes_per_frame | 13951826.969 | 104040.206 | 13878259.434 | 14098962.038 |
| ServerPhysics | cpu_allocated_bytes_per_second | 144946704.310 | 117530.044 | 144794455.658 | 145080580.069 |
| ServerPhysics | cpu_allocations | 1344726.000 | 47158.365 | 1278034.000 | 1378072.000 |
| ServerPhysics | cpu_allocations_per_frame | 25526.765 | 671.177 | 24577.577 | 26001.358 |
| ServerPhysics | cpu_allocations_per_second | 265265.306 | 8883.565 | 252705.861 | 271813.061 |
| ServerPhysics | cpu_live_blocks | 212832.000 | 0.000 | 212832.000 | 212832.000 |
| ServerPhysics | cpu_live_bytes | 434408483.000 | 0.000 | 434408483.000 | 434408483.000 |
| ServerPhysics | cpu_peak_bytes | 434408483.000 | 0.000 | 434408483.000 | 434408483.000 |
| ServerPhysics | cpu_process_peak_bytes | 445252283.000 | 0.000 | 445252283.000 | 445252283.000 |
| ServerPhysics | cpu_profiler_overhead_bytes | 6810624.000 | 0.000 | 6810624.000 | 6810624.000 |
| ServerPhysics | duration_seconds | 5.069 | 0.009 | 5.057 | 5.080 |
| ServerPhysics | frame_count | 52.667 | 0.471 | 52.000 | 53.000 |
| ServerPhysics | frame_ms_per_frame | 95.663 | 0.855 | 94.740 | 96.800 |
| ServerPhysics | frame_ms_per_second | 993.838 | 2.444 | 990.395 | 995.820 |
| ServerPhysics | framegraph_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | framegraph_off_thread_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | physics_ms_per_frame | 91.796 | 0.787 | 90.930 | 92.834 |
| ServerPhysics | physics_ms_per_second | 953.671 | 2.267 | 950.569 | 955.924 |
| ServerPhysics | physics_wall_ms_per_frame | 91.796 | 0.787 | 90.930 | 92.834 |
| ServerPhysics | physics_wall_ms_per_second | 953.671 | 2.267 | 950.569 | 955.924 |
| ServerPhysics | process_peak_rss_bytes | 457286997.333 | 417073.020 | 456794112.000 | 457814016.000 |
| ServerPhysics | render_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| ServerPhysics | render_ms_per_second | 0.017 | 0.001 | 0.016 | 0.018 |
| ServerPhysics | render_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| ServerPhysics | render_wall_ms_per_second | 0.017 | 0.001 | 0.016 | 0.018 |
| ServerPhysics | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | replication_ms_per_second | 0.004 | 0.001 | 0.004 | 0.005 |
| ServerPhysics | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | replication_wall_ms_per_second | 0.004 | 0.001 | 0.004 | 0.005 |
| ServerPhysics | script_ms_per_frame | 0.054 | 0.001 | 0.052 | 0.056 |
| ServerPhysics | script_ms_per_second | 0.557 | 0.010 | 0.547 | 0.571 |
| ServerPhysics | tick_count | 52.667 | 0.471 | 52.000 | 53.000 |
| ServerPhysics | tick_overruns | 44.667 | 1.886 | 42.000 | 46.000 |
| ServerPhysics | tick_rate_hz | 30.000 | 0.000 | 30.000 | 30.000 |
| ServerRings | clients_admitted | 1.000 | 0.000 | 1.000 | 1.000 |
| ServerRings | cpu_allocated_bytes | 20800038.667 | 18712.925 | 20773600.000 | 20814262.000 |
| ServerRings | cpu_allocated_bytes_per_frame | 137748.600 | 123.927 | 137573.510 | 137842.795 |
| ServerRings | cpu_allocated_bytes_per_second | 4158494.803 | 3707.255 | 4153253.707 | 4161232.813 |
| ServerRings | cpu_allocations | 131911.667 | 372.767 | 131385.000 | 132195.000 |
| ServerRings | cpu_allocations_per_frame | 873.587 | 2.469 | 870.099 | 875.464 |
| ServerRings | cpu_allocations_per_second | 26372.738 | 74.309 | 26267.726 | 26428.714 |
| ServerRings | cpu_live_blocks | 16362.000 | 0.000 | 16362.000 | 16362.000 |
| ServerRings | cpu_live_bytes | 18945597.000 | 0.000 | 18945597.000 | 18945597.000 |
| ServerRings | cpu_peak_bytes | 18946966.000 | 0.000 | 18946966.000 | 18946966.000 |
| ServerRings | cpu_process_peak_bytes | 18967941.000 | 0.000 | 18967941.000 | 18967941.000 |
| ServerRings | cpu_profiler_overhead_bytes | 523584.000 | 0.000 | 523584.000 | 523584.000 |
| ServerRings | duration_seconds | 5.002 | 0.000 | 5.002 | 5.002 |
| ServerRings | frame_count | 151.000 | 0.000 | 151.000 | 151.000 |
| ServerRings | frame_ms_per_frame | 1.788 | 0.054 | 1.716 | 1.846 |
| ServerRings | frame_ms_per_second | 53.989 | 1.631 | 51.815 | 55.744 |
| ServerRings | framegraph_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | framegraph_off_thread_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | physics_ms_per_frame | 0.063 | 0.003 | 0.059 | 0.066 |
| ServerRings | physics_ms_per_second | 1.901 | 0.082 | 1.788 | 1.982 |
| ServerRings | physics_wall_ms_per_frame | 0.063 | 0.003 | 0.059 | 0.066 |
| ServerRings | physics_wall_ms_per_second | 1.901 | 0.082 | 1.788 | 1.982 |
| ServerRings | process_peak_rss_bytes | 32967338.667 | 440167.035 | 32415744.000 | 33492992.000 |
| ServerRings | render_ms_per_frame | 0.002 | 0.000 | 0.001 | 0.002 |
| ServerRings | render_ms_per_second | 0.045 | 0.004 | 0.042 | 0.051 |
| ServerRings | render_wall_ms_per_frame | 0.002 | 0.000 | 0.001 | 0.002 |
| ServerRings | render_wall_ms_per_second | 0.045 | 0.004 | 0.042 | 0.051 |
| ServerRings | replication_ms_per_frame | 0.710 | 0.030 | 0.674 | 0.748 |
| ServerRings | replication_ms_per_second | 21.433 | 0.912 | 20.335 | 22.568 |
| ServerRings | replication_wall_ms_per_frame | 0.717 | 0.031 | 0.680 | 0.755 |
| ServerRings | replication_wall_ms_per_second | 21.638 | 0.924 | 20.526 | 22.789 |
| ServerRings | script_ms_per_frame | 0.672 | 0.010 | 0.659 | 0.680 |
| ServerRings | script_ms_per_second | 20.290 | 0.290 | 19.884 | 20.542 |
| ServerRings | tick_count | 151.000 | 0.000 | 151.000 | 151.000 |
| ServerRings | tick_overruns | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | tick_rate_hz | 30.000 | 0.000 | 30.000 | 30.000 |
| StressPhysics | cpu_allocated_bytes | 2450652952.667 | 18194068.669 | 2424922654.000 | 2463518102.000 |
| StressPhysics | cpu_allocated_bytes_per_frame | 51413562.202 | 127658.809 | 51323293.792 | 51594099.021 |
| StressPhysics | cpu_allocated_bytes_per_second | 483993255.776 | 648505.182 | 483114925.317 | 484660982.070 |
| StressPhysics | cpu_allocations | 148004.667 | 116.437 | 147840.000 | 148087.000 |
| StressPhysics | cpu_allocations_per_frame | 3105.275 | 28.466 | 3085.146 | 3145.532 |
| StressPhysics | cpu_allocations_per_second | 29231.473 | 157.761 | 29106.462 | 29454.016 |
| StressPhysics | cpu_live_blocks | 223100.000 | 0.000 | 223100.000 | 223100.000 |
| StressPhysics | cpu_live_bytes | 587858939.000 | 0.000 | 587858939.000 | 587858939.000 |
| StressPhysics | cpu_peak_bytes | 587858939.000 | 0.000 | 587858939.000 | 587858939.000 |
| StressPhysics | cpu_process_peak_bytes | 587932763.000 | 0.000 | 587932763.000 | 587932763.000 |
| StressPhysics | cpu_profiler_overhead_bytes | 7139200.000 | 0.000 | 7139200.000 | 7139200.000 |
| StressPhysics | draw_calls_per_frame | 16.000 | 0.000 | 16.000 | 16.000 |
| StressPhysics | duration_seconds | 5.063 | 0.031 | 5.019 | 5.088 |
| StressPhysics | frame_count | 47.667 | 0.471 | 47.000 | 48.000 |
| StressPhysics | frame_ms_per_frame | 106.159 | 0.402 | 105.825 | 106.724 |
| StressPhysics | frame_ms_per_second | 999.344 | 0.007 | 999.336 | 999.354 |
| StressPhysics | framegraph_dropped_spans | 474.667 | 3.399 | 470.000 | 478.000 |
| StressPhysics | framegraph_off_thread_dropped_spans | 474.667 | 3.399 | 470.000 | 478.000 |
| StressPhysics | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | gpu_allocated_bytes | 133084224.000 | 0.000 | 133084224.000 | 133084224.000 |
| StressPhysics | gpu_allocated_bytes_per_frame | 2792251.745 | 27808.734 | 2772588.000 | 2831579.234 |
| StressPhysics | gpu_allocated_bytes_per_second | 26284757.736 | 162579.510 | 26157670.571 | 26514237.406 |
| StressPhysics | gpu_live_bytes | 128645376.000 | 0.000 | 128645376.000 | 128645376.000 |
| StressPhysics | gpu_peak_bytes | 128645376.000 | 0.000 | 128645376.000 | 128645376.000 |
| StressPhysics | gpu_resources_created | 43.000 | 0.000 | 43.000 | 43.000 |
| StressPhysics | gpu_resources_created_per_frame | 0.902 | 0.009 | 0.896 | 0.915 |
| StressPhysics | gpu_resources_created_per_second | 8.493 | 0.053 | 8.452 | 8.567 |
| StressPhysics | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | physics_ms_per_frame | 25.658 | 0.103 | 25.519 | 25.764 |
| StressPhysics | physics_ms_per_second | 241.539 | 1.097 | 240.559 | 243.070 |
| StressPhysics | physics_wall_ms_per_frame | 25.658 | 0.103 | 25.519 | 25.764 |
| StressPhysics | physics_wall_ms_per_second | 241.539 | 1.097 | 240.559 | 243.070 |
| StressPhysics | process_peak_rss_bytes | 763853482.667 | 137255.052 | 763703296.000 | 764035072.000 |
| StressPhysics | render_ms_per_frame | 48.518 | 0.106 | 48.374 | 48.628 |
| StressPhysics | render_ms_per_second | 456.740 | 1.885 | 454.631 | 459.207 |
| StressPhysics | render_wall_ms_per_frame | 48.534 | 0.106 | 48.391 | 48.644 |
| StressPhysics | render_wall_ms_per_second | 456.893 | 1.888 | 454.778 | 459.362 |
| StressPhysics | replication_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| StressPhysics | replication_ms_per_second | 0.016 | 0.001 | 0.015 | 0.017 |
| StressPhysics | replication_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| StressPhysics | replication_wall_ms_per_second | 0.016 | 0.001 | 0.015 | 0.017 |
| StressPhysics | script_ms_per_frame | 0.046 | 0.001 | 0.044 | 0.047 |
| StressPhysics | script_ms_per_second | 0.431 | 0.009 | 0.418 | 0.439 |
| StressPhysics | submitted_frames | 47.667 | 0.471 | 47.000 | 48.000 |
| StressPhysics | uploaded_bytes | 376152168.000 | 3771424.728 | 370818568.000 | 378818968.000 |
| StressPhysics | uploaded_bytes_per_frame | 7891293.478 | 1086.619 | 7889756.766 | 7892061.833 |
| StressPhysics | uploaded_bytes_per_second | 74287217.029 | 290910.510 | 73877813.995 | 74527064.733 |
| Terrain | cpu_allocated_bytes | 839193915.667 | 6948038.067 | 829579798.000 | 845758645.000 |
| Terrain | cpu_allocated_bytes_per_frame | 49005.680 | 155.351 | 48803.153 | 49180.685 |
| Terrain | cpu_allocated_bytes_per_second | 167835606.144 | 1388267.131 | 165915305.628 | 169149681.375 |
| Terrain | cpu_allocations | 3981804.000 | 42347.028 | 3924628.000 | 4025822.000 |
| Terrain | cpu_allocations_per_frame | 232.516 | 0.154 | 232.304 | 232.667 |
| Terrain | cpu_allocations_per_second | 796345.722 | 8464.073 | 784922.506 | 805154.653 |
| Terrain | cpu_live_blocks | 44852.000 | 4.243 | 44846.000 | 44855.000 |
| Terrain | cpu_live_bytes | 96254746.333 | 141997.177 | 96053932.000 | 96355156.000 |
| Terrain | cpu_peak_bytes | 96254746.333 | 141997.177 | 96053932.000 | 96355156.000 |
| Terrain | cpu_process_peak_bytes | 96371327.667 | 25469.751 | 96335308.000 | 96389340.000 |
| Terrain | cpu_profiler_overhead_bytes | 1435264.000 | 135.765 | 1435072.000 | 1435360.000 |
| Terrain | draw_calls_per_frame | 450.290 | 1.331 | 448.633 | 451.891 |
| Terrain | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Terrain | frame_count | 17125.000 | 192.161 | 16868.000 | 17330.000 |
| Terrain | frame_ms_per_frame | 0.281 | 0.003 | 0.278 | 0.286 |
| Terrain | frame_ms_per_second | 963.023 | 0.883 | 961.792 | 963.822 |
| Terrain | framegraph_dropped_spans | 452.333 | 1.886 | 451.000 | 455.000 |
| Terrain | framegraph_off_thread_dropped_spans | 452.333 | 1.886 | 451.000 | 455.000 |
| Terrain | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | gpu_allocated_bytes | 219875712.000 | 0.000 | 219875712.000 | 219875712.000 |
| Terrain | gpu_allocated_bytes_per_frame | 12841.082 | 144.728 | 12687.577 | 13035.079 |
| Terrain | gpu_allocated_bytes_per_second | 43974312.888 | 689.876 | 43973359.527 | 43974969.068 |
| Terrain | gpu_live_bytes | 120495936.000 | 0.000 | 120495936.000 | 120495936.000 |
| Terrain | gpu_peak_bytes | 131676144.000 | 0.000 | 131676144.000 | 131676144.000 |
| Terrain | gpu_resources_created | 115.000 | 0.000 | 115.000 | 115.000 |
| Terrain | gpu_resources_created_per_frame | 0.007 | 0.000 | 0.007 | 0.007 |
| Terrain | gpu_resources_created_per_second | 23.000 | 0.000 | 22.999 | 23.000 |
| Terrain | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | physics_ms_per_frame | 0.001 | 0.000 | 0.000 | 0.001 |
| Terrain | physics_ms_per_second | 1.921 | 0.263 | 1.709 | 2.291 |
| Terrain | physics_wall_ms_per_frame | 0.001 | 0.000 | 0.000 | 0.001 |
| Terrain | physics_wall_ms_per_second | 1.921 | 0.263 | 1.709 | 2.291 |
| Terrain | process_peak_rss_bytes | 286861994.667 | 2564685.541 | 284332032.000 | 290377728.000 |
| Terrain | render_ms_per_frame | 0.171 | 0.001 | 0.170 | 0.172 |
| Terrain | render_ms_per_second | 584.694 | 3.314 | 580.905 | 588.978 |
| Terrain | render_wall_ms_per_frame | 0.172 | 0.001 | 0.172 | 0.174 |
| Terrain | render_wall_ms_per_second | 590.398 | 3.369 | 586.578 | 594.773 |
| Terrain | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | replication_ms_per_second | 0.921 | 0.020 | 0.899 | 0.948 |
| Terrain | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | replication_wall_ms_per_second | 0.921 | 0.020 | 0.899 | 0.948 |
| Terrain | script_ms_per_frame | 0.073 | 0.002 | 0.072 | 0.076 |
| Terrain | script_ms_per_second | 251.127 | 2.832 | 248.633 | 255.088 |
| Terrain | submitted_frames | 17125.000 | 192.161 | 16868.000 | 17330.000 |
| Terrain | uploaded_bytes | 24726.667 | 37.712 | 24700.000 | 24780.000 |
| Terrain | uploaded_bytes_per_frame | 1.444 | 0.015 | 1.430 | 1.464 |
| Terrain | uploaded_bytes_per_second | 4945.240 | 7.566 | 4939.800 | 4955.940 |

<details>
<summary>Exact reusable baseline aggregates</summary>

<!-- VERSION_IMPACT_BASELINE_START -->
```json
{
  "dropped_spans": 478.0,
  "heap_compiled": true,
  "heap_dropped_scopes": 0,
  "manifest": {
    "client_binary_sha256": "11fe6407670445c7c0ad669820df7d0d0682010cfcf8c99a13f86e7705a68417",
    "label": "v0.24.1",
    "machine": {
      "compiler": "c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0\nCopyright (C) 2023 Free Software Foundation, Inc.\nThis is free software; see the source for copying conditions.  There is NO\nwarranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.",
      "cpu": "AMD Ryzen 9 9900X 12-Core Processor",
      "fingerprint": "5675a8d8002198eeac91db34876ea236c284ac3be38542a446379a7671fa438a",
      "gpu": "NVIDIA GeForce RTX 4090, 580.173.02",
      "os": "Linux 7.0.0-38-generic #38~24.04.4-Ubuntu SMP PREEMPT_DYNAMIC Mon Sep 14 16:37:11 UTC 2 x86_64 GNU/Linux"
    },
    "schema": 1,
    "server_binary_sha256": "c4eee0c5650bbf368a7fbfe3ff1db6db73c76e4be817ba40cc131b98c3971532",
    "settings": {
      "compute": "serial",
      "height": 540,
      "max_fps": 0,
      "preset": "profile",
      "replica_viewer_max_fps": 60,
      "runs": 3,
      "seconds": 5,
      "width": 960
    },
    "source_diff_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "source_dirty": false,
    "source_revision": "4b41df33893e0ea2b903f9e8b8cf7c009f245f58",
    "workloads": [
      {
        "asset_sha256": "c8df001fd24092388f34eead2603adda66b40c3b8f83be596ac5f39edeb78df2",
        "kind": "client",
        "name": "Rings",
        "reports": [
          "Rings/run-1.json",
          "Rings/run-2.json",
          "Rings/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "3ecaa52f5a1472a8fc1c1cb470b97a182e78d6a88eba667102685ae1b620d76d",
        "kind": "client",
        "name": "Meshes",
        "reports": [
          "Meshes/run-1.json",
          "Meshes/run-2.json",
          "Meshes/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "3a7abace2e8d0b6d1eaefecda1446ddc696fc52e73ca7bde8a09227ed3e49c40",
        "kind": "client",
        "name": "Particles",
        "reports": [
          "Particles/run-1.json",
          "Particles/run-2.json",
          "Particles/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "7f126002299ac10e2bdbeb4cdb40c30dbfbded0bb9d4602fafc10b8f7a7ae56d",
        "kind": "client",
        "name": "Interface",
        "reports": [
          "Interface/run-1.json",
          "Interface/run-2.json",
          "Interface/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "992e63069c3de33dbfdb4a0db129afd62b206987089c401c0017c2e59f3f551d",
        "kind": "client",
        "name": "Terrain",
        "reports": [
          "Terrain/run-1.json",
          "Terrain/run-2.json",
          "Terrain/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "3d198ca251f42f7c88ed2f103e3799813b909a96fc3ee9d7ff68c598bf826389",
        "kind": "client",
        "name": "Cube",
        "reports": [
          "Cube/run-1.json",
          "Cube/run-2.json",
          "Cube/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "d3d0c7b0d5a62f95dd5c983af37ed8a51d8c6393b5120317e5e3120c40d5346c",
        "kind": "client",
        "name": "StressPhysics",
        "reports": [
          "StressPhysics/run-1.json",
          "StressPhysics/run-2.json",
          "StressPhysics/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "1627a655968751ec91466190049a01899adc279a3f383978646b291902947868",
        "kind": "client",
        "name": "RenderFeatures",
        "reports": [
          "RenderFeatures/run-1.json",
          "RenderFeatures/run-2.json",
          "RenderFeatures/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "c8df001fd24092388f34eead2603adda66b40c3b8f83be596ac5f39edeb78df2",
        "kind": "replica",
        "name": "ReplicationRings",
        "reports": [
          "ReplicationRings/run-1.json",
          "ReplicationRings/run-2.json",
          "ReplicationRings/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "c8df001fd24092388f34eead2603adda66b40c3b8f83be596ac5f39edeb78df2",
        "kind": "server-replica",
        "name": "ServerRings",
        "reports": [
          "ServerRings/run-1.json",
          "ServerRings/run-2.json",
          "ServerRings/run-3.json"
        ],
        "status": "measured"
      },
      {
        "asset_sha256": "d3d0c7b0d5a62f95dd5c983af37ed8a51d8c6393b5120317e5e3120c40d5346c",
        "kind": "server",
        "name": "ServerPhysics",
        "reports": [
          "ServerPhysics/run-1.json",
          "ServerPhysics/run-2.json",
          "ServerPhysics/run-3.json"
        ],
        "status": "measured"
      },
      {
        "kind": "client-world",
        "name": "ImageGraph3D",
        "reports": [],
        "status": "unavailable"
      },
      {
        "kind": "client-world",
        "name": "ImageGraphFeedback",
        "reports": [],
        "status": "unavailable"
      },
      {
        "kind": "client-world",
        "name": "ImageGraphGui",
        "reports": [],
        "status": "unavailable"
      },
      {
        "kind": "client-world",
        "name": "ImageGraphMaterials",
        "reports": [],
        "status": "unavailable"
      },
      {
        "kind": "client-world",
        "name": "ImageGraphFlipbook",
        "reports": [],
        "status": "unavailable"
      },
      {
        "kind": "client-world",
        "name": "ImageGraphSkybox",
        "reports": [],
        "status": "unavailable"
      },
      {
        "kind": "client-world",
        "name": "ImageGraphVerlet",
        "reports": [],
        "status": "unavailable"
      }
    ]
  },
  "off_thread_dropped_spans": 478.0,
  "owner_dropped_spans": 0,
  "schema": 1,
  "workloads": {
    "Cube": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 2230550685.0,
          "mean": 2151880285.0,
          "min": 2002903222.0,
          "stddev": 105398013.89409818
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 7385929.420529801,
          "mean": 7283550.953804506,
          "min": 7178864.594982079,
          "stddev": 84549.61174453003
        },
        "cpu_allocated_bytes_per_second": {
          "max": 440220297.8333307,
          "mean": 426334371.65600306,
          "min": 398589468.94490075,
          "stddev": 19618611.93539137
        },
        "cpu_allocations": {
          "max": 871381.0,
          "mean": 843250.6666666667,
          "min": 796397.0,
          "stddev": 33352.38594496984
        },
        "cpu_allocations_per_frame": {
          "max": 2885.3675496688743,
          "mean": 2855.327115338045,
          "min": 2826.144262295082,
          "stddev": 24.185409183895214
        },
        "cpu_allocations_per_second": {
          "max": 171975.29109109016,
          "mean": 167070.50572064752,
          "min": 158487.66621002127,
          "stddev": 6089.612353655923
        },
        "cpu_live_blocks": {
          "max": 47670.0,
          "mean": 47668.0,
          "min": 47666.0,
          "stddev": 1.632993161855452
        },
        "cpu_live_bytes": {
          "max": 141017922.0,
          "mean": 140577582.0,
          "min": 139836114.0,
          "stddev": 527368.3668328999
        },
        "cpu_peak_bytes": {
          "max": 141017922.0,
          "mean": 140577582.0,
          "min": 139836114.0,
          "stddev": 527368.3668328999
        },
        "cpu_process_peak_bytes": {
          "max": 147476070.0,
          "mean": 146874520.66666666,
          "min": 145871046.0,
          "stddev": 714228.4836686056
        },
        "cpu_profiler_overhead_bytes": {
          "max": 1525440.0,
          "mean": 1525376.0,
          "min": 1525312.0,
          "stddev": 52.255781179374466
        },
        "draw_calls_per_frame": {
          "max": 16.0,
          "mean": 16.0,
          "min": 16.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.066896497,
          "mean": 5.046693609999999,
          "min": 5.024977773,
          "stddev": 0.017146654070574523
        },
        "frame_count": {
          "max": 305.0,
          "mean": 295.33333333333337,
          "min": 279.0,
          "stddev": 11.61416759345623
        },
        "frame_ms_per_frame": {
          "max": 17.95568040365814,
          "mean": 17.062091352966604,
          "min": 16.503282630639,
          "stddev": 0.6384477133184491
        },
        "frame_ms_per_second": {
          "max": 997.0870134808617,
          "mean": 997.0080764381119,
          "min": 996.9466650257004,
          "stddev": 0.05862185602435032
        },
        "framegraph_dropped_spans": {
          "max": 474.0,
          "mean": 472.33333333333326,
          "min": 470.0,
          "stddev": 1.699673171197595
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 474.0,
          "mean": 472.33333333333326,
          "min": 470.0,
          "stddev": 1.699673171197595
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 108442688.0,
          "mean": 108442688.0,
          "min": 108442688.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 388683.4695340502,
          "mean": 367771.6715332442,
          "min": 355549.79672131146,
          "stddev": 14857.010517749846
        },
        "gpu_allocated_bytes_per_second": {
          "max": 21580729.88554889,
          "mean": 21488116.535602704,
          "min": 21402191.27511418,
          "stddev": 73041.34335856019
        },
        "gpu_live_bytes": {
          "max": 103872768.0,
          "mean": 103872768.0,
          "min": 103872768.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 103872768.0,
          "mean": 103872768.0,
          "min": 103872768.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 45.0,
          "mean": 45.0,
          "min": 45.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.16129032258064516,
          "mean": 0.15261264290125295,
          "min": 0.14754098360655737,
          "stddev": 0.006165150326214174
        },
        "gpu_resources_created_per_second": {
          "max": 8.955263492266994,
          "mean": 8.916832125206282,
          "min": 8.881176086119684,
          "stddev": 0.03030965491315762
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 11.442890343792884,
          "mean": 11.309203154450067,
          "min": 11.240690489601894,
          "stddev": 0.09454094083532107
        },
        "physics_ms_per_second": {
          "max": 682.0255526971051,
          "mean": 661.8193361908008,
          "min": 624.298082394237,
          "stddev": 26.557774682936518
        },
        "physics_wall_ms_per_frame": {
          "max": 11.442890380108297,
          "mean": 11.309203160432821,
          "min": 11.240690452861982,
          "stddev": 0.09454096260585328
        },
        "physics_wall_ms_per_second": {
          "max": 682.0255548615967,
          "mean": 661.8193365124214,
          "min": 624.2980834143409,
          "stddev": 26.55777426906954
        },
        "process_peak_rss_bytes": {
          "max": 323481600.0,
          "mean": 321452714.6666666,
          "min": 318312448.0,
          "stddev": 2251874.693203766
        },
        "render_ms_per_frame": {
          "max": 3.314279954920533,
          "mean": 3.14332385912419,
          "min": 3.048769663591854,
          "stddev": 0.12111115135368684
        },
        "render_ms_per_second": {
          "max": 184.1990291687897,
          "mean": 183.6709931652152,
          "min": 182.79639856945258,
          "stddev": 0.6228538046303554
        },
        "render_wall_ms_per_frame": {
          "max": 3.3235715529397396,
          "mean": 3.1515009149136066,
          "min": 3.056627520576852,
          "stddev": 0.12188612151913927
        },
        "render_wall_ms_per_second": {
          "max": 184.67378121230047,
          "mean": 184.14786482749741,
          "min": 183.23636751992584,
          "stddev": 0.6470672385251104
        },
        "replication_ms_per_frame": {
          "max": 0.0012553045292481727,
          "mean": 0.0011293701022927706,
          "min": 0.0010455650262168195,
          "stddev": 9.065990086435736e-05
        },
        "replication_ms_per_second": {
          "max": 0.0696978134992121,
          "mean": 0.06589018865031519,
          "min": 0.06317042086251914,
          "stddev": 0.0027736003554259684
        },
        "replication_wall_ms_per_frame": {
          "max": 0.0012553045292481727,
          "mean": 0.0011293701022927706,
          "min": 0.0010455650262168195,
          "stddev": 9.065990086435736e-05
        },
        "replication_wall_ms_per_second": {
          "max": 0.0696978134992121,
          "mean": 0.06589018865031519,
          "min": 0.06317042086251914,
          "stddev": 0.0027736003554259684
        },
        "script_ms_per_frame": {
          "max": 0.010459143120969069,
          "mean": 0.009270444354799345,
          "min": 0.008493665753808242,
          "stddev": 0.0008536330152416987
        },
        "script_ms_per_second": {
          "max": 0.5807191718199806,
          "mean": 0.5407244135979712,
          "min": 0.5062442185603795,
          "stddev": 0.03065330293341042
        },
        "submitted_frames": {
          "max": 305.0,
          "mean": 295.33333333333337,
          "min": 279.0,
          "stddev": 11.61416759345623
        },
        "uploaded_bytes": {
          "max": 245062296.0,
          "mean": 237329593.3333333,
          "min": 224223988.0,
          "stddev": 9317003.537433458
        },
        "uploaded_bytes_per_frame": {
          "max": 803670.2078853047,
          "mean": 803601.264865615,
          "min": 803482.937704918,
          "stddev": 84.05008488118237
        },
        "uploaded_bytes_per_second": {
          "max": 48544427.231202684,
          "mean": 47021983.13146512,
          "min": 44621886.52948695,
          "stddev": 1717417.9893415698
        }
      },
      "reason": "",
      "status": "measured"
    },
    "ImageGraph3D": {
      "basis": {},
      "metrics": {},
      "reason": "unavailable on this source revision",
      "status": "unavailable"
    },
    "ImageGraphFeedback": {
      "basis": {},
      "metrics": {},
      "reason": "unavailable on this source revision",
      "status": "unavailable"
    },
    "ImageGraphFlipbook": {
      "basis": {},
      "metrics": {},
      "reason": "unavailable on this source revision",
      "status": "unavailable"
    },
    "ImageGraphGui": {
      "basis": {},
      "metrics": {},
      "reason": "unavailable on this source revision",
      "status": "unavailable"
    },
    "ImageGraphMaterials": {
      "basis": {},
      "metrics": {},
      "reason": "unavailable on this source revision",
      "status": "unavailable"
    },
    "ImageGraphSkybox": {
      "basis": {},
      "metrics": {},
      "reason": "unavailable on this source revision",
      "status": "unavailable"
    },
    "ImageGraphVerlet": {
      "basis": {},
      "metrics": {},
      "reason": "unavailable on this source revision",
      "status": "unavailable"
    },
    "Interface": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 3332243374.0,
          "mean": 3241679910.6666665,
          "min": 3085794266.0,
          "stddev": 110708419.55066043
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 125077.79441449475,
          "mean": 124989.82233886202,
          "min": 124938.82396610551,
          "stddev": 62.46859162630655
        },
        "cpu_allocated_bytes_per_second": {
          "max": 666447309.6493309,
          "mean": 648321176.8811312,
          "min": 617143624.687346,
          "stddev": 22142989.853443835
        },
        "cpu_allocations": {
          "max": 9493664.0,
          "mean": 9234021.0,
          "min": 8786439.0,
          "stddev": 317839.1636734949
        },
        "cpu_allocations_per_frame": {
          "max": 356.1444205747639,
          "mean": 356.0337987187069,
          "min": 355.954557384425,
          "stddev": 0.08062489533458848
        },
        "cpu_allocations_per_second": {
          "max": 1898728.9106436993,
          "mean": 1846762.0271569374,
          "min": 1757244.4385876823,
          "stddev": 63571.51116334391
        },
        "cpu_live_blocks": {
          "max": 16711.0,
          "mean": 16710.0,
          "min": 16709.0,
          "stddev": 0.816496580927726
        },
        "cpu_live_bytes": {
          "max": 32525351.0,
          "mean": 32525052.33333333,
          "min": 32524863.0,
          "stddev": 213.69968543625785
        },
        "cpu_peak_bytes": {
          "max": 32525351.0,
          "mean": 32525068.33333333,
          "min": 32524887.0,
          "stddev": 202.52626715784027
        },
        "cpu_process_peak_bytes": {
          "max": 32656015.0,
          "mean": 32655988.333333336,
          "min": 32655935.0,
          "stddev": 37.712361663282536
        },
        "cpu_profiler_overhead_bytes": {
          "max": 534752.0,
          "mean": 534720.0,
          "min": 534688.0,
          "stddev": 26.127890589687233
        },
        "draw_calls_per_frame": {
          "max": 15.0,
          "mean": 15.0,
          "min": 15.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.000210336,
          "mean": 5.000114652333333,
          "min": 5.000010242,
          "stddev": 8.192076758408916e-05
        },
        "frame_count": {
          "max": 26671.0,
          "mean": 25936.0,
          "min": 24671.0,
          "stddev": 898.3967200889965
        },
        "frame_ms_per_frame": {
          "max": 0.19461113185923834,
          "mean": 0.18558051617910892,
          "min": 0.1803336040658823,
          "stddev": 0.006413488883291643
        },
        "frame_ms_per_second": {
          "max": 962.2464095067714,
          "mean": 961.4688341172757,
          "min": 960.2265524614907,
          "stddev": 0.8876634333233695
        },
        "framegraph_dropped_spans": {
          "max": 460.0,
          "mean": 456.0,
          "min": 454.0,
          "stddev": 2.82842712474619
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 460.0,
          "mean": 456.0,
          "min": 454.0,
          "stddev": 2.82842712474619
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 88035376.0,
          "mean": 88035376.0,
          "min": 88035376.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 3568.3748530663534,
          "mean": 3398.5075047964474,
          "min": 3300.7902215889917,
          "stddev": 120.56701502236731
        },
        "gpu_allocated_bytes_per_second": {
          "max": 17607039.13374104,
          "mean": 17606671.475532867,
          "min": 17606334.550803185,
          "stddev": 288.46454329117086
        },
        "gpu_live_bytes": {
          "max": 83596528.0,
          "mean": 83596528.0,
          "min": 83596528.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 83596528.0,
          "mean": 83596528.0,
          "min": 83596528.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 33.0,
          "mean": 33.0,
          "min": 33.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.0013376028535527543,
          "mean": 0.001273928195164212,
          "min": 0.001237298938922425,
          "stddev": 4.519446245947907e-05
        },
        "gpu_resources_created_per_second": {
          "max": 6.599986480587693,
          "mean": 6.599848664161831,
          "min": 6.599722368159194,
          "stddev": 0.00010813073517842132
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.00031383193981192114,
          "mean": 0.00030152412516282244,
          "min": 0.0002811758093076926,
          "stddev": 1.4493514862642141e-05
        },
        "physics_ms_per_second": {
          "max": 1.6611053457616596,
          "mean": 1.5627887882345541,
          "min": 1.4998449297267398,
          "stddev": 0.07042561712838528
        },
        "physics_wall_ms_per_frame": {
          "max": 0.00031383193981192114,
          "mean": 0.00030152412516282244,
          "min": 0.0002811758093076926,
          "stddev": 1.4493514862642141e-05
        },
        "physics_wall_ms_per_second": {
          "max": 1.6611053457616596,
          "mean": 1.5627887882345541,
          "min": 1.4998449297267398,
          "stddev": 0.07042561712838528
        },
        "process_peak_rss_bytes": {
          "max": 214745088.0,
          "mean": 211376810.66666663,
          "min": 209428480.0,
          "stddev": 2391480.2331818584
        },
        "render_ms_per_frame": {
          "max": 0.11129907985551162,
          "mean": 0.10678906244756631,
          "min": 0.10422200866863185,
          "stddev": 0.0031992254160223585
        },
        "render_ms_per_second": {
          "max": 555.9398998529251,
          "mean": 553.3487646369084,
          "min": 549.1583689009861,
          "stddev": 2.990598175496337
        },
        "render_wall_ms_per_frame": {
          "max": 0.14848051052970132,
          "mean": 0.1433639598941881,
          "min": 0.14028239361752554,
          "stddev": 0.003643089461066502
        },
        "render_wall_ms_per_second": {
          "max": 748.2928112316101,
          "mean": 742.9861111097769,
          "min": 732.6144572078291,
          "stddev": 7.3345308334286825
        },
        "replication_ms_per_frame": {
          "max": 0.00026941951203842616,
          "mean": 0.0002553407709735032,
          "min": 0.00024743523451289674,
          "stddev": 9.980262384491381e-06
        },
        "replication_ms_per_second": {
          "max": 1.3293369538471966,
          "mean": 1.3226805202144314,
          "min": 1.3188382824715825,
          "stddev": 0.004725484034706672
        },
        "replication_wall_ms_per_frame": {
          "max": 0.00026941951203842616,
          "mean": 0.0002553407709735032,
          "min": 0.00024743523451289674,
          "stddev": 9.980262384491381e-06
        },
        "replication_wall_ms_per_second": {
          "max": 1.3293369538471966,
          "mean": 1.3226805202144314,
          "min": 1.3188382824715825,
          "stddev": 0.004725484034706672
        },
        "script_ms_per_frame": {
          "max": 0.0002375613630608246,
          "mean": 0.00022773726258962349,
          "min": 0.0002180367051180466,
          "stddev": 7.971386918229822e-06
        },
        "script_ms_per_second": {
          "max": 1.257406911365535,
          "mean": 1.1811732747057837,
          "min": 1.1230639027007239,
          "stddev": 0.05632274686266265
        },
        "submitted_frames": {
          "max": 26671.0,
          "mean": 25936.0,
          "min": 24671.0,
          "stddev": 898.3967200889965
        },
        "uploaded_bytes": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "uploaded_bytes_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "uploaded_bytes_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        }
      },
      "reason": "",
      "status": "measured"
    },
    "Meshes": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 957176622.0,
          "mean": 931392720.3333334,
          "min": 895267733.0,
          "stddev": 26310722.45147194
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 31237.53429867411,
          "mean": 31170.46472448644,
          "min": 31124.658472344163,
          "stddev": 48.47222359744943
        },
        "cpu_allocated_bytes_per_second": {
          "max": 191432268.87298363,
          "mean": 186275617.6044723,
          "min": 179049742.6524074,
          "stddev": 5262635.834525551
        },
        "cpu_allocations": {
          "max": 6831988.0,
          "mean": 6641629.333333334,
          "min": 6376606.0,
          "stddev": 193260.2749615715
        },
        "cpu_allocations_per_frame": {
          "max": 222.49148639218424,
          "mean": 222.26645909310236,
          "min": 222.1510931763305,
          "stddev": 0.15913537094229102
        },
        "cpu_allocations_per_second": {
          "max": 1366375.790729454,
          "mean": 1328305.001449261,
          "min": 1275294.1061216567,
          "stddev": 38655.51615901226
        },
        "cpu_live_blocks": {
          "max": 16390.0,
          "mean": 16390.0,
          "min": 16390.0,
          "stddev": 0.0
        },
        "cpu_live_bytes": {
          "max": 32508777.0,
          "mean": 32508777.0,
          "min": 32508777.0,
          "stddev": 0.0
        },
        "cpu_peak_bytes": {
          "max": 32508777.0,
          "mean": 32508777.0,
          "min": 32508777.0,
          "stddev": 0.0
        },
        "cpu_process_peak_bytes": {
          "max": 32639943.0,
          "mean": 32639601.666666664,
          "min": 32638919.0,
          "stddev": 482.71822929001644
        },
        "cpu_profiler_overhead_bytes": {
          "max": 524480.0,
          "mean": 524480.0,
          "min": 524480.0,
          "stddev": 0.0
        },
        "draw_calls_per_frame": {
          "max": 37.09710397766923,
          "mean": 37.093216981819324,
          "min": 37.09049523623712,
          "stddev": 0.002821018961169266
        },
        "duration_seconds": {
          "max": 5.000106226,
          "mean": 5.000079000333333,
          "min": 5.000050968,
          "stddev": 2.2566194101288364e-05
        },
        "frame_count": {
          "max": 30753.0,
          "mean": 29882.0,
          "min": 28660.0,
          "stddev": 889.7801226520329
        },
        "frame_ms_per_frame": {
          "max": 0.16571408987294853,
          "mean": 0.15929274987349895,
          "min": 0.15460022523709943,
          "stddev": 0.00469901398917069
        },
        "frame_ms_per_second": {
          "max": 952.716374801242,
          "mean": 951.1461087599124,
          "min": 949.8529833351395,
          "stddev": 1.1852869872021796
        },
        "framegraph_dropped_spans": {
          "max": 453.0,
          "mean": 450.66666666666674,
          "min": 448.0,
          "stddev": 2.0548046676563256
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 453.0,
          "mean": 450.66666666666674,
          "min": 448.0,
          "stddev": 2.0548046676563256
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 119985024.0,
          "mean": 119985024.0,
          "min": 119985024.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 4186.49769713887,
          "mean": 4018.9154871715864,
          "min": 3901.5713588918156,
          "stddev": 121.62415657932854
        },
        "gpu_allocated_bytes_per_second": {
          "max": 23996760.18662536,
          "mean": 23996625.65220371,
          "min": 23996494.989664648,
          "stddev": 108.30081747628996
        },
        "gpu_live_bytes": {
          "max": 115546176.0,
          "mean": 115546176.0,
          "min": 115546176.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 115546176.0,
          "mean": 115546176.0,
          "min": 115546176.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 51.0,
          "mean": 51.0,
          "min": 51.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.001779483600837404,
          "mean": 0.0017082522719314612,
          "min": 0.001658374792703151,
          "stddev": 5.1696718296658055e-05
        },
        "gpu_resources_created_per_second": {
          "max": 10.199896026339866,
          "mean": 10.199838842074067,
          "min": 10.199783303563759,
          "stddev": 4.6033592420272585e-05
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.0003331255900446506,
          "mean": 0.00029712142110802314,
          "min": 0.00025275176429102734,
          "stddev": 3.334140576096426e-05
        },
        "physics_ms_per_second": {
          "max": 2.0142566602372924,
          "mean": 1.7732735368573982,
          "min": 1.5545501886510118,
          "stddev": 0.1883332740961411
        },
        "physics_wall_ms_per_frame": {
          "max": 0.0003331255900446506,
          "mean": 0.00029712142110802314,
          "min": 0.00025275176429102734,
          "stddev": 3.334140576096426e-05
        },
        "physics_wall_ms_per_second": {
          "max": 2.0142566602372924,
          "mean": 1.7732735368573982,
          "min": 1.5545501886510118,
          "stddev": 0.1883332740961411
        },
        "process_peak_rss_bytes": {
          "max": 247902208.0,
          "mean": 237214378.6666667,
          "min": 219619328.0,
          "stddev": 12536948.19814193
        },
        "render_ms_per_frame": {
          "max": 0.13499405026911587,
          "mean": 0.12987075559576322,
          "min": 0.12596927253025564,
          "stddev": 0.003784289542547856
        },
        "render_ms_per_second": {
          "max": 777.8807753118743,
          "mean": 775.4748245701971,
          "min": 773.769457255699,
          "stddev": 1.7500184558240166
        },
        "render_wall_ms_per_frame": {
          "max": 0.13750424766497113,
          "mean": 0.1322848052460876,
          "min": 0.12833537164530065,
          "stddev": 0.003849395074948302
        },
        "render_wall_ms_per_second": {
          "max": 792.1859928543539,
          "mean": 789.8901779942189,
          "min": 788.1576030497063,
          "stddev": 1.6921210568316924
        },
        "replication_ms_per_frame": {
          "max": 0.00028125444366690875,
          "mean": 0.00026687385481192084,
          "min": 0.00025623048545191817,
          "stddev": 1.055224236364549e-05
        },
        "replication_ms_per_second": {
          "max": 1.618422356893737,
          "mean": 1.5932820128579843,
          "min": 1.549307460313041,
          "stddev": 0.03120109816406924
        },
        "replication_wall_ms_per_frame": {
          "max": 0.000281254443650661,
          "mean": 0.0002668738548065049,
          "min": 0.00025623048545191817,
          "stddev": 1.0552242356264685e-05
        },
        "replication_wall_ms_per_second": {
          "max": 1.618422356893737,
          "mean": 1.5932820128269407,
          "min": 1.549307460313041,
          "stddev": 0.031201098145330185
        },
        "script_ms_per_frame": {
          "max": 0.0003051547589293669,
          "mean": 0.00027383985601343646,
          "min": 0.000235772382850199,
          "stddev": 2.8724860676266535e-05
        },
        "script_ms_per_second": {
          "max": 1.8451299568255821,
          "mean": 1.6345233490250586,
          "min": 1.4501184720374545,
          "stddev": 0.1623235786976635
        },
        "submitted_frames": {
          "max": 30753.0,
          "mean": 29882.0,
          "min": 28660.0,
          "stddev": 889.7801226520329
        },
        "uploaded_bytes": {
          "max": 1324.0,
          "mean": 1324.0,
          "min": 1324.0,
          "stddev": 0.0
        },
        "uploaded_bytes_per_frame": {
          "max": 0.04619678995115143,
          "mean": 0.04434756878504421,
          "min": 0.04305271030468572,
          "stddev": 0.0013420873534269666
        },
        "uploaded_bytes_per_second": {
          "max": 264.79730076223495,
          "mean": 264.79581621384443,
          "min": 264.7943743905572,
          "stddev": 0.0011950681640024797
        }
      },
      "reason": "",
      "status": "measured"
    },
    "Particles": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 22649137525.0,
          "mean": 20832077209.0,
          "min": 19469472493.0,
          "stddev": 1337277046.3917615
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 860072.9996465963,
          "mean": 859915.447662465,
          "min": 859712.9445815146,
          "stddev": 150.38919132633754
        },
        "cpu_allocated_bytes_per_second": {
          "max": 4529669495.63305,
          "mean": 4166321991.052423,
          "min": 3893860674.1898675,
          "stddev": 267405358.57842687
        },
        "cpu_allocations": {
          "max": 6398705.0,
          "mean": 5889143.0,
          "min": 5506972.0,
          "stddev": 375027.3643198142
        },
        "cpu_allocations_per_frame": {
          "max": 243.273048548836,
          "mean": 243.10239254348136,
          "min": 242.8811918770165,
          "stddev": 0.16391864829322408
        },
        "cpu_allocations_per_second": {
          "max": 1279696.3600959314,
          "mean": 1177802.1880997918,
          "min": 1101384.8327105122,
          "stddev": 74991.33877964388
        },
        "cpu_live_blocks": {
          "max": 16898.0,
          "mean": 16898.0,
          "min": 16898.0,
          "stddev": 0.0
        },
        "cpu_live_bytes": {
          "max": 40585337.0,
          "mean": 40585337.0,
          "min": 40585337.0,
          "stddev": 0.0
        },
        "cpu_peak_bytes": {
          "max": 40585337.0,
          "mean": 40585337.0,
          "min": 40585337.0,
          "stddev": 0.0
        },
        "cpu_process_peak_bytes": {
          "max": 40716509.0,
          "mean": 40716509.0,
          "min": 40716509.0,
          "stddev": 0.0
        },
        "cpu_profiler_overhead_bytes": {
          "max": 540736.0,
          "mean": 540736.0,
          "min": 540736.0,
          "stddev": 0.0
        },
        "draw_calls_per_frame": {
          "max": 25.99962042133232,
          "mean": 25.999585551509632,
          "min": 25.99955824535053,
          "stddev": 2.594057224979678e-05
        },
        "duration_seconds": {
          "max": 5.000174416,
          "mean": 5.000108816333333,
          "min": 5.000043433,
          "stddev": 5.347380462952723e-05
        },
        "frame_count": {
          "max": 26345.0,
          "mean": 24226.0,
          "min": 22637.0,
          "stddev": 1559.4851714588376
        },
        "frame_ms_per_frame": {
          "max": 0.21162525438867957,
          "mean": 0.19872125933704293,
          "min": 0.1818669403899121,
          "stddev": 0.012465769321674303
        },
        "frame_ms_per_second": {
          "max": 960.4803056234019,
          "mean": 958.9358808969384,
          "min": 958.1038540543692,
          "stddev": 1.0931646944299787
        },
        "framegraph_dropped_spans": {
          "max": 452.0,
          "mean": 449.0,
          "min": 447.0,
          "stddev": 2.1602468994692865
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 452.0,
          "mean": 449.0,
          "min": 447.0,
          "stddev": 2.1602468994692865
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 109632576.0,
          "mean": 109632576.0,
          "min": 109632576.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 4843.070018111941,
          "mean": 4543.705561808445,
          "min": 4161.418713228317,
          "stddev": 284.3931758652056
        },
        "gpu_allocated_bytes_per_second": {
          "max": 21926324.734787557,
          "mean": 21926038.02029542,
          "min": 21925750.359665055,
          "stddev": 234.4886162419331
        },
        "gpu_live_bytes": {
          "max": 105193728.0,
          "mean": 105193728.0,
          "min": 105193728.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 105193728.0,
          "mean": 105193728.0,
          "min": 105193728.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 60.0,
          "mean": 60.0,
          "min": 60.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.002650527896806114,
          "mean": 0.0024866909421931917,
          "min": 0.0022774720060732587,
          "stddev": 0.00015564343349838225
        },
        "gpu_resources_created_per_second": {
          "max": 11.999895761705476,
          "mean": 11.999738847855998,
          "min": 11.999581416201542,
          "stddev": 0.00012833153692012354
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.0014807890456202922,
          "mean": 0.0012805573109460836,
          "min": 0.001057217837837881,
          "stddev": 0.00017369248246486868
        },
        "physics_ms_per_second": {
          "max": 6.704066089600817,
          "mean": 6.150849259791135,
          "min": 5.5702864781505195,
          "stddev": 0.4632673113486075
        },
        "physics_wall_ms_per_frame": {
          "max": 0.0014807890467722572,
          "mean": 0.0012805573107486478,
          "min": 0.0010572178365652442,
          "stddev": 0.00017369248343207912
        },
        "physics_wall_ms_per_second": {
          "max": 6.704066094816178,
          "mean": 6.150849258549451,
          "min": 5.570286471445231,
          "stddev": 0.46326731618163086
        },
        "process_peak_rss_bytes": {
          "max": 237035520.0,
          "mean": 232947712.0,
          "min": 229810176.0,
          "stddev": 3025300.1652807943
        },
        "render_ms_per_frame": {
          "max": 0.16987326484363283,
          "mean": 0.1590972173228846,
          "min": 0.14514269052417325,
          "stddev": 0.010343351308381921
        },
        "render_ms_per_second": {
          "max": 769.0775385841165,
          "mean": 767.6159921655892,
          "min": 764.7301601367468,
          "stddev": 2.0406480941074383
        },
        "render_wall_ms_per_frame": {
          "max": 0.17374087413794745,
          "mean": 0.16277957092228093,
          "min": 0.14871517522021926,
          "stddev": 0.010449666304545703
        },
        "render_wall_ms_per_second": {
          "max": 786.5876008002901,
          "mean": 785.4248565990629,
          "min": 783.552925401928,
          "stddev": 1.336543636830858
        },
        "replication_ms_per_frame": {
          "max": 0.0003071084372846411,
          "mean": 0.00028845098217082134,
          "min": 0.0002647826937871281,
          "stddev": 1.7638943244956022e-05
        },
        "replication_ms_per_second": {
          "max": 1.4554166943287703,
          "mean": 1.393038508262358,
          "min": 1.3286074821444471,
          "stddev": 0.0517899905896236
        },
        "replication_wall_ms_per_frame": {
          "max": 0.0003071084372846411,
          "mean": 0.00028845098217082134,
          "min": 0.0002647826937871281,
          "stddev": 1.7638943244956022e-05
        },
        "replication_wall_ms_per_second": {
          "max": 1.4554166943287703,
          "mean": 1.393038508262358,
          "min": 1.3286074821444471,
          "stddev": 0.0517899905896236
        },
        "script_ms_per_frame": {
          "max": 0.00045248804536735356,
          "mean": 0.0003794809121997528,
          "min": 0.000291346691002408,
          "stddev": 6.664960923034712e-05
        },
        "script_ms_per_second": {
          "max": 2.048576581430824,
          "mean": 1.8179047884062638,
          "min": 1.5350521673599236,
          "stddev": 0.21286765852910322
        },
        "submitted_frames": {
          "max": 26345.0,
          "mean": 24226.0,
          "min": 22637.0,
          "stddev": 1559.4851714588376
        },
        "uploaded_bytes": {
          "max": 69628488.0,
          "mean": 64009912.0,
          "min": 59764104.0,
          "stddev": 4142453.935469651
        },
        "uploaded_bytes_per_frame": {
          "max": 2643.3636056718433,
          "mean": 2642.1398525841128,
          "min": 2640.1070813270308,
          "stddev": 1.4473240827689364
        },
        "uploaded_bytes_per_second": {
          "max": 13925211.8440502,
          "mean": 12801695.17524507,
          "min": 11952716.971528756,
          "stddev": 828336.69971449
        }
      },
      "reason": "",
      "status": "measured"
    },
    "RenderFeatures": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 672099358.0,
          "mean": 658872302.3333334,
          "min": 648222885.0,
          "stddev": 9916473.004149353
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 24780.109522535266,
          "mean": 24749.958343535192,
          "min": 24709.535220588234,
          "stddev": 29.71327030316789
        },
        "cpu_allocated_bytes_per_second": {
          "max": 134415626.13621473,
          "mean": 131770350.0886372,
          "min": 129640280.95444167,
          "stddev": 1983273.2244059334
        },
        "cpu_allocations": {
          "max": 6981447.0,
          "mean": 6838264.666666666,
          "min": 6720894.0,
          "stddev": 107924.81350252849
        },
        "cpu_allocations_per_frame": {
          "max": 257.0155059231872,
          "mean": 256.8703603500045,
          "min": 256.6708455882353,
          "stddev": 0.1458645333244882
        },
        "cpu_allocations_per_second": {
          "max": 1396245.300150693,
          "mean": 1367610.273665158,
          "min": 1344134.2578101377,
          "stddev": 21584.74347726664
        },
        "cpu_live_blocks": {
          "max": 16576.0,
          "mean": 16575.666666666664,
          "min": 16575.0,
          "stddev": 0.4714045207910317
        },
        "cpu_live_bytes": {
          "max": 32502007.0,
          "mean": 32501921.66666667,
          "min": 32501775.0,
          "stddev": 104.17079991799791
        },
        "cpu_peak_bytes": {
          "max": 32502007.0,
          "mean": 32501929.66666667,
          "min": 32501775.0,
          "stddev": 109.36584882351934
        },
        "cpu_process_peak_bytes": {
          "max": 32633207.0,
          "mean": 32633129.666666664,
          "min": 32632975.0,
          "stddev": 109.36584882351934
        },
        "cpu_profiler_overhead_bytes": {
          "max": 530432.0,
          "mean": 530421.3333333333,
          "min": 530400.0,
          "stddev": 15.084944665313014
        },
        "draw_calls_per_frame": {
          "max": 26.0,
          "mean": 26.0,
          "min": 26.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.000165691,
          "mean": 5.000155991666667,
          "min": 5.000144361,
          "stddev": 8.814373161025428e-06
        },
        "frame_count": {
          "max": 27200.0,
          "mean": 26621.666666666664,
          "min": 26159.0,
          "stddev": 432.78503773685253
        },
        "frame_ms_per_frame": {
          "max": 0.18244667950675977,
          "mean": 0.17933898809495058,
          "min": 0.1756481301527032,
          "stddev": 0.002805961597688424
        },
        "frame_ms_per_second": {
          "max": 955.4956490828275,
          "mean": 954.5881150814035,
          "min": 953.7757884947904,
          "stddev": 0.7053502437073657
        },
        "framegraph_dropped_spans": {
          "max": 450.0,
          "mean": 442.33333333333337,
          "min": 435.0,
          "stddev": 6.128258770283412
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 450.0,
          "mean": 442.33333333333337,
          "min": 435.0,
          "stddev": 6.128258770283412
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 108986176.0,
          "mean": 108986176.0,
          "min": 108986176.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 4166.297488436102,
          "mean": 4094.965705910674,
          "min": 4006.844705882353,
          "stddev": 66.17001251474801
        },
        "gpu_allocated_bytes_per_second": {
          "max": 21796605.884035595,
          "mean": 21796555.18387359,
          "min": 21796512.902796123,
          "stddev": 38.42341700715578
        },
        "gpu_live_bytes": {
          "max": 104547328.0,
          "mean": 104547328.0,
          "min": 104547328.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 104547328.0,
          "mean": 104547328.0,
          "min": 104547328.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 51.0,
          "mean": 51.0,
          "min": 51.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.0019496158110019497,
          "mean": 0.0019162361564226678,
          "min": 0.001875,
          "stddev": 3.096420814188548e-05
        },
        "gpu_resources_created_per_second": {
          "max": 10.199705512062515,
          "mean": 10.199681786959413,
          "min": 10.19966200156066,
          "stddev": 1.7980209410973494e-05
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.0003932003152781618,
          "mean": 0.00030981197521432217,
          "min": 0.00026030687741962645,
          "stddev": 5.930835824438926e-05
        },
        "physics_ms_per_second": {
          "max": 2.0570772416352376,
          "mean": 1.6452710325414124,
          "min": 1.4160246885893888,
          "stddev": 0.29181406433103335
        },
        "physics_wall_ms_per_frame": {
          "max": 0.0003932003152781618,
          "mean": 0.00030981197521432217,
          "min": 0.00026030687741962645,
          "stddev": 5.930835824438926e-05
        },
        "physics_wall_ms_per_second": {
          "max": 2.0570772416352376,
          "mean": 1.6452710325414124,
          "min": 1.4160246885893888,
          "stddev": 0.29181406433103335
        },
        "process_peak_rss_bytes": {
          "max": 362913792.0,
          "mean": 328052736.0,
          "min": 304476160.0,
          "stddev": 25156097.009070598
        },
        "render_ms_per_frame": {
          "max": 0.15324338129286344,
          "mean": 0.15050925233888912,
          "min": 0.14876603804158922,
          "stddev": 0.0019575638840221414
        },
        "render_ms_per_second": {
          "max": 809.2616867395744,
          "mean": 801.1925230658198,
          "min": 792.6037275076155,
          "stddev": 6.810502400210223
        },
        "render_wall_ms_per_frame": {
          "max": 0.15502208935984,
          "mean": 0.1522632214338071,
          "min": 0.15042840929558118,
          "stddev": 0.001985931092699655
        },
        "render_wall_ms_per_second": {
          "max": 818.3047007413106,
          "mean": 810.5261380713052,
          "min": 802.2560220262568,
          "stddev": 6.561058891426116
        },
        "replication_ms_per_frame": {
          "max": 0.00030086762874760283,
          "mean": 0.0002801433483889154,
          "min": 0.000262096641047666,
          "stddev": 1.5941025056019195e-05
        },
        "replication_ms_per_second": {
          "max": 1.5949134248574068,
          "mean": 1.49075715344699,
          "min": 1.4257606952180486,
          "stddev": 0.07440107397261377
        },
        "replication_wall_ms_per_frame": {
          "max": 0.0003008676286246258,
          "mean": 0.00028014334837759164,
          "min": 0.000262096641047666,
          "stddev": 1.5941024997743342e-05
        },
        "replication_wall_ms_per_second": {
          "max": 1.5949134242054999,
          "mean": 1.4907571533849027,
          "min": 1.4257606952180486,
          "stddev": 0.07440107358671075
        },
        "script_ms_per_frame": {
          "max": 0.00018148721312568787,
          "mean": 0.00014324858974208948,
          "min": 0.0001185815833399401,
          "stddev": 2.7415581400653113e-05
        },
        "script_ms_per_second": {
          "max": 0.9494733377936113,
          "mean": 0.7606534992185809,
          "min": 0.6450634392985694,
          "stddev": 0.1346310933299776
        },
        "submitted_frames": {
          "max": 27200.0,
          "mean": 26621.666666666664,
          "min": 26159.0,
          "stddev": 432.78503773685253
        },
        "uploaded_bytes": {
          "max": 41997152.0,
          "mean": 41104205.33333333,
          "min": 40389848.0,
          "stddev": 668220.0982657004
        },
        "uploaded_bytes_per_frame": {
          "max": 1544.0134561718721,
          "mean": 1544.013225786805,
          "min": 1544.0129411764706,
          "stddev": 0.0002137137503581244
        },
        "uploaded_bytes_per_second": {
          "max": 8399165.115729487,
          "mean": 8220584.639845545,
          "min": 8077701.919498251,
          "stddev": 133642.29356897995
        }
      },
      "reason": "",
      "status": "measured"
    },
    "ReplicationRings": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 2011371769.0,
          "mean": 1917331132.3333335,
          "min": 1782053479.0,
          "stddev": 98054706.46466096
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 247610.5987216896,
          "mean": 247292.61238703592,
          "min": 247066.91671784793,
          "stddev": 231.35212054991294
        },
        "cpu_allocated_bytes_per_second": {
          "max": 402232252.4719196,
          "mean": 383433411.29673463,
          "min": 356370856.2466901,
          "stddev": 19613492.65490527
        },
        "cpu_allocations": {
          "max": 2677135.0,
          "mean": 2567178.3333333335,
          "min": 2400609.0,
          "stddev": 119778.69228799512
        },
        "cpu_allocations_per_frame": {
          "max": 333.5568987077949,
          "mean": 331.1880553712978,
          "min": 328.8459648691807,
          "stddev": 1.9233237192527983
        },
        "cpu_allocations_per_second": {
          "max": 535370.9631495839,
          "mean": 513391.72826537373,
          "min": 480068.1320313567,
          "stddev": 23959.990028504264
        },
        "cpu_live_blocks": {
          "max": 29102.0,
          "mean": 29101.0,
          "min": 29100.0,
          "stddev": 0.816496580927726
        },
        "cpu_live_bytes": {
          "max": 38339716.0,
          "mean": 38339640.0,
          "min": 38339518.0,
          "stddev": 87.13208364316786
        },
        "cpu_peak_bytes": {
          "max": 38343702.0,
          "mean": 38341197.666666664,
          "min": 38339910.0,
          "stddev": 1771.0682902951228
        },
        "cpu_process_peak_bytes": {
          "max": 43133446.0,
          "mean": 43133419.333333336,
          "min": 43133366.0,
          "stddev": 37.712361663282536
        },
        "cpu_profiler_overhead_bytes": {
          "max": 931264.0,
          "mean": 931232.0,
          "min": 931200.0,
          "stddev": 26.127890589687233
        },
        "draw_calls_per_frame": {
          "max": 27.0,
          "mean": 27.0,
          "min": 27.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.000558962,
          "mean": 5.000430996333334,
          "min": 5.000210681,
          "stddev": 0.00015646354088679176
        },
        "frame_count": {
          "max": 8141.0,
          "mean": 7753.666666666666,
          "min": 7197.0,
          "stddev": 403.55861477058767
        },
        "frame_ms_per_frame": {
          "max": 0.666419170234739,
          "mean": 0.6218784994662612,
          "min": 0.5910086733434464,
          "stddev": 0.03226819647209202
        },
        "frame_ms_per_second": {
          "max": 963.7252421091735,
          "mean": 961.680460979374,
          "min": 959.1365294613271,
          "stddev": 1.906293834239967
        },
        "framegraph_dropped_spans": {
          "max": 457.0,
          "mean": 452.0,
          "min": 449.0,
          "stddev": 3.559026084010437
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 457.0,
          "mean": 452.0,
          "min": 449.0,
          "stddev": 3.559026084010437
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 15796736.0,
          "mean": 15796736.0,
          "min": 15796736.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 2194.9056551340836,
          "mean": 2043.0267963743022,
          "min": 1940.392580764034,
          "stddev": 109.58406944903324
        },
        "gpu_allocated_bytes_per_second": {
          "max": 3159214.0827235673,
          "mean": 3159074.893154137,
          "min": 3158994.0484737353,
          "stddev": 98.84959056298364
        },
        "gpu_live_bytes": {
          "max": 199632960.0,
          "mean": 199632960.0,
          "min": 199632960.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 200699200.0,
          "mean": 200699200.0,
          "min": 200699200.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 12.0,
          "mean": 12.0,
          "min": 12.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.0016673614005835765,
          "mean": 0.0015519865342113475,
          "min": 0.0014740203906154035,
          "stddev": 8.324560424307905e-05
        },
        "gpu_resources_created_per_second": {
          "max": 2.399898877380923,
          "mean": 2.3997931419408185,
          "min": 2.3997317282307447,
          "stddev": 7.509115090342916e-05
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.005301083849733433,
          "mean": 0.004387016377198526,
          "min": 0.0037597806496435164,
          "stddev": 0.0006611143250089268
        },
        "physics_ms_per_second": {
          "max": 7.629527170153086,
          "mean": 6.749146718193818,
          "min": 6.121034169999827,
          "stddev": 0.6411537019729565
        },
        "physics_wall_ms_per_frame": {
          "max": 0.005301083849733433,
          "mean": 0.004387016377198526,
          "min": 0.0037597806496435164,
          "stddev": 0.0006611143250089268
        },
        "physics_wall_ms_per_second": {
          "max": 7.629527170153086,
          "mean": 6.749146718193818,
          "min": 6.121034169999827,
          "stddev": 0.6411537019729565
        },
        "process_peak_rss_bytes": {
          "max": 225026048.0,
          "mean": 223673002.6666667,
          "min": 222887936.0,
          "stddev": 960843.5722417163
        },
        "render_ms_per_frame": {
          "max": 0.39602191627390004,
          "mean": 0.3742896484270125,
          "min": 0.35843140568125237,
          "stddev": 0.015898422349832814
        },
        "render_ms_per_second": {
          "max": 583.7667990076011,
          "mean": 579.0913211128185,
          "min": 569.9702279449413,
          "stddev": 6.450269484759785
        },
        "render_wall_ms_per_frame": {
          "max": 0.39970456988211556,
          "mean": 0.37768836281811835,
          "min": 0.36156938735799726,
          "stddev": 0.01611740347118132
        },
        "render_wall_ms_per_second": {
          "max": 589.1154034376257,
          "mean": 584.343837962546,
          "min": 575.2704470243951,
          "stddev": 6.418721641007548
        },
        "replication_ms_per_frame": {
          "max": 0.015946308794544357,
          "mean": 0.013303978984434086,
          "min": 0.011453374690974415,
          "stddev": 0.001917765398415945
        },
        "replication_ms_per_second": {
          "max": 22.950551181669226,
          "mean": 20.47435519853635,
          "min": 18.646432964623283,
          "stddev": 1.815957515476306
        },
        "replication_wall_ms_per_frame": {
          "max": 0.017537779673727973,
          "mean": 0.01459071363369967,
          "min": 0.012528636829922758,
          "stddev": 0.002138579352221448
        },
        "replication_wall_ms_per_second": {
          "max": 25.24105830387771,
          "mean": 22.451741884834533,
          "min": 20.396991549692324,
          "stddev": 2.044657457613907
        },
        "script_ms_per_frame": {
          "max": 0.027085674415157083,
          "mean": 0.023130681105517373,
          "min": 0.020562020074945595,
          "stddev": 0.0028379516531739715
        },
        "script_ms_per_second": {
          "max": 38.98276177667946,
          "mean": 35.637660787822135,
          "min": 33.47557722413883,
          "stddev": 2.3988772505951697
        },
        "submitted_frames": {
          "max": 8141.0,
          "mean": 7753.666666666666,
          "min": 7197.0,
          "stddev": 403.55861477058767
        },
        "uploaded_bytes": {
          "max": 582715656.0,
          "mean": 558137888.0,
          "min": 517342920.0,
          "stddev": 29047625.66213659
        },
        "uploaded_bytes_per_frame": {
          "max": 72492.12268080273,
          "mean": 71984.38463104614,
          "min": 71577.8965729026,
          "stddev": 380.03597609769173
        },
        "uploaded_bytes_per_second": {
          "max": 116530934.00036293,
          "mean": 111618043.28925323,
          "min": 103457018.29162833,
          "stddev": 5810599.524271773
        }
      },
      "reason": "",
      "status": "measured"
    },
    "Rings": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 3764320476.0,
          "mean": 3669822233.666667,
          "min": 3571202376.0,
          "stddev": 78893983.38538234
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 232560.71737431624,
          "mean": 232460.23633482825,
          "min": 232365.46148148147,
          "stddev": 79.81493737341836
        },
        "cpu_allocated_bytes_per_second": {
          "max": 752824410.2117805,
          "mean": 733920552.1246971,
          "min": 714191212.7183968,
          "stddev": 15782734.272974383
        },
        "cpu_allocations": {
          "max": 3359478.0,
          "mean": 3275118.333333334,
          "min": 3186986.0,
          "stddev": 70470.07533382915
        },
        "cpu_allocations_per_frame": {
          "max": 207.54011461318052,
          "mean": 207.4581316737988,
          "min": 207.3751851851852,
          "stddev": 0.06733560412866578
        },
        "cpu_allocations_per_second": {
          "max": 671860.1830248239,
          "mean": 654984.4931920383,
          "min": 637353.2375406754,
          "stddev": 14097.528895546437
        },
        "cpu_live_blocks": {
          "max": 24596.0,
          "mean": 24595.0,
          "min": 24594.0,
          "stddev": 0.816496580927726
        },
        "cpu_live_bytes": {
          "max": 34768654.0,
          "mean": 34768235.333333336,
          "min": 34767806.0,
          "stddev": 346.27670374363265
        },
        "cpu_peak_bytes": {
          "max": 34768774.0,
          "mean": 34768323.333333336,
          "min": 34767806.0,
          "stddev": 397.9860410729111
        },
        "cpu_process_peak_bytes": {
          "max": 34899318.0,
          "mean": 34899163.333333336,
          "min": 34898854.0,
          "stddev": 218.73169764703871
        },
        "cpu_profiler_overhead_bytes": {
          "max": 787072.0,
          "mean": 787040.0,
          "min": 787008.0,
          "stddev": 26.127890589687233
        },
        "draw_calls_per_frame": {
          "max": 16.0,
          "mean": 16.0,
          "min": 16.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.000344883,
          "mean": 5.000299760333333,
          "min": 5.000263574,
          "stddev": 3.3790352768706275e-05
        },
        "frame_count": {
          "max": 16200.0,
          "mean": 15787.0,
          "min": 15356.0,
          "stddev": 344.7965583741617
        },
        "frame_ms_per_frame": {
          "max": 0.31392220622178446,
          "mean": 0.30539043884785305,
          "min": 0.2978499740840476,
          "stddev": 0.006598796462030389
        },
        "frame_ms_per_second": {
          "max": 964.9830471439807,
          "mean": 963.7280454359764,
          "min": 962.1497064022122,
          "stddev": 1.1790858632611243
        },
        "framegraph_dropped_spans": {
          "max": 458.0,
          "mean": 456.66666666666663,
          "min": 456.0,
          "stddev": 0.9428090415820634
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 458.0,
          "mean": 456.66666666666663,
          "min": 456.0,
          "stddev": 0.9428090415820634
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 104883264.0,
          "mean": 104883264.0,
          "min": 104883264.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 6830.116176087523,
          "mean": 6646.824323918989,
          "min": 6474.275555555556,
          "stddev": 145.46980715914077
        },
        "gpu_allocated_bytes_per_second": {
          "max": 20975547.07823088,
          "mean": 20975395.282661572,
          "min": 20975206.00160571,
          "stddev": 141.74434106586696
        },
        "gpu_live_bytes": {
          "max": 100444416.0,
          "mean": 100444416.0,
          "min": 100444416.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 100444416.0,
          "mean": 100444416.0,
          "min": 100444416.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 43.0,
          "mean": 43.0,
          "min": 43.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.0028002083876009377,
          "mean": 0.0027250624649564348,
          "min": 0.002654320987654321,
          "stddev": 5.963965526323674e-05
        },
        "gpu_resources_created_per_second": {
          "max": 8.599546676616852,
          "mean": 8.59948444352807,
          "min": 8.59940684215401,
          "stddev": 5.811229011576677e-05
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.001566990953560809,
          "mean": 0.001515682010998761,
          "min": 0.001461038447594569,
          "stddev": 4.331915013210803e-05
        },
        "physics_ms_per_second": {
          "max": 4.812210686644308,
          "mean": 4.78235267842258,
          "min": 4.733515043907567,
          "stddev": 0.03481781830338914
        },
        "physics_wall_ms_per_frame": {
          "max": 0.001566990953560809,
          "mean": 0.001515682010998761,
          "min": 0.001461038447594569,
          "stddev": 4.331915013210803e-05
        },
        "physics_wall_ms_per_second": {
          "max": 4.812210686644308,
          "mean": 4.78235267842258,
          "min": 4.733515043907567,
          "stddev": 0.03481781830338914
        },
        "process_peak_rss_bytes": {
          "max": 213909504.0,
          "mean": 212930560.0,
          "min": 211513344.0,
          "stddev": 1026144.0593756804
        },
        "render_ms_per_frame": {
          "max": 0.21575072033762133,
          "mean": 0.2102209370474597,
          "min": 0.2053410834632814,
          "stddev": 0.004274493453883867
        },
        "render_ms_per_second": {
          "max": 665.2700408438826,
          "mean": 663.4177921362466,
          "min": 662.4154249456982,
          "stddev": 1.3112162105864875
        },
        "render_wall_ms_per_frame": {
          "max": 0.21746265500765483,
          "mean": 0.2119198249953227,
          "min": 0.206962995307727,
          "stddev": 0.004306449484127694
        },
        "render_wall_ms_per_second": {
          "max": 670.5247582185109,
          "mean": 668.7791221318677,
          "min": 667.8252417449397,
          "stddev": 1.2361243502157218
        },
        "replication_ms_per_frame": {
          "max": 0.0002851738030846946,
          "mean": 0.0002706502811450058,
          "min": 0.00025003939487591937,
          "stddev": 1.4975505854657699e-05
        },
        "replication_ms_per_second": {
          "max": 0.901381962849127,
          "mean": 0.8537749784149584,
          "min": 0.8100849359325981,
          "stddev": 0.03737462246533714
        },
        "replication_wall_ms_per_frame": {
          "max": 0.0002851738030846946,
          "mean": 0.0002706502811450058,
          "min": 0.00025003939487591937,
          "stddev": 1.4975505854657699e-05
        },
        "replication_wall_ms_per_second": {
          "max": 0.901381962849127,
          "mean": 0.8537749784149584,
          "min": 0.8100849359325981,
          "stddev": 0.03737462246533714
        },
        "script_ms_per_frame": {
          "max": 0.011135183435238708,
          "mean": 0.01083871891921247,
          "min": 0.010487976428091236,
          "stddev": 0.0002669941340935002
        },
        "script_ms_per_second": {
          "max": 34.43076052460614,
          "mean": 34.20200986115669,
          "min": 33.979252417520264,
          "stddev": 0.18437612228605552
        },
        "submitted_frames": {
          "max": 16200.0,
          "mean": 15787.0,
          "min": 15356.0,
          "stddev": 344.7965583741617
        },
        "uploaded_bytes": {
          "max": 663566940.0,
          "mean": 646650460.0,
          "min": 628996700.0,
          "stddev": 14122867.031005662
        },
        "uploaded_bytes_per_frame": {
          "max": 40960.972909611875,
          "mean": 40960.946800772705,
          "min": 40960.92222222222,
          "stddev": 0.020721312780606356
        },
        "uploaded_bytes_per_second": {
          "max": 132706392.40906544,
          "mean": 129322357.73304166,
          "min": 125790663.38772777,
          "stddev": 2825264.5312836557
        }
      },
      "reason": "",
      "status": "measured"
    },
    "ServerPhysics": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 735547750.0,
          "mean": 734747175.3333334,
          "min": 733146026.0,
          "stddev": 1132183.5512923198
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 14098962.038461538,
          "mean": 13951826.968795357,
          "min": 13878259.433962265,
          "stddev": 104040.2055113124
        },
        "cpu_allocated_bytes_per_second": {
          "max": 145080580.06873074,
          "mean": 144946704.31047052,
          "min": 144794455.6581154,
          "stddev": 117530.04442996347
        },
        "cpu_allocations": {
          "max": 1378072.0,
          "mean": 1344726.0,
          "min": 1278034.0,
          "stddev": 47158.36545089323
        },
        "cpu_allocations_per_frame": {
          "max": 26001.35849056604,
          "mean": 25526.764634736333,
          "min": 24577.576923076922,
          "stddev": 671.1770675333112
        },
        "cpu_allocations_per_second": {
          "max": 271813.06058848783,
          "mean": 265265.3062268592,
          "min": 252705.86064672962,
          "stddev": 8883.565195855852
        },
        "cpu_live_blocks": {
          "max": 212832.0,
          "mean": 212832.0,
          "min": 212832.0,
          "stddev": 0.0
        },
        "cpu_live_bytes": {
          "max": 434408483.0,
          "mean": 434408483.0,
          "min": 434408483.0,
          "stddev": 0.0
        },
        "cpu_peak_bytes": {
          "max": 434408483.0,
          "mean": 434408483.0,
          "min": 434408483.0,
          "stddev": 0.0
        },
        "cpu_process_peak_bytes": {
          "max": 445252283.0,
          "mean": 445252283.0,
          "min": 445252283.0,
          "stddev": 0.0
        },
        "cpu_profiler_overhead_bytes": {
          "max": 6810624.0,
          "mean": 6810624.0,
          "min": 6810624.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.079944164,
          "mean": 5.069089111,
          "min": 5.057397548,
          "stddev": 0.009223603236549706
        },
        "frame_count": {
          "max": 53.0,
          "mean": 52.66666666666667,
          "min": 52.0,
          "stddev": 0.4714045207910317
        },
        "frame_ms_per_frame": {
          "max": 96.80046230769231,
          "mean": 95.66265622206096,
          "min": 94.74019111320757,
          "stddev": 0.854774103068819
        },
        "frame_ms_per_second": {
          "max": 995.8195493268419,
          "mean": 993.8380118245042,
          "min": 990.3952255634089,
          "stddev": 2.4436663743732585
        },
        "framegraph_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 92.83435017364816,
          "mean": 91.79608725172606,
          "min": 90.93049663874619,
          "stddev": 0.78677670076342
        },
        "physics_ms_per_second": {
          "max": 955.9240879812836,
          "mean": 953.6711143718812,
          "min": 950.5694327923845,
          "stddev": 2.2669086089323573
        },
        "physics_wall_ms_per_frame": {
          "max": 92.83435043040663,
          "mean": 91.79608758322824,
          "min": 90.93049645880764,
          "stddev": 0.7867768125601308
        },
        "physics_wall_ms_per_second": {
          "max": 955.9240975556783,
          "mean": 953.6711178163263,
          "min": 950.5694309113425,
          "stddev": 2.2669129681480618
        },
        "process_peak_rss_bytes": {
          "max": 457814016.0,
          "mean": 457286997.3333334,
          "min": 456794112.0,
          "stddev": 417073.0196958588
        },
        "render_ms_per_frame": {
          "max": 0.0017323493957519531,
          "mean": 0.0015913315539090138,
          "min": 0.0015122395641398879,
          "stddev": 9.996063511660911e-05
        },
        "render_ms_per_second": {
          "max": 0.017811961136953824,
          "mean": 0.01652572933001695,
          "min": 0.01580865339866769,
          "stddev": 0.0009115058169886441
        },
        "render_wall_ms_per_frame": {
          "max": 0.0017323493957519531,
          "mean": 0.0015913315539090138,
          "min": 0.0015122395641398879,
          "stddev": 9.996063511660911e-05
        },
        "render_wall_ms_per_second": {
          "max": 0.017811961136953824,
          "mean": 0.01652572933001695,
          "min": 0.01580865339866769,
          "stddev": 0.0009115058169886441
        },
        "replication_ms_per_frame": {
          "max": 0.00045355310979879125,
          "mean": 0.0003850315870959573,
          "min": 0.0003488288735443691,
          "stddev": 4.8477970828573196e-05
        },
        "replication_ms_per_second": {
          "max": 0.004732003747145112,
          "mean": 0.004001724444314419,
          "min": 0.0036265815133275734,
          "stddev": 0.0005164500367633815
        },
        "replication_wall_ms_per_frame": {
          "max": 0.00045355310979879125,
          "mean": 0.0003850315870959573,
          "min": 0.0003488288735443691,
          "stddev": 4.8477970828573196e-05
        },
        "replication_wall_ms_per_second": {
          "max": 0.004732003747145112,
          "mean": 0.004001724444314419,
          "min": 0.0036265815133275734,
          "stddev": 0.0005164500367633815
        },
        "script_ms_per_frame": {
          "max": 0.05555462847538505,
          "mean": 0.05366657802601374,
          "min": 0.05234502362536831,
          "stddev": 0.001370177398059411
        },
        "script_ms_per_second": {
          "max": 0.5712109149620727,
          "mean": 0.5574728165391795,
          "min": 0.5472045271538551,
          "stddev": 0.010103014235614017
        },
        "tick_count": {
          "max": 53.0,
          "mean": 52.66666666666667,
          "min": 52.0,
          "stddev": 0.4714045207910317
        },
        "tick_overruns": {
          "max": 46.0,
          "mean": 44.66666666666667,
          "min": 42.0,
          "stddev": 1.8856180831641267
        },
        "tick_rate_hz": {
          "max": 30.0,
          "mean": 30.0,
          "min": 30.0,
          "stddev": 0.0
        }
      },
      "reason": "",
      "status": "measured"
    },
    "ServerRings": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "clients_admitted": {
          "max": 1.0,
          "mean": 1.0,
          "min": 1.0,
          "stddev": 0.0
        },
        "cpu_allocated_bytes": {
          "max": 20814262.0,
          "mean": 20800038.666666664,
          "min": 20773600.0,
          "stddev": 18712.924897573394
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 137842.79470198674,
          "mean": 137748.6004415011,
          "min": 137573.50993377483,
          "stddev": 123.92665495081903
        },
        "cpu_allocated_bytes_per_second": {
          "max": 4161232.8132347334,
          "mean": 4158494.8032304114,
          "min": 4153253.7070691315,
          "stddev": 3707.255411788518
        },
        "cpu_allocations": {
          "max": 132195.0,
          "mean": 131911.66666666666,
          "min": 131385.0,
          "stddev": 372.76742823851384
        },
        "cpu_allocations_per_frame": {
          "max": 875.4635761589404,
          "mean": 873.5871964679914,
          "min": 870.0993377483444,
          "stddev": 2.468658465155731
        },
        "cpu_allocations_per_second": {
          "max": 26428.713722617962,
          "mean": 26372.73806663225,
          "min": 26267.726263299468,
          "stddev": 74.30858328366645
        },
        "cpu_live_blocks": {
          "max": 16362.0,
          "mean": 16362.0,
          "min": 16362.0,
          "stddev": 0.0
        },
        "cpu_live_bytes": {
          "max": 18945597.0,
          "mean": 18945597.0,
          "min": 18945597.0,
          "stddev": 0.0
        },
        "cpu_peak_bytes": {
          "max": 18946966.0,
          "mean": 18946966.0,
          "min": 18946966.0,
          "stddev": 0.0
        },
        "cpu_process_peak_bytes": {
          "max": 18967941.0,
          "mean": 18967941.0,
          "min": 18967941.0,
          "stddev": 0.0
        },
        "cpu_profiler_overhead_bytes": {
          "max": 523584.0,
          "mean": 523584.0,
          "min": 523584.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.001946042,
          "mean": 5.001819048,
          "min": 5.001745868,
          "stddev": 9.014568769870293e-05
        },
        "frame_count": {
          "max": 151.0,
          "mean": 151.0,
          "min": 151.0,
          "stddev": 0.0
        },
        "frame_ms_per_frame": {
          "max": 1.8464779602649,
          "mean": 1.7883515761589401,
          "min": 1.7163315827814576,
          "stddev": 0.05403267022186963
        },
        "frame_ms_per_second": {
          "max": 55.74395417535902,
          "mean": 53.98856819528529,
          "min": 51.81512132755164,
          "stddev": 1.6309533134225154
        },
        "framegraph_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.06566243782170758,
          "mean": 0.06298112134689725,
          "min": 0.0592350172841095,
          "stddev": 0.0027298672919414298
        },
        "physics_ms_per_second": {
          "max": 1.9822341200452807,
          "mean": 1.901337018920056,
          "min": 1.7882731042225222,
          "stddev": 0.08238627246613303
        },
        "physics_wall_ms_per_frame": {
          "max": 0.06566243782170758,
          "mean": 0.06298112134689725,
          "min": 0.0592350172841095,
          "stddev": 0.0027298672919414298
        },
        "physics_wall_ms_per_second": {
          "max": 1.9822341200452807,
          "mean": 1.901337018920056,
          "min": 1.7882731042225222,
          "stddev": 0.08238627246613303
        },
        "process_peak_rss_bytes": {
          "max": 33492992.0,
          "mean": 32967338.666666664,
          "min": 32415744.0,
          "stddev": 440167.0349290394
        },
        "render_ms_per_frame": {
          "max": 0.001672955538263384,
          "mean": 0.0015041987364392143,
          "min": 0.0013836961708321477,
          "stddev": 0.00012292029638849103
        },
        "render_ms_per_second": {
          "max": 0.050505426476354094,
          "mean": 0.04541029401667467,
          "min": 0.041773038316958784,
          "stddev": 0.003711037970435241
        },
        "render_wall_ms_per_frame": {
          "max": 0.001672955538263384,
          "mean": 0.0015041987364392143,
          "min": 0.0013836961708321477,
          "stddev": 0.00012292029638849103
        },
        "render_wall_ms_per_second": {
          "max": 0.050505426476354094,
          "mean": 0.04541029401667467,
          "min": 0.041773038316958784,
          "stddev": 0.003711037970435241
        },
        "replication_ms_per_frame": {
          "max": 0.747564720396964,
          "mean": 0.7099452780571995,
          "min": 0.6735915971907559,
          "stddev": 0.030212661289606358
        },
        "replication_ms_per_second": {
          "max": 22.568486823934283,
          "mean": 21.43254908082187,
          "min": 20.335365662325195,
          "stddev": 0.9120796580211243
        },
        "replication_wall_ms_per_frame": {
          "max": 0.7548526233395204,
          "mean": 0.7167447823825525,
          "min": 0.679912524902268,
          "stddev": 0.030607460129307946
        },
        "replication_wall_ms_per_second": {
          "max": 22.788503816504313,
          "mean": 21.637819417115917,
          "min": 20.526191048025964,
          "stddev": 0.9239981225565514
        },
        "script_ms_per_frame": {
          "max": 0.6804568314151368,
          "mean": 0.672108141231019,
          "min": 0.6586488132334788,
          "stddev": 0.009608550997171538
        },
        "script_ms_per_second": {
          "max": 20.541801267132833,
          "mean": 20.290280510331723,
          "min": 19.88425110410973,
          "stddev": 0.2898249428906203
        },
        "tick_count": {
          "max": 151.0,
          "mean": 151.0,
          "min": 151.0,
          "stddev": 0.0
        },
        "tick_overruns": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "tick_rate_hz": {
          "max": 30.0,
          "mean": 30.0,
          "min": 30.0,
          "stddev": 0.0
        }
      },
      "reason": "",
      "status": "measured"
    },
    "StressPhysics": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 2463518102.0,
          "mean": 2450652952.6666665,
          "min": 2424922654.0,
          "stddev": 18194068.66915518
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 51594099.02127659,
          "mean": 51413562.20153664,
          "min": 51323293.791666664,
          "stddev": 127658.8094919739
        },
        "cpu_allocated_bytes_per_second": {
          "max": 484660982.070234,
          "mean": 483993255.77568877,
          "min": 483114925.3167466,
          "stddev": 648505.182090337
        },
        "cpu_allocations": {
          "max": 148087.0,
          "mean": 148004.6666666667,
          "min": 147840.0,
          "stddev": 116.43691663538482
        },
        "cpu_allocations_per_frame": {
          "max": 3145.531914893617,
          "mean": 3105.2745271867616,
          "min": 3085.1458333333335,
          "stddev": 28.46627184037355
        },
        "cpu_allocations_per_second": {
          "max": 29454.01596253462,
          "mean": 29231.473095826437,
          "min": 29106.46239974229,
          "stddev": 157.76092245839945
        },
        "cpu_live_blocks": {
          "max": 223100.0,
          "mean": 223100.0,
          "min": 223100.0,
          "stddev": 0.0
        },
        "cpu_live_bytes": {
          "max": 587858939.0,
          "mean": 587858939.0,
          "min": 587858939.0,
          "stddev": 0.0
        },
        "cpu_peak_bytes": {
          "max": 587858939.0,
          "mean": 587858939.0,
          "min": 587858939.0,
          "stddev": 0.0
        },
        "cpu_process_peak_bytes": {
          "max": 587932763.0,
          "mean": 587932763.0,
          "min": 587932763.0,
          "stddev": 0.0
        },
        "cpu_profiler_overhead_bytes": {
          "max": 7139200.0,
          "mean": 7139200.0,
          "min": 7139200.0,
          "stddev": 0.0
        },
        "draw_calls_per_frame": {
          "max": 16.0,
          "mean": 16.0,
          "min": 16.0,
          "stddev": 0.0
        },
        "duration_seconds": {
          "max": 5.087770474,
          "mean": 5.063363919666666,
          "min": 5.01934949,
          "stddev": 0.031184497689265706
        },
        "frame_count": {
          "max": 48.0,
          "mean": 47.666666666666664,
          "min": 47.0,
          "stddev": 0.4714045207910317
        },
        "frame_ms_per_frame": {
          "max": 106.72434543041473,
          "mean": 106.15866965210466,
          "min": 105.8249626159668,
          "stddev": 0.40214382563106993
        },
        "frame_ms_per_second": {
          "max": 999.3535812316909,
          "mean": 999.3437914912095,
          "min": 999.336295858082,
          "stddev": 0.007240774894658107
        },
        "framegraph_dropped_spans": {
          "max": 478.0,
          "mean": 474.66666666666663,
          "min": 470.0,
          "stddev": 3.39934634239519
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 478.0,
          "mean": 474.66666666666663,
          "min": 470.0,
          "stddev": 3.39934634239519
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 133084224.0,
          "mean": 133084224.0,
          "min": 133084224.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 2831579.234042553,
          "mean": 2792251.744680851,
          "min": 2772588.0,
          "stddev": 27808.7344147014
        },
        "gpu_allocated_bytes_per_second": {
          "max": 26514237.405692186,
          "mean": 26284757.73620467,
          "min": 26157670.571048643,
          "stddev": 162579.5099828275
        },
        "gpu_live_bytes": {
          "max": 128645376.0,
          "mean": 128645376.0,
          "min": 128645376.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 128645376.0,
          "mean": 128645376.0,
          "min": 128645376.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 43.0,
          "mean": 43.0,
          "min": 43.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.9148936170212766,
          "mean": 0.9021867612293144,
          "min": 0.8958333333333334,
          "stddev": 0.008985103898055988
        },
        "gpu_resources_created_per_second": {
          "max": 8.566847175250194,
          "mean": 8.492701453906367,
          "min": 8.45163912557428,
          "stddev": 0.05253003488423733
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 25.76427843235433,
          "mean": 25.658041222122264,
          "min": 25.519413833195966,
          "stddev": 0.1025557108344199
        },
        "physics_ms_per_second": {
          "max": 243.07019569236328,
          "mean": 241.5388846480358,
          "min": 240.55911592607097,
          "stddev": 1.09682256182379
        },
        "physics_wall_ms_per_frame": {
          "max": 25.764278472090762,
          "mean": 25.65804123536774,
          "min": 25.519413833195966,
          "stddev": 0.10255572455537795
        },
        "physics_wall_ms_per_second": {
          "max": 243.07019606725217,
          "mean": 241.53888477299878,
          "min": 240.55911592607097,
          "stddev": 1.0968227362888037
        },
        "process_peak_rss_bytes": {
          "max": 764035072.0,
          "mean": 763853482.6666667,
          "min": 763703296.0,
          "stddev": 137255.05248583344
        },
        "render_ms_per_frame": {
          "max": 48.6277981599172,
          "mean": 48.5181191333924,
          "min": 48.374380350112915,
          "stddev": 0.1062236535307841
        },
        "render_ms_per_second": {
          "max": 459.20662278158983,
          "mean": 456.7401317021539,
          "min": 454.63110556123365,
          "stddev": 1.8849713251833078
        },
        "render_wall_ms_per_frame": {
          "max": 48.644301970799766,
          "mean": 48.53433811523672,
          "min": 48.39087390899658,
          "stddev": 0.10613874802648933
        },
        "render_wall_ms_per_second": {
          "max": 459.36247312944,
          "mean": 456.89282821506333,
          "min": 454.7777381203932,
          "stddev": 1.8884262611899223
        },
        "replication_ms_per_frame": {
          "max": 0.0017683150920462102,
          "mean": 0.0016893294967939949,
          "min": 0.0015658934911092122,
          "stddev": 8.841382008182545e-05
        },
        "replication_ms_per_second": {
          "max": 0.01655808376996914,
          "mean": 0.015901308238137596,
          "min": 0.014773246544305918,
          "stddev": 0.0008012465144585262
        },
        "replication_wall_ms_per_frame": {
          "max": 0.0017683150920462102,
          "mean": 0.0016893294967939949,
          "min": 0.0015658934911092122,
          "stddev": 8.841382008182545e-05
        },
        "replication_wall_ms_per_second": {
          "max": 0.01655808376996914,
          "mean": 0.015901308238137596,
          "min": 0.014773246544305918,
          "stddev": 0.0008012465144585262
        },
        "script_ms_per_frame": {
          "max": 0.0465679229700788,
          "mean": 0.045796636778119355,
          "min": 0.04431679090612306,
          "stddev": 0.001046722346971042
        },
        "script_ms_per_second": {
          "max": 0.4391622696366935,
          "mean": 0.43110502227295977,
          "min": 0.4181017941679078,
          "stddev": 0.009281987964617208
        },
        "submitted_frames": {
          "max": 48.0,
          "mean": 47.666666666666664,
          "min": 47.0,
          "stddev": 0.4714045207910317
        },
        "uploaded_bytes": {
          "max": 378818968.0,
          "mean": 376152168.0,
          "min": 370818568.0,
          "stddev": 3771424.7281365697
        },
        "uploaded_bytes_per_frame": {
          "max": 7892061.833333333,
          "mean": 7891293.47754137,
          "min": 7889756.765957447,
          "stddev": 1086.6191817207005
        },
        "uploaded_bytes_per_second": {
          "max": 74527064.73261082,
          "mean": 74287217.02916169,
          "min": 73877813.99537493,
          "stddev": 290910.5102773768
        }
      },
      "reason": "",
      "status": "measured"
    },
    "Terrain": {
      "basis": {
        "cpu_peak_basis": "frame_end_samples",
        "frame_basis": "update_iteration_or_server_tick",
        "gpu_peak_basis": "process_lifetime",
        "process_peak_rss_basis": "whole_process_lifetime",
        "replication_timing_basis": "network_category_cpu_self"
      },
      "metrics": {
        "cpu_allocated_bytes": {
          "max": 845758645.0,
          "mean": 839193915.6666666,
          "min": 829579798.0,
          "stddev": 6948038.066867238
        },
        "cpu_allocated_bytes_per_frame": {
          "max": 49180.68520275077,
          "mean": 49005.68000402984,
          "min": 48803.15320253895,
          "stddev": 155.35053215283625
        },
        "cpu_allocated_bytes_per_second": {
          "max": 169149681.3754471,
          "mean": 167835606.1435628,
          "min": 165915305.62823135,
          "stddev": 1388267.130697415
        },
        "cpu_allocations": {
          "max": 4025822.0,
          "mean": 3981804.0,
          "min": 3924628.0,
          "stddev": 42347.027695774195
        },
        "cpu_allocations_per_frame": {
          "max": 232.6670618923405,
          "mean": 232.51564429026445,
          "min": 232.30363531448356,
          "stddev": 0.15443057835973353
        },
        "cpu_allocations_per_second": {
          "max": 805154.6532808603,
          "mean": 796345.7220254054,
          "min": 784922.5061494497,
          "stddev": 8464.07338675266
        },
        "cpu_live_blocks": {
          "max": 44855.0,
          "mean": 44852.0,
          "min": 44846.0,
          "stddev": 4.242640687119285
        },
        "cpu_live_bytes": {
          "max": 96355156.0,
          "mean": 96254746.33333333,
          "min": 96053932.0,
          "stddev": 141997.1768741274
        },
        "cpu_peak_bytes": {
          "max": 96355156.0,
          "mean": 96254746.33333333,
          "min": 96053932.0,
          "stddev": 141997.1768741274
        },
        "cpu_process_peak_bytes": {
          "max": 96389340.0,
          "mean": 96371327.66666666,
          "min": 96335308.0,
          "stddev": 25469.750637875426
        },
        "cpu_profiler_overhead_bytes": {
          "max": 1435360.0,
          "mean": 1435264.0,
          "min": 1435072.0,
          "stddev": 135.7645019878171
        },
        "draw_calls_per_frame": {
          "max": 451.8914245793794,
          "mean": 450.2895661905852,
          "min": 448.6327755337565,
          "stddev": 1.3309048466333862
        },
        "duration_seconds": {
          "max": 5.000202722,
          "mean": 5.0000943190000005,
          "min": 5.000019708,
          "stddev": 7.84429962755218e-05
        },
        "frame_count": {
          "max": 17330.0,
          "mean": 17125.0,
          "min": 16868.0,
          "stddev": 192.16139050287913
        },
        "frame_ms_per_frame": {
          "max": 0.285587863429925,
          "mean": 0.28121559488829273,
          "min": 0.2780825323789056,
          "stddev": 0.0031868712178889256
        },
        "frame_ms_per_second": {
          "max": 963.82238976973,
          "mean": 963.0232330939893,
          "min": 961.7918910009208,
          "stddev": 0.8834853285886435
        },
        "framegraph_dropped_spans": {
          "max": 455.0,
          "mean": 452.33333333333337,
          "min": 451.0,
          "stddev": 1.8856180831641267
        },
        "framegraph_off_thread_dropped_spans": {
          "max": 455.0,
          "mean": 452.33333333333337,
          "min": 451.0,
          "stddev": 1.8856180831641267
        },
        "framegraph_owner_dropped_spans": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes": {
          "max": 219875712.0,
          "mean": 219875712.0,
          "min": 219875712.0,
          "stddev": 0.0
        },
        "gpu_allocated_bytes_per_frame": {
          "max": 13035.078966089637,
          "mean": 12841.081603241846,
          "min": 12687.577149451818,
          "stddev": 144.72764054046795
        },
        "gpu_allocated_bytes_per_second": {
          "max": 43974969.06826192,
          "mean": 43974312.888179734,
          "min": 43973359.52652201,
          "stddev": 689.8762303270827
        },
        "gpu_live_bytes": {
          "max": 120495936.0,
          "mean": 120495936.0,
          "min": 120495936.0,
          "stddev": 0.0
        },
        "gpu_peak_bytes": {
          "max": 131676144.0,
          "mean": 131676144.0,
          "min": 131676144.0,
          "stddev": 0.0
        },
        "gpu_resources_created": {
          "max": 115.0,
          "mean": 115.0,
          "min": 115.0,
          "stddev": 0.0
        },
        "gpu_resources_created_per_frame": {
          "max": 0.0068176428740811005,
          "mean": 0.006716177839473295,
          "min": 0.006635891517599538,
          "stddev": 7.569584885370986e-05
        },
        "gpu_resources_created_per_second": {
          "max": 22.99990934355733,
          "mean": 22.999566146444902,
          "min": 22.999067516606978,
          "stddev": 0.00036082096456222754
        },
        "imagegraph_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_frame": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "imagegraph_wall_ms_per_second": {
          "max": 0.0,
          "mean": 0.0,
          "min": 0.0,
          "stddev": 0.0
        },
        "physics_ms_per_frame": {
          "max": 0.0006790400814761482,
          "mean": 0.000561728305497206,
          "min": 0.000493172830238345,
          "stddev": 8.334483153583233e-05
        },
        "physics_ms_per_second": {
          "max": 2.2908005894483305,
          "mean": 1.9207698350890028,
          "min": 1.7093163376481102,
          "stddev": 0.26254021099220465
        },
        "physics_wall_ms_per_frame": {
          "max": 0.0006790400814761482,
          "mean": 0.000561728305497206,
          "min": 0.000493172830238345,
          "stddev": 8.334483153583233e-05
        },
        "physics_wall_ms_per_second": {
          "max": 2.2908005894483305,
          "mean": 1.9207698350890028,
          "min": 1.7093163376481102,
          "stddev": 0.26254021099220465
        },
        "process_peak_rss_bytes": {
          "max": 290377728.0,
          "mean": 286861994.6666667,
          "min": 284332032.0,
          "stddev": 2564685.5410295865
        },
        "render_ms_per_frame": {
          "max": 0.17219214538368027,
          "mean": 0.170727904225148,
          "min": 0.16993227414292403,
          "stddev": 0.0010366725888536804
        },
        "render_ms_per_second": {
          "max": 588.9781323634912,
          "mean": 584.6937580199817,
          "min": 580.9051319707156,
          "stddev": 3.314378635971115
        },
        "render_wall_ms_per_frame": {
          "max": 0.1738736084123505,
          "mean": 0.17239341824932122,
          "min": 0.17160432252874927,
          "stddev": 0.0010474169025669656
        },
        "render_wall_ms_per_second": {
          "max": 594.7733819149475,
          "mean": 590.3977745354206,
          "min": 586.5776932852698,
          "stddev": 3.368855887782697
        },
        "replication_ms_per_frame": {
          "max": 0.00027604679028192,
          "mean": 0.00026886015517078144,
          "min": 0.00026398522946478414,
          "stddev": 5.188340300888173e-06
        },
        "replication_ms_per_second": {
          "max": 0.9482926953761496,
          "mean": 0.9208262389919555,
          "min": 0.8992242922524935,
          "stddev": 0.0204568065495126
        },
        "replication_wall_ms_per_frame": {
          "max": 0.00027604679022770083,
          "mean": 0.0002688601551345496,
          "min": 0.0002639852294379139,
          "stddev": 5.188340288370003e-06
        },
        "replication_wall_ms_per_second": {
          "max": 0.9482926951898926,
          "mean": 0.9208262388677821,
          "min": 0.8992242921593615,
          "stddev": 0.020456806507834213
        },
        "script_ms_per_frame": {
          "max": 0.07561332752570255,
          "mean": 0.07334055089441119,
          "min": 0.0720315880407477,
          "stddev": 0.001613261124344129
        },
        "script_ms_per_second": {
          "max": 255.08811628539095,
          "mean": 251.12651341193654,
          "min": 248.6329620167322,
          "stddev": 2.8323883343849574
        },
        "submitted_frames": {
          "max": 17330.0,
          "mean": 17125.0,
          "min": 16868.0,
          "stddev": 192.16139050287913
        },
        "uploaded_bytes": {
          "max": 24780.0,
          "mean": 24726.66666666667,
          "min": 24700.0,
          "stddev": 37.712361663282536
        },
        "uploaded_bytes_per_frame": {
          "max": 1.4643111216504625,
          "mean": 1.4440569542793598,
          "min": 1.4298903635314484,
          "stddev": 0.01469673635492679
        },
        "uploaded_bytes_per_second": {
          "max": 4955.940006363848,
          "mean": 4945.2400845736265,
          "min": 4939.799718784282,
          "stddev": 7.566347325825674
        }
      },
      "reason": "",
      "status": "measured"
    }
  }
}
```
<!-- VERSION_IMPACT_BASELINE_END -->

</details>

## v0.24.1 vs v0.25

Ceiling = baseline mean + max(15.000% of baseline, absolute floor). Floors: 1048576.000 bytes (memory), 1024.000 bytes/frame (churn), 0.100 ms/frame, 65536.000 bytes/second, 5.000 ms/second, 1000.000 allocations/second; 100.000 allocations or resources. These are absolute increments in each metric row's unit, including bytes/frame and allocations/frame for normalized churn. A zero baseline uses the absolute floor. Three-run population standard deviation and range follow below.

| Demo | Metric | Baseline mean | Current mean | Impact | Ceiling | Result |
|---|---|---:|---:|---:|---:|---|
| Cube | Demo input SHA-256 changed: 3d198ca251f42f7c88ed2f103e3799813b909a96fc3ee9d7ff68c598bf826389 to ad22a15fe8be6b1d230ec10771d90ee51a616d6e5d5491b63bfe513348c8ca57 | | | | | Input warning |
| Cube | cpu_allocated_bytes | 2151880285.000 | 1102796768.667 | -1049083516.333 (-48.752%) | Diagnostic | Diagnostic |
| Cube | cpu_allocated_bytes_per_frame | 7283550.954 | 4860048.024 | -2423502.930 (-33.274%) | Diagnostic | Diagnostic |
| Cube | cpu_allocated_bytes_per_second | 426334371.656 | 220147993.752 | -206186377.904 (-48.363%) | 490284527.404 | PASS |
| Cube | cpu_allocations | 843250.667 | 817908.667 | -25342.000 (-3.005%) | Diagnostic | Diagnostic |
| Cube | cpu_allocations_per_frame | 2855.327 | 3604.527 | +749.200 (26.239%) | Diagnostic | Diagnostic |
| Cube | cpu_allocations_per_second | 167070.506 | 163276.245 | -3794.261 (-2.271%) | 192131.082 | PASS |
| Cube | cpu_live_blocks | 47668.000 | 48708.667 | +1040.667 (2.183%) | 54818.200 | PASS |
| Cube | cpu_live_bytes | 140577582.000 | 179305615.667 | +38728033.667 (27.549%) | 161664219.300 | FAIL |
| Cube | cpu_peak_bytes | 140577582.000 | 179305615.667 | +38728033.667 (27.549%) | 161664219.300 | FAIL |
| Cube | cpu_process_peak_bytes | 146874520.667 | 185564943.667 | +38690423.000 (26.343%) | 168905698.767 | FAIL |
| Cube | cpu_profiler_overhead_bytes | 1525376.000 | 1558677.333 | +33301.333 (2.183%) | 2573952.000 | PASS |
| Cube | draw_calls_per_frame | 16.000 | 19.000 | +3.000 (18.750%) | Diagnostic | Diagnostic |
| Cube | duration_seconds | 5.047 | 5.009 | -0.037 (-0.739%) | Diagnostic | Diagnostic |
| Cube | frame_count | 295.333 | 227.000 | -68.333 (-23.138%) | Diagnostic | Diagnostic |
| Cube | frame_ms_per_frame | 17.062 | 22.010 | +4.948 (28.999%) | 19.621 | FAIL |
| Cube | frame_ms_per_second | 997.008 | 997.044 | +0.036 (0.004%) | 1146.559 | PASS |
| Cube | framegraph_dropped_spans | 472.333 | 470.667 | -1.667 (-0.353%) | Diagnostic | Diagnostic |
| Cube | framegraph_off_thread_dropped_spans | 472.333 | 470.667 | -1.667 (-0.353%) | Diagnostic | Diagnostic |
| Cube | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| Cube | gpu_allocated_bytes | 108442688.000 | 108442784.000 | +96.000 (0.000%) | Diagnostic | Diagnostic |
| Cube | gpu_allocated_bytes_per_frame | 367771.672 | 477890.687 | +110119.015 (29.942%) | Diagnostic | Diagnostic |
| Cube | gpu_allocated_bytes_per_second | 21488116.536 | 21647901.581 | +159785.045 (0.744%) | 24711334.016 | PASS |
| Cube | gpu_live_bytes | 103872768.000 | 103873080.000 | +312.000 (0.000%) | 119453683.200 | PASS |
| Cube | gpu_peak_bytes | 103872768.000 | 103873080.000 | +312.000 (0.000%) | 119453683.200 | PASS |
| Cube | gpu_resources_created | 45.000 | 47.000 | +2.000 (4.444%) | Diagnostic | Diagnostic |
| Cube | gpu_resources_created_per_frame | 0.153 | 0.207 | +0.055 (35.717%) | Diagnostic | Diagnostic |
| Cube | gpu_resources_created_per_second | 8.917 | 9.382 | +0.466 (5.221%) | 1008.917 | PASS |
| Cube | imagegraph_ms_per_frame | 0.000 | 0.019 | +0.019 (new nonzero) | 0.100 | PASS |
| Cube | imagegraph_ms_per_second | 0.000 | 0.874 | +0.874 (new nonzero) | 5.000 | PASS |
| Cube | imagegraph_wall_ms_per_frame | 0.000 | 0.019 | +0.019 (new nonzero) | 0.100 | PASS |
| Cube | imagegraph_wall_ms_per_second | 0.000 | 0.874 | +0.874 (new nonzero) | 5.000 | PASS |
| Cube | physics_ms_per_frame | 11.309 | 14.420 | +3.111 (27.509%) | 13.006 | FAIL |
| Cube | physics_ms_per_second | 661.819 | 653.255 | -8.564 (-1.294%) | 761.092 | PASS |
| Cube | physics_wall_ms_per_frame | 11.309 | 14.420 | +3.111 (27.509%) | 13.006 | FAIL |
| Cube | physics_wall_ms_per_second | 661.819 | 653.255 | -8.564 (-1.294%) | 761.092 | PASS |
| Cube | process_peak_rss_bytes | 321452714.667 | 365726378.667 | +44273664.000 (13.773%) | 369670621.867 | PASS |
| Cube | render_ms_per_frame | 3.143 | 4.596 | +1.453 (46.214%) | 3.615 | FAIL |
| Cube | render_ms_per_second | 183.671 | 208.352 | +24.681 (13.438%) | 211.222 | PASS |
| Cube | render_wall_ms_per_frame | 3.152 | 4.608 | +1.456 (46.213%) | 3.624 | FAIL |
| Cube | render_wall_ms_per_second | 184.148 | 208.892 | +24.744 (13.437%) | 211.770 | PASS |
| Cube | replication_ms_per_frame | 0.001 | 0.001 | +0.000 (29.621%) | 0.101 | PASS |
| Cube | replication_ms_per_second | 0.066 | 0.066 | +0.000 (0.651%) | 5.066 | PASS |
| Cube | replication_wall_ms_per_frame | 0.001 | 0.001 | +0.000 (29.621%) | 0.101 | PASS |
| Cube | replication_wall_ms_per_second | 0.066 | 0.066 | +0.000 (0.651%) | 5.066 | PASS |
| Cube | script_ms_per_frame | 0.009 | 0.012 | +0.003 (34.637%) | 0.109 | PASS |
| Cube | script_ms_per_second | 0.541 | 0.565 | +0.025 (4.550%) | 5.541 | PASS |
| Cube | submitted_frames | 295.333 | 227.000 | -68.333 (-23.138%) | Diagnostic | Diagnostic |
| Cube | uploaded_bytes | 237329593.333 | 182163181.333 | -55166412.000 (-23.245%) | Diagnostic | Diagnostic |
| Cube | uploaded_bytes_per_frame | 803601.265 | 802478.780 | -1122.485 (-0.140%) | Diagnostic | Diagnostic |
| Cube | uploaded_bytes_per_second | 47021983.131 | 36363526.306 | -10658456.826 (-22.667%) | 54075280.601 | PASS |
| Cube | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| Cube | imagegraph_published_textures | | 0.000 | | | Added |
| ImageGraph3D | Added | | | | | Added |
| ImageGraphFeedback | Added | | | | | Added |
| ImageGraphFlipbook | Added | | | | | Added |
| ImageGraphGui | Added | | | | | Added |
| ImageGraphMaterials | Added | | | | | Added |
| ImageGraphSkybox | Added | | | | | Added |
| ImageGraphVerlet | Added | | | | | Added |
| Interface | cpu_allocated_bytes | 3241679910.667 | 2043006218.333 | -1198673692.333 (-36.977%) | Diagnostic | Diagnostic |
| Interface | cpu_allocated_bytes_per_frame | 124989.822 | 89009.387 | -35980.436 (-28.787%) | Diagnostic | Diagnostic |
| Interface | cpu_allocated_bytes_per_second | 648321176.881 | 408596887.868 | -239724289.013 (-36.976%) | 745569353.413 | PASS |
| Interface | cpu_allocations | 9234021.000 | 4597089.000 | -4636932.000 (-50.216%) | Diagnostic | Diagnostic |
| Interface | cpu_allocations_per_frame | 356.034 | 200.286 | -155.747 (-43.745%) | Diagnostic | Diagnostic |
| Interface | cpu_allocations_per_second | 1846762.027 | 919408.000 | -927354.027 (-50.215%) | 2123776.331 | PASS |
| Interface | cpu_live_blocks | 16710.000 | 18283.333 | +1573.333 (9.416%) | 19216.500 | PASS |
| Interface | cpu_live_bytes | 32525052.333 | 60216333.000 | +27691280.667 (85.138%) | 37403810.183 | FAIL |
| Interface | cpu_peak_bytes | 32525068.333 | 60216341.000 | +27691272.667 (85.138%) | 37403828.583 | FAIL |
| Interface | cpu_process_peak_bytes | 32655988.333 | 60347149.000 | +27691160.667 (84.797%) | 37554386.583 | FAIL |
| Interface | cpu_profiler_overhead_bytes | 534720.000 | 585066.667 | +50346.667 (9.416%) | 1583296.000 | PASS |
| Interface | draw_calls_per_frame | 15.000 | 18.000 | +3.000 (20.000%) | Diagnostic | Diagnostic |
| Interface | duration_seconds | 5.000 | 5.000 | -0.000 (-0.001%) | Diagnostic | Diagnostic |
| Interface | frame_count | 25936.000 | 22953.000 | -2983.000 (-11.501%) | Diagnostic | Diagnostic |
| Interface | frame_ms_per_frame | 0.186 | 0.209 | +0.023 (12.628%) | 0.286 | PASS |
| Interface | frame_ms_per_second | 961.469 | 959.079 | -2.389 (-0.249%) | 1105.689 | PASS |
| Interface | framegraph_dropped_spans | 456.000 | 448.333 | -7.667 (-1.681%) | Diagnostic | Diagnostic |
| Interface | framegraph_off_thread_dropped_spans | 456.000 | 448.333 | -7.667 (-1.681%) | Diagnostic | Diagnostic |
| Interface | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| Interface | gpu_allocated_bytes | 88035376.000 | 92208408.000 | +4173032.000 (4.740%) | Diagnostic | Diagnostic |
| Interface | gpu_allocated_bytes_per_frame | 3398.508 | 4019.154 | +620.646 (18.262%) | Diagnostic | Diagnostic |
| Interface | gpu_allocated_bytes_per_second | 17606671.476 | 18441486.960 | +834815.485 (4.741%) | 20247672.197 | PASS |
| Interface | gpu_live_bytes | 83596528.000 | 87769776.000 | +4173248.000 (4.992%) | 96136007.200 | PASS |
| Interface | gpu_peak_bytes | 83596528.000 | 87769776.000 | +4173248.000 (4.992%) | 96136007.200 | PASS |
| Interface | gpu_resources_created | 33.000 | 41.000 | +8.000 (24.242%) | Diagnostic | Diagnostic |
| Interface | gpu_resources_created_per_frame | 0.001 | 0.002 | +0.001 (40.282%) | Diagnostic | Diagnostic |
| Interface | gpu_resources_created_per_second | 6.600 | 8.200 | +1.600 (24.244%) | 1006.600 | PASS |
| Interface | imagegraph_ms_per_frame | 0.000 | 0.001 | +0.001 (new nonzero) | 0.100 | PASS |
| Interface | imagegraph_ms_per_second | 0.000 | 6.877 | +6.877 (new nonzero) | 5.000 | FAIL |
| Interface | imagegraph_wall_ms_per_frame | 0.000 | 0.001 | +0.001 (new nonzero) | 0.100 | PASS |
| Interface | imagegraph_wall_ms_per_second | 0.000 | 6.877 | +6.877 (new nonzero) | 5.000 | FAIL |
| Interface | physics_ms_per_frame | 0.000 | 0.000 | +0.000 (5.730%) | 0.100 | PASS |
| Interface | physics_ms_per_second | 1.563 | 1.460 | -0.103 (-6.561%) | 6.563 | PASS |
| Interface | physics_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (5.730%) | 0.100 | PASS |
| Interface | physics_wall_ms_per_second | 1.563 | 1.460 | -0.103 (-6.561%) | 6.563 | PASS |
| Interface | process_peak_rss_bytes | 211376810.667 | 247383381.333 | +36006570.667 (17.034%) | 243083332.267 | FAIL |
| Interface | render_ms_per_frame | 0.107 | 0.131 | +0.024 (22.529%) | 0.207 | PASS |
| Interface | render_ms_per_second | 553.349 | 600.367 | +47.018 (8.497%) | 636.351 | PASS |
| Interface | render_wall_ms_per_frame | 0.143 | 0.139 | -0.004 (-3.043%) | 0.243 | PASS |
| Interface | render_wall_ms_per_second | 742.986 | 637.790 | -105.196 (-14.159%) | 854.434 | PASS |
| Interface | replication_ms_per_frame | 0.000 | 0.000 | +0.000 (5.752%) | 0.100 | PASS |
| Interface | replication_ms_per_second | 1.323 | 1.239 | -0.083 (-6.291%) | 6.323 | PASS |
| Interface | replication_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (5.752%) | 0.100 | PASS |
| Interface | replication_wall_ms_per_second | 1.323 | 1.239 | -0.083 (-6.291%) | 6.323 | PASS |
| Interface | script_ms_per_frame | 0.000 | 0.000 | +0.000 (10.084%) | 0.100 | PASS |
| Interface | script_ms_per_second | 1.181 | 1.148 | -0.033 (-2.792%) | 6.181 | PASS |
| Interface | submitted_frames | 25936.000 | 22953.000 | -2983.000 (-11.501%) | Diagnostic | Diagnostic |
| Interface | uploaded_bytes | 0.000 | 48.000 | +48.000 (new nonzero) | Diagnostic | Diagnostic |
| Interface | uploaded_bytes_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | Diagnostic | Diagnostic |
| Interface | uploaded_bytes_per_second | 0.000 | 9.600 | +9.600 (new nonzero) | 65536.000 | PASS |
| Interface | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| Interface | imagegraph_published_textures | | 0.000 | | | Added |
| Meshes | cpu_allocated_bytes | 931392720.333 | 542722665.333 | -388670055.000 (-41.730%) | Diagnostic | Diagnostic |
| Meshes | cpu_allocated_bytes_per_frame | 31170.465 | 24092.877 | -7077.588 (-22.706%) | Diagnostic | Diagnostic |
| Meshes | cpu_allocated_bytes_per_second | 186275617.604 | 108543332.426 | -77732285.179 (-41.730%) | 214216960.245 | PASS |
| Meshes | cpu_allocations | 6641629.333 | 4631380.667 | -2010248.667 (-30.267%) | Diagnostic | Diagnostic |
| Meshes | cpu_allocations_per_frame | 222.266 | 205.599 | -16.668 (-7.499%) | Diagnostic | Diagnostic |
| Meshes | cpu_allocations_per_second | 1328305.001 | 926265.888 | -402039.113 (-30.267%) | 1527550.752 | PASS |
| Meshes | cpu_live_blocks | 16390.000 | 17445.000 | +1055.000 (6.437%) | 18848.500 | PASS |
| Meshes | cpu_live_bytes | 32508777.000 | 59723891.000 | +27215114.000 (83.716%) | 37385093.550 | FAIL |
| Meshes | cpu_peak_bytes | 32508777.000 | 59723891.000 | +27215114.000 (83.716%) | 37385093.550 | FAIL |
| Meshes | cpu_process_peak_bytes | 32639601.667 | 59854929.000 | +27215327.333 (83.381%) | 37535541.917 | FAIL |
| Meshes | cpu_profiler_overhead_bytes | 524480.000 | 558240.000 | +33760.000 (6.437%) | 1573056.000 | PASS |
| Meshes | draw_calls_per_frame | 37.093 | 40.124 | +3.030 (8.170%) | Diagnostic | Diagnostic |
| Meshes | duration_seconds | 5.000 | 5.000 | -0.000 (-0.000%) | Diagnostic | Diagnostic |
| Meshes | frame_count | 29882.000 | 22526.333 | -7355.667 (-24.616%) | Diagnostic | Diagnostic |
| Meshes | frame_ms_per_frame | 0.159 | 0.211 | +0.051 (32.311%) | 0.259 | PASS |
| Meshes | frame_ms_per_second | 951.146 | 949.494 | -1.652 (-0.174%) | 1093.818 | PASS |
| Meshes | framegraph_dropped_spans | 450.667 | 443.667 | -7.000 (-1.553%) | Diagnostic | Diagnostic |
| Meshes | framegraph_off_thread_dropped_spans | 450.667 | 443.667 | -7.000 (-1.553%) | Diagnostic | Diagnostic |
| Meshes | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| Meshes | gpu_allocated_bytes | 119985024.000 | 119985120.000 | +96.000 (0.000%) | Diagnostic | Diagnostic |
| Meshes | gpu_allocated_bytes_per_frame | 4018.915 | 5326.613 | +1307.697 (32.539%) | Diagnostic | Diagnostic |
| Meshes | gpu_allocated_bytes_per_second | 23996625.652 | 23996758.380 | +132.728 (0.001%) | 27596119.500 | PASS |
| Meshes | gpu_live_bytes | 115546176.000 | 115546488.000 | +312.000 (0.000%) | 132878102.400 | PASS |
| Meshes | gpu_peak_bytes | 115546176.000 | 115546488.000 | +312.000 (0.000%) | 132878102.400 | PASS |
| Meshes | gpu_resources_created | 51.000 | 53.000 | +2.000 (3.922%) | Diagnostic | Diagnostic |
| Meshes | gpu_resources_created_per_frame | 0.002 | 0.002 | +0.001 (37.736%) | Diagnostic | Diagnostic |
| Meshes | gpu_resources_created_per_second | 10.200 | 10.600 | +0.400 (3.922%) | 1010.200 | PASS |
| Meshes | imagegraph_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Meshes | imagegraph_ms_per_second | 0.000 | 7.021 | +7.021 (new nonzero) | 5.000 | FAIL |
| Meshes | imagegraph_wall_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Meshes | imagegraph_wall_ms_per_second | 0.000 | 7.021 | +7.021 (new nonzero) | 5.000 | FAIL |
| Meshes | physics_ms_per_frame | 0.000 | 0.000 | +0.000 (2.268%) | 0.100 | PASS |
| Meshes | physics_ms_per_second | 1.773 | 1.368 | -0.405 (-22.829%) | 6.773 | PASS |
| Meshes | physics_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (2.268%) | 0.100 | PASS |
| Meshes | physics_wall_ms_per_second | 1.773 | 1.368 | -0.405 (-22.829%) | 6.773 | PASS |
| Meshes | process_peak_rss_bytes | 237214378.667 | 254810794.667 | +17596416.000 (7.418%) | 272796535.467 | PASS |
| Meshes | render_ms_per_frame | 0.130 | 0.170 | +0.041 (31.268%) | 0.230 | PASS |
| Meshes | render_ms_per_second | 775.475 | 768.018 | -7.457 (-0.962%) | 891.796 | PASS |
| Meshes | render_wall_ms_per_frame | 0.132 | 0.173 | +0.041 (30.924%) | 0.232 | PASS |
| Meshes | render_wall_ms_per_second | 789.890 | 780.249 | -9.641 (-1.221%) | 908.374 | PASS |
| Meshes | replication_ms_per_frame | 0.000 | 0.000 | +0.000 (17.879%) | 0.100 | PASS |
| Meshes | replication_ms_per_second | 1.593 | 1.418 | -0.176 (-11.031%) | 6.593 | PASS |
| Meshes | replication_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (17.879%) | 0.100 | PASS |
| Meshes | replication_wall_ms_per_second | 1.593 | 1.418 | -0.176 (-11.031%) | 6.593 | PASS |
| Meshes | script_ms_per_frame | 0.000 | 0.000 | +0.000 (2.264%) | 0.100 | PASS |
| Meshes | script_ms_per_second | 1.635 | 1.261 | -0.373 (-22.842%) | 6.635 | PASS |
| Meshes | submitted_frames | 29882.000 | 22526.333 | -7355.667 (-24.616%) | Diagnostic | Diagnostic |
| Meshes | uploaded_bytes | 1324.000 | 1372.000 | +48.000 (3.625%) | Diagnostic | Diagnostic |
| Meshes | uploaded_bytes_per_frame | 0.044 | 0.061 | +0.017 (37.343%) | Diagnostic | Diagnostic |
| Meshes | uploaded_bytes_per_second | 264.796 | 274.397 | +9.601 (3.626%) | 65800.796 | PASS |
| Meshes | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| Meshes | imagegraph_published_textures | | 0.000 | | | Added |
| Particles | cpu_allocated_bytes | 20832077209.000 | 577336668.667 | -20254740540.333 (-97.229%) | Diagnostic | Diagnostic |
| Particles | cpu_allocated_bytes_per_frame | 859915.448 | 24428.475 | -835486.973 (-97.159%) | Diagnostic | Diagnostic |
| Particles | cpu_allocated_bytes_per_second | 4166321991.052 | 115464774.084 | -4050857216.968 (-97.229%) | 4791270289.710 | PASS |
| Particles | cpu_allocations | 5889143.000 | 4835937.333 | -1053205.667 (-17.884%) | Diagnostic | Diagnostic |
| Particles | cpu_allocations_per_frame | 243.102 | 204.569 | -38.533 (-15.851%) | Diagnostic | Diagnostic |
| Particles | cpu_allocations_per_second | 1177802.188 | 967166.010 | -210636.178 (-17.884%) | 1354472.516 | PASS |
| Particles | cpu_live_blocks | 16898.000 | 17945.667 | +1047.667 (6.200%) | 19432.700 | PASS |
| Particles | cpu_live_bytes | 40585337.000 | 67917604.000 | +27332267.000 (67.345%) | 46673137.550 | FAIL |
| Particles | cpu_peak_bytes | 40585337.000 | 67917612.000 | +27332275.000 (67.345%) | 46673137.550 | FAIL |
| Particles | cpu_process_peak_bytes | 40716509.000 | 68048784.000 | +27332275.000 (67.128%) | 46823985.350 | FAIL |
| Particles | cpu_profiler_overhead_bytes | 540736.000 | 574261.333 | +33525.333 (6.200%) | 1589312.000 | PASS |
| Particles | draw_calls_per_frame | 26.000 | 28.999 | +2.999 (11.535%) | Diagnostic | Diagnostic |
| Particles | duration_seconds | 5.000 | 5.000 | +0.000 (0.000%) | Diagnostic | Diagnostic |
| Particles | frame_count | 24226.000 | 23641.667 | -584.333 (-2.412%) | Diagnostic | Diagnostic |
| Particles | frame_ms_per_frame | 0.199 | 0.202 | +0.003 (1.748%) | 0.299 | PASS |
| Particles | frame_ms_per_second | 958.936 | 953.122 | -5.814 (-0.606%) | 1102.776 | PASS |
| Particles | framegraph_dropped_spans | 449.000 | 452.000 | +3.000 (0.668%) | Diagnostic | Diagnostic |
| Particles | framegraph_off_thread_dropped_spans | 449.000 | 452.000 | +3.000 (0.668%) | Diagnostic | Diagnostic |
| Particles | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| Particles | gpu_allocated_bytes | 109632576.000 | 109681832.000 | +49256.000 (0.045%) | Diagnostic | Diagnostic |
| Particles | gpu_allocated_bytes_per_frame | 4543.706 | 4653.935 | +110.230 (2.426%) | Diagnostic | Diagnostic |
| Particles | gpu_allocated_bytes_per_second | 21926038.020 | 21935883.486 | +9845.466 (0.045%) | 25214943.723 | PASS |
| Particles | gpu_live_bytes | 105193728.000 | 105243196.000 | +49468.000 (0.047%) | 120972787.200 | PASS |
| Particles | gpu_peak_bytes | 105193728.000 | 105243196.000 | +49468.000 (0.047%) | 120972787.200 | PASS |
| Particles | gpu_resources_created | 60.000 | 64.000 | +4.000 (6.667%) | Diagnostic | Diagnostic |
| Particles | gpu_resources_created_per_frame | 0.002 | 0.003 | +0.000 (9.205%) | Diagnostic | Diagnostic |
| Particles | gpu_resources_created_per_second | 12.000 | 12.800 | +0.800 (6.667%) | 1012.000 | PASS |
| Particles | imagegraph_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Particles | imagegraph_ms_per_second | 0.000 | 10.608 | +10.608 (new nonzero) | 5.000 | FAIL |
| Particles | imagegraph_wall_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Particles | imagegraph_wall_ms_per_second | 0.000 | 10.608 | +10.608 (new nonzero) | 5.000 | FAIL |
| Particles | physics_ms_per_frame | 0.001 | 0.001 | +0.000 (3.813%) | 0.101 | PASS |
| Particles | physics_ms_per_second | 6.151 | 6.239 | +0.088 (1.431%) | 11.151 | PASS |
| Particles | physics_wall_ms_per_frame | 0.001 | 0.001 | +0.000 (3.813%) | 0.101 | PASS |
| Particles | physics_wall_ms_per_second | 6.151 | 6.239 | +0.088 (1.431%) | 11.151 | PASS |
| Particles | process_peak_rss_bytes | 232947712.000 | 309548373.333 | +76600661.333 (32.883%) | 267889868.800 | FAIL |
| Particles | render_ms_per_frame | 0.159 | 0.159 | -0.001 (-0.359%) | 0.259 | PASS |
| Particles | render_ms_per_second | 767.616 | 747.656 | -19.960 (-2.600%) | 882.758 | PASS |
| Particles | render_wall_ms_per_frame | 0.163 | 0.162 | -0.001 (-0.734%) | 0.263 | PASS |
| Particles | render_wall_ms_per_second | 785.425 | 762.083 | -23.342 (-2.972%) | 903.239 | PASS |
| Particles | replication_ms_per_frame | 0.000 | 0.000 | -0.000 (-2.779%) | 0.100 | PASS |
| Particles | replication_ms_per_second | 1.393 | 1.321 | -0.072 (-5.137%) | 6.393 | PASS |
| Particles | replication_wall_ms_per_frame | 0.000 | 0.000 | -0.000 (-2.779%) | 0.100 | PASS |
| Particles | replication_wall_ms_per_second | 1.393 | 1.321 | -0.072 (-5.137%) | 6.393 | PASS |
| Particles | script_ms_per_frame | 0.000 | 0.000 | -0.000 (-4.545%) | 0.100 | PASS |
| Particles | script_ms_per_second | 1.818 | 1.705 | -0.113 (-6.225%) | 6.818 | PASS |
| Particles | submitted_frames | 24226.000 | 23641.667 | -584.333 (-2.412%) | Diagnostic | Diagnostic |
| Particles | uploaded_bytes | 64009912.000 | 62471512.000 | -1538400.000 (-2.403%) | Diagnostic | Diagnostic |
| Particles | uploaded_bytes_per_frame | 2642.140 | 2642.335 | +0.195 (0.007%) | Diagnostic | Diagnostic |
| Particles | uploaded_bytes_per_second | 12801695.175 | 12494025.094 | -307670.082 (-2.403%) | 14721949.452 | PASS |
| Particles | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| Particles | imagegraph_published_textures | | 0.000 | | | Added |
| RenderFeatures | cpu_allocated_bytes | 658872302.333 | 590937416.333 | -67934886.000 (-10.311%) | Diagnostic | Diagnostic |
| RenderFeatures | cpu_allocated_bytes_per_frame | 24749.958 | 24761.456 | +11.497 (0.046%) | Diagnostic | Diagnostic |
| RenderFeatures | cpu_allocated_bytes_per_second | 131770350.089 | 118185710.213 | -13584639.875 (-10.309%) | 151535902.602 | PASS |
| RenderFeatures | cpu_allocations | 6838264.667 | 5796386.667 | -1041878.000 (-15.236%) | Diagnostic | Diagnostic |
| RenderFeatures | cpu_allocations_per_frame | 256.870 | 242.879 | -13.991 (-5.447%) | Diagnostic | Diagnostic |
| RenderFeatures | cpu_allocations_per_second | 1367610.274 | 1159259.944 | -208350.330 (-15.235%) | 1572751.815 | PASS |
| RenderFeatures | cpu_live_blocks | 16575.667 | 17624.000 | +1048.333 (6.325%) | 19062.017 | PASS |
| RenderFeatures | cpu_live_bytes | 32501921.667 | 59702902.000 | +27200980.333 (83.690%) | 37377209.917 | FAIL |
| RenderFeatures | cpu_peak_bytes | 32501929.667 | 59702902.000 | +27200972.333 (83.690%) | 37377219.117 | FAIL |
| RenderFeatures | cpu_process_peak_bytes | 32633129.667 | 59834094.000 | +27200964.333 (83.354%) | 37528099.117 | FAIL |
| RenderFeatures | cpu_profiler_overhead_bytes | 530421.333 | 563968.000 | +33546.667 (6.325%) | 1578997.333 | PASS |
| RenderFeatures | draw_calls_per_frame | 26.000 | 29.000 | +3.000 (11.538%) | Diagnostic | Diagnostic |
| RenderFeatures | duration_seconds | 5.000 | 5.000 | -0.000 (-0.002%) | Diagnostic | Diagnostic |
| RenderFeatures | frame_count | 26621.667 | 23865.667 | -2756.000 (-10.352%) | Diagnostic | Diagnostic |
| RenderFeatures | frame_ms_per_frame | 0.179 | 0.200 | +0.020 (11.417%) | 0.279 | PASS |
| RenderFeatures | frame_ms_per_second | 954.588 | 953.568 | -1.020 (-0.107%) | 1097.776 | PASS |
| RenderFeatures | framegraph_dropped_spans | 442.333 | 446.667 | +4.333 (0.980%) | Diagnostic | Diagnostic |
| RenderFeatures | framegraph_off_thread_dropped_spans | 442.333 | 446.667 | +4.333 (0.980%) | Diagnostic | Diagnostic |
| RenderFeatures | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| RenderFeatures | gpu_allocated_bytes | 108986176.000 | 108987296.000 | +1120.000 (0.001%) | Diagnostic | Diagnostic |
| RenderFeatures | gpu_allocated_bytes_per_frame | 4094.966 | 4567.642 | +472.676 (11.543%) | Diagnostic | Diagnostic |
| RenderFeatures | gpu_allocated_bytes_per_second | 21796555.184 | 21797130.804 | +575.620 (0.003%) | 25066038.461 | PASS |
| RenderFeatures | gpu_live_bytes | 104547328.000 | 104548664.000 | +1336.000 (0.001%) | 120229427.200 | PASS |
| RenderFeatures | gpu_peak_bytes | 104547328.000 | 104548664.000 | +1336.000 (0.001%) | 120229427.200 | PASS |
| RenderFeatures | gpu_resources_created | 51.000 | 53.000 | +2.000 (3.922%) | Diagnostic | Diagnostic |
| RenderFeatures | gpu_resources_created_per_frame | 0.002 | 0.002 | +0.000 (15.916%) | Diagnostic | Diagnostic |
| RenderFeatures | gpu_resources_created_per_second | 10.200 | 10.600 | +0.400 (3.923%) | 1010.200 | PASS |
| RenderFeatures | imagegraph_ms_per_frame | 0.000 | 0.001 | +0.001 (new nonzero) | 0.100 | PASS |
| RenderFeatures | imagegraph_ms_per_second | 0.000 | 6.829 | +6.829 (new nonzero) | 5.000 | FAIL |
| RenderFeatures | imagegraph_wall_ms_per_frame | 0.000 | 0.001 | +0.001 (new nonzero) | 0.100 | PASS |
| RenderFeatures | imagegraph_wall_ms_per_second | 0.000 | 6.829 | +6.829 (new nonzero) | 5.000 | FAIL |
| RenderFeatures | physics_ms_per_frame | 0.000 | 0.000 | +0.000 (24.013%) | 0.100 | PASS |
| RenderFeatures | physics_ms_per_second | 1.645 | 1.831 | +0.186 (11.317%) | 6.645 | PASS |
| RenderFeatures | physics_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (24.013%) | 0.100 | PASS |
| RenderFeatures | physics_wall_ms_per_second | 1.645 | 1.831 | +0.186 (11.317%) | 6.645 | PASS |
| RenderFeatures | process_peak_rss_bytes | 328052736.000 | 320077824.000 | -7974912.000 (-2.431%) | 377260646.400 | PASS |
| RenderFeatures | render_ms_per_frame | 0.151 | 0.166 | +0.016 (10.621%) | 0.251 | PASS |
| RenderFeatures | render_ms_per_second | 801.193 | 794.678 | -6.515 (-0.813%) | 921.371 | PASS |
| RenderFeatures | render_wall_ms_per_frame | 0.152 | 0.168 | +0.016 (10.536%) | 0.252 | PASS |
| RenderFeatures | render_wall_ms_per_second | 810.526 | 803.321 | -7.205 (-0.889%) | 932.105 | PASS |
| RenderFeatures | replication_ms_per_frame | 0.000 | 0.000 | -0.000 (-0.035%) | 0.100 | PASS |
| RenderFeatures | replication_ms_per_second | 1.491 | 1.336 | -0.155 (-10.398%) | 6.491 | PASS |
| RenderFeatures | replication_wall_ms_per_frame | 0.000 | 0.000 | -0.000 (-0.035%) | 0.100 | PASS |
| RenderFeatures | replication_wall_ms_per_second | 1.491 | 1.336 | -0.155 (-10.398%) | 6.491 | PASS |
| RenderFeatures | script_ms_per_frame | 0.000 | 0.000 | +0.000 (19.658%) | 0.100 | PASS |
| RenderFeatures | script_ms_per_second | 0.761 | 0.817 | +0.056 (7.421%) | 5.761 | PASS |
| RenderFeatures | submitted_frames | 26621.667 | 23865.667 | -2756.000 (-10.352%) | Diagnostic | Diagnostic |
| RenderFeatures | uploaded_bytes | 41104205.333 | 37612690.667 | -3491514.667 (-8.494%) | Diagnostic | Diagnostic |
| RenderFeatures | uploaded_bytes_per_frame | 1544.013 | 1576.017 | +32.004 (2.073%) | Diagnostic | Diagnostic |
| RenderFeatures | uploaded_bytes_per_second | 8220584.640 | 7522425.328 | -698159.312 (-8.493%) | 9453672.336 | PASS |
| RenderFeatures | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| RenderFeatures | imagegraph_published_textures | | 0.000 | | | Added |
| ReplicationRings | cpu_allocated_bytes | 1917331132.333 | 490174768.667 | -1427156363.667 (-74.435%) | Diagnostic | Diagnostic |
| ReplicationRings | cpu_allocated_bytes_per_frame | 247292.612 | 72328.678 | -174963.935 (-70.752%) | Diagnostic | Diagnostic |
| ReplicationRings | cpu_allocated_bytes_per_second | 383433411.297 | 98029091.601 | -285404319.696 (-74.434%) | 440948422.991 | PASS |
| ReplicationRings | cpu_allocations | 2567178.333 | 2184954.667 | -382223.667 (-14.889%) | Diagnostic | Diagnostic |
| ReplicationRings | cpu_allocations_per_frame | 331.188 | 322.407 | -8.781 (-2.651%) | Diagnostic | Diagnostic |
| ReplicationRings | cpu_allocations_per_second | 513391.728 | 436964.800 | -76426.928 (-14.887%) | 590400.488 | PASS |
| ReplicationRings | cpu_live_blocks | 29101.000 | 30225.333 | +1124.333 (3.864%) | 33466.150 | PASS |
| ReplicationRings | cpu_live_bytes | 38339640.000 | 66669412.333 | +28329772.333 (73.892%) | 44090586.000 | FAIL |
| ReplicationRings | cpu_peak_bytes | 38341197.667 | 66669839.000 | +28328641.333 (73.886%) | 44092377.317 | FAIL |
| ReplicationRings | cpu_process_peak_bytes | 43133419.333 | 71402097.333 | +28268678.000 (65.538%) | 49603432.233 | FAIL |
| ReplicationRings | cpu_profiler_overhead_bytes | 931232.000 | 967210.667 | +35978.667 (3.864%) | 1979808.000 | PASS |
| ReplicationRings | draw_calls_per_frame | 27.000 | 33.000 | +6.000 (22.222%) | Diagnostic | Diagnostic |
| ReplicationRings | duration_seconds | 5.000 | 5.000 | -0.000 (-0.003%) | Diagnostic | Diagnostic |
| ReplicationRings | frame_count | 7753.667 | 6776.667 | -977.000 (-12.600%) | Diagnostic | Diagnostic |
| ReplicationRings | frame_ms_per_frame | 0.622 | 0.710 | +0.088 (14.225%) | 0.722 | PASS |
| ReplicationRings | frame_ms_per_second | 961.680 | 962.049 | +0.369 (0.038%) | 1105.933 | PASS |
| ReplicationRings | framegraph_dropped_spans | 452.000 | 452.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ReplicationRings | framegraph_off_thread_dropped_spans | 452.000 | 452.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ReplicationRings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ReplicationRings | gpu_allocated_bytes | 15796736.000 | 15796736.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ReplicationRings | gpu_allocated_bytes_per_frame | 2043.027 | 2332.673 | +289.646 (14.177%) | Diagnostic | Diagnostic |
| ReplicationRings | gpu_allocated_bytes_per_second | 3159074.893 | 3159159.084 | +84.191 (0.003%) | 3632936.127 | PASS |
| ReplicationRings | gpu_live_bytes | 199632960.000 | 199633272.000 | +312.000 (0.000%) | 229577904.000 | PASS |
| ReplicationRings | gpu_peak_bytes | 200699200.000 | 200699512.000 | +312.000 (0.000%) | 230804080.000 | PASS |
| ReplicationRings | gpu_resources_created | 12.000 | 12.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ReplicationRings | gpu_resources_created_per_frame | 0.002 | 0.002 | +0.000 (14.177%) | Diagnostic | Diagnostic |
| ReplicationRings | gpu_resources_created_per_second | 2.400 | 2.400 | +0.000 (0.003%) | 1002.400 | PASS |
| ReplicationRings | imagegraph_ms_per_frame | 0.000 | 0.005 | +0.005 (new nonzero) | 0.100 | PASS |
| ReplicationRings | imagegraph_ms_per_second | 0.000 | 6.442 | +6.442 (new nonzero) | 5.000 | FAIL |
| ReplicationRings | imagegraph_wall_ms_per_frame | 0.000 | 0.005 | +0.005 (new nonzero) | 0.100 | PASS |
| ReplicationRings | imagegraph_wall_ms_per_second | 0.000 | 6.442 | +6.442 (new nonzero) | 5.000 | FAIL |
| ReplicationRings | physics_ms_per_frame | 0.004 | 0.005 | +0.001 (19.888%) | 0.104 | PASS |
| ReplicationRings | physics_ms_per_second | 6.749 | 7.118 | +0.369 (5.472%) | 11.749 | PASS |
| ReplicationRings | physics_wall_ms_per_frame | 0.004 | 0.005 | +0.001 (19.888%) | 0.104 | PASS |
| ReplicationRings | physics_wall_ms_per_second | 6.749 | 7.118 | +0.369 (5.472%) | 11.749 | PASS |
| ReplicationRings | process_peak_rss_bytes | 223673002.667 | 257236992.000 | +33563989.333 (15.006%) | 257223953.067 | FAIL |
| ReplicationRings | render_ms_per_frame | 0.374 | 0.437 | +0.062 (16.647%) | 0.474 | PASS |
| ReplicationRings | render_ms_per_second | 579.091 | 591.335 | +12.244 (2.114%) | 665.955 | PASS |
| ReplicationRings | render_wall_ms_per_frame | 0.378 | 0.440 | +0.062 (16.524%) | 0.478 | PASS |
| ReplicationRings | render_wall_ms_per_second | 584.344 | 596.072 | +11.728 (2.007%) | 671.995 | PASS |
| ReplicationRings | replication_ms_per_frame | 0.013 | 0.015 | +0.002 (12.168%) | 0.113 | PASS |
| ReplicationRings | replication_ms_per_second | 20.474 | 20.202 | -0.272 (-1.330%) | 25.474 | PASS |
| ReplicationRings | replication_wall_ms_per_frame | 0.015 | 0.016 | +0.002 (11.447%) | 0.115 | PASS |
| ReplicationRings | replication_wall_ms_per_second | 22.452 | 22.016 | -0.436 (-1.943%) | 27.452 | PASS |
| ReplicationRings | script_ms_per_frame | 0.023 | 0.027 | +0.004 (17.243%) | 0.123 | PASS |
| ReplicationRings | script_ms_per_second | 35.638 | 36.718 | +1.080 (3.030%) | 40.983 | PASS |
| ReplicationRings | submitted_frames | 7753.667 | 6776.667 | -977.000 (-12.600%) | Diagnostic | Diagnostic |
| ReplicationRings | uploaded_bytes | 558137888.000 | 481055654.667 | -77082233.333 (-13.811%) | Diagnostic | Diagnostic |
| ReplicationRings | uploaded_bytes_per_frame | 71984.385 | 70980.918 | -1003.467 (-1.394%) | Diagnostic | Diagnostic |
| ReplicationRings | uploaded_bytes_per_second | 111618043.289 | 96205386.858 | -15412656.431 (-13.808%) | 128360749.783 | PASS |
| ReplicationRings | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| ReplicationRings | imagegraph_published_textures | | 0.000 | | | Added |
| Rings | cpu_allocated_bytes | 3669822233.667 | 730265331.333 | -2939556902.333 (-80.101%) | Diagnostic | Diagnostic |
| Rings | cpu_allocated_bytes_per_frame | 232460.236 | 58153.302 | -174306.934 (-74.984%) | Diagnostic | Diagnostic |
| Rings | cpu_allocated_bytes_per_second | 733920552.125 | 146050154.984 | -587870397.140 (-80.100%) | 844008634.943 | PASS |
| Rings | cpu_allocations | 3275118.333 | 2434110.667 | -841007.667 (-25.679%) | Diagnostic | Diagnostic |
| Rings | cpu_allocations_per_frame | 207.458 | 193.835 | -13.623 (-6.567%) | Diagnostic | Diagnostic |
| Rings | cpu_allocations_per_second | 654984.493 | 486812.431 | -168172.062 (-25.676%) | 753232.167 | PASS |
| Rings | cpu_live_blocks | 24595.000 | 25644.333 | +1049.333 (4.266%) | 28284.250 | PASS |
| Rings | cpu_live_bytes | 34768235.333 | 62453063.000 | +27684827.667 (79.627%) | 39983470.633 | FAIL |
| Rings | cpu_peak_bytes | 34768323.333 | 62453079.000 | +27684755.667 (79.626%) | 39983571.833 | FAIL |
| Rings | cpu_process_peak_bytes | 34899163.333 | 62584135.000 | +27684971.667 (79.328%) | 40134037.833 | FAIL |
| Rings | cpu_profiler_overhead_bytes | 787040.000 | 820618.667 | +33578.667 (4.266%) | 1835616.000 | PASS |
| Rings | draw_calls_per_frame | 16.000 | 19.000 | +3.000 (18.750%) | Diagnostic | Diagnostic |
| Rings | duration_seconds | 5.000 | 5.000 | -0.000 (-0.004%) | Diagnostic | Diagnostic |
| Rings | frame_count | 15787.000 | 12557.667 | -3229.333 (-20.456%) | Diagnostic | Diagnostic |
| Rings | frame_ms_per_frame | 0.305 | 0.382 | +0.077 (25.171%) | 0.405 | PASS |
| Rings | frame_ms_per_second | 963.728 | 959.969 | -3.759 (-0.390%) | 1108.287 | PASS |
| Rings | framegraph_dropped_spans | 456.667 | 452.000 | -4.667 (-1.022%) | Diagnostic | Diagnostic |
| Rings | framegraph_off_thread_dropped_spans | 456.667 | 452.000 | -4.667 (-1.022%) | Diagnostic | Diagnostic |
| Rings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| Rings | gpu_allocated_bytes | 104883264.000 | 104883360.000 | +96.000 (0.000%) | Diagnostic | Diagnostic |
| Rings | gpu_allocated_bytes_per_frame | 6646.824 | 8352.778 | +1705.954 (25.666%) | Diagnostic | Diagnostic |
| Rings | gpu_allocated_bytes_per_second | 20975395.283 | 20976252.803 | +857.520 (0.004%) | 24121704.575 | PASS |
| Rings | gpu_live_bytes | 100444416.000 | 100444728.000 | +312.000 (0.000%) | 115511078.400 | PASS |
| Rings | gpu_peak_bytes | 100444416.000 | 100444728.000 | +312.000 (0.000%) | 115511078.400 | PASS |
| Rings | gpu_resources_created | 43.000 | 45.000 | +2.000 (4.651%) | Diagnostic | Diagnostic |
| Rings | gpu_resources_created_per_frame | 0.003 | 0.004 | +0.001 (31.510%) | Diagnostic | Diagnostic |
| Rings | gpu_resources_created_per_second | 8.599 | 9.000 | +0.400 (4.655%) | 1008.599 | PASS |
| Rings | imagegraph_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Rings | imagegraph_ms_per_second | 0.000 | 6.077 | +6.077 (new nonzero) | 5.000 | FAIL |
| Rings | imagegraph_wall_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Rings | imagegraph_wall_ms_per_second | 0.000 | 6.077 | +6.077 (new nonzero) | 5.000 | FAIL |
| Rings | physics_ms_per_frame | 0.002 | 0.002 | +0.000 (31.788%) | 0.102 | PASS |
| Rings | physics_ms_per_second | 4.782 | 5.016 | +0.234 (4.889%) | 9.782 | PASS |
| Rings | physics_wall_ms_per_frame | 0.002 | 0.002 | +0.000 (31.788%) | 0.102 | PASS |
| Rings | physics_wall_ms_per_second | 4.782 | 5.016 | +0.234 (4.889%) | 9.782 | PASS |
| Rings | process_peak_rss_bytes | 212930560.000 | 244774229.333 | +31843669.333 (14.955%) | 244870144.000 | PASS |
| Rings | render_ms_per_frame | 0.210 | 0.268 | +0.058 (27.563%) | 0.310 | PASS |
| Rings | render_ms_per_second | 663.418 | 673.443 | +10.025 (1.511%) | 762.930 | PASS |
| Rings | render_wall_ms_per_frame | 0.212 | 0.270 | +0.058 (27.516%) | 0.312 | PASS |
| Rings | render_wall_ms_per_second | 668.779 | 678.630 | +9.851 (1.473%) | 769.096 | PASS |
| Rings | replication_ms_per_frame | 0.000 | 0.000 | +0.000 (12.430%) | 0.100 | PASS |
| Rings | replication_ms_per_second | 0.854 | 0.764 | -0.090 (-10.505%) | 5.854 | PASS |
| Rings | replication_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (12.430%) | 0.100 | PASS |
| Rings | replication_wall_ms_per_second | 0.854 | 0.764 | -0.090 (-10.505%) | 5.854 | PASS |
| Rings | script_ms_per_frame | 0.011 | 0.015 | +0.004 (36.670%) | 0.111 | PASS |
| Rings | script_ms_per_second | 34.202 | 37.197 | +2.995 (8.757%) | 39.332 | PASS |
| Rings | submitted_frames | 15787.000 | 12557.667 | -3229.333 (-20.456%) | Diagnostic | Diagnostic |
| Rings | uploaded_bytes | 646650460.000 | 514349708.000 | -132300752.000 (-20.459%) | Diagnostic | Diagnostic |
| Rings | uploaded_bytes_per_frame | 40960.947 | 40959.018 | -1.928 (-0.005%) | Diagnostic | Diagnostic |
| Rings | uploaded_bytes_per_second | 129322357.733 | 102867891.553 | -26454466.180 (-20.456%) | 148720711.393 | PASS |
| Rings | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| Rings | imagegraph_published_textures | | 0.000 | | | Added |
| ServerPhysics | Demo input SHA-256 changed: d3d0c7b0d5a62f95dd5c983af37ed8a51d8c6393b5120317e5e3120c40d5346c to 158ac6af81322a0f482a1b4848a07cf46c269a16e9f96453a2c7d44375b0b0de | | | | | Input warning |
| ServerPhysics | cpu_allocated_bytes | 734747175.333 | 737267630.333 | +2520455.000 (0.343%) | Diagnostic | Diagnostic |
| ServerPhysics | cpu_allocated_bytes_per_frame | 13951826.969 | 13738786.214 | -213040.755 (-1.527%) | Diagnostic | Diagnostic |
| ServerPhysics | cpu_allocated_bytes_per_second | 144946704.310 | 143964623.110 | -982081.200 (-0.678%) | 166688709.957 | PASS |
| ServerPhysics | cpu_allocations | 1344726.000 | 1448313.333 | +103587.333 (7.703%) | Diagnostic | Diagnostic |
| ServerPhysics | cpu_allocations_per_frame | 25526.765 | 26981.528 | +1454.763 (5.699%) | Diagnostic | Diagnostic |
| ServerPhysics | cpu_allocations_per_second | 265265.306 | 282766.348 | +17501.042 (6.598%) | 305055.102 | PASS |
| ServerPhysics | cpu_live_blocks | 212832.000 | 213329.000 | +497.000 (0.234%) | 244756.800 | PASS |
| ServerPhysics | cpu_live_bytes | 434408483.000 | 438288931.000 | +3880448.000 (0.893%) | 499569755.450 | PASS |
| ServerPhysics | cpu_peak_bytes | 434408483.000 | 438288931.000 | +3880448.000 (0.893%) | 499569755.450 | PASS |
| ServerPhysics | cpu_process_peak_bytes | 445252283.000 | 449133365.000 | +3881082.000 (0.872%) | 512040125.450 | PASS |
| ServerPhysics | cpu_profiler_overhead_bytes | 6810624.000 | 6826528.000 | +15904.000 (0.234%) | 7859200.000 | PASS |
| ServerPhysics | duration_seconds | 5.069 | 5.121 | +0.052 (1.034%) | Diagnostic | Diagnostic |
| ServerPhysics | frame_count | 52.667 | 53.667 | +1.000 (1.899%) | Diagnostic | Diagnostic |
| ServerPhysics | frame_ms_per_frame | 95.663 | 94.378 | -1.285 (-1.343%) | 110.012 | PASS |
| ServerPhysics | frame_ms_per_second | 993.838 | 988.925 | -4.913 (-0.494%) | 1142.914 | PASS |
| ServerPhysics | framegraph_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ServerPhysics | framegraph_off_thread_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ServerPhysics | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ServerPhysics | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 (0%) | 0.100 | PASS |
| ServerPhysics | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 (0%) | 5.000 | PASS |
| ServerPhysics | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 (0%) | 0.100 | PASS |
| ServerPhysics | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 (0%) | 5.000 | PASS |
| ServerPhysics | physics_ms_per_frame | 91.796 | 90.571 | -1.225 (-1.334%) | 105.566 | PASS |
| ServerPhysics | physics_ms_per_second | 953.671 | 949.041 | -4.630 (-0.486%) | 1096.722 | PASS |
| ServerPhysics | physics_wall_ms_per_frame | 91.796 | 90.571 | -1.225 (-1.334%) | 105.566 | PASS |
| ServerPhysics | physics_wall_ms_per_second | 953.671 | 949.041 | -4.630 (-0.486%) | 1096.722 | PASS |
| ServerPhysics | process_peak_rss_bytes | 457286997.333 | 462573568.000 | +5286570.667 (1.156%) | 525880046.933 | PASS |
| ServerPhysics | render_ms_per_frame | 0.002 | 0.001 | -0.000 (-22.222%) | 0.102 | PASS |
| ServerPhysics | render_ms_per_second | 0.017 | 0.013 | -0.004 (-21.528%) | 5.017 | PASS |
| ServerPhysics | render_wall_ms_per_frame | 0.002 | 0.001 | -0.000 (-22.222%) | 0.102 | PASS |
| ServerPhysics | render_wall_ms_per_second | 0.017 | 0.013 | -0.004 (-21.528%) | 5.017 | PASS |
| ServerPhysics | replication_ms_per_frame | 0.000 | 0.000 | +0.000 (20.871%) | 0.100 | PASS |
| ServerPhysics | replication_ms_per_second | 0.004 | 0.005 | +0.001 (21.840%) | 5.004 | PASS |
| ServerPhysics | replication_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (20.871%) | 0.100 | PASS |
| ServerPhysics | replication_wall_ms_per_second | 0.004 | 0.005 | +0.001 (21.840%) | 5.004 | PASS |
| ServerPhysics | script_ms_per_frame | 0.054 | 0.051 | -0.003 (-5.001%) | 0.154 | PASS |
| ServerPhysics | script_ms_per_second | 0.557 | 0.534 | -0.023 (-4.176%) | 5.557 | PASS |
| ServerPhysics | tick_count | 52.667 | 53.667 | +1.000 (1.899%) | Diagnostic | Diagnostic |
| ServerPhysics | tick_overruns | 44.667 | 42.333 | -2.333 (-5.224%) | Diagnostic | Diagnostic |
| ServerPhysics | tick_rate_hz | 30.000 | 30.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ServerRings | clients_admitted | 1.000 | 1.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ServerRings | cpu_allocated_bytes | 20800038.667 | 19707950.667 | -1092088.000 (-5.250%) | Diagnostic | Diagnostic |
| ServerRings | cpu_allocated_bytes_per_frame | 137748.600 | 130516.230 | -7232.371 (-5.250%) | Diagnostic | Diagnostic |
| ServerRings | cpu_allocated_bytes_per_second | 4158494.803 | 3939981.837 | -218512.966 (-5.255%) | 4782269.024 | PASS |
| ServerRings | cpu_allocations | 131911.667 | 135583.667 | +3672.000 (2.784%) | Diagnostic | Diagnostic |
| ServerRings | cpu_allocations_per_frame | 873.587 | 897.905 | +24.318 (2.784%) | Diagnostic | Diagnostic |
| ServerRings | cpu_allocations_per_second | 26372.738 | 27105.669 | +732.931 (2.779%) | 30328.649 | PASS |
| ServerRings | cpu_live_blocks | 16362.000 | 17037.000 | +675.000 (4.125%) | 18816.300 | PASS |
| ServerRings | cpu_live_bytes | 18945597.000 | 19791339.000 | +845742.000 (4.464%) | 21787436.550 | PASS |
| ServerRings | cpu_peak_bytes | 18946966.000 | 19791547.000 | +844581.000 (4.458%) | 21789010.900 | PASS |
| ServerRings | cpu_process_peak_bytes | 18967941.000 | 19811618.333 | +843677.333 (4.448%) | 21813132.150 | PASS |
| ServerRings | cpu_profiler_overhead_bytes | 523584.000 | 545184.000 | +21600.000 (4.125%) | 1572160.000 | PASS |
| ServerRings | duration_seconds | 5.002 | 5.002 | +0.000 (0.004%) | Diagnostic | Diagnostic |
| ServerRings | frame_count | 151.000 | 151.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ServerRings | frame_ms_per_frame | 1.788 | 1.875 | +0.086 (4.836%) | 2.057 | PASS |
| ServerRings | frame_ms_per_second | 53.989 | 56.597 | +2.609 (4.832%) | 62.087 | PASS |
| ServerRings | framegraph_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ServerRings | framegraph_off_thread_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ServerRings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ServerRings | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 (0%) | 0.100 | PASS |
| ServerRings | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 (0%) | 5.000 | PASS |
| ServerRings | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 (0%) | 0.100 | PASS |
| ServerRings | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 (0%) | 5.000 | PASS |
| ServerRings | physics_ms_per_frame | 0.063 | 0.068 | +0.005 (7.588%) | 0.163 | PASS |
| ServerRings | physics_ms_per_second | 1.901 | 2.046 | +0.144 (7.583%) | 6.901 | PASS |
| ServerRings | physics_wall_ms_per_frame | 0.063 | 0.068 | +0.005 (7.588%) | 0.163 | PASS |
| ServerRings | physics_wall_ms_per_second | 1.901 | 2.046 | +0.144 (7.583%) | 6.901 | PASS |
| ServerRings | process_peak_rss_bytes | 32967338.667 | 35658410.667 | +2691072.000 (8.163%) | 37912439.467 | PASS |
| ServerRings | render_ms_per_frame | 0.002 | 0.001 | -0.000 (-22.064%) | 0.102 | PASS |
| ServerRings | render_ms_per_second | 0.045 | 0.035 | -0.010 (-22.068%) | 5.045 | PASS |
| ServerRings | render_wall_ms_per_frame | 0.002 | 0.001 | -0.000 (-22.064%) | 0.102 | PASS |
| ServerRings | render_wall_ms_per_second | 0.045 | 0.035 | -0.010 (-22.068%) | 5.045 | PASS |
| ServerRings | replication_ms_per_frame | 0.710 | 0.746 | +0.036 (5.073%) | 0.816 | PASS |
| ServerRings | replication_ms_per_second | 21.433 | 22.519 | +1.086 (5.068%) | 26.433 | PASS |
| ServerRings | replication_wall_ms_per_frame | 0.717 | 0.754 | +0.038 (5.246%) | 0.824 | PASS |
| ServerRings | replication_wall_ms_per_second | 21.638 | 22.772 | +1.134 (5.242%) | 26.638 | PASS |
| ServerRings | script_ms_per_frame | 0.672 | 0.679 | +0.007 (1.047%) | 0.773 | PASS |
| ServerRings | script_ms_per_second | 20.290 | 20.502 | +0.211 (1.042%) | 25.290 | PASS |
| ServerRings | tick_count | 151.000 | 151.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| ServerRings | tick_overruns | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| ServerRings | tick_rate_hz | 30.000 | 30.000 | 0.000 (0.000%) | Diagnostic | Diagnostic |
| StressPhysics | Demo input SHA-256 changed: d3d0c7b0d5a62f95dd5c983af37ed8a51d8c6393b5120317e5e3120c40d5346c to 158ac6af81322a0f482a1b4848a07cf46c269a16e9f96453a2c7d44375b0b0de | | | | | Input warning |
| StressPhysics | cpu_allocated_bytes | 2450652952.667 | 944479327.667 | -1506173625.000 (-61.460%) | Diagnostic | Diagnostic |
| StressPhysics | cpu_allocated_bytes_per_frame | 51413562.202 | 22319669.495 | -29093892.706 (-56.588%) | Diagnostic | Diagnostic |
| StressPhysics | cpu_allocated_bytes_per_second | 483993255.776 | 187469525.589 | -296523730.186 (-61.266%) | 556592244.142 | PASS |
| StressPhysics | cpu_allocations | 148004.667 | 146179.333 | -1825.333 (-1.233%) | Diagnostic | Diagnostic |
| StressPhysics | cpu_allocations_per_frame | 3105.275 | 3454.685 | +349.411 (11.252%) | Diagnostic | Diagnostic |
| StressPhysics | cpu_allocations_per_second | 29231.473 | 29015.524 | -215.949 (-0.739%) | 33616.194 | PASS |
| StressPhysics | cpu_live_blocks | 223100.000 | 224142.000 | +1042.000 (0.467%) | 256565.000 | PASS |
| StressPhysics | cpu_live_bytes | 587858939.000 | 735845026.000 | +147986087.000 (25.174%) | 676037779.850 | FAIL |
| StressPhysics | cpu_peak_bytes | 587858939.000 | 735845026.000 | +147986087.000 (25.174%) | 676037779.850 | FAIL |
| StressPhysics | cpu_process_peak_bytes | 587932763.000 | 739922920.000 | +151990157.000 (25.852%) | 676122677.450 | FAIL |
| StressPhysics | cpu_profiler_overhead_bytes | 7139200.000 | 7172544.000 | +33344.000 (0.467%) | 8210080.000 | PASS |
| StressPhysics | draw_calls_per_frame | 16.000 | 19.000 | +3.000 (18.750%) | Diagnostic | Diagnostic |
| StressPhysics | duration_seconds | 5.063 | 5.038 | -0.025 (-0.492%) | Diagnostic | Diagnostic |
| StressPhysics | frame_count | 47.667 | 42.333 | -5.333 (-11.189%) | Diagnostic | Diagnostic |
| StressPhysics | frame_ms_per_frame | 106.159 | 118.989 | +12.831 (12.086%) | 122.082 | PASS |
| StressPhysics | frame_ms_per_second | 999.344 | 999.371 | +0.028 (0.003%) | 1149.245 | PASS |
| StressPhysics | framegraph_dropped_spans | 474.667 | 472.000 | -2.667 (-0.562%) | Diagnostic | Diagnostic |
| StressPhysics | framegraph_off_thread_dropped_spans | 474.667 | 472.000 | -2.667 (-0.562%) | Diagnostic | Diagnostic |
| StressPhysics | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| StressPhysics | gpu_allocated_bytes | 133084224.000 | 133084320.000 | +96.000 (0.000%) | Diagnostic | Diagnostic |
| StressPhysics | gpu_allocated_bytes_per_frame | 2792251.745 | 3145309.132 | +353057.387 (12.644%) | Diagnostic | Diagnostic |
| StressPhysics | gpu_allocated_bytes_per_second | 26284757.736 | 26416453.641 | +131695.905 (0.501%) | 30227471.397 | PASS |
| StressPhysics | gpu_live_bytes | 128645376.000 | 128645688.000 | +312.000 (0.000%) | 147942182.400 | PASS |
| StressPhysics | gpu_peak_bytes | 128645376.000 | 128645688.000 | +312.000 (0.000%) | 147942182.400 | PASS |
| StressPhysics | gpu_resources_created | 43.000 | 45.000 | +2.000 (4.651%) | Diagnostic | Diagnostic |
| StressPhysics | gpu_resources_created_per_frame | 0.902 | 1.064 | +0.161 (17.883%) | Diagnostic | Diagnostic |
| StressPhysics | gpu_resources_created_per_second | 8.493 | 8.932 | +0.440 (5.175%) | 1008.493 | PASS |
| StressPhysics | imagegraph_ms_per_frame | 0.000 | 0.221 | +0.221 (new nonzero) | 0.100 | FAIL |
| StressPhysics | imagegraph_ms_per_second | 0.000 | 1.855 | +1.855 (new nonzero) | 5.000 | PASS |
| StressPhysics | imagegraph_wall_ms_per_frame | 0.000 | 0.221 | +0.221 (new nonzero) | 0.100 | FAIL |
| StressPhysics | imagegraph_wall_ms_per_second | 0.000 | 1.855 | +1.855 (new nonzero) | 5.000 | PASS |
| StressPhysics | physics_ms_per_frame | 25.658 | 26.560 | +0.901 (3.513%) | 29.507 | PASS |
| StressPhysics | physics_ms_per_second | 241.539 | 223.130 | -18.409 (-7.621%) | 277.770 | PASS |
| StressPhysics | physics_wall_ms_per_frame | 25.658 | 26.560 | +0.901 (3.513%) | 29.507 | PASS |
| StressPhysics | physics_wall_ms_per_second | 241.539 | 223.130 | -18.409 (-7.621%) | 277.770 | PASS |
| StressPhysics | process_peak_rss_bytes | 763853482.667 | 916630186.667 | +152776704.000 (20.001%) | 878431505.067 | FAIL |
| StressPhysics | render_ms_per_frame | 48.518 | 58.806 | +10.288 (21.205%) | 55.796 | FAIL |
| StressPhysics | render_ms_per_second | 456.740 | 493.773 | +37.033 (8.108%) | 525.251 | PASS |
| StressPhysics | render_wall_ms_per_frame | 48.534 | 58.825 | +10.290 (21.202%) | 55.814 | FAIL |
| StressPhysics | render_wall_ms_per_second | 456.893 | 493.926 | +37.033 (8.105%) | 525.427 | PASS |
| StressPhysics | replication_ms_per_frame | 0.002 | 0.002 | +0.000 (14.185%) | 0.102 | PASS |
| StressPhysics | replication_ms_per_second | 0.016 | 0.016 | +0.000 (1.987%) | 5.016 | PASS |
| StressPhysics | replication_wall_ms_per_frame | 0.002 | 0.002 | +0.000 (14.185%) | 0.102 | PASS |
| StressPhysics | replication_wall_ms_per_second | 0.016 | 0.016 | +0.000 (1.987%) | 5.016 | PASS |
| StressPhysics | script_ms_per_frame | 0.046 | 0.054 | +0.009 (18.607%) | 0.146 | PASS |
| StressPhysics | script_ms_per_second | 0.431 | 0.456 | +0.025 (5.846%) | 5.431 | PASS |
| StressPhysics | submitted_frames | 47.667 | 42.333 | -5.333 (-11.189%) | Diagnostic | Diagnostic |
| StressPhysics | uploaded_bytes | 376152168.000 | 333483416.000 | -42668752.000 (-11.343%) | Diagnostic | Diagnostic |
| StressPhysics | uploaded_bytes_per_frame | 7891293.478 | 7877499.074 | -13794.404 (-0.175%) | Diagnostic | Diagnostic |
| StressPhysics | uploaded_bytes_per_second | 74287217.029 | 66186753.078 | -8100463.951 (-10.904%) | 85430299.584 | PASS |
| StressPhysics | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| StressPhysics | imagegraph_published_textures | | 0.000 | | | Added |
| Terrain | cpu_allocated_bytes | 839193915.667 | 538321989.000 | -300871926.667 (-35.852%) | Diagnostic | Diagnostic |
| Terrain | cpu_allocated_bytes_per_frame | 49005.680 | 39739.294 | -9266.386 (-18.909%) | Diagnostic | Diagnostic |
| Terrain | cpu_allocated_bytes_per_second | 167835606.144 | 107662549.913 | -60173056.231 (-35.852%) | 193010947.065 | PASS |
| Terrain | cpu_allocations | 3981804.000 | 2883181.000 | -1098623.000 (-27.591%) | Diagnostic | Diagnostic |
| Terrain | cpu_allocations_per_frame | 232.516 | 212.834 | -19.682 (-8.465%) | Diagnostic | Diagnostic |
| Terrain | cpu_allocations_per_second | 796345.722 | 576626.303 | -219719.419 (-27.591%) | 915797.580 | PASS |
| Terrain | cpu_live_blocks | 44852.000 | 45906.000 | +1054.000 (2.350%) | 51579.800 | PASS |
| Terrain | cpu_live_bytes | 96254746.333 | 123495401.333 | +27240655.000 (28.301%) | 110692958.283 | FAIL |
| Terrain | cpu_peak_bytes | 96254746.333 | 123495401.333 | +27240655.000 (28.301%) | 110692958.283 | FAIL |
| Terrain | cpu_process_peak_bytes | 96371327.667 | 123617783.333 | +27246455.667 (28.272%) | 110827026.817 | FAIL |
| Terrain | cpu_profiler_overhead_bytes | 1435264.000 | 1468992.000 | +33728.000 (2.350%) | 2483840.000 | PASS |
| Terrain | draw_calls_per_frame | 450.290 | 460.095 | +9.806 (2.178%) | Diagnostic | Diagnostic |
| Terrain | duration_seconds | 5.000 | 5.000 | -0.000 (-0.000%) | Diagnostic | Diagnostic |
| Terrain | frame_count | 17125.000 | 13546.667 | -3578.333 (-20.895%) | Diagnostic | Diagnostic |
| Terrain | frame_ms_per_frame | 0.281 | 0.356 | +0.074 (26.458%) | 0.381 | PASS |
| Terrain | frame_ms_per_second | 963.023 | 963.409 | +0.385 (0.040%) | 1107.477 | PASS |
| Terrain | framegraph_dropped_spans | 452.333 | 459.000 | +6.667 (1.474%) | Diagnostic | Diagnostic |
| Terrain | framegraph_off_thread_dropped_spans | 452.333 | 459.000 | +6.667 (1.474%) | Diagnostic | Diagnostic |
| Terrain | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 (0%) | Diagnostic | Diagnostic |
| Terrain | gpu_allocated_bytes | 219875712.000 | 220213416.000 | +337704.000 (0.154%) | Diagnostic | Diagnostic |
| Terrain | gpu_allocated_bytes_per_frame | 12841.082 | 16257.022 | +3415.941 (26.602%) | Diagnostic | Diagnostic |
| Terrain | gpu_allocated_bytes_per_second | 43974312.888 | 44041927.462 | +67614.573 (0.154%) | 50570459.821 | PASS |
| Terrain | gpu_live_bytes | 120495936.000 | 120606624.000 | +110688.000 (0.092%) | 138570326.400 | PASS |
| Terrain | gpu_peak_bytes | 131676144.000 | 131787048.000 | +110904.000 (0.084%) | 151427565.600 | PASS |
| Terrain | gpu_resources_created | 115.000 | 117.000 | +2.000 (1.739%) | Diagnostic | Diagnostic |
| Terrain | gpu_resources_created_per_frame | 0.007 | 0.009 | +0.002 (28.606%) | Diagnostic | Diagnostic |
| Terrain | gpu_resources_created_per_second | 23.000 | 23.400 | +0.400 (1.739%) | 1023.000 | PASS |
| Terrain | imagegraph_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Terrain | imagegraph_ms_per_second | 0.000 | 6.473 | +6.473 (new nonzero) | 5.000 | FAIL |
| Terrain | imagegraph_wall_ms_per_frame | 0.000 | 0.002 | +0.002 (new nonzero) | 0.100 | PASS |
| Terrain | imagegraph_wall_ms_per_second | 0.000 | 6.473 | +6.473 (new nonzero) | 5.000 | FAIL |
| Terrain | physics_ms_per_frame | 0.001 | 0.001 | +0.000 (77.605%) | 0.101 | PASS |
| Terrain | physics_ms_per_second | 1.921 | 2.702 | +0.781 (40.669%) | 6.921 | PASS |
| Terrain | physics_wall_ms_per_frame | 0.001 | 0.001 | +0.000 (77.605%) | 0.101 | PASS |
| Terrain | physics_wall_ms_per_second | 1.921 | 2.702 | +0.781 (40.669%) | 6.921 | PASS |
| Terrain | process_peak_rss_bytes | 286861994.667 | 321850026.667 | +34988032.000 (12.197%) | 329891293.867 | PASS |
| Terrain | render_ms_per_frame | 0.171 | 0.209 | +0.038 (22.476%) | 0.271 | PASS |
| Terrain | render_ms_per_second | 584.694 | 566.480 | -18.213 (-3.115%) | 672.398 | PASS |
| Terrain | render_wall_ms_per_frame | 0.172 | 0.211 | +0.039 (22.396%) | 0.272 | PASS |
| Terrain | render_wall_ms_per_second | 590.398 | 571.631 | -18.767 (-3.179%) | 678.957 | PASS |
| Terrain | replication_ms_per_frame | 0.000 | 0.000 | +0.000 (6.372%) | 0.100 | PASS |
| Terrain | replication_ms_per_second | 0.921 | 0.775 | -0.146 (-15.872%) | 5.921 | PASS |
| Terrain | replication_wall_ms_per_frame | 0.000 | 0.000 | +0.000 (6.372%) | 0.100 | PASS |
| Terrain | replication_wall_ms_per_second | 0.921 | 0.775 | -0.146 (-15.872%) | 5.921 | PASS |
| Terrain | script_ms_per_frame | 0.073 | 0.098 | +0.024 (33.026%) | 0.173 | PASS |
| Terrain | script_ms_per_second | 251.127 | 264.293 | +13.167 (5.243%) | 288.795 | PASS |
| Terrain | submitted_frames | 17125.000 | 13546.667 | -3578.333 (-20.895%) | Diagnostic | Diagnostic |
| Terrain | uploaded_bytes | 24726.667 | 24054.667 | -672.000 (-2.718%) | Diagnostic | Diagnostic |
| Terrain | uploaded_bytes_per_frame | 1.444 | 1.776 | +0.332 (22.977%) | Diagnostic | Diagnostic |
| Terrain | uploaded_bytes_per_second | 4945.240 | 4810.851 | -134.389 (-2.718%) | 70481.240 | PASS |
| Terrain | imagegraph_diagnostic_frames | | 0.000 | | | Added |
| Terrain | imagegraph_published_textures | | 0.000 | | | Added |

### v0.25

Source: `24be7b2c61918b5df9d04fc59cd85ebcef69a6ff` (clean). Source diff SHA-256: `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`. Three independent five-second runs.

```json
{
  "compiler": "c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0\nCopyright (C) 2023 Free Software Foundation, Inc.\nThis is free software; see the source for copying conditions.  There is NO\nwarranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.",
  "cpu": "AMD Ryzen 9 9900X 12-Core Processor",
  "fingerprint": "5675a8d8002198eeac91db34876ea236c284ac3be38542a446379a7671fa438a",
  "gpu": "NVIDIA GeForce RTX 4090, 580.173.02",
  "os": "Linux 7.0.0-38-generic #38~24.04.4-Ubuntu SMP PREEMPT_DYNAMIC Mon Sep 14 16:37:11 UTC 2 x86_64 GNU/Linux"
}
{
  "compute": "serial",
  "height": 540,
  "max_fps": 0,
  "preset": "profile",
  "replica_viewer_max_fps": 60,
  "runs": 3,
  "seconds": 5,
  "width": 960
}
```

| Demo | Metric | Mean | Population stddev | Minimum | Maximum |
|---|---|---:|---:|---:|---:|
| Cube | cpu_allocated_bytes | 1102796768.667 | 8385680.702 | 1091293390.000 | 1111045190.000 |
| Cube | cpu_allocated_bytes_per_frame | 4860048.024 | 108165.518 | 4744753.870 | 5004758.941 |
| Cube | cpu_allocated_bytes_per_second | 220147993.752 | 1955300.701 | 217412105.135 | 221863768.988 |
| Cube | cpu_allocations | 817908.667 | 4375.312 | 811802.000 | 821826.000 |
| Cube | cpu_allocations_per_frame | 3604.527 | 77.259 | 3529.574 | 3710.851 |
| Cube | cpu_allocations_per_second | 163276.245 | 1094.031 | 161730.643 | 164109.809 |
| Cube | cpu_live_blocks | 48708.667 | 3.300 | 48704.000 | 48711.000 |
| Cube | cpu_live_bytes | 179305615.667 | 78123.043 | 179195133.000 | 179360857.000 |
| Cube | cpu_peak_bytes | 179305615.667 | 78123.043 | 179195133.000 | 179360857.000 |
| Cube | cpu_process_peak_bytes | 185564943.667 | 105174.120 | 185416205.000 | 185639313.000 |
| Cube | cpu_profiler_overhead_bytes | 1558677.333 | 105.595 | 1558528.000 | 1558752.000 |
| Cube | draw_calls_per_frame | 19.000 | 0.000 | 19.000 | 19.000 |
| Cube | duration_seconds | 5.009 | 0.008 | 5.001 | 5.019 |
| Cube | frame_count | 227.000 | 4.243 | 221.000 | 230.000 |
| Cube | frame_ms_per_frame | 22.010 | 0.391 | 21.706 | 22.562 |
| Cube | frame_ms_per_second | 997.044 | 0.094 | 996.923 | 997.150 |
| Cube | framegraph_dropped_spans | 470.667 | 0.943 | 470.000 | 472.000 |
| Cube | framegraph_off_thread_dropped_spans | 470.667 | 0.943 | 470.000 | 472.000 |
| Cube | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | gpu_allocated_bytes | 108442784.000 | 0.000 | 108442784.000 | 108442784.000 |
| Cube | gpu_allocated_bytes_per_frame | 477890.687 | 9051.422 | 471490.365 | 490691.330 |
| Cube | gpu_allocated_bytes_per_second | 21647901.581 | 33021.433 | 21604432.110 | 21684415.571 |
| Cube | gpu_live_bytes | 103873080.000 | 0.000 | 103873080.000 | 103873080.000 |
| Cube | gpu_peak_bytes | 103873080.000 | 0.000 | 103873080.000 | 103873080.000 |
| Cube | gpu_resources_created | 47.000 | 0.000 | 47.000 | 47.000 |
| Cube | gpu_resources_created_per_frame | 0.207 | 0.004 | 0.204 | 0.213 |
| Cube | gpu_resources_created_per_second | 9.382 | 0.014 | 9.364 | 9.398 |
| Cube | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | imagegraph_ms_per_frame | 0.019 | 0.002 | 0.017 | 0.021 |
| Cube | imagegraph_ms_per_second | 0.874 | 0.074 | 0.778 | 0.957 |
| Cube | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| Cube | imagegraph_wall_ms_per_frame | 0.019 | 0.002 | 0.017 | 0.021 |
| Cube | imagegraph_wall_ms_per_second | 0.874 | 0.074 | 0.778 | 0.957 |
| Cube | physics_ms_per_frame | 14.420 | 0.237 | 14.209 | 14.751 |
| Cube | physics_ms_per_second | 653.255 | 2.525 | 651.083 | 656.796 |
| Cube | physics_wall_ms_per_frame | 14.420 | 0.237 | 14.209 | 14.751 |
| Cube | physics_wall_ms_per_second | 653.255 | 2.525 | 651.083 | 656.796 |
| Cube | process_peak_rss_bytes | 365726378.667 | 1248800.176 | 364318720.000 | 367353856.000 |
| Cube | render_ms_per_frame | 4.596 | 0.150 | 4.427 | 4.791 |
| Cube | render_ms_per_second | 208.352 | 9.823 | 195.632 | 219.548 |
| Cube | render_wall_ms_per_frame | 4.608 | 0.149 | 4.439 | 4.802 |
| Cube | render_wall_ms_per_second | 208.892 | 9.802 | 196.180 | 220.036 |
| Cube | replication_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.002 |
| Cube | replication_ms_per_second | 0.066 | 0.004 | 0.062 | 0.071 |
| Cube | replication_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.002 |
| Cube | replication_wall_ms_per_second | 0.066 | 0.004 | 0.062 | 0.071 |
| Cube | script_ms_per_frame | 0.012 | 0.001 | 0.012 | 0.013 |
| Cube | script_ms_per_second | 0.565 | 0.021 | 0.537 | 0.586 |
| Cube | submitted_frames | 227.000 | 4.243 | 221.000 | 230.000 |
| Cube | uploaded_bytes | 182163181.333 | 3430939.636 | 177311100.000 | 184589700.000 |
| Cube | uploaded_bytes_per_frame | 802478.780 | 117.470 | 802312.670 | 802563.913 |
| Cube | uploaded_bytes_per_second | 36363526.306 | 643058.923 | 35455448.818 | 36860385.901 |
| ImageGraph3D | cpu_allocated_bytes | 2473377848.667 | 62992240.434 | 2394023748.000 | 2548115255.000 |
| ImageGraph3D | cpu_allocated_bytes_per_frame | 3986369.910 | 38354.031 | 3944450.859 | 4037139.541 |
| ImageGraph3D | cpu_allocated_bytes_per_second | 494022229.199 | 12643860.318 | 478199305.094 | 509146673.312 |
| ImageGraph3D | cpu_allocations | 1269064.000 | 24736.047 | 1238163.000 | 1298715.000 |
| ImageGraph3D | cpu_allocations_per_frame | 2045.796 | 32.027 | 2010.395 | 2087.965 |
| ImageGraph3D | cpu_allocations_per_second | 253477.425 | 4973.681 | 247319.471 | 259500.201 |
| ImageGraph3D | cpu_live_blocks | 25691.667 | 20.072 | 25666.000 | 25715.000 |
| ImageGraph3D | cpu_live_bytes | 66341505.333 | 59919.806 | 66266210.000 | 66412821.000 |
| ImageGraph3D | cpu_peak_bytes | 66413086.667 | 375.709 | 66412821.000 | 66413618.000 |
| ImageGraph3D | cpu_process_peak_bytes | 72174036.667 | 375.709 | 72173771.000 | 72174568.000 |
| ImageGraph3D | cpu_profiler_overhead_bytes | 822133.333 | 642.307 | 821312.000 | 822880.000 |
| ImageGraph3D | draw_calls_per_frame | 37.000 | 0.000 | 37.000 | 37.000 |
| ImageGraph3D | duration_seconds | 5.007 | 0.002 | 5.005 | 5.009 |
| ImageGraph3D | frame_count | 620.667 | 21.700 | 593.000 | 646.000 |
| ImageGraph3D | frame_ms_per_frame | 8.028 | 0.283 | 7.700 | 8.391 |
| ImageGraph3D | frame_ms_per_second | 993.985 | 0.042 | 993.942 | 994.041 |
| ImageGraph3D | framegraph_dropped_spans | 465.667 | 2.055 | 463.000 | 468.000 |
| ImageGraph3D | framegraph_off_thread_dropped_spans | 465.667 | 2.055 | 463.000 | 468.000 |
| ImageGraph3D | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraph3D | gpu_allocated_bytes | 2344338152.000 | 51617513.275 | 2278221600.000 | 2404192000.000 |
| ImageGraph3D | gpu_allocated_bytes_per_frame | 3778850.732 | 49242.732 | 3721659.443 | 3841857.673 |
| ImageGraph3D | gpu_allocated_bytes_per_second | 468248070.289 | 10363237.440 | 455068161.659 | 480388929.191 |
| ImageGraph3D | gpu_live_bytes | 101213493.333 | 339950.542 | 100973112.000 | 101694256.000 |
| ImageGraph3D | gpu_peak_bytes | 111634696.000 | 123579.638 | 111547312.000 | 111809464.000 |
| ImageGraph3D | gpu_resources_created | 10611.667 | 184.846 | 10378.000 | 10830.000 |
| ImageGraph3D | gpu_resources_created_per_frame | 17.108 | 0.303 | 16.765 | 17.501 |
| ImageGraph3D | gpu_resources_created_per_second | 2119.528 | 37.180 | 2072.975 | 2163.975 |
| ImageGraph3D | imagegraph_diagnostic_frames | 620.667 | 21.700 | 593.000 | 646.000 |
| ImageGraph3D | imagegraph_ms_per_frame | 5.246 | 0.048 | 5.209 | 5.314 |
| ImageGraph3D | imagegraph_ms_per_second | 650.184 | 17.906 | 629.486 | 673.168 |
| ImageGraph3D | imagegraph_published_textures | 1.000 | 0.000 | 1.000 | 1.000 |
| ImageGraph3D | imagegraph_wall_ms_per_frame | 5.246 | 0.048 | 5.209 | 5.314 |
| ImageGraph3D | imagegraph_wall_ms_per_second | 650.184 | 17.906 | 629.486 | 673.168 |
| ImageGraph3D | physics_ms_per_frame | 0.018 | 0.001 | 0.017 | 0.020 |
| ImageGraph3D | physics_ms_per_second | 2.271 | 0.065 | 2.180 | 2.318 |
| ImageGraph3D | physics_wall_ms_per_frame | 0.018 | 0.001 | 0.017 | 0.020 |
| ImageGraph3D | physics_wall_ms_per_second | 2.271 | 0.065 | 2.180 | 2.318 |
| ImageGraph3D | process_peak_rss_bytes | 308132522.667 | 4802703.178 | 303607808.000 | 314781696.000 |
| ImageGraph3D | render_ms_per_frame | 5.444 | 0.210 | 5.179 | 5.693 |
| ImageGraph3D | render_ms_per_second | 673.962 | 4.337 | 668.452 | 679.051 |
| ImageGraph3D | render_wall_ms_per_frame | 7.336 | 0.268 | 7.024 | 7.679 |
| ImageGraph3D | render_wall_ms_per_second | 908.294 | 1.205 | 906.685 | 909.584 |
| ImageGraph3D | replication_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ImageGraph3D | replication_ms_per_second | 0.114 | 0.004 | 0.109 | 0.118 |
| ImageGraph3D | replication_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ImageGraph3D | replication_wall_ms_per_second | 0.114 | 0.004 | 0.109 | 0.118 |
| ImageGraph3D | script_ms_per_frame | 0.009 | 0.000 | 0.008 | 0.009 |
| ImageGraph3D | script_ms_per_second | 1.075 | 0.011 | 1.061 | 1.085 |
| ImageGraph3D | submitted_frames | 620.667 | 21.700 | 593.000 | 646.000 |
| ImageGraph3D | uploaded_bytes | 1156.000 | 0.000 | 1156.000 | 1156.000 |
| ImageGraph3D | uploaded_bytes_per_frame | 1.865 | 0.066 | 1.789 | 1.949 |
| ImageGraph3D | uploaded_bytes_per_second | 230.894 | 0.080 | 230.790 | 230.984 |
| ImageGraphFeedback | cpu_allocated_bytes | 1310204811.000 | 21628462.754 | 1282637307.000 | 1335465067.000 |
| ImageGraphFeedback | cpu_allocated_bytes_per_frame | 58410.387 | 212.461 | 58164.855 | 58683.136 |
| ImageGraphFeedback | cpu_allocated_bytes_per_second | 262035249.146 | 4329300.519 | 256518101.362 | 267092752.076 |
| ImageGraphFeedback | cpu_allocations | 6598616.667 | 120965.446 | 6444470.000 | 6739940.000 |
| ImageGraphFeedback | cpu_allocations_per_frame | 294.163 | 0.531 | 293.551 | 294.847 |
| ImageGraphFeedback | cpu_allocations_per_second | 1319694.595 | 24211.215 | 1288846.972 | 1347986.681 |
| ImageGraphFeedback | cpu_live_blocks | 25927.667 | 0.471 | 25927.000 | 25928.000 |
| ImageGraphFeedback | cpu_live_bytes | 64968122.000 | 11.314 | 64968106.000 | 64968130.000 |
| ImageGraphFeedback | cpu_peak_bytes | 64970434.000 | 0.000 | 64970434.000 | 64970434.000 |
| ImageGraphFeedback | cpu_process_peak_bytes | 65099636.000 | 78.384 | 65099540.000 | 65099732.000 |
| ImageGraphFeedback | cpu_profiler_overhead_bytes | 829685.333 | 15.085 | 829664.000 | 829696.000 |
| ImageGraphFeedback | draw_calls_per_frame | 26.000 | 0.000 | 26.000 | 26.000 |
| ImageGraphFeedback | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| ImageGraphFeedback | frame_count | 22432.667 | 451.593 | 21857.000 | 22960.000 |
| ImageGraphFeedback | frame_ms_per_frame | 0.213 | 0.004 | 0.208 | 0.218 |
| ImageGraphFeedback | frame_ms_per_second | 955.261 | 0.664 | 954.680 | 956.191 |
| ImageGraphFeedback | framegraph_dropped_spans | 448.333 | 6.342 | 442.000 | 457.000 |
| ImageGraphFeedback | framegraph_off_thread_dropped_spans | 448.333 | 6.342 | 442.000 | 457.000 |
| ImageGraphFeedback | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFeedback | gpu_allocated_bytes | 109433832.000 | 0.000 | 109433832.000 | 109433832.000 |
| ImageGraphFeedback | gpu_allocated_bytes_per_frame | 4880.309 | 98.590 | 4766.282 | 5006.809 |
| ImageGraphFeedback | gpu_allocated_bytes_per_second | 21886284.126 | 333.337 | 21885967.807 | 21886744.986 |
| ImageGraphFeedback | gpu_live_bytes | 104586624.000 | 0.000 | 104586624.000 | 104586624.000 |
| ImageGraphFeedback | gpu_peak_bytes | 104594816.000 | 0.000 | 104594816.000 | 104594816.000 |
| ImageGraphFeedback | gpu_resources_created | 216.000 | 0.000 | 216.000 | 216.000 |
| ImageGraphFeedback | gpu_resources_created_per_frame | 0.010 | 0.000 | 0.009 | 0.010 |
| ImageGraphFeedback | gpu_resources_created_per_second | 43.199 | 0.001 | 43.198 | 43.200 |
| ImageGraphFeedback | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFeedback | imagegraph_ms_per_frame | 0.022 | 0.000 | 0.021 | 0.022 |
| ImageGraphFeedback | imagegraph_ms_per_second | 97.691 | 0.111 | 97.536 | 97.796 |
| ImageGraphFeedback | imagegraph_published_textures | 3.000 | 0.000 | 3.000 | 3.000 |
| ImageGraphFeedback | imagegraph_wall_ms_per_frame | 0.022 | 0.000 | 0.021 | 0.022 |
| ImageGraphFeedback | imagegraph_wall_ms_per_second | 97.691 | 0.111 | 97.536 | 97.796 |
| ImageGraphFeedback | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFeedback | physics_ms_per_second | 1.998 | 0.030 | 1.957 | 2.030 |
| ImageGraphFeedback | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFeedback | physics_wall_ms_per_second | 1.998 | 0.030 | 1.957 | 2.030 |
| ImageGraphFeedback | process_peak_rss_bytes | 268671658.667 | 1082782.504 | 267837440.000 | 270200832.000 |
| ImageGraphFeedback | render_ms_per_frame | 0.167 | 0.003 | 0.163 | 0.171 |
| ImageGraphFeedback | render_ms_per_second | 747.999 | 1.148 | 746.514 | 749.310 |
| ImageGraphFeedback | render_wall_ms_per_frame | 0.174 | 0.003 | 0.170 | 0.178 |
| ImageGraphFeedback | render_wall_ms_per_second | 779.734 | 1.661 | 777.702 | 781.771 |
| ImageGraphFeedback | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFeedback | replication_ms_per_second | 1.301 | 0.020 | 1.275 | 1.323 |
| ImageGraphFeedback | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFeedback | replication_wall_ms_per_second | 1.301 | 0.020 | 1.275 | 1.323 |
| ImageGraphFeedback | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFeedback | script_ms_per_second | 1.946 | 0.021 | 1.917 | 1.969 |
| ImageGraphFeedback | submitted_frames | 22432.667 | 451.593 | 21857.000 | 22960.000 |
| ImageGraphFeedback | uploaded_bytes | 508.000 | 0.000 | 508.000 | 508.000 |
| ImageGraphFeedback | uploaded_bytes_per_frame | 0.023 | 0.000 | 0.022 | 0.023 |
| ImageGraphFeedback | uploaded_bytes_per_second | 101.598 | 0.002 | 101.596 | 101.600 |
| ImageGraphFlipbook | cpu_allocated_bytes | 800600991.333 | 19701689.658 | 773076136.000 | 818107964.000 |
| ImageGraphFlipbook | cpu_allocated_bytes_per_frame | 32630.109 | 51.590 | 32583.558 | 32702.036 |
| ImageGraphFlipbook | cpu_allocated_bytes_per_second | 160118083.592 | 3939410.932 | 154614013.604 | 163616675.955 |
| ImageGraphFlipbook | cpu_allocations | 6002995.667 | 154700.283 | 5786685.000 | 6139533.000 |
| ImageGraphFlipbook | cpu_allocations_per_frame | 244.657 | 0.106 | 244.525 | 244.784 |
| ImageGraphFlipbook | cpu_allocations_per_second | 1200583.272 | 30933.163 | 1157327.916 | 1227869.701 |
| ImageGraphFlipbook | cpu_live_blocks | 17569.667 | 0.471 | 17569.000 | 17570.000 |
| ImageGraphFlipbook | cpu_live_bytes | 59843523.000 | 130.476 | 59843419.000 | 59843707.000 |
| ImageGraphFlipbook | cpu_peak_bytes | 59843531.000 | 124.451 | 59843443.000 | 59843707.000 |
| ImageGraphFlipbook | cpu_process_peak_bytes | 59974747.000 | 124.451 | 59974659.000 | 59974923.000 |
| ImageGraphFlipbook | cpu_profiler_overhead_bytes | 562229.333 | 15.085 | 562208.000 | 562240.000 |
| ImageGraphFlipbook | draw_calls_per_frame | 22.998 | 0.000 | 22.998 | 22.998 |
| ImageGraphFlipbook | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| ImageGraphFlipbook | frame_count | 24536.667 | 641.944 | 23640.000 | 25108.000 |
| ImageGraphFlipbook | frame_ms_per_frame | 0.195 | 0.005 | 0.190 | 0.202 |
| ImageGraphFlipbook | frame_ms_per_second | 954.661 | 0.209 | 954.513 | 954.957 |
| ImageGraphFlipbook | framegraph_dropped_spans | 446.000 | 5.354 | 439.000 | 452.000 |
| ImageGraphFlipbook | framegraph_off_thread_dropped_spans | 446.000 | 5.354 | 439.000 | 452.000 |
| ImageGraphFlipbook | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFlipbook | gpu_allocated_bytes | 108165552.000 | 0.000 | 108165552.000 | 108165552.000 |
| ImageGraphFlipbook | gpu_allocated_bytes_per_frame | 4411.393 | 117.360 | 4308.011 | 4575.531 |
| ImageGraphFlipbook | gpu_allocated_bytes_per_second | 21632827.607 | 266.039 | 21632460.323 | 21633081.901 |
| ImageGraphFlipbook | gpu_live_bytes | 103564908.000 | 0.000 | 103564908.000 | 103564908.000 |
| ImageGraphFlipbook | gpu_peak_bytes | 103565012.000 | 0.000 | 103565012.000 | 103565012.000 |
| ImageGraphFlipbook | gpu_resources_created | 662.000 | 0.000 | 662.000 | 662.000 |
| ImageGraphFlipbook | gpu_resources_created_per_frame | 0.027 | 0.001 | 0.026 | 0.028 |
| ImageGraphFlipbook | gpu_resources_created_per_second | 132.398 | 0.002 | 132.396 | 132.400 |
| ImageGraphFlipbook | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFlipbook | imagegraph_ms_per_frame | 0.009 | 0.000 | 0.009 | 0.010 |
| ImageGraphFlipbook | imagegraph_ms_per_second | 46.393 | 0.769 | 45.376 | 47.236 |
| ImageGraphFlipbook | imagegraph_published_textures | 2.000 | 0.000 | 2.000 | 2.000 |
| ImageGraphFlipbook | imagegraph_wall_ms_per_frame | 0.009 | 0.000 | 0.009 | 0.010 |
| ImageGraphFlipbook | imagegraph_wall_ms_per_second | 46.393 | 0.769 | 45.376 | 47.236 |
| ImageGraphFlipbook | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFlipbook | physics_ms_per_second | 1.838 | 0.061 | 1.761 | 1.911 |
| ImageGraphFlipbook | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFlipbook | physics_wall_ms_per_second | 1.838 | 0.061 | 1.761 | 1.911 |
| ImageGraphFlipbook | process_peak_rss_bytes | 278766933.333 | 2218571.301 | 276332544.000 | 281698304.000 |
| ImageGraphFlipbook | render_ms_per_frame | 0.160 | 0.004 | 0.156 | 0.165 |
| ImageGraphFlipbook | render_ms_per_second | 783.985 | 1.184 | 782.311 | 784.874 |
| ImageGraphFlipbook | render_wall_ms_per_frame | 0.163 | 0.004 | 0.159 | 0.168 |
| ImageGraphFlipbook | render_wall_ms_per_second | 798.460 | 1.335 | 796.592 | 799.632 |
| ImageGraphFlipbook | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFlipbook | replication_ms_per_second | 1.284 | 0.036 | 1.250 | 1.333 |
| ImageGraphFlipbook | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFlipbook | replication_wall_ms_per_second | 1.284 | 0.036 | 1.250 | 1.333 |
| ImageGraphFlipbook | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphFlipbook | script_ms_per_second | 1.876 | 0.037 | 1.850 | 1.928 |
| ImageGraphFlipbook | submitted_frames | 24536.667 | 641.944 | 23640.000 | 25108.000 |
| ImageGraphFlipbook | uploaded_bytes | 64764965.333 | 1689563.835 | 62408208.000 | 66284240.000 |
| ImageGraphFlipbook | uploaded_bytes_per_frame | 2639.523 | 0.608 | 2638.663 | 2639.965 |
| ImageGraphFlipbook | uploaded_bytes_per_second | 12952821.858 | 337836.089 | 12481543.630 | 13256449.630 |
| ImageGraphGui | cpu_allocated_bytes | 1217952036.000 | 14238003.629 | 1201780963.000 | 1236427636.000 |
| ImageGraphGui | cpu_allocated_bytes_per_frame | 51052.813 | 49.433 | 50988.809 | 51109.167 |
| ImageGraphGui | cpu_allocated_bytes_per_second | 243583936.324 | 2847025.241 | 240348434.633 | 247277012.612 |
| ImageGraphGui | cpu_allocations | 5443095.667 | 67886.324 | 5365878.000 | 5531110.000 |
| ImageGraphGui | cpu_allocations_per_frame | 228.156 | 0.043 | 228.096 | 228.199 |
| ImageGraphGui | cpu_allocations_per_second | 1088590.213 | 13574.685 | 1073140.961 | 1106183.910 |
| ImageGraphGui | cpu_live_blocks | 20349.000 | 0.000 | 20349.000 | 20349.000 |
| ImageGraphGui | cpu_live_bytes | 79982920.000 | 0.000 | 79982920.000 | 79982920.000 |
| ImageGraphGui | cpu_peak_bytes | 79982920.000 | 0.000 | 79982920.000 | 79982920.000 |
| ImageGraphGui | cpu_process_peak_bytes | 99712087.000 | 0.000 | 99712087.000 | 99712087.000 |
| ImageGraphGui | cpu_profiler_overhead_bytes | 651168.000 | 0.000 | 651168.000 | 651168.000 |
| ImageGraphGui | draw_calls_per_frame | 22.000 | 0.000 | 22.000 | 22.000 |
| ImageGraphGui | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| ImageGraphGui | frame_count | 23857.000 | 302.056 | 23514.000 | 24249.000 |
| ImageGraphGui | frame_ms_per_frame | 0.200 | 0.003 | 0.196 | 0.203 |
| ImageGraphGui | frame_ms_per_second | 953.833 | 0.990 | 952.448 | 954.701 |
| ImageGraphGui | framegraph_dropped_spans | 448.333 | 4.028 | 445.000 | 454.000 |
| ImageGraphGui | framegraph_off_thread_dropped_spans | 448.333 | 4.028 | 445.000 | 454.000 |
| ImageGraphGui | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphGui | gpu_allocated_bytes | 105217528.000 | 0.000 | 105217528.000 | 105217528.000 |
| ImageGraphGui | gpu_allocated_bytes_per_frame | 4411.047 | 55.686 | 4339.046 | 4474.676 |
| ImageGraphGui | gpu_allocated_bytes_per_second | 21042947.074 | 203.602 | 21042781.026 | 21043233.818 |
| ImageGraphGui | gpu_live_bytes | 100582288.000 | 0.000 | 100582288.000 | 100582288.000 |
| ImageGraphGui | gpu_peak_bytes | 100582288.000 | 0.000 | 100582288.000 | 100582288.000 |
| ImageGraphGui | gpu_resources_created | 57.000 | 0.000 | 57.000 | 57.000 |
| ImageGraphGui | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| ImageGraphGui | gpu_resources_created_per_second | 11.400 | 0.000 | 11.400 | 11.400 |
| ImageGraphGui | imagegraph_diagnostic_frames | 23857.000 | 302.056 | 23514.000 | 24249.000 |
| ImageGraphGui | imagegraph_ms_per_frame | 0.003 | 0.000 | 0.003 | 0.004 |
| ImageGraphGui | imagegraph_ms_per_second | 16.602 | 0.380 | 16.319 | 17.139 |
| ImageGraphGui | imagegraph_published_textures | 3.000 | 0.000 | 3.000 | 3.000 |
| ImageGraphGui | imagegraph_wall_ms_per_frame | 0.003 | 0.000 | 0.003 | 0.004 |
| ImageGraphGui | imagegraph_wall_ms_per_second | 16.602 | 0.380 | 16.319 | 17.139 |
| ImageGraphGui | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphGui | physics_ms_per_second | 1.788 | 0.048 | 1.731 | 1.848 |
| ImageGraphGui | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphGui | physics_wall_ms_per_second | 1.788 | 0.048 | 1.731 | 1.848 |
| ImageGraphGui | process_peak_rss_bytes | 289412437.333 | 1278097.861 | 287744000.000 | 290848768.000 |
| ImageGraphGui | render_ms_per_frame | 0.154 | 0.002 | 0.151 | 0.156 |
| ImageGraphGui | render_ms_per_second | 732.698 | 0.341 | 732.347 | 733.161 |
| ImageGraphGui | render_wall_ms_per_frame | 0.159 | 0.002 | 0.156 | 0.161 |
| ImageGraphGui | render_wall_ms_per_second | 758.722 | 0.251 | 758.411 | 759.025 |
| ImageGraphGui | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphGui | replication_ms_per_second | 1.326 | 0.090 | 1.220 | 1.440 |
| ImageGraphGui | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphGui | replication_wall_ms_per_second | 1.326 | 0.090 | 1.220 | 1.440 |
| ImageGraphGui | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphGui | script_ms_per_second | 0.859 | 0.020 | 0.831 | 0.880 |
| ImageGraphGui | submitted_frames | 23857.000 | 302.056 | 23514.000 | 24249.000 |
| ImageGraphGui | uploaded_bytes | 292.000 | 0.000 | 292.000 | 292.000 |
| ImageGraphGui | uploaded_bytes_per_frame | 0.012 | 0.000 | 0.012 | 0.012 |
| ImageGraphGui | uploaded_bytes_per_second | 58.398 | 0.001 | 58.398 | 58.399 |
| ImageGraphMaterials | cpu_allocated_bytes | 220460557.667 | 1892489.521 | 218777947.000 | 223104331.000 |
| ImageGraphMaterials | cpu_allocated_bytes_per_frame | 991614.150 | 3170.615 | 987187.305 | 994445.214 |
| ImageGraphMaterials | cpu_allocated_bytes_per_second | 43997083.753 | 348836.823 | 43640968.395 | 44470805.231 |
| ImageGraphMaterials | cpu_allocations | 313345.667 | 1582.607 | 311931.000 | 315555.000 |
| ImageGraphMaterials | cpu_allocations_per_frame | 1409.462 | 9.450 | 1396.261 | 1417.868 |
| ImageGraphMaterials | cpu_allocations_per_second | 62534.227 | 278.509 | 62222.775 | 62898.756 |
| ImageGraphMaterials | cpu_live_blocks | 25421.000 | 0.000 | 25421.000 | 25421.000 |
| ImageGraphMaterials | cpu_live_bytes | 64246091.000 | 0.000 | 64246091.000 | 64246091.000 |
| ImageGraphMaterials | cpu_peak_bytes | 64246091.000 | 0.000 | 64246091.000 | 64246091.000 |
| ImageGraphMaterials | cpu_process_peak_bytes | 64444249.000 | 0.000 | 64444249.000 | 64444249.000 |
| ImageGraphMaterials | cpu_profiler_overhead_bytes | 813472.000 | 0.000 | 813472.000 | 813472.000 |
| ImageGraphMaterials | draw_calls_per_frame | 21.000 | 0.000 | 21.000 | 21.000 |
| ImageGraphMaterials | duration_seconds | 5.011 | 0.006 | 5.002 | 5.017 |
| ImageGraphMaterials | frame_count | 222.333 | 2.625 | 220.000 | 226.000 |
| ImageGraphMaterials | frame_ms_per_frame | 22.474 | 0.247 | 22.134 | 22.717 |
| ImageGraphMaterials | frame_ms_per_second | 997.049 | 0.100 | 996.908 | 997.134 |
| ImageGraphMaterials | framegraph_dropped_spans | 468.000 | 1.633 | 466.000 | 470.000 |
| ImageGraphMaterials | framegraph_off_thread_dropped_spans | 468.000 | 1.633 | 466.000 | 470.000 |
| ImageGraphMaterials | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphMaterials | gpu_allocated_bytes | 192246944.000 | 1032061.960 | 191329440.000 | 193688736.000 |
| ImageGraphMaterials | gpu_allocated_bytes_per_frame | 864744.136 | 5525.408 | 857029.805 | 869679.273 |
| ImageGraphMaterials | gpu_allocated_bytes_per_second | 38366610.169 | 182566.796 | 38165647.674 | 38607471.292 |
| ImageGraphMaterials | gpu_live_bytes | 100579896.000 | 0.000 | 100579896.000 | 100579896.000 |
| ImageGraphMaterials | gpu_peak_bytes | 100710968.000 | 0.000 | 100710968.000 | 100710968.000 |
| ImageGraphMaterials | gpu_resources_created | 1379.000 | 15.748 | 1365.000 | 1401.000 |
| ImageGraphMaterials | gpu_resources_created_per_frame | 6.202 | 0.002 | 6.199 | 6.205 |
| ImageGraphMaterials | gpu_resources_created_per_second | 275.205 | 2.957 | 272.285 | 279.258 |
| ImageGraphMaterials | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphMaterials | imagegraph_ms_per_frame | 21.043 | 0.236 | 20.722 | 21.280 |
| ImageGraphMaterials | imagegraph_ms_per_second | 933.583 | 0.200 | 933.414 | 933.865 |
| ImageGraphMaterials | imagegraph_published_textures | 3.000 | 0.000 | 3.000 | 3.000 |
| ImageGraphMaterials | imagegraph_wall_ms_per_frame | 21.043 | 0.236 | 20.722 | 21.280 |
| ImageGraphMaterials | imagegraph_wall_ms_per_second | 933.583 | 0.200 | 933.414 | 933.865 |
| ImageGraphMaterials | physics_ms_per_frame | 0.037 | 0.001 | 0.036 | 0.038 |
| ImageGraphMaterials | physics_ms_per_second | 1.634 | 0.039 | 1.581 | 1.673 |
| ImageGraphMaterials | physics_wall_ms_per_frame | 0.037 | 0.001 | 0.036 | 0.038 |
| ImageGraphMaterials | physics_wall_ms_per_second | 1.634 | 0.039 | 1.581 | 1.673 |
| ImageGraphMaterials | process_peak_rss_bytes | 251542186.667 | 194932.117 | 251269120.000 | 251711488.000 |
| ImageGraphMaterials | render_ms_per_frame | 14.198 | 0.162 | 13.977 | 14.360 |
| ImageGraphMaterials | render_ms_per_second | 629.886 | 0.222 | 629.643 | 630.180 |
| ImageGraphMaterials | render_wall_ms_per_frame | 22.101 | 0.239 | 21.771 | 22.327 |
| ImageGraphMaterials | render_wall_ms_per_second | 980.535 | 0.539 | 979.800 | 981.080 |
| ImageGraphMaterials | replication_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ImageGraphMaterials | replication_ms_per_second | 0.050 | 0.003 | 0.046 | 0.054 |
| ImageGraphMaterials | replication_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ImageGraphMaterials | replication_wall_ms_per_second | 0.050 | 0.003 | 0.046 | 0.054 |
| ImageGraphMaterials | script_ms_per_frame | 0.017 | 0.001 | 0.016 | 0.017 |
| ImageGraphMaterials | script_ms_per_second | 0.733 | 0.016 | 0.710 | 0.745 |
| ImageGraphMaterials | submitted_frames | 222.333 | 2.625 | 220.000 | 226.000 |
| ImageGraphMaterials | uploaded_bytes | 292.000 | 0.000 | 292.000 | 292.000 |
| ImageGraphMaterials | uploaded_bytes_per_frame | 1.314 | 0.015 | 1.292 | 1.327 |
| ImageGraphMaterials | uploaded_bytes_per_second | 58.274 | 0.072 | 58.204 | 58.373 |
| ImageGraphSkybox | cpu_allocated_bytes | 1092211513.667 | 8490226.510 | 1081114739.000 | 1101731313.000 |
| ImageGraphSkybox | cpu_allocated_bytes_per_frame | 54915.967 | 67.841 | 54839.787 | 55004.566 |
| ImageGraphSkybox | cpu_allocated_bytes_per_second | 218430461.862 | 1690023.339 | 216216666.706 | 220317488.651 |
| ImageGraphSkybox | cpu_allocations | 6404442.333 | 55466.969 | 6331774.000 | 6466357.000 |
| ImageGraphSkybox | cpu_allocations_per_frame | 322.010 | 0.113 | 321.869 | 322.146 |
| ImageGraphSkybox | cpu_allocations_per_second | 1280818.996 | 11046.576 | 1266318.013 | 1293102.518 |
| ImageGraphSkybox | cpu_live_blocks | 17420.000 | 0.000 | 17420.000 | 17420.000 |
| ImageGraphSkybox | cpu_live_bytes | 59734241.000 | 124.451 | 59734153.000 | 59734417.000 |
| ImageGraphSkybox | cpu_peak_bytes | 59734241.000 | 124.451 | 59734153.000 | 59734417.000 |
| ImageGraphSkybox | cpu_process_peak_bytes | 59955932.000 | 124.451 | 59955844.000 | 59956108.000 |
| ImageGraphSkybox | cpu_profiler_overhead_bytes | 557440.000 | 0.000 | 557440.000 | 557440.000 |
| ImageGraphSkybox | draw_calls_per_frame | 19.000 | 0.000 | 19.000 | 19.000 |
| ImageGraphSkybox | duration_seconds | 5.000 | 0.000 | 5.000 | 5.001 |
| ImageGraphSkybox | frame_count | 19889.000 | 179.114 | 19655.000 | 20090.000 |
| ImageGraphSkybox | frame_ms_per_frame | 0.242 | 0.002 | 0.239 | 0.244 |
| ImageGraphSkybox | frame_ms_per_second | 961.429 | 0.774 | 960.421 | 962.303 |
| ImageGraphSkybox | framegraph_dropped_spans | 452.000 | 4.546 | 447.000 | 458.000 |
| ImageGraphSkybox | framegraph_off_thread_dropped_spans | 452.000 | 4.546 | 447.000 | 458.000 |
| ImageGraphSkybox | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphSkybox | gpu_allocated_bytes | 162624672.000 | 0.000 | 162624672.000 | 162624672.000 |
| ImageGraphSkybox | gpu_allocated_bytes_per_frame | 8177.279 | 73.826 | 8094.807 | 8273.959 |
| ImageGraphSkybox | gpu_allocated_bytes_per_second | 32523180.529 | 1798.950 | 32520687.127 | 32524864.881 |
| ImageGraphSkybox | gpu_live_bytes | 100481592.000 | 0.000 | 100481592.000 | 100481592.000 |
| ImageGraphSkybox | gpu_peak_bytes | 100596280.000 | 0.000 | 100596280.000 | 100596280.000 |
| ImageGraphSkybox | gpu_resources_created | 3633.000 | 0.000 | 3633.000 | 3633.000 |
| ImageGraphSkybox | gpu_resources_created_per_frame | 0.183 | 0.002 | 0.181 | 0.185 |
| ImageGraphSkybox | gpu_resources_created_per_second | 726.561 | 0.040 | 726.505 | 726.598 |
| ImageGraphSkybox | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphSkybox | imagegraph_ms_per_frame | 0.057 | 0.001 | 0.056 | 0.058 |
| ImageGraphSkybox | imagegraph_ms_per_second | 226.761 | 1.916 | 225.016 | 229.429 |
| ImageGraphSkybox | imagegraph_published_textures | 6.000 | 0.000 | 6.000 | 6.000 |
| ImageGraphSkybox | imagegraph_wall_ms_per_frame | 0.057 | 0.001 | 0.056 | 0.058 |
| ImageGraphSkybox | imagegraph_wall_ms_per_second | 226.761 | 1.916 | 225.016 | 229.429 |
| ImageGraphSkybox | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphSkybox | physics_ms_per_second | 1.919 | 0.053 | 1.844 | 1.959 |
| ImageGraphSkybox | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphSkybox | physics_wall_ms_per_second | 1.919 | 0.053 | 1.844 | 1.959 |
| ImageGraphSkybox | process_peak_rss_bytes | 270237696.000 | 5563993.494 | 264581120.000 | 277803008.000 |
| ImageGraphSkybox | render_ms_per_frame | 0.208 | 0.001 | 0.207 | 0.210 |
| ImageGraphSkybox | render_ms_per_second | 828.789 | 2.179 | 825.761 | 830.797 |
| ImageGraphSkybox | render_wall_ms_per_frame | 0.210 | 0.001 | 0.209 | 0.212 |
| ImageGraphSkybox | render_wall_ms_per_second | 837.191 | 2.071 | 834.317 | 839.118 |
| ImageGraphSkybox | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphSkybox | replication_ms_per_second | 1.100 | 0.019 | 1.082 | 1.126 |
| ImageGraphSkybox | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphSkybox | replication_wall_ms_per_second | 1.100 | 0.019 | 1.082 | 1.126 |
| ImageGraphSkybox | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphSkybox | script_ms_per_second | 0.882 | 0.025 | 0.852 | 0.913 |
| ImageGraphSkybox | submitted_frames | 19889.000 | 179.114 | 19655.000 | 20090.000 |
| ImageGraphSkybox | uploaded_bytes | 184.000 | 0.000 | 184.000 | 184.000 |
| ImageGraphSkybox | uploaded_bytes_per_frame | 0.009 | 0.000 | 0.009 | 0.009 |
| ImageGraphSkybox | uploaded_bytes_per_second | 36.798 | 0.002 | 36.795 | 36.800 |
| ImageGraphVerlet | cpu_allocated_bytes | 1154084019.000 | 19951202.585 | 1129042389.000 | 1177863755.000 |
| ImageGraphVerlet | cpu_allocated_bytes_per_frame | 48614.094 | 102.111 | 48493.711 | 48743.357 |
| ImageGraphVerlet | cpu_allocated_bytes_per_second | 230814304.117 | 3991501.062 | 225803779.410 | 235571033.263 |
| ImageGraphVerlet | cpu_allocations | 6235259.333 | 114060.011 | 6092150.000 | 6371268.000 |
| ImageGraphVerlet | cpu_allocations_per_frame | 262.646 | 0.287 | 262.311 | 263.012 |
| ImageGraphVerlet | cpu_allocations_per_second | 1247038.369 | 22818.791 | 1218404.648 | 1274244.308 |
| ImageGraphVerlet | cpu_live_blocks | 25858.000 | 0.000 | 25858.000 | 25858.000 |
| ImageGraphVerlet | cpu_live_bytes | 64940300.000 | 0.000 | 64940300.000 | 64940300.000 |
| ImageGraphVerlet | cpu_peak_bytes | 64940300.000 | 0.000 | 64940300.000 | 64940300.000 |
| ImageGraphVerlet | cpu_process_peak_bytes | 65071254.000 | 169.706 | 65071134.000 | 65071494.000 |
| ImageGraphVerlet | cpu_profiler_overhead_bytes | 827456.000 | 0.000 | 827456.000 | 827456.000 |
| ImageGraphVerlet | draw_calls_per_frame | 24.000 | 0.000 | 24.000 | 24.000 |
| ImageGraphVerlet | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| ImageGraphVerlet | frame_count | 23740.667 | 460.155 | 23163.000 | 24289.000 |
| ImageGraphVerlet | frame_ms_per_frame | 0.201 | 0.004 | 0.197 | 0.206 |
| ImageGraphVerlet | frame_ms_per_second | 953.768 | 1.295 | 952.733 | 955.594 |
| ImageGraphVerlet | framegraph_dropped_spans | 446.667 | 3.399 | 442.000 | 450.000 |
| ImageGraphVerlet | framegraph_off_thread_dropped_spans | 446.667 | 3.399 | 442.000 | 450.000 |
| ImageGraphVerlet | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphVerlet | gpu_allocated_bytes | 110358976.000 | 0.000 | 110358976.000 | 110358976.000 |
| ImageGraphVerlet | gpu_allocated_bytes_per_frame | 4650.271 | 90.326 | 4543.578 | 4764.451 |
| ImageGraphVerlet | gpu_allocated_bytes_per_second | 22071553.990 | 155.952 | 22071335.953 | 22071691.759 |
| ImageGraphVerlet | gpu_live_bytes | 104609624.000 | 0.000 | 104609624.000 | 104609624.000 |
| ImageGraphVerlet | gpu_peak_bytes | 104642392.000 | 0.000 | 104642392.000 | 104642392.000 |
| ImageGraphVerlet | gpu_resources_created | 136.000 | 0.000 | 136.000 | 136.000 |
| ImageGraphVerlet | gpu_resources_created_per_frame | 0.006 | 0.000 | 0.006 | 0.006 |
| ImageGraphVerlet | gpu_resources_created_per_second | 27.200 | 0.000 | 27.199 | 27.200 |
| ImageGraphVerlet | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphVerlet | imagegraph_ms_per_frame | 0.014 | 0.000 | 0.014 | 0.015 |
| ImageGraphVerlet | imagegraph_ms_per_second | 67.594 | 0.829 | 66.529 | 68.550 |
| ImageGraphVerlet | imagegraph_published_textures | 2.000 | 0.000 | 2.000 | 2.000 |
| ImageGraphVerlet | imagegraph_wall_ms_per_frame | 0.014 | 0.000 | 0.014 | 0.015 |
| ImageGraphVerlet | imagegraph_wall_ms_per_second | 67.594 | 0.829 | 66.529 | 68.550 |
| ImageGraphVerlet | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphVerlet | physics_ms_per_second | 1.923 | 0.190 | 1.746 | 2.186 |
| ImageGraphVerlet | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphVerlet | physics_wall_ms_per_second | 1.923 | 0.190 | 1.746 | 2.186 |
| ImageGraphVerlet | process_peak_rss_bytes | 272340309.333 | 3034315.645 | 268443648.000 | 275845120.000 |
| ImageGraphVerlet | render_ms_per_frame | 0.158 | 0.002 | 0.155 | 0.161 |
| ImageGraphVerlet | render_ms_per_second | 747.873 | 3.115 | 744.823 | 752.151 |
| ImageGraphVerlet | render_wall_ms_per_frame | 0.164 | 0.003 | 0.161 | 0.167 |
| ImageGraphVerlet | render_wall_ms_per_second | 777.318 | 2.703 | 774.747 | 781.054 |
| ImageGraphVerlet | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphVerlet | replication_ms_per_second | 1.380 | 0.063 | 1.292 | 1.438 |
| ImageGraphVerlet | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphVerlet | replication_wall_ms_per_second | 1.380 | 0.063 | 1.292 | 1.438 |
| ImageGraphVerlet | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ImageGraphVerlet | script_ms_per_second | 1.762 | 0.125 | 1.667 | 1.938 |
| ImageGraphVerlet | submitted_frames | 23740.667 | 460.155 | 23163.000 | 24289.000 |
| ImageGraphVerlet | uploaded_bytes | 400.000 | 0.000 | 400.000 | 400.000 |
| ImageGraphVerlet | uploaded_bytes_per_frame | 0.017 | 0.000 | 0.016 | 0.017 |
| ImageGraphVerlet | uploaded_bytes_per_second | 79.999 | 0.001 | 79.998 | 80.000 |
| Interface | cpu_allocated_bytes | 2043006218.333 | 43139104.932 | 1994873613.000 | 2099536544.000 |
| Interface | cpu_allocated_bytes_per_frame | 89009.387 | 52.652 | 88940.801 | 89068.787 |
| Interface | cpu_allocated_bytes_per_second | 408596887.868 | 8625667.379 | 398972729.332 | 419900106.169 |
| Interface | cpu_allocations | 4597089.000 | 95986.431 | 4489991.000 | 4722871.000 |
| Interface | cpu_allocations_per_frame | 200.286 | 0.165 | 200.071 | 200.473 |
| Interface | cpu_allocations_per_second | 919408.000 | 19192.443 | 897993.714 | 944557.998 |
| Interface | cpu_live_blocks | 18283.333 | 0.943 | 18282.000 | 18284.000 |
| Interface | cpu_live_bytes | 60216333.000 | 192.333 | 60216061.000 | 60216469.000 |
| Interface | cpu_peak_bytes | 60216341.000 | 181.019 | 60216085.000 | 60216469.000 |
| Interface | cpu_process_peak_bytes | 60347149.000 | 11.314 | 60347133.000 | 60347157.000 |
| Interface | cpu_profiler_overhead_bytes | 585066.667 | 30.170 | 585024.000 | 585088.000 |
| Interface | draw_calls_per_frame | 18.000 | 0.000 | 18.000 | 18.000 |
| Interface | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Interface | frame_count | 22953.000 | 498.315 | 22397.000 | 23606.000 |
| Interface | frame_ms_per_frame | 0.209 | 0.004 | 0.203 | 0.214 |
| Interface | frame_ms_per_second | 959.079 | 1.404 | 957.231 | 960.633 |
| Interface | framegraph_dropped_spans | 448.333 | 6.600 | 441.000 | 457.000 |
| Interface | framegraph_off_thread_dropped_spans | 448.333 | 6.600 | 441.000 | 457.000 |
| Interface | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | gpu_allocated_bytes | 92208408.000 | 0.000 | 92208408.000 | 92208408.000 |
| Interface | gpu_allocated_bytes_per_frame | 4019.154 | 86.747 | 3906.143 | 4116.998 |
| Interface | gpu_allocated_bytes_per_second | 18441486.960 | 92.527 | 18441365.272 | 18441589.466 |
| Interface | gpu_live_bytes | 87769776.000 | 0.000 | 87769776.000 | 87769776.000 |
| Interface | gpu_peak_bytes | 87769776.000 | 0.000 | 87769776.000 | 87769776.000 |
| Interface | gpu_resources_created | 41.000 | 0.000 | 41.000 | 41.000 |
| Interface | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Interface | gpu_resources_created_per_second | 8.200 | 0.000 | 8.200 | 8.200 |
| Interface | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | imagegraph_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.002 |
| Interface | imagegraph_ms_per_second | 6.877 | 0.124 | 6.770 | 7.051 |
| Interface | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | imagegraph_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.002 |
| Interface | imagegraph_wall_ms_per_second | 6.877 | 0.124 | 6.770 | 7.051 |
| Interface | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | physics_ms_per_second | 1.460 | 0.136 | 1.345 | 1.651 |
| Interface | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | physics_wall_ms_per_second | 1.460 | 0.136 | 1.345 | 1.651 |
| Interface | process_peak_rss_bytes | 247383381.333 | 240104.649 | 247062528.000 | 247640064.000 |
| Interface | render_ms_per_frame | 0.131 | 0.003 | 0.127 | 0.134 |
| Interface | render_ms_per_second | 600.367 | 0.764 | 599.334 | 601.160 |
| Interface | render_wall_ms_per_frame | 0.139 | 0.003 | 0.135 | 0.142 |
| Interface | render_wall_ms_per_second | 637.790 | 0.383 | 637.403 | 638.312 |
| Interface | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | replication_ms_per_second | 1.239 | 0.042 | 1.184 | 1.286 |
| Interface | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | replication_wall_ms_per_second | 1.239 | 0.042 | 1.184 | 1.286 |
| Interface | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Interface | script_ms_per_second | 1.148 | 0.102 | 1.032 | 1.279 |
| Interface | submitted_frames | 22953.000 | 498.315 | 22397.000 | 23606.000 |
| Interface | uploaded_bytes | 48.000 | 0.000 | 48.000 | 48.000 |
| Interface | uploaded_bytes_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Interface | uploaded_bytes_per_second | 9.600 | 0.000 | 9.600 | 9.600 |
| Meshes | cpu_allocated_bytes | 542722665.333 | 2847143.704 | 539562484.000 | 546463608.000 |
| Meshes | cpu_allocated_bytes_per_frame | 24092.877 | 11.700 | 24077.529 | 24105.906 |
| Meshes | cpu_allocated_bytes_per_second | 108543332.426 | 569581.230 | 107911514.719 | 109291952.447 |
| Meshes | cpu_allocations | 4631380.667 | 26112.738 | 4602397.000 | 4665691.000 |
| Meshes | cpu_allocations_per_frame | 205.599 | 0.019 | 205.573 | 205.620 |
| Meshes | cpu_allocations_per_second | 926265.888 | 5223.846 | 920471.023 | 933131.633 |
| Meshes | cpu_live_blocks | 17445.000 | 0.000 | 17445.000 | 17445.000 |
| Meshes | cpu_live_bytes | 59723891.000 | 0.000 | 59723891.000 | 59723891.000 |
| Meshes | cpu_peak_bytes | 59723891.000 | 0.000 | 59723891.000 | 59723891.000 |
| Meshes | cpu_process_peak_bytes | 59854929.000 | 181.019 | 59854673.000 | 59855057.000 |
| Meshes | cpu_profiler_overhead_bytes | 558240.000 | 0.000 | 558240.000 | 558240.000 |
| Meshes | draw_calls_per_frame | 40.124 | 0.001 | 40.123 | 40.124 |
| Meshes | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Meshes | frame_count | 22526.333 | 129.131 | 22383.000 | 22696.000 |
| Meshes | frame_ms_per_frame | 0.211 | 0.001 | 0.209 | 0.212 |
| Meshes | frame_ms_per_second | 949.494 | 1.258 | 947.745 | 950.648 |
| Meshes | framegraph_dropped_spans | 443.667 | 6.182 | 435.000 | 449.000 |
| Meshes | framegraph_off_thread_dropped_spans | 443.667 | 6.182 | 435.000 | 449.000 |
| Meshes | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | gpu_allocated_bytes | 119985120.000 | 0.000 | 119985120.000 | 119985120.000 |
| Meshes | gpu_allocated_bytes_per_frame | 5326.613 | 30.483 | 5286.620 | 5360.547 |
| Meshes | gpu_allocated_bytes_per_second | 23996758.380 | 103.789 | 23996614.411 | 23996855.120 |
| Meshes | gpu_live_bytes | 115546488.000 | 0.000 | 115546488.000 | 115546488.000 |
| Meshes | gpu_peak_bytes | 115546488.000 | 0.000 | 115546488.000 | 115546488.000 |
| Meshes | gpu_resources_created | 53.000 | 0.000 | 53.000 | 53.000 |
| Meshes | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Meshes | gpu_resources_created_per_second | 10.600 | 0.000 | 10.600 | 10.600 |
| Meshes | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | imagegraph_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Meshes | imagegraph_ms_per_second | 7.021 | 0.038 | 6.971 | 7.065 |
| Meshes | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | imagegraph_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Meshes | imagegraph_wall_ms_per_second | 7.021 | 0.038 | 6.971 | 7.065 |
| Meshes | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | physics_ms_per_second | 1.368 | 0.084 | 1.274 | 1.479 |
| Meshes | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | physics_wall_ms_per_second | 1.368 | 0.084 | 1.274 | 1.479 |
| Meshes | process_peak_rss_bytes | 254810794.667 | 1346765.520 | 253722624.000 | 256708608.000 |
| Meshes | render_ms_per_frame | 0.170 | 0.001 | 0.169 | 0.171 |
| Meshes | render_ms_per_second | 768.018 | 0.592 | 767.202 | 768.587 |
| Meshes | render_wall_ms_per_frame | 0.173 | 0.001 | 0.172 | 0.174 |
| Meshes | render_wall_ms_per_second | 780.249 | 0.609 | 779.389 | 780.715 |
| Meshes | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | replication_ms_per_second | 1.418 | 0.053 | 1.376 | 1.492 |
| Meshes | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | replication_wall_ms_per_second | 1.418 | 0.053 | 1.376 | 1.492 |
| Meshes | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Meshes | script_ms_per_second | 1.261 | 0.078 | 1.173 | 1.362 |
| Meshes | submitted_frames | 22526.333 | 129.131 | 22383.000 | 22696.000 |
| Meshes | uploaded_bytes | 1372.000 | 0.000 | 1372.000 | 1372.000 |
| Meshes | uploaded_bytes_per_frame | 0.061 | 0.000 | 0.060 | 0.061 |
| Meshes | uploaded_bytes_per_second | 274.397 | 0.001 | 274.395 | 274.398 |
| Particles | cpu_allocated_bytes | 577336668.667 | 28909285.219 | 542465956.000 | 613255672.000 |
| Particles | cpu_allocated_bytes_per_frame | 24428.475 | 145.872 | 24249.898 | 24607.211 |
| Particles | cpu_allocated_bytes_per_second | 115464774.084 | 5781409.985 | 108491889.992 | 122648677.109 |
| Particles | cpu_allocations | 4835937.333 | 263408.827 | 4517015.000 | 5162110.000 |
| Particles | cpu_allocations_per_frame | 204.569 | 0.327 | 204.125 | 204.900 |
| Particles | cpu_allocations_per_second | 967166.010 | 52677.903 | 903392.165 | 1032401.316 |
| Particles | cpu_live_blocks | 17945.667 | 0.471 | 17945.000 | 17946.000 |
| Particles | cpu_live_bytes | 67917604.000 | 11.314 | 67917588.000 | 67917612.000 |
| Particles | cpu_peak_bytes | 67917612.000 | 0.000 | 67917612.000 | 67917612.000 |
| Particles | cpu_process_peak_bytes | 68048784.000 | 0.000 | 68048784.000 | 68048784.000 |
| Particles | cpu_profiler_overhead_bytes | 574261.333 | 15.085 | 574240.000 | 574272.000 |
| Particles | draw_calls_per_frame | 28.999 | 0.001 | 28.998 | 29.000 |
| Particles | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Particles | frame_count | 23641.667 | 1324.842 | 22045.000 | 25289.000 |
| Particles | frame_ms_per_frame | 0.202 | 0.011 | 0.189 | 0.216 |
| Particles | frame_ms_per_second | 953.122 | 2.535 | 950.514 | 956.555 |
| Particles | framegraph_dropped_spans | 452.000 | 2.160 | 449.000 | 454.000 |
| Particles | framegraph_off_thread_dropped_spans | 452.000 | 2.160 | 449.000 | 454.000 |
| Particles | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | gpu_allocated_bytes | 109681832.000 | 0.000 | 109681832.000 | 109681832.000 |
| Particles | gpu_allocated_bytes_per_frame | 4653.935 | 260.575 | 4337.136 | 4975.361 |
| Particles | gpu_allocated_bytes_per_second | 21935883.486 | 199.586 | 21935620.242 | 21936103.307 |
| Particles | gpu_live_bytes | 105243196.000 | 0.000 | 105243196.000 | 105243196.000 |
| Particles | gpu_peak_bytes | 105243196.000 | 0.000 | 105243196.000 | 105243196.000 |
| Particles | gpu_resources_created | 64.000 | 0.000 | 64.000 | 64.000 |
| Particles | gpu_resources_created_per_frame | 0.003 | 0.000 | 0.003 | 0.003 |
| Particles | gpu_resources_created_per_second | 12.800 | 0.000 | 12.800 | 12.800 |
| Particles | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | imagegraph_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Particles | imagegraph_ms_per_second | 10.608 | 0.060 | 10.531 | 10.677 |
| Particles | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | imagegraph_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Particles | imagegraph_wall_ms_per_second | 10.608 | 0.060 | 10.531 | 10.677 |
| Particles | physics_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.002 |
| Particles | physics_ms_per_second | 6.239 | 0.491 | 5.587 | 6.773 |
| Particles | physics_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.002 |
| Particles | physics_wall_ms_per_second | 6.239 | 0.491 | 5.587 | 6.773 |
| Particles | process_peak_rss_bytes | 309548373.333 | 40094297.442 | 269774848.000 | 364433408.000 |
| Particles | render_ms_per_frame | 0.159 | 0.007 | 0.150 | 0.168 |
| Particles | render_ms_per_second | 747.656 | 8.756 | 739.600 | 759.828 |
| Particles | render_wall_ms_per_frame | 0.162 | 0.007 | 0.153 | 0.171 |
| Particles | render_wall_ms_per_second | 762.083 | 8.908 | 754.005 | 774.494 |
| Particles | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | replication_ms_per_second | 1.321 | 0.011 | 1.308 | 1.334 |
| Particles | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | replication_wall_ms_per_second | 1.321 | 0.011 | 1.308 | 1.334 |
| Particles | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Particles | script_ms_per_second | 1.705 | 0.235 | 1.482 | 2.030 |
| Particles | submitted_frames | 23641.667 | 1324.842 | 22045.000 | 25289.000 |
| Particles | uploaded_bytes | 62471512.000 | 3540140.508 | 58165736.000 | 66836664.000 |
| Particles | uploaded_bytes_per_frame | 2642.335 | 2.924 | 2638.500 | 2645.591 |
| Particles | uploaded_bytes_per_second | 12494025.094 | 707976.974 | 11633007.678 | 13367064.988 |
| RenderFeatures | cpu_allocated_bytes | 590937416.333 | 7745580.979 | 583565987.000 | 601640079.000 |
| RenderFeatures | cpu_allocated_bytes_per_frame | 24761.456 | 32.603 | 24716.132 | 24791.452 |
| RenderFeatures | cpu_allocated_bytes_per_second | 118185710.213 | 1549670.742 | 116710633.034 | 120326933.098 |
| RenderFeatures | cpu_allocations | 5796386.667 | 77523.800 | 5721776.000 | 5903261.000 |
| RenderFeatures | cpu_allocations_per_frame | 242.879 | 0.259 | 242.513 | 243.076 |
| RenderFeatures | cpu_allocations_per_second | 1159259.944 | 15510.220 | 1144330.057 | 1180641.577 |
| RenderFeatures | cpu_live_blocks | 17624.000 | 0.000 | 17624.000 | 17624.000 |
| RenderFeatures | cpu_live_bytes | 59702902.000 | 0.000 | 59702902.000 | 59702902.000 |
| RenderFeatures | cpu_peak_bytes | 59702902.000 | 0.000 | 59702902.000 | 59702902.000 |
| RenderFeatures | cpu_process_peak_bytes | 59834094.000 | 11.314 | 59834078.000 | 59834102.000 |
| RenderFeatures | cpu_profiler_overhead_bytes | 563968.000 | 0.000 | 563968.000 | 563968.000 |
| RenderFeatures | draw_calls_per_frame | 29.000 | 0.000 | 29.000 | 29.000 |
| RenderFeatures | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| RenderFeatures | frame_count | 23865.667 | 344.483 | 23539.000 | 24342.000 |
| RenderFeatures | frame_ms_per_frame | 0.200 | 0.002 | 0.197 | 0.202 |
| RenderFeatures | frame_ms_per_second | 953.568 | 2.844 | 951.486 | 957.589 |
| RenderFeatures | framegraph_dropped_spans | 446.667 | 1.247 | 445.000 | 448.000 |
| RenderFeatures | framegraph_off_thread_dropped_spans | 446.667 | 1.247 | 445.000 | 448.000 |
| RenderFeatures | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | gpu_allocated_bytes | 108987296.000 | 0.000 | 108987296.000 | 108987296.000 |
| RenderFeatures | gpu_allocated_bytes_per_frame | 4567.642 | 65.396 | 4477.335 | 4630.073 |
| RenderFeatures | gpu_allocated_bytes_per_second | 21797130.804 | 116.169 | 21796980.277 | 21797263.068 |
| RenderFeatures | gpu_live_bytes | 104548664.000 | 0.000 | 104548664.000 | 104548664.000 |
| RenderFeatures | gpu_peak_bytes | 104548664.000 | 0.000 | 104548664.000 | 104548664.000 |
| RenderFeatures | gpu_resources_created | 53.000 | 0.000 | 53.000 | 53.000 |
| RenderFeatures | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| RenderFeatures | gpu_resources_created_per_second | 10.600 | 0.000 | 10.600 | 10.600 |
| RenderFeatures | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | imagegraph_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| RenderFeatures | imagegraph_ms_per_second | 6.829 | 0.225 | 6.515 | 7.029 |
| RenderFeatures | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | imagegraph_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| RenderFeatures | imagegraph_wall_ms_per_second | 6.829 | 0.225 | 6.515 | 7.029 |
| RenderFeatures | physics_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | physics_ms_per_second | 1.831 | 0.299 | 1.559 | 2.247 |
| RenderFeatures | physics_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | physics_wall_ms_per_second | 1.831 | 0.299 | 1.559 | 2.247 |
| RenderFeatures | process_peak_rss_bytes | 320077824.000 | 48040722.776 | 284844032.000 | 388001792.000 |
| RenderFeatures | render_ms_per_frame | 0.166 | 0.000 | 0.166 | 0.167 |
| RenderFeatures | render_ms_per_second | 794.678 | 10.889 | 786.034 | 810.037 |
| RenderFeatures | render_wall_ms_per_frame | 0.168 | 0.000 | 0.168 | 0.169 |
| RenderFeatures | render_wall_ms_per_second | 803.321 | 10.754 | 794.831 | 818.493 |
| RenderFeatures | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | replication_ms_per_second | 1.336 | 0.046 | 1.271 | 1.373 |
| RenderFeatures | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | replication_wall_ms_per_second | 1.336 | 0.046 | 1.271 | 1.373 |
| RenderFeatures | script_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| RenderFeatures | script_ms_per_second | 0.817 | 0.108 | 0.706 | 0.963 |
| RenderFeatures | submitted_frames | 23865.667 | 344.483 | 23539.000 | 24342.000 |
| RenderFeatures | uploaded_bytes | 37612690.667 | 542904.487 | 37097864.000 | 38363392.000 |
| RenderFeatures | uploaded_bytes_per_frame | 1576.017 | 0.000 | 1576.016 | 1576.017 |
| RenderFeatures | uploaded_bytes_per_second | 7522425.328 | 108616.011 | 7419409.781 | 7672609.362 |
| ReplicationRings | cpu_allocated_bytes | 490174768.667 | 13990890.160 | 471945807.000 | 505952299.000 |
| ReplicationRings | cpu_allocated_bytes_per_frame | 72328.678 | 263.326 | 72084.606 | 72694.296 |
| ReplicationRings | cpu_allocated_bytes_per_second | 98029091.601 | 2797207.137 | 94386283.449 | 101186165.257 |
| ReplicationRings | cpu_allocations | 2184954.667 | 61885.127 | 2104019.000 | 2254262.000 |
| ReplicationRings | cpu_allocations_per_frame | 322.407 | 1.067 | 321.420 | 323.888 |
| ReplicationRings | cpu_allocations_per_second | 436964.800 | 12372.575 | 420790.970 | 450833.266 |
| ReplicationRings | cpu_live_blocks | 30225.333 | 0.943 | 30224.000 | 30226.000 |
| ReplicationRings | cpu_live_bytes | 66669412.333 | 1211.983 | 66667700.000 | 66670334.000 |
| ReplicationRings | cpu_peak_bytes | 66669839.000 | 1200.234 | 66668149.000 | 66670821.000 |
| ReplicationRings | cpu_process_peak_bytes | 71402097.333 | 395.508 | 71401538.000 | 71402377.000 |
| ReplicationRings | cpu_profiler_overhead_bytes | 967210.667 | 30.170 | 967168.000 | 967232.000 |
| ReplicationRings | draw_calls_per_frame | 33.000 | 0.000 | 33.000 | 33.000 |
| ReplicationRings | duration_seconds | 5.000 | 0.000 | 5.000 | 5.001 |
| ReplicationRings | frame_count | 6776.667 | 177.781 | 6536.000 | 6960.000 |
| ReplicationRings | frame_ms_per_frame | 0.710 | 0.018 | 0.692 | 0.735 |
| ReplicationRings | frame_ms_per_second | 962.049 | 1.087 | 960.956 | 963.532 |
| ReplicationRings | framegraph_dropped_spans | 452.000 | 5.715 | 447.000 | 460.000 |
| ReplicationRings | framegraph_off_thread_dropped_spans | 452.000 | 5.715 | 447.000 | 460.000 |
| ReplicationRings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | gpu_allocated_bytes | 15796736.000 | 0.000 | 15796736.000 | 15796736.000 |
| ReplicationRings | gpu_allocated_bytes_per_frame | 2332.673 | 61.946 | 2269.646 | 2416.881 |
| ReplicationRings | gpu_allocated_bytes_per_second | 3159159.084 | 104.256 | 3159013.264 | 3159250.871 |
| ReplicationRings | gpu_live_bytes | 199633272.000 | 0.000 | 199633272.000 | 199633272.000 |
| ReplicationRings | gpu_peak_bytes | 200699512.000 | 0.000 | 200699512.000 | 200699512.000 |
| ReplicationRings | gpu_resources_created | 12.000 | 0.000 | 12.000 | 12.000 |
| ReplicationRings | gpu_resources_created_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| ReplicationRings | gpu_resources_created_per_second | 2.400 | 0.000 | 2.400 | 2.400 |
| ReplicationRings | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | imagegraph_ms_per_frame | 0.005 | 0.000 | 0.005 | 0.005 |
| ReplicationRings | imagegraph_ms_per_second | 6.442 | 0.059 | 6.361 | 6.498 |
| ReplicationRings | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| ReplicationRings | imagegraph_wall_ms_per_frame | 0.005 | 0.000 | 0.005 | 0.005 |
| ReplicationRings | imagegraph_wall_ms_per_second | 6.442 | 0.059 | 6.361 | 6.498 |
| ReplicationRings | physics_ms_per_frame | 0.005 | 0.000 | 0.005 | 0.006 |
| ReplicationRings | physics_ms_per_second | 7.118 | 0.174 | 6.909 | 7.335 |
| ReplicationRings | physics_wall_ms_per_frame | 0.005 | 0.000 | 0.005 | 0.006 |
| ReplicationRings | physics_wall_ms_per_second | 7.118 | 0.174 | 6.909 | 7.335 |
| ReplicationRings | process_peak_rss_bytes | 257236992.000 | 897544.421 | 256090112.000 | 258281472.000 |
| ReplicationRings | render_ms_per_frame | 0.437 | 0.010 | 0.427 | 0.451 |
| ReplicationRings | render_ms_per_second | 591.335 | 2.396 | 589.605 | 594.723 |
| ReplicationRings | render_wall_ms_per_frame | 0.440 | 0.010 | 0.431 | 0.455 |
| ReplicationRings | render_wall_ms_per_second | 596.072 | 2.389 | 594.356 | 599.451 |
| ReplicationRings | replication_ms_per_frame | 0.015 | 0.001 | 0.014 | 0.016 |
| ReplicationRings | replication_ms_per_second | 20.202 | 0.329 | 19.769 | 20.566 |
| ReplicationRings | replication_wall_ms_per_frame | 0.016 | 0.001 | 0.016 | 0.017 |
| ReplicationRings | replication_wall_ms_per_second | 22.016 | 0.255 | 21.754 | 22.362 |
| ReplicationRings | script_ms_per_frame | 0.027 | 0.001 | 0.025 | 0.028 |
| ReplicationRings | script_ms_per_second | 36.718 | 1.022 | 35.400 | 37.890 |
| ReplicationRings | submitted_frames | 6776.667 | 177.781 | 6536.000 | 6960.000 |
| ReplicationRings | uploaded_bytes | 481055654.667 | 14575499.963 | 463078124.000 | 498777924.000 |
| ReplicationRings | uploaded_bytes_per_frame | 70980.918 | 512.414 | 70428.873 | 71663.495 |
| ReplicationRings | uploaded_bytes_per_second | 96205386.858 | 2914487.379 | 92612800.925 | 99751351.153 |
| Rings | cpu_allocated_bytes | 730265331.333 | 5883460.489 | 722886042.000 | 737283974.000 |
| Rings | cpu_allocated_bytes_per_frame | 58153.302 | 40.505 | 58104.183 | 58203.385 |
| Rings | cpu_allocated_bytes_per_second | 146050154.984 | 1177585.892 | 144572511.470 | 147454175.984 |
| Rings | cpu_allocations | 2434110.667 | 20916.101 | 2407931.000 | 2459125.000 |
| Rings | cpu_allocations_per_frame | 193.835 | 0.031 | 193.800 | 193.875 |
| Rings | cpu_allocations_per_second | 486812.431 | 4186.179 | 481570.555 | 491816.265 |
| Rings | cpu_live_blocks | 25644.333 | 0.471 | 25644.000 | 25645.000 |
| Rings | cpu_live_bytes | 62453063.000 | 243.442 | 62452879.000 | 62453407.000 |
| Rings | cpu_peak_bytes | 62453079.000 | 248.902 | 62452903.000 | 62453431.000 |
| Rings | cpu_process_peak_bytes | 62584135.000 | 260.215 | 62583951.000 | 62584503.000 |
| Rings | cpu_profiler_overhead_bytes | 820618.667 | 15.085 | 820608.000 | 820640.000 |
| Rings | draw_calls_per_frame | 19.000 | 0.000 | 19.000 | 19.000 |
| Rings | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Rings | frame_count | 12557.667 | 109.910 | 12420.000 | 12689.000 |
| Rings | frame_ms_per_frame | 0.382 | 0.003 | 0.379 | 0.387 |
| Rings | frame_ms_per_second | 959.969 | 1.561 | 957.942 | 961.742 |
| Rings | framegraph_dropped_spans | 452.000 | 5.715 | 447.000 | 460.000 |
| Rings | framegraph_off_thread_dropped_spans | 452.000 | 5.715 | 447.000 | 460.000 |
| Rings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | gpu_allocated_bytes | 104883360.000 | 0.000 | 104883360.000 | 104883360.000 |
| Rings | gpu_allocated_bytes_per_frame | 8352.778 | 73.166 | 8265.692 | 8444.715 |
| Rings | gpu_allocated_bytes_per_second | 20976252.803 | 197.873 | 20975990.524 | 20976468.428 |
| Rings | gpu_live_bytes | 100444728.000 | 0.000 | 100444728.000 | 100444728.000 |
| Rings | gpu_peak_bytes | 100444728.000 | 0.000 | 100444728.000 | 100444728.000 |
| Rings | gpu_resources_created | 45.000 | 0.000 | 45.000 | 45.000 |
| Rings | gpu_resources_created_per_frame | 0.004 | 0.000 | 0.004 | 0.004 |
| Rings | gpu_resources_created_per_second | 9.000 | 0.000 | 9.000 | 9.000 |
| Rings | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | imagegraph_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Rings | imagegraph_ms_per_second | 6.077 | 0.018 | 6.064 | 6.103 |
| Rings | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | imagegraph_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Rings | imagegraph_wall_ms_per_second | 6.077 | 0.018 | 6.064 | 6.103 |
| Rings | physics_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Rings | physics_ms_per_second | 5.016 | 0.227 | 4.827 | 5.336 |
| Rings | physics_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Rings | physics_wall_ms_per_second | 5.016 | 0.227 | 4.827 | 5.336 |
| Rings | process_peak_rss_bytes | 244774229.333 | 271334.023 | 244576256.000 | 245157888.000 |
| Rings | render_ms_per_frame | 0.268 | 0.002 | 0.266 | 0.272 |
| Rings | render_ms_per_second | 673.443 | 2.175 | 670.406 | 675.385 |
| Rings | render_wall_ms_per_frame | 0.270 | 0.002 | 0.268 | 0.274 |
| Rings | render_wall_ms_per_second | 678.630 | 2.158 | 675.616 | 680.550 |
| Rings | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | replication_ms_per_second | 0.764 | 0.019 | 0.741 | 0.787 |
| Rings | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Rings | replication_wall_ms_per_second | 0.764 | 0.019 | 0.741 | 0.787 |
| Rings | script_ms_per_frame | 0.015 | 0.000 | 0.014 | 0.015 |
| Rings | script_ms_per_second | 37.197 | 0.710 | 36.285 | 38.018 |
| Rings | submitted_frames | 12557.667 | 109.910 | 12420.000 | 12689.000 |
| Rings | uploaded_bytes | 514349708.000 | 4502744.307 | 508697228.000 | 519715468.000 |
| Rings | uploaded_bytes_per_frame | 40959.018 | 1.538 | 40957.909 | 40961.193 |
| Rings | uploaded_bytes_per_second | 102867891.553 | 901177.736 | 101736140.356 | 103941247.583 |
| ServerPhysics | cpu_allocated_bytes | 737267630.333 | 1133081.577 | 735665211.000 | 738068840.000 |
| ServerPhysics | cpu_allocated_bytes_per_frame | 13738786.214 | 100189.582 | 13667941.481 | 13880475.679 |
| ServerPhysics | cpu_allocated_bytes_per_second | 143964623.110 | 1070123.029 | 142454158.990 | 144801209.657 |
| ServerPhysics | cpu_allocations | 1448313.333 | 47189.007 | 1381578.000 | 1481681.000 |
| ServerPhysics | cpu_allocations_per_frame | 26981.528 | 646.309 | 26067.509 | 27438.537 |
| ServerPhysics | cpu_allocations_per_second | 282766.348 | 8105.533 | 271630.855 | 290689.959 |
| ServerPhysics | cpu_live_blocks | 213329.000 | 0.000 | 213329.000 | 213329.000 |
| ServerPhysics | cpu_live_bytes | 438288931.000 | 0.000 | 438288931.000 | 438288931.000 |
| ServerPhysics | cpu_peak_bytes | 438288931.000 | 0.000 | 438288931.000 | 438288931.000 |
| ServerPhysics | cpu_process_peak_bytes | 449133365.000 | 0.000 | 449133365.000 | 449133365.000 |
| ServerPhysics | cpu_profiler_overhead_bytes | 6826528.000 | 0.000 | 6826528.000 | 6826528.000 |
| ServerPhysics | duration_seconds | 5.121 | 0.042 | 5.086 | 5.181 |
| ServerPhysics | frame_count | 53.667 | 0.471 | 53.000 | 54.000 |
| ServerPhysics | frame_ms_per_frame | 94.378 | 0.759 | 93.369 | 95.201 |
| ServerPhysics | frame_ms_per_second | 988.925 | 2.810 | 985.367 | 992.236 |
| ServerPhysics | framegraph_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | framegraph_off_thread_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | physics_ms_per_frame | 90.571 | 0.717 | 89.616 | 91.343 |
| ServerPhysics | physics_ms_per_second | 949.041 | 2.594 | 945.697 | 952.019 |
| ServerPhysics | physics_wall_ms_per_frame | 90.571 | 0.717 | 89.616 | 91.343 |
| ServerPhysics | physics_wall_ms_per_second | 949.041 | 2.594 | 945.697 | 952.019 |
| ServerPhysics | process_peak_rss_bytes | 462573568.000 | 586438.879 | 461770752.000 | 463155200.000 |
| ServerPhysics | render_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ServerPhysics | render_ms_per_second | 0.013 | 0.000 | 0.013 | 0.013 |
| ServerPhysics | render_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ServerPhysics | render_wall_ms_per_second | 0.013 | 0.000 | 0.013 | 0.013 |
| ServerPhysics | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | replication_ms_per_second | 0.005 | 0.000 | 0.005 | 0.005 |
| ServerPhysics | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerPhysics | replication_wall_ms_per_second | 0.005 | 0.000 | 0.005 | 0.005 |
| ServerPhysics | script_ms_per_frame | 0.051 | 0.001 | 0.050 | 0.053 |
| ServerPhysics | script_ms_per_second | 0.534 | 0.011 | 0.524 | 0.549 |
| ServerPhysics | tick_count | 53.667 | 0.471 | 53.000 | 54.000 |
| ServerPhysics | tick_overruns | 42.333 | 2.055 | 40.000 | 45.000 |
| ServerPhysics | tick_rate_hz | 30.000 | 0.000 | 30.000 | 30.000 |
| ServerRings | clients_admitted | 1.000 | 0.000 | 1.000 | 1.000 |
| ServerRings | cpu_allocated_bytes | 19707950.667 | 5504.224 | 19703098.000 | 19715648.000 |
| ServerRings | cpu_allocated_bytes_per_frame | 130516.230 | 36.452 | 130484.093 | 130567.205 |
| ServerRings | cpu_allocated_bytes_per_second | 3939981.837 | 1288.914 | 3938692.911 | 3941742.523 |
| ServerRings | cpu_allocations | 135583.667 | 109.646 | 135487.000 | 135737.000 |
| ServerRings | cpu_allocations_per_frame | 897.905 | 0.726 | 897.265 | 898.921 |
| ServerRings | cpu_allocations_per_second | 27105.669 | 23.183 | 27084.151 | 27137.850 |
| ServerRings | cpu_live_blocks | 17037.000 | 0.000 | 17037.000 | 17037.000 |
| ServerRings | cpu_live_bytes | 19791339.000 | 8.485 | 19791333.000 | 19791351.000 |
| ServerRings | cpu_peak_bytes | 19791547.000 | 8.485 | 19791541.000 | 19791559.000 |
| ServerRings | cpu_process_peak_bytes | 19811618.333 | 18.856 | 19811605.000 | 19811645.000 |
| ServerRings | cpu_profiler_overhead_bytes | 545184.000 | 0.000 | 545184.000 | 545184.000 |
| ServerRings | duration_seconds | 5.002 | 0.000 | 5.002 | 5.002 |
| ServerRings | frame_count | 151.000 | 0.000 | 151.000 | 151.000 |
| ServerRings | frame_ms_per_frame | 1.875 | 0.066 | 1.784 | 1.936 |
| ServerRings | frame_ms_per_second | 56.597 | 1.985 | 53.844 | 58.448 |
| ServerRings | framegraph_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | framegraph_off_thread_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | imagegraph_wall_ms_per_second | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | physics_ms_per_frame | 0.068 | 0.003 | 0.064 | 0.070 |
| ServerRings | physics_ms_per_second | 2.046 | 0.082 | 1.930 | 2.111 |
| ServerRings | physics_wall_ms_per_frame | 0.068 | 0.003 | 0.064 | 0.070 |
| ServerRings | physics_wall_ms_per_second | 2.046 | 0.082 | 1.930 | 2.111 |
| ServerRings | process_peak_rss_bytes | 35658410.667 | 109924.173 | 35545088.000 | 35807232.000 |
| ServerRings | render_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ServerRings | render_ms_per_second | 0.035 | 0.003 | 0.031 | 0.038 |
| ServerRings | render_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| ServerRings | render_wall_ms_per_second | 0.035 | 0.003 | 0.031 | 0.038 |
| ServerRings | replication_ms_per_frame | 0.746 | 0.026 | 0.710 | 0.767 |
| ServerRings | replication_ms_per_second | 22.519 | 0.772 | 21.431 | 23.144 |
| ServerRings | replication_wall_ms_per_frame | 0.754 | 0.026 | 0.718 | 0.775 |
| ServerRings | replication_wall_ms_per_second | 22.772 | 0.786 | 21.664 | 23.408 |
| ServerRings | script_ms_per_frame | 0.679 | 0.018 | 0.657 | 0.700 |
| ServerRings | script_ms_per_second | 20.502 | 0.537 | 19.830 | 21.145 |
| ServerRings | tick_count | 151.000 | 0.000 | 151.000 | 151.000 |
| ServerRings | tick_overruns | 0.000 | 0.000 | 0.000 | 0.000 |
| ServerRings | tick_rate_hz | 30.000 | 0.000 | 30.000 | 30.000 |
| StressPhysics | cpu_allocated_bytes | 944479327.667 | 3954140.178 | 938887329.000 | 947275327.000 |
| StressPhysics | cpu_allocated_bytes_per_frame | 22319669.495 | 410137.105 | 22029658.767 | 22899690.951 |
| StressPhysics | cpu_allocated_bytes_per_second | 187469525.589 | 1611510.683 | 185380044.778 | 189302303.098 |
| StressPhysics | cpu_allocations | 146179.333 | 207.418 | 145886.000 | 146326.000 |
| StressPhysics | cpu_allocations_per_frame | 3454.685 | 73.193 | 3402.930 | 3558.195 |
| StressPhysics | cpu_allocations_per_second | 29015.524 | 270.175 | 28635.730 | 29241.603 |
| StressPhysics | cpu_live_blocks | 224142.000 | 0.000 | 224142.000 | 224142.000 |
| StressPhysics | cpu_live_bytes | 735845026.000 | 0.000 | 735845026.000 | 735845026.000 |
| StressPhysics | cpu_peak_bytes | 735845026.000 | 0.000 | 735845026.000 | 735845026.000 |
| StressPhysics | cpu_process_peak_bytes | 739922920.000 | 0.000 | 739922920.000 | 739922920.000 |
| StressPhysics | cpu_profiler_overhead_bytes | 7172544.000 | 0.000 | 7172544.000 | 7172544.000 |
| StressPhysics | draw_calls_per_frame | 19.000 | 0.000 | 19.000 | 19.000 |
| StressPhysics | duration_seconds | 5.038 | 0.051 | 5.001 | 5.110 |
| StressPhysics | frame_count | 42.333 | 0.943 | 41.000 | 43.000 |
| StressPhysics | frame_ms_per_frame | 118.989 | 2.296 | 116.299 | 121.909 |
| StressPhysics | frame_ms_per_second | 999.371 | 0.008 | 999.365 | 999.383 |
| StressPhysics | framegraph_dropped_spans | 472.000 | 5.657 | 468.000 | 480.000 |
| StressPhysics | framegraph_off_thread_dropped_spans | 472.000 | 5.657 | 468.000 | 480.000 |
| StressPhysics | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | gpu_allocated_bytes | 133084320.000 | 0.000 | 133084320.000 | 133084320.000 |
| StressPhysics | gpu_allocated_bytes_per_frame | 3145309.132 | 71170.221 | 3094984.186 | 3245959.024 |
| StressPhysics | gpu_allocated_bytes_per_second | 26416453.641 | 263175.603 | 26044357.430 | 26609601.330 |
| StressPhysics | gpu_live_bytes | 128645688.000 | 0.000 | 128645688.000 | 128645688.000 |
| StressPhysics | gpu_peak_bytes | 128645688.000 | 0.000 | 128645688.000 | 128645688.000 |
| StressPhysics | gpu_resources_created | 45.000 | 0.000 | 45.000 | 45.000 |
| StressPhysics | gpu_resources_created_per_frame | 1.064 | 0.024 | 1.047 | 1.098 |
| StressPhysics | gpu_resources_created_per_second | 8.932 | 0.089 | 8.806 | 8.998 |
| StressPhysics | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | imagegraph_ms_per_frame | 0.221 | 0.013 | 0.203 | 0.230 |
| StressPhysics | imagegraph_ms_per_second | 1.855 | 0.082 | 1.743 | 1.935 |
| StressPhysics | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| StressPhysics | imagegraph_wall_ms_per_frame | 0.221 | 0.013 | 0.203 | 0.230 |
| StressPhysics | imagegraph_wall_ms_per_second | 1.855 | 0.082 | 1.743 | 1.935 |
| StressPhysics | physics_ms_per_frame | 26.560 | 0.280 | 26.201 | 26.884 |
| StressPhysics | physics_ms_per_second | 223.130 | 3.810 | 220.388 | 228.518 |
| StressPhysics | physics_wall_ms_per_frame | 26.560 | 0.280 | 26.201 | 26.884 |
| StressPhysics | physics_wall_ms_per_second | 223.130 | 3.810 | 220.388 | 228.518 |
| StressPhysics | process_peak_rss_bytes | 916630186.667 | 77307.291 | 916520960.000 | 916688896.000 |
| StressPhysics | render_ms_per_frame | 58.806 | 1.962 | 56.306 | 61.099 |
| StressPhysics | render_ms_per_second | 493.773 | 7.234 | 483.845 | 500.874 |
| StressPhysics | render_wall_ms_per_frame | 58.825 | 1.963 | 56.324 | 61.117 |
| StressPhysics | render_wall_ms_per_second | 493.926 | 7.236 | 483.994 | 501.027 |
| StressPhysics | replication_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| StressPhysics | replication_ms_per_second | 0.016 | 0.001 | 0.015 | 0.017 |
| StressPhysics | replication_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| StressPhysics | replication_wall_ms_per_second | 0.016 | 0.001 | 0.015 | 0.017 |
| StressPhysics | script_ms_per_frame | 0.054 | 0.009 | 0.047 | 0.067 |
| StressPhysics | script_ms_per_second | 0.456 | 0.074 | 0.404 | 0.560 |
| StressPhysics | submitted_frames | 42.333 | 0.943 | 41.000 | 43.000 |
| StressPhysics | uploaded_bytes | 333483416.000 | 7542849.456 | 322816216.000 | 338817016.000 |
| StressPhysics | uploaded_bytes_per_frame | 7877499.074 | 2780.931 | 7873566.244 | 7879465.488 |
| StressPhysics | uploaded_bytes_per_second | 66186753.078 | 1294089.379 | 64545626.491 | 67708763.893 |
| Terrain | cpu_allocated_bytes | 538321989.000 | 2884554.777 | 534288758.000 | 540868482.000 |
| Terrain | cpu_allocated_bytes_per_frame | 39739.294 | 118.034 | 39639.354 | 39905.053 |
| Terrain | cpu_allocated_bytes_per_second | 107662549.913 | 576817.014 | 106855959.006 | 108171368.509 |
| Terrain | cpu_allocations | 2883181.000 | 23211.038 | 2850393.000 | 2900931.000 |
| Terrain | cpu_allocations_per_frame | 212.834 | 0.043 | 212.787 | 212.891 |
| Terrain | cpu_allocations_per_second | 576626.303 | 4641.878 | 570069.037 | 580173.714 |
| Terrain | cpu_live_blocks | 45906.000 | 4.243 | 45900.000 | 45909.000 |
| Terrain | cpu_live_bytes | 123495401.333 | 142234.766 | 123294251.000 | 123595998.000 |
| Terrain | cpu_peak_bytes | 123495401.333 | 142234.766 | 123294251.000 | 123595998.000 |
| Terrain | cpu_process_peak_bytes | 123617783.333 | 24019.245 | 123583815.000 | 123634789.000 |
| Terrain | cpu_profiler_overhead_bytes | 1468992.000 | 135.765 | 1468800.000 | 1469088.000 |
| Terrain | draw_calls_per_frame | 460.095 | 2.270 | 457.845 | 463.204 |
| Terrain | duration_seconds | 5.000 | 0.000 | 5.000 | 5.000 |
| Terrain | frame_count | 13546.667 | 111.655 | 13389.000 | 13633.000 |
| Terrain | frame_ms_per_frame | 0.356 | 0.003 | 0.353 | 0.360 |
| Terrain | frame_ms_per_second | 963.409 | 0.503 | 962.787 | 964.017 |
| Terrain | framegraph_dropped_spans | 459.000 | 5.715 | 452.000 | 466.000 |
| Terrain | framegraph_off_thread_dropped_spans | 459.000 | 5.715 | 452.000 | 466.000 |
| Terrain | framegraph_owner_dropped_spans | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | gpu_allocated_bytes | 220213416.000 | 0.000 | 220213416.000 | 220213416.000 |
| Terrain | gpu_allocated_bytes_per_frame | 16257.022 | 134.770 | 16152.968 | 16447.339 |
| Terrain | gpu_allocated_bytes_per_second | 44041927.462 | 150.390 | 44041735.404 | 44042102.619 |
| Terrain | gpu_live_bytes | 120606624.000 | 0.000 | 120606624.000 | 120606624.000 |
| Terrain | gpu_peak_bytes | 131787048.000 | 0.000 | 131787048.000 | 131787048.000 |
| Terrain | gpu_resources_created | 117.000 | 0.000 | 117.000 | 117.000 |
| Terrain | gpu_resources_created_per_frame | 0.009 | 0.000 | 0.009 | 0.009 |
| Terrain | gpu_resources_created_per_second | 23.400 | 0.000 | 23.399 | 23.400 |
| Terrain | imagegraph_diagnostic_frames | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | imagegraph_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Terrain | imagegraph_ms_per_second | 6.473 | 0.052 | 6.399 | 6.518 |
| Terrain | imagegraph_published_textures | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | imagegraph_wall_ms_per_frame | 0.002 | 0.000 | 0.002 | 0.002 |
| Terrain | imagegraph_wall_ms_per_second | 6.473 | 0.052 | 6.399 | 6.518 |
| Terrain | physics_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| Terrain | physics_ms_per_second | 2.702 | 0.106 | 2.594 | 2.845 |
| Terrain | physics_wall_ms_per_frame | 0.001 | 0.000 | 0.001 | 0.001 |
| Terrain | physics_wall_ms_per_second | 2.702 | 0.106 | 2.594 | 2.845 |
| Terrain | process_peak_rss_bytes | 321850026.667 | 2023170.633 | 319168512.000 | 324055040.000 |
| Terrain | render_ms_per_frame | 0.209 | 0.002 | 0.208 | 0.211 |
| Terrain | render_ms_per_second | 566.480 | 0.832 | 565.887 | 567.658 |
| Terrain | render_wall_ms_per_frame | 0.211 | 0.002 | 0.209 | 0.213 |
| Terrain | render_wall_ms_per_second | 571.631 | 0.861 | 571.006 | 572.848 |
| Terrain | replication_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | replication_ms_per_second | 0.775 | 0.020 | 0.748 | 0.793 |
| Terrain | replication_wall_ms_per_frame | 0.000 | 0.000 | 0.000 | 0.000 |
| Terrain | replication_wall_ms_per_second | 0.775 | 0.020 | 0.748 | 0.793 |
| Terrain | script_ms_per_frame | 0.098 | 0.001 | 0.096 | 0.099 |
| Terrain | script_ms_per_second | 264.293 | 1.756 | 262.050 | 266.337 |
| Terrain | submitted_frames | 13546.667 | 111.655 | 13389.000 | 13633.000 |
| Terrain | uploaded_bytes | 24054.667 | 135.974 | 23868.000 | 24188.000 |
| Terrain | uploaded_bytes_per_frame | 1.776 | 0.023 | 1.753 | 1.807 |
| Terrain | uploaded_bytes_per_second | 4810.851 | 27.183 | 4773.537 | 4837.519 |


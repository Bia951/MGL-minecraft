# GPU timing diagnostics

`MGL_GPU_PROFILE` is disabled by default:

- `1`: sample render vertex/fragment boundaries and compute/blit encoder boundaries using the device's timestamp counter set. Render descriptor copies prevent sampling attachments from leaking into later passes. Parallel render encoders are sampled as a single parent pass.
- `2`: record command buffer `GPUStartTime` / `GPUEndTime` only, without counter attachments or clock sampling. Use this to check the overhead of mode 1.

Both modes sample command buffers whose first encoder is created at a swap count divisible by 30. Results are read asynchronously after completion; no diagnostic GPU waits are introduced. The `frame` field is that first-encoder swap count, not an application frame ID. Independently submitted upload/readback buffers may share that bucket.

`MGL GPU PASS` identifies the encoder creation site, target dimensions, and current GL context program/FBO. Utility copy/clear pipelines are identified by the site; their context program is not their Metal pipeline identity. Each command buffer has up to 2048 counter samples; additional encoders beyond that limit remain unmeasured.

Render `ms` is the envelope of its four stage timestamps. Vertex and fragment stages, and command buffer intervals, can overlap. Do not sum their envelopes to estimate a frame's GPU cost. Raw `vs_begin` / `vs_end` / `fs_begin` / `fs_end` permit interval unions. These intervals can still include scheduling/dependency waits and do not measure shader ALU utilization.

Counter intervals are converted with two CPU/GPU clock pairs. Metal's CPU timestamps are already nanoseconds; they must not be multiplied by the Mach timebase. See [Apple's timestamp conversion documentation](https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time).

Profiling can affect scheduling and sampled frames. Compare mode 1 with mode 2 and normal performance summaries in the actual application before drawing conclusions.

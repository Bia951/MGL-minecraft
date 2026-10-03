# GPU timing diagnostics

`MGL_GPU_PROFILE` is disabled by default:

- `1`: sample render vertex/fragment boundaries and compute/blit encoder boundaries using the device's timestamp counter set. Render descriptor copies prevent sampling attachments from leaking into later passes. Parallel render encoders are sampled as a single parent pass.
- `2`: record command buffer `GPUStartTime` / `GPUEndTime` only, without counter attachments or clock sampling. Use this to check the overhead of mode 1.

Both modes sample command buffers whose first encoder is created at a swap count divisible by 30. Results are read asynchronously after completion; no diagnostic GPU waits are introduced. The `frame` field is that first-encoder swap count, not an application frame ID. Independently submitted upload/readback buffers may share that bucket.

`MGL GPU PASS` identifies the encoder creation site, target dimensions, and current GL context program/FBO. Utility copy/clear pipelines are identified by the site; their context program is not their Metal pipeline identity. Each command buffer has up to 2048 counter samples; additional encoders beyond that limit remain unmeasured.

Render `ms` is the envelope of its four stage timestamps. Vertex and fragment stages, and command buffer intervals, can overlap. Do not sum their envelopes to estimate a frame's GPU cost. Raw `vs_begin` / `vs_end` / `fs_begin` / `fs_end` permit interval unions. These intervals can still include scheduling/dependency waits and do not measure shader ALU utilization.

Counter intervals are converted with two CPU/GPU clock pairs. Metal's CPU timestamps are already nanoseconds; they must not be multiplied by the Mach timebase. See [Apple's timestamp conversion documentation](https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time).

Profiling can affect scheduling and sampled frames. Compare mode 1 with mode 2 and normal performance summaries in the actual application before drawing conclusions.

## Uniform arena experiments

`MGL_PLAIN_UNIFORM_ARENA=1` opts ordinary vertex/fragment plain uniforms into immutable command-buffer arena storage. It is disabled by default. UBO and SSBO storage is unchanged. `MGL_UNIFORM_VERSIONS=1` independently opts queued draws into immutable Program uniform versions; it also remains disabled by default.

Arena storage is reused only after command-buffer completion. The pool holds at most eight buffers and 32 MiB; additional in-flight storage is temporary. A bounded cache compares source bytes before reusing an arena slice within the same command buffer. Changed values append new slices, preserving bytes referenced by earlier draws.

`MGL_PACKED_ARENA_TRACE=1` reports allocations, reuse, and plain-uniform cache hits/misses. `MGL_PERF_SUMMARY=1` also enables these counters. Leave diagnostics and GPU profiling disabled for final performance comparisons.

## Single-frame capture

`MGL_CAPTURE_SWAP_FRAME=N` captures only swap N using the existing drawable readback path. It takes precedence over the repeated capture schedule when both capture options are set. Exclude the capture interval from performance measurements.

When `MGL_CAPTURE_SWAP_FRAME=N` is set together with `MGL_CAPTURE_SWAP_FRAMES=1`, the default-blit source capture targets the corresponding default-blit call once, rather than following the repeated 300-call schedule. This assumes the application's default-blit sequence tracks swap numbers; if it does not, the requested call may not occur and no source file is captured.

## Sampled render-target copy experiment

`MGL_RT_SAMPLE_COMPUTE=1` replaces nearest raster row-flip copies with compute for eligible unpacked, single-sample 2D float/normalized color textures. It is disabled by default. Unsupported formats retain raster copies. Dirty mip levels share a compute encoder, with level-zero views to support Mac GPUs that cannot write nonzero mip LODs directly. `MGL_RT_SAMPLE_COMPUTE_UNORM=0` keeps normalized formats on the raster path for format-specific comparisons.

`MGL_RT_SAMPLE_COMPUTE_VERIFY=1` asynchronously compares raw source rows against reversed destination rows for up to eight format/size combinations, including newly copied mip levels. It inserts readback blits and retains their buffers until completion; it does not wait on the CPU. Leave verification disabled for performance comparisons.

### Direct destination mip attachment experiment

`MGL_RT_SAMPLE_DIRECT_MIP=1` removes the destination single-level texture view
from the raster sampled-copy path. The render attachment addresses the original
destination texture at the copied mip level, with viewport and scissor dimensions
for that level. Source sampling keeps its single-level view, and compute copies
keep both views. The flag defaults off pending real-game correctness and FPS
comparison at fixed scene, resolution and shader settings.

`MGL_RT_SAMPLE_COPY_VERIFY=1` applies the same asynchronous raw-byte mip
verification to sampled copies made by either the raster or compute path.
`MGL_RT_SAMPLE_COMPUTE_VERIFY` remains accepted for existing diagnostic launches.
Both flags default off.

### Presented drawable ownership experiment

`MGL_RELEASE_PRESENTED_DRAWABLE=1` drops the renderer's strong reference to its
submitted drawable after command buffer submission, before requesting the next
drawable. The submitted command buffer still owns the presentation resource.
This defaults off until a fixed-scene comparison establishes whether the extra
reference contributes to drawable-pool waits.

### Deferred drawable acquisition

Drawable acquisition is deferred by default; `MGL_DEFER_DRAWABLE_ACQUIRE=0`
restores eager acquisition for comparison. This delays `CAMetalLayer` acquisition until
the default framebuffer is used or a swap must present. User-FBO work can run
before that acquisition. The drawable is released after presentation, and the
next frame does not prefetch another one. Default framebuffer draw, clear,
readback, blit, CopyTex readback, and swap still acquire a drawable when needed.
An unlocked, visible 90-second Minecraft run measured 48.67 FPS versus
30.36 FPS in the adjacent eager-acquisition comparison; this is workload evidence, not a guarantee
of the same gain in every application.

### Vertex conversion resource lifetime

The shared cache for double, integer-to-float and integer-width vertex
conversions uses `NSCache` with a 128-entry count policy and a 64 MiB cost
policy. These are eviction policies, not hard allocation limits. Content hashes
still distinguish changed source data. Encoded command buffers retain their
resources independently of cache eviction.

C-to-Objective-C Metal bridge dispatches drain their own autorelease pools,
including calls made by render loops that do not provide a per-frame pool.
Persistent resources remain owned by renderer fields or retained C slots.

### Pipeline lookup before descriptor construction

`MGL_EARLY_PIPELINE_CACHE=1` enables a renderer-local `NSCache` with a
256-entry eviction policy. Its key compares complete input bytes: VS/FS
lifetime and link generations, Metal functions, clip and raster state,
attachment formats and samples, blend and draw-buffer state, and resolved
vertex stream layout. Buffer contents are excluded; transient buffers with the
same stream grouping can reuse a pipeline. Pending program/attachment updates
use the existing descriptor construction path.

Only successful compilation of the requested descriptor populates this cache;
fallback pipelines do not. Hits skip both pipeline and vertex descriptor
construction and preserve the deferred buffer mapping and state binding path.
The experiment defaults off.

`MGL_EARLY_PIPELINE_CACHE_VERIFY=1` keeps descriptor construction on cache
hits and compares the canonical descriptor key. A mismatch reports an error
and disables the early cache for that renderer. This checks the input key in
the actual application and must be disabled for performance measurements.

The canonical PSO cache key also includes both stages' lifetime/link
generations and Metal function identities, preventing reuse of an old linked
executable when the descriptor layout is unchanged.

In the Minecraft workload, verification reported 786432 matching cache-hit
keys without a mismatch. An adjacent pair of visible, unlocked 90-second
measurements at 1708x960 with Complementary and fixed noon measured 49.93 FPS
with the early cache and 49.81 FPS without it. This did not establish a
frame-rate benefit, so the experiment remains disabled by default. A separate
five-second CPU sample saw neither descriptor construction method on the
early-cache path; this confirms the path change, not a frame-rate gain or
coverage of other applications.

### Descriptor reuse experiment

`MGL_REUSE_PIPELINE_DESCRIPTORS=1` reuses a renderer-owned pipeline/vertex
descriptor pair. Each generation resets the descriptor to its defaults and
executes the existing state translation, signature calculation and PSO lookup.
The pair is used only by synchronous pipeline creation under the renderer lock.
This defaults off pending actual-game correctness and performance comparison;
Metal does not guarantee that reset preserves internal descriptor allocations.

`MGL_SPARSE_VERTEX_SIGNATURE=1` hashes only the attributes and buffer layouts
configured during vertex descriptor generation. Both slot masks are part of the
key, and selected slots retain their final format, offset, buffer index, stride
and instance stepping fields. This avoids accessing untouched descriptor slots,
which can allocate otherwise unused Metal descriptor objects. The experiment
defaults off and can be measured independently of descriptor reuse.

After bounding the vertex conversion cache, adjacent visible, unlocked
90-second Minecraft runs at 1708x960 with Complementary and the world clock
fixed at noon measured 49.74 FPS for the baseline, 50.07 FPS with descriptor
reuse, and 49.43 FPS with sparse vertex signatures alone. Neither experiment
demonstrated a material frame-rate gain; both remain disabled by default.

### Sampling preparation before FBO rotation

`MGL_EARLY_SAMPLE_PREFLIGHT=1` prepares sampled color/depth copies before
opening the incoming draw's framebuffer encoder. The previous encoder is ended
before a stale source is copied, preserving draw ordering and stored contents.
Final resource synchronization retains its normal freshness checks and binds.
Parallel workers keep their existing path. This defaults off pending actual
game correctness and frame-rate measurements.

### SPIR-V optimization experiment

`MGL_SPIRV_OPTIMIZE=1` enables the bundled glslang's GLSL optimization passes
before SPIRV-Cross reflection and MSL translation, for both linked programs and
standalone translation. Names are retained and SPIR-V validation remains
enabled. Stage/word-count messages confirm the experimental path ran. This
defaults off pending shader resource mapping, actual-game visual checks and
fixed-scene performance measurements.

`MGL_SPIRV_OPTIMIZE=2` starts with unoptimized glslang IR and applies
SPIRV-Tools' performance passes before reflection. The pass sequence preserves
entry-point interfaces, resource bindings and specialization constants. Both
input and optimized output are validated before reflection. Failure
reports a shader compile error. This mode is also opt-in pending actual-game
correctness and performance checks.

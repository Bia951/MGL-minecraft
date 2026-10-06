# Copy/CPU submission optimization implementation status

Working baseline: `23e618f` (not `cd165bc`). No history reset or deployment was performed.

## Resource binding metadata (first CPU increment)

`MGL_RESOURCE_BINDING_PLAN=1`, default off. On successful link, private per-resource metadata records the finalized MSL texture type/data kind and buffer/sampler presence decisions. Existing Metal slots and resource IDs remain in reflection. Draw-time type queries avoid NSString keys/NSCache lookups when metadata is valid. GL binding points and sampler units remain dynamic; glUniformBlockBinding/glUniform sampler updates are not cached as static state.

Relink replaces stage reflection storage; failed links never publish new metadata. Metadata contains no retained texture/buffer pointers. Backing replacement therefore cannot leave stale Metal resources in this cache. Disabled mode retains the previous query paths.

Build: `make -j4` passed for core/ES dylibs and GLFW. The initial increment was static metadata only; the next runtime texture-plan increment is described below.

### Once-per-draw resolved texture/sampler replay (implemented)

The same default-off `MGL_RESOURCE_BINDING_PLAN` gate now prepares a one-draw collection of final vertex/fragment texture and sampler bindings. Existing scalar/array/separate-sampler/storage-image resolution, mip views, upload/copy and fallback rules are reused, with the four final texture/sampler setters recording strongly retained Metal objects rather than issuing encoder calls during preparation. Latest writes per stage/slot win, explicit nil differs from an untouched slot, and sealing prevents partial collections from being replayed. Thus default sampler warmup followed by a real sampler no longer forces redundant intermediate Metal bindings; final replay uses the existing encoder-aware pointer dedup setters.

Mapped buffer uploads and active-texture uploads run before this collection. Encoder-ending copies trigger up to three preparation attempts, discarding all partial records. Successful records are tagged with the exact command buffer/encoder and both program lifetime/link generations. Final pipeline validation/binding follows texture preparation; final resource replay does not resolve GL sampler units/textures again. The buffer maps prepared earlier are not remapped again on this successful path. Collections are consumed and released after one replay, not cached across draws/command buffers. No GL object pointers are retained by the collection.

When native depth is also enabled, private shader/PSO/texture selection is an earlier **preflight**: a depth copy may be bypassed only if that combination was already usable. Texture preparation failure cancels the native transaction and resolves the entire retry against the base shader. Final pipeline validation/binding occurs only after preparation. A command-buffer/encoder/program mismatch at replay cancels the collection, restores the base pipeline if necessary, and remaps/retries the legacy path. This is not a claim that all PSO construction has been moved after copies; native eligibility requires the preflight.

Argument-buffer stages and parallel encode retain the previous path; `MGL_PARALLEL_ENCODE=1` disables this runtime texture-plan experiment without changing scheduling. Full typed buffer/inline-byte binding records, argument-buffer preparation and all-resource ordering remain unfinished. Buffer conversions and auxiliary bindings still use their existing final binders. Shader/resource failures unrelated to the texture plan can still reject the draw, as before.

Validation: `make -j4` and `python3 tools/check_resolved_texture_bindings.py`, plus uniform snapshot and both native-depth checks with CPU switches enabled. The new offline mock sink verifies final-write collapse, stage isolation, explicit nil/untouched slots, no replay before sealing, sealed/bounds rejection and temporary resource lifetimes. It creates no GL context, Metal device or actual draw. End-to-end GPU ordering, fallback and visual/performance verification remain deferred.

## Packed uniform range reuse (second CPU increment)

`MGL_PACKED_UNIFORM_REUSE=1`, default off, independently switchable. No existing uniform-generation/snapshot experiment switch was found in this checkout, so this incremental experiment has an explicit new gate rather than silently changing an unrelated/default-on switch.

After packing, an exact immutable byte key plus program lifetime/link generation/stage/resource ID/array element/Metal slot/size identifies identical content. At most 128 cached ranges are retained per renderer, strictly within one command buffer. Hits build a fresh per-map Buffer wrapper pointing at the retained original arena/offset (including retired arenas after growth). Misses allocate monotonically and never overwrite an earlier range. Changing command buffers drops the cache; no backing arena is pooled/recycled across command buffers. Existing Metal command buffers retain encoded resources through GPU completion.

The initial increment only deduplicated immutable Metal arena ranges after packing. Exact byte comparison protects global fallback changes and hash collisions. Small UBO setBytes snapshots, GPU-written buffers, uniform-update flushes, and GL hazard handling are unchanged; old draws still execute before mutable program uniform storage is updated.

Build: `make -j4` passed for core/ES dylibs and GLFW. No game validation/performance claim. Neither CPU increment changes SPIR-V; native-depth variant validation is documented below.

### Versioned immutable CPU packing snapshots (implemented for private loose uniforms)

The same default-off `MGL_PACKED_UNIFORM_REUSE` switch now also tracks Program plain-uniform and live-context global-fallback epochs. Updates still flush queued draws before advancing epochs/mutating bytes; identical uploads retain the existing early return. Default initializers advance the Program epoch too. Relinks use the existing independent link generation; saturated epochs disable CPU caching rather than wrap/reuse an old version.

Packed loose-uniform resources cache immutable CPU bytes by program lifetime/link/Program epoch/global-fallback epoch/stage/resource/element/Metal slot/layout size. Hits skip the MSL layout scan, zeroing, member copies and matrix-padding work. Exact source witnesses additionally validate the current preferred/fallback buffer identity, data address, size and bytes before reuse. Thus a fallback change, binding replacement or unexpected unversioned CPU write cannot reuse stale packed data. Immutable NSData is also reused as the arena's exact content key, avoiding another full packed-byte copy on CPU hits.

Only private unmapped CPU glUniform storage qualifies. Public buffer name resolution/binding permanently revokes a buffer's private eligibility, including texture-buffer, storage, atomic and transform-feedback alias paths. GPU-written/public/mapped buffers retain the previous path. Captured GL pointer values are identities only; snapshots own copied bytes, not GL or Metal objects. The first capture probes CPU data readability; hits validate live private allocation identity/address/size and compare bytes without adding per-source VM syscalls.

CPU snapshots may survive command-buffer changes, but the immutable Metal arena ranges are still reused only inside one command buffer. CPU cache limits: 128 entries and 8 MiB copied payload, maximum 256 KiB packed bytes/1 MiB total source+packed payload per entry. Eviction releases cache references, never overwrites snapshots/ranges retained elsewhere. Parallel encode workers bypass the CPU snapshot cache. Existing deferred-uniform flushes remain necessary; this does not introduce draw-record-time snapshot capture or remove the GL ordering barrier.

Validation: `make -j4`, `python3 tools/check_uniform_snapshot.py`, and both native-depth offline checks. The CPU adapter covers mutable input isolation, old-version immutability, same-version source changes, array-element independence, global fallback changes/preferred-source replacement, object identity, mapped/public alias rejection and source deletion, plus default-off/explicit-enable/off switch parsing. The compiler adapter now explicitly uses the core-library context ABI and additionally checks default initializers with uniform reuse enabled. No GL API, device, game or performance run is involved. End-to-end queued draw/GPU arena behavior still awaits final real-game validation.

## Native depth status

The earlier uncommitted MSL text-substitution prototype was removed after review. It did not provide legal private SPIR-V rewriting, independent depth coordinate flip masks, operation eligibility, and final shader/resource transactional fallback. Its patch was saved outside the repository at `/tmp/mgl-native-depth-incomplete.patch` for inspection only. Do not deploy it.

### Private SPIR-V rewriter (implemented)

`MGL/src/mgl_native_depth_ir.c` provides a transactional pure IR transform. It validates both input/output with SPIRV-Tools, clones the selected sampler variable's image/sampled-image/pointer type chain, preserves original resource IDs/decorations, and explicitly reconstructs depth results as `(d,0,0,1)`. Selected resources share deduplicated private types where needed; unselected samplers retain their original types. The source module/reflection is never mutated.

Supported: fragment scalar float sampler2D, ordinary sample (including bias), LOD, Grad, texelFetch and size/levels queries. Normalized coordinate flips use `1-y`; Grad flips negate gradient Y; fetch queries the bound image's size at the requested LOD and uses `height-1-y`. Color-only flip masks are also supported by the transform without changing the image type. Array/MS/comparison, gather/offset/projection, sparse operations and opaque sampler parameter passing reject the combination. Conservative rejection/validation failure returns no output; runtime must use its existing fallback, not bypass copies.

`python3 tools/check_native_depth_ir.py` passed offline fixtures for sample/LOD/Grad/fetch/combined, shared-type isolation, reversed resource order, malformed masks/modules and unsupported-operation rejection. Every supported fixture combination passes `spirv-val`, SPIRV-Cross MSL generation and `xcrun metal` compilation. Tests do not create a GL context or launch a game. `make -j4` passed.

### Private MSL and draw transaction (implemented, game verification pending)

`MGL_NATIVE_DEPTH_SAMPLING=1`, default off and independent of CPU switches. `MGL/src/mgl_native_depth_msl.c` compiles transformed IR with copied fragment resource records and the normal compatibility/layout passes. Shared names/member metadata are read-only. Final resource bindings, auxiliary buffer requirements and every original MSL struct declaration must match; otherwise the variant is rejected. Argument-buffer stages remain on the old path.

`MGLRenderer+NativeDepth.m` prepares eligible fragment ordinary sampler2D bindings before selecting the final private pipeline. Only single-sample ShaderRead Depth32Float, default swizzle, depth-component/noncomparison sampling and valid mip views qualify. Current framebuffer feedback attachments are excluded. Selected Metal textures/samplers are strongly retained for the draw; original GL objects and backing identities are checked again before binding. Existing orientation authority determines the independent depth flip mask. Native mode also marks depth-writing draws GPU-authoritative; disabled mode preserves previous behavior.

Shader variants have a strict renderer-wide 128-entry cache (failures included), keyed by program lifetime/link generation/depth mask/flip mask. Dependent PSOs are discarded upon shader eviction; pipeline entries are separately bounded to 128 and additionally key vertex-program lifetime/generation, clip convention, attachment/vertex-layout/blend state. Cached entries retain no GL object pointers. Deleted/relinked programs cannot reuse old entries; bounded cached Metal objects may live until eviction/renderer destruction or GPU completion.

Only a successfully compiled private shader, exact compatible PSO and complete selected texture/sampler set enable the direct binding branch. That branch bypasses the selected resource's legacy depth recovery and sampled-copy resolution. Shader/view/pipeline rejection uses the original path. Binding/encoder interruption rolls the whole private transaction back to the base pipeline before retrying legacy texture binding; no draw has been issued at that point. Color copies, unsupported sampler operations and normal pipeline emergency fallbacks are unchanged. This checkout's old depth path differs from the requested historical baseline, so this is not a claim that every historical Depth-to-R32 conversion site has been removed.

`MGL_PARALLEL_ENCODE=1` disables the native-depth experiment (rather than changing parallel scheduling); workers do not yet own native resource snapshots. Native mode does not implement color-only flip/copy bypass. Without the resource-plan switch, unselected bindings can still require upload/copy during final resource replay, conservatively cancelling native mode on interruption. With it, supported vertex/fragment texture/sampler bindings are prepared before final pipeline validation/binding as described above. Full buffer/argument-buffer resolution and all-resource upload/copy ordering are still unfinished.

Validation: `make -j4`, `python3 tools/check_native_depth_ir.py`, and `python3 tools/check_native_depth_msl.py`. The new offline adapter uses internal compiler records only, not GL APIs/a GL context/a Metal device. Sample/LOD/Grad/fetch, shared color sampler isolation, UBO layouts and plain uniforms compile as Metal 3.1 with/without flips. Original Program/Shader/resource reflection is byte-compared after private compilation. Gather/offset/projection/opaque-helper uses return no private variant, with reflection unchanged.

Remaining: complete typed buffer/inline-byte and argument-buffer plans and all-resource preparation ordering, audit/validation of the new texture and private-uniform snapshot paths, and Minecraft visual/performance/four-way comparison. No standalone GL validation program, game screenshots, FPS calibration, game launch or A/B runs were added/run. Runtime wiring is present but has not been validated by real GL draws or game play; no FPS or overall-completion claim is made.

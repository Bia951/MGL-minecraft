# Copy/CPU submission optimization implementation status

Working baseline: `23e618f` (not `cd165bc`). No history reset or deployment was performed.

## Resource binding metadata (first CPU increment)

`MGL_RESOURCE_BINDING_PLAN=1`, default off. On successful link, private per-resource metadata records the finalized MSL texture type/data kind and buffer/sampler presence decisions. Existing Metal slots and resource IDs remain in reflection. Draw-time type queries avoid NSString keys/NSCache lookups when metadata is valid. GL binding points and sampler units remain dynamic; glUniformBlockBinding/glUniform sampler updates are not cached as static state.

Relink replaces stage reflection storage; failed links never publish new metadata. Metadata contains no retained texture/buffer pointers. Backing replacement therefore cannot leave stale Metal resources in this cache. Disabled mode retains the previous query paths.

Build: `make -j4` passed for core/ES dylibs and GLFW. This is static metadata only, not yet the complete once-per-draw resolved binding plan or upload/copy-before-pipeline submission ordering.

## Packed uniform range reuse (second CPU increment)

`MGL_PACKED_UNIFORM_REUSE=1`, default off, independently switchable. No existing uniform-generation/snapshot experiment switch was found in this checkout, so this incremental experiment has an explicit new gate rather than silently changing an unrelated/default-on switch.

After packing, an exact immutable byte key plus program lifetime/link generation/stage/resource ID/array element/Metal slot/size identifies identical content. At most 128 cached ranges are retained per renderer, strictly within one command buffer. Hits build a fresh per-map Buffer wrapper pointing at the retained original arena/offset (including retired arenas after growth). Misses allocate monotonically and never overwrite an earlier range. Changing command buffers drops the cache; no backing arena is pooled/recycled across command buffers. Existing Metal command buffers retain encoded resources through GPU completion.

Small UBO setBytes snapshots, GPU-written buffers, uniform-update flushes, and GL hazard handling are unchanged. Thus old draws continue to execute before mutable program uniform storage is updated. CPU packing/version reuse and queued immutable snapshot ownership have **not** been implemented by this increment; it only deduplicates immutable Metal arena ranges after packing. Exact byte comparison deliberately protects global fallback changes and hash collisions.

Build: `make -j4` passed for core/ES dylibs and GLFW. No game validation/performance claim. SPIR-V was not changed by either CPU increment, so no new SPIR-V variant exists to validate yet.

## Native depth status

The earlier uncommitted MSL text-substitution prototype was removed after review. It did not provide legal private SPIR-V rewriting, independent depth coordinate flip masks, operation eligibility, and final shader/resource transactional fallback. Its patch was saved outside the repository at `/tmp/mgl-native-depth-incomplete.patch` for inspection only. Do not deploy it.

### Private SPIR-V rewriter (implemented, not yet connected to draws)

`MGL/src/mgl_native_depth_ir.c` provides a transactional pure IR transform. It validates both input/output with SPIRV-Tools, clones the selected sampler variable's image/sampled-image/pointer type chain, preserves original resource IDs/decorations, and explicitly reconstructs depth results as `(d,0,0,1)`. Selected resources share deduplicated private types where needed; unselected samplers retain their original types. The source module/reflection is never mutated.

Supported: fragment scalar float sampler2D, ordinary sample (including bias), LOD, Grad, texelFetch and size/levels queries. Normalized coordinate flips use `1-y`; Grad flips negate gradient Y; fetch queries the bound image's size at the requested LOD and uses `height-1-y`. Color-only flip masks are also supported by the transform without changing the image type. Array/MS/comparison, gather/offset/projection, sparse operations and opaque sampler parameter passing reject the combination. Conservative rejection/validation failure returns no output; runtime must use its existing fallback, not bypass copies.

`python3 tools/check_native_depth_ir.py` passed offline fixtures for sample/LOD/Grad/fetch/combined, shared-type isolation, reversed resource order, malformed masks/modules and unsupported-operation rejection. Every supported fixture combination passes `spirv-val`, SPIRV-Cross MSL generation and `xcrun metal` compilation. Tests do not create a GL context or launch a game. `make -j4` passed.

No runtime native-depth switch/copy bypass is published yet: private MSL ABI parity, bounded program-lifecycle variant cache, final pipeline/texture transaction and resource preparation ordering still need integration. Native-depth draws, full binding resolution, generation-keyed CPU snapshots, and Minecraft performance/visual verification remain pending. No independent GL validation executable, game screenshots, FPS calibration, or A/B runs were added/run.

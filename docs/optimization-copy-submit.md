# Copy/CPU submission optimization implementation status

Working baseline: `23e618f` (not `cd165bc`). No history reset or deployment was performed.

## Resource binding metadata (first CPU increment)

`MGL_RESOURCE_BINDING_PLAN=1`, default off. On successful link, private per-resource metadata records the finalized MSL texture type/data kind and buffer/sampler presence decisions. Existing Metal slots and resource IDs remain in reflection. Draw-time type queries avoid NSString keys/NSCache lookups when metadata is valid. GL binding points and sampler units remain dynamic; glUniformBlockBinding/glUniform sampler updates are not cached as static state.

Relink replaces stage reflection storage; failed links never publish new metadata. Metadata contains no retained texture/buffer pointers. Backing replacement therefore cannot leave stale Metal resources in this cache. Disabled mode retains the previous query paths.

Build: `make -j4` passed for core/ES dylibs and GLFW. This is static metadata only, not yet the complete once-per-draw resolved binding plan or upload/copy-before-pipeline submission ordering.

## Native depth status

The earlier uncommitted MSL text-substitution prototype was removed after review. It did not provide legal private SPIR-V rewriting, independent depth coordinate flip masks, operation eligibility, and final shader/resource transactional fallback. Its patch was saved outside the repository at `/tmp/mgl-native-depth-incomplete.patch` for inspection only. Do not deploy it.

Native depth, full binding resolution, generation-keyed CPU snapshots, and Minecraft performance/visual verification remain pending. No independent GL validation executable, game screenshots, FPS calibration, or A/B runs were added/run.

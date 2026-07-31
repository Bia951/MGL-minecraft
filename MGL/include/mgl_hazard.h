/*
 * Copyright (C) Michael Larson on 1/6/2022
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * mgl_hazard.h
 * MGL
 *
 * Lightweight per-resource access-state tracking for the glMemoryBarrier
 * hazard model (Spec R4 / Task 4).
 *
 * Each Metal-backed resource (Buffer / Texture) records its last access kind
 * and the command-buffer serial in which it occurred.  The renderer keeps an
 * aggregate MGLHazardState recording whether compute / render / blit writes
 * are pending in the current (uncommitted) command buffer.
 *
 * mglMemoryBarrier consults this state together with the barrier bits to
 * decide between:
 *
 *   - GPU->GPU visibility within the same command buffer — free: Metal
 *     guarantees that encoders execute in encoding order, so writes from an
 *     earlier encoder are visible to a later encoder.  Ending the current
 *     encoder is enough; no commit, no CPU wait.
 *   - CPU visibility — commit + waitUntilCompleted, used ONLY for
 *     GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT (the only bit whose spec semantics
 *     promise CPU observability of prior GPU writes).
 *
 * The design is deliberately minimal: it records enough state to make the
 * per-bit decision required by R4 without a heavy per-resource dependency
 * graph.  The per-resource fields are two scalars (access kind + CB serial)
 * so the overhead per Buffer/Texture is 9 bytes.
 */
#ifndef MGL_HAZARD_H
#define MGL_HAZARD_H

#include <stdint.h>

/* Last access kind recorded per resource.  Stored as uint8_t in the
 * Buffer / Texture structs to keep the per-resource overhead small. */
#define MGL_HAZARD_NONE            0u
#define MGL_HAZARD_CPU_READ        1u
#define MGL_HAZARD_CPU_WRITE       2u
#define MGL_HAZARD_RENDER_READ     3u
#define MGL_HAZARD_RENDER_WRITE    4u
#define MGL_HAZARD_COMPUTE_READ    5u
#define MGL_HAZARD_COMPUTE_WRITE   6u
#define MGL_HAZARD_BLIT_READ       7u
#define MGL_HAZARD_BLIT_WRITE      8u

/* Aggregate write-pending state for the current command buffer.
 *
 * The pending flags are set when an encoder writes resources into the
 * current (uncommitted) command buffer, and cleared when the command buffer
 * is committed / rotated (newCommandBuffer).  cb_serial is monotonic and
 * tags the current command buffer so per-resource state can be invalidated
 * across rotations. */
typedef struct MGLHazardState_t {
    uint8_t  compute_writes_pending;
    uint8_t  render_writes_pending;
    uint8_t  blit_writes_pending;
    uint8_t  _pad;
    uint64_t cb_serial;
} MGLHazardState;

/* Called when a fresh command buffer is created.  Clears the pending-write
 * flags (the old CB has been committed, so its writes are no longer
 * "pending in the current CB") and advances the serial so per-resource
 * access stamps from the previous CB are recognizably stale. */
static inline void mglHazardResetForNewCB(MGLHazardState *s) {
    if (s) {
        s->compute_writes_pending = 0;
        s->render_writes_pending = 0;
        s->blit_writes_pending = 0;
        s->cb_serial++;
    }
}

#endif /* MGL_HAZARD_H */

/**************************************************************************
 *
 * Copyright 2013-2014 Alexander von Gluck IV <kallisti5@unixzen.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * Authors:
 *	Alexander von Gluck IV <kallisti5@unixzen.com>
 *
 **************************************************************************/


#include "os/os_thread.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_debug.h"
#include "../intel_winsys.h"

#include "intel_hgl_winsys.h"


#define BATCH_SZ (8192 * sizeof(uint32_t))


struct intel_winsys {
	int fd;
	struct intel_winsys_info info;

	/* these are protected by the mutex */
	pipe_mutex mutex;
};


static bool
probe_winsys(struct intel_winsys *winsys)
{
	struct intel_winsys_info *info = &winsys->info;

	//info->devid = drm_intel_bufmgr_gem_get_devid(winsys->bufmgr);
	info->devid = 0;

	info->max_batch_size = BATCH_SZ;

	info->has_llc = false; // has I915_PARAM_HAS_LLC
	info->has_address_swizzling = false; // test_address_swizzling

	// TODO: logical context is first instance of access
	info->has_logical_context = true;

	info->has_ppgtt = false; // has I915_PARAM_HAS_ALIASING_PPGTT
	//info->has_timestamp = test_reg_read(winsys, 0x2358);
	info->has_timestamp = false;
	info->has_gen7_sol_reset = false; // has I915_PARAM_HAS_GEN7_SOL_RESET

	return true;
}


struct intel_winsys *
intel_winsys_create_for_fd(int fd)
{
	struct intel_winsys* winsys = CALLOC_STRUCT(intel_winsys);

	if (!winsys)
		return NULL;

	winsys->fd = fd;

	// TODO: Do we need GEM? Area's may be enough

	pipe_mutex_init(winsys->mutex);

	if (!probe_winsys(winsys)) {
		pipe_mutex_destroy(winsys->mutex);
		FREE(winsys);
		return NULL;
	}

	return winsys;
}

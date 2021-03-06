/*
 * Copyright 2012, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Artur Wyszynski, harakash@gmail.com
 *		Alexander von Gluck IV, kallisti5@unixzen.com
 */


#include "GalliumContext.h"

#include "GLView.h"

#include "bitmap_wrapper.h"
extern "C" {
#include "glapi/glapi.h"
#include "main/context.h"
#include "main/framebuffer.h"
#include "main/renderbuffer.h"
#include "main/viewport.h"
#include "pipe/p_format.h"
#include "state_tracker/st_cb_fbo.h"
#include "state_tracker/st_cb_flush.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_gl_api.h"
#include "state_tracker/st_manager.h"
#include "state_tracker/sw_winsys.h"
#include "util/u_memory.h"

#include "intel/intel_winsys.h"
#include "ilo/ilo_public.h"
#include "target-helpers/inline_debug_helper.h"

#include "intel_hgl_winsys.h"
}


#ifdef DEBUG
#	define TRACE(x...) printf("GalliumContext: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
#	define CALLED()
#endif
#define ERROR(x...) printf("GalliumContext: " x)


static struct pipe_screen *
pipe_ilo_create_screen(int fd)
{
	struct intel_winsys* iws = intel_winsys_create_for_fd(fd);
	if (!iws)
		return NULL;

	struct pipe_screen* screen = ilo_screen_create(iws);
	return screen ? debug_screen_wrap(screen) : NULL;
}


static void
hgl_viewport(struct gl_context* glContext)
{
	// TODO: We should try to eliminate this function

	GLint x = glContext->ViewportArray[0].X;
	GLint y = glContext->ViewportArray[0].Y;
	GLint width = glContext->ViewportArray[0].Width;
	GLint height = glContext->ViewportArray[0].Height;

	TRACE("%s(glContext: %p, x: %d, y: %d, w: %d, h: %d\n", __func__,
		glContext, x, y, width, height);

	_mesa_check_init_viewport(glContext, width, height);

	struct gl_framebuffer *draw = glContext->WinSysDrawBuffer;
	struct gl_framebuffer *read = glContext->WinSysReadBuffer;

	if (draw)
		_mesa_resize_framebuffer(glContext, draw, width, height);
	if (read)
		_mesa_resize_framebuffer(glContext, read, width, height);
}


static int
hook_stm_get_param(struct st_manager *smapi, enum st_manager_param param)
{
	CALLED();

	switch (param) {
		case ST_MANAGER_BROKEN_INVALIDATE:
			TRACE("%s: TODO: How should we handle BROKEN_INVALIDATE calls?\n",
				__func__);
			// For now we force validation of the framebuffer.
			return 1;
	}

	return 0;
}


GalliumContext::GalliumContext(ulong options)
	:
	fOptions(options),
	fScreen(NULL),
	fCurrentContext(0)
{
	CALLED();

	// Make all contexts a known value
	for (context_id i = 0; i < CONTEXT_MAX; i++)
		fContext[i] = NULL;

	CreateScreen();

	pipe_mutex_init(fMutex);
}


GalliumContext::~GalliumContext()
{
	CALLED();

	// Destroy our contexts
	Lock();
	for (context_id i = 0; i < CONTEXT_MAX; i++)
		DestroyContext(i);
	Unlock();

	pipe_mutex_destroy(fMutex);

	// TODO: Destroy fScreen
}


struct st_visual*
GalliumContext::CreateVisual()
{
	struct st_visual* visual = CALLOC_STRUCT(st_visual);
	if (!visual) {
		ERROR("%s: Couldn't allocate st_visual\n", __func__);
		return NULL;
	}

	// Calculate visual configuration
	const GLboolean rgbFlag		= ((fOptions & BGL_INDEX) == 0);
	const GLboolean alphaFlag	= ((fOptions & BGL_ALPHA) == BGL_ALPHA);
	const GLboolean dblFlag		= ((fOptions & BGL_DOUBLE) == BGL_DOUBLE);
	const GLboolean stereoFlag	= false;
	const GLint depth			= (fOptions & BGL_DEPTH) ? 24 : 0;
	const GLint stencil			= (fOptions & BGL_STENCIL) ? 8 : 0;
	const GLint accum			= (fOptions & BGL_ACCUM) ? 16 : 0;
	const GLint red				= rgbFlag ? 8 : 5;
	const GLint green			= rgbFlag ? 8 : 5;
	const GLint blue			= rgbFlag ? 8 : 5;
	const GLint alpha			= alphaFlag ? 8 : 0;

	TRACE("rgb      :\t%d\n", (bool)rgbFlag);
	TRACE("alpha    :\t%d\n", (bool)alphaFlag);
	TRACE("dbl      :\t%d\n", (bool)dblFlag);
	TRACE("stereo   :\t%d\n", (bool)stereoFlag);
	TRACE("depth    :\t%d\n", depth);
	TRACE("stencil  :\t%d\n", stencil);
	TRACE("accum    :\t%d\n", accum);
	TRACE("red      :\t%d\n", red);
	TRACE("green    :\t%d\n", green);
	TRACE("blue     :\t%d\n", blue);
	TRACE("alpha    :\t%d\n", alpha);

	// Determine color format
	if (red == 8) {
		if (alpha == 8)
			visual->color_format = PIPE_FORMAT_A8R8G8B8_UNORM;
		else
			visual->color_format = PIPE_FORMAT_X8R8G8B8_UNORM;
	} else {
		// TODO: I think this should be RGB vs BGR
		visual->color_format = PIPE_FORMAT_B5G6R5_UNORM;
	}

	// Determine depth stencil format
	switch (depth) {
		default:
		case 0:
			visual->depth_stencil_format = PIPE_FORMAT_NONE;
			break;
		case 16:
			visual->depth_stencil_format = PIPE_FORMAT_Z16_UNORM;
			break;
		case 24:
			if ((fOptions & BGL_STENCIL) != 0)
				visual->depth_stencil_format = PIPE_FORMAT_S8_UINT_Z24_UNORM;
			else
				visual->depth_stencil_format = PIPE_FORMAT_X8Z24_UNORM;
			break;
		case 32:
			visual->depth_stencil_format = PIPE_FORMAT_Z32_UNORM;
			break;
	}

	visual->accum_format = (fOptions & BGL_ACCUM)
		? PIPE_FORMAT_R16G16B16A16_SNORM : PIPE_FORMAT_NONE;

	visual->buffer_mask |= ST_ATTACHMENT_FRONT_LEFT_MASK;
	visual->render_buffer = ST_ATTACHMENT_FRONT_LEFT;

	if (dblFlag) {
		visual->buffer_mask |= ST_ATTACHMENT_BACK_LEFT_MASK;
		visual->render_buffer = ST_ATTACHMENT_BACK_LEFT;
	}

	if (stereoFlag) {
		visual->buffer_mask |= ST_ATTACHMENT_FRONT_RIGHT_MASK;
		if (dblFlag)
			visual->buffer_mask |= ST_ATTACHMENT_BACK_RIGHT_MASK;
	}

	if ((fOptions & BGL_DEPTH) || (fOptions & BGL_STENCIL)) 
		visual->buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;

	return visual;
}


status_t
GalliumContext::CreateScreen()
{
	CALLED();

	// Allocate winsys and attach callback hooks
	// TODO: Get FD for driver
	fScreen = pipe_ilo_create_screen(0);

	if (fScreen == NULL) {
		ERROR("%s: Couldn't create screen!\n", __FUNCTION__);
		return B_ERROR;
	}

	const char* driverName = fScreen->get_name(fScreen);
	ERROR("%s: Using %s driver.\n", __func__, driverName);

	return B_OK;
}


context_id
GalliumContext::CreateContext(Bitmap *bitmap)
{
	CALLED();

	struct hgl_context* context = CALLOC_STRUCT(hgl_context);

	if (!context) {
		ERROR("%s: Couldn't create pipe context!\n", __FUNCTION__);
		return 0;
	}

	// Set up the initial things our context needs
	context->bitmap = bitmap;
	context->colorSpace = get_bitmap_color_space(bitmap);
	context->draw = NULL;
	context->read = NULL;
	context->st = NULL;

	context->api = st_gl_api_create();
	if (!context->api) {
		ERROR("%s: Couldn't obtain Mesa state tracker API!\n", __func__);
		return -1;
	}

	context->manager = CALLOC_STRUCT(st_manager);
	if (!context->manager) {
		ERROR("%s: Couldn't allocate Mesa state tracker manager!\n", __func__);
		return -1;
	}
	context->manager->get_param = hook_stm_get_param;

	// Convert Mesa calculated visual into state tracker visual
	context->stVisual = CreateVisual();
	if (context->stVisual == NULL) {
		ERROR("%s: Couldn't create state_tracker visual!\n", __func__);
		return -1;
	}

	context->draw = new GalliumFramebuffer(context->stVisual, (void*)this);
	context->read = new GalliumFramebuffer(context->stVisual, (void*)this);

	if (!context->draw || !context->read) {
		ERROR("%s: Problem allocating framebuffer!\n", __func__);
		FREE(context->stVisual);
		return -1;
	}

	// We need to assign the screen *before* calling st_api create_context
	context->manager->screen = fScreen;

	// Build state tracker attributes
	struct st_context_attribs attribs;
	memset(&attribs, 0, sizeof(attribs));
	attribs.options.force_glsl_extensions_warn = false;
	attribs.profile = ST_PROFILE_DEFAULT;
	attribs.visual = *context->stVisual;
	attribs.major = 1;
	attribs.minor = 0;
	//attribs.flags |= ST_CONTEXT_FLAG_DEBUG;

	struct st_api* api = context->api;

	// Create context using state tracker api call
	enum st_context_error result;
	context->st = api->create_context(api, context->manager, &attribs,
		&result, context->st);

	if (!context->st) {
		ERROR("%s: Couldn't create mesa state tracker context!\n",
			__func__);
		switch (result) {
			case ST_CONTEXT_SUCCESS:
				ERROR("%s: State tracker error: SUCCESS?\n", __func__);
				break;
			case ST_CONTEXT_ERROR_NO_MEMORY:
				ERROR("%s: State tracker error: NO_MEMORY\n", __func__);
				break;
			case ST_CONTEXT_ERROR_BAD_API:
				ERROR("%s: State tracker error: BAD_API\n", __func__);
				break;
			case ST_CONTEXT_ERROR_BAD_VERSION:
				ERROR("%s: State tracker error: BAD_VERSION\n", __func__);
				break;
			case ST_CONTEXT_ERROR_BAD_FLAG:
				ERROR("%s: State tracker error: BAD_FLAG\n", __func__);
				break;
			case ST_CONTEXT_ERROR_UNKNOWN_ATTRIBUTE:
				ERROR("%s: State tracker error: BAD_ATTRIBUTE\n", __func__);
				break;
			case ST_CONTEXT_ERROR_UNKNOWN_FLAG:
				ERROR("%s: State tracker error: UNKNOWN_FLAG\n", __func__);
				break;
		}

		FREE(context);
		return -1;
	}

	assert(!context->st->st_manager_private);
	context->st->st_manager_private = (void*)context;

	struct st_context *stContext = (struct st_context*)context->st;
	
	stContext->ctx->Driver.Viewport = hgl_viewport;

	// Init Gallium3D Post Processing
	// TODO: no pp filters are enabled yet through postProcessEnable
	context->postProcess = pp_init(stContext->pipe, context->postProcessEnable,
		stContext->cso_context);

	context_id contextNext = -1;
	Lock();
	for (context_id i = 0; i < CONTEXT_MAX; i++) {
		if (fContext[i] == NULL) {
			fContext[i] = context;
			contextNext = i;
			break;
		}
	}
	Unlock();

	if (contextNext < 0) {
		ERROR("%s: The next context is invalid... something went wrong!\n",
			__func__);
		//st_destroy_context(context->st);
		FREE(context->stVisual);
		FREE(context);
		return -1;
	}

	TRACE("%s: context #%" B_PRIu64 " is the next available context\n",
		__func__, contextNext);

	return contextNext;
}


void
GalliumContext::DestroyContext(context_id contextID)
{
	// fMutex should be locked *before* calling DestoryContext

	// See if context is used
	if (!fContext[contextID])
		return;

	if (fContext[contextID]->st) {
		fContext[contextID]->st->flush(fContext[contextID]->st, 0, NULL);
		fContext[contextID]->st->destroy(fContext[contextID]->st);
	}

	if (fContext[contextID]->postProcess)
		pp_free(fContext[contextID]->postProcess);

	// Delete framebuffer objects
	if (fContext[contextID]->read)
		delete fContext[contextID]->read;
	if (fContext[contextID]->draw)
		delete fContext[contextID]->draw;

	if (fContext[contextID]->stVisual)
		FREE(fContext[contextID]->stVisual);

	if (fContext[contextID]->manager)
		FREE(fContext[contextID]->manager);

	FREE(fContext[contextID]);
}


status_t
GalliumContext::SetCurrentContext(Bitmap *bitmap, context_id contextID)
{
	CALLED();

	if (contextID < 0 || contextID > CONTEXT_MAX) {
		ERROR("%s: Invalid context ID range!\n", __func__);
		return B_ERROR;
	}

	Lock();
	context_id oldContextID = fCurrentContext;
	struct hgl_context* context = fContext[contextID];
	Unlock();

	if (!context) {
		ERROR("%s: Invalid context provided (#%" B_PRIu64 ")!\n",
			__func__, contextID);
		return B_ERROR;
	}

	struct st_api* api = context->api;

	if (!bitmap) {
		api->make_current(context->api, NULL, NULL, NULL);
		return B_OK;
	}

	// Everything seems valid, lets set the new context.
	fCurrentContext = contextID;

	if (oldContextID > 0 && oldContextID != contextID) {
		fContext[oldContextID]->st->flush(fContext[oldContextID]->st,
			ST_FLUSH_FRONT, NULL);
	}

	// We need to lock and unlock framebuffers before accessing them
	context->draw->Lock();
	context->read->Lock();
	api->make_current(context->api, context->st, context->draw->fBuffer,
		context->read->fBuffer);
	context->draw->Unlock();
	context->read->Unlock();

	if (context->textures[ST_ATTACHMENT_BACK_LEFT]
		&& context->textures[ST_ATTACHMENT_DEPTH_STENCIL]
		&& context->postProcess) {
		TRACE("Postprocessing textures...\n");
		pp_init_fbos(context->postProcess,
			context->textures[ST_ATTACHMENT_BACK_LEFT]->width0,
			context->textures[ST_ATTACHMENT_BACK_LEFT]->height0);
	}

	context->bitmap = bitmap;
	//context->st->pipe->priv = context;

	return B_OK;
}


status_t
GalliumContext::SwapBuffers(context_id contextID)
{
	CALLED();

	Lock();
	struct hgl_context *context = fContext[contextID];
	Unlock();

	if (!context) {
		ERROR("%s: context not found\n", __func__);
		return B_ERROR;
	}

	// TODO: Where did st_notify_swapbuffers go?
	//st_notify_swapbuffers(context->draw->stfb);

	context->st->flush(context->st, ST_FLUSH_FRONT, NULL);

	struct st_context *stContext = (struct st_context*)context->st;

	unsigned nColorBuffers = stContext->state.framebuffer.nr_cbufs;
	for (unsigned i = 0; i < nColorBuffers; i++) {
		pipe_surface* surface = stContext->state.framebuffer.cbufs[i];
		if (!surface) {
			ERROR("%s: Color buffer %d invalid!\n", __func__, i);
			continue;
		}

		TRACE("%s: Flushing color buffer #%d\n", __func__, i);

		// We pass our destination bitmap to flush_fronbuffer which passes it
		// to the private winsys display call.
		fScreen->flush_frontbuffer(fScreen, surface->texture, 0, 0,
			context->bitmap, NULL);
	}

	#if 0
	// TODO... should we flush the z stencil buffer?
	pipe_surface* zSurface = stContext->state.framebuffer.zsbuf;
	fScreen->flush_frontbuffer(fScreen, zSurface->texture, 0, 0,
		context->bitmap, NULL);
	#endif

	return B_OK;
}


void
GalliumContext::ResizeViewport(int32 width, int32 height)
{
	CALLED();
	for (context_id i = 0; i < CONTEXT_MAX; i++) {
		if (fContext[i] && fContext[i]->st) {
			struct st_context *stContext = (struct st_context*)fContext[i]->st;
			_mesa_set_viewport(stContext->ctx, 0, 0, 0, width, height);
        		st_manager_validate_framebuffers(stContext);
		}
	}
}


void
GalliumContext::Lock()
{
	CALLED();
	pipe_mutex_lock(fMutex);
}


void
GalliumContext::Unlock()
{
	CALLED();
	pipe_mutex_unlock(fMutex);
}
/* vim: set tabstop=4: */

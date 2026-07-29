/*=============================================================================
	OpenGLDrvHooks.h: OpenGLDrv public hook surface (cross-platform port,
	Phase 5).

	The ImGui editor frame (EdGui) draws its UI after the world render:
	under UNREAL_USE_SDL, OpenGLDrv invokes GGLPostRenderHook right before
	SDL_GL_SwapWindow, with the GL context current — the natural place for
	an immediate-mode overlay. NULL when no UI overlay is installed.
=============================================================================*/

#ifndef OPENGLDRV_API
	#define OPENGLDRV_API DLL_IMPORT
#endif

class UViewport;

// Called with the GL context current, after the frame, before the swap.
extern OPENGLDRV_API void (*GGLPostRenderHook)( UViewport* Viewport );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

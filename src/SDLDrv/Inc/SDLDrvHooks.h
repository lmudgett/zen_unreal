/*=============================================================================
	SDLDrvHooks.h: SDLDrv public hook surface (cross-platform port, Phase 5).

	The ImGui editor frame (EdGui) taps the SDL event stream ahead of the
	engine: SDLDrv calls GSDLEventHook for every polled event; a nonzero
	return means the hook consumed it (UI has focus) and the engine input
	translation is skipped. SDL_EVENT_QUIT is always forwarded regardless.
=============================================================================*/

#ifndef SDLDRV_API
	#define SDLDRV_API DLL_IMPORT
#endif

union SDL_Event;

// First-chance SDL event filter; NULL when no UI overlay is installed.
extern SDLDRV_API UBOOL (*GSDLEventHook)( const union SDL_Event* Ev );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

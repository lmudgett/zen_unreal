/*=============================================================================
	EdGui.h: Dear ImGui editor frame (cross-platform port, Phase 5).

	Public surface of the ImGui-based UnrealEd shell that replaces the
	retired Visual Basic frontend. EdGui is a static library linked into
	the SDL launcher; it installs itself on the SDLDrv event hook and the
	OpenGLDrv post-render hook, so the UI draws as an overlay on the SDL
	window and takes input focus ahead of the engine when a panel wants it.
=============================================================================*/

#ifndef _INC_EDGUI
#define _INC_EDGUI

class UEngine;

// Install the editor UI over the given engine (call once after InitEngine,
// before the main loop; editor mode only). Safe no-op if already installed.
void EdGuiInstall( UEngine* InEngine );

// Tear down the UI and restore the hooks (call before ExitEngine).
void EdGuiShutdown();

#endif

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

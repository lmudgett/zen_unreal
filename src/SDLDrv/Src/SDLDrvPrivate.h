/*=============================================================================
	SDLDrvPrivate.h: SDL windowing/input driver (cross-platform port).

	The portable replacement for WinDrv: implements the engine's UClient /
	UViewport contract on SDL2 (window, OpenGL surface, input) so the game
	runs on Linux/macOS as well as Windows. Selected via
	[Engine.Engine] ViewportManager=SDLDrv.SDLClient. OpenGLDrv creates its
	GL context from the SDL window (see its UNREAL_USE_SDL path).

	Author: Len Mudgett
=============================================================================*/

#include "Engine.h"
#include "UnRender.h"

#define SDL_MAIN_HANDLED   // the engine owns main(); don't let SDL hijack it
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   // SDL_SetMainReady

class USDLViewport;
class USDLClient;

/*-----------------------------------------------------------------------------
	USDLViewport.
-----------------------------------------------------------------------------*/

class DLL_EXPORT USDLViewport : public UViewport
{
	DECLARE_CLASS(USDLViewport,UViewport,CLASS_Transient)
	NO_DEFAULT_CONSTRUCTOR(USDLViewport)

	// Variables.
	USDLClient*		Client;			// Owning client.
	SDL_Window*		Window;			// The SDL window (NULL until opened).
	UBOOL			Fullscreen;		// Currently fullscreen.
	UBOOL			HoldingCapture;	// Mouse currently captured (relative mode).

	// Constructors.
	USDLViewport( ULevel* InLevel, USDLClient* InClient );

	// UObject interface.
	void Destroy();

	// UViewport interface.
	void SetModeCursor();
	void UpdateWindow();
	void OpenWindow( DWORD ParentWindow, UBOOL Temporary, INT NewX, INT NewY, INT OpenX, INT OpenY );
	void CloseWindow();
	void UpdateInput( UBOOL Reset );
	void MakeCurrent();
	void* GetWindow();
	void SetMouseCapture( UBOOL Capture, UBOOL Clip, UBOOL FocusOnly=0 );
	void Repaint();
	void MakeFullscreen( INT NewX, INT NewY, UBOOL SaveConfig );
	UBOOL Exec( const char* Cmd, FOutputDevice* Out=GSystem );

	// USDLViewport interface.
	void Resized( INT NewX, INT NewY );
	UBOOL CauseInputEvent( INT iKey, EInputAction Action, FLOAT Delta=0.f );
};

/*-----------------------------------------------------------------------------
	UNullRenderDevice.
-----------------------------------------------------------------------------*/

// x64 port: retail WinDrv gave every viewport a render device -- temporary
// (offscreen) ones got the software renderer (WindowedRenderDevice). The
// editor lighting raytracer (shadowIlluminateBsp -> GetVisibleSurfs) Locks
// such a viewport, and UViewport::Lock check(RenDev) fires without one. The
// raytracer's visibility work is all software (OccludeBsp); the device is
// only a formality, so temporary SDL viewports get this no-op device.
class DLL_EXPORT UNullRenderDevice : public URenderDevice
{
	DECLARE_CLASS(UNullRenderDevice,URenderDevice,CLASS_Transient)

	// URenderDevice interface.
	UBOOL Init( UViewport* InViewport ) { Viewport=InViewport; SpanBased=1; FrameBuffered=0; return 1; }
	void Exit() {}
	void Flush() {}
	UBOOL Exec( const char* Cmd, FOutputDevice* Out ) { return 0; }
	void Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize ) {}
	void Unlock( UBOOL Blit ) {}
	void DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet ) {}
	void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span ) {}
	void DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags ) {}
	void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 ) {}
	void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 ) {}
	void ClearZ( FSceneNode* Frame ) {}
	void PushHit( const BYTE* Data, INT Count ) {}
	void PopHit( INT Count, UBOOL bForce ) {}
	void GetStats( char* Result ) { if( Result ) *Result=0; }
	void ReadPixels( FColor* Pixels ) {}
};

/*-----------------------------------------------------------------------------
	USDLClient.
-----------------------------------------------------------------------------*/

class DLL_EXPORT USDLClient : public UClient
{
	DECLARE_CLASS(USDLClient,UClient,CLASS_Transient|CLASS_Config)

	// Configuration.
	UBOOL StartupFullscreen;

	// Constructors.
	USDLClient();
	static void InternalClassInitializer( UClass* Class );

	// UObject interface.
	void Destroy();

	// UClient interface.
	void Init( UEngine* InEngine );
	void ShowViewportWindows( DWORD ShowFlags, int DoShow );
	void EnableViewportWindows( DWORD ShowFlags, int DoEnable );
	void Poll();
	UViewport* CurrentViewport();
	void Tick();
	UBOOL Exec( const char* Cmd, FOutputDevice* Out=GSystem );
	UViewport* NewViewport( ULevel* InLevel, const FName Name );
	void EndFullscreen();

	// USDLClient interface.
	void PumpEvents();
};

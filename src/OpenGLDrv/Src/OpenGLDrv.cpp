/*=============================================================================
	OpenGLDrv.cpp: Modern OpenGL rendering device for the Unreal x64 port.

	This module replaces the retired legacy renderers (SoftDrv software
	rasterizer, GlideDrv 3dfx driver, SglDrv PowerVR driver) with a single
	hardware renderer running on current OpenGL drivers.

	Implementation notes:
	* A 4.6 compatibility-profile context is requested first (via
	  WGL_ARB_create_context), falling back to whatever the driver offers.
	  The draw path currently uses the compatibility pipeline; migrating
	  hot paths to VBO/shader submission is a planned follow-up that will
	  not change this module's interface.
	* World geometry arrives from the engine already transformed to eye
	  space (FTransform::Point). We rebuild the engine's projection with
	  glFrustum, so texture perspective correction and depth buffering
	  are done by the GPU - the jobs the 1998 code did with hand-written
	  MMX and FPU assembly.
	* Textures arrive as palettized P8 or 32-bit RGBA mips; they are
	  converted and uploaded on first use, keyed by the engine's 64-bit
	  CacheID. Realtime textures (fire, water) re-upload when flagged.

	Author: Len Mudgett
=============================================================================*/

#pragma warning( disable : 4201 )
#define STRICT
#if _WIN32
#include <windows.h>
#endif
// cross-platform port: under SDL, SDL owns the GL context and header; the
// renderer itself is fixed-function GL and portable. wgl is used otherwise.
#if UNREAL_USE_SDL
	#define SDL_MAIN_HANDLED
	#include <SDL3/SDL.h>
	#include <SDL3/SDL_opengl.h>
#else
	#include <GL/gl.h>
#endif
#include "Engine.h"
#include "UnRender.h"

// GL 1.3 texture-combine tokens for the DetailTexture pass. The default build's
// <GL/gl.h> is OpenGL 1.1 and lacks them; the runtime context is 4.6 either
// way, so define any the header omits (values are the standard GL enums).
#ifndef GL_COMBINE
	#define GL_COMBINE            0x8570
#endif
#ifndef GL_COMBINE_RGB
	#define GL_COMBINE_RGB        0x8571
#endif
#ifndef GL_INTERPOLATE
	#define GL_INTERPOLATE        0x8575
#endif
#ifndef GL_CONSTANT
	#define GL_CONSTANT           0x8576
#endif
#ifndef GL_PRIMARY_COLOR
	#define GL_PRIMARY_COLOR      0x8577
#endif
#ifndef GL_SOURCE0_RGB
	#define GL_SOURCE0_RGB        0x8580
	#define GL_SOURCE1_RGB        0x8581
	#define GL_SOURCE2_RGB        0x8582
#endif
#ifndef GL_OPERAND0_RGB
	#define GL_OPERAND0_RGB       0x8590
	#define GL_OPERAND1_RGB       0x8591
	#define GL_OPERAND2_RGB       0x8592
#endif

/*-----------------------------------------------------------------------------
	GL 1.2+ / WGL constants missing from the SDK's GL 1.1 header.
-----------------------------------------------------------------------------*/

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                  0x812F
#endif
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS               0x8501
#endif

#if !UNREAL_USE_SDL
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARB)( HDC hDC, HGLRC hShareContext, const int* attribList );
typedef BOOL  (WINAPI *PFNWGLSWAPINTERVALEXT)( int interval );
#endif

/*-----------------------------------------------------------------------------
	UOpenGLRenderDevice definition.
-----------------------------------------------------------------------------*/

//
// A cached, uploaded texture.
//
struct FCachedTexture
{
	QWORD			CacheID;		// Engine cache id.
	GLuint			GLId;			// GL texture object.
	FLOAT			UMult, VMult;	// Texel -> normalized texture coordinate scale.
	FCachedTexture*	Next;			// Hash chain.
};

//
// The modern OpenGL rendering device.
//
class DLL_EXPORT UOpenGLRenderDevice : public URenderDevice
{
	DECLARE_CLASS(UOpenGLRenderDevice,URenderDevice,CLASS_Config)

	// Configuration.
	UBOOL			UseVSync;

	// Screen brightness (x64 port): the video-menu Brightness slider (UClient::
	// Brightness, 0..1, 0.5=neutral) is baked into texture colors at upload via a
	// gamma LUT -- applied ONCE per texture, so it can't accumulate like a per-
	// frame blend, and it works windowed (unlike a hardware gamma ramp). When the
	// slider moves, AppliedGamma changes and the texture cache is flushed so every
	// texture re-uploads through the new curve. Neutral (0.5) => identity LUT.
	FLOAT			AppliedGamma;		// Brightness the current GammaLUT was built for (-1 = none yet)
	BYTE			GammaLUT[256];
	void			UpdateGamma();		// rebuild LUT + flush cache when Brightness changes

	// Windows/GL state.
#if UNREAL_USE_SDL
	SDL_Window*		SdlWindow;
	SDL_GLContext	SdlContext;
#else
	HWND			hWindow;
	HDC				hDC;
	HGLRC			hRC;
#endif


	// Texture cache.
	enum {CACHE_BUCKETS=1024};
	FCachedTexture*	BindMap[CACHE_BUCKETS];
	QWORD			CurrentTextureID;
	FLOAT			UMult, VMult;
	UBOOL			TileDraw;	// inside DrawTile: translucent binds go mip-0-only (see SetTexture)

	// Frame state.
	FPlane			FlashScale;
	FPlane			FlashFog;
	FSceneNode*		CurrentFrame;
	DWORD			CurrentBlendFlags;

	// Zone distance fog, resolved from the viewer's zone in SetSceneNode and
	// applied as a dedicated per-surface blended pass (the engine never submits
	// distance-fog params, and fixed-function GL_FOG over-fogs a multi-pass
	// renderer, so we draw a fog-colored overlay faded by eye-space depth).
	UBOOL			FogActive;
	FLOAT			FogRGB[3];
	FLOAT			FogEnd;		// world units where fog reaches full opacity

	// Stats.
	INT				SurfCount, PolyCount, TileCount, UploadCount, DetailCount;

	// Editor hit testing (Phase 5). While a hit-test Draw is locked, pushed
	// proxy blobs accumulate on HitStack; any primitive that covers the
	// viewport's 5px hit region sets HitCovered, and the innermost enclosing
	// PopHit snapshots the whole stack as the winner (painter's order — later
	// covered proxies overwrite earlier ones). Unlock writes the winner back
	// to the caller's buffer. See UViewport::ExecuteHits for the consumer.
	BYTE*			HitDataPtr;			// caller's buffer (NULL = not hit testing)
	INT*			HitSizePtr;			// caller's in/out size
	INT				HitBufferCap;		// caller's buffer capacity (in *HitSizePtr at Lock)
	BYTE			HitStack[1024];		// concatenated pushed proxy records
	INT				HitStackSize;
	BYTE			HitBest[1024];		// winning stack snapshot
	INT				HitBestSize;
	FLOAT			HitCx, HitCy;		// center of the hit region, screen px
	UBOOL			HitCovered;			// a primitive covered the region since last push/pop

	// Screen-projection of an eye-space point (inverse of EyeX/EyeY).
	UBOOL HitScreen( FSceneNode* Frame, const FVector& Eye, FLOAT& SX, FLOAT& SY )
	{
		if( Eye.Z < 0.01f )
			return 0;
		SX = Eye.X * Frame->Proj.Z / Eye.Z + Frame->FX2;
		SY = Eye.Y * Frame->Proj.Z / Eye.Z + Frame->FY2;
		return 1;
	}
	// Point-in-polygon (screen space, crossing test).
	void HitTestPoly( FSceneNode* Frame, FTransform** Pts, INT NumPts )
	{
		if( !HitDataPtr || HitCovered || NumPts<3 || NumPts>64 )
			return;
		FLOAT X[64], Y[64];
		for( INT i=0; i<NumPts; i++ )
			if( !HitScreen( Frame, Pts[i]->Point, X[i], Y[i] ) )
				return;
		UBOOL Inside = 0;
		for( INT i=0, j=NumPts-1; i<NumPts; j=i++ )
			if( ((Y[i]>HitCy) != (Y[j]>HitCy)) && (HitCx < (X[j]-X[i])*(HitCy-Y[i])/(Y[j]-Y[i]) + X[i]) )
				Inside = !Inside;
		if( Inside )
			HitCovered = 1;
	}
	// Screen-space axis box.
	void HitTestBox( FLOAT X0, FLOAT Y0, FLOAT X1, FLOAT Y1 )
	{
		if( HitDataPtr && HitCx>=X0-2 && HitCx<=X1+2 && HitCy>=Y0-2 && HitCy<=Y1+2 )
			HitCovered = 1;
	}
	// Screen-space segment proximity (for wireframe picking).
	void HitTestLine( FLOAT X0, FLOAT Y0, FLOAT X1, FLOAT Y1 )
	{
		if( !HitDataPtr || HitCovered )
			return;
		FLOAT DX=X1-X0, DY=Y1-Y0;
		FLOAT L2=DX*DX+DY*DY;
		FLOAT T = L2>0.f ? ((HitCx-X0)*DX + (HitCy-Y0)*DY)/L2 : 0.f;
		T = Clamp( T, 0.f, 1.f );
		FLOAT PX=X0+T*DX-HitCx, PY=Y0+T*DY-HitCy;
		if( PX*PX+PY*PY <= 9.f )
			HitCovered = 1;
	}

	// Constructor.
	UOpenGLRenderDevice();
	static void InternalClassInitializer( UClass* Class );

	// URenderDevice interface.
	UBOOL Init( UViewport* InViewport );
	void Exit();
	void Flush();
	UBOOL Exec( const char* Cmd, FOutputDevice* Out );
	void Lock( FPlane InFlashScale, FPlane InFlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize );
	void Unlock( UBOOL Blit );
	void DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet );
	void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span );
	void DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags );
	void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 );
	void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 );
	void ClearZ( FSceneNode* Frame );
	void PushHit( const BYTE* Data, INT Count );
	void PopHit( INT Count, UBOOL bForce );
	void GetStats( char* Result );
	void ReadPixels( FColor* Pixels );
	void EndFlash();
	FLOAT GetPixelDepth( FSceneNode* Frame, INT X, INT Y );

	// Internals.
	void SetSceneNode( FSceneNode* Frame );
	void SetBlend( DWORD PolyFlags );
	void SetTexture( FTextureInfo& Info, DWORD PolyFlags, UBOOL Clamp );
	void UploadTexture( FTextureInfo& Info, DWORD PolyFlags, UBOOL SpriteTile );
	FLOAT EyeX( FSceneNode* Frame, FLOAT ScreenX, FLOAT Z ) { return (ScreenX - Frame->FX2) * Z / Frame->Proj.Z; }
	FLOAT EyeY( FSceneNode* Frame, FLOAT ScreenY, FLOAT Z ) { return (ScreenY - Frame->FY2) * Z / Frame->Proj.Z; }
};

IMPLEMENT_CLASS(UOpenGLRenderDevice);
IMPLEMENT_PACKAGE(OpenGLDrv);

// Phase 5 (ImGui editor): post-render overlay hook, see OpenGLDrvHooks.h.
#include "../Inc/OpenGLDrvHooks.h"
OPENGLDRV_API void (*GGLPostRenderHook)( UViewport* Viewport ) = NULL;

/*-----------------------------------------------------------------------------
	Construction and registration.
-----------------------------------------------------------------------------*/

UOpenGLRenderDevice::UOpenGLRenderDevice()
{
	guard(UOpenGLRenderDevice::UOpenGLRenderDevice);
	UseVSync         = 1;
#if UNREAL_USE_SDL
	SdlWindow        = NULL;
	SdlContext       = NULL;
#else
	hWindow          = NULL;
	hDC              = NULL;
	hRC              = NULL;
#endif
	CurrentTextureID = 0;
	CurrentFrame     = NULL;
	CurrentBlendFlags= (DWORD)-1;
	TileDraw         = 0;
	appMemset( BindMap, 0, sizeof(BindMap) );
	AppliedGamma     = -1.f;
	for( INT i=0; i<256; i++ ) GammaLUT[i] = (BYTE)i;	// identity until first UpdateGamma
	unguard;
}

// Rebuild the brightness gamma LUT and flush the texture cache when the video-menu
// Brightness slider changes. Called once per frame from Lock; early-outs (no work)
// while unchanged. 0.5 = neutral (identity); >0.5 brightens, <0.5 darkens.
void UOpenGLRenderDevice::UpdateGamma()
{
	guard(UOpenGLRenderDevice::UpdateGamma);
	FLOAT B = (Viewport && Viewport->Client) ? Viewport->Client->Brightness : 0.5f;
	if( B==AppliedGamma )
		return;
	AppliedGamma = B;
	FLOAT Gamma = Clamp( 0.4f + B*1.2f, 0.3f, 2.5f );	// 0.5 -> 1.0 (identity exponent)
	for( INT i=0; i<256; i++ )
	{
		FLOAT v = appPow( i/255.f, 1.f/Gamma );
		GammaLUT[i] = (BYTE)Clamp( (INT)(v*255.f + 0.5f), 0, 255 );
	}
	Flush();	// drop cached GL textures so they re-upload through the new curve
	unguard;
}


void UOpenGLRenderDevice::InternalClassInitializer( UClass* Class )
{
	guard(UOpenGLRenderDevice::InternalClassInitializer);
	if( appStricmp( Class->GetName(), "OpenGLRenderDevice" )==0 )
	{
		new(Class,"UseVSync", RF_Public)UBoolProperty( CPP_PROPERTY(UseVSync), "Options", CPF_Config );
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Init/Exit.
-----------------------------------------------------------------------------*/

UBOOL UOpenGLRenderDevice::Init( UViewport* InViewport )
{
	guard(UOpenGLRenderDevice::Init);

	// Device capabilities.
	Viewport            = InViewport;
	SpanBased           = 0;
	FrameBuffered       = 0;
	SupportsFogMaps     = 1;
	SupportsDistanceFog = 1;	// zone distance fog implemented (see SetSceneNode/DrawComplexSurface)
	VolumetricLighting  = 1;
	ShinySurfaces       = 1;
	Coronas             = 1;
	HighDetailActors    = 1;

#if UNREAL_USE_SDL
	// cross-platform port: SDL owns the window + GL context.
	SdlWindow = (SDL_Window*)InViewport->GetWindow();
	check(SdlWindow);
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 4 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 6 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY );
	SdlContext = SDL_GL_CreateContext( SdlWindow );
	if( !SdlContext )
	{
		// Fall back to whatever the driver offers if 4.6 compat is refused.
		SDL_GL_ResetAttributes();
		SdlContext = SDL_GL_CreateContext( SdlWindow );
	}
	if( !SdlContext )
	{
		debugf( NAME_Init, "OpenGL: SDL_GL_CreateContext failed: %s", SDL_GetError() );
		return 0;
	}
	SDL_GL_MakeCurrent( SdlWindow, SdlContext );

	debugf( NAME_Init, "OpenGL vendor    : %s", (const char*)glGetString(GL_VENDOR)   );
	debugf( NAME_Init, "OpenGL renderer  : %s", (const char*)glGetString(GL_RENDERER) );
	debugf( NAME_Init, "OpenGL version   : %s", (const char*)glGetString(GL_VERSION)  );

	SDL_GL_SetSwapInterval( UseVSync ? 1 : 0 );
#else
	// Get the viewport window.
	hWindow = (HWND)InViewport->GetWindow();
	check(hWindow);
	hDC = GetDC( hWindow );
	check(hDC);

	// Choose a pixel format.
	PIXELFORMATDESCRIPTOR pfd;
	appMemset( &pfd, 0, sizeof(pfd) );
	pfd.nSize        = sizeof(pfd);
	pfd.nVersion     = 1;
	pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType   = PFD_TYPE_RGBA;
	pfd.cColorBits   = 32;
	pfd.cDepthBits   = 24;
	pfd.cStencilBits = 8;
	pfd.iLayerType   = PFD_MAIN_PLANE;
	INT PixelFormat = ChoosePixelFormat( hDC, &pfd );
	if( !PixelFormat || !SetPixelFormat( hDC, PixelFormat, &pfd ) )
	{
		debugf( NAME_Init, "OpenGL: no suitable pixel format" );
		return 0;
	}

	// Create a legacy context first, then try to upgrade it to a modern
	// compatibility-profile context.
	HGLRC hBootstrap = wglCreateContext( hDC );
	if( !hBootstrap )
	{
		debugf( NAME_Init, "OpenGL: wglCreateContext failed" );
		return 0;
	}
	wglMakeCurrent( hDC, hBootstrap );

	PFNWGLCREATECONTEXTATTRIBSARB wglCreateContextAttribsARB
		= (PFNWGLCREATECONTEXTATTRIBSARB)wglGetProcAddress("wglCreateContextAttribsARB");
	if( wglCreateContextAttribsARB )
	{
		static const int Attribs[] =
		{
			WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
			WGL_CONTEXT_MINOR_VERSION_ARB, 6,
			WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
			0
		};
		HGLRC hModern = wglCreateContextAttribsARB( hDC, NULL, Attribs );
		if( hModern )
		{
			wglMakeCurrent( NULL, NULL );
			wglDeleteContext( hBootstrap );
			hRC = hModern;
			wglMakeCurrent( hDC, hRC );
		}
		else hRC = hBootstrap;
	}
	else hRC = hBootstrap;

	debugf( NAME_Init, "OpenGL vendor    : %s", (const char*)glGetString(GL_VENDOR)   );
	debugf( NAME_Init, "OpenGL renderer  : %s", (const char*)glGetString(GL_RENDERER) );
	debugf( NAME_Init, "OpenGL version   : %s", (const char*)glGetString(GL_VERSION)  );

	// VSync.
	PFNWGLSWAPINTERVALEXT wglSwapIntervalEXT
		= (PFNWGLSWAPINTERVALEXT)wglGetProcAddress("wglSwapIntervalEXT");
	if( wglSwapIntervalEXT )
		wglSwapIntervalEXT( UseVSync ? 1 : 0 );
#endif

	// Baseline GL state.
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glEnable( GL_TEXTURE_2D );
	glShadeModel( GL_SMOOTH );
	glAlphaFunc( GL_GREATER, 0.5f );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );

	return 1;
	unguard;
}

void UOpenGLRenderDevice::Exit()
{
	guard(UOpenGLRenderDevice::Exit);
	Flush();
#if UNREAL_USE_SDL
	if( SdlContext )
	{
		SDL_GL_DestroyContext( SdlContext ); // SDL3 (was SDL_GL_DeleteContext)
		SdlContext = NULL;
	}
	SdlWindow = NULL;
#else
	if( hRC )
	{
		wglMakeCurrent( NULL, NULL );
		wglDeleteContext( hRC );
		hRC = NULL;
	}
	if( hDC && hWindow )
	{
		ReleaseDC( hWindow, hDC );
		hDC = NULL;
	}
#endif
	debugf( NAME_Exit, "OpenGL renderer shut down" );
	unguard;
}

void UOpenGLRenderDevice::Flush()
{
	guard(UOpenGLRenderDevice::Flush);
	for( INT i=0; i<CACHE_BUCKETS; i++ )
	{
		for( FCachedTexture* T=BindMap[i]; T; )
		{
			FCachedTexture* Next = T->Next;
			glDeleteTextures( 1, &T->GLId );
			appFree( T );
			T = Next;
		}
		BindMap[i] = NULL;
	}
	CurrentTextureID = 0;
	unguard;
}

UBOOL UOpenGLRenderDevice::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UOpenGLRenderDevice::Exec);
	return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Lock/Unlock.
-----------------------------------------------------------------------------*/

// x64 port: -framestats accumulators (single render thread).
static UBOOL GLStats = 0;
static DOUBLE GLFrameStart = 0.0, GLBlockMs = 0.0, GLUploadMs = 0.0;
static INT GLUploadCount = 0;

void UOpenGLRenderDevice::Lock( FPlane InFlashScale, FPlane InFlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize )
{
	guard(UOpenGLRenderDevice::Lock);

	// x64 port: -framestats — the first GL calls of the frame absorb the
	// driver's present back-pressure (vsync wait); time them separately.
	static UBOOL Checked = 0;
	if( !Checked ) { Checked = 1; GLStats = ParseParam( appCmdLine(), "FRAMESTATS" ); }
	DOUBLE T0 = GLStats ? appSeconds() : 0.0;
#if UNREAL_USE_SDL
	check(SdlContext);
	SDL_GL_MakeCurrent( SdlWindow, SdlContext );
#else
	check(hRC);
	wglMakeCurrent( hDC, hRC );
#endif

	UpdateGamma();	// screen brightness: rebuild the texture gamma LUT if the slider moved

	FlashScale   = InFlashScale;
	FlashFog     = InFlashFog;
	CurrentFrame = NULL;
	CurrentBlendFlags = (DWORD)-1;
	SurfCount = PolyCount = TileCount = DetailCount = 0;

	// Editor hit testing: arm the proxy-stack recorder for this frame.
	HitDataPtr   = HitData;
	HitSizePtr   = HitSize;
	HitBufferCap = HitSize ? *HitSize : 0;
	HitStackSize = 0;
	HitBestSize  = 0;
	HitCovered   = 0;
	if( HitDataPtr && Viewport )
	{
		HitCx = Viewport->HitX + Viewport->HitXL*0.5f;
		HitCy = Viewport->HitY + Viewport->HitYL*0.5f;
	}

	glDepthMask( GL_TRUE );
	glClearDepth( 1.0 );
	// x64 port: ALWAYS clear the color buffer. The engine only sets
	// LOCKR_ClearScreen when it knows the 3D view won't fill the screen, but the
	// pull-down console (and menus) shrink the view via UConsole::PreRender WITHOUT
	// that flag -- so the uncovered top area kept the previous frame, and each
	// slide-animation frame's text accumulated through the translucent panel into
	// a ghost trail (the doubled text at the top of the console). A per-frame color
	// clear is cheap and the world simply redraws over it.
	FPlane ClearColor = (RenderLockFlags & LOCKR_ClearScreen) ? ScreenClear : FPlane(0,0,0,0);
	glClearColor( ClearColor.X, ClearColor.Y, ClearColor.Z, 1.0f );
	DWORD ClearBits = GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT;
	glViewport( 0, 0, Viewport->SizeX, Viewport->SizeY );
	glClear( ClearBits );
	if( GLStats )
	{
		DOUBLE T1 = appSeconds();
		GLBlockMs = (T1-T0)*1000.0;
		GLFrameStart = T1;
		GLUploadMs = 0.0;
		GLUploadCount = 0;
	}

	unguard;
}

void UOpenGLRenderDevice::Unlock( UBOOL Blit )
{
	guard(UOpenGLRenderDevice::Unlock);
	// Editor hit testing: hand the winning proxy stack back to the caller
	// (see UEditorEngine::Click / UViewport::ExecuteHits).
	if( HitSizePtr )
	{
		INT N = Min( HitBestSize, HitBufferCap );
		if( N > 0 && HitDataPtr )
			appMemcpy( HitDataPtr, HitBest, N );
		*HitSizePtr = N;
		HitDataPtr = NULL;
		HitSizePtr = NULL;
	}
	if( Blit )
	{
		// -glcounts diagnostic: per-viewport primitive counts, for telling
		// "nothing submitted" (engine cull, e.g. camera in solid) apart from
		// "submitted but invisible" (GL state) when a view renders black.
		if( ParseParam( appCmdLine(), "GLCOUNTS" ) )
		{
			static INT Every=0;
			if( ((Every++) % 120)==0 )
				debugf( "GLCounts: %s surfs=%i polys=%i tiles=%i detail=%i", Viewport ? Viewport->GetName() : "?", SurfCount, PolyCount, TileCount, DetailCount );
		}

		// Phase 5 (ImGui editor): draw the UI overlay on the finished frame,
		// with the GL context still current, before presenting.
		if( GGLPostRenderHook )
			(*GGLPostRenderHook)( Viewport );

		// x64 port: -framestats — time the present separately from the frame.
		static UBOOL FrameStats = ParseParam( appCmdLine(), "FRAMESTATS" );
		if( FrameStats )
		{
			DOUBLE T0 = appSeconds();
#if UNREAL_USE_SDL
			SDL_GL_SwapWindow( SdlWindow );
#else
			SwapBuffers( hDC );
#endif
			glFinish(); // x64 port: sync the frame boundary — see non-stats path below
			DOUBLE T1 = appSeconds();
			if( T1-T0 > 0.010 )
				debugf( "FrameStats: SwapBuffers took %.1fms", (T1-T0)*1000.0 );
			static DOUBLE SwapSum = 0.0; static INT SwapCount = 0;
			SwapSum += T1-T0; SwapCount++;
			if( SwapCount == 300 )
			{
				debugf( "FrameStats: swap avg=%.2fms over %i presents", SwapSum*1000.0/SwapCount, SwapCount );
				SwapSum = 0.0; SwapCount = 0;
			}
			DOUBLE DrawMs = (T1-GLFrameStart)*1000.0;
			if( GLBlockMs+DrawMs > 25.0 )
				debugf( "FrameStats: GL block=%.1fms draw=%.1fms uploads=%i (%.1fms)", GLBlockMs, DrawMs, GLUploadCount, GLUploadMs );
		}
		else
		{
#if UNREAL_USE_SDL
			SDL_GL_SwapWindow( SdlWindow );
#else
			SwapBuffers( hDC );
#endif
			// x64 port: with a modern driver's deep present queue, back-pressure
			// otherwise lands on an arbitrary mid-frame GL call and stalls for
			// whole vsync multiples (measured 33-66ms hitches ~2/sec). Draining
			// the queue at the frame boundary keeps pacing even; the CPU frame
			// is <1ms so the lost parallelism is irrelevant.
			glFinish();
		}
	}
	unguard;
}

void UOpenGLRenderDevice::ClearZ( FSceneNode* Frame )
{
	guard(UOpenGLRenderDevice::ClearZ);
	// x64 port: glDepthMask gates glClear of the depth buffer -- if the last
	// SetBlend was a non-occluding translucent/modulated draw the mask is off
	// and the clear would silently no-op (broken sky/mirror layering). Force
	// it on and resync the blend cache so the next SetBlend reapplies state.
	glDepthMask( GL_TRUE );
	CurrentBlendFlags = (DWORD)-1;
	glClear( GL_DEPTH_BUFFER_BIT );
	unguard;
}

void UOpenGLRenderDevice::EndFlash()
{
	guard(UOpenGLRenderDevice::EndFlash);
	if( FlashScale!=FPlane(0.5f,0.5f,0.5f,0.f) || FlashFog!=FPlane(0.f,0.f,0.f,0.f) )
	{
		// Blend a full-screen quad: out = FlashFog + dst * 2*FlashScale.
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_DEPTH_TEST );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_SRC_ALPHA );
		glMatrixMode( GL_PROJECTION );
		glPushMatrix();
		glLoadIdentity();
		glMatrixMode( GL_MODELVIEW );
		glPushMatrix();
		glLoadIdentity();
		glColor4f( FlashFog.X, FlashFog.Y, FlashFog.Z, Min( FlashScale.X*2.f, 1.f ) );
		glBegin( GL_QUADS );
		glVertex2f( -1, -1 ); glVertex2f( 1, -1 ); glVertex2f( 1, 1 ); glVertex2f( -1, 1 );
		glEnd();
		glPopMatrix();
		glMatrixMode( GL_PROJECTION );
		glPopMatrix();
		glMatrixMode( GL_MODELVIEW );
		glDisable( GL_BLEND );
		glEnable( GL_DEPTH_TEST );
		glEnable( GL_TEXTURE_2D );
		CurrentBlendFlags = (DWORD)-1;
		CurrentFrame = NULL;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Scene setup.
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::SetSceneNode( FSceneNode* Frame )
{
	guard(UOpenGLRenderDevice::SetSceneNode);
	if( Frame==CurrentFrame )
		return;
	CurrentFrame = Frame;

	// Sub-viewport within the window (GL origin is bottom-left).
	glViewport( Frame->XB, Viewport->SizeY - Frame->Y - Frame->YB, Frame->X, Frame->Y );

	// Rebuild the engine's projection: ScreenX = Point.X * Proj.Z / Point.Z + FX/2.
	// x64 port: zNear must be below 1.0 — the canvas draws tiles/fonts at
	// Z=1.0 exactly, and vertices on the near plane get clipped by floating-
	// point rounding (the whole 2D overlay vanished at zNear=1). The frustum
	// edges are specified at zNear, so the field of view is unaffected.
	const GLdouble zNear = 0.5, zFar = 49152.0;
	GLdouble RProjZ = 1.0 / Frame->Proj.Z;
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glFrustum( -Frame->FX2*RProjZ*zNear, +Frame->FX2*RProjZ*zNear,
	           -Frame->FY2*RProjZ*zNear, +Frame->FY2*RProjZ*zNear,
	           zNear, zFar );

	// Unreal eye space is X right, Y down, Z forward; GL eye space is
	// X right, Y up, Z backward.
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
	glScalef( 1.f, -1.f, -1.f );

	// Resolve the viewer's zone distance fog (bFogZone + FogColor + FogDistance).
	// Verts reach us in Unreal eye space, so Point.Z is the view depth in the
	// same world units as FogDistance — no remapping needed.
	FogActive = 0;
	AZoneInfo* Zone = ( Frame->Viewport && Frame->Viewport->Actor )
		? Frame->Viewport->Actor->Region.Zone : NULL;
	if( Zone && Zone->bFogZone && Zone->FogDistance > 0.f )
	{
		FogActive  = 1;
		FogRGB[0]  = Zone->FogColor.R/255.f;
		FogRGB[1]  = Zone->FogColor.G/255.f;
		FogRGB[2]  = Zone->FogColor.B/255.f;
		FogEnd     = Zone->FogDistance;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Blending and texture binding.
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::SetBlend( DWORD PolyFlags )
{
	guard(UOpenGLRenderDevice::SetBlend);
	DWORD Relevant = PolyFlags & (PF_Translucent|PF_Modulated|PF_Masked|PF_Invisible|PF_Occlude);
	if( Relevant==CurrentBlendFlags )
		return;
	CurrentBlendFlags = Relevant;

	if( PolyFlags & PF_Translucent )
	{
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_COLOR );
	}
	else if( PolyFlags & PF_Modulated )
	{
		glEnable( GL_BLEND );
		glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );
	}
	else if( PolyFlags & PF_Invisible )
	{
		glEnable( GL_BLEND );
		glBlendFunc( GL_ZERO, GL_ONE );
	}
	else if( PolyFlags & PF_Masked )
	{
		// x64 port: match retail GlideDrv SetBlending -- masked polys also
		// alpha-BLEND (GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA) in
		// addition to alpha-testing, so bilinear-filtered edge texels fade
		// out smoothly instead of hard-cutting at the 0.5 alpha reference
		// (jagged dark-fringed cutouts on grates/railings/foliage without it).
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	}
	else
	{
		glDisable( GL_BLEND );
	}

	if( PolyFlags & PF_Masked )
		glEnable( GL_ALPHA_TEST );
	else
		glDisable( GL_ALPHA_TEST );

	// x64 port: match retail GlideDrv SetBlending -- translucent/modulated
	// non-occluders must NOT write depth (grDepthMask(0) in UnGlide.cpp). Under
	// these blend modes black texels are invisible but were still stamping the
	// z-buffer across the whole primitive, so every world sprite/flame quad
	// (drawn at Z-SPRITE_PROJECTION_FORWARD, i.e. 32 units nearer than its
	// actor) acted as an invisible square occluder, depth-rejecting any mesh
	// drawn later and farther -- the distance-dependent square hole punched
	// through enemies standing in front of torch flames at 4K.
	if( (PolyFlags & (PF_Translucent|PF_Modulated)) && !(PolyFlags & PF_Occlude) )
		glDepthMask( GL_FALSE );
	else
		glDepthMask( GL_TRUE );
	unguard;
}

//
// Bind a texture, uploading it on first use.
//
void UOpenGLRenderDevice::SetTexture( FTextureInfo& Info, DWORD PolyFlags, UBOOL Clamp )
{
	guard(UOpenGLRenderDevice::SetTexture);

	// Masked textures get a distinct cache entry: palette index 0 differs.
	QWORD TestID = Info.CacheID;
	if( PolyFlags & PF_Masked )
		TestID |= (QWORD)4;
	// x64 port: filtering is baked at upload, so a texture drawn both with and
	// without PF_NoSmooth needs distinct cache entries (else it keeps whichever
	// filter its first draw requested).
	if( PolyFlags & PF_NoSmooth )
		TestID |= (QWORD)8;
	// Translucent uploads gate the near-black dither floor (see UploadTexture),
	// so they too need their own cache entry.
	if( (PolyFlags & PF_Translucent) && !(PolyFlags & PF_Modulated) )
		TestID |= (QWORD)16;
	// Translucent and modulated WORLD-SPRITE tiles (smoke, fireballs, flame
	// sprites -- they alone carry PF_TwoSided, added by DrawActorSprite; UI
	// tiles and coronas don't) get a dedicated upload variant, sampled from
	// mip 0 only and with a border fade (see UploadTexture). Small mips
	// concentrate the image's energy into a couple of texels -- at 2x2/4x4 ANY
	// non-neutral texel bilinear-spreads across the entire tile, so a minified
	// puff renders as a full-quad square: gray blocks for translucent trails,
	// and a square tint riding on modulated smoke (the grenade's BlackSmoke
	// trail). 1998 sprites were never mipmapped: the software renderer point-
	// sampled mip 0 at every distance, so do the same. Translucent world
	// SURFACES (water, lava) keep their mip chains -- TileDraw is only set
	// inside DrawTile. Clamp too: REPEAT wrap bilinear-bleeds the opposite
	// edge into the quad border.
	UBOOL SpriteTile = TileDraw && (PolyFlags & (PF_Translucent|PF_Modulated)) && (PolyFlags & PF_TwoSided);
	if( SpriteTile )
	{
		TestID |= (QWORD)32;
		Clamp = 1;
	}

	// Find in cache.
	FCachedTexture** Bucket = &BindMap[ (DWORD)(TestID>>12) & (CACHE_BUCKETS-1) ];
	FCachedTexture* T;
	for( T=*Bucket; T; T=T->Next )
		if( T->CacheID==TestID )
			break;

	UBOOL NeedUpload = 0;
	if( !T )
	{
		T = (FCachedTexture*)appMalloc( sizeof(FCachedTexture), "GLTex" );
		T->CacheID = TestID;
		glGenTextures( 1, &T->GLId );
		// x64 port: guard file-controlled 0 scale/size — a zero divisor gave
		// inf UMult and NaN glTexCoord2f (blank/garbage polys).
		FLOAT UDiv = Info.UScale * Info.USize, VDiv = Info.VScale * Info.VSize;
		T->UMult = UDiv!=0.f ? 1.f/UDiv : 0.f;
		T->VMult = VDiv!=0.f ? 1.f/VDiv : 0.f;
		T->Next  = *Bucket;
		*Bucket  = T;
		NeedUpload = 1;
	}
	else if( Info.TextureFlags & TF_RealtimeChanged )
	{
		NeedUpload = 1;
	}

	if( CurrentTextureID!=TestID || NeedUpload )
	{
		glBindTexture( GL_TEXTURE_2D, T->GLId );
		CurrentTextureID = TestID;
	}
	UMult = T->UMult;
	VMult = T->VMult;

	if( NeedUpload )
	{
		Info.TextureFlags &= ~TF_RealtimeChanged;
		DOUBLE U0 = GLStats ? appSeconds() : 0.0;
		UploadTexture( Info, PolyFlags, SpriteTile );
		if( GLStats )
		{
			GLUploadMs += (appSeconds()-U0)*1000.0;
			GLUploadCount++;
		}

		// Filtering and wrapping.
		UBOOL Smooth = !(PolyFlags & PF_NoSmooth);
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Smooth ? GL_LINEAR : GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			(Info.NumMips>1 && !SpriteTile) ? (Smooth ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
			                                : (Smooth ? GL_LINEAR : GL_NEAREST) );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, Clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, Clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT );
		// x64 port: retail GlideDrv sharpened mip selection via grTexLodBiasValue
		// with shipped DetailBias=-1.5 (UnGlide.cpp TryRes + Default.ini). -0.5
		// gives comparable bite under trilinear without 4K mip shimmer.
		glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -0.5f );
	}
	unguard;
}

//
// Convert and upload all mips of a texture.
//
void UOpenGLRenderDevice::UploadTexture( FTextureInfo& Info, DWORD PolyFlags, UBOOL SpriteTile )
{
	guard(UOpenGLRenderDevice::UploadTexture);
	FMemMark Mark(GMem);
	UploadCount++;

	for( INT iMip=0; iMip<(SpriteTile ? Min(1,Info.NumMips) : Info.NumMips); iMip++ )
	{
		FMipmap* Mip = Info.Mips[iMip];
		if( !Mip )
			break;
		BYTE* Src = Mip->DataPtr ? Mip->DataPtr : ( Mip->DataArray.Num() ? &Mip->DataArray(0) : NULL );
		if( !Src )
			break;
		// x64 port / security: mip dimensions are file-controlled. Reject absurd
		// sizes so USize*VSize can't overflow the destination allocation/loop
		// counts (with dims <= MAX_TEXTURE_SIZE, USize*VSize <= 2^20 and the
		// FColor byte count <= 2^22 — well within INT).
		INT USize = Mip->USize, VSize = Mip->VSize;
		if( USize<=0 || VSize<=0 || USize>MAX_TEXTURE_SIZE || VSize>MAX_TEXTURE_SIZE )
			break;
		INT Count = USize*VSize;
		// Bytes actually available at Src. For loaded textures this is the
		// serialized DataArray length, which the format does NOT guarantee
		// matches USize*VSize — so every source read below is clamped to it to
		// prevent a heap over-read (crash / adjacent-heap info leak into the
		// uploaded texture). DataPtr buffers are engine-generated and correctly
		// sized for the format.
		INT BytesPerTexel = (Info.Format==TEXF_RGB32) ? (INT)sizeof(FColor) : 1;
		INT SrcBytes = Mip->DataPtr ? Count*BytesPerTexel : Mip->DataArray.Num();
		FColor* Dst = NewZeroed<FColor>( GMem, Count );

		if( Info.Format==TEXF_P8 && Info.Palette )
		{
			// Palettized: expand through the palette. FColor is R,G,B,A in
			// memory, which matches GL_RGBA byte order directly. x64 port: a
			// null palette (malformed content) now falls through to the gray
			// branch below instead of asserting (check(Info.Palette) crashed).
			FColor LocalPal[256];
			appMemcpy( LocalPal, Info.Palette, 256*sizeof(FColor) );
			for( INT i=0; i<256; i++ )
				LocalPal[i].A = 255;
			if( PolyFlags & PF_Masked )
				LocalPal[0] = FColor(0,0,0,0);
			// x64 port: gate the near-black dither floor out of translucent
			// textures. Effect sprites (SmokeGray, explosion frames) carry a
			// scattered 1..22-value speckle field around the actual puff. Under
			// ONE / ONE_MINUS_SRC_COLOR every texel adds, and BILINEAR filtering
			// smears the speckles into a smooth gray field that fills the whole
			// quad -- dozens of stacked trail puffs then render as bright
			// straight-edged rectangles (the reported "square overlays"). 1998
			// never showed this floor: the software renderer POINT-sampled it
			// into isolated sub-visible dots (DrawTile.cpp adds per-texel with
			// no filtering), and Glide's dithered 16bpp quantized the +1..3
			// lifts away entirely. Quadratic gate below L=16: scale=(L/16)^2,
			// continuous at 16 so smooth dark gradients don't band; texels >=16
			// (the visible smoke itself, incl. the faint dissipation frames)
			// are untouched.
			if( (PolyFlags & PF_Translucent) && !(PolyFlags & PF_Modulated) )
				for( INT i=0; i<256; i++ )
				{
					INT L = Max( (INT)LocalPal[i].R, Max( (INT)LocalPal[i].G, (INT)LocalPal[i].B ) );
					if( L>0 && L<16 )
					{
						INT Num = L*L;	// /256 == (L/16)^2
						LocalPal[i].R = (BYTE)( (LocalPal[i].R*Num) >> 8 );
						LocalPal[i].G = (BYTE)( (LocalPal[i].G*Num) >> 8 );
						LocalPal[i].B = (BYTE)( (LocalPal[i].B*Num) >> 8 );
					}
				}
			INT N = Min( Count, SrcBytes );
			for( INT i=0; i<N; i++ )
				Dst[i] = LocalPal[Src[i]];
		}
		else if( Info.Format==TEXF_RGB32 )
		{
			// Raw 32-bit color (light maps, fog maps).
			INT N = Min( Count, SrcBytes/(INT)sizeof(FColor) );
			appMemcpy( Dst, Src, N*sizeof(FColor) );
			for( INT i=0; i<N; i++ )
				Dst[i].A = 255;
		}
		else
		{
			// Unsupported format: upload opaque gray so it is visible.
			for( INT i=0; i<Count; i++ )
				Dst[i] = FColor(127,127,127,255);
		}

		// x64 port: bake screen brightness into the texel colors via the gamma LUT
		// (identity when the slider is at neutral 0.5, so this is a no-op then).
		// NOT for modulated sprite tiles: their texels are blend FACTORS
		// (2*src*dst), not colors -- the background they multiply is already
		// gamma-baked, so lifting the texels too double-applies brightness.
		// Worse, the lift moves the texture's neutral-gray surround (127) off
		// the blend's fixed neutral point, so at any brightness above 0.5 the
		// whole quad brightened what's behind it -- a straight-edged light
		// square riding on the grenade's BlackSmoke trail.
		if( (AppliedGamma<0.49f || AppliedGamma>0.51f)
		&&	!( SpriteTile && (PolyFlags & PF_Modulated) ) )
			for( INT i=0; i<Count; i++ )
			{
				Dst[i].R = GammaLUT[Dst[i].R];
				Dst[i].G = GammaLUT[Dst[i].G];
				Dst[i].B = GammaLUT[Dst[i].B];
			}

		// x64 port: world-sprite tiles fade their outermost texels to the blend
		// mode's NEUTRAL value. A quad's edge is only invisible where the
		// texture reaches the border at exactly that value -- black under
		// additive translucency, mid-gray (127: 2*src*dst == dst) under
		// modulation -- and effect sprites don't guarantee it (the late
		// SmokeGray dissipation frames are a faint speckle field spread to the
		// tile border), so any off-neutral border content bilinear-smears into
		// a film that stops dead at the quad edge -- a visible straight line,
		// so the aging top of a smoke trail "squares off". The 1998 software
		// renderer got away with border speckles because point sampling drew
		// them as isolated sub-visible dots. Ramp the outer ~1/8th (max 8
		// texels) linearly to neutral per axis: content in the tile's core is
		// untouched, and no quad edge can ever carry a luminance step. RGB
		// only -- alpha is left alone so masked alpha-testing is unaffected.
		if( SpriteTile )
		{
			FLOAT Neutral = (PolyFlags & PF_Modulated) ? 127.5f : 0.f;
			// Modulated tiles: the smoke art's "empty" surround is not exactly
			// neutral (bs2_* sits at 129..131 vs 2*src*dst neutral 127.5), so
			// the whole quad faintly brightened everything behind it -- on a
			// bright wall a visible straight-edged tint. Same idea as the
			// translucent dither-floor gate: pull texels within +-12 of neutral
			// quadratically onto it (continuous at the band edge), which
			// flattens the surround to a true no-op while leaving the actual
			// smoke values (70..110) untouched.
			if( PolyFlags & PF_Modulated )
				for( INT i=0; i<Count; i++ )
				{
					FColor& C = Dst[i];
					BYTE* Ch[3] = { &C.R, &C.G, &C.B };
					for( INT c=0; c<3; c++ )
					{
						FLOAT D = (FLOAT)*Ch[c] - 127.5f;
						FLOAT A = Abs(D);
						if( A < 12.f )
							*Ch[c] = (BYTE)( 127.5f + D*(A/12.f)*(A/12.f) );
					}
				}
			INT Fx = Clamp( USize/8, 1, 8 ), Fy = Clamp( VSize/8, 1, 8 );
			for( INT y=0; y<VSize; y++ )
			{
				FLOAT Wy = Min( 1.f, (FLOAT)Min(y+1,VSize-y)/Fy );
				for( INT x=0; x<USize; x++ )
				{
					FLOAT W = Wy * Min( 1.f, (FLOAT)Min(x+1,USize-x)/Fx );
					if( W < 1.f )
					{
						FColor& C = Dst[y*USize+x];
						C.R = (BYTE)(C.R*W + Neutral*(1.f-W));
						C.G = (BYTE)(C.G*W + Neutral*(1.f-W));
						C.B = (BYTE)(C.B*W + Neutral*(1.f-W));
					}
				}
			}
		}

		glTexImage2D( GL_TEXTURE_2D, iMip, GL_RGBA8, USize, VSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, Dst );
	}
	Mark.Pop();
	unguard;
}

/*-----------------------------------------------------------------------------
	Complex surfaces (BSP).
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	guard(UOpenGLRenderDevice::DrawComplexSurface);
	SetSceneNode( Frame );
	SurfCount++;

	// Editor hit testing.
	if( HitDataPtr )
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
			HitTestPoly( Frame, Poly->Pts, Poly->NumPts );

	// Mutually exclusive effects, as in the original drivers.
	if( Surface.DetailTexture && Surface.FogMap )
		Surface.DetailTexture = NULL;

	UBOOL Masked = (Surface.PolyFlags & PF_Masked)!=0;

	// Pass 1: base texture (or flat color).
	SetBlend( Surface.PolyFlags );
	if( Surface.Texture )
	{
		SetTexture( *Surface.Texture, Surface.PolyFlags, 0 );
		glColor3f( 1.f, 1.f, 1.f );
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<Poly->NumPts; i++ )
			{
				FVector& P = Poly->Pts[i]->Point;
				FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
				FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
				glTexCoord2f( (U-Surface.Texture->Pan.X)*UMult, (V-Surface.Texture->Pan.Y)*VMult );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
			PolyCount++;
		}
	}

	// Subsequent passes hit exactly the pixels the base pass resolved.
	if( Masked )
		glDepthFunc( GL_EQUAL );

	// Pass 1.5a: bump map, 2x-modulated over the base texture at the base
	// texture's UVs (ports the GlideDrv bump pass; needs a base to modulate).
	if( Surface.BumpMap && Surface.Texture )
	{
		glDisable( GL_ALPHA_TEST );
		glEnable( GL_BLEND );
		glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );
		SetTexture( *Surface.BumpMap, 0, 0 );	// tiles like the base texture
		CurrentBlendFlags = (DWORD)-1;
		glColor3f( 1.f, 1.f, 1.f );
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<Poly->NumPts; i++ )
			{
				FVector& P = Poly->Pts[i]->Point;
				FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
				FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
				glTexCoord2f( (U-Surface.Texture->Pan.X)*UMult, (V-Surface.Texture->Pan.Y)*VMult );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
		}
	}

	// Pass 1.5b: macro texture, 2x-modulated at its own (coarser) UVs.
	if( Surface.MacroTexture )
	{
		glDisable( GL_ALPHA_TEST );
		glEnable( GL_BLEND );
		glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );
		SetTexture( *Surface.MacroTexture, 0, 0 );	// tiles at the macro scale
		CurrentBlendFlags = (DWORD)-1;
		glColor3f( 1.f, 1.f, 1.f );
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<Poly->NumPts; i++ )
			{
				FVector& P = Poly->Pts[i]->Point;
				FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
				FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
				glTexCoord2f( (U-Surface.MacroTexture->Pan.X)*UMult, (V-Surface.MacroTexture->Pan.Y)*VMult );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
		}
	}

	// Pass 2: light map, 2x modulated (Unreal light maps are half-bright).
	if( Surface.LightMap )
	{
		glDisable( GL_ALPHA_TEST );
		glEnable( GL_BLEND );
		glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );
		SetTexture( *Surface.LightMap, 0, 1 );
		CurrentBlendFlags = (DWORD)-1;
		glColor3f( 1.f, 1.f, 1.f );
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<Poly->NumPts; i++ )
			{
				FVector& P = Poly->Pts[i]->Point;
				FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
				FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
				glTexCoord2f( (U-Surface.LightMap->Pan.X+0.5f*Surface.LightMap->UScale)*UMult,
				              (V-Surface.LightMap->Pan.Y+0.5f*Surface.LightMap->VScale)*VMult );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
		}
	}

	// Pass 2.5: detail texture. A high-frequency overlay that fades in near the
	// camera and vanishes with distance, giving close-up surfaces crisp detail
	// instead of a blurry magnified base texture. Ports the GlideDrv near-Z
	// detail blend (UnGlide.cpp): the verts are already in Unreal eye space, so
	// Point.Z is the view-forward depth. The texture-combine interpolates the
	// detail texel toward neutral gray (0.5) by a per-vertex distance alpha, and
	// the whole pass is 2x-modulated — so far surfaces multiply by ~1.0 (no-op)
	// while near surfaces get the detail. (DetailTexture was nulled above when a
	// fog map is present, so the two are mutually exclusive as in the original.)
	if( Surface.DetailTexture )
	{
		DetailCount++;
		const FLOAT NearZ = 200.f;
		glDisable( GL_ALPHA_TEST );
		glEnable( GL_BLEND );
		glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );	// 2x modulate
		SetTexture( *Surface.DetailTexture, 0, 0 );	// Clamp=0: detail texture tiles
		CurrentBlendFlags = (DWORD)-1;
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE );			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT );		glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PRIMARY_COLOR );	glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA );
		FLOAT Gray[4] = { 0.5f, 0.5f, 0.5f, 1.f };
		glTexEnvfv( GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, Gray );
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<Poly->NumPts; i++ )
			{
				FVector& P = Poly->Pts[i]->Point;
				FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
				FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
				FLOAT A = P.Z>0.f ? Clamp( 100.f*(NearZ/P.Z - 1.f)/255.f, 0.f, 1.f ) : 0.f;
				glColor4f( 1.f, 1.f, 1.f, A );
				glTexCoord2f( (U-Surface.DetailTexture->Pan.X)*UMult, (V-Surface.DetailTexture->Pan.Y)*VMult );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
		}
		// Restore the default modulate env mode for later passes/primitives.
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		glColor3f( 1.f, 1.f, 1.f );
	}

	// Pass 3: fog map, additive over the lit surface.
	if( Surface.FogMap )
	{
		glDisable( GL_ALPHA_TEST );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_COLOR );
		SetTexture( *Surface.FogMap, 0, 1 );
		CurrentBlendFlags = (DWORD)-1;
		glColor3f( 1.f, 1.f, 1.f );
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<Poly->NumPts; i++ )
			{
				FVector& P = Poly->Pts[i]->Point;
				FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
				FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
				glTexCoord2f( (U-Surface.FogMap->Pan.X+0.5f*Surface.FogMap->UScale)*UMult,
				              (V-Surface.FogMap->Pan.Y+0.5f*Surface.FogMap->VScale)*VMult );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
		}
	}

	// Pass 4: zone distance fog. A fog-colored, untextured overlay whose alpha
	// rises linearly with eye-space depth (Point.Z), blended over the finished
	// surface. Done as its own pass rather than fixed-function GL_FOG so it
	// applies once to the final color instead of stacking on every modulate pass.
	if( FogActive )
	{
		glDisable( GL_ALPHA_TEST );
		glDisable( GL_TEXTURE_2D );
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		CurrentBlendFlags = (DWORD)-1;
		const FLOAT RFogEnd = FogEnd>0.f ? 1.f/FogEnd : 0.f;
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<Poly->NumPts; i++ )
			{
				FVector& P = Poly->Pts[i]->Point;
				FLOAT f = Clamp( P.Z*RFogEnd, 0.f, 1.f );
				glColor4f( FogRGB[0], FogRGB[1], FogRGB[2], f );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
		}
		glEnable( GL_TEXTURE_2D );
	}

	if( Masked )
		glDepthFunc( GL_LEQUAL );

	unguard;
}

/*-----------------------------------------------------------------------------
	Gouraud polygons (meshes/actors).
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span )
{
	guard(UOpenGLRenderDevice::DrawGouraudPolygon);
	SetSceneNode( Frame );
	PolyCount++;

	// Editor hit testing (FTransTexture derives from FTransform).
	if( HitDataPtr )
		HitTestPoly( Frame, (FTransform**)Pts, NumPts );

	SetBlend( PolyFlags );
	SetTexture( Info, PolyFlags, 0 );

	glBegin( GL_TRIANGLE_FAN );
	for( INT i=0; i<NumPts; i++ )
	{
		FTransTexture* P = Pts[i];
		if( PolyFlags & PF_Modulated )
			glColor3f( 1.f, 1.f, 1.f );
		else
			glColor3f( P->Light.R, P->Light.G, P->Light.B );
		glTexCoord2f( P->U*UMult, P->V*VMult );
		glVertex3f( P->Point.X, P->Point.Y, P->Point.Z );
	}
	glEnd();

	// Additive fog pass for vertex-fogged actors.
	if( (PolyFlags & (PF_RenderFog|PF_Translucent|PF_Modulated))==PF_RenderFog )
	{
		glDisable( GL_TEXTURE_2D );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_COLOR );
		glBegin( GL_TRIANGLE_FAN );
		for( INT i=0; i<NumPts; i++ )
		{
			FTransTexture* P = Pts[i];
			glColor3f( P->Fog.R, P->Fog.G, P->Fog.B );
			glVertex3f( P->Point.X, P->Point.Y, P->Point.Z );
		}
		glEnd();
		glEnable( GL_TEXTURE_2D );
		CurrentBlendFlags = (DWORD)-1;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Tiles (HUD, fonts, canvas).
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags )
{
	guard(UOpenGLRenderDevice::DrawTile);
	SetSceneNode( Frame );
	TileCount++;

	// Editor hit testing (screen-space rect: texture browser, icons, HUD).
	if( HitDataPtr )
		HitTestBox( X, Y, X+XL, Y+YL );

	SetBlend( PolyFlags );
	TileDraw = 1;
	SetTexture( Info, PolyFlags, 0 );
	TileDraw = 0;

	if( PolyFlags & PF_Modulated )
		glColor3f( 1.f, 1.f, 1.f );
	else
		glColor3f( Color.R, Color.G, Color.B );

	// x64 port: a world sprite is a screen-space quad at ONE depth, so a
	// per-pixel depth test cuts it along a dead-straight isodepth line wherever
	// it meets a surface. A rocket fireball is a billboard CENTRED on the impact
	// point, so half of it is geometrically inside the ground it went off
	// against and the z-test slices the bottom off with a razor edge -- the
	// reported "square" explosions and smoke. No forward depth offset can fix
	// that: the floor runs continuously from the sprite all the way back to the
	// eye, so moving the sprite's plane nearer only slides the cut line, it
	// never removes it.
	//
	// The 1998 software renderer never showed this because sprites were occluded
	// per-SPAN against the BSP (DrawTile clipped to Sprite->SpanBuffer), not per
	// pixel -- a fireball resting on the floor drew whole. Hardware span buffers
	// do not exist here (SpanBased=0, so Span arrives NULL), so match that look
	// directly: non-occluding translucent/modulated tiles skip the depth TEST.
	// They still never write depth, and the engine still draws sprites in its
	// own back-to-front order, so sprite-vs-sprite and sprite-vs-weapon layering
	// is unchanged. The cost is that an effect can bleed over world geometry
	// nearer than it -- a flash going off just around a corner. BSP leaf
	// visibility culls the fully-hidden cases before they ever reach the driver.
	UBOOL NoDepthTest = (PolyFlags & (PF_Translucent|PF_Modulated)) && !(PolyFlags & PF_Occlude);
	if( NoDepthTest )
		glDisable( GL_DEPTH_TEST );

	FLOAT u0=U*UMult, v0=V*VMult, u1=(U+UL)*UMult, v1=(V+VL)*VMult;
	glBegin( GL_QUADS );
	glTexCoord2f( u0, v0 ); glVertex3f( EyeX(Frame,X,   Z), EyeY(Frame,Y,   Z), Z );
	glTexCoord2f( u1, v0 ); glVertex3f( EyeX(Frame,X+XL,Z), EyeY(Frame,Y,   Z), Z );
	glTexCoord2f( u1, v1 ); glVertex3f( EyeX(Frame,X+XL,Z), EyeY(Frame,Y+YL,Z), Z );
	glTexCoord2f( u0, v1 ); glVertex3f( EyeX(Frame,X,   Z), EyeY(Frame,Y+YL,Z), Z );
	glEnd();

	if( NoDepthTest )
		glEnable( GL_DEPTH_TEST );
	unguard;
}

/*-----------------------------------------------------------------------------
	Lines and points (editor/debug).
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 )
{
	guard(UOpenGLRenderDevice::Draw2DLine);
	SetSceneNode( Frame );

	// Editor hit testing (wireframe picking in ortho views).
	if( HitDataPtr )
		HitTestLine( P1.X, P1.Y, P2.X, P2.Y );

	SetBlend( 0 );
	glDisable( GL_TEXTURE_2D );
	glColor3f( Color.R, Color.G, Color.B );
	glBegin( GL_LINES );
	glVertex3f( EyeX(Frame,P1.X,P1.Z), EyeY(Frame,P1.Y,P1.Z), P1.Z );
	glVertex3f( EyeX(Frame,P2.X,P2.Z), EyeY(Frame,P2.Y,P2.Z), P2.Z );
	glEnd();
	glEnable( GL_TEXTURE_2D );
	unguard;
}

void UOpenGLRenderDevice::Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 )
{
	guard(UOpenGLRenderDevice::Draw2DPoint);
	SetSceneNode( Frame );

	// Editor hit testing.
	if( HitDataPtr )
		HitTestBox( Min(X1,X2), Min(Y1,Y2), Max(X1,X2), Max(Y1,Y2) );

	SetBlend( 0 );
	glDisable( GL_TEXTURE_2D );
	glColor3f( Color.R, Color.G, Color.B );
	const FLOAT Z = 1.f;
	glBegin( GL_QUADS );
	glVertex3f( EyeX(Frame,X1,Z), EyeY(Frame,Y1,Z), Z );
	glVertex3f( EyeX(Frame,X2,Z), EyeY(Frame,Y1,Z), Z );
	glVertex3f( EyeX(Frame,X2,Z), EyeY(Frame,Y2,Z), Z );
	glVertex3f( EyeX(Frame,X1,Z), EyeY(Frame,Y2,Z), Z );
	glEnd();
	glEnable( GL_TEXTURE_2D );
	unguard;
}

/*-----------------------------------------------------------------------------
	Hit testing (editor selection) - not supported in-game.
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::PushHit( const BYTE* Data, INT Count )
{
	guard(UOpenGLRenderDevice::PushHit);
	if( !HitDataPtr || Count<=0 || HitStackSize+Count > (INT)sizeof(HitStack) )
		return;
	appMemcpy( HitStack+HitStackSize, Data, Count );
	HitStackSize += Count;
	HitCovered = 0;
	unguard;
}

void UOpenGLRenderDevice::PopHit( INT Count, UBOOL bForce )
{
	guard(UOpenGLRenderDevice::PopHit);
	if( !HitDataPtr )
		return;
	// Snapshot the full current stack (outermost..this proxy) as the winner
	// if anything drawn inside this proxy's scope covered the hit region.
	if( HitCovered || bForce )
	{
		HitBestSize = HitStackSize;
		appMemcpy( HitBest, HitStack, HitStackSize );
	}
	HitCovered = 0;
	HitStackSize = Max( 0, HitStackSize-Count );
	unguard;
}

/*-----------------------------------------------------------------------------
	Stats and readback.
-----------------------------------------------------------------------------*/

void UOpenGLRenderDevice::GetStats( char* Result )
{
	guard(UOpenGLRenderDevice::GetStats);
	appSprintf( Result, "OpenGL: surfs=%i polys=%i tiles=%i uploads=%i", SurfCount, PolyCount, TileCount, UploadCount );
	unguard;
}

void UOpenGLRenderDevice::ReadPixels( FColor* Pixels )
{
	guard(UOpenGLRenderDevice::ReadPixels);
	INT X = Viewport->SizeX, Y = Viewport->SizeY;
	FMemMark Mark(GMem);
	FColor* Buf = New<FColor>( GMem, X*Y );
	glReadPixels( 0, 0, X, Y, GL_RGBA, GL_UNSIGNED_BYTE, Buf );
	// GL rows run bottom-up; the engine expects top-down.
	for( INT i=0; i<Y; i++ )
		appMemcpy( Pixels + i*X, Buf + (Y-1-i)*X, X*sizeof(FColor) );
	Mark.Pop();
	unguard;
}

//
// x64 port: eye-space Z of the pixel rendered so far at frame-local (X,Y).
// Reads the depth buffer, so it sees exactly what was drawn -- including
// alpha-tested masked surfaces and decoration meshes that collision traces
// pass through. Translucent/modulated surfaces don't write depth (SetBlend),
// so light through water/glass keeps its flare. Used by the corona/lens-flare
// occlusion in Render.
//
FLOAT UOpenGLRenderDevice::GetPixelDepth( FSceneNode* Frame, INT X, INT Y )
{
	guard(UOpenGLRenderDevice::GetPixelDepth);
	if( X<0 || Y<0 || X>=Frame->X || Y>=Frame->Y )
		return 0.f;
	GLfloat D = 1.f;
	GLint WinX = Frame->XB + X;
	GLint WinY = Viewport->SizeY - Frame->YB - Y - 1;
	glReadPixels( WinX, WinY, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &D );
	// Invert the SetSceneNode glFrustum mapping (keep zNear/zFar in sync
	// with it): window depth D in [0,1] -> eye Z = n*f / (f - D*(f-n)).
	const GLdouble zNear = 0.5, zFar = 49152.0;
	if( D >= 1.f )
		return (FLOAT)zFar;	// nothing rendered there: unoccluded
	return (FLOAT)( (zNear*zFar) / (zFar - (GLdouble)D*(zFar-zNear)) );
	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

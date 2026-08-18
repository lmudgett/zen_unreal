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
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
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
	// Anisotropic filtering. A wall or floor running away from the camera is
	// sampled over a pixel footprint that is long in one texture axis and
	// narrow in the other, and plain trilinear must pick ONE mip for both --
	// so it picks for the long axis and throws away the detail the short axis
	// still had. That is why a corridor floor dissolves into mush a few metres
	// out while the same texture is crisp underfoot. Config; 0 or 1 disables.
	INT				MaxAnisotropy;
	FLOAT			AnisotropyLimit;	// what the hardware actually allows (0 = unsupported)
	// Mip selection bias for every texture. This is what retail Glide's
	// DetailBias actually was -- despite the name it was a global
	// grTexLodBiasValue applied to all TMUs at init (UnGlide.cpp), shipped at
	// -1.5. Negative sharpens by biasing toward the higher-resolution mip, at
	// the cost of shimmer on minified surfaces.
	FLOAT			LodBias;
	// Detail textures: the high-frequency overlay that gives a surface bite
	// close up, where the 256x256 base texture has run out of texels.
	// DetailRange is Glide's NearZ, a hardcoded 200 units there. DetailBias is
	// ours, not retail's: an extra bias for the overlay alone, since it is
	// high-frequency by design and is what the eye reads as sharpness.
	UBOOL			DetailTextures;
	FLOAT			DetailRange;
	FLOAT			DetailBias;

	// (The world DISTANCE FOG that used to live here was removed at the user's
	// call, 2026-08-18 -- retail zone fog, the mapper-flagged bFogZone kind, is
	// still supported below. See the fog pass in DrawComplexSurface.)

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

	// Wavy-surface warp shader (see InitWavyProgram).
	GLuint			WavyProgram;
	GLint			WavyLocTime, WavyLocAmp, WavyLocUVMult;
	GLint			WavyLocGloss, WavyLocBase, WavyLocLightOn, WavyLocUEdge, WavyLocVEdge;
	GLint			WavyLocFlowV, WavyLocScroll, WavyLocBlob, WavyLocFoam;
	GLint			WavyLocFall, WavyLocFallDn, WavyLocFallFd, WavyLocFallSol;
	UBOOL			WavyTried;

	// Sky box (see SetSceneNode). The sky is DRAWN exactly as stock; the flag
	// exists for the one sky treatment that survived -- the CLOUD CHURN in
	// DrawComplexSurface -- and for ClearZ ordering.
	// (Several attempts at treating the sky box's own seams -- aerial
	// perspective, a haze band, an atmospheric horizon, a screen-space blur --
	// were backed out: every one traded the seams for a worse artifact, because
	// the discontinuity is in the ARTWORK. Three unrelated paintings meet along
	// the box's edges, and no post-process can invent the transition between
	// them. The fix other engines use is to stop rasterising a box at all -- a
	// dome, or a cube map sampled by view direction, which has no edge to show.)
	UBOOL			SkyFrame;			// the scene node being drawn is a sky box

	// Fountain columns (see DrawComplexSurface). A pour is authored as several
	// translucent sheets -- crossed, or the four walls of a box -- and the
	// particles must fill the whole VOLUME those faces bound, not sit in the
	// plane of whichever face happened to draw first. So faces accumulate a
	// world-space box per column (world, not eye: it must survive camera
	// motion across frames), and the pour draws once per column per frame.
	enum {MAX_STREAM_COLS=64};
	struct FStreamColumn
	{
		FVector		WMin, WMax;		// world bounds of all faces seen so far
		INT			Stamp;			// frame stamp of the last draw
		void*		StampFrame;		// scene node of that draw (mirrors redraw)
	};
	FStreamColumn	StreamCols[MAX_STREAM_COLS];
	INT				NumStreamCols;
	INT				FrameStamp;

	// Underwater full-screen distortion (see InitUnderwaterProgram / EndFlash).
	GLuint			UnderwaterProgram;
	GLint			UWLocTime, UWLocAmp, UWLocPx;
	UBOOL			UnderwaterTried;
	GLuint			UnderwaterTex;			// framebuffer copy the warp pass resamples
	INT				UnderwaterTexW, UnderwaterTexH;
	UBOOL			ViewerUnderwater;		// view origin is in a water zone (set per master frame)

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
	// PopHit snapshots the whole stack as the winner (painter's order ??? later
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
	void InitWavyProgram();
	void DrawFallFoot( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet,
		DOUBLE T, FLOAT BaseUM, FLOAT BaseVM, UBOOL FoldLight, FLOAT LightUM, FLOAT LightVM );
	void InitUnderwaterProgram();
	void QueryAnisotropy();
	FLOAT EyeX( FSceneNode* Frame, FLOAT ScreenX, FLOAT Z ) { return (ScreenX - Frame->FX2) * Z / Frame->Proj.Z; }
	FLOAT EyeY( FSceneNode* Frame, FLOAT ScreenY, FLOAT Z ) { return (ScreenY - Frame->FY2) * Z / Frame->Proj.Z; }
};

IMPLEMENT_CLASS(UOpenGLRenderDevice);
IMPLEMENT_PACKAGE(OpenGLDrv);

// Phase 5 (ImGui editor): post-render overlay hook, see OpenGLDrvHooks.h.
#include "../Inc/OpenGLDrvHooks.h"
OPENGLDRV_API void (*GGLPostRenderHook)( UViewport* Viewport ) = NULL;

/*-----------------------------------------------------------------------------
	Fragment-program plumbing (wavy surface warp, underwater view warp).
-----------------------------------------------------------------------------*/

typedef GLuint (APIENTRY *ZPFNGLCREATESHADER)( GLenum );
typedef void   (APIENTRY *ZPFNGLSHADERSOURCE)( GLuint, GLsizei, const char* const*, const GLint* );
typedef void   (APIENTRY *ZPFNGLCOMPILESHADER)( GLuint );
typedef void   (APIENTRY *ZPFNGLGETSHADERIV)( GLuint, GLenum, GLint* );
typedef void   (APIENTRY *ZPFNGLGETSHADERINFOLOG)( GLuint, GLsizei, GLsizei*, char* );
typedef GLuint (APIENTRY *ZPFNGLCREATEPROGRAM)( void );
typedef void   (APIENTRY *ZPFNGLATTACHSHADER)( GLuint, GLuint );
typedef void   (APIENTRY *ZPFNGLLINKPROGRAM)( GLuint );
typedef void   (APIENTRY *ZPFNGLGETPROGRAMIV)( GLuint, GLenum, GLint* );
typedef void   (APIENTRY *ZPFNGLUSEPROGRAM)( GLuint );
typedef GLint  (APIENTRY *ZPFNGLGETUNIFORMLOCATION)( GLuint, const char* );
typedef void   (APIENTRY *ZPFNGLUNIFORM1F)( GLint, GLfloat );
typedef void   (APIENTRY *ZPFNGLUNIFORM1I)( GLint, GLint );
typedef void   (APIENTRY *ZPFNGLUNIFORM2F)( GLint, GLfloat, GLfloat );
typedef void   (APIENTRY *ZPFNGLUNIFORM3F)( GLint, GLfloat, GLfloat, GLfloat );
typedef void   (APIENTRY *ZPFNGLACTIVETEXTURE)( GLenum );
typedef void   (APIENTRY *ZPFNGLMULTITEXCOORD2F)( GLenum, GLfloat, GLfloat );
#ifndef GL_FRAGMENT_SHADER
	#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
	#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
	#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
	#define GL_TEXTURE0 0x84C0
	#define GL_TEXTURE1 0x84C1
#endif

static ZPFNGLUSEPROGRAM         ZglUseProgram;
static ZPFNGLUNIFORM1F          ZglUniform1f;
static ZPFNGLUNIFORM1I          ZglUniform1i;
static ZPFNGLUNIFORM2F          ZglUniform2f;
static ZPFNGLUNIFORM3F          ZglUniform3f;
static ZPFNGLGETUNIFORMLOCATION ZglGetUniformLocation;
static ZPFNGLACTIVETEXTURE      ZglActiveTexture;
static ZPFNGLMULTITEXCOORD2F    ZglMultiTexCoord2f;

static void* ZGLProc( const char* Name )
{
#if UNREAL_USE_SDL
	return (void*)SDL_GL_GetProcAddress( Name );
#else
	return (void*)wglGetProcAddress( Name );
#endif
}

// Compile and link a fragment-only program (fixed-function vertex path), bind
// its "Tex" sampler to unit 0 and return the program id. 0 = no shader
// support or compile/link failure; callers just skip their effect.
static GLuint ZBuildFragmentProgram( const char* Src, const char* What )
{
	guard(ZBuildFragmentProgram);
	ZPFNGLCREATESHADER       CreateShader       = (ZPFNGLCREATESHADER)      ZGLProc("glCreateShader");
	ZPFNGLSHADERSOURCE       ShaderSource       = (ZPFNGLSHADERSOURCE)      ZGLProc("glShaderSource");
	ZPFNGLCOMPILESHADER      CompileShader      = (ZPFNGLCOMPILESHADER)     ZGLProc("glCompileShader");
	ZPFNGLGETSHADERIV        GetShaderiv        = (ZPFNGLGETSHADERIV)       ZGLProc("glGetShaderiv");
	ZPFNGLGETSHADERINFOLOG   GetShaderInfoLog   = (ZPFNGLGETSHADERINFOLOG)  ZGLProc("glGetShaderInfoLog");
	ZPFNGLCREATEPROGRAM      CreateProgram      = (ZPFNGLCREATEPROGRAM)     ZGLProc("glCreateProgram");
	ZPFNGLATTACHSHADER       AttachShader       = (ZPFNGLATTACHSHADER)      ZGLProc("glAttachShader");
	ZPFNGLLINKPROGRAM        LinkProgram        = (ZPFNGLLINKPROGRAM)       ZGLProc("glLinkProgram");
	ZPFNGLGETPROGRAMIV       GetProgramiv       = (ZPFNGLGETPROGRAMIV)      ZGLProc("glGetProgramiv");
	ZPFNGLUNIFORM1I          Uniform1i          = (ZPFNGLUNIFORM1I)         ZGLProc("glUniform1i");
	ZglGetUniformLocation = (ZPFNGLGETUNIFORMLOCATION)ZGLProc("glGetUniformLocation");
	ZglUseProgram = (ZPFNGLUSEPROGRAM)ZGLProc("glUseProgram");
	ZglUniform1f  = (ZPFNGLUNIFORM1F) ZGLProc("glUniform1f");
	ZglUniform1i  = Uniform1i;
	ZglUniform2f  = (ZPFNGLUNIFORM2F) ZGLProc("glUniform2f");
	ZglUniform3f  = (ZPFNGLUNIFORM3F) ZGLProc("glUniform3f");
	// Multitexture (core since GL 1.3), used to fold the light map into the
	// translucent base pass. Optional: callers must null-check before use.
	ZglActiveTexture   = (ZPFNGLACTIVETEXTURE)  ZGLProc("glActiveTexture");
	ZglMultiTexCoord2f = (ZPFNGLMULTITEXCOORD2F)ZGLProc("glMultiTexCoord2f");
	if( !CreateShader || !ShaderSource || !CompileShader || !GetShaderiv || !CreateProgram
	 || !AttachShader || !LinkProgram || !GetProgramiv || !ZglGetUniformLocation || !Uniform1i
	 || !ZglUseProgram || !ZglUniform1f || !ZglUniform2f || !ZglUniform3f )
	{
		debugf( NAME_Init, "OpenGL: no shader support, %s disabled", What );
		return 0;
	}
	GLuint Shader = CreateShader( GL_FRAGMENT_SHADER );
	ShaderSource( Shader, 1, &Src, NULL );
	CompileShader( Shader );
	GLint Ok = 0;
	GetShaderiv( Shader, GL_COMPILE_STATUS, &Ok );
	if( !Ok )
	{
		char Log[1024]="";
		if( GetShaderInfoLog ) GetShaderInfoLog( Shader, ARRAY_COUNT(Log), NULL, Log );
		debugf( NAME_Init, "OpenGL: %s shader compile failed: %s", What, Log );
		return 0;
	}
	GLuint Program = CreateProgram();
	AttachShader( Program, Shader );
	LinkProgram( Program );
	GetProgramiv( Program, GL_LINK_STATUS, &Ok );
	if( !Ok )
	{
		debugf( NAME_Init, "OpenGL: %s shader link failed", What );
		return 0;
	}
	ZglUseProgram( Program );
	Uniform1i( ZglGetUniformLocation( Program, "Tex" ), 0 );
	ZglUseProgram( 0 );
	return Program;
	unguard;
}

// Anisotropic filtering has been core-adjacent since 1999 (EXT_texture_filter_
// anisotropic) and every driver this port will meet has it, but ask rather than
// assume: an unsupported enum would otherwise raise a GL error on every texture
// bind. AnisotropyLimit stays 0 when unavailable and the filter is skipped.
void UOpenGLRenderDevice::QueryAnisotropy()
{
	guard(UOpenGLRenderDevice::QueryAnisotropy);
	AnisotropyLimit = 0.f;
	const char* Ext = (const char*)glGetString( GL_EXTENSIONS );
	if( Ext && appStrfind( const_cast<char*>(Ext), "GL_EXT_texture_filter_anisotropic" ) )
	{
		GLfloat Max = 0.f;
		glGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &Max );
		AnisotropyLimit = Max;
	}
	debugf( NAME_Init, "OpenGL anisotropy: hardware max %.0fx, using %.0fx",
		AnisotropyLimit, AnisotropyLimit>0.f ? Min( (FLOAT)MaxAnisotropy, AnisotropyLimit ) : 1.f );
	unguard;
}

/*-----------------------------------------------------------------------------
	Construction and registration.
-----------------------------------------------------------------------------*/

UOpenGLRenderDevice::UOpenGLRenderDevice()
{
	guard(UOpenGLRenderDevice::UOpenGLRenderDevice);
	UseVSync         = 1;
	MaxAnisotropy    = 16;
	AnisotropyLimit  = 0.f;
	LodBias          = -0.5f;
	DetailTextures   = 1;
	DetailRange      = 200.f;	// retail Glide's NearZ
	DetailBias       = -0.5f;
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
	WavyProgram      = 0;
	WavyLocTime      = -1;
	WavyLocAmp       = -1;
	WavyLocUVMult    = -1;
	WavyLocGloss     = -1;
	WavyLocBase      = -1;
	WavyLocLightOn   = -1;
	WavyLocUEdge     = -1;
	WavyLocVEdge     = -1;
	WavyLocFlowV     = -1;
	WavyLocScroll    = -1;
	WavyLocBlob      = -1;
	WavyLocFoam      = -1;
	WavyLocFall      = -1;
	WavyLocFallDn    = -1;
	WavyLocFallFd    = -1;
	WavyLocFallSol   = -1;
	WavyTried        = 0;
	SkyFrame         = 0;
	NumStreamCols    = 0;
	FrameStamp       = 0;
	UnderwaterProgram= 0;
	UWLocTime        = -1;
	UWLocAmp         = -1;
	UWLocPx          = -1;
	UnderwaterTried  = 0;
	UnderwaterTex    = 0;
	UnderwaterTexW   = -1;
	UnderwaterTexH   = -1;
	ViewerUnderwater = 0;
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
		new(Class,"MaxAnisotropy", RF_Public)UIntProperty( CPP_PROPERTY(MaxAnisotropy), "Options", CPF_Config );
		new(Class,"LodBias", RF_Public)UFloatProperty( CPP_PROPERTY(LodBias), "Options", CPF_Config );
		new(Class,"DetailTextures", RF_Public)UBoolProperty( CPP_PROPERTY(DetailTextures), "Options", CPF_Config );
		new(Class,"DetailRange", RF_Public)UFloatProperty( CPP_PROPERTY(DetailRange), "Options", CPF_Config );
		new(Class,"DetailBias", RF_Public)UFloatProperty( CPP_PROPERTY(DetailBias), "Options", CPF_Config );
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
	QueryAnisotropy();

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
	QueryAnisotropy();

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
	// Recompile the wavy warp program on next use: Flush accompanies context
	// or mode changes, where the old program id may be stale. (The old object,
	// if the context survived, is a few bytes -- not worth a delete-proc.)
	WavyProgram = 0;
	WavyTried   = 0;
	UnderwaterProgram = 0;
	UnderwaterTried   = 0;
	// Unlike the programs, the underwater copy texture is screen-sized -- do
	// delete it (a GL context is current whenever Flush runs, see above loop).
	if( UnderwaterTex )
		glDeleteTextures( 1, &UnderwaterTex );
	UnderwaterTex  = 0;
	UnderwaterTexW = UnderwaterTexH = -1;
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

	// x64 port: -framestats ??? the first GL calls of the frame absorb the
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
	FrameStamp++;	// fountain columns draw once per stamp (their boxes persist)

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

		// x64 port: -framestats ??? time the present separately from the frame.
		static UBOOL FrameStats = ParseParam( appCmdLine(), "FRAMESTATS" );
		if( FrameStats )
		{
			DOUBLE T0 = appSeconds();
#if UNREAL_USE_SDL
			SDL_GL_SwapWindow( SdlWindow );
#else
			SwapBuffers( hDC );
#endif
			glFinish(); // x64 port: sync the frame boundary ??? see non-stats path below
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
	// Underwater refraction: when the view origin is in a water zone (set by
	// SetSceneNode), copy the finished 3D view into a texture and redraw it
	// through the sinusoidal warp program -- the software renderer's
	// underwater wobble, which the hardware drivers never had. Runs before
	// the flash overlay (the tint is uniform; warping it adds nothing) and
	// before the HUD, which the engine draws after EndFlash.
	if( ViewerUnderwater && CurrentFrame && Viewport && !HitDataPtr )
	{
		if( !UnderwaterTried )
			InitUnderwaterProgram();
		FSceneNode* F = CurrentFrame;
		while( F->Parent )
			F = F->Parent;
		INT W = F->X, H = F->Y;
		if( UnderwaterProgram && W>0 && H>0 )
		{
			INT X0 = F->XB, Y0 = Viewport->SizeY - F->Y - F->YB;

			// (Re)build the copy texture at the view size.
			if( !UnderwaterTex )
				glGenTextures( 1, &UnderwaterTex );
			glBindTexture( GL_TEXTURE_2D, UnderwaterTex );
			CurrentTextureID = 0;	// SetTexture must rebind after this pass
			if( W!=UnderwaterTexW || H!=UnderwaterTexH )
			{
				glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, W, H, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
				UnderwaterTexW = W;
				UnderwaterTexH = H;
			}
			glCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, X0, Y0, W, H );

			ALevelInfo* Info = F->Level ? F->Level->GetLevelInfo() : NULL;
			DOUBLE T = Info ? (DOUBLE)Info->TimeSeconds : 0.0;
			glViewport( X0, Y0, W, H );
			glDisable( GL_DEPTH_TEST );
			glDisable( GL_BLEND );
			glDisable( GL_ALPHA_TEST );
			ZglUseProgram( UnderwaterProgram );
			// Wrap where every wave rate (multiples of 0.05 rad/s) completes.
			ZglUniform1f( UWLocTime, (FLOAT)fmod( T, 125.66370614359172 ) );
			ZglUniform1f( UWLocAmp, Max( 3.f, H/240.f ) );	// pixels: ~4.5 at 1080p
			ZglUniform2f( UWLocPx, 1.f/W, 1.f/H );
			glMatrixMode( GL_PROJECTION );
			glPushMatrix();
			glLoadIdentity();
			glMatrixMode( GL_MODELVIEW );
			glPushMatrix();
			glLoadIdentity();
			glColor3f( 1.f, 1.f, 1.f );
			glBegin( GL_QUADS );
			glTexCoord2f( 0, 0 ); glVertex2f( -1, -1 );
			glTexCoord2f( 1, 0 ); glVertex2f(  1, -1 );
			glTexCoord2f( 1, 1 ); glVertex2f(  1,  1 );
			glTexCoord2f( 0, 1 ); glVertex2f( -1,  1 );
			glEnd();
			ZglUseProgram( 0 );
			glPopMatrix();
			glMatrixMode( GL_PROJECTION );
			glPopMatrix();
			glMatrixMode( GL_MODELVIEW );
			glEnable( GL_DEPTH_TEST );
			CurrentBlendFlags = (DWORD)-1;
			CurrentFrame = NULL;	// next SetSceneNode must re-apply the viewport
		}
	}
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
	// x64 port: zNear must be below 1.0 ??? the canvas draws tiles/fonts at
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

	// Is this scene node the SKY BOX? Render draws it as a child frame whose
	// zone is the sky zone (see the PF_FakeBackdrop portal in UnRender), which
	// is what distinguishes it from the other child frames -- a mirror or a
	// warp zone reaches us the same way. Same test Render itself uses to know
	// when to clear Z after a sky.
	SkyFrame = 0;
	if( Frame->Parent && Frame->Level && Frame->Level->Model
	 && Frame->Level->Model->Nodes && Frame->ZoneNumber>=0 && Frame->ZoneNumber<64 )
		SkyFrame = Cast<ASkyZoneInfo>( Frame->Level->Model->Nodes->Zones[Frame->ZoneNumber].ZoneActor ) != NULL;

	// Resolve the viewer's zone distance fog (bFogZone + FogColor + FogDistance).
	// Verts reach us in Unreal eye space, so Point.Z is the view depth in the
	// same world units as FogDistance ??? no remapping needed.
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

	// Underwater view warp (see EndFlash): does the view origin itself sit in
	// a water zone? Recomputed from the BSP rather than read off the player
	// actor -- Region is the body's zone (wrong when wading with the camera
	// above the surface, and stale on pinned probe cameras); this matches how
	// Render resolves the view zone (UnRender.cpp SetupFrame). Root frames
	// only: sky/mirror child frames have their own unrelated origins.
	if( !Frame->Parent )
	{
		ViewerUnderwater = 0;
		if( !GIsEditor && Frame->Level && Frame->Level->Model && Frame->Level->GetLevelInfo() )
		{
			AZoneInfo* VZone = Frame->Level->Model->PointRegion( Frame->Level->GetLevelInfo(), Frame->Coords.Origin ).Zone;
			ViewerUnderwater = VZone && VZone->bWaterZone;
		}
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
	{
		glDepthMask( GL_FALSE );

		// x64 port: these are the map's DECALS -- ripple rings, scorch marks,
		// mist sheets -- and mappers lay them a unit or two above the surface
		// they decorate, trusting the BSP sort to order them. The depth buffer
		// does not have a unit to spare at range: with zNear 0.5 and zFar 49152
		// a 24-bit buffer resolves about d*d/8.4e6 world units, so 1 unit stops
		// being representable past ~2900 and the decal starts winning and losing
		// the test pixel by pixel. NyLeve's HubEffects.WaterRings2 sits exactly
		// 1 unit over the lake; stand at the TOP of the big fall (the save spot)
		// and the lake is ~5400 away, where the quantum is ~3.5 units, and the
		// decal breaks into 1-pixel horizontal dashes.
		//
		// It shows up only at high resolution -- at 800x600 the same view is
		// clean, which is why this took two passes to pin down. Always reproduce
		// a rasterisation complaint at the reporter's RESOLUTION.
		//
		// Bias them toward the viewer. They do not write depth (just above), so
		// nothing drawn later can inherit the bias. Masked surfaces are excluded:
		// their later passes re-draw the same geometry under glDepthFunc(GL_EQUAL)
		// (see DrawComplexSurface), which an offset would make match nothing. The
		// slope term does most of the work here -- the lake is near edge-on from
		// up there, which is exactly where depth quantisation is worst.
		if( !(PolyFlags & PF_Masked) )
		{
			glEnable( GL_POLYGON_OFFSET_FILL );
			glPolygonOffset( -1.f, -4.f );
		}
		else
			glDisable( GL_POLYGON_OFFSET_FILL );
	}
	else
	{
		glDepthMask( GL_TRUE );
		glDisable( GL_POLYGON_OFFSET_FILL );
	}
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
	// Modulated uploads skip the gamma bake entirely (see UploadTexture), so
	// they need their own entry too -- the same texture drawn unmodulated
	// somewhere else would otherwise hand them its gamma-baked texels.
	if( PolyFlags & PF_Modulated )
		TestID |= (QWORD)64;
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
		// x64 port: guard file-controlled 0 scale/size ??? a zero divisor gave
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
		// (UnGlide.cpp, its misleadingly named DetailBias, shipped at -1.5).
		// The default here is gentler because trilinear at 4K shimmers where
		// 640x480 did not; LodBias reopens the retail value to anyone who
		// wants it.
		// (::Clamp -- SetTexture's own Clamp parameter shadows the global.)
		glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, ::Clamp( LodBias, -3.f, 3.f ) );
		// Anisotropy, for the mipmapped world textures only: sprite tiles are
		// mip-0 single-level uploads with nothing for it to do, and unfiltered
		// (PF_NoSmooth) surfaces are asking for the crunchy look on purpose.
		if( AnisotropyLimit > 1.f && Info.NumMips>1 && !SpriteTile && Smooth )
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
				Min( (FLOAT)MaxAnisotropy, AnisotropyLimit ) );
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
		// FColor byte count <= 2^22 ??? well within INT).
		INT USize = Mip->USize, VSize = Mip->VSize;
		if( USize<=0 || VSize<=0 || USize>MAX_TEXTURE_SIZE || VSize>MAX_TEXTURE_SIZE )
			break;
		INT Count = USize*VSize;
		// Bytes actually available at Src. For loaded textures this is the
		// serialized DataArray length, which the format does NOT guarantee
		// matches USize*VSize ??? so every source read below is clamped to it to
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
		// NOT for anything MODULATED: its texels are blend FACTORS (2*src*dst),
		// not colors -- the background they multiply is already gamma-baked, so
		// lifting the texels too double-applies brightness. Worse, the lift
		// moves the texture's neutral-gray surround (127) off the blend's fixed
		// neutral point, so at any brightness above 0.5 the whole quad
		// brightened what's behind it -- a straight-edged light square.
		//
		// This was first found on the grenade's BlackSmoke trail and fixed for
		// sprite tiles only, but nothing about the argument is sprite-specific:
		// modulated WORLD SURFACES break the same way, and more visibly, because
		// they are big. NyLeve's waterfall lands on HubEffects.WaterRings2, a
		// modulated ripple decal 2948x2683 across laid over the lake -- at the
		// shipped Brightness 0.7 its neutral surround lifted and the whole decal
		// read as a hard-edged pale square sitting at the foot of the fall, with
		// the ripple rings banding across it.
		if( (AppliedGamma<0.49f || AppliedGamma>0.51f)
		&&	!(PolyFlags & PF_Modulated) )
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
		// Modulated art's "empty" surround is not exactly neutral (bs2_* sits at
		// 129..131 vs the 2*src*dst neutral of 127.5), so the whole quad faintly
		// brightens everything behind it -- a visible straight-edged tint. Same
		// idea as the translucent dither-floor gate: pull texels within +-12 of
		// neutral quadratically onto it (continuous at the band edge), which
		// flattens the surround to a true no-op while leaving real content (the
		// smoke values 70..110, a ripple ring's light and dark) untouched.
		//
		// Applies to every modulated upload, not just sprite tiles. NyLeve's
		// WaterRings2 is a modulated ripple decal laid on the lake at the big
		// fall's foot, and its off-neutral surround drew the decal's rectangular
		// brush outline across the water as a paler box -- the same artifact the
		// grenade trail showed, just on a world surface. Paired with skipping the
		// gamma bake for modulated content (above); both are needed, since gamma
		// moves neutral and this flattens what is already near it.
		if( PolyFlags & PF_Modulated )
		{
			// First remove any uniform DC BIAS. A modulated decal is supposed to
			// be invisible where it has nothing to say and show only its
			// variation, but a texture whose flat field does not sit exactly on
			// 127.5 tints its whole quad -- and since the quad is a mapper's
			// brush, that tint has straight edges and reads as a pale BOX.
			// NyLeve's WaterRings2 rests at 134.5 (measured: border 134.3, centre
			// 135.1, so a genuine constant offset rather than content), which is
			// 2*134.5/255 = 1.055 -- every pixel behind the decal brightened 5.5%,
			// which is the square at the foot of the big waterfall.
			//
			// This is faithful to 1998 -- the procedural WaveTexture code is
			// byte-identical to the v200 reference, so retail showed the same box
			// -- but it is plainly an artifact rather than intent, and it is the
			// one thing standing between this decal and doing its job.
			//
			// Only ever a small correction: if the flat field is more than 25 off
			// neutral the texture MEANS to darken or brighten (smoke, shadow
			// decals) and is left alone.
			//
			// The estimator is the MODE -- the most common value -- because that
			// is literally the level most of the texture sits at. The mean is
			// dragged up by bright crests and the median down by the dark ring
			// troughs: measured here the mean says 134.5 while the median lands
			// BELOW neutral, and correcting to the median made the box brighter
			// rather than dimmer. The mode picks out the flat field the eye reads.
			INT Hist[256];
			for( INT h=0; h<256; h++ )
				Hist[h] = 0;
			for( INT i=0; i<Count; i++ )
				Hist[ ( Dst[i].R + Dst[i].G + Dst[i].B + 1 ) / 3 ]++;
			INT Mode=128, ModeCount=-1;
			for( INT v=0; v<256; v++ )
				if( Hist[v] > ModeCount ) { ModeCount = Hist[v]; Mode = v; }
			FLOAT Bias = 127.5f - (FLOAT)Mode;
			if( Abs(Bias) > 25.f )
				Bias = 0.f;
			// -probemodinfo reports what the estimator saw, so a correction that
			// does nothing can be told apart from one that is not running.
			{
				static UBOOL ModInfo = ParseParam( appCmdLine(), "PROBEMODINFO" );
				static INT ModLogged = 0;
				if( ModInfo && ModLogged<10 )
				{
					ModLogged++;
					debugf( NAME_Log, "MODPROBE %ix%i count=%i mode=%i modecount=%i bias=%.1f",
						USize, VSize, Count, Mode, ModeCount, Bias );
				}
			}
			// -probeflatmod forces every modulated texel to the blend's exact
			// no-op. Anything still visible afterwards is NOT the texture, which
			// is how the pale box at the waterfall was traced to the shader.
			static UBOOL FlatMod = ParseParam( appCmdLine(), "PROBEFLATMOD" );
			for( INT i=0; i<Count; i++ )
			{
				FColor& C = Dst[i];
				if( FlatMod ) { C.R = C.G = C.B = 128; continue; }
				BYTE* Ch[3] = { &C.R, &C.G, &C.B };
				for( INT c=0; c<3; c++ )
				{
					FLOAT V = (FLOAT)*Ch[c] + Bias;
					// Then flatten what is left near neutral, so a surround that
					// is merely speckly also becomes a true no-op.
					FLOAT D = V - 127.5f;
					FLOAT A = Abs(D);
					if( A < 12.f )
						V = 127.5f + D*(A/12.f)*(A/12.f);
					*Ch[c] = (BYTE)Clamp( appRound(V), 0, 255 );
				}
			}
		}

		if( SpriteTile )
		{
			FLOAT Neutral = (PolyFlags & PF_Modulated) ? 127.5f : 0.f;
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

// PF_SmallWavy/PF_BigWavy liquid surfaces, plus translucent auto-panning ones
// (flowing water: rivers, waterfalls). Retail v200 faked wavy by sliding the
// whole texture with a rigid sinusoidal pan (in Render's UnRender.cpp, now
// retired), and auto-pan alone slides the picture rigidly along the flow -- a
// liquid read as a static picture being shifted around. Instead, warp the
// texture lookup per PIXEL: a sinusoidal texel-space UV offset, phase keyed to
// the surface's own texel coordinates, so the image undulates internally like
// liquid. Two superimposed waves per axis keep the pattern from reading as
// periodic. (For auto-panning surfaces the phase key includes the pan offset,
// so the ripples travel with the current -- flowing water shimmers as it
// flows.)
//
// Translucent wavy/panning surfaces read as WATER and additionally get
// slope-keyed glint shading (Gloss: brightness rides the wave slope, like
// light catching ripples -- a cheap reflectivity cue) and a slight base fade
// (BaseMul<1: under the engine's ONE/ONE_MINUS_SRC_COLOR translucency a dimmer
// source lets more of what's beyond show through). OPAQUE wavy surfaces
// (slime, lava) keep the plain warp: Gloss=0, BaseMul=1.
//
// Per-pixel (a fragment program on the base pass only) rather than per-vertex
// over a tessellated grid: a tessellated version drew each cell with a
// constant UV gradient that jumped every frame as the warp moved, so at
// minification neighboring cells flickered between mip levels -- visible
// strobing. The shader's UV field is smooth, so its derivatives (and mip
// selection) are too, and the pass draws the exact same fan as every other
// pass, so depth and coverage stay consistent for the light-map/fog passes.
// If the context has no shader support, the surface just draws unwarped.
void UOpenGLRenderDevice::InitWavyProgram()
{
	guard(UOpenGLRenderDevice::InitWavyProgram);
	WavyTried = 1;

	// Wave numbers are 2pi/64, 2pi/96, 2pi/80 texels. The glint term is the
	// analytic slope of the displacement field, rescaled to [-1,1].
	static const char* Src =
		"#version 120\n"
		"uniform sampler2D Tex;\n"
		"uniform float Time;\n"
		"uniform float Amp;\n"			// amplitude in texels
		"uniform vec2 UVMult;\n"		// driver's UMult/VMult: texcoord units per texel
		"uniform float Gloss;\n"		// slope-glint strength (water only, else 0)
		"uniform float BaseMul;\n"		// base color scale (water only, else 1)
		"uniform sampler2D LightTex;\n"	// light map on unit 1 (when LightOn)
		"uniform float LightOn;\n"		// 1 = multiply by 2x light map (lit translucent)
		"uniform vec2 UEdge;\n"			// texcoord-u extent of a stream sheet; y<=x disables
		"uniform vec2 VEdge;\n"			// texcoord-v extent of a stream sheet; y<=x disables
		"uniform float FlowV;\n"		// 1 = stream flows along t, 0 = along s
		"uniform float Scroll;\n"		// texture scroll along the flow (texcoord units)
		"uniform float Blob;\n"			// 0 = stream sheet, 1 = radial droplet, 2 = band (ripple ring), 3 = falling sheet
		"uniform float Foam;\n"			// 1 = desaturate toward white (froth)
		"uniform float Fall;\n"			// 1 = falling sheet (waterfall face)
		"uniform float FallDown;\n"		// +1 if the flow axis grows downward, else -1
		"uniform float FallFade;\n"		// 1 = the fall ends in open air, not water
		"uniform float FallSolid;\n"	// 1 = the fall is opaque, so no alpha tricks
		// Value noise on a lattice: hashed corners, smoothstepped between.
		// Used to wobble the froth particles' outlines (see the Blob branch)
		// so a bank of them does not read as a heap of perfect discs.
		"float ZHash( vec2 p )\n"
		"{\n"
		"	return fract( sin( dot( p, vec2(12.9898,78.233) ) ) * 43758.5453 );\n"
		"}\n"
		"float ZNoise( vec2 p )\n"
		"{\n"
		"	vec2 i = floor(p), f = fract(p);\n"
		"	f = f*f*(3.0-2.0*f);\n"
		"	return mix( mix( ZHash(i), ZHash(i+vec2(1.0,0.0)), f.x ),\n"
		"	            mix( ZHash(i+vec2(0.0,1.0)), ZHash(i+vec2(1.0,1.0)), f.x ), f.y );\n"
		"}\n"
		"void main()\n"
		"{\n"
		"	vec2 t = gl_TexCoord[0].st / UVMult;\n"
		"	float a1 = t.y*0.0981748 + 2.1*Time;\n"
		"	float a2 = (t.x+t.y)*0.0654498 - 1.6*Time;\n"
		"	float b1 = t.x*0.0981748 + 1.9*Time;\n"
		"	float b2 = (t.x-t.y)*0.0785398 + 1.3*Time;\n"
		"	float dU = Amp*( 0.6*sin(a1) + 0.4*sin(a2) );\n"
		"	float dV = Amp*( 0.6*cos(b1) + 0.4*cos(b2) );\n"
		// A falling sheet does not ripple like a pool: its water is drawn out
		// into long strands running down the face. Displace ALONG the fall by
		// an amount that varies ACROSS it, so neighbouring strands slip past
		// one another, and sway only gently sideways. Isotropic pool ripples
		// on a vertical face read as a wobbling pane, never as a fall.
		"	float ac = mix( t.y, t.x, FlowV );\n"	// across the fall, in texels
		"	float al = mix( t.x, t.y, FlowV );\n"	// along it
		"	float st1 = sin( ac*0.42 + 0.9*Time );\n"
		"	float st2 = sin( ac*0.13 - 0.5*Time );\n"
		"	float dAl = Amp*( 2.4*st1 + 3.4*st2 );\n"
		"	float dAc = Amp*0.30*sin( al*0.045 + 1.4*Time );\n"
		"	vec2 dF = mix( vec2(dAl,dAc), vec2(dAc,dAl), FlowV );\n"
		// Stream sheets scroll the droplet texture along the fall -- fast
		// coherent downward motion is what reads as RUNNING water; the
		// fractal animation alone just simmers in place.
		"	vec2 uv = gl_TexCoord[0].st + mix( vec2(dU,dV), dF, Fall )*UVMult;\n"
		"	uv -= mix( vec2(Scroll,0.0), vec2(0.0,Scroll), FlowV );\n"
		"	vec4 C = texture2D( Tex, uv ) * gl_Color;\n"
		// Light is clamped at 1.0: overbright (the opaque Pass-2 modulate goes
		// to 2x) saturates every texel of a self-lit stream to a solid block
		// of color, erasing the droplet contrast that makes it read as water.
		"	C.rgb *= mix( vec3(1.0), min( texture2D( LightTex, gl_TexCoord[1].st ).rgb*2.0, vec3(1.0) ), LightOn );\n"
		"	float g = 0.5*( 0.6*cos(a1) + 0.4*cos(a2) - 0.6*sin(b1) - 0.4*sin(b2) );\n"
		"	C.rgb *= BaseMul*( 1.0 + Gloss*g );\n"
		// Falling water is not an even veil. Under the engine's
		// ONE/ONE_MINUS_SRC_COLOR translucency the source colour IS the
		// opacity, so bending the texel toward its own square opens the dark
		// texels into see-through gaps and leaves the bright ones standing as
		// solid strands -- a curtain of water rather than a pane of tinted
		// glass. The strand field then varies their brightness across the
		// face so the eye has edges to follow as they travel.
		"	if( Fall > 0.5 )\n"
		"	{\n"
		"		float sg = 0.5 + 0.5*sin( ac*0.19 - 0.55*Time + 2.1 );\n"
		"		float strand = ( 0.5 + 0.5*st1 )*( 0.55 + 0.45*sg );\n"
		// The opacity gate is for a TRANSLUCENT fall only -- there the colour
		// is the opacity, so squaring it opens gaps. On an opaque sheet the
		// same arithmetic just darkens it, so skip it and let the strands do
		// the work on their own.
		"		C.rgb = mix( C.rgb*( 0.40 + 1.20*C.rgb ), C.rgb, FallSolid );\n"
		"		C.rgb *= mix( 0.55 + 1.55*strand, 0.72 + 0.62*strand, FallSolid );\n"
		"	}\n"
		// Lateral edge fade for stream sheets: a fountain column is a
		// hard-edged quad, and under additive translucency dark = transparent,
		// so rounding the sides off in RGB reads as a narrowing stream. The
		// band is capped at 20 texels so a wide lit sheet only vignettes.
		// Stream shaping: a flat quad reads as a round JET when its brightness
		// follows a cylinder profile across the width -- bright core, limbs
		// falling to nothing (no hard edges to read as a box). Along the flow
		// a short band melts each end into whatever it pours from and into.
		"	float w = UEdge.y - UEdge.x;\n"
		"	float h = VEdge.y - VEdge.x;\n"
		"	if( w > 0.0 && h > 0.0 )\n"
		"	{\n"
		"		float cu = clamp( (gl_TexCoord[0].s - UEdge.x)/w, 0.0, 1.0 );\n"
		"		float cv = clamp( (gl_TexCoord[0].t - VEdge.x)/h, 0.0, 1.0 );\n"
		"		if( Blob > 2.5 )\n"
		"		{\n"
		// Falling sheet. A fall has no ruled vertical borders and no clean
		// horizontal top, so soften both sides and melt the lip it comes over.
		// Extents come from the surface's whole world box, so this is stable as
		// the camera moves and identical on every facet of the same fall.
		"			float cx = mix( cv, cu, FlowV );\n"	// across the fall
		"			float fl = mix( cu, cv, FlowV );\n"	// along it
		"			fl = mix( 1.0-fl, fl, step( 0.0, FallDown ) );\n"	// 0 = lip, 1 = foot
		// Edge softening is fading toward TRANSPARENT, which only works under
		// additive translucency; on an opaque sheet it would paint black
		// borders, so it is skipped there (mix toward 1.0 = no change).
		"			float edge = smoothstep( 0.0, 0.10, cx )*( 1.0 - smoothstep( 0.90, 1.0, cx ) )\n"
		"			           * smoothstep( 0.0, 0.06, fl );\n"
		"			C.rgb *= mix( edge, 1.0, FallSolid );\n"
		"			float foot = smoothstep( 0.80, 1.0, fl );\n"
		// What happens at the bottom depends on whether there is anything down
		// there. Landing IN water, the fall is aerated where it hits: froth and
		// brighten. Falling into OPEN AIR -- off a ledge, into a sky box -- it
		// does not end at all, it thins into spray, so it has to DISSOLVE well
		// before the sheet's edge. Brightening that edge instead (which is what
		// the froth did) leaves a lit rectangle hanging in the sky, which is
		// exactly how the mapper's sheet gives itself away.
		"			vec3 Wet = mix( C.rgb, vec3( dot( C.rgb, vec3(0.3,0.59,0.11) ) )*1.30 + C.rgb*0.25, foot*0.75 )\n"
		"			         * ( 1.0 + 0.45*foot );\n"
		"			vec3 Air = C.rgb * ( 1.0 - smoothstep( 0.35, 0.97, fl ) );\n"
		"			C.rgb = mix( Wet, Air, FallFade*( 1.0 - FallSolid ) );\n"
		"		}\n"
		"		else if( Blob > 1.5 )\n"
		"		{\n"
		// Ripple ring: soft band across its width, unbroken along its length
		// (so ring segments join seamlessly).
		"			float cx = mix( cv, cu, FlowV );\n"
		"			float p = 4.0*cx*(1.0-cx);\n"
		"			C.rgb *= p*( 0.55 + 0.75*p );\n"
		"		}\n"
		"		else if( Blob > 0.5 )\n"
		"		{\n"
		// Droplet / froth puff. A plain radial gate (1 - dot(q,q)) is a
		// perfect disc, and a bank of them reads as a heap of BUBBLES rather
		// than as foam. So wobble the silhouette: the cutoff radius varies
		// with direction, by a noise sampled on the unit circle (continuous
		// all the way round -- keyed to the ANGLE it would seam at +/-pi) and
		// offset by a per-particle seed riding in on texcoord r, so no two
		// puffs are the same shape. Two octaves: one for the overall lopsided
		// outline, one for the ragged edge.
		"			vec2 q = vec2(cu,cv)*2.0 - 1.0;\n"
		"			float r = length(q);\n"
		"			vec2 dir = q / max( r, 0.001 );\n"
		"			float sd = gl_TexCoord[0].p;\n"
		// Clamped below 1: the gate has to reach zero INSIDE the quad, or the
		// puff is cut off square along the quad's own edge and the hard
		// rectangle is worse than the disc it replaced.
		"			float wob = min( 0.62 + 0.46*ZNoise( dir*2.2 + vec2(sd*11.0,sd*7.0) )\n"
		"			                      + 0.20*ZNoise( dir*5.5 + vec2(sd*3.0,sd*13.0) ), 0.97 );\n"
		"			C.rgb *= max( 1.0 - (r*r)/max( wob*wob, 0.04 ), 0.0 );\n"
		"		}\n"
		"		else\n"
		"		{\n"
		"			float cx = mix( cv, cu, FlowV );\n"	// across the stream
		"			float fl = mix( cu, cv, FlowV );\n"	// along the flow
		"			float p = 4.0*cx*(1.0-cx);\n"
		"			C.rgb *= p*( 0.6 + 0.7*p );\n"
		"			C.rgb *= smoothstep( 0.0, 0.12, fl ) * ( 1.0 - smoothstep( 0.88, 1.0, fl ) );\n"
		"		}\n"
		"	}\n"
		// Froth is aerated water: nearly white, keeping a trace of the
		// liquid's own tint so it still belongs to the pool it sits on.
		"	C.rgb = mix( C.rgb, vec3( dot( C.rgb, vec3(0.3,0.59,0.11) ) )*1.25 + C.rgb*0.20, Foam );\n"
		"	gl_FragColor = C;\n"
		"}\n";

	GLuint Program = ZBuildFragmentProgram( Src, "wavy surface warp" );
	if( !Program )
		return;
	WavyLocTime    = ZglGetUniformLocation( Program, "Time" );
	WavyLocAmp     = ZglGetUniformLocation( Program, "Amp" );
	WavyLocUVMult  = ZglGetUniformLocation( Program, "UVMult" );
	WavyLocGloss   = ZglGetUniformLocation( Program, "Gloss" );
	WavyLocBase    = ZglGetUniformLocation( Program, "BaseMul" );
	WavyLocLightOn = ZglGetUniformLocation( Program, "LightOn" );
	WavyLocUEdge   = ZglGetUniformLocation( Program, "UEdge" );
	WavyLocVEdge   = ZglGetUniformLocation( Program, "VEdge" );
	WavyLocFlowV   = ZglGetUniformLocation( Program, "FlowV" );
	WavyLocScroll  = ZglGetUniformLocation( Program, "Scroll" );
	WavyLocBlob    = ZglGetUniformLocation( Program, "Blob" );
	WavyLocFoam    = ZglGetUniformLocation( Program, "Foam" );
	WavyLocFall    = ZglGetUniformLocation( Program, "Fall" );
	WavyLocFallDn  = ZglGetUniformLocation( Program, "FallDown" );
	WavyLocFallFd  = ZglGetUniformLocation( Program, "FallFade" );
	WavyLocFallSol = ZglGetUniformLocation( Program, "FallSolid" );
	ZglUseProgram( Program );
	ZglUniform1i( ZglGetUniformLocation( Program, "LightTex" ), 1 );
	ZglUseProgram( 0 );
	WavyProgram    = Program;
	debugf( NAME_Init, "OpenGL: wavy surface warp shader ready" );
	unguard;
}

// Full-screen refraction for a submerged camera (see EndFlash): the finished
// 3D view, resampled through a sinusoidal screen-space warp. Amp is in
// pixels; Px is 1/view size, and the sample point is clamped half a pixel
// inside the copy so the warp never reads beyond the view rect.
void UOpenGLRenderDevice::InitUnderwaterProgram()
{
	guard(UOpenGLRenderDevice::InitUnderwaterProgram);
	UnderwaterTried = 1;

	static const char* Src =
		"#version 120\n"
		"uniform sampler2D Tex;\n"
		"uniform float Time;\n"
		"uniform float Amp;\n"			// amplitude in pixels
		"uniform vec2 Px;\n"			// 1 / view size in pixels
		"void main()\n"
		"{\n"
		"	vec2 uv = gl_TexCoord[0].st;\n"
		"	vec2 d;\n"
		"	d.x = sin( uv.y*11.0 + 1.90*Time ) + 0.55*sin( uv.y*23.0 - 2.60*Time );\n"
		"	d.y = cos( uv.x*13.0 + 1.60*Time ) + 0.55*cos( uv.x*19.0 + 2.20*Time );\n"
		"	uv += d*Amp*Px;\n"
		"	gl_FragColor = texture2D( Tex, clamp( uv, 0.5*Px, vec2(1.0)-0.5*Px ) );\n"
		"}\n";

	GLuint Program = ZBuildFragmentProgram( Src, "underwater view warp" );
	if( !Program )
		return;
	UWLocTime = ZglGetUniformLocation( Program, "Time" );
	UWLocAmp  = ZglGetUniformLocation( Program, "Amp" );
	UWLocPx   = ZglGetUniformLocation( Program, "Px" );
	UnderwaterProgram = Program;
	debugf( NAME_Init, "OpenGL: underwater view warp shader ready" );
	unguard;
}

static inline FLOAT ZSmooth( FLOAT A, FLOAT B, FLOAT X )
{
	X = Clamp( (X-A)/(B-A), 0.f, 1.f );
	return X*X*(3.f-2.f*X);
}

// What is underneath a fall, and where its water surface is. Returns 1 and
// fills SurfZ when the foot ends in water; 0 when it ends in anything else --
// stone, or nothing at all, which is what a fall spilling off a ledge into a
// sky box does. The sheet's own shading and its particles must agree about
// this, so both ask here.
static UBOOL ZFallLandsInWater( FSceneNode* Frame, const FVector& Centre, const FVector& Spread, FLOAT BottomZ, FLOAT& SurfZ )
{
	if( !Frame->Level || !Frame->Level->Model || !Frame->Level->GetLevelInfo() )
		return 0;
	ALevelInfo* Info = Frame->Level->GetLevelInfo();
	UModel* Model = Frame->Level->Model;
	FVector Probe = Centre;
	// Search DOWN for the water, not just immediately below. A fall is rarely
	// one sheet: mappers split it where the rock steps, so the piece you are
	// drawing often stops a hundred units or more above the pool the whole
	// thing lands in (SkyTown's ends at z=2080 over a river at 1952). Probing
	// only just under the sheet found air there and the landing went undrawn.
	// Bounded, so this cannot reach through a floor into unrelated water below.
	// Sampled ACROSS the foot as well as down it: a slanted sheet's bounding-box
	// centre can sit well to one side of where the water actually is (SkyTown's
	// is at x=590 with the river ending at x=384), so a single plumb line down
	// the middle misses the pool entirely.
	// Kept SHORT. A fall is usually several stacked sheets, and a generous
	// search let every one of them find the pool at the bottom and draw a
	// landing there -- so the upper sheets put froth on the water far from
	// where they end, behind whatever geometry sits between. Only the sheet
	// that genuinely reaches the water gets a foot; the ones above it land on
	// the next sheet down, which is not an event.
	const FLOAT MaxDrop = 64.f;
	UBOOL Found = 0;
	for( FLOAT D=6.f; D<=MaxDrop && !Found; D+=12.f )
		for( INT s=0; s<5 && !Found; s++ )
		{
			Probe = Centre + Spread*( s==0 ? 0.f : (s<3 ? 0.45f : 0.9f)*( (s&1) ? 1.f : -1.f ) );
			Probe.Z = BottomZ - D;
			AZoneInfo* BZone = Model->PointRegion( Info, Probe ).Zone;
			if( BZone && BZone->bWaterZone )
				{ BottomZ -= D; Found = 1; }
		}
	static UBOOL FallInfo = ParseParam( appCmdLine(), "PROBEFALLINFO" );
	if( FallInfo )
		debugf( "FALLFOOT: centre=(%.0f,%.0f,%.0f) found=%i", Centre.X, Centre.Y, Centre.Z, (INT)Found );
	if( !Found )
		return 0;
	// Walk UP out of the water for the true surface: sheets are often built
	// poking a little way into the pool.
	SurfZ = BottomZ + 1.f;
	for( INT st=0; st<10; st++ )
	{
		Probe.Z = BottomZ + st*4.f;
		AZoneInfo* PZone = Model->PointRegion( Info, Probe ).Zone;
		if( !PZone || !PZone->bWaterZone )
			{ SurfZ = Probe.Z; break; }
	}
	SurfZ += 1.f;	// clear of the pool surface itself
	return 1;
}

// Where a waterfall LANDS. The sheet stops dead at the waterline, so however
// well it falls it reads as passing BEHIND a pane of water rather than hitting
// it -- a fall with no foot is the single loudest thing left. Three effects are
// drawn along the line where the sheet meets the pool, all of them pure
// functions of level time (stateless, so saves, demos and paused frames agree),
// and all sized off the fall's own width:
//   FROTH  aerated white puffs churning on the line and drifting outward, drawn
//          FLAT IN the water plane -- camera-facing froth stands up out of the
//          water the moment you look down on it from a ledge.
//   WAKE   ripple bands travelling out from the line to either side, born on
//          the cadence of the water arriving rather than a clock of their own.
//   SPRAY  droplets thrown up and out of the impact and arcing back down. These
//          ARE camera-facing: they are in the air, not on the surface.
// Only when the foot really ends in water -- a fall onto stone gets none of it.
void UOpenGLRenderDevice::DrawFallFoot( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet,
	DOUBLE T, FLOAT BaseUM, FLOAT BaseVM, UBOOL FoldLight, FLOAT LightUM, FLOAT LightVM )
{
	guard(UOpenGLRenderDevice::DrawFallFoot);
	if( !Facet.Bounds.IsValid || !Surface.Texture || !Frame->Level
	 || !Frame->Level->Model || !Frame->Level->GetLevelInfo() )
		return;
	static UBOOL NoFoot = ParseParam( appCmdLine(), "PROBENOFALLFOOT" );
	if( NoFoot )
		return;

	FVector WMin = Facet.Bounds.Min, WMax = Facet.Bounds.Max;
	FLOAT HX = WMax.X-WMin.X, HY = WMax.Y-WMin.Y, Height = Max( WMax.Z-WMin.Z, 1.f );

	// Where the foot actually IS. A sheet's bounding-box centre is only its
	// foot when the sheet is vertical; on a SLANTED one (SkyTown's fall drops
	// 1280 while running 825 x 704 sideways) the box centre is halfway up the
	// slope, which is inside the rock. Everything was being emitted and probed
	// there, which is why no foam ever appeared however bright it was made.
	//
	// So walk from the box centre DOWN-SLOPE to the box edge: the plane's
	// steepest-descent direction, flattened, is which way the foot lies.
	FVector NEye = Facet.MapCoords.ZAxis;
	FVector NW   = NEye.TransformVectorBy( Frame->Uncoords );
	FLOAT NWLen = NW.Size();
	if( NWLen > 0.0001f )
		NW /= NWLen;
	FVector Down( 0, 0, -1 );
	FVector Slope = Down - NW*(NW|Down);		// steepest descent, in the plane
	FVector Dh( Slope.X, Slope.Y, 0.f );		// ... flattened: toward the foot
	FLOAT DhLen = Dh.Size();
	FVector Along, Out;
	FLOAT Width;
	FVector Centre( (WMin.X+WMax.X)*0.5f, (WMin.Y+WMax.Y)*0.5f, WMin.Z );
	if( DhLen > 0.05f )
		Dh /= DhLen;
	// The foot line runs ALONG the sheet horizontally, which is the direction
	// perpendicular to its normal and to up -- always, whatever the sheet's
	// tilt. Taking it from the bounding box's longer axis instead was the axis
	// bug: a vertical sheet set at an angle to the world grid (SkyTown's runs
	// 208 x 481 in plan) has a DIAGONAL footprint, so snapping the foot to Y
	// laid the foam away from the viewer instead of across the fall's face.
	FVector AlongRaw = NW ^ FVector(0,0,1);
	FLOAT AL = AlongRaw.Size();
	if( AL > 0.001f )
	{
		Along = AlongRaw/AL;
		Out   = FVector( -Along.Y, Along.X, 0.f );	// horizontal, out of the face
		if( DhLen > 0.05f && (Out|Dh) < 0.f )
			Out = -Out;								// point it downhill
		// Extent of the box along the foot line: for a diagonal footprint this
		// is its true length, since only one axis pair can be spanned at once.
		Width = Max( Abs(Along.X)*HX + Abs(Along.Y)*HY, 16.f );
		if( DhLen > 0.05f )
			Centre += Dh*( 0.5f*( Abs(Dh.X)*HX + Abs(Dh.Y)*HY ) );
	}
	else
	{
		// Degenerate (a horizontal face): nothing sensible to run a line along.
		UBOOL AlongX = HX >= HY;
		Width = Max( AlongX ? HX : HY, 16.f );
		Along = AlongX ? FVector(1,0,0) : FVector(0,1,0);
		Out   = AlongX ? FVector(0,1,0) : FVector(1,0,0);
	}

	// Landing in water gets froth, wakes and spray ON the surface; falling into
	// open air gets none of those -- there is no surface -- but it does get
	// spray carrying on downward and thinning out, which is what stops the
	// sheet reading as a slab with a cut-off bottom.
	FLOAT SurfZ = WMin.Z;
	FVector Spread = Along*(Width*0.5f) + Out*32.f;
	UBOOL Lands = ZFallLandsInWater( Frame, Centre, Spread, WMin.Z, SurfZ );
	// Now that the waterline is known, put the foot EXACTLY where the sheet
	// meets it: the line where the sheet's own plane crosses z = SurfZ. The
	// approximation above (box centre pushed down-slope by the box's support
	// width) overshoots on a diagonal sheet, whose plan-view footprint is a
	// line rather than the whole box -- SkyTown's landed 64 units outside the
	// sheet and laid the foam along the wrong axis, so the foam spread away
	// from the viewer instead of across the fall's face.
	if( Lands && DhLen > 0.05f )
	{
		FVector P0w = Facet.MapCoords.Origin.TransformPointBy( Frame->Uncoords );
		FLOAT Denom = NW | Dh;
		if( Abs(Denom) > 0.02f )
		{
			FVector C( (WMin.X+WMax.X)*0.5f, (WMin.Y+WMax.Y)*0.5f, SurfZ );
			FLOAT t = -( NW | (C - P0w) )/Denom;
			FVector Hit = C + Dh*t;
			// Only trust it if it lands on the sheet's own footprint.
			if( Hit.X >= WMin.X-64.f && Hit.X <= WMax.X+64.f
			 && Hit.Y >= WMin.Y-64.f && Hit.Y <= WMax.Y+64.f )
			{
				Centre = Hit;
				Centre.Z = SurfZ;
				// The foot line runs ALONG the sheet horizontally; its length is
				// that line clipped to the sheet's box, not the box's support
				// width (which counts both extents and over-measures a diagonal).
				FLOAT S0 = -100000.f, S1 = 100000.f;
				for( INT ax=0; ax<2; ax++ )
				{
					FLOAT A  = ax ? Along.Y : Along.X;
					FLOAT Cc = ax ? Centre.Y : Centre.X;
					FLOAT Lo = ax ? WMin.Y : WMin.X;
					FLOAT Hi = ax ? WMax.Y : WMax.X;
					if( Abs(A) < 0.0001f )
						{ if( Cc<Lo || Cc>Hi ) { S0=0.f; S1=0.f; } continue; }
					FLOAT Ta = (Lo-Cc)/A, Tb = (Hi-Cc)/A;
					S0 = Max( S0, Min(Ta,Tb) );
					S1 = Min( S1, Max(Ta,Tb) );
				}
				if( S1 > S0 )
				{
					Centre += Along*( 0.5f*(S0+S1) );	// midpoint of the wetted line
					Width   = Max( S1-S0, 16.f );
				}
			}
		}
	}
	{
		static UBOOL DrawInfo = ParseParam( appCmdLine(), "PROBEFALLINFO" );
		if( DrawInfo )
			debugf( "FALLDRAW: box=(%.0f,%.0f,%.0f)..(%.0f,%.0f,%.0f) lands=%i surfZ=%.0f foot=(%.0f,%.0f) width=%.0f",
				WMin.X, WMin.Y, WMin.Z, WMax.X, WMax.Y, WMax.Z, (INT)Lands, SurfZ, Centre.X, Centre.Y, Width );
	}
	// How far the spray carries on below the sheet when there is nothing to
	// land on -- proportional to the fall, so a short spill trails a little and
	// a long one trails a lot.
	FLOAT Drop = Clamp( Height*0.7f, 64.f, 900.f );

	// Scale: everything keys off the fall's width, clamped so a 32-unit spout
	// and a 500-unit curtain both land somewhere sane.
	// Kept SMALL and dim on purpose: under the engine's additive translucency a
	// bright particle saturates to solid white, and once it does its soft
	// radial falloff is clipped away and it reads as a flat square. Many small
	// faint ones, never a few big bright ones.
	FLOAT R = Clamp( Width*0.05f, 4.f, 34.f );
	// Cadence: the same fall time the water itself takes, so froth churns and
	// wakes are born as water arrives.
	FLOAT FallTime = Max( 0.25f, Height/350.f );
	// How much the camera is looking DOWN on the pool. The effects that lie IN
	// the water plane are only legible from above -- seen from the waterline
	// they compress to hairlines and read as drawn stripes -- so they fade out
	// as the view flattens. (The airborne ones are camera-facing and don't
	// care.) World up resolved into eye space: its forward component is zero
	// looking level and one looking straight down.
	FLOAT FlatFade = Clamp( Abs( FVector(0,0,1).TransformVectorBy( Frame->Coords ).Z )*2.5f, 0.12f, 1.f );

	// All three sample the same small drifting window of the fall's own
	// animated texture; the shader's Blob profile makes the shape.
	FLOAT TexU = (FLOAT)Surface.Texture->USize, TexV = (FLOAT)Surface.Texture->VSize;
	FLOAT WinU = Min( 28.f, TexU ), WinV = Min( 28.f, TexV );
	FLOAT W0u = (FLOAT)fmod( T*11.0, (DOUBLE)Max( 1.f, TexU-WinU ) );
	FLOAT W0v = (FLOAT)fmod( T* 7.0, (DOUBLE)Max( 1.f, TexV-WinV ) );
	FLOAT SU0 = W0u*BaseUM, SU1 = (W0u+WinU)*BaseUM;
	FLOAT SV0 = W0v*BaseVM, SV1 = (W0v+WinV)*BaseVM;

	// Froth and spray are ALWAYS translucent, whatever the sheet is. On an
	// opaque fall the surface's own blend would draw every particle as a solid
	// square; the caller's next pass resets the blend anyway (CurrentBlendFlags
	// is poisoned below), so borrowing it here is safe.
	if( !(Surface.PolyFlags & PF_Translucent) )
	{
		SetBlend( (Surface.PolyFlags & ~(PF_Modulated|PF_Masked)) | PF_Translucent );
		CurrentBlendFlags = (DWORD)-1;
	}

	// Shared shader state: no strand shading, no warp, no grain on any of this.
	ZglUniform1f( WavyLocFall, 0.f );
	ZglUniform1f( WavyLocAmp, 0.f );
	ZglUniform1f( WavyLocGloss, 0.f );
	ZglUniform1f( WavyLocBase, 1.f );
	ZglUniform1f( WavyLocScroll, 0.f );
	ZglUniform1f( WavyLocFlowV, 1.f );
	ZglUniform2f( WavyLocUEdge, SU0, SU1 );
	ZglUniform2f( WavyLocVEdge, SV0, SV1 );

	// Light-map coordinate for a world point, projected through the sheet's own
	// mapping (as the fountain particles do): approximate off the sheet plane,
	// but it keeps the effects lit like the water they belong to.
	#define ZFOOTLIGHT(Pe) \
		if( FoldLight ) \
		{ \
			FLOAT LU = Facet.MapCoords.XAxis | ((Pe) - Facet.MapCoords.Origin); \
			FLOAT LV = Facet.MapCoords.YAxis | ((Pe) - Facet.MapCoords.Origin); \
			ZglMultiTexCoord2f( GL_TEXTURE1, \
				(LU-Surface.LightMap->Pan.X+0.5f*Surface.LightMap->UScale)*LightUM, \
				(LV-Surface.LightMap->Pan.Y+0.5f*Surface.LightMap->VScale)*LightVM ); \
		}

	// ---- WAKE: ripple bands running out from the line, both sides ----------
	ZglUniform1f( WavyLocBlob, 2.f );
	ZglUniform1f( WavyLocFoam, 0.40f );
	if( Lands )
	{
		const INT NBands = 3, NSeg = 10;
		FLOAT BandTime = FallTime*NBands;
		FLOAT BandPh = (FLOAT)( fmod( T, (DOUBLE)BandTime ) / BandTime );
		FLOAT L = Width*0.55f;		// half-length, a little past the sheet
		glBegin( GL_QUADS );
		for( INT r=0; r<NBands; r++ )
		{
			FLOAT Sr = (FLOAT)r/(FLOAT)NBands + BandPh;
			Sr -= appFloor(Sr);
			// Kept THICK and faint: seen from the waterline a thin band projects
			// to a one-pixel line, and a bright one reads as a drawn stripe
			// rather than a ripple.
			FLOAT Dist = R*( 0.8f + 7.0f*Sr );		// travelling outward
			FLOAT Half = R*( 0.55f + 0.85f*Sr );	// and widening
			FLOAT Ar = ZSmooth( 0.f, 0.12f, Sr )*( 1.f-ZSmooth( 0.20f, 1.f, Sr ) )*0.14f*FlatFade;
			if( Ar <= 0.002f )
				continue;
			glColor3f( Ar, Ar, Ar );
			for( INT s=-1; s<=1; s+=2 )
			{
				FLOAT D0 = s*(Dist-Half), D1 = s*(Dist+Half);
				for( INT g=0; g<NSeg; g++ )
				{
					FLOAT A0 = -L + (2.f*L*g)/NSeg, A1 = -L + (2.f*L*(g+1))/NSeg;
					FLOAT T0 = SV0 + (SV1-SV0)*((FLOAT)g/NSeg);
					FLOAT T1 = SV0 + (SV1-SV0)*((FLOAT)(g+1)/NSeg);
					FVector Q[4];
					Q[0] = Centre + Along*A0 + Out*D0;
					Q[1] = Centre + Along*A0 + Out*D1;
					Q[2] = Centre + Along*A1 + Out*D1;
					Q[3] = Centre + Along*A1 + Out*D0;
					FVector E[4];
					UBOOL Bad = 0;
					for( INT q=0; q<4; q++ )
					{
						Q[q].Z = SurfZ;
						E[q] = Q[q].TransformPointBy( Frame->Coords );
						if( E[q].Z < 1.f )
							Bad = 1;
					}
					if( Bad )
						continue;
					ZFOOTLIGHT(E[0]); glTexCoord2f( SU0, T0 ); glVertex3f( E[0].X, E[0].Y, E[0].Z );
					ZFOOTLIGHT(E[1]); glTexCoord2f( SU1, T0 ); glVertex3f( E[1].X, E[1].Y, E[1].Z );
					ZFOOTLIGHT(E[2]); glTexCoord2f( SU1, T1 ); glVertex3f( E[2].X, E[2].Y, E[2].Z );
					ZFOOTLIGHT(E[3]); glTexCoord2f( SU0, T1 ); glVertex3f( E[3].X, E[3].Y, E[3].Z );
				}
			}
		}
		glEnd();
		PolyCount++;
	}

	// ---- FROTH: white churn on the line, spreading outward ----------------
	ZglUniform1f( WavyLocBlob, 1.f );
	ZglUniform1f( WavyLocFoam, 1.f );
	if( Lands )
	{
		INT NFoam = Clamp( (INT)(Width/8.f) + 20, 24, 140 );
		FLOAT FoamTime = Max( 0.40f, FallTime );
		FLOAT FoamPh = (FLOAT)( fmod( T, (DOUBLE)FoamTime ) / FoamTime );
		glBegin( GL_QUADS );
		for( INT m=0; m<NFoam; m++ )
		{
			DWORD hm = (DWORD)(m+7)*2246822519u;
			DWORD g1 = hm ^ (hm>>13);
			DWORD g2 = (hm*3266489917u+374761393u); g2 ^= g2>>15;
			DWORD g3 = (hm*668265263u+1u);          g3 ^= g3>>17;
			FLOAT G1 = (g1 & 0xFFFFu)/65536.f;
			FLOAT G2 = (g2 & 0xFFFFu)/65536.f;
			FLOAT G3 = (g3 & 0xFFFFu)/65536.f;
			FLOAT Sf = G1 + FoamPh;
			Sf -= appFloor(Sf);
			// NOT faded by the view angle. The wake BANDS had to be, because
			// seen edge-on they collapse into drawn-looking stripes -- but
			// froth is blobs, and seen from the waterline they pile up into
			// exactly what a fall's foot should have there: a band of white
			// water. Fading these out was why there was no foam at eye level.
			FLOAT Af = ZSmooth( 0.f, 0.12f, Sf )*( 1.f-ZSmooth( 0.30f, 1.f, Sf ) )*( 0.16f + 0.18f*G2 );
			if( Af <= 0.002f )
				continue;
			FLOAT Side  = (g3 & 0x10000u) ? 1.f : -1.f;
			// Wanders ALONG the line as well as out from it, or the puffs land
			// in tidy rows and read as a checkerboard.
			FLOAT UPos  = (G2*2.f-1.f)*Width*0.5f + (G3*2.f-1.f)*R*1.5f*Sf;
			FLOAT Drift = R*( 0.2f + 1.8f*Sf*(0.4f+0.6f*G3) );
			FLOAT Rf    = R*( 0.35f + 0.40f*G3 )*( 0.6f + 0.8f*Sf );
			FVector Pw = Centre + Along*UPos + Out*(Side*Drift);
			Pw.Z = SurfZ;
			// Elliptical and turned, not an axis-aligned square: the quad's
			// own shape has to vary too, or the shader's wobble is the only
			// thing breaking up a field of identical outlines.
			FLOAT Ang = G1*2.f*PI, Ca = appCos(Ang), Sa = appSin(Ang);
			FLOAT AxA = Rf*( 0.75f + 0.75f*G2 ), AxB = Rf*( 0.70f + 0.60f*G3 );
			FVector Ea = Along*(Ca*AxA) + Out*(Sa*AxA);
			FVector Eb = Along*(-Sa*AxB) + Out*(Ca*AxB);
			FVector P0 = Pw - Ea - Eb, P1 = Pw + Ea - Eb;
			FVector P2 = Pw + Ea + Eb, P3 = Pw - Ea + Eb;
			FVector E0 = P0.TransformPointBy( Frame->Coords );
			FVector E1 = P1.TransformPointBy( Frame->Coords );
			FVector E2 = P2.TransformPointBy( Frame->Coords );
			FVector E3 = P3.TransformPointBy( Frame->Coords );
			if( E0.Z<1.f || E1.Z<1.f || E2.Z<1.f || E3.Z<1.f )
				continue;
			glColor3f( Af, Af, Af );
			ZFOOTLIGHT(E0); glTexCoord3f( SU0, SV0, G3 ); glVertex3f( E0.X, E0.Y, E0.Z );
			ZFOOTLIGHT(E1); glTexCoord3f( SU1, SV0, G3 ); glVertex3f( E1.X, E1.Y, E1.Z );
			ZFOOTLIGHT(E2); glTexCoord3f( SU1, SV1, G3 ); glVertex3f( E2.X, E2.Y, E2.Z );
			ZFOOTLIGHT(E3); glTexCoord3f( SU0, SV1, G3 ); glVertex3f( E3.X, E3.Y, E3.Z );
		}
		glEnd();
		PolyCount++;
	}

	// ---- MIST: the aerated haze standing over the impact ------------------
	// The flat-in-the-water effects above only read when you are looking DOWN
	// on the pool; from the waterline they compress to hairlines. What the eye
	// actually reads at a fall's foot from any angle is airborne mist, so this
	// is CAMERA-FACING: a low bank of large, very faint white puffs rising off
	// the line and dissipating. Faint and overlapping, never individually
	// legible -- it is the union of them that reads as froth.
	ZglUniform1f( WavyLocFoam, 1.f );
	{
		INT NMist = Clamp( (INT)(Width/5.f) + 30, 40, 220 );
		FLOAT MistTime = Max( 0.9f, FallTime*2.f );
		FLOAT MistPh = (FLOAT)( fmod( T, (DOUBLE)MistTime ) / MistTime );
		glBegin( GL_QUADS );
		for( INT m=0; m<NMist; m++ )
		{
			DWORD hm = (DWORD)(m+53)*2654435761u;
			DWORD g1 = hm ^ (hm>>14);
			DWORD g2 = (hm*1103515245u+12345u); g2 ^= g2>>16;
			DWORD g3 = (hm*668265263u+1u);      g3 ^= g3>>17;
			FLOAT G1 = (g1 & 0xFFFFu)/65536.f;
			FLOAT G2 = (g2 & 0xFFFFu)/65536.f;
			FLOAT G3 = (g3 & 0xFFFFu)/65536.f;
			FLOAT Sm = G1 + MistPh;
			Sm -= appFloor(Sm);
			// Mist is what carries the foot from the waterline, where the flat
			// effects are edge-on. At 0.03 it was invisible over bright water.
			FLOAT Am = ZSmooth( 0.f, 0.18f, Sm )*( 1.f-ZSmooth( 0.25f, 1.f, Sm ) )*( 0.075f + 0.10f*G2 );
			if( Am <= 0.002f )
				continue;
			FLOAT Side = (g3 & 0x10000u) ? 1.f : -1.f;
			FLOAT UPos = (G2*2.f-1.f)*Width*0.5f + (G3*2.f-1.f)*R*2.f*Sm;
			FVector Pw = Centre + Along*UPos + Out*( Side*R*(0.3f + 1.5f*Sm*(0.4f+0.6f*G3)) );
			// Landing in water, the mist RISES off the surface. Falling into
			// open air it goes on DOWN, thinning as it goes -- water leaving a
			// ledge does not stop at the bottom of the mapper's sheet.
			// Heights SPREAD either way, not a row: puffs at one altitude read
			// as a line of separate bubbles instead of a bank of mist.
			if( Lands )
				Pw.Z = SurfZ + R*( 0.1f + 3.4f*Sm*(0.25f+0.75f*G2) );
			else
				Pw.Z = WMin.Z - Drop*Sm*( 0.35f + 0.65f*G2 );
			FLOAT Rm = R*( 0.7f + 0.8f*G3 )*( 0.6f + 1.0f*Sm );	// swelling as it goes
			FVector Pp = Pw.TransformPointBy( Frame->Coords );
			if( Pp.Z < 1.f )
				continue;
			ZFOOTLIGHT(Pp);
			glColor3f( Am, Am, Am );
			// Wider than tall and turned, on top of the shader's ragged edge:
			// mist lies in flattened banks, it does not stack up in balls.
			FLOAT Ang = G1*2.f*PI, Ca = appCos(Ang), Sa = appSin(Ang);
			FLOAT AxA = Rm*( 0.9f + 0.6f*G2 ), AxB = Rm*( 0.55f + 0.35f*G3 );
			FLOAT AX = Ca*AxA, AY = Sa*AxA, BX = -Sa*AxB, BY = Ca*AxB;
			glTexCoord3f( SU0, SV0, G2 ); glVertex3f( Pp.X-AX-BX, Pp.Y-AY-BY, Pp.Z );
			glTexCoord3f( SU1, SV0, G2 ); glVertex3f( Pp.X+AX-BX, Pp.Y+AY-BY, Pp.Z );
			glTexCoord3f( SU1, SV1, G2 ); glVertex3f( Pp.X+AX+BX, Pp.Y+AY+BY, Pp.Z );
			glTexCoord3f( SU0, SV1, G2 ); glVertex3f( Pp.X-AX+BX, Pp.Y-AY+BY, Pp.Z );
		}
		glEnd();
		PolyCount++;
	}

	// ---- SPRAY: droplets thrown out of the impact ------------------------
	ZglUniform1f( WavyLocFoam, 0.35f );
	{
		INT NSpray = Clamp( (INT)(Width/6.f) + 24, 30, 160 );
		FLOAT FlyTime = Max( 0.45f, FallTime*1.3f );
		FLOAT FlyPh = (FLOAT)( fmod( T, (DOUBLE)FlyTime ) / FlyTime );
		glBegin( GL_QUADS );
		for( INT k=0; k<NSpray; k++ )
		{
			DWORD hk = (DWORD)(k+29)*2654435761u;
			DWORD h1 = hk ^ (hk>>15);
			DWORD h2 = (hk*1103515245u+12345u); h2 ^= h2>>16;
			DWORD h3 = (hk*22695477u+1u);       h3 ^= h3>>13;
			FLOAT H1 = (h1 & 0xFFFFu)/65536.f;
			FLOAT H2 = (h2 & 0xFFFFu)/65536.f;
			FLOAT H3 = (h3 & 0xFFFFu)/65536.f;
			FLOAT Sp = H1 + FlyPh;
			Sp -= appFloor(Sp);
			FLOAT Ap = ZSmooth( 0.f, 0.08f, Sp )*( 1.f-ZSmooth( 0.45f, 1.f, Sp ) )*( 0.28f + 0.30f*H2 );
			if( Ap <= 0.002f )
				continue;
			FLOAT Side = (h3 & 0x10000u) ? 1.f : -1.f;
			FLOAT UPos = (H2*2.f-1.f)*Width*0.5f + (H1*2.f-1.f)*R;
			FVector Pw = Centre + Along*UPos + Out*( Side*R*(0.15f + 1.6f*Sp*(0.4f+0.6f*H3)) );
			FLOAT Rd = R*( 0.10f + 0.11f*H3 );
			if( Lands )
			{
				// Thrown out of the impact: a ballistic arc, up and out, falling
				// back. Rise peaks mid-life.
				FLOAT Rise = 4.f*Sp*(1.f-Sp);
				Pw.Z = SurfZ + Rise*R*( 0.8f + 1.4f*H2 );
			}
			else
			{
				// Nothing to hit: the water keeps going, accelerating and
				// pulling apart, until it is too thin to see.
				FLOAT FallS = Sp*Sp*0.75f + Sp*0.25f;
				Pw.Z = WMin.Z - Drop*FallS*( 0.5f + 0.5f*H2 );
				Rd  *= 0.7f + 0.8f*FallS;
			}
			FVector Pp = Pw.TransformPointBy( Frame->Coords );
			if( Pp.Z < 1.f )
				continue;
			ZFOOTLIGHT(Pp);
			glColor3f( Ap, Ap, Ap );
			// Camera-facing, stretched a little along screen-vertical so a
			// droplet reads as moving rather than hanging.
			FLOAT LY = Rd*1.7f, WX = Rd;
			glTexCoord3f( SU0, SV0, H3 ); glVertex3f( Pp.X-WX, Pp.Y-LY, Pp.Z );
			glTexCoord3f( SU1, SV0, H3 ); glVertex3f( Pp.X+WX, Pp.Y-LY, Pp.Z );
			glTexCoord3f( SU1, SV1, H3 ); glVertex3f( Pp.X+WX, Pp.Y+LY, Pp.Z );
			glTexCoord3f( SU0, SV1, H3 ); glVertex3f( Pp.X-WX, Pp.Y+LY, Pp.Z );
		}
		glEnd();
		PolyCount++;
	}

	ZglUniform1f( WavyLocFoam, 0.f );
	ZglUniform1f( WavyLocBlob, 0.f );
	glColor3f( 1.f, 1.f, 1.f );
	#undef ZFOOTLIGHT
	unguard;
}

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

	// Inside the sky box (see SetSceneNode): its CLOUD SHEETS churn -- the one
	// sky treatment that survived: a sheet of clouds sliding rigidly across is
	// the clearest tell that a sky is a painted box, and the same per-pixel
	// warp that reads as water at water speed reads as weather at a fraction of
	// it. Nothing else about the sky is changed; it draws as stock.
	UBOOL SkyLook = SkyFrame != 0;
	static UBOOL NoChurn = ParseParam( appCmdLine(), "PROBENOCHURN" );
	UBOOL SkyCloud = SkyLook && !NoChurn
	              && (Surface.PolyFlags & (PF_AutoUPan|PF_AutoVPan))!=0;

	// Pass 1: base texture (or flat color). PF_SmallWavy/PF_BigWavy -- and
	// translucent auto-panning surfaces, i.e. flowing water -- bind the
	// per-pixel warp program for this pass only (see InitWavyProgram): same
	// fan, same depth, only the texture lookup undulates.
	//
	// x64 port: pin the vertex colour to white. Other passes (fall-foot
	// particles, coronas) leave their last glColor behind, and the base fan
	// here inherits whatever that was -- caught by the -probeglstate readback
	// as the first modulated surface of a frame drawing with glColor 0.5,
	// i.e. at half brightness for one frame.
	glColor3f( 1.f, 1.f, 1.f );
	SetBlend( Surface.PolyFlags );
	UBOOL LightDone = 0;	// light map folded into the base pass below
	if( Surface.Texture )
	{
		UBOOL Translucent = (Surface.PolyFlags & PF_Translucent)!=0 && !(Surface.PolyFlags & PF_Modulated);
		UBOOL WavyFlags   = (Surface.PolyFlags & (PF_SmallWavy|PF_BigWavy))!=0;
		// Flow warp only in the root frame: sky boxes layer translucent
		// auto-panning cloud sheets, and those must keep sliding rigidly.
		UBOOL Flowing = Translucent && Frame->Parent==NULL && !WavyFlags
		             && (Surface.PolyFlags & (PF_AutoUPan|PF_AutoVPan))!=0;
		// A liquid sheet standing UP is a WATERFALL, not a pool. Sliding a
		// static picture down it was all a 1998 renderer could do, and treating
		// it as a pool afterwards only adds ripples: it reads as a pane of
		// glass hung in front of the rock. Recognise the standing sheet and
		// shade it as falling water instead (the Fall path in InitWavyProgram).
		//
		// The test is GEOMETRY plus liquid-ness, and deliberately nothing else.
		// Keying it on the auto-pan flag missed SkyTown's falls, which carry no
		// pan at all; requiring PF_Translucent missed them too, since that map's
		// water is opaque. What is left: it is wavy (mapper-flagged, or a
		// WaterTexture that Render flags for them), it is tall, and it STANDS.
		// Root frame only -- sky boxes hang auto-panning sheets of their own.
		UBOOL Falling = WavyFlags && Frame->Parent==NULL && Facet.Bounds.IsValid;
		if( Falling )
		{
			// Standing = the surface's own plane is near-vertical, so its
			// normal lies near the horizontal. Taken off the NORMAL rather than
			// the bounding box: SkyTown's fall is a slanted chute, 825 x 704 x
			// 1280, and a box test calls that a pool. MapCoords is in eye space,
			// so bring world up along to compare with.
			FLOAT HZ = Facet.Bounds.Max.Z-Facet.Bounds.Min.Z;
			FVector UpEye = FVector(0,0,1).TransformVectorBy( Frame->Coords );
			FLOAT NL = Facet.MapCoords.ZAxis.Size();
			FLOAT NZ = NL>0.0001f ? Abs( (UpEye | Facet.MapCoords.ZAxis)/NL ) : 1.f;
			Falling = HZ >= 64.f && NZ <= 0.6f;
		}
		// An OPAQUE fall cannot use the shaping that assumes additive
		// translucency: there, dark means see-through, so the edge fades and
		// the strand gate open the sheet up. On an opaque surface the same
		// arithmetic just paints it black. It keeps the strands and the foot.
		UBOOL FallSolid = Falling && !Translucent;
		// -probenofall shades falls as the pools they used to be taken for:
		// the A/B for "is this sheet better as falling water or not".
		static UBOOL NoFall = ParseParam( appCmdLine(), "PROBENOFALL" );
		if( NoFall )
			Falling = 0;
		// Water look (glints, base fade, livelier time) is for translucent
		// LIQUID surfaces; a merely lit-translucent sheet (below) gets none.
		UBOOL WaterFX = Flowing || (WavyFlags && Translucent);
		// The shader serves warped surfaces AND lit translucent ones. For the
		// latter it multiplies the base texel by its own 2x light map in this
		// single pass (unit 1) instead of the Pass-2 framebuffer modulate:
		// DST_COLOR modulate over an additive-translucent base darkens
		// everything seen THROUGH the surface by the surface's lighting -- a
		// dim-lit waterfall rendered as a dark glassy slab over the scene
		// behind it. Software-renderer translucency lit the texel, never the
		// backdrop.
		UBOOL Wavy = Flowing || WavyFlags || SkyCloud || (Translucent && Surface.LightMap);
		if( Wavy && !WavyTried )
			InitWavyProgram();
		Wavy = Wavy && WavyProgram!=0;
		UBOOL FoldLight = Wavy && Translucent && Surface.LightMap
		               && ZglActiveTexture && ZglMultiTexCoord2f;
		if( !WavyFlags && !Flowing && !SkyCloud && !FoldLight )
			Wavy = 0;	// lit-translucent was the only reason, and it needs multitexture

		// Fountain columns: mappers author a pour as several translucent
		// sheets -- crossed, or the four walls of a box -- so no per-face
		// treatment can avoid reading as a glass block, and particles put in
		// ONE face's plane pour down the side of the column instead of
		// through it. Each such face therefore only contributes its bounds to
		// a world-space column box (see StreamCols) and never draws itself;
		// one face per column per frame draws the whole pour, filling the
		// accumulated volume. Resolved before any GL binding so the other
		// faces can skip the draw entirely.
		UBOOL StreamLook = 0;
		FStreamColumn* Col = NULL;
		if( FoldLight && !WavyFlags && !Flowing && Facet.Bounds.IsValid )
		{
			// The surface's TRUE world bounds (see FSurfaceFacet::Bounds).
			// Never the clipped polys: those describe only the part of the
			// sheet currently on screen, so the emitter volume shrank -- and
			// the pour vanished -- as the fountain left frame.
			FVector WMin = Facet.Bounds.Min, WMax = Facet.Bounds.Max;
			// A pour is narrow in BOTH horizontal axes. That rules out pool
			// surfaces (both axes large) and lit glass or force-field walls
			// (thin one way, wide the other), view-independently.
			FLOAT HX = WMax.X-WMin.X, HY = WMax.Y-WMin.Y, HZ = WMax.Z-WMin.Z;
			if( Max( HX, HY ) <= 96.f )
			{
				FVector FC = (WMin+WMax)*0.5f;
				for( INT c=0; c<NumStreamCols; c++ )
				{
					FStreamColumn& E = StreamCols[c];
					if( FC.X > E.WMin.X-48.f && FC.X < E.WMax.X+48.f
					 && FC.Y > E.WMin.Y-48.f && FC.Y < E.WMax.Y+48.f
					 && FC.Z > E.WMin.Z-48.f && FC.Z < E.WMax.Z+48.f )
						{ Col = &E; break; }
				}
				if( HZ > 0.f )
				{
					// A stream face: tall enough to pour. Registers a column.
					StreamLook = 1;
					if( !Col && NumStreamCols < MAX_STREAM_COLS )
					{
						Col = &StreamCols[NumStreamCols++];
						Col->WMin = WMin;
						Col->WMax = WMax;
						Col->Stamp = -1;
						Col->StampFrame = NULL;
					}
					if( Col )
					{
						Col->WMin.X = Min( Col->WMin.X, WMin.X ); Col->WMax.X = Max( Col->WMax.X, WMax.X );
						Col->WMin.Y = Min( Col->WMin.Y, WMin.Y ); Col->WMax.Y = Max( Col->WMax.Y, WMax.Y );
						Col->WMin.Z = Min( Col->WMin.Z, WMin.Z ); Col->WMax.Z = Max( Col->WMax.Z, WMax.Z );
					}
					else StreamLook = 0;	// registry full: draw the sheet as-is
				}
				else if( Col )
				{
					// A FLAT face (zero height) sitting at a pour we already
					// know about: the splash cap mappers lay in the water to
					// hide the moment of impact. Its authored form is a hard-
					// edged pane, and once the pour became particles it was
					// left standing as a bright rectangle in the water at the
					// centre of the ripples. Suppress it -- the froth and
					// ripples play that part now.
					//
					// Only ever JOINS an existing column, never creates one: a
					// small flat lit-translucent sheet on its own is as likely
					// to be a light pool or a pane of glass lying in a frame.
					// And it extends the column only in Z, down to the water
					// it marks, because a cap can be much broader than the
					// stream it belongs to -- sizing the pour off it would
					// spread the droplets into a curtain.
					StreamLook = 1;
					Col->WMin.Z = Min( Col->WMin.Z, WMin.Z );
					Col->WMax.Z = Max( Col->WMax.Z, WMax.Z );
				}
				if( Col && StreamLook )
				{
					if( Col->Stamp==FrameStamp && Col->StampFrame==(void*)Frame )
						return;		// this column's pour is already drawn this frame
					Col->Stamp = FrameStamp;
					Col->StampFrame = (void*)Frame;
				}
			}
			// -probefountain: report every candidate that ends up drawing as
			// its authored self -- too wide to be a stream, or flat with no
			// pour to belong to. A stray hard-edged pane at a fountain is
			// always one of these (reported once per distinct box, not once
			// per frame).
			if( !StreamLook )
			{
				static UBOOL FountDbg2 = ParseParam( appCmdLine(), "PROBEFOUNTAIN" );
				if( FountDbg2 )
				{
					static FVector LastMin(0,0,0), LastMax(0,0,0);
					static INT LastStamp = -1;
					if( WMin!=LastMin || WMax!=LastMax || FrameStamp!=LastStamp )
					{
						LastMin = WMin;
						LastMax = WMax;
						LastStamp = FrameStamp;
						debugf( "FOUNTAIN REJECT: frame=%i tex=%ix%i box=(%.0f,%.0f,%.0f)..(%.0f,%.0f,%.0f) HX=%.0f HY=%.0f HZ=%.0f",
							FrameStamp, Surface.Texture->USize, Surface.Texture->VSize,
							WMin.X, WMin.Y, WMin.Z, WMax.X, WMax.Y, WMax.Z,
							HX, HY, HZ );
					}
				}
			}
		}

		SetTexture( *Surface.Texture, Surface.PolyFlags, 0 );
		// -probeglstate: read the ACTUAL GL state back for modulated surfaces at
		// the moment they draw, instead of reasoning about what it should be.
		// Written after three texture-side fixes to the WaterRings2 pale box all
		// failed the flat-neutral test -- the scale factor is in the pipeline
		// state somewhere, and this prints the pipeline's own account of itself.
		{
			static UBOOL GLStateInfo = ParseParam( appCmdLine(), "PROBEGLSTATE" );
			static INT GLStateLogged = 0;
			if( GLStateInfo && (Surface.PolyFlags & PF_Modulated) && GLStateLogged<4 )
			{
				GLStateLogged++;
				GLint BlendOn=0, SrcF=0, DstF=0, Prog=0, Tex=0, Env=0;
				GLfloat Col[4]={0,0,0,0};
				glGetIntegerv( GL_BLEND, &BlendOn );
				glGetIntegerv( GL_BLEND_SRC, &SrcF );
				glGetIntegerv( GL_BLEND_DST, &DstF );
				glGetIntegerv( 0x8B8D /*GL_CURRENT_PROGRAM*/, &Prog );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &Tex );
				glGetTexEnviv( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &Env );
				glGetFloatv( GL_CURRENT_COLOR, Col );
				debugf( NAME_Log, "GLSTATE modulated: blend=%i src=%04X dst=%04X prog=%i tex=%i env=%04X color=(%.2f,%.2f,%.2f,%.2f)",
					BlendOn, SrcF, DstF, Prog, Tex, Env, Col[0], Col[1], Col[2], Col[3] );
				// And what is actually IN the bound texture: read back the top
				// mip's centre row and report its min/mean/max. If the upload
				// path corrected this texture, that shows here; if the box
				// remains anyway, the scaling is downstream of the texture.
				GLint W=0, H=0;
				glGetTexLevelParameteriv( GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &W );
				glGetTexLevelParameteriv( GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &H );
				if( W>0 && W<=1024 && H>0 )
				{
					BYTE* Pix = (BYTE*)appMalloc( W*H*4, "GLStateProbe" );
					glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, Pix );
					INT Mn=255, Mx=0; DOUBLE Sum=0;
					BYTE* Row = Pix + (H/2)*W*4;
					for( INT x=0; x<W; x++ )
					{
						INT L = ( Row[x*4+0] + Row[x*4+1] + Row[x*4+2] + 1 ) / 3;
						Sum += L; Mn = Min(Mn,L); Mx = Max(Mx,L);
					}
					debugf( NAME_Log, "GLSTATE   bound tex %ix%i centre row: min=%i mean=%.1f max=%i (modulate no-op=127.5)",
						W, H, Mn, Sum/W, Mx );
					appFree( Pix );
				}
			}
		}
		FLOAT BaseUM = UMult, BaseVM = VMult;
		FLOAT LightUM = 0.f, LightVM = 0.f;
		if( FoldLight )
		{
			// Bind the light map on unit 1 (SetTexture binds on the active
			// unit and handles dynamic-light re-uploads). The bind cache then
			// describes unit 1, so poison it: the next SetTexture must rebind
			// on unit 0 even if the cache id happens to match.
			ZglActiveTexture( GL_TEXTURE1 );
			SetTexture( *Surface.LightMap, 0, 1 );
			LightUM = UMult;
			LightVM = VMult;
			ZglActiveTexture( GL_TEXTURE0 );
			CurrentTextureID = 0;
			LightDone = 1;
		}
		DOUBLE T = 0.0;
		if( Wavy )
		{
			// Wrap time at the waves' common period (every rate, including
			// the water-speed 1.5x variants, is a multiple of 0.05 rad/s ->
			// period 2pi/0.05) so the shader's float phase stays small and
			// continuous forever -- raw TimeSeconds from a long-played save
			// quantizes sin() into visible per-frame jumps.
			ALevelInfo* Info = Frame->Level ? Frame->Level->GetLevelInfo() : NULL;
			T = Info ? (DOUBLE)Info->TimeSeconds : 0.0;
			ZglUseProgram( WavyProgram );
			// Clouds evolve over half a minute, not half a second, so their
			// time runs at an eighth. The wrap has to grow by the same factor to
			// stay on a whole number of wave periods (every rate is a multiple
			// of 0.05 rad/s, so 8 x 2pi/0.05 is still a multiple of 2pi).
			ZglUniform1f( WavyLocTime, SkyCloud
				? (FLOAT)fmod( T*0.125, 1005.3096491487338 )
				: (FLOAT)fmod( WaterFX ? T*1.5 : T, 125.66370614359172 ) );
			// A lit translucent surface that is not a liquid by any other test
			// is here for ONE reason: to have its light map folded into this
			// pass (see FoldLight). It must not be animated at all. The old
			// catch-all gave everything that landed here a 1.5-texel sway,
			// meant for fountain streams pouring from statues -- but the test
			// is just "translucent and lit", which is precisely what a pane of
			// GLASS is, so every window in the game rippled.
			UBOOL LitOnly = !WavyFlags && !Flowing && !SkyCloud;
			// Flowing surfaces warp gently (the pan supplies the motion) and
			// wavy pools use the tuned amplitudes. Cloud sheets churn WIDE -- a
			// whole cloud's worth of texels, where water ripples a few.
			ZglUniform1f( WavyLocAmp, SkyCloud ? 9.f : Falling ? 2.f : Flowing ? 2.5f
				: WavyFlags ? ((Surface.PolyFlags & PF_BigWavy) ? 7.f : 3.5f) : 0.f );
			ZglUniform2f( WavyLocUVMult, BaseUM, BaseVM );
			// A fall gets no pool glint (its slope field is the strand field,
			// not a wave) and no base fade: the strand gate below decides what
			// is water and what is a gap through it. Clouds get neither either.
			// MODULATED surfaces get neither glint nor base fade, whatever else
			// they are. Their texels are blend FACTORS (2*src*dst), not colours:
			// the no-op is exactly 0.5, and ANY shading applied to them tints the
			// framebuffer instead of lighting the surface. Gloss rides the wave
			// slope by +-0.15, which on a modulate decal is +-7.5% brightness
			// smeared over the whole quad -- and since the quad is a mapper's
			// brush, that reads as a hard-edged pale BOX. NyLeve's WaterRings2
			// ripple decal is a WaveTexture, so Render tags it PF_SmallWavy and it
			// arrives here as "a liquid surface"; it is a decal, and the shading a
			// pool wants is exactly what it must not have. Proved by forcing the
			// texture to a flat neutral 128, which must be invisible under this
			// blend: the box did not change at all, so it was never the texture.
			// The WARP still applies -- displacing the rings is the point of it.
			UBOOL ModDecal = (Surface.PolyFlags & PF_Modulated)!=0;
			{
				static UBOOL PassInfo = ParseParam( appCmdLine(), "PROBEPASSINFO" );
				static INT PassLogged = 0;
				if( PassInfo && ModDecal && PassLogged<6 )
				{
					PassLogged++;
					debugf( NAME_Log, "PASSPROBE modulated surf flags=%08X lightmap=%i detail=%i macro=%i fog=%i bump=%i",
						Surface.PolyFlags, Surface.LightMap!=NULL, Surface.DetailTexture!=NULL,
						Surface.MacroTexture!=NULL, Surface.FogMap!=NULL, Surface.BumpMap!=NULL );
				}
			}
			// LitOnly gets no glint either. The glint rides the same sinusoids
			// as the warp but is NOT scaled by Amp, so zeroing the amplitude
			// alone still left glass shimmering by +-7.5% in a travelling
			// pattern -- the rippling would have looked half fixed.
			ZglUniform1f( WavyLocGloss, (Falling || SkyCloud || ModDecal || LitOnly) ? 0.f : WaterFX ? 0.22f : 0.15f );
			ZglUniform1f( WavyLocBase,  (Falling || SkyCloud || ModDecal) ? 1.f : WaterFX ? 0.88f : 1.f );
			ZglUniform1f( WavyLocLightOn, FoldLight ? 1.f : 0.f );
			// Ordinary surfaces: no stream shaping. (The fountain path sets
			// its own uniforms right before drawing, below.)
			ZglUniform2f( WavyLocUEdge, 0.f, 0.f );
			ZglUniform2f( WavyLocVEdge, 0.f, 0.f );
			ZglUniform1f( WavyLocFlowV, 1.f );
			ZglUniform1f( WavyLocScroll, 0.f );
			ZglUniform1f( WavyLocBlob, 0.f );
			ZglUniform1f( WavyLocFoam, 0.f );
			ZglUniform1f( WavyLocFall, 0.f );
			ZglUniform1f( WavyLocFallDn, 1.f );
			ZglUniform1f( WavyLocFallFd, 0.f );
			ZglUniform1f( WavyLocFallSol, 0.f );
			if( Falling )
			{
				// Which way is DOWN on this face, in the texture's own axes:
				// the world down vector resolved onto them (both are in eye
				// space here, so bring down along too). Falls are authored with
				// one texture axis running down the face, so the dominant one is
				// the flow axis and its sign says which end is the foot.
				FVector DownEye = FVector(0,0,-1).TransformVectorBy( Frame->Coords );
				FLOAT DU = Facet.MapCoords.XAxis | DownEye;
				FLOAT DV = Facet.MapCoords.YAxis | DownEye;
				FLOAT SU = Facet.MapCoords.XAxis.Size(), SV = Facet.MapCoords.YAxis.Size();
				FLOAT CU = SU>0.0001f ? Abs(DU)/SU : 0.f;	// |cos| to the U axis
				FLOAT CV = SV>0.0001f ? Abs(DV)/SV : 0.f;
				UBOOL AlongV = CV >= CU;
				FLOAT Down = AlongV ? DV : DU;

				// Texel extents of the WHOLE surface, from its unclipped world
				// box (the polys handed to us are clipped to the view, so their
				// extent would shrink as the fall leaves frame and the edge
				// fades would crawl across it). A rigid transform, so the texel
				// values a box corner maps to do not depend on the camera.
				FLOAT UMin=99999999.f, UMax=-99999999.f, VMin=99999999.f, VMax=-99999999.f;
				for( INT c=0; c<8; c++ )
				{
					FVector W( (c&1) ? Facet.Bounds.Max.X : Facet.Bounds.Min.X,
					           (c&2) ? Facet.Bounds.Max.Y : Facet.Bounds.Min.Y,
					           (c&4) ? Facet.Bounds.Max.Z : Facet.Bounds.Min.Z );
					FVector E = W.TransformPointBy( Frame->Coords ) - Facet.MapCoords.Origin;
					FLOAT U = Facet.MapCoords.XAxis | E;
					FLOAT V = Facet.MapCoords.YAxis | E;
					UMin = Min(UMin,U); UMax = Max(UMax,U);
					VMin = Min(VMin,V); VMax = Max(VMax,V);
				}
				// Same texcoord units the vertices carry, pan included.
				ZglUniform2f( WavyLocUEdge, (UMin-Surface.Texture->Pan.X)*BaseUM, (UMax-Surface.Texture->Pan.X)*BaseUM );
				ZglUniform2f( WavyLocVEdge, (VMin-Surface.Texture->Pan.Y)*BaseVM, (VMax-Surface.Texture->Pan.Y)*BaseVM );
				ZglUniform1f( WavyLocFlowV, AlongV ? 1.f : 0.f );
				ZglUniform1f( WavyLocBlob, 3.f );
				ZglUniform1f( WavyLocFall, 1.f );
				ZglUniform1f( WavyLocFallDn, Down >= 0.f ? 1.f : -1.f );
				ZglUniform1f( WavyLocFallSol, FallSolid ? 1.f : 0.f );
				// Does it land in anything? A fall spilling off a ledge into a
				// sky box lands in NOTHING, and must dissolve well before the
				// sheet's bottom edge rather than being brightened at it.
				FLOAT FootZ = 0.f;
				FVector FootMid( (Facet.Bounds.Min.X+Facet.Bounds.Max.X)*0.5f,
				                 (Facet.Bounds.Min.Y+Facet.Bounds.Max.Y)*0.5f, Facet.Bounds.Min.Z );
				ZglUniform1f( WavyLocFallFd,
					ZFallLandsInWater( Frame, FootMid,
						FVector( Max( (Facet.Bounds.Max.X-Facet.Bounds.Min.X)*0.5f, 24.f ),
						         Max( (Facet.Bounds.Max.Y-Facet.Bounds.Min.Y)*0.5f, 24.f ), 0.f ),
						Facet.Bounds.Min.Z, FootZ ) ? 0.f : 1.f );
				// Speed. The zone's auto-pan is 35 texels/s, tuned for sliding
				// clouds, and water falling a room's height at that rate reads
				// as a picture creeping downward. Scroll the lookup along the
				// fall on top of it, wrapped at the texture's own size along
				// that axis so the wrap lands exactly on a repeat.
				FLOAT TexSz = (FLOAT)Max( 1, AlongV ? Surface.Texture->VSize : Surface.Texture->USize );
				FLOAT Mult  = AlongV ? BaseVM : BaseUM;
				FLOAT Slide = (FLOAT)fmod( T*130.0, (DOUBLE)TexSz )*Mult;
				ZglUniform1f( WavyLocScroll, Down >= 0.f ? Slide : -Slide );
			}
		}
		glColor3f( 1.f, 1.f, 1.f );
		if( StreamLook && Col )
		{
			// Fountain PARTICLES. The authored sheets are only an emitter
			// VOLUME (Col's accumulated world box); what draws is a
			// procedural pour filling that volume's full cross-section --
			// camera-facing soft droplets falling at ~350 uu/s under
			// gravity, spreading as they go, fading in at the spout and out
			// at the water. Positions are pure functions of level time: no
			// state, so saves, demos and paused frames are consistent.
			// (OldUnreal 227 replaced these fountains with emitters in
			// CONTENT; doing it in the driver covers stock maps untouched.)
			FVector WC   = (Col->WMin + Col->WMax)*0.5f;
			FVector WExt = (Col->WMax - Col->WMin)*0.5f;	// world half-extents

			// Fall along the zone's GRAVITY, not an assumed world -Z, and
			// build an orthonormal frame around it: droplets then travel the
			// way everything else in the zone falls (low-grav zones and
			// custom ZoneGravity included).
			FVector GDir( 0, 0, -1 );
			if( Frame->Level && Frame->Level->Model && Frame->Level->GetLevelInfo() )
			{
				AZoneInfo* GZone = Frame->Level->Model->PointRegion( Frame->Level->GetLevelInfo(), WC ).Zone;
				if( GZone )
				{
					FLOAT GS = GZone->ZoneGravity.Size();
					if( GS > 0.001f )
						GDir = GZone->ZoneGravity/GS;
				}
			}
			FVector Helper = ( Abs(GDir.Z) < 0.9f ) ? FVector(0,0,1) : FVector(1,0,0);
			FVector S1 = GDir ^ Helper;
			FLOAT S1S = S1.Size();
			S1 = S1S>0.001f ? S1/S1S : FVector(1,0,0);
			FVector S2 = GDir ^ S1;
			// Box extents resolved onto that frame.
			FLOAT ExtA = Abs(GDir.X)*WExt.X + Abs(GDir.Y)*WExt.Y + Abs(GDir.Z)*WExt.Z;
			FLOAT Rad1 = Max( Abs(S1.X)*WExt.X + Abs(S1.Y)*WExt.Y + Abs(S1.Z)*WExt.Z, 1.f );
			FLOAT Rad2 = Max( Abs(S2.X)*WExt.X + Abs(S2.Y)*WExt.Y + Abs(S2.Z)*WExt.Z, 1.f );
			FLOAT Height = 2.f*ExtA;
			// Droplet size follows the column's VISIBLE width, not its
			// thinnest axis: these volumes are often thin slabs (the Ceremony
			// fountain is 12x4 units), and sizing off the 4 would draw
			// two-pixel specks.
			FLOAT RadRef = Max( Max( Rad1, Rad2 ), 1.f );
			FLOAT RadMin = Max( Min( Rad1, Rad2 ), 1.f );
			FVector Spout = WC - GDir*ExtA;		// where the pour starts
			// Squat volumes are the SPLASH where a pour lands, not the pour.
			UBOOL Splash = ExtA < 1.2f*Max( Rad1, Rad2 );
			// Gravity in eye space: droplets are stretched along it so they
			// read as streaking downward rather than hanging as round dots.
			FVector GEyeDir = GDir.TransformVectorBy( Frame->Coords );

			// Every droplet samples the same small window of the animated
			// waterfall texture (drifting for variety); the shader's Blob
			// profile rounds it into a droplet. Motion replaces warp/scroll.
			FLOAT TexU = (FLOAT)Surface.Texture->USize, TexV = (FLOAT)Surface.Texture->VSize;
			FLOAT WinU = Min( 28.f, TexU ), WinV = Min( 28.f, TexV );
			FLOAT W0u = (FLOAT)fmod( T*11.0, (DOUBLE)Max( 1.f, TexU-WinU ) );
			FLOAT W0v = (FLOAT)fmod( T* 7.0, (DOUBLE)Max( 1.f, TexV-WinV ) );
			FLOAT SU0 = W0u*BaseUM, SU1 = (W0u+WinU)*BaseUM;
			FLOAT SV0 = W0v*BaseVM, SV1 = (W0v+WinV)*BaseVM;
			ZglUniform2f( WavyLocUEdge, SU0, SU1 );
			ZglUniform2f( WavyLocVEdge, SV0, SV1 );
			ZglUniform1f( WavyLocBlob, 1.f );
			ZglUniform1f( WavyLocAmp, 0.f );
			ZglUniform1f( WavyLocScroll, 0.f );
			ZglUniform1f( WavyLocGloss, 0.f );

			// Dense enough that droplets OVERLAP near the spout (a coherent
			// pour) and separate into droplets as the spread grows.
			// Legible count, NOT maximum density: hundreds of overlapping
			// random droplets are statistically stationary -- every frame
			// looks the same (measured frame-to-frame r=0.98), so the pour
			// reads as fixed static however fast the droplets really move.
			// Water reads as falling when the eye can follow individual
			// strands, so draw fewer, longer, brighter ones.
			INT K = Splash ? 140 : Clamp( (INT)(Height/2.2f) + 40, 60, 160 );
			// -probefountainoff draws nothing for these columns: whatever is
			// still on screen at a fountain is then some OTHER primitive.
			static UBOOL FountOff = ParseParam( appCmdLine(), "PROBEFOUNTAINOFF" );
			if( FountOff )
				K = 0;
			FLOAT FallTime = Splash ? 0.55f : Max( 0.15f, Height/350.f );
			FLOAT Phase = (FLOAT)( fmod( T, (DOUBLE)FallTime ) / FallTime );
			// -probefountain: is the pour actually advancing? (T frozen or a
			// degenerate box both read on screen as "animated but static".)
			static UBOOL FountDbg = ParseParam( appCmdLine(), "PROBEFOUNTAIN" );
			if( FountDbg )
			{
				static INT FountEvery = 0;
				if( ((FountEvery++) % 4)==0 && !Splash )
				{
					// Particle 0's phase and world Z: if these do not move,
					// the pour is frozen no matter what the shader does.
					DWORD hk0 = 0*2654435761u; DWORD h10 = hk0 ^ (hk0>>15);
					FLOAT H10 = (h10 & 0xFFFFu)/65536.f;
					FLOAT S0 = H10 + Phase; S0 -= appFloor(S0);
					FLOAT F0 = S0*S0*0.75f + S0*0.25f;
					debugf( "FOUNTAIN: T=%.4f phase=%.4f p0.S=%.4f p0.Z=%.2f height=%.1f K=%i box=(%.0f,%.0f,%.0f)..(%.0f,%.0f,%.0f)",
						T, Phase, S0, Spout.Z + GDir.Z*(F0*Height), Height, K,
						Col->WMin.X, Col->WMin.Y, Col->WMin.Z, Col->WMax.X, Col->WMax.Y, Col->WMax.Z );
				}
			}
			glBegin( GL_QUADS );
			for( INT k=0; k<K; k++ )
			{
				// Per-droplet constants: spawn phase, lateral offsets, size.
				// SCATTERED hashes, not a low-discrepancy ladder: with evenly
				// spaced phases each droplet is replaced by its neighbour at
				// the very position it vacated, so the pattern is invariant
				// under time and the pour shimmers in place instead of
				// visibly flowing (measured: frame-to-frame r=0.98 at zero
				// displacement). Irregular phases let the eye lock onto
				// individual droplets and read the motion.
				DWORD hk = (DWORD)k*2654435761u;
				DWORD h1 = hk ^ (hk>>15);
				DWORD h2 = (hk*1103515245u+12345u); h2 ^= h2>>16;
				DWORD h3 = (hk*22695477u+1u);       h3 ^= h3>>13;
				FLOAT H1 = (h1 & 0xFFFFu)/65536.f;
				FLOAT H2 = (h2 & 0xFFFFu)/65536.f;
				FLOAT H3 = (h3 & 0xFFFFu)/65536.f;
				// Each droplet advances a FULL travel per cycle, carrying its
				// own offsets and size with it.
				FLOAT S  = H1 + Phase;
				S -= appFloor(S);
				FVector Pw;
				FLOAT R, A, Stretch;
				if( Splash )
				{
					// Low wide spray at the waterline, thrown outward and
					// arcing back down along gravity.
					FLOAT Rise = 4.f*S*(1.f-S);
					FLOAT G = 0.25f + 2.2f*S;
					Pw = WC - GDir*( Rise*RadMin*0.55f )
					   + S1*((H2*2.f-1.f)*Rad1*G)
					   + S2*((H3*2.f-1.f)*Rad2*G);
					R = RadRef*( 0.10f + 0.10f*H3 );
					A = ZSmooth( 0.f, 0.12f, S )*( 1.f-ZSmooth( 0.35f, 1.f, S ) )*( 0.22f + 0.22f*H2 );
					Stretch = 1.f;	// spray flies every which way: keep it round
				}
				else
				{
					// Accelerating fall: droplets bunch at the spout and
					// streak apart lower down. Spawn across the WHOLE
					// cross-section of the column, drifting outward as they
					// fall (never just the plane of one authored face).
					FLOAT Fall = S*S*0.75f + S*0.25f;
					FLOAT G = 0.55f + 0.5f*Fall;	// fraction of the cross-section
					Pw = Spout + GDir*(Fall*Height)
					   + S1*((H2*2.f-1.f)*Rad1*G)
					   + S2*((H3*2.f-1.f)*Rad2*G);
					R = RadRef*( 0.08f + 0.06f*H3 );
					// Visible right down to the surface, so a droplet is
					// touching the water when its ripple is born.
					A = ZSmooth( 0.f, 0.06f, S )*( 1.f-ZSmooth( 0.97f, 1.f, S ) )*( 0.55f + 0.35f*H2 );
					// Motion streak, stretched further the faster it falls.
					// Kept SHORT relative to the drop: streaks spanning a
					// large part of the column overlap into a union that
					// barely changes as they move, so the pour looks frozen
					// however fast the strands travel.
					Stretch = 3.f + 3.f*Fall;
				}
				glColor3f( A, A, A );
				FVector Pp = Pw.TransformPointBy( Frame->Coords );	// world -> eye
				if( Pp.Z < 1.f )
					continue;	// behind/at the eye: nothing sane to draw
				if( FoldLight )
				{
					FLOAT LU = Facet.MapCoords.XAxis | (Pp - Facet.MapCoords.Origin);
					FLOAT LV = Facet.MapCoords.YAxis | (Pp - Facet.MapCoords.Origin);
					ZglMultiTexCoord2f( GL_TEXTURE1,
						(LU-Surface.LightMap->Pan.X+0.5f*Surface.LightMap->UScale)*LightUM,
						(LV-Surface.LightMap->Pan.Y+0.5f*Surface.LightMap->VScale)*LightVM );
				}
				// Camera-facing quad, its long axis along gravity AS SEEN ON
				// SCREEN: the perspective-correct direction of a world vector
				// at this depth is its eye-space XY minus the radial part.
				// (Falls back to screen-vertical when gravity points at the
				// camera, where a streak has no direction anyway.)
				FLOAT DX = GEyeDir.X - GEyeDir.Z*Pp.X/Pp.Z;
				FLOAT DY = GEyeDir.Y - GEyeDir.Z*Pp.Y/Pp.Z;
				FLOAT DL = appSqrt( DX*DX + DY*DY );
				if( DL > 0.001f ) { DX /= DL; DY /= DL; }
				else              { DX = 0.f;  DY = 1.f; }
				FLOAT LX = DX*R*Stretch, LY = DY*R*Stretch;	// along the fall
				FLOAT WX = -DY*R,        WY = DX*R;			// across it
				glTexCoord3f( SU0, SV0, H3 ); glVertex3f( Pp.X-LX-WX, Pp.Y-LY-WY, Pp.Z );
				glTexCoord3f( SU1, SV0, H3 ); glVertex3f( Pp.X-LX+WX, Pp.Y-LY+WY, Pp.Z );
				glTexCoord3f( SU1, SV1, H3 ); glVertex3f( Pp.X+LX+WX, Pp.Y+LY+WY, Pp.Z );
				glTexCoord3f( SU0, SV1, H3 ); glVertex3f( Pp.X+LX-WX, Pp.Y+LY-WY, Pp.Z );
			}
			glEnd();
			PolyCount++;

			// Where the pour lands: FOAM and RIPPLES on the pool surface.
			// Only when the column really ends in water (a pour onto stone
			// must not grow rings), and drawn FLAT in the water plane rather
			// than camera-facing, so they read as marks on the surface from
			// any angle.
			UBOOL Lands = 0;
			if( !Splash && Frame->Level && Frame->Level->Model && Frame->Level->GetLevelInfo() )
			{
				FVector Below = WC;
				Below.Z = Col->WMin.Z - 4.f;	// just under the waterline
				AZoneInfo* BZone = Frame->Level->Model->PointRegion( Frame->Level->GetLevelInfo(), Below ).Zone;
				Lands = BZone && BZone->bWaterZone;
			}
			if( Lands )
			{
				// Put the marks on the WATERLINE. Sheets usually end there,
				// but some are built poking into the pool, so walk up out of
				// the water zone to find the surface rather than trusting the
				// sheet's bottom.
				FLOAT SurfZ = Col->WMin.Z + 1.f;
				{
					FVector Probe = WC;
					for( INT st=0; st<8; st++ )
					{
						Probe.Z = Col->WMin.Z + st*4.f;
						AZoneInfo* PZone = Frame->Level->Model->PointRegion( Frame->Level->GetLevelInfo(), Probe ).Zone;
						if( !PZone || !PZone->bWaterZone )
							{ SurfZ = Probe.Z; break; }
					}
				}

				// Ripples: concentric rings expanding from the impact and
				// fading as they widen. Each ring is a strip of segments; the
				// shader's band profile softens them across their width, and
				// the texture coordinate runs along the circumference so no
				// two rings look alike.
				ZglUniform1f( WavyLocBlob, 2.f );
				ZglUniform1f( WavyLocFoam, 0.45f );
				ZglUniform1f( WavyLocFlowV, 1.f );		// 's' spans the ring width
				ZglUniform2f( WavyLocUEdge, SU0, SU1 );
				ZglUniform2f( WavyLocVEdge, SV0, SV1 );
				// Ring births are PHASE-LOCKED to the pour: one ring per fall
				// cycle, so a ring starts at the instant the falling water
				// arrives rather than on a clock of its own (which looks like
				// a surface rippling for no reason). Each ring then lives
				// NRings cycles, so NRings are alive, evenly staggered.
				const INT   NRings = 4, NSeg = 28;
				FLOAT RingTime = FallTime*NRings;
				FLOAT RingPh = (FLOAT)( fmod( T, (DOUBLE)RingTime ) / RingTime );
				glBegin( GL_QUADS );
				for( INT r=0; r<NRings; r++ )
				{
					FLOAT Sr = (FLOAT)r/(FLOAT)NRings + RingPh;
					Sr -= appFloor(Sr);
					FLOAT Rr = RadRef*( 0.8f + 7.5f*Sr );			// expanding
					FLOAT Wr = RadRef*( 0.35f + 0.45f*Sr );			// widening
					FLOAT Ar = ZSmooth( 0.f, 0.10f, Sr )*( 1.f-ZSmooth( 0.15f, 1.f, Sr ) )*0.5f;
					if( Ar <= 0.002f )
						continue;
					glColor3f( Ar, Ar, Ar );
					for( INT s=0; s<NSeg; s++ )
					{
						FLOAT A0 = (2.f*PI*s)/NSeg, A1 = (2.f*PI*(s+1))/NSeg;
						FLOAT C0 = appCos(A0), N0 = appSin(A0);
						FLOAT C1 = appCos(A1), N1 = appSin(A1);
						// Sample a different strip of the texture per segment
						// so the ring is not a perfectly even band.
						FLOAT T0 = SV0 + (SV1-SV0)*((FLOAT)s/NSeg);
						FLOAT T1 = SV0 + (SV1-SV0)*((FLOAT)(s+1)/NSeg);
						FVector Q0( WC.X + C0*(Rr-Wr), WC.Y + N0*(Rr-Wr), SurfZ );
						FVector Q1( WC.X + C0*(Rr+Wr), WC.Y + N0*(Rr+Wr), SurfZ );
						FVector Q2( WC.X + C1*(Rr+Wr), WC.Y + N1*(Rr+Wr), SurfZ );
						FVector Q3( WC.X + C1*(Rr-Wr), WC.Y + N1*(Rr-Wr), SurfZ );
						FVector E0 = Q0.TransformPointBy( Frame->Coords );
						FVector E1 = Q1.TransformPointBy( Frame->Coords );
						FVector E2 = Q2.TransformPointBy( Frame->Coords );
						FVector E3 = Q3.TransformPointBy( Frame->Coords );
						if( E0.Z<1.f || E1.Z<1.f || E2.Z<1.f || E3.Z<1.f )
							continue;
						glTexCoord2f( SU0, T0 ); glVertex3f( E0.X, E0.Y, E0.Z );
						glTexCoord2f( SU1, T0 ); glVertex3f( E1.X, E1.Y, E1.Z );
						glTexCoord2f( SU1, T1 ); glVertex3f( E2.X, E2.Y, E2.Z );
						glTexCoord2f( SU0, T1 ); glVertex3f( E3.X, E3.Y, E3.Z );
					}
				}
				glEnd();
				PolyCount++;

				// Froth: aerated white puffs churning at the impact point,
				// drifting outward and dissolving. Flat on the surface too.
				ZglUniform1f( WavyLocBlob, 1.f );
				ZglUniform1f( WavyLocFoam, 1.f );
				// Froth churns on the same arrival cadence.
				const INT   NFoam = 54;
				FLOAT FoamTime = Max( 0.35f, FallTime );
				FLOAT FoamPh = (FLOAT)( fmod( T, (DOUBLE)FoamTime ) / FoamTime );
				glBegin( GL_QUADS );
				for( INT m=0; m<NFoam; m++ )
				{
					DWORD hm = (DWORD)(m+11)*2246822519u;
					DWORD g1 = hm ^ (hm>>13);
					DWORD g2 = (hm*3266489917u+374761393u); g2 ^= g2>>15;
					DWORD g3 = (hm*668265263u+1u);          g3 ^= g3>>17;
					FLOAT G1 = (g1 & 0xFFFFu)/65536.f;
					FLOAT G2 = (g2 & 0xFFFFu)/65536.f;
					FLOAT G3 = (g3 & 0xFFFFu)/65536.f;
					FLOAT Sf = G1 + FoamPh;
					Sf -= appFloor(Sf);
					FLOAT Ang = G2*2.f*PI + 0.9f*Sf;			// swirls as it spreads
					FLOAT Rad = RadRef*( 0.15f + 2.3f*Sf*(0.5f+0.5f*G3) );
					FLOAT Rf  = RadRef*( 0.30f + 0.26f*G3 )*( 0.8f + 0.5f*Sf );
					FLOAT Af  = ZSmooth( 0.f, 0.10f, Sf )*( 1.f-ZSmooth( 0.30f, 1.f, Sf ) )*( 0.34f + 0.28f*G2 );
					if( Af <= 0.002f )
						continue;
					FLOAT FX = WC.X + appCos(Ang)*Rad, FY = WC.Y + appSin(Ang)*Rad;
					FVector P0( FX-Rf, FY-Rf, SurfZ ), P1( FX+Rf, FY-Rf, SurfZ );
					FVector P2( FX+Rf, FY+Rf, SurfZ ), P3( FX-Rf, FY+Rf, SurfZ );
					FVector E0 = P0.TransformPointBy( Frame->Coords );
					FVector E1 = P1.TransformPointBy( Frame->Coords );
					FVector E2 = P2.TransformPointBy( Frame->Coords );
					FVector E3 = P3.TransformPointBy( Frame->Coords );
					if( E0.Z<1.f || E1.Z<1.f || E2.Z<1.f || E3.Z<1.f )
						continue;
					glColor3f( Af, Af, Af );
					glTexCoord3f( SU0, SV0, G3 ); glVertex3f( E0.X, E0.Y, E0.Z );
					glTexCoord3f( SU1, SV0, G3 ); glVertex3f( E1.X, E1.Y, E1.Z );
					glTexCoord3f( SU1, SV1, G3 ); glVertex3f( E2.X, E2.Y, E2.Z );
					glTexCoord3f( SU0, SV1, G3 ); glVertex3f( E3.X, E3.Y, E3.Z );
				}
				glEnd();
				PolyCount++;
				ZglUniform1f( WavyLocFoam, 0.f );
			}
			glColor3f( 1.f, 1.f, 1.f );
		}
		else
		{
			for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
			{
				glBegin( GL_TRIANGLE_FAN );
				for( INT i=0; i<Poly->NumPts; i++ )
				{
					FVector& P = Poly->Pts[i]->Point;
					FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
					FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
					if( FoldLight )
						ZglMultiTexCoord2f( GL_TEXTURE1,
							(U-Surface.LightMap->Pan.X+0.5f*Surface.LightMap->UScale)*LightUM,
							(V-Surface.LightMap->Pan.Y+0.5f*Surface.LightMap->VScale)*LightVM );
					glTexCoord2f( (U-Surface.Texture->Pan.X)*BaseUM, (V-Surface.Texture->Pan.Y)*BaseVM );
					glVertex3f( P.X, P.Y, P.Z );
				}
				glEnd();
				PolyCount++;
			}
			// A fall that ends in water gets froth, wakes and spray where it
			// lands (see DrawFallFoot) -- drawn with the same program still
			// bound, right after the sheet itself.
			if( Falling && Wavy )
				DrawFallFoot( Frame, Surface, Facet, T, BaseUM, BaseVM, FoldLight, LightUM, LightVM );
		}
		if( Wavy )
			ZglUseProgram( 0 );
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
	// Skipped when the base pass already folded the light map in (lit
	// translucent surfaces -- see above).
	if( Surface.LightMap && !LightDone )
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
	// the whole pass is 2x-modulated ??? so far surfaces multiply by ~1.0 (no-op)
	// while near surfaces get the detail. (DetailTexture was nulled above when a
	// fog map is present, so the two are mutually exclusive as in the original.)
	if( Surface.DetailTexture && DetailTextures )
	{
		DetailCount++;
		const FLOAT NearZ = Max( 1.f, DetailRange );
		glDisable( GL_ALPHA_TEST );
		glEnable( GL_BLEND );
		glBlendFunc( GL_DST_COLOR, GL_SRC_COLOR );	// 2x modulate
		SetTexture( *Surface.DetailTexture, 0, 0 );	// Clamp=0: detail texture tiles
		CurrentBlendFlags = (DWORD)-1;
		// The overlay gets its own bias on top of the shared one SetTexture
		// just applied: it is pure high-frequency noise, so biasing it sharper
		// costs none of the base texture's stability.
		glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, Clamp( DetailBias, -3.f, 3.f ) );
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE );			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT );		glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
		glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PRIMARY_COLOR );	glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA );
		FLOAT Gray[4] = { 0.5f, 0.5f, 0.5f, 1.f };
		glTexEnvfv( GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, Gray );
		// CLIP each polygon to the near band before drawing it, exactly as
		// retail Glide does (UnGlide.cpp, same NearZ and the same alpha ramp).
		// This is not an optimization -- it is what makes the pass work at all.
		// The fade is a per-VERTEX alpha, and BSP surfaces are large: a floor
		// polygon's vertices sit at the far corners of the room, all of them
		// past NearZ and so all of them at alpha 0, which left the whole
		// polygon undetailed even where it ran directly beneath the camera.
		// Clipping introduces vertices ON the boundary, so the near part of a
		// big surface gets the alpha ramp it should always have had.
		for( FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
		{
			enum {MAX_CLIP=34};
			FVector Clipped[MAX_CLIP];
			INT NumClipped = 0;
			for( INT i=0, j=Poly->NumPts-1; i<Poly->NumPts && NumClipped<MAX_CLIP-1; j=i++ )
			{
				const FVector& Pi = Poly->Pts[i]->Point;
				const FVector& Pj = Poly->Pts[j]->Point;
				UBOOL NearI = Pi.Z < NearZ, NearJ = Pj.Z < NearZ;
				// Edge crosses the boundary: emit the crossing point. Straight
				// edge, so interpolating the eye-space position is exact, and
				// the mapping below is affine in position -- no need to carry
				// UVs through the clip.
				if( NearI != NearJ && Abs(Pi.Z-Pj.Z) > 0.0001f )
					Clipped[NumClipped++] = Pi + (Pj-Pi)*((NearZ-Pi.Z)/(Pj.Z-Pi.Z));
				if( NearI )
					Clipped[NumClipped++] = Pi;
			}
			if( NumClipped < 3 )
				continue;	// nothing of this polygon is near enough to detail
			glBegin( GL_TRIANGLE_FAN );
			for( INT i=0; i<NumClipped; i++ )
			{
				const FVector& P = Clipped[i];
				FLOAT U = Facet.MapCoords.XAxis | (P - Facet.MapCoords.Origin);
				FLOAT V = Facet.MapCoords.YAxis | (P - Facet.MapCoords.Origin);
				FLOAT A = P.Z>0.f ? Clamp( 100.f*(NearZ/P.Z - 1.f)/255.f, 0.f, 1.f ) : 0.f;
				glColor4f( 1.f, 1.f, 1.f, A );
				glTexCoord2f( (U-Surface.DetailTexture->Pan.X)*UMult, (V-Surface.DetailTexture->Pan.Y)*VMult );
				glVertex3f( P.X, P.Y, P.Z );
			}
			glEnd();
		}
		// Restore the shared mip bias and the default modulate env mode for
		// later passes/primitives -- the bias is a per-texture-object
		// parameter and would otherwise persist if this texture were ever
		// bound as something other than a detail overlay.
		glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, Clamp( LodBias, -3.f, 3.f ) );
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		glColor3f( 1.f, 1.f, 1.f );
	}

	// Pass 3: fog map, additive over the lit surface.
	// -probenofogmap drops this pass: a translucent surface takes it on top of
	// its own additive blend, so a strongly volumetric-lit sheet can be washed
	// flat by fog it is easy to mistake for the surface's own colour.
	static UBOOL NoFogMap = ParseParam( appCmdLine(), "PROBENOFOGMAP" );
	if( Surface.FogMap && !NoFogMap )
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

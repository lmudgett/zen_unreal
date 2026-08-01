/*=============================================================================
	SDLDrv.cpp: SDL windowing/input driver (cross-platform port).

	Portable replacement for WinDrv. Implements the engine's UClient /
	UViewport contract on SDL2 so the game runs on Linux/macOS as well as
	Windows. The engine drives these classes polymorphically (Client from
	[Engine.Engine] ViewportManager; NewViewport/OpenWindow/Tick from
	UGameEngine), so no engine changes are needed. OpenGLDrv creates its GL
	context from the SDL window (its UNREAL_USE_SDL path).

	Author: Len Mudgett
=============================================================================*/

#include "SDLDrvPrivate.h"
#include "../Inc/SDLDrvHooks.h"

/*-----------------------------------------------------------------------------
	Package.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS(USDLViewport);
IMPLEMENT_CLASS(USDLClient);
IMPLEMENT_CLASS(UNullRenderDevice);
IMPLEMENT_PACKAGE(SDLDrv);

// Phase 5 (ImGui editor): first-chance event filter, see SDLDrvHooks.h.
SDLDRV_API UBOOL (*GSDLEventHook)( const union SDL_Event* Ev ) = NULL;

// One active viewport receives input (single-window game path).
static USDLViewport* GInputViewport = NULL;

// Phase 5 (editor): drag state for translating SDL mouse events into the
// UEngine editor-navigation virtuals (MouseDelta/MousePosition/Click), the
// way WinDrv's window proc did. Buttons is an EMouseButtons mask; a press
// captures the mouse (relative mode), a non-moved release is a Click.
static DWORD GEdButtons   = 0;
static FLOAT GEdMoveAccum = 0.f;
static FLOAT GEdPressX    = 0.f;
static FLOAT GEdPressY    = 0.f;

static DWORD EdModifierFlags( USDLViewport* V )
{
	DWORD Flags = 0;
	if( V && V->Input )
	{
		if( V->Input->KeyDown(IK_Shift) ) Flags |= MOUSE_Shift;
		if( V->Input->KeyDown(IK_Ctrl)  ) Flags |= MOUSE_Ctrl;
		if( V->Input->KeyDown(IK_Alt)   ) Flags |= MOUSE_Alt;
	}
	return Flags;
}

/*-----------------------------------------------------------------------------
	Render device helper (mirrors WinDrv's TryRenderDevice).
-----------------------------------------------------------------------------*/

static void SDLTryRenderDevice( UViewport* Viewport, const char* ClassName, UBOOL Fullscreen )
{
	guard(SDLTryRenderDevice);
	if( Viewport->RenDev )
	{
		Viewport->RenDev->Exit();
		delete Viewport->RenDev;
		Viewport->RenDev = NULL;
	}
	UClass* RenderClass = GObj.LoadClass( URenderDevice::StaticClass, NULL, ClassName, NULL, LOAD_KeepImports, NULL );
	if( RenderClass )
	{
		Viewport->RenDev = ConstructClassObject<URenderDevice>( RenderClass );
		if( Viewport->Client->Engine->Audio && !GIsEditor )
			Viewport->Client->Engine->Audio->SetViewport( NULL );
		if( Viewport->RenDev->Init( Viewport ) )
		{
			Viewport->Actor->XLevel->DetailChange( Viewport->RenDev->HighDetailActors );
			if( Viewport->Client->Engine->Audio && !GIsEditor )
				Viewport->Client->Engine->Audio->SetViewport( Viewport );
		}
		else
		{
			debugf( NAME_Log, LocalizeError("Failed3D") );
			delete Viewport->RenDev;
			Viewport->RenDev = NULL;
		}
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Input translation: SDL keycode -> engine EInputKey (Win32 VK codes).
-----------------------------------------------------------------------------*/

static INT SDLKeyToIK( SDL_Keycode K )
{
	// Printable ASCII a-z / 0-9 map directly (IK_A..Z are ASCII uppercase).
	// SDL3 renamed the letter keycodes SDLK_a..z -> SDLK_A..Z (values stay 'a'..'z').
	if( K>=SDLK_A && K<=SDLK_Z ) return IK_A + (K-SDLK_A);
	if( K>=SDLK_0 && K<=SDLK_9 ) return IK_0 + (K-SDLK_0);
	switch( K )
	{
		case SDLK_BACKSPACE:	return IK_Backspace;
		case SDLK_TAB:			return IK_Tab;
		case SDLK_RETURN:		return IK_Enter;
		case SDLK_KP_ENTER:		return IK_Enter;
		case SDLK_LSHIFT: case SDLK_RSHIFT:	return IK_Shift;
		case SDLK_LCTRL:  case SDLK_RCTRL:	return IK_Ctrl;
		case SDLK_LALT:   case SDLK_RALT:	return IK_Alt;
		case SDLK_PAUSE:		return IK_Pause;
		case SDLK_CAPSLOCK:		return IK_CapsLock;
		case SDLK_ESCAPE:		return IK_Escape;
		case SDLK_SPACE:		return IK_Space;
		case SDLK_PAGEUP:		return IK_PageUp;
		case SDLK_PAGEDOWN:		return IK_PageDown;
		case SDLK_END:			return IK_End;
		case SDLK_HOME:			return IK_Home;
		case SDLK_LEFT:			return IK_Left;
		case SDLK_UP:			return IK_Up;
		case SDLK_RIGHT:		return IK_Right;
		case SDLK_DOWN:			return IK_Down;
		case SDLK_INSERT:		return IK_Insert;
		case SDLK_DELETE:		return IK_Delete;
		case SDLK_GRAVE:		return IK_Tilde; // SDL3: was SDLK_BACKQUOTE
		case SDLK_NUMLOCKCLEAR:	return IK_NumLock;
		case SDLK_SCROLLLOCK:	return IK_ScrollLock;

		// Numeric keypad (bindable game controls, e.g. lean/inventory).
		case SDLK_KP_0:			return IK_NumPad0;
		case SDLK_KP_1:			return IK_NumPad1;
		case SDLK_KP_2:			return IK_NumPad2;
		case SDLK_KP_3:			return IK_NumPad3;
		case SDLK_KP_4:			return IK_NumPad4;
		case SDLK_KP_5:			return IK_NumPad5;
		case SDLK_KP_6:			return IK_NumPad6;
		case SDLK_KP_7:			return IK_NumPad7;
		case SDLK_KP_8:			return IK_NumPad8;
		case SDLK_KP_9:			return IK_NumPad9;
		case SDLK_KP_MULTIPLY:	return IK_GreyStar;
		case SDLK_KP_PLUS:		return IK_GreyPlus;
		case SDLK_KP_MINUS:		return IK_GreyMinus;
		case SDLK_KP_PERIOD:	return IK_NumPadPeriod;
		case SDLK_KP_DIVIDE:	return IK_GreySlash;

		// OEM punctuation (Win32 VK_OEM_*); WinDrv gets these free via wParam,
		// so without them these keys were unbindable on the SDL build.
		case SDLK_MINUS:		return IK_Minus;
		case SDLK_EQUALS:		return IK_Equals;
		case SDLK_LEFTBRACKET:	return IK_LeftBracket;
		case SDLK_RIGHTBRACKET:	return IK_RightBracket;
		case SDLK_BACKSLASH:	return IK_Backslash;
		case SDLK_SEMICOLON:	return IK_Semicolon;
		case SDLK_APOSTROPHE:	return IK_SingleQuote;
		case SDLK_COMMA:		return IK_Comma;
		case SDLK_PERIOD:		return IK_Period;
		case SDLK_SLASH:		return IK_Slash;
		default: break;
	}
	if( K>=SDLK_F1 && K<=SDLK_F12 ) return IK_F1 + (K-SDLK_F1);
	return -1; // unmapped
}

/*-----------------------------------------------------------------------------
	USDLViewport.
-----------------------------------------------------------------------------*/

USDLViewport::USDLViewport( ULevel* InLevel, USDLClient* InClient )
:	UViewport( InLevel, InClient )
,	Client( InClient )
,	Window( NULL )
,	Fullscreen( 0 )
,	HoldingCapture( 0 )
{
	guard(USDLViewport::USDLViewport);
	ColorBytes = 4;
	if( GIsEditor )
		Input->Init( this, GSystem );
	unguard;
}

void USDLViewport::Destroy()
{
	guard(USDLViewport::Destroy);
	if( GInputViewport==this )
		GInputViewport = NULL;
	CloseWindow();
	UViewport::Destroy();
	unguard;
}

void USDLViewport::OpenWindow( DWORD ParentWindow, UBOOL Temporary, INT NewX, INT NewY, INT OpenX, INT OpenY )
{
	guard(USDLViewport::OpenWindow);
	check(Actor);

	if( NewX<=0 ) NewX = 800;
	if( NewY<=0 ) NewY = 600;
	NewX = Align(NewX,4);

	if( Temporary )
	{
		// Offscreen buffer (e.g. thumbnail render); no window.
		ColorBytes = 4;
		SizeX = NewX; SizeY = NewY;
		ScreenPointer = (BYTE*)appMalloc( 4*NewX*NewY, "TemporaryViewportData" );
		Window = NULL;
		// x64 port: the editor raytracer Locks temporary viewports; retail gave
		// them the software renderer, we give a no-op device (see UNullRenderDevice).
		RenDev = ConstructClassObject<URenderDevice>( UNullRenderDevice::StaticClass );
		RenDev->Init( this );
		debugf( NAME_Log, "Opened temporary SDL viewport" );
		return;
	}

	// Request a double-buffered OpenGL surface; OpenGLDrv makes the context.
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
	SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );

	// SDL3: creation flags are Uint64; position is set after creation.
	SDL_WindowFlags Flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
	if( Client->StartupFullscreen )
	{
		Flags |= SDL_WINDOW_FULLSCREEN;
		Fullscreen = 1;
	}

	char Title[256];
	// Multi-viewport editor: name each window after its viewport.
	if( GIsEditor )
		appSprintf( Title, "%s - %s", LocalizeGeneral("Product","Core"), GetName() );
	else
		appSprintf( Title, "%s", LocalizeGeneral("Product","Core") );
	Window = SDL_CreateWindow( Title, NewX, NewY, Flags );
	if( !Window )
	{
		appErrorf( "SDL_CreateWindow failed: %s", SDL_GetError() );
		return;
	}
	SDL_SetWindowPosition( Window,
		OpenX==INDEX_NONE ? (int)SDL_WINDOWPOS_CENTERED : OpenX,
		OpenY==INDEX_NONE ? (int)SDL_WINDOWPOS_CENTERED : OpenY );
	SDL_StartTextInput( Window ); // SDL3: text-input events are opt-in

	INT DrawX=NewX, DrawY=NewY;
	SDL_GetWindowSizeInPixels( Window, &DrawX, &DrawY );
	SizeX = DrawX; SizeY = DrawY;

	// Init input and the render device.
	Input->Init( this, GSystem );
	SDLTryRenderDevice( this, "ini:Engine.Engine.GameRenderDevice", Client->StartupFullscreen );

	Current = 1;
	// Multi-viewport: the first-opened window (Standard3V in the editor, the
	// game window otherwise) starts with input focus; later windows take it
	// via SDL_EVENT_WINDOW_FOCUS_GAINED.
	if( !GInputViewport )
		GInputViewport = this;
	if( Fullscreen )
		SetMouseCapture( 1, 1 );

	debugf( NAME_Log, "Opened SDL viewport %ix%i", SizeX, SizeY );
	unguard;
}

void USDLViewport::CloseWindow()
{
	guard(USDLViewport::CloseWindow);
	if( RenDev )
	{
		RenDev->Exit();
		delete RenDev;
		RenDev = NULL;
	}
	if( Window )
	{
		SDL_DestroyWindow( Window );
		Window = NULL;
	}
	if( ScreenPointer )
	{
		appFree( ScreenPointer );
		ScreenPointer = NULL;
	}
	unguard;
}

void* USDLViewport::GetWindow()
{
	return Window; // OpenGLDrv treats this as SDL_Window* under UNREAL_USE_SDL
}

void USDLViewport::Repaint()
{
	guard(USDLViewport::Repaint);
	if( RenDev && !OnHold && SizeX && SizeY && Window )
		Client->Engine->Draw( this, 0 );
	unguard;
}

void USDLViewport::Resized( INT NewX, INT NewY )
{
	guard(USDLViewport::Resized);
	if( NewX<=0 || NewY<=0 || (NewX==SizeX && NewY==SizeY) )
		return;
	SizeX = NewX; SizeY = NewY;
	// OpenGLDrv reads Viewport->SizeX/Y each Lock (glViewport), so the next
	// frame picks up the new size; no explicit surface realloc needed.
	unguard;
}

void USDLViewport::MakeFullscreen( INT NewX, INT NewY, UBOOL SaveConfig )
{
	guard(USDLViewport::MakeFullscreen);
	if( !Window )
		return;
	Fullscreen = !Fullscreen;
	SDL_SetWindowFullscreen( Window, Fullscreen ? true : false ); // SDL3: bool
	INT DrawX=NewX, DrawY=NewY;
	SDL_GetWindowSizeInPixels( Window, &DrawX, &DrawY );
	SizeX = DrawX; SizeY = DrawY;
	SetMouseCapture( Fullscreen, Fullscreen );
	if( SaveConfig )
	{
		Client->ViewportX = SizeX;
		Client->ViewportY = SizeY;
	}
	unguard;
}

// Viewport-level console commands (the video menu drives these). Mirrors
// UWindowsViewport::Exec so the SDL build's Options->Video works.
UBOOL USDLViewport::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(USDLViewport::Exec);
	if( UViewport::Exec( Cmd, Out ) )
	{
		return 1;
	}
	else if( ParseCommand(&Cmd,"ToggleFullscreen") )
	{
		if( Window )
		{
			MakeFullscreen( SizeX, SizeY, 1 );	// toggles Fullscreen
			Client->FullscreenViewport = Fullscreen ? this : NULL;
		}
		return 1;
	}
	else if( ParseCommand(&Cmd,"GetCurrentRes") )
	{
		Out->Logf( "%ix%i", SizeX, SizeY );
		return 1;
	}
	else if( ParseCommand(&Cmd,"SetRes") )
	{
		INT X = appAtoi( Cmd );
		const char* P = appStrchr( Cmd, 'x' ); if( !P ) P = appStrchr( Cmd, 'X' );
		INT Y = P ? appAtoi( P+1 ) : 0;
		if( X>0 && Y>0 && Window )
		{
			if( Fullscreen )
			{
				// Switch the fullscreen mode to the requested resolution.
				SDL_DisplayMode Mode;
				if( SDL_GetClosestFullscreenDisplayMode( SDL_GetDisplayForWindow(Window), X, Y, 0.f, false, &Mode ) )
					SDL_SetWindowFullscreenMode( Window, &Mode );
			}
			else
			{
				SDL_SetWindowSize( Window, X, Y );
			}
			INT DrawX=X, DrawY=Y;
			SDL_GetWindowSizeInPixels( Window, &DrawX, &DrawY );
			SizeX = DrawX; SizeY = DrawY;
			Client->ViewportX = SizeX;
			Client->ViewportY = SizeY;
		}
		return 1;
	}
	return 0;
	unguard;
}

void USDLViewport::SetMouseCapture( UBOOL Capture, UBOOL Clip, UBOOL FocusOnly )
{
	guard(USDLViewport::SetMouseCapture);
	HoldingCapture = Capture;
	if( Window )
		SDL_SetWindowRelativeMouseMode( Window, Capture ? true : false ); // SDL3
	unguard;
}

void USDLViewport::SetModeCursor()  { /* cursor handled by SDL relative mode */ }
void USDLViewport::UpdateWindow()   {}
// Per-frame device input poll (joystick etc). MUST NOT call Input->ResetInput()
// — ResetInput() calls back into UpdateInput(), which would recurse forever.
// Keyboard/mouse arrive via SDL events in PumpEvents; no joystick yet.
void USDLViewport::UpdateInput( UBOOL Reset ) {}
void USDLViewport::MakeCurrent()    { /* single GL context; OpenGLDrv binds it */ }

UBOOL USDLViewport::CauseInputEvent( INT iKey, EInputAction Action, FLOAT Delta )
{
	guard(USDLViewport::CauseInputEvent);
	if( iKey>=0 && iKey<IK_MAX )
		return Client->Engine->InputEvent( this, (EInputKey)iKey, Action, Delta );
	return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	USDLClient.
-----------------------------------------------------------------------------*/

USDLClient::USDLClient()
{}

void USDLClient::InternalClassInitializer( UClass* Class )
{
	guard(USDLClient::InternalClassInitializer);
	if( appStricmp( Class->GetName(), "SDLClient" )==0 )
		new(Class,"StartupFullscreen", RF_Public)UBoolProperty(CPP_PROPERTY(StartupFullscreen), "Display", CPF_Config );
	unguard;
}

void USDLClient::Init( UEngine* InEngine )
{
	guard(USDLClient::Init);
	UClient::Init( InEngine );

	SDL_SetMainReady();
	if( !SDL_Init( SDL_INIT_VIDEO ) ) // SDL3: returns bool (true=success); EVENTS folded in
		appErrorf( "SDL_Init failed: %s", SDL_GetError() );

	debugf( NAME_Init, "SDL client initialized (%s)", SDL_GetCurrentVideoDriver() );
	unguard;
}

void USDLClient::Destroy()
{
	guard(USDLClient::Destroy);

	// x64 port: delete viewports now, while the client/engine are still alive.
	// The final GObj.Exit() purge dispatches UEngine::Destroy first, which
	// does `delete Client` (frees this object); a viewport surviving to its
	// own purge slot would then dereference the freed client in
	// UViewport::Destroy (Client->Viewports.RemoveItem) -> C0000005 on exit.
	// WinDrv never hits this: WM_DESTROY deletes the viewport at close time.
	// SDL's close path only sets GIsRequestingExit, so do it here - and do it
	// before SDL_Quit, since CloseWindow/RenDev->Exit need SDL video up.
	// Each delete removes itself from Viewports via UViewport::Destroy.
	while( Viewports.Num() )
		delete Viewports( Viewports.Num()-1 );

	if( SDL_WasInit(SDL_INIT_VIDEO) )
		SDL_Quit();
	UClient::Destroy();
	unguard;
}

UViewport* USDLClient::NewViewport( ULevel* InLevel, const FName Name )
{
	guard(USDLClient::NewViewport);
	return new( GObj.GetTransientPackage(), Name )USDLViewport( InLevel, this );
	unguard;
}

UViewport* USDLClient::CurrentViewport()
{
	guard(USDLClient::CurrentViewport);
	return GInputViewport;
	unguard;
}

void USDLClient::PumpEvents()
{
	guard(USDLClient::PumpEvents);
	SDL_Event Ev;
	while( SDL_PollEvent(&Ev) )
	{
		// Phase 5: the UI overlay (ImGui editor) sees every event first; when
		// it claims one, the engine input translation below is skipped. Quit
		// must never be swallowed.
		if( GSDLEventHook && (*GSDLEventHook)( &Ev ) && Ev.type!=SDL_EVENT_QUIT )
			continue;

		// SDL3: event enums are SDL_EVENT_*, key events are flattened
		// (Ev.key.key), window resize is its own event type, mouse rel is float.
		// Phase 5 (multi-viewport editor): route each event to the viewport
		// owning its window; keyboard goes to the focused (input) viewport.
		USDLViewport* V = GInputViewport;
		{
			Uint32 WinId = 0;
			switch( Ev.type )
			{
				case SDL_EVENT_MOUSE_MOTION:					WinId = Ev.motion.windowID; break;
				case SDL_EVENT_MOUSE_WHEEL:						WinId = Ev.wheel.windowID;  break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:					WinId = Ev.button.windowID; break;
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				case SDL_EVENT_WINDOW_RESIZED:
				case SDL_EVENT_WINDOW_FOCUS_GAINED:
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:			WinId = Ev.window.windowID; break;
			}
			if( WinId )
				for( INT i=0; i<Viewports.Num(); i++ )
				{
					USDLViewport* Test = (USDLViewport*)Viewports(i);
					if( Test->Window && SDL_GetWindowID(Test->Window)==WinId )
					{
						V = Test;
						break;
					}
				}
		}
		switch( Ev.type )
		{
			case SDL_EVENT_QUIT:
				GIsRequestingExit = 1;
				break;
			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				// The focused window's viewport receives keyboard input and
				// counts as current for realtime updates.
				if( V )
				{
					GInputViewport = V;
					for( INT i=0; i<Viewports.Num(); i++ )
						Viewports(i)->Current = (Viewports(i)==V);
				}
				break;
			case SDL_EVENT_WINDOW_FOCUS_LOST:
				// Release all held input so alt-tabbing with a key or mouse
				// button down doesn't leave it stuck (WinDrv resyncs held keys
				// via GetAsyncKeyState in UpdateInput; SDL gets no such polling).
				// Safe to call directly - ResetInput does not re-enter
				// UpdateInput (that recursion is what deadlocked earlier).
				if( V && V->Input )
					V->Input->ResetInput();
				break;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				// Closing the primary (first) viewport window quits. Secondary
				// editor views tear down properly via CAMERA CLOSE (safe here:
				// the viewport lookup for later events in this pump just finds
				// nothing, and every branch null-checks V).
				if( V && Viewports.Num() && V==Viewports(0) )
					GIsRequestingExit = 1;
				else if( V && GIsEditor && Engine )
				{
					char Cmd[128];
					appSprintf( Cmd, "CAMERA CLOSE NAME=%s", V->GetName() );
					Engine->Exec( Cmd );
				}
				else if( V && V->Window )
					SDL_HideWindow( V->Window );
				break;
			case SDL_EVENT_KEY_DOWN:
				if( V && !Ev.key.repeat )
				{
					// Alt+Enter: standard windowed<->fullscreen toggle. Swallow the key
					// so the menu/game never sees a stray Enter press from the combo.
					if( Ev.key.key==SDLK_RETURN && (Ev.key.mod & SDL_KMOD_ALT) && !GIsEditor )
					{
						V->Exec( "ToggleFullscreen" );
						break;
					}
					INT iKey = SDLKeyToIK( Ev.key.key );
					if( iKey>=0 )
					{
						V->CauseInputEvent( iKey, IST_Press );
						// Editor keyboard shortcuts (UnEdCam Key handler).
						if( GIsEditor && Engine )
							Engine->Key( V, (EInputKey)iKey );
					}
				}
				break;
			case SDL_EVENT_KEY_UP:
				if( V )
				{
					INT iKey = SDLKeyToIK( Ev.key.key );
					if( iKey>=0 ) V->CauseInputEvent( iKey, IST_Release );
				}
				break;
			case SDL_EVENT_TEXT_INPUT:
				// Feed typed characters to the console/UI.
				if( V && Engine )
					for( const char* c=Ev.text.text; *c; c++ )
						Engine->Key( V, (EInputKey)(BYTE)*c );
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
				if( V )
				{
					INT iKey = Ev.button.button==SDL_BUTTON_LEFT ? IK_LeftMouse
						: Ev.button.button==SDL_BUTTON_RIGHT ? IK_RightMouse
						: Ev.button.button==SDL_BUTTON_MIDDLE ? IK_MiddleMouse : -1;
					if( iKey>=0 )
						V->CauseInputEvent( iKey, Ev.type==SDL_EVENT_MOUSE_BUTTON_DOWN ? IST_Press : IST_Release );

					// Editor navigation: press captures the mouse and starts a
					// drag (MouseDelta); a release with no movement is a Click
					// (hit-test selection). Mirrors WinDrv's window proc.
					if( GIsEditor && iKey>=0 )
					{
						DWORD B = iKey==IK_LeftMouse ? MOUSE_Left
							: iKey==IK_RightMouse ? MOUSE_Right : MOUSE_Middle;
						if( Ev.type==SDL_EVENT_MOUSE_BUTTON_DOWN )
						{
							if( !GEdButtons )
							{
								GEdMoveAccum = 0.f;
								GEdPressX = (FLOAT)Ev.button.x;
								GEdPressY = (FLOAT)Ev.button.y;
								V->SetMouseCapture( 1, 1 );
							}
							GEdButtons |= B;
						}
						else
						{
							GEdButtons &= ~B;
							if( !GEdButtons )
							{
								V->SetMouseCapture( 0, 0 );
								if( GEdMoveAccum <= 4.f && Engine )
								{
									Engine->Click( V, B|EdModifierFlags(V), GEdPressX, GEdPressY );
									if( !V->IsRealtime() )
										V->Repaint();
								}
							}
						}
					}
				}
				break;
			case SDL_EVENT_MOUSE_MOTION:
				if( GIsEditor && V )
				{
					// Editor: drag drives MouseDelta (camera/actor/brush per
					// mode+buttons), hover drives MousePosition.
					if( GEdButtons && Engine )
					{
						GEdMoveAccum += Abs((FLOAT)Ev.motion.xrel) + Abs((FLOAT)Ev.motion.yrel);
						Engine->MouseDelta( V, GEdButtons|EdModifierFlags(V), (FLOAT)Ev.motion.xrel, (FLOAT)Ev.motion.yrel );
						if( !V->IsRealtime() )
							V->Repaint();
					}
					else if( Engine )
						Engine->MousePosition( V, EdModifierFlags(V), (FLOAT)Ev.motion.x, (FLOAT)Ev.motion.y );
				}
				else if( V && V->HoldingCapture )
				{
					if( Ev.motion.xrel )
						V->CauseInputEvent( IK_MouseX, IST_Axis, +(FLOAT)Ev.motion.xrel );
					if( Ev.motion.yrel )
						V->CauseInputEvent( IK_MouseY, IST_Axis, -(FLOAT)Ev.motion.yrel );
				}
				break;
			case SDL_EVENT_MOUSE_WHEEL:
				// Editor: scroll the in-engine texture browser (Misc2 is its
				// scroll offset in 512ths of a viewport height).
				if( GIsEditor && V && V->Actor && V->Actor->RendMap==REN_TexBrowser )
					V->Actor->Misc2 = Max( 0, V->Actor->Misc2 - (INT)(Ev.wheel.y*48.f) );
				else if( V && !GIsEditor && Ev.wheel.y!=0.f )
				{
					// In-game: the wheel drives weapon switching. Each notch is a
					// momentary press+release, as WinDrv does for WM_MOUSEWHEEL;
					// send one per notch so a fast flick still steps once each.
					INT iKey = Ev.wheel.y>0.f ? IK_MouseWheelUp : IK_MouseWheelDown;
					for( INT n=Max(1,(INT)Abs(Ev.wheel.y)); n>0; n-- )
					{
						V->CauseInputEvent( iKey, IST_Press );
						V->CauseInputEvent( iKey, IST_Release );
					}
				}
				break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			case SDL_EVENT_WINDOW_RESIZED:
				if( V )
					V->Resized( Ev.window.data1, Ev.window.data2 );
				break;
		}
	}
	unguard;
}

void USDLClient::Poll()
{
	guard(USDLClient::Poll);
	PumpEvents();
	unguard;
}

void USDLClient::Tick()
{
	guard(USDLClient::Tick);
	PumpEvents();

	// Blit realtime viewports.
	for( INT i=0; i<Viewports.Num(); i++ )
	{
		USDLViewport* V = (USDLViewport*)Viewports(i);
		if( V->Window && V->IsRealtime() && !V->OnHold && V->SizeX && V->SizeY )
			V->Repaint();
	}
	unguard;
}

void USDLClient::ShowViewportWindows( DWORD ShowFlags, int DoShow ) {}
void USDLClient::EnableViewportWindows( DWORD ShowFlags, int DoEnable ) {}
void USDLClient::EndFullscreen()
{
	guard(USDLClient::EndFullscreen);
	if( GInputViewport && GInputViewport->Fullscreen )
		GInputViewport->MakeFullscreen( GInputViewport->SizeX, GInputViewport->SizeY, 0 );
	FullscreenViewport = NULL;
	unguard;
}

UBOOL USDLClient::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(USDLClient::Exec);
	const char* Str = Cmd;
	if( ParseCommand(&Str,"EndFullscreen") )
	{
		EndFullscreen();
		return 1;
	}
	else if( ParseCommand(&Str,"GetCurrentRes") )
	{
		Out->Logf( "%ix%i", GInputViewport?GInputViewport->SizeX:0, GInputViewport?GInputViewport->SizeY:0 );
		return 1;
	}
	else if( ParseCommand(&Str,"GetRes") )
	{
		// The "Select Resolution" menu populates from this list. IMPORTANT: the
		// retail UnrealVideoMenu.GetAvailableRes parses into Resolutions[16] and
		// overflows (writes Resolutions[16]) if given MORE than 16 entries — a
		// 4K display exposes 20+ modes, which corrupts the menu and quits the
		// game. So emit a fixed set of COMMON resolutions filtered to the
		// display's native size (<=14 entries, always safe) rather than every
		// raw mode.
		SDL_DisplayID Disp = ( GInputViewport && GInputViewport->Window )
			? SDL_GetDisplayForWindow( GInputViewport->Window )
			: SDL_GetPrimaryDisplay();
		const SDL_DisplayMode* Desktop = SDL_GetDesktopDisplayMode( Disp );
		INT MaxW = Desktop ? Desktop->w : 1920;
		INT MaxH = Desktop ? Desktop->h : 1080;
		static const INT Common[][2] =
		{
			{640,480},{800,600},{1024,768},{1152,864},{1280,720},{1280,1024},
			{1600,900},{1600,1200},{1680,1050},{1920,1080},{1920,1200},
			{2560,1440},{2560,1600},{3840,2160}
		};
		FString Result;
		for( INT i=0; i<(INT)ARRAY_COUNT(Common); i++ )
			if( Common[i][0]<=MaxW && Common[i][1]<=MaxH )
				Result.Appendf( "%ix%i ", Common[i][0], Common[i][1] );
		Out->Log( *Result );
		return 1;
	}
	return UClient::Exec( Cmd, Out );
	unguard;
}

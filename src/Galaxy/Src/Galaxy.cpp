/*=============================================================================
	Galaxy.cpp: "Galaxy" audio subsystem rebuilt for the x64 port.

	The original module wrapped the third-party Galaxy Sound System, which
	only exists as a 32-bit static library and cannot be linked into a
	64-bit build. This is a from-scratch reimplementation of the original
	driver's exact behavior (preserved at legacy/Galaxy/Src/Galaxy.cpp) on
	modern backends:
	  * Sound effects: XAudio2 (in-box since Windows 10). One source voice
	    per active channel, routed through an "effects" submix; per-frame
	    volume/pan/pitch control mirrors the original glx formulas.
	  * Music: the retail .umx tracker modules (S3M and Impulse Tracker)
	    are rendered by the vendored libxmp-lite (MIT, see
	    src/ThirdParty/libxmp-lite) into a streaming XAudio2 voice on a
	    "music" submix, double-buffered from a small feeder thread.
	Package name, class name, and config properties are identical to the
	original so Unreal.ini and the audio options UI keep working.
	Not carried over: Aureal A3D hardware 3D and CD audio (UseCDMusic) — those
	flags are accepted but inert. Zone reverb (UseReverb) IS restored (Freeverb
	on SDL, the XAudio2 reverb APO on Windows).
=============================================================================*/

#pragma warning (disable:4201)
#define STRICT
#if _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
// cross-platform port: AUDIO BACKEND SELECTION. SDL3 audio is the backend on
// every platform (2026-08-01; XAudio2 retired with the rest of the
// Windows-only stack). The XAudio2 branches below are kept compilable for
// reference but are not built. Everything above the backend (spatialization,
// priority, ambient scan, music state machine) is backend-agnostic and shared.
#if UNREAL_USE_SDL
	#define UNREAL_SDL_AUDIO 1
#else
	#define UNREAL_SDL_AUDIO 0
#endif

#if UNREAL_SDL_AUDIO
	#define SDL_MAIN_HANDLED
	#include <SDL3/SDL.h>
#else
	#include <xaudio2.h>
	#include <xaudio2fx.h>	// built-in reverb APO for zone reverb
#endif
#include <math.h>
#include "Engine.h"
#include "UnRender.h"

#define LIBXMP_STATIC
#include <xmp.h>

/*-----------------------------------------------------------------------------
	Constants.
-----------------------------------------------------------------------------*/

#define MAX_EFFECTS_CHANNELS 32
#define MUSIC_RATE           44100
#define MUSIC_BUFFER_FRAMES  4096
#define MUSIC_BUFFER_BYTES   (MUSIC_BUFFER_FRAMES*4)	/* 16-bit stereo */
#define MUSIC_NUM_BUFFERS    2

/*-----------------------------------------------------------------------------
	FGalaxySample - decoded PCM stored in USound::Handle.
-----------------------------------------------------------------------------*/

struct FGalaxySample
{
	INT		Rate;		// Native sample rate, Hz (the glx "C4Speed").
	INT		Bits;		// 8 (unsigned) or 16 (signed).
	INT		Channels;	// Nearly always 1 in retail content.
	INT		NumBytes;	// Size of Pcm.
	INT		LoopStart;	// In frames; -1 = no loop.
	INT		LoopEnd;	// In frames.
	BYTE*	Pcm;		// appMalloc'd copy (USound::Data is emptied after registration).
};

/*-----------------------------------------------------------------------------
	FPlayingSound - one active effects channel (mirrors the original).
-----------------------------------------------------------------------------*/

struct FPlayingSound
{
#if UNREAL_SDL_AUDIO
	// SDL backend: a per-voice stream whose get-callback pulls from SamplePtr,
	// applies live L/R pan gains, and loops. Pitch via frequency ratio.
	SDL_AudioStream*	Stream;
	struct FGalaxySample* SamplePtr;
	INT					Cursor;		// current source frame (callback thread)
	volatile FLOAT		CbLeft;		// live pan gains (written by Update, read by cb)
	volatile FLOAT		CbRight;
	volatile INT		Finished;	// set by callback when a non-looping sound ends
#else
	IXAudio2SourceVoice* Voice;		// NULL until actually started in Update.
#endif
	AActor*	Actor;
	INT		Id;
	USound*	Sound;
	FVector	Location;
	FLOAT	Volume;
	FLOAT	Radius;
	FLOAT	Pitch;
	FLOAT	Priority;
	UBOOL	bStarted;	// x64 port (audit): voice started only after params applied
	FPlayingSound()
	:
#if UNREAL_SDL_AUDIO
		Stream(NULL), SamplePtr(NULL), Cursor(0), CbLeft(0), CbRight(0), Finished(0),
#else
		Voice(NULL),
#endif
		Actor(NULL), Id(0), Sound(NULL), Location(0,0,0)
	,	Volume(0), Radius(0), Pitch(0), Priority(0), bStarted(0)
	{}
	FPlayingSound( AActor* InActor, INT InId, USound* InSound, FVector InLocation, FLOAT InVolume, FLOAT InRadius, FLOAT InPitch, FLOAT InPriority )
	:
#if UNREAL_SDL_AUDIO
		Stream(NULL), SamplePtr(NULL), Cursor(0), CbLeft(0), CbRight(0), Finished(0),
#else
		Voice(NULL),
#endif
		Actor(InActor), Id(InId), Sound(InSound), Location(InLocation)
	,	Volume(InVolume), Radius(InRadius), Pitch(InPitch), Priority(InPriority), bStarted(0)
	{}
};

/*-----------------------------------------------------------------------------
	Music voice callback - signals the feeder thread when a buffer drains.
-----------------------------------------------------------------------------*/

#if !UNREAL_SDL_AUDIO
class FMusicVoiceCallback : public IXAudio2VoiceCallback
{
public:
	HANDLE Event;
	FMusicVoiceCallback() : Event(NULL) {}
	void __stdcall OnBufferEnd( void* ) { if(Event) SetEvent(Event); }
	void __stdcall OnVoiceProcessingPassStart( UINT32 ) {}
	void __stdcall OnVoiceProcessingPassEnd() {}
	void __stdcall OnStreamEnd() {}
	void __stdcall OnBufferStart( void* ) {}
	void __stdcall OnLoopEnd( void* ) {}
	void __stdcall OnVoiceError( void*, HRESULT ) {}
};
#endif

/*-----------------------------------------------------------------------------
	Zone reverb.

	The original driver applied per-zone reverb (glxSetSampleReverb) to the
	effect samples. This port restores it: on the SDL backend via a hand-written
	Freeverb (public-domain Schroeder/Moorer design by Jezar at Dreampoint) run
	as an SDL postmix on the effects device (music plays on a separate device so
	it stays dry); on the XAudio2 backend via the built-in reverb APO on the
	effects submix. Both are driven from the current ZoneInfo's reverb fields.
-----------------------------------------------------------------------------*/

// Reverb parameters resolved from the listener's zone, cached to update the
// backend only when the zone (or its settings) changes.
struct FZoneReverbState
{
	UBOOL	On;
	FLOAT	Wet;	// 0..1 reverb send level (from MasterGain)
	FLOAT	Room;	// 0..1 room size (from average Delay tap)
	FLOAT	Damp;	// 0..1 HF damping (from CutoffHz)
};

#if UNREAL_SDL_AUDIO
// Freeverb: 8 parallel comb filters -> 4 series allpass filters per channel,
// tuned for 44.1kHz (the device rate). Processes the interleaved-stereo effects
// mix in place, adding the reverb tail on top of the dry signal.
class FGalaxyReverb
{
public:
	FGalaxyReverb()
	{
		static const INT CombTune[8] = { 1116,1188,1277,1356,1422,1491,1557,1617 };
		static const INT ApTune[4]   = { 556,441,341,225 };
		const INT Spread = 23;
		FLOAT* p = Storage;
		for( INT i=0; i<8; i++ ) { Comb[0][i].Init(p,CombTune[i]);        p+=CombTune[i]; }
		for( INT i=0; i<8; i++ ) { Comb[1][i].Init(p,CombTune[i]+Spread); p+=CombTune[i]+Spread; }
		for( INT i=0; i<4; i++ ) { Ap[0][i].Init(p,ApTune[i]);            p+=ApTune[i]; }
		for( INT i=0; i<4; i++ ) { Ap[1][i].Init(p,ApTune[i]+Spread);     p+=ApTune[i]+Spread; }
		RoomSize=0.7f; Damp=0.4f; Wet=0.f; Active=0;
		Mute();
	}
	void SetParams( FLOAT Room, FLOAT Dmp, FLOAT W ) { RoomSize=Room; Damp=Dmp; Wet=W; }
	void SetActive( UBOOL b ) { Active=b; if(!b) Mute(); }	// dry when off
	void Mute()
	{
		for( INT i=0; i<(INT)ARRAY_COUNT(Storage); i++ ) Storage[i]=0.f;
		for( INT c=0; c<2; c++ ) for( INT i=0; i<8; i++ ) Comb[c][i].Filt=0.f;
	}
	void Process( FLOAT* Buf, INT Frames )	// runs on the SDL audio thread
	{
		if( !Active )
			return;
		// Tighter tail than before (max feedback ~0.87 not ~0.97) so a busy DM
		// doesn't smear into a muddy wash.
		const FLOAT Feedback = Clamp(RoomSize,0.f,1.f)*0.28f + 0.59f;
		const FLOAT Damp1 = Damp*0.4f, Damp2 = 1.f-Damp1;
		const FLOAT Gain = 0.015f;
		// Wet/dry CROSSFADE, not wet-on-top. The 8-comb sum runs ~2.4x the input,
		// so normalise it back to ~unity (WetN) and blend a modest amount in while
		// pulling the dry down by the SAME amount -- total level stays ~1.0, so the
		// reverb no longer boosts the mix into the limiter (that was the "harsh"),
		// and even a full-gain reverb zone tops out at ~18% wet (subtle ambience,
		// not the previous 1.7x muddy echo on every effect).
		const FLOAT Mix  = Clamp( Wet, 0.f, 1.f ) * 0.18f;	// 0..0.18 wet
		const FLOAT WetN = 1.f/2.4f;						// comb-sum -> ~unity
		const FLOAT DryG = 1.f - Mix, WetG = WetN*Mix;
		for( INT f=0; f<Frames; f++ )
		{
			FLOAT inL = Buf[f*2], inR = Buf[f*2+1];
			FLOAT input = (inL+inR)*Gain;
			FLOAT outL=0.f, outR=0.f;
			for( INT i=0; i<8; i++ )
			{
				outL += Comb[0][i].Process(input,Feedback,Damp1,Damp2);
				outR += Comb[1][i].Process(input,Feedback,Damp1,Damp2);
			}
			for( INT i=0; i<4; i++ ) { outL=Ap[0][i].Process(outL); outR=Ap[1][i].Process(outR); }
			Buf[f*2]   = inL*DryG + outL*WetG;
			Buf[f*2+1] = inR*DryG + outR*WetG;
		}
	}
private:
	struct FComb
	{
		FLOAT* Buf; INT Size, Idx; FLOAT Filt;
		void Init( FLOAT* b, INT s ) { Buf=b; Size=s; Idx=0; Filt=0.f; }
		FLOAT Process( FLOAT in, FLOAT fb, FLOAT d1, FLOAT d2 )
		{ FLOAT out=Buf[Idx]; Filt=out*d2+Filt*d1; Buf[Idx]=in+Filt*fb; if(++Idx>=Size)Idx=0; return out; }
	};
	struct FAllpass
	{
		FLOAT* Buf; INT Size, Idx;
		void Init( FLOAT* b, INT s ) { Buf=b; Size=s; Idx=0; }
		FLOAT Process( FLOAT in )
		{ FLOAT buf=Buf[Idx]; FLOAT out=buf-in; Buf[Idx]=in+buf*0.5f; if(++Idx>=Size)Idx=0; return out; }
	};
	FComb			Comb[2][8];
	FAllpass		Ap[2][4];
	volatile UBOOL	Active;
	FLOAT			RoomSize, Damp, Wet;
	FLOAT			Storage[25600];	// sum of all comb+allpass line lengths (both channels)
};
#endif

/*-----------------------------------------------------------------------------
	UGalaxyAudioSubsystem.
-----------------------------------------------------------------------------*/

class DLL_EXPORT UGalaxyAudioSubsystem : public UAudioSubsystem
{
	DECLARE_CLASS(UGalaxyAudioSubsystem,UAudioSubsystem,CLASS_Config)

	// Configuration (identical to the original so ini settings round-trip).
	UBOOL			UseDirectSound;
	UBOOL			UseFilter;
	UBOOL			UseSurround;
	UBOOL			UseStereo;
	UBOOL			UseCDMusic;
	UBOOL			UseDigitalMusic;
	UBOOL			UseSpatial;
	UBOOL			UseReverb;
	UBOOL			Use3dHardware;
	UBOOL			ReverseStereo;
	UBOOL			LowSoundQuality;
	UBOOL			Initialized;
	INT				Latency;
	BYTE			OutputRate;
	INT				EffectsChannels;
	BYTE			MusicVolume;
	BYTE			SoundVolume;
	FLOAT			AmbientFactor;
	FLOAT			DopplerSpeed;

	// Runtime state.
	UViewport*				Viewport;
	FZoneReverbState		CurrentReverb;	// last reverb settings pushed to the backend
#if UNREAL_SDL_AUDIO
	SDL_AudioDeviceID		AudioDevice;	// effects device (carries the reverb postmix)
	SDL_AudioDeviceID		MusicDevice;	// separate device so music stays dry
	SDL_AudioStream*		MusicStream;	// bound music stream (xmp-fed callback)
	SDL_Mutex*				MusicMutex;
	FGalaxyReverb			Reverb;			// zone reverb applied to the effects mix
#else
	IXAudio2*				XAudio2;
	IXAudio2MasteringVoice*	MasterVoice;
	IXAudio2SubmixVoice*	EffectsSubmix;
	IXAudio2SubmixVoice*	MusicSubmix;
#endif
	FPlayingSound			PlayingSounds[MAX_EFFECTS_CHANNELS];
	DOUBLE					LastTime;
	INT						FreeSlot;

	// Music state (mirrors the original state machine).
	UMusic*					CurrentMusic;
	BYTE					CurrentCDTrack;
	BYTE					CurrentSection;
	FLOAT					MusicFade;

	// Music playback machinery.
	xmp_context				XmpCtx;
	volatile UBOOL			MusicPlaying;
#if !UNREAL_SDL_AUDIO
	IXAudio2SourceVoice*	MusicVoice;
	FMusicVoiceCallback		MusicCallback;
	CRITICAL_SECTION		MusicLock;
	HANDLE					MusicThread;
	HANDLE					MusicEvent;
	volatile UBOOL			MusicThreadExit;
	BYTE					MusicBuffers[MUSIC_NUM_BUFFERS][MUSIC_BUFFER_BYTES];
	INT						MusicNextBuffer;
#endif

	// Constructor.
	UGalaxyAudioSubsystem();
	static void InternalClassInitializer( UClass* Class );

	// UObject interface.
	void Destroy();
	void PostEditChange();
	void ShutdownAfterError();

	// UAudioSubsystem interface.
	UBOOL Init();
	void SetViewport( UViewport* InViewport );
	UBOOL Exec( const char* Cmd, FOutputDevice* Out=GSystem );
	void Update( FPointRegion Region, FCoords& Listener );
	void RegisterMusic( UMusic* Music );
	void RegisterSound( USound* Sound );
	void UnregisterSound( USound* Sound );
	void UnregisterMusic( UMusic* Music );
	UBOOL PlaySound( AActor* Actor, INT Id, USound* Sound, FVector Location, FLOAT Volume, FLOAT Radius, FLOAT Pitch );
	void NoteDestroy( AActor* Actor );
	UBOOL GetLowQualitySetting();

	// Internals.
	void SetVolumes();
	void StopSound( INT Index );
	void StopAllSound();
	FLOAT SoundPriority( FVector Location, FLOAT Volume, FLOAT Radius )
	{
		if( !Viewport || !Viewport->Actor )
			return 0;
		return Volume * (1.f - FDist(Location, Viewport->Actor->Location)/Radius);
	}
	FGalaxySample* GetSample( USound* Sound )
	{
		if( !Sound->Handle )
			RegisterSound( Sound );
		return (FGalaxySample*)Sound->Handle;
	}
	void StartMusic( UMusic* Song, BYTE Section );
	void StopMusic();
#if !UNREAL_SDL_AUDIO
	void MusicFeed();
	static DWORD WINAPI MusicThreadProc( LPVOID Param );
#endif
};

IMPLEMENT_CLASS(UGalaxyAudioSubsystem);
IMPLEMENT_PACKAGE(Galaxy);

// cross-platform port: music-state lock is an SDL mutex or a Win32 critical
// section; the guarded xmp calls are identical.
#if UNREAL_SDL_AUDIO
	#define GALAXY_MLOCK()   SDL_LockMutex(MusicMutex)
	#define GALAXY_MUNLOCK() SDL_UnlockMutex(MusicMutex)
#else
	#define GALAXY_MLOCK()   EnterCriticalSection(&MusicLock)
	#define GALAXY_MUNLOCK() LeaveCriticalSection(&MusicLock)
#endif

#if UNREAL_SDL_AUDIO
/*-----------------------------------------------------------------------------
	SDL audio callbacks (cross-platform port).

	SDL mixes all bound streams into the device. Each effect voice is a stream
	whose get-callback pulls mono sample frames, applies the current L/R pan
	gains (updated live by Update), and loops; pitch is SDL's per-stream
	frequency ratio. Music is one stream fed by libxmp on demand.
-----------------------------------------------------------------------------*/

// Read one mono source frame from a decoded sample as float [-1,1].
static inline FLOAT GalaxySampleFrame( const FGalaxySample* S, INT Frame )
{
	if( S->Bits==16 )
		return ((const SWORD*)S->Pcm)[Frame*S->Channels] / 32768.f;
	else
		return ((INT)S->Pcm[Frame*S->Channels] - 128) / 128.f;
}

static void SDLCALL GalaxyEffectsCB( void* userdata, SDL_AudioStream* stream, int additional, int total )
{
	FPlayingSound* P = (FPlayingSound*)userdata;
	FGalaxySample* S = P->SamplePtr;
	// Once the sample is exhausted (non-looping) feed NOTHING more, so the data
	// already queued drains and plays out fully -- the game thread tears the
	// stream down only after SDL reports it empty. Feeding trailing zeros here
	// would keep the stream perpetually "non-empty" and either clip the tail
	// (old behaviour) or leak the channel.
	if( !S || additional<=0 || P->Finished )
		return;
	INT Frames = additional / (INT)(2*sizeof(float));      // stereo-float input frames
	INT TotalFrames = S->NumBytes / (S->Channels * (S->Bits/8));
	// Looping: honor an embedded WAV smpl loop region if present; otherwise
	// AMBIENT-slot sounds (Actor.AmbientSound) still loop the WHOLE sample --
	// that's how the engine treats ambients, and most retail ambient WAVs carry
	// no smpl chunk, so without this they'd play once and fall silent (the scan
	// won't restart a channel that's still occupied). Non-ambient one-shots with
	// no loop region play once and finish. Wrap at the loop END, not the sample
	// end -- the old code looped 0..TotalFrames, replaying the post-loop tail and
	// clicking at the seam every cycle. (SLOT_Ambient*2==8; Id == Index*16 +
	// Slot*2 + bNoOverride, so Slot*2 == (Id & 14).)
	UBOOL HasSmpl  = (S->LoopStart>=0 && S->LoopEnd>S->LoopStart);
	UBOOL Ambient  = ((P->Id & 14)==SLOT_Ambient*2);
	UBOOL Loops    = HasSmpl || Ambient;
	INT   LoopBack = HasSmpl ? S->LoopStart : 0;
	INT   LoopEnd  = HasSmpl ? Min(S->LoopEnd+1,TotalFrames) : TotalFrames;	// loopE is inclusive
	float Buf[1024*2];
	while( Frames>0 && !P->Finished )
	{
		INT N = Min( Frames, 1024 );
		FLOAT L = P->CbLeft, R = P->CbRight;
		INT Produced = 0;
		for( INT i=0; i<N; i++ )
		{
			if( P->Cursor>=LoopEnd )
			{
				if( !Loops )
					{ P->Finished = 1; break; }		// one-shot done: stop, drain the rest
				P->Cursor = LoopBack;				// seamless wrap at the loop end
			}
			FLOAT s = GalaxySampleFrame( S, P->Cursor );
			Buf[i*2]   = s * L;
			Buf[i*2+1] = s * R;
			Produced++;
			P->Cursor++;
		}
		if( Produced>0 )
			SDL_PutAudioStreamData( stream, Buf, Produced*2*(INT)sizeof(float) );
		Frames -= N;
	}
}

static void SDLCALL GalaxyMusicCB( void* userdata, SDL_AudioStream* stream, int additional, int total )
{
	UGalaxyAudioSubsystem* A = (UGalaxyAudioSubsystem*)userdata;
	if( additional<=0 )
		return;
	SDL_LockMutex( A->MusicMutex );
	if( A->MusicPlaying && A->XmpCtx )
	{
		BYTE Buf[4096];
		INT Left = additional;
		while( Left>0 )
		{
			INT N = Min( Left, (INT)sizeof(Buf) );
			if( xmp_play_buffer( A->XmpCtx, Buf, N, 0 )!=0 )
				{ A->MusicPlaying = 0; break; }
			SDL_PutAudioStreamData( stream, Buf, N );
			Left -= N;
		}
	}
	SDL_UnlockMutex( A->MusicMutex );
}

// Postmix on the effects device: runs the fully-mixed effects buffer through
// the zone reverb (music is on a separate device and bypasses this).
static void SDLCALL GalaxyReverbPostmix( void* userdata, const SDL_AudioSpec* spec, float* buffer, int buflen )
{
	UGalaxyAudioSubsystem* A = (UGalaxyAudioSubsystem*)userdata;
	if( spec->channels!=2 )
		return;
	INT Frames = buflen/(INT)(sizeof(float)*2);
	A->Reverb.Process( buffer, Frames );		// zone reverb (when active)
	// Soft-knee peak limiter. Everything at/below the knee passes THROUGH UNCHANGED
	// (bit-exact -- no waveshaping distortion of normal-level explosions/pickups);
	// only the overshoot above the knee is smoothly saturated toward +-1.0 so loud
	// sums of voices never hard-clip into crackle. Replaces a global tanh() that
	// coloured/dulled every sound even when it wasn't clipping.
	const FLOAT Knee = 0.75f, Range = 1.f - Knee;
	for( INT i=0; i<Frames*2; i++ )
	{
		FLOAT x = buffer[i];
		FLOAT a = fabsf( x );
		if( a > Knee )
		{
			a = Knee + Range * tanhf( (a-Knee)/Range );	// asymptotes to 1.0
			buffer[i] = (x<0.f) ? -a : a;
		}
	}
}
#endif

/*-----------------------------------------------------------------------------
	Construction and class registration.
-----------------------------------------------------------------------------*/

UGalaxyAudioSubsystem::UGalaxyAudioSubsystem()
{
	guard(UGalaxyAudioSubsystem::UGalaxyAudioSubsystem);

	// Defaults, matching the original driver.
	MusicVolume			= 63;
	SoundVolume			= 127;
	AmbientFactor		= 1.0;
	UseDirectSound		= 1;
	UseFilter			= 1;
	UseSurround			= 1;
	UseStereo			= 1;
	UseCDMusic			= 1;
	UseDigitalMusic		= 1;
	UseSpatial			= 0;
	UseReverb			= 1;
	ReverseStereo		= 0;
	LowSoundQuality		= 0;
	Latency				= 40;
	OutputRate			= 1;
	EffectsChannels		= 16;
	DopplerSpeed		= 8000.0;
	Initialized			= 0;
	Viewport			= NULL;
	appMemset( &CurrentReverb, 0, sizeof(CurrentReverb) );
	LastTime			= 0;
	FreeSlot			= 0;
	CurrentMusic		= NULL;
	CurrentCDTrack		= 255;
	CurrentSection		= 255;
	MusicFade			= 1.0;
	XmpCtx				= NULL;
	MusicPlaying		= 0;
#if UNREAL_SDL_AUDIO
	AudioDevice			= 0;
	MusicDevice			= 0;
	MusicStream			= NULL;
	MusicMutex			= NULL;
#else
	XAudio2				= NULL;
	MasterVoice			= NULL;
	EffectsSubmix		= NULL;
	MusicSubmix			= NULL;
	MusicVoice			= NULL;
	MusicThread			= NULL;
	MusicEvent			= NULL;
	MusicThreadExit		= 0;
	MusicNextBuffer		= 0;
#endif

	unguard;
}

void UGalaxyAudioSubsystem::InternalClassInitializer( UClass* Class )
{
	guard(UGalaxyAudioSubsystem::InternalClassInitializer);
	if( appStricmp( Class->GetName(), "GalaxyAudioSubsystem" )==0 )
	{
		UEnum* OutputRates = new( Class, "OutputRates" )UEnum( NULL );
		new( OutputRates->Names )FName( "11025Hz" );
		new( OutputRates->Names )FName( "22050Hz" );
		new( OutputRates->Names )FName( "44100Hz" );
		new(Class,"UseDirectSound",  RF_Public)UBoolProperty  (CPP_PROPERTY(UseDirectSound ), "Audio", CPF_Config );
		new(Class,"UseFilter",       RF_Public)UBoolProperty  (CPP_PROPERTY(UseFilter      ), "Audio", CPF_Config );
		new(Class,"UseSurround",     RF_Public)UBoolProperty  (CPP_PROPERTY(UseSurround    ), "Audio", CPF_Config );
		new(Class,"UseStereo",       RF_Public)UBoolProperty  (CPP_PROPERTY(UseStereo      ), "Audio", CPF_Config );
		new(Class,"UseCDMusic",      RF_Public)UBoolProperty  (CPP_PROPERTY(UseCDMusic     ), "Audio", CPF_Config );
		new(Class,"UseDigitalMusic", RF_Public)UBoolProperty  (CPP_PROPERTY(UseDigitalMusic), "Audio", CPF_Config );
		new(Class,"UseSpatial",      RF_Public)UBoolProperty  (CPP_PROPERTY(UseSpatial     ), "Audio", CPF_Config );
		new(Class,"UseReverb",       RF_Public)UBoolProperty  (CPP_PROPERTY(UseReverb      ), "Audio", CPF_Config );
		new(Class,"Use3dHardware",   RF_Public)UBoolProperty  (CPP_PROPERTY(Use3dHardware  ), "Audio", CPF_Config );
		new(Class,"ReverseStereo",   RF_Public)UBoolProperty  (CPP_PROPERTY(ReverseStereo  ), "Audio", CPF_Config );
		new(Class,"LowSoundQuality", RF_Public)UBoolProperty  (CPP_PROPERTY(LowSoundQuality), "Audio", CPF_Config );
		new(Class,"Latency",         RF_Public)UIntProperty   (CPP_PROPERTY(Latency        ), "Audio", CPF_Config );
		new(Class,"OutputRate",      RF_Public)UByteProperty  (CPP_PROPERTY(OutputRate     ), "Audio", CPF_Config, OutputRates );
		new(Class,"EffectsChannels", RF_Public)UIntProperty   (CPP_PROPERTY(EffectsChannels), "Audio", CPF_Config );
		new(Class,"MusicVolume",     RF_Public)UByteProperty  (CPP_PROPERTY(MusicVolume    ), "Audio", CPF_Config );
		new(Class,"SoundVolume",     RF_Public)UByteProperty  (CPP_PROPERTY(SoundVolume    ), "Audio", CPF_Config );
		new(Class,"AmbientFactor",   RF_Public)UFloatProperty (CPP_PROPERTY(AmbientFactor  ), "Audio", CPF_Config );
		new(Class,"DopplerSpeed",    RF_Public)UFloatProperty (CPP_PROPERTY(DopplerSpeed   ), "Audio", CPF_Config );
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Init and teardown.
-----------------------------------------------------------------------------*/

UBOOL UGalaxyAudioSubsystem::Init()
{
	guard(UGalaxyAudioSubsystem::Init);

	EffectsChannels = Clamp( EffectsChannels, 0, MAX_EFFECTS_CHANNELS );

	XmpCtx = xmp_create_context();

#if UNREAL_SDL_AUDIO
	// cross-platform port: open one SDL playback device; SDL mixes all bound
	// streams. Effect voices are created lazily in Update; music is one stream
	// pulled by libxmp in its get-callback (no feeder thread needed).
	if( !SDL_InitSubSystem( SDL_INIT_AUDIO ) )
	{
		debugf( NAME_Init, "Galaxy: SDL audio init failed: %s", SDL_GetError() );
		return 0;
	}
	SDL_AudioSpec DevSpec; DevSpec.format = SDL_AUDIO_F32; DevSpec.channels = 2; DevSpec.freq = MUSIC_RATE;
	AudioDevice = SDL_OpenAudioDevice( SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &DevSpec );
	if( !AudioDevice )
	{
		debugf( NAME_Init, "Galaxy: SDL_OpenAudioDevice failed: %s", SDL_GetError() );
		return 0;
	}
	// Music runs on its own logical device so the effects-device reverb postmix
	// leaves it dry (the original reverbed only samples, not music).
	MusicDevice = SDL_OpenAudioDevice( SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &DevSpec );
	MusicMutex = SDL_CreateMutex();
	SDL_AudioSpec MusicSpec; MusicSpec.format = SDL_AUDIO_S16; MusicSpec.channels = 2; MusicSpec.freq = MUSIC_RATE;
	MusicStream = SDL_CreateAudioStream( &MusicSpec, &DevSpec );
	SDL_SetAudioStreamGetCallback( MusicStream, GalaxyMusicCB, this );
	SDL_BindAudioStream( MusicDevice ? MusicDevice : AudioDevice, MusicStream );

	// Zone reverb: process the effects device's final mix through Freeverb.
	SDL_SetAudioPostmixCallback( AudioDevice, GalaxyReverbPostmix, this );

	SetVolumes();
	USound::Audio = this;
	UMusic::Audio = this;
	Initialized   = 1;
	debugf( NAME_Init, "Galaxy: SDL audio initialized (%i effect channels, libxmp %s music)", EffectsChannels, XMP_VERSION );
	return 1;
#else
	// Bring up XAudio2: mastering voice + one submix each for effects and music,
	// so the two config volumes can be applied as independent master gains.
	CoInitializeEx( NULL, COINIT_MULTITHREADED );
	if( FAILED(XAudio2Create( &XAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR )) )
	{
		debugf( NAME_Init, "Galaxy: XAudio2Create failed; no audio" );
		return 0;
	}
	if( FAILED(XAudio2->CreateMasteringVoice( &MasterVoice, 2, MUSIC_RATE )) )
	{
		debugf( NAME_Init, "Galaxy: CreateMasteringVoice failed; no audio" );
		XAudio2->Release(); XAudio2=NULL;
		return 0;
	}
	if( FAILED(XAudio2->CreateSubmixVoice( &EffectsSubmix, 2, MUSIC_RATE ))
	||	FAILED(XAudio2->CreateSubmixVoice( &MusicSubmix,   2, MUSIC_RATE )) )
	{
		debugf( NAME_Init, "Galaxy: CreateSubmixVoice failed; no audio" );
		if( EffectsSubmix ) { EffectsSubmix->DestroyVoice(); EffectsSubmix=NULL; }
		MasterVoice->DestroyVoice(); MasterVoice=NULL;
		XAudio2->Release(); XAudio2=NULL;
		return 0;
	}

	// Zone reverb: attach the built-in reverb APO to the effects submix,
	// disabled until the listener enters a reverb zone (see Update).
	IUnknown* ReverbAPO = NULL;
	if( SUCCEEDED(XAudio2CreateReverb( &ReverbAPO )) )
	{
		XAUDIO2_EFFECT_DESCRIPTOR Desc = { ReverbAPO, FALSE, 2 };
		XAUDIO2_EFFECT_CHAIN Chain = { 1, &Desc };
		if( SUCCEEDED(EffectsSubmix->SetEffectChain( &Chain )) )
			EffectsSubmix->DisableEffect( 0 );
		ReverbAPO->Release();	// the submix holds its own reference
	}

	// Music: libxmp context, streaming voice, and the feeder thread.
	InitializeCriticalSection( &MusicLock );
	MusicEvent = CreateEvent( NULL, FALSE, FALSE, NULL );
	MusicCallback.Event = MusicEvent;
	WAVEFORMATEX MusicFormat;
	appMemset( &MusicFormat, 0, sizeof(MusicFormat) );
	MusicFormat.wFormatTag      = WAVE_FORMAT_PCM;
	MusicFormat.nChannels       = 2;
	MusicFormat.nSamplesPerSec  = MUSIC_RATE;
	MusicFormat.wBitsPerSample  = 16;
	MusicFormat.nBlockAlign     = 4;
	MusicFormat.nAvgBytesPerSec = MUSIC_RATE*4;
	XAUDIO2_SEND_DESCRIPTOR MusicSend = { 0, MusicSubmix };
	XAUDIO2_VOICE_SENDS MusicSends = { 1, &MusicSend };
	if( FAILED(XAudio2->CreateSourceVoice( &MusicVoice, &MusicFormat, 0, 2.0f, &MusicCallback, &MusicSends )) )
		appErrorf( "Galaxy: CreateSourceVoice for music failed" );
	MusicThreadExit = 0;
	MusicThread = CreateThread( NULL, 0, MusicThreadProc, this, 0, NULL );

	SetVolumes();
	USound::Audio = this;
	UMusic::Audio = this;
	Initialized   = 1;
	debugf( NAME_Init, "Galaxy: XAudio2 audio initialized (%i effect channels, libxmp %s music)", EffectsChannels, XMP_VERSION );
	return 1;
#endif
	unguard;
}

void UGalaxyAudioSubsystem::Destroy()
{
	guard(UGalaxyAudioSubsystem::Destroy);
	if( Initialized )
	{
		USound::Audio = NULL;
		UMusic::Audio = NULL;
		StopAllSound();
		StopMusic();

#if UNREAL_SDL_AUDIO
		// cross-platform port: SDL_DestroyAudioStream waits for the music
		// callback to finish, so it's safe to free xmp afterward.
		if( MusicStream ) { SDL_DestroyAudioStream( MusicStream ); MusicStream=NULL; }
		if( XmpCtx )      { xmp_free_context( XmpCtx ); XmpCtx=NULL; }
		if( AudioDevice ) { SDL_SetAudioPostmixCallback( AudioDevice, NULL, NULL ); SDL_CloseAudioDevice( AudioDevice ); AudioDevice=0; }
		if( MusicDevice ) { SDL_CloseAudioDevice( MusicDevice ); MusicDevice=0; }
		if( MusicMutex )  { SDL_DestroyMutex( MusicMutex ); MusicMutex=NULL; }
#else
		// Stop the feeder thread.
		MusicThreadExit = 1;
		if( MusicEvent )
			SetEvent( MusicEvent );
		// x64 port / security: if the feeder does not actually exit (e.g. it is
		// wedged inside libxmp on a pathological module), tearing down the
		// shared XmpCtx / voice / lock / event it is still touching would be a
		// use-after-free. Only free that state once the thread has provably
		// exited; otherwise deliberately leak it (a bounded, one-shot leak on
		// shutdown is strictly safer than a UAF/heap corruption).
		UBOOL FeederJoined = 1;
		if( MusicThread )
		{
			FeederJoined = (WaitForSingleObject( MusicThread, 5000 )==WAIT_OBJECT_0);
			if( FeederJoined )
			{
				CloseHandle( MusicThread );
				MusicThread = NULL;
			}
			else
				debugf( NAME_Warning, "Galaxy: music feeder did not exit; leaking audio state to avoid UAF" );
		}
		if( FeederJoined )
		{
			if( MusicVoice )	{ MusicVoice->DestroyVoice(); MusicVoice=NULL; }
			if( XmpCtx )		{ xmp_free_context( XmpCtx ); XmpCtx=NULL; }
			if( MusicEvent )	{ CloseHandle( MusicEvent ); MusicEvent=NULL; }
			DeleteCriticalSection( &MusicLock );
		}

		if( EffectsSubmix )	{ EffectsSubmix->DestroyVoice(); EffectsSubmix=NULL; }
		if( MusicSubmix )	{ MusicSubmix->DestroyVoice(); MusicSubmix=NULL; }
		if( MasterVoice )	{ MasterVoice->DestroyVoice(); MasterVoice=NULL; }
		if( XAudio2 )		{ XAudio2->Release(); XAudio2=NULL; }
#endif
		Initialized = 0;
		debugf( NAME_Exit, "Galaxy: audio shut down" );
	}
	UAudioSubsystem::Destroy();
	unguard;
}

void UGalaxyAudioSubsystem::PostEditChange()
{
	guard(UGalaxyAudioSubsystem::PostEditChange);
	// Validate configurable variables (same clamps as the original).
	OutputRate      = Clamp( (INT)OutputRate, 0, 2 );
	Latency         = Clamp( Latency, 10, 250 );
	EffectsChannels = Clamp( EffectsChannels, 0, MAX_EFFECTS_CHANNELS );
	DopplerSpeed    = Clamp( DopplerSpeed, 1.f, 100000.f );
	AmbientFactor   = Clamp( AmbientFactor, 0.f, 10.f );
	SetVolumes();
	unguard;
}

void UGalaxyAudioSubsystem::ShutdownAfterError()
{
	guard(UGalaxyAudioSubsystem::ShutdownAfterError);
	// Crash path: just silence the output; don't attempt an orderly teardown.
#if UNREAL_SDL_AUDIO
	if( Initialized && AudioDevice )
		SDL_PauseAudioDevice( AudioDevice );
#else
	if( Initialized && MasterVoice )
		MasterVoice->SetVolume( 0.f );
	MusicThreadExit = 1;
#endif
	debugf( NAME_Exit, "UGalaxyAudioSubsystem::ShutdownAfterError" );
	unguard;
}

/*-----------------------------------------------------------------------------
	Volumes.
-----------------------------------------------------------------------------*/

void UGalaxyAudioSubsystem::SetVolumes()
{
	guard(UGalaxyAudioSubsystem::SetVolumes);
	// The original mapped the 0..255 config volumes onto the glx 0..127
	// master gains; here they are the submix / stream gains directly.
#if UNREAL_SDL_AUDIO
	// Effects master volume is applied per-stream in Update (SetAudioStreamGain);
	// here just set the music stream gain (folded with the fade).
	if( MusicStream )
		SDL_SetAudioStreamGain( MusicStream, Clamp(MusicVolume/255.f,0.f,1.f) * Max(MusicFade,0.f) );
#else
	if( EffectsSubmix )
		EffectsSubmix->SetVolume( SoundVolume/255.f );
	if( MusicSubmix )
		MusicSubmix->SetVolume( Clamp(MusicVolume/255.f,0.f,1.f) * Max(MusicFade,0.f) );
#endif
	unguard;
}

/*-----------------------------------------------------------------------------
	Viewport.
-----------------------------------------------------------------------------*/

void UGalaxyAudioSubsystem::SetViewport( UViewport* InViewport )
{
	guard(UGalaxyAudioSubsystem::SetViewport);

	// Stop all effects (the original also restarted the output device here;
	// XAudio2 doesn't need the DirectSound focus dance, so the device and
	// voices persist across viewport changes).
	StopAllSound();

	// Force zone reverb to re-resolve for the new viewport/level.
	appMemset( &CurrentReverb, 0, sizeof(CurrentReverb) );
#if UNREAL_SDL_AUDIO
	Reverb.SetActive( 0 );
#endif
	if( Viewport != InViewport )
	{
		if( Viewport )
			StopMusic();
		Viewport = InViewport;
		if( Viewport && Viewport->Actor && Viewport->Actor->Song && Viewport->Actor->Transition==MTRAN_None )
			Viewport->Actor->Transition = MTRAN_Instant;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Command line.
-----------------------------------------------------------------------------*/

UBOOL UGalaxyAudioSubsystem::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UGalaxyAudioSubsystem::Exec);
	const char* Str = Cmd;
	if( ParseCommand(&Str,"CDTRACK") )
	{
		// CD audio isn't carried over; keep the state machine consistent.
		if( Viewport && Viewport->Actor )
		{
			Viewport->Actor->CdTrack    = appAtoi(Str);
			Viewport->Actor->Transition = MTRAN_Instant;
			Out->Log( "CD track set" );
		}
		return 1;
	}
	else if( ParseCommand(&Str,"MUSICORDER") )
	{
		// x64 port: gate on MusicPlaying (the player is actually started), not
		// CurrentMusic (set before StartMusic and left set on a load failure).
		if( MusicPlaying )
		{
			GALAXY_MLOCK();
			xmp_set_position( XmpCtx, appAtoi(Str) );
			GALAXY_MUNLOCK();
			Out->Log( "Music order set" );
		}
		return 1;
	}
	return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Sound registration.
-----------------------------------------------------------------------------*/

void UGalaxyAudioSubsystem::RegisterSound( USound* Sound )
{
	guard(UGalaxyAudioSubsystem::RegisterSound);
	check(Initialized);
	if( !Sound->Handle )
	{
		check(Sound->Data.Num()>0);

		// Parse the in-memory WAV with the engine's own reader (which now
		// clamps the data size to the bytes actually present).
		FWaveModInfo Wave;
		if( !Wave.ReadWaveInfo( Sound->Data ) || Wave.SampleDataSize<=0 )
		{
			debugf( NAME_Warning, "Galaxy: invalid sound format in %s", Sound->GetFullName() );
			Sound->Data.Empty();
			return;
		}

		// x64 port / security: the WAV format fields are untrusted. Accept only
		// the PCM shapes retail content uses (mono/stereo, 8/16-bit, sane rate)
		// so downstream buffer/loop math can't be driven to a bad state, and so
		// the XAudio2 WAVEFORMATEX (nBlockAlign = Channels*Bits/8) is valid.
		INT Rate = *Wave.pSamplesPerSec, Bits = *Wave.pBitsPerSample, Channels = *Wave.pChannels;
		if( (Channels!=1 && Channels!=2) || (Bits!=8 && Bits!=16) || Rate<1000 || Rate>192000 )
		{
			debugf( NAME_Warning, "Galaxy: unsupported WAV format (%i ch, %i bit, %i Hz) in %s", Channels, Bits, Rate, Sound->GetFullName() );
			Sound->Data.Empty();
			return;
		}

		FGalaxySample* Sample = (FGalaxySample*)appMalloc( sizeof(FGalaxySample), "GalaxySample" );
		Sample->Rate      = Rate;
		Sample->Bits      = Bits;
		Sample->Channels  = Channels;
		Sample->NumBytes  = Wave.SampleDataSize;
		Sample->LoopStart = -1;
		Sample->LoopEnd   = -1;
		// x64 port / security: validate loop points against the actual frame
		// count so LoopBegin/LoopLength handed to XAudio2 stay in range.
		INT FrameCount = Sample->NumBytes / (Channels * Bits/8);
		if( Wave.SampleLoopsNum>0 && Wave.pSampleLoop )
		{
			INT LoopStart = Wave.pSampleLoop->dwStart, LoopEnd = Wave.pSampleLoop->dwEnd;
			if( LoopStart>=0 && LoopEnd>LoopStart && LoopEnd<=FrameCount )
			{
				Sample->LoopStart = LoopStart;
				Sample->LoopEnd   = LoopEnd;
			}
		}
		Sample->Pcm = (BYTE*)appMalloc( Sample->NumBytes, "GalaxyPcm" );
		appMemcpy( Sample->Pcm, Wave.SampleDataStart, Sample->NumBytes );
		Sound->Handle = Sample;

		// As in the original: the raw file bytes aren't needed in-game.
		if( !GIsEditor )
			Sound->Data.Empty();
	}
	unguard;
}

void UGalaxyAudioSubsystem::UnregisterSound( USound* Sound )
{
	guard(UGalaxyAudioSubsystem::UnregisterSound);
	if( Sound->Handle )
	{
		// Stop this sound on all playing channels. x64 port: scan the whole
		// array, not just EffectsChannels -- lowering the channel count in the
		// options mid-game would otherwise strand live voices above the new
		// count, and freeing their Pcm below would be a use-after-free.
		for( INT i=0; i<MAX_EFFECTS_CHANNELS; i++ )
			if( PlayingSounds[i].Sound==Sound )
				StopSound( i );
		FGalaxySample* Sample = (FGalaxySample*)Sound->Handle;
		appFree( Sample->Pcm );
		appFree( Sample );
		Sound->Handle = NULL;
	}
	unguard;
}

void UGalaxyAudioSubsystem::RegisterMusic( UMusic* Music )
{
	guard(UGalaxyAudioSubsystem::RegisterMusic);
	// As in the original: music is loaded lazily by the Update state machine.
	unguard;
}

void UGalaxyAudioSubsystem::UnregisterMusic( UMusic* Music )
{
	guard(UGalaxyAudioSubsystem::UnregisterMusic);
	if( Music->Handle )
	{
		check(Music==CurrentMusic);
		StopMusic();
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Effects channels.
-----------------------------------------------------------------------------*/

void UGalaxyAudioSubsystem::StopSound( INT Index )
{
	guard(UGalaxyAudioSubsystem::StopSound);
#if UNREAL_SDL_AUDIO
	if( PlayingSounds[Index].Stream )
	{
		// SDL_DestroyAudioStream unbinds and waits for the callback to finish.
		SDL_DestroyAudioStream( PlayingSounds[Index].Stream );
		PlayingSounds[Index].Stream = NULL;
	}
#else
	if( PlayingSounds[Index].Voice )
	{
		// x64 port: stop + flush BEFORE destroying, mirroring the music teardown.
		// The source buffer's pAudioData points straight at Sample->Pcm, which
		// UnregisterSound frees on level travel; DestroyVoice alone left a window
		// where XAudio2's audio thread was still reading that buffer -> access
		// violation in XAudio2_9.dll when going from one map to the next.
		PlayingSounds[Index].Voice->Stop( 0 );
		PlayingSounds[Index].Voice->FlushSourceBuffers();
		PlayingSounds[Index].Voice->DestroyVoice();
		PlayingSounds[Index].Voice = NULL;
	}
#endif
	PlayingSounds[Index] = FPlayingSound();
	unguard;
}

void UGalaxyAudioSubsystem::StopAllSound()
{
	guard(UGalaxyAudioSubsystem::StopAllSound);
	for( INT i=0; i<MAX_EFFECTS_CHANNELS; i++ )
		StopSound( i );
	unguard;
}

UBOOL UGalaxyAudioSubsystem::PlaySound
(
	AActor*	Actor,
	INT		Id,
	USound*	Sound,
	FVector	Location,
	FLOAT	Volume,
	FLOAT	Radius,
	FLOAT	Pitch
)
{
	guard(UGalaxyAudioSubsystem::PlaySound);
	check(Radius);
	if( !Viewport )
		return 0;

	// Sounds with no slot get a fresh unique (negative) slot so they never
	// interrupt each other.
	if( (Id&14)==2*SLOT_None )
		Id = 16 * --FreeSlot;

	// Compute priority.
	FLOAT Priority = SoundPriority( Location, Volume, Radius );

	// If already playing, stop it (same actor+slot, ignoring the
	// bNoOverride bit); otherwise find the lowest-priority channel.
	INT   Index        = -1;
	FLOAT BestPriority = Priority;
	for( INT i=0; i<EffectsChannels; i++ )
	{
		FPlayingSound& Playing = PlayingSounds[i];
		if( (Playing.Id&~1)==(Id&~1) )
		{
			// Skip if not interruptable.
			if( Id&1 )
				return 0;

			// Stop the sound.
			Index = i;
			StopSound( i );
			break;
		}
		else if( Playing.Priority<=BestPriority )
		{
			Index        = i;
			BestPriority = Playing.Priority;
		}
	}

	// If no sound, or its priority is overruled, stop it.
	if( !Sound || Index==-1 )
		return 0;

	// x64 port: when STEALING an occupied lowest-priority channel, stop its
	// live voice before overwriting the slot. The original could overwrite
	// freely because Galaxy addressed hardware channels by slot index
	// (glxStartSample on channel Index implicitly replaced the old sample);
	// our per-slot IXAudio2SourceVoice is owned by the struct, so overwriting
	// without DestroyVoice leaked a still-playing voice -- looping ambients
	// then played forever untracked (immune to the out-of-range stop) while
	// the ambient scan stacked fresh copies on top, and the leaked voice's
	// buffer outlived Sample->Pcm across level travel (XAudio2-thread AV).
	if( PlayingSounds[Index].Id )
		StopSound( Index );

	// Put the sound on the play-list; the voice starts in the next Update.
	PlayingSounds[Index] = FPlayingSound( Actor, Id, Sound, Location, Volume, Radius, Pitch, Priority );
	return 1;
	unguard;
}

void UGalaxyAudioSubsystem::NoteDestroy( AActor* Actor )
{
	guard(UGalaxyAudioSubsystem::NoteDestroy);
	check(Actor);
	check(Actor->IsValid());
	// x64 port: whole array, matching UnregisterSound (see comment there).
	for( INT i=0; i<MAX_EFFECTS_CHANNELS; i++ )
	{
		if( PlayingSounds[i].Actor==Actor )
		{
			if( (PlayingSounds[i].Id&14)==SLOT_Ambient*2 )
			{
				// Stop ambient sound when actor dies.
				StopSound( i );
			}
			else
			{
				// Unbind regular sounds from actors.
				PlayingSounds[i].Actor = NULL;
			}
		}
	}
	unguard;
}

UBOOL UGalaxyAudioSubsystem::GetLowQualitySetting()
{
	guard(UGalaxyAudioSubsystem::GetLowQualitySetting);
	return LowSoundQuality;
	unguard;
}

/*-----------------------------------------------------------------------------
	Per-frame update.
-----------------------------------------------------------------------------*/

void UGalaxyAudioSubsystem::Update( FPointRegion Region, FCoords& Coords )
{
	guard(UGalaxyAudioSubsystem::Update);
	// x64 port: also require a valid listener actor -- during level travel the
	// viewport briefly has no Actor, and the ambient scan / spatialization below
	// dereference Viewport->Actor (XLevel, Location).
	if( !Viewport || !Viewport->Actor || !Initialized )
		return;

	// Time passes...
	DOUBLE DeltaTime = appSeconds() - LastTime;
	LastTime += DeltaTime;
	DeltaTime = Clamp( DeltaTime, 0.0, 1.0 );

	// See if any new ambient sounds need to be started.
	// x64 port: in the editor, never auto-start level ambient sounds — the
	// realtime editor viewports would otherwise play the map's ambients while
	// you edit. (Explicit sound-browser preview via PlaySound is unaffected.)
	UBOOL Realtime = Viewport->IsRealtime();
	if( Realtime && !GIsEditor )
	{
		guard(StartAmbience);
		for( INT i=0; i<Viewport->Actor->XLevel->Num(); i++ )
		{
			AActor* Actor = Viewport->Actor->XLevel->Actors(i);
			if
			(	Actor
			&&	Actor->AmbientSound
			&&	FDistSquared(Viewport->Actor->Location,Actor->Location)<=Square(Actor->WorldSoundRadius()) )
			{
				INT Id = Actor->GetIndex()*16 + SLOT_Ambient*2;
				INT j;
				for( j=0; j<EffectsChannels; j++ )
					if( PlayingSounds[j].Id==Id )
						break;
				if( j==EffectsChannels )
					PlaySound( Actor, Id, Actor->AmbientSound, Actor->Location, AmbientFactor*Actor->SoundVolume/255.f, Actor->WorldSoundRadius(), Actor->SoundPitch/64.f );
			}
		}
		unguard;
	}

	// Update all playing ambient sounds.
	guard(UpdateAmbience);
	for( INT i=0; i<EffectsChannels; i++ )
	{
		FPlayingSound& Playing = PlayingSounds[i];
		if( (Playing.Id&14)==SLOT_Ambient*2 )
		{
			check(Playing.Actor);
			if
			(	FDistSquared(Viewport->Actor->Location,Playing.Actor->Location)>Square(Playing.Actor->WorldSoundRadius())
			||	Playing.Actor->AmbientSound!=Playing.Sound
			||  !Realtime )
			{
				// Ambient sound went out of range.
				StopSound( i );
			}
			else
			{
				// Update basic sound properties. As in the original driver,
				// light-emitting ambients are modulated by the light's ANIMATED
				// brightness curve (flicker averages ~0.4x and pulses, blink /
				// strobe gate to silence) — without this, torch ambients play
				// at constant full volume and overwhelm the mix. (x64 port:
				// this call was initially omitted; restored after audit.)
				FLOAT Brightness = 2.f * (AmbientFactor * Playing.Actor->SoundVolume/255.f);
				if( Playing.Actor->LightType!=LT_None )
				{
					Brightness *= Playing.Actor->LightBrightness/255.f;
					if( Viewport->Client && Viewport->Client->Engine && Viewport->Client->Engine->Render )
					{
						FPlane Color;
						Viewport->Client->Engine->Render->GlobalLighting( (Viewport->Actor->ShowFlags & SHOW_PlayerCtrl)!=0, Playing.Actor, Brightness, Color );
					}
				}
				Playing.Volume = Brightness;
				Playing.Radius = Playing.Actor->WorldSoundRadius();
				Playing.Pitch  = Playing.Actor->SoundPitch/64.f;
			}
		}
	}
	unguard;

	// Update all active sounds.
	guard(UpdateSounds);
	for( INT Index=0; Index<EffectsChannels; Index++ )
	{
		FPlayingSound& Playing = PlayingSounds[Index];
		if( Playing.Id==0 )
			continue;

		// Check if the voice finished. Once the callback has stopped feeding
		// (Finished), wait for SDL to drain the already-queued audio before
		// destroying the stream -- otherwise the tail of short one-shots
		// (pickups, explosion decays) is dropped. Re-pan/re-pitch is skipped
		// meanwhile (the sound content is done; only the tail is playing out).
#if UNREAL_SDL_AUDIO
		if( Playing.Stream && Playing.Finished )
		{
			if( SDL_GetAudioStreamAvailable( Playing.Stream )<=0 )
				StopSound( Index );
			continue;
		}
#else
		if( Playing.Voice )
		{
			XAUDIO2_VOICE_STATE State;
			Playing.Voice->GetState( &State, XAUDIO2_VOICE_NOSAMPLESPLAYED );
			if( State.BuffersQueued==0 )
			{
				StopSound( Index );
				continue;
			}
		}
#endif

		// Update positioning from actor, if available.
		if( Playing.Actor )
		{
			Playing.Location = Playing.Actor->Location;
			Playing.Priority = SoundPriority( Playing.Location, Playing.Volume, Playing.Radius );
		}

		// Spatialize into the listener frame (original glx pan/volume math).
		FVector Local     = Playing.Location.TransformPointBy( Coords );
		FLOAT   Size      = Local.Size();
		FLOAT   PanAngle  = (FLOAT)atan2( Local.X, Abs(Local.Z) );
		FLOAT   CenterDist= 0.1f*Playing.Radius;
		if( Local.SizeSquared() < Square(CenterDist) && CenterDist>0.f )
			PanAngle *= Size / CenterDist;

		// Pan01: 0=hard left, 0.5=center, 1=hard right; +-7/8 sweep as original.
		FLOAT Pan01 = Clamp( 0.5f + PanAngle*(7.f/8.f)/(FLOAT)PI, 0.f, 1.f );
		if( ReverseStereo )
			Pan01 = 1.f - Pan01;
		FLOAT Attenuation = Clamp( 1.f - Size/Playing.Radius, 0.f, 1.f );
		FLOAT Gain        = Clamp( Playing.Volume * Attenuation, 0.f, 1.f );

		// Doppler shift from the emitting actor's radial velocity.
		FLOAT Doppler = 1.f;
		if( Playing.Actor )
		{
			FLOAT V = Playing.Actor->Velocity | (Playing.Actor->Location - Viewport->Actor->Location).SafeNormal();
			Doppler = Clamp( 1.f - V/DopplerSpeed, 0.5f, 2.f );
		}

		FGalaxySample* Sample = GetSample( Playing.Sound );
		if( !Sample )
		{
			StopSound( Index );
			continue;
		}

		// x64 port: NaN guard. On level travel a carried-over / glitching actor can
		// have a NaN Location or Velocity, which makes the pan/gain/Doppler math NaN
		// (x!=x). Feeding NaN into XAudio2's SetOutputMatrix/SetFrequencyRatio crashes
		// its audio thread in the mixer (X3DAudioCalculate) -- the map-to-map crash.
		// Drop any voice whose spatialization went non-finite instead of submitting it.
		if( Gain!=Gain || Pan01!=Pan01 || Doppler!=Doppler || Size!=Size )
		{
			StopSound( Index );
			continue;
		}

		// Equal-power stereo gains.
		FLOAT LeftGain  = Gain * (FLOAT)cos( Pan01 * PI/2.f );
		FLOAT RightGain = Gain * (FLOAT)sin( Pan01 * PI/2.f );

#if UNREAL_SDL_AUDIO
		// cross-platform port: SDL stream + get-callback per voice. (Finished is
		// handled drain-aware at the top of the loop; don't tear down here or the
		// tail gets clipped.)
		if( !Playing.Stream )
		{
			SDL_AudioSpec In; In.format = SDL_AUDIO_F32; In.channels = 2; In.freq = Sample->Rate;
			SDL_AudioSpec Out; Out.format = SDL_AUDIO_F32; Out.channels = 2; Out.freq = MUSIC_RATE;
			Playing.Stream = SDL_CreateAudioStream( &In, &Out );
			if( !Playing.Stream ) { StopSound( Index ); continue; }
			Playing.SamplePtr = Sample;
			Playing.Cursor    = 0;
			Playing.Finished  = 0;
			Playing.CbLeft    = LeftGain;
			Playing.CbRight   = RightGain;
			SDL_SetAudioStreamGetCallback( Playing.Stream, GalaxyEffectsCB, &Playing );
			SDL_BindAudioStream( AudioDevice, Playing.Stream );
			Playing.bStarted = 1;
		}
		// Live pan (read by the callback) + master effects volume + pitch.
		Playing.CbLeft  = LeftGain;
		Playing.CbRight = RightGain;
		SDL_SetAudioStreamGain( Playing.Stream, SoundVolume/255.f );
		SDL_SetAudioStreamFrequencyRatio( Playing.Stream, Clamp( Playing.Pitch*Doppler, 0.05f, 4.f ) );
#else
		if( !Playing.Voice )
		{
			// Start the voice.
			guard(StartVoice);
			WAVEFORMATEX Format;
			appMemset( &Format, 0, sizeof(Format) );
			Format.wFormatTag      = WAVE_FORMAT_PCM;
			Format.nChannels       = Sample->Channels;
			Format.nSamplesPerSec  = Sample->Rate;
			Format.wBitsPerSample  = Sample->Bits;
			Format.nBlockAlign     = Sample->Channels * Sample->Bits/8;
			Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;
			XAUDIO2_SEND_DESCRIPTOR Send  = { 0, EffectsSubmix };
			XAUDIO2_VOICE_SENDS     Sends = { 1, &Send };
			if( FAILED(XAudio2->CreateSourceVoice( &Playing.Voice, &Format, 0, 4.0f, NULL, &Sends )) )
			{
				StopSound( Index );
				continue;
			}
			XAUDIO2_BUFFER Buffer;
			appMemset( &Buffer, 0, sizeof(Buffer) );
			Buffer.AudioBytes = Sample->NumBytes;
			Buffer.pAudioData = Sample->Pcm;
			Buffer.Flags      = XAUDIO2_END_OF_STREAM;
			if( Sample->LoopStart>=0 && Sample->LoopEnd>Sample->LoopStart )
			{
				Buffer.LoopBegin  = Sample->LoopStart;
				Buffer.LoopLength = Sample->LoopEnd - Sample->LoopStart;
				Buffer.LoopCount  = XAUDIO2_LOOP_INFINITE;
			}
			Playing.Voice->SubmitSourceBuffer( &Buffer );
			// x64 port (audit): do NOT Start yet — the per-frame parameters
			// below must be applied first, or the first samples render at the
			// voice's default full volume (audible blip on every sound start).
			unguard;
		}

		// Apply the per-frame parameters.
		UBOOL JustCreated = !Playing.bStarted;
		Playing.Voice->SetVolume( Gain );
		if( Sample->Channels==1 )
		{
			FLOAT Matrix[2] = { Attenuation>0.f && Gain>0.f ? LeftGain/Gain : 0.707f, Attenuation>0.f && Gain>0.f ? RightGain/Gain : 0.707f };
			Playing.Voice->SetOutputMatrix( EffectsSubmix, 1, 2, Matrix );
		}
		Playing.Voice->SetFrequencyRatio( Clamp( Playing.Pitch*Doppler, 0.05f, 4.f ) );
		if( JustCreated )
		{
			Playing.Voice->Start( 0 );
			Playing.bStarted = 1;
		}
#endif
	}
	unguard;

	// Update music (the original's fade-out-then-start state machine).
	// x64 port: skip the level-music state machine in the editor so a loaded
	// map's Song doesn't start playing while editing (browser music preview,
	// which calls StartMusic directly, still works).
	guard(UpdateMusic);
	APlayerPawn* PlayerActor = Viewport->Actor;
	if( PlayerActor && PlayerActor->Transition!=MTRAN_None && !GIsEditor )
	{
		UBOOL ChangeMusic = (CurrentMusic != PlayerActor->Song);
		if( !ChangeMusic && MusicPlaying && CurrentMusic
		&&	PlayerActor->SongSection!=CurrentSection && PlayerActor->SongSection!=255 )
		{
			// x64 port: same song already playing, only the section changed —
			// reposition the live module instead of fading out and reloading
			// it from scratch (the original did this via GLX_SETPOSITION; the
			// port's StartMusic tears down + reloads, which drops a beat).
			CurrentSection = PlayerActor->SongSection;
			GALAXY_MLOCK();
			if( XmpCtx )
				xmp_set_position( XmpCtx, CurrentSection );
			GALAXY_MUNLOCK();
			PlayerActor->Transition = MTRAN_None;
		}
		else if( CurrentMusic || CurrentCDTrack!=255 )
		{
			// Fade out the old music first.
			UBOOL Ready = 0;
			if( CurrentSection==255 )
				Ready = 1;
			else if( PlayerActor->Transition==MTRAN_Fade )
				{ MusicFade -= DeltaTime*1.0f; Ready = MusicFade < -1.0f*2.f*Latency/1000.f; }
			else if( PlayerActor->Transition==MTRAN_SlowFade )
				{ MusicFade -= DeltaTime*0.2f; Ready = MusicFade < -0.2f*2.f*Latency/1000.f; }
			else if( PlayerActor->Transition==MTRAN_FastFade )
				{ MusicFade -= DeltaTime*3.0f; Ready = MusicFade < -3.0f*2.f*Latency/1000.f; }
			else
				Ready = 1;
			if( Ready )
			{
				if( ChangeMusic && CurrentMusic )
					StopMusic();
				CurrentMusic   = NULL;
				CurrentCDTrack = 255;
			}
			else
				SetVolumes();
		}
		else
		{
			// Start the new music.
			MusicFade = 1.0;
			SetVolumes();
			CurrentMusic   = PlayerActor->Song;
			CurrentCDTrack = PlayerActor->CdTrack;
			CurrentSection = PlayerActor->SongSection;
			if( CurrentMusic && UseDigitalMusic )
				StartMusic( CurrentMusic, CurrentSection );
			// CD audio (UseCDMusic) is not carried over in this port.
			PlayerActor->Transition = MTRAN_None;
		}
	}
	unguard;

	// Zone reverb: resolve the listener zone's reverb settings and push them to
	// the backend when they change (mirrors the original's memcmp change guard).
	guard(UpdateReverb);
	{
		FZoneReverbState New;
		appMemset( &New, 0, sizeof(New) );
		AZoneInfo* Zone = (Viewport && Viewport->Actor) ? Viewport->Actor->Region.Zone : NULL;
		if( UseReverb && Zone && Zone->bReverbZone )
		{
			New.On   = 1;
			New.Wet  = Clamp( Zone->MasterGain/255.f, 0.f, 1.f );
			New.Damp = Clamp( 1.f - Zone->CutoffHz/11025.f, 0.f, 0.9f );
			INT DSum = 0;
			for( INT i=0; i<6; i++ )
				DSum += Zone->Delay[i];
			New.Room = Clamp( 0.55f + (DSum/6.f)/255.f*0.4f, 0.1f, 0.96f );
		}
		if( appMemcmp( &New, &CurrentReverb, sizeof(New) )!=0 )
		{
			CurrentReverb = New;
#if UNREAL_SDL_AUDIO
			Reverb.SetParams( New.Room, New.Damp, New.Wet );
			Reverb.SetActive( New.On );
#else
			if( EffectsSubmix )
			{
				XAUDIO2FX_REVERB_I3DL2_PARAMETERS I3DL2 = XAUDIO2FX_I3DL2_PRESET_STONEROOM;
				I3DL2.WetDryMix = New.Wet*100.f;
				XAUDIO2FX_REVERB_PARAMETERS Native;
				ReverbConvertI3DL2ToNative( &I3DL2, &Native );
				EffectsSubmix->SetEffectParameters( 0, &Native, sizeof(Native) );
				if( New.On )
					EffectsSubmix->EnableEffect( 0 );
				else
					EffectsSubmix->DisableEffect( 0 );
			}
#endif
		}
	}
	unguard;

	unguard;
}

/*-----------------------------------------------------------------------------
	Music playback (libxmp -> streaming XAudio2 voice).
-----------------------------------------------------------------------------*/

void UGalaxyAudioSubsystem::StartMusic( UMusic* Song, BYTE Section )
{
	guard(UGalaxyAudioSubsystem::StartMusic);
	GALAXY_MLOCK();
	if( MusicPlaying )
	{
		MusicPlaying = 0;
#if !UNREAL_SDL_AUDIO
		MusicVoice->Stop( 0 );
		MusicVoice->FlushSourceBuffers();
#endif
		xmp_end_player( XmpCtx );
		xmp_release_module( XmpCtx );
	}
	// The retail songs are S3M and Impulse Tracker modules embedded in the
	// UMusic Data (music Data is never emptied, unlike sounds).
	UBOOL Ok = 0;
	if( Song->Data.Num()>0 && xmp_load_module_from_memory( XmpCtx, &Song->Data(0), Song->Data.Num() )==0 )
	{
		if( xmp_start_player( XmpCtx, MUSIC_RATE, 0 )==0 )
		{
			if( Section!=255 && Section!=0 )
				xmp_set_position( XmpCtx, Section );
			Ok = 1;
		}
		else
			xmp_release_module( XmpCtx );
	}
	if( Ok && Section!=255 )
	{
		Song->Handle = (void*)-1;
		MusicPlaying = 1;
#if !UNREAL_SDL_AUDIO
		MusicNextBuffer = 0;	// x64 port: resync the double-buffer index on each start
		MusicVoice->Start( 0 );
		SetEvent( MusicEvent );	// kick the feeder
#endif
		debugf( "Galaxy: playing music %s (section %i)", Song->GetFullName(), Section );
	}
	else
	{
		Song->Handle = Ok ? (void*)-1 : NULL;
		if( !Ok )
			debugf( NAME_Warning, "Galaxy: failed to load music %s", Song->GetFullName() );
	}
	GALAXY_MUNLOCK();
	unguard;
}

void UGalaxyAudioSubsystem::StopMusic()
{
	guard(UGalaxyAudioSubsystem::StopMusic);
	GALAXY_MLOCK();
	if( MusicPlaying )
	{
		MusicPlaying = 0;
#if !UNREAL_SDL_AUDIO
		MusicVoice->Stop( 0 );
		MusicVoice->FlushSourceBuffers();
#endif
		xmp_end_player( XmpCtx );
		xmp_release_module( XmpCtx );
	}
	if( CurrentMusic )
	{
		CurrentMusic->Handle = NULL;
		CurrentMusic = NULL;
	}
	GALAXY_MUNLOCK();
	unguard;
}

#if !UNREAL_SDL_AUDIO
// Feeder: keep MUSIC_NUM_BUFFERS render-ahead buffers queued on the voice.
// (SDL builds pull music directly in GalaxyMusicCB — no feeder thread.)
void UGalaxyAudioSubsystem::MusicFeed()
{
	for( ; ; )
	{
		WaitForSingleObject( MusicEvent, 100 );
		if( MusicThreadExit )
			return;
		EnterCriticalSection( &MusicLock );
		if( MusicPlaying )
		{
			XAUDIO2_VOICE_STATE State;
			MusicVoice->GetState( &State, XAUDIO2_VOICE_NOSAMPLESPLAYED );
			while( State.BuffersQueued < MUSIC_NUM_BUFFERS )
			{
				BYTE* Buf = MusicBuffers[MusicNextBuffer];
				MusicNextBuffer = (MusicNextBuffer+1) % MUSIC_NUM_BUFFERS;
				if( xmp_play_buffer( XmpCtx, Buf, MUSIC_BUFFER_BYTES, 0 )!=0 )
				{
					// Module ended (shouldn't happen with loop=forever).
					MusicPlaying = 0;
					break;
				}
				XAUDIO2_BUFFER Buffer;
				memset( &Buffer, 0, sizeof(Buffer) );
				Buffer.AudioBytes = MUSIC_BUFFER_BYTES;
				Buffer.pAudioData = Buf;
				MusicVoice->SubmitSourceBuffer( &Buffer );
				MusicVoice->GetState( &State, XAUDIO2_VOICE_NOSAMPLESPLAYED );
			}
		}
		LeaveCriticalSection( &MusicLock );
	}
}

DWORD WINAPI UGalaxyAudioSubsystem::MusicThreadProc( LPVOID Param )
{
	((UGalaxyAudioSubsystem*)Param)->MusicFeed();
	return 0;
}
#endif // !UNREAL_SDL_AUDIO (feeder thread; SDL pulls music in GalaxyMusicCB)

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

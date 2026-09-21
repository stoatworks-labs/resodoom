/*
	ResodoomEngine.h -- the C ABI between the plugin and one loaded engine copy.

	This header is shared by two targets that must NOT share C++ runtime state:
	the FFGL plugin (C++) and the engine shared library (C, statically linked
	against doomgeneric). Keep it C, keep it POD, and keep it versioned.

	**One loaded copy of the library is one Doom instance.** doomgeneric is a
	single pile of file-scope globals -- there is no instance handle to pass and
	no way to make one without forking the engine. The plugin therefore copies
	the library to a uniquely-named temp file and dlopens *that* once per plugin
	instance, which is the only arrangement that gives two layers two separate
	games. See AGENTS.md, "Two instances of one engine share globals".
*/
#ifndef RESODOOM_ENGINE_H
#define RESODOOM_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever anything below changes shape. The plugin refuses a library
   that does not match rather than reading a struct with a different layout. */
#define RESODOOM_ABI_VERSION 4u

/* Doom's own framebuffer, 1:1. doomgeneric is built with DOOMGENERIC_RESX/RESY
   set to these, so its internal integer scaler is a straight copy and every
   scaling decision belongs to the plugin's shader instead. */
#define RESODOOM_WIDTH  320
#define RESODOOM_HEIGHT 200
#define RESODOOM_BYTES  ( RESODOOM_WIDTH * RESODOOM_HEIGHT * 4 )

/* Doom's tic rate. Not the composition's frame rate, and deliberately not
   derived from it. */
#define RESODOOM_TICRATE 35

typedef enum ResodoomState
{
	RESODOOM_STARTING = 0, /* thread up, D_DoomMain has not produced a tic yet */
	RESODOOM_RUNNING  = 1, /* at least one frame has been published */
	RESODOOM_FAILED   = 2, /* the engine called exit(); status() says why       */
	RESODOOM_STOPPED  = 3  /* torn down                                        */
} ResodoomState;

typedef struct ResodoomConfig
{
	const char* iwad;     /* absolute path to the IWAD. Required.            */
	const char* pwad;     /* optional PWAD, or NULL                          */
	const char* savedir;  /* writable directory for saves/config, or NULL    */
	int         skill;    /* 1..5, or 0 to leave Doom's default              */
	int         episode;  /* 1..4  } both non-zero to warp straight into a   */
	int         map;      /* 1..32 } map and skip the title screen           */
	int         heapMiB;  /* zone size; 0 means doomgeneric's default (16)   */
	int         deterministic; /* 1: virtual clock, no wall-clock sleeping   */
} ResodoomConfig;

typedef struct ResodoomFrame
{
	uint32_t seq;               /* increments once per published tic         */
	uint32_t tic;               /* engine tic count                          */
	uint8_t  pixels[ RESODOOM_BYTES ]; /* BGRA, bottom-up, alpha forced to 255 */
} ResodoomFrame;

/*
	The vtable. `resodoom_engine_api` is the only exported symbol the plugin
	looks up; everything else is reached through here so adding a call does not
	mean another dlsym.
*/
typedef struct ResodoomEngineApi
{
	uint32_t abiVersion;

	/* Spawns the engine thread. Returns 0 on success. Does not block waiting
	   for Doom to start -- poll State() for that. */
	int ( *Start )( const ResodoomConfig* cfg );

	/* Stops the engine thread and releases every allocation it made. Safe to
	   call in any state, including after a failure. */
	void ( *Stop )( void );

	ResodoomState ( *State )( void );

	/* Human-readable status, never NULL. After RESODOOM_FAILED this is the
	   engine's own error text -- usually the only thing that names a missing
	   or malformed WAD. */
	const char* ( *Status )( void );

	/* Allow the engine to run `tics` more tics. The engine runs no faster than
	   it is granted, so this is where the composition's clock meets Doom's. */
	void ( *Grant )( int tics );

	/* Copies the most recent published frame. Returns 1 if `out->seq` changed
	   since the caller's `lastSeq`, 0 if there is nothing new. Never blocks. */
	int ( *Frame )( ResodoomFrame* out, uint32_t lastSeq );

	/* Queue a key edge. `doomKey` is a doomkeys.h code. */
	void ( *Key )( int doomKey, int down );

	/* Release every held key -- used when a clip is deactivated so the player
	   does not keep walking into a wall forever. */
	void ( *ReleaseAll )( void );

	/*
		How many times the engine has run out of budget and parked, counted
		once per episode rather than once per wakeup.

		This is the handshake that makes stepping deterministic. "Grant a tic,
		then take the next frame that appears" samples whenever the engine
		happens to have published, which is a race: Doom often draws several
		frames while spending one tic's budget, so which one the caller sees
		depends on thread timing and two identical runs diverge. Waiting for
		this counter to move instead means the engine has spent everything it
		was given and stopped, and there is exactly one frame it can be
		showing. The harness relies on it; the plugin has no use for it.
	*/
	uint32_t ( *Parks )( void );
} ResodoomEngineApi;

/* The single exported entry point. The engine library is built with hidden
   visibility so that none of doomgeneric's several hundred globals reach the
   host's symbol namespace -- this attribute is what pokes the one hole. */
#if defined( _WIN32 )
	#define RESODOOM_EXPORT __declspec( dllexport )
#else
	#define RESODOOM_EXPORT __attribute__( ( visibility( "default" ) ) )
#endif

RESODOOM_EXPORT const ResodoomEngineApi* resodoom_engine_api( void );

typedef const ResodoomEngineApi* ( *resodoom_engine_api_fn )( void );
#define RESODOOM_ENGINE_ENTRY "resodoom_engine_api"

#ifdef __cplusplus
}
#endif

#endif /* RESODOOM_ENGINE_H */

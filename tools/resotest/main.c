/*
	resotest -- the engine, on the CPU, with no graphics API anywhere.

	It loads the shipped engine library the way the plugin does: by copying it
	to a uniquely-named file and dlopening *that*. So what this exercises is
	the artefact, through the arrangement it actually ships in, rather than a
	relinked copy of the same code.

	  resotest --iwad W.wad --check          the self-test
	  resotest --iwad W.wad --tics 200 --out /tmp/f.ppm
	  resotest --iwad W.wad --tics 400 --seq /tmp/f_     a PPM per frame

	Every run here sets `deterministic`, which freezes the engine's only link
	to real time. Two cold runs are then byte-identical and a pixel digest is a
	stable thing to assert against.
*/
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ResodoomEngine.h"

#ifndef RESODOOM_ENGINE_DEFAULT_PATH
	#define RESODOOM_ENGINE_DEFAULT_PATH "./libresodoom_engine.dylib"
#endif

static int g_failures = 0;
static int g_checks   = 0;

static void ok( int condition, const char* what )
{
	g_checks += 1;
	printf( condition ? "  ok    %s\n" : "  FAIL  %s\n", what );
	if( !condition )
		g_failures += 1;
}

/* ------------------------------------------------------------------ */
/* Loading a private copy                                              */
/* ------------------------------------------------------------------ */

typedef struct EngineRef
{
	void*                    dl;
	char                     path[ 1024 ];
	const ResodoomEngineApi* api;
} EngineRef;

static int g_copyCounter = 0;

/*
	dlopen keys on path: opening a library that is already loaded returns the
	SAME image with a bumped refcount, not a second copy of its globals. Doom
	is nothing but globals, so every instance needs its own file on disk. This
	is the same trick, and the same reason, as cartridge's `uniqueInstance`.
*/
static int engine_open( const char* source, EngineRef* out )
{
	memset( out, 0, sizeof( *out ) );
	snprintf( out->path, sizeof( out->path ), "/tmp/resodoom-engine-%d-%d.dylib",
			  (int)getpid(), g_copyCounter++ );

	FILE* in = fopen( source, "rb" );
	if( !in )
	{
		fprintf( stderr, "resotest: cannot read engine '%s'\n", source );
		return 0;
	}
	FILE* cp = fopen( out->path, "wb" );
	if( !cp )
	{
		fprintf( stderr, "resotest: cannot write '%s'\n", out->path );
		fclose( in );
		return 0;
	}

	char   buf[ 65536 ];
	size_t n;
	while( ( n = fread( buf, 1, sizeof( buf ), in ) ) > 0 )
		fwrite( buf, 1, n, cp );
	fclose( in );
	fclose( cp );
	chmod( out->path, 0755 );

	out->dl = dlopen( out->path, RTLD_NOW | RTLD_LOCAL );
	if( !out->dl )
	{
		fprintf( stderr, "resotest: cannot load '%s': %s\n", out->path, dlerror() );
		unlink( out->path );
		return 0;
	}

	resodoom_engine_api_fn entry =
		(resodoom_engine_api_fn)dlsym( out->dl, RESODOOM_ENGINE_ENTRY );
	if( !entry )
	{
		fprintf( stderr, "resotest: no %s in the engine\n", RESODOOM_ENGINE_ENTRY );
		dlclose( out->dl );
		unlink( out->path );
		return 0;
	}

	out->api = entry();
	if( !out->api || out->api->abiVersion != RESODOOM_ABI_VERSION )
	{
		fprintf( stderr, "resotest: ABI mismatch -- engine %u, harness %u\n",
				 out->api ? out->api->abiVersion : 0u, RESODOOM_ABI_VERSION );
		dlclose( out->dl );
		unlink( out->path );
		return 0;
	}
	return 1;
}

static void engine_close( EngineRef* e )
{
	if( e->api )
		e->api->Stop();
	if( e->dl )
		dlclose( e->dl );
	if( e->path[ 0 ] )
		unlink( e->path );
	memset( e, 0, sizeof( *e ) );
}

/* ------------------------------------------------------------------ */

static uint64_t digest( const ResodoomFrame* f )
{
	uint64_t h = 1469598103934665603ull; /* FNV-1a */
	for( size_t i = 0; i < RESODOOM_BYTES; ++i )
	{
		h ^= f->pixels[ i ];
		h *= 1099511628211ull;
	}
	return h;
}

static ResodoomConfig base_config( const char* iwad )
{
	ResodoomConfig cfg = { 0 };
	cfg.iwad           = iwad;
	cfg.heapMiB        = 16;
	cfg.deterministic  = 1;
	return cfg;
}

/*
	Grants `tics` tics one at a time, waiting for a frame after each. Returns
	how many frames arrived, or -1 if the engine failed or went quiet.

	The grant-then-poll shape is the plugin's in miniature, and like the plugin
	it never waits on the engine thread for anything: in Resolume the caller is
	the render thread, and a stall there is a dropped show.
*/
static int run( const ResodoomEngineApi* api, int tics, ResodoomFrame* out,
				void ( *onFrame )( const ResodoomFrame*, void* ), void* user )
{
	uint32_t lastSeq = 0;
	int      frames  = 0;

	for( int i = 0; i < tics; ++i )
	{
		/*
			Grant a tic, then wait for the engine to SPEND it and park again.

			Waiting for "a frame appeared" instead is the obvious version and
			it is not reproducible: Doom draws several frames while spending
			one tic's worth of budget, so the frame the caller catches depends
			on how the two threads interleave, and two identical runs disagree
			on the last one. Waiting for the park counter means the engine has
			stopped with nothing left to spend, and there is exactly one frame
			it can be showing.
		*/
		uint32_t parked = api->Parks();
		api->Grant( 1 );

		int spins = 0;
		while( api->Parks() == parked )
		{
			if( api->State() == RESODOOM_FAILED )
			{
				fprintf( stderr, "resotest: %s\n", api->Status() );
				return -1;
			}
			if( ++spins > 100000 ) /* 10 s; the first tic is all of D_DoomMain */
			{
				fprintf( stderr, "resotest: engine never parked after tic %d\n", i );
				return -1;
			}
			usleep( 100 );
		}

		if( api->Frame( out, lastSeq ) )
		{
			lastSeq = out->seq;
			frames += 1;
			if( onFrame )
				onFrame( out, user );
		}
	}
	return frames;
}

/* ------------------------------------------------------------------ */

static int write_ppm( const char* path, const ResodoomFrame* f )
{
	FILE* fp = fopen( path, "wb" );
	if( !fp )
	{
		fprintf( stderr, "resotest: cannot write '%s'\n", path );
		return 0;
	}
	fprintf( fp, "P6\n%d %d\n255\n", RESODOOM_WIDTH, RESODOOM_HEIGHT );

	/* ResodoomFrame is bottom-up BGRA, shaped for a texture upload. PPM is
	   top-down RGB, so un-flip here and the file looks like the screen. */
	for( int y = RESODOOM_HEIGHT - 1; y >= 0; --y )
	{
		const uint8_t* row = f->pixels + (size_t)y * RESODOOM_WIDTH * 4;
		for( int x = 0; x < RESODOOM_WIDTH; ++x )
		{
			const uint8_t* px = row + (size_t)x * 4;
			fputc( px[ 2 ], fp );
			fputc( px[ 1 ], fp );
			fputc( px[ 0 ], fp );
		}
	}
	fclose( fp );
	return 1;
}

typedef struct SeqWriter
{
	const char* prefix;
	int         index;
} SeqWriter;

static void seq_write( const ResodoomFrame* f, void* user )
{
	SeqWriter* w = (SeqWriter*)user;
	char       path[ 1024 ];
	snprintf( path, sizeof( path ), "%s%05d.ppm", w->prefix, w->index++ );
	write_ppm( path, f );
}

/* ------------------------------------------------------------------ */

static void check_alpha_and_ink( const ResodoomFrame* f )
{
	int      opaque    = 1;
	uint32_t seen[ 16 ];
	int      seenCount = 0;

	for( size_t i = 0; i < RESODOOM_BYTES; i += 4 )
	{
		if( f->pixels[ i + 3 ] != 0xFF )
			opaque = 0;

		uint32_t rgb = (uint32_t)f->pixels[ i ] | ( (uint32_t)f->pixels[ i + 1 ] << 8 )
					 | ( (uint32_t)f->pixels[ i + 2 ] << 16 );
		int known = 0;
		for( int k = 0; k < seenCount; ++k )
			if( seen[ k ] == rgb )
			{
				known = 1;
				break;
			}
		if( !known && seenCount < 16 )
			seen[ seenCount++ ] = rgb;
	}

	/* The X in XRGB8888 is undefined and Doom leaves stale bits in it. Passed
	   through, Resolume gets a mostly-transparent layer -- which on a dark
	   composition looks exactly like a plugin that does nothing. */
	ok( opaque, "every pixel is opaque (the undefined X is forced to 255)" );

	/* A flat frame is the failure every other check here misses: the engine
	   can start, tick and publish perfectly while drawing nothing at all. */
	ok( seenCount >= 8, "the frame is real picture, not one flat colour" );
}

static int self_test( const char* enginePath, const char* iwad )
{
	printf( "resotest: engine %s\n", enginePath );
	printf( "resotest: iwad   %s\n\n", iwad );

	ResodoomFrame* frame = (ResodoomFrame*)malloc( sizeof( ResodoomFrame ) );
	uint64_t       digestA = 0;
	uint32_t       ticA    = 0;

	/* --- a private copy loads at all ------------------------------- */
	EngineRef e;
	if( !engine_open( enginePath, &e ) )
	{
		free( frame );
		return 1;
	}
	ok( 1, "a private copy of the engine loads and its ABI matches" );

	/* --- a bad start is refused, and does not burn the copy -------- */
	{
		ResodoomConfig bad = { 0 };
		ok( e.api->Start( &bad ) != 0, "Start with no IWAD is refused" );
		ok( e.api->State() == RESODOOM_FAILED, "...and the engine says FAILED" );
		ok( e.api->Status() && e.api->Status()[ 0 ], "...with a message naming why" );
	}

	/* --- a real run ------------------------------------------------ */
	{
		ResodoomConfig cfg = base_config( iwad );
		ok( e.api->Start( &cfg ) == 0, "Start with a real IWAD is accepted" );

		int frames = run( e.api, 120, frame, NULL, NULL );
		if( frames < 0 )
		{
			ok( 0, "the engine runs 120 tics" );
			engine_close( &e );
			free( frame );
			printf( "\nresotest: %d checks, %d failures\n", g_checks, g_failures );
			return 1;
		}

		ok( e.api->State() == RESODOOM_RUNNING, "the engine reaches RUNNING" );
		ok( frames == 120, "a frame arrives for every granted tic" );

		/*
			Frames and tics are different counts and must not be asserted as if
			they were one: Doom redraws on its own schedule -- menus and screen
			wipes repaint without advancing the game -- so it publishes rather
			more frames than it runs tics. What has to hold is the direction.
		*/
		ok( frame->tic <= 121, "the engine never runs past its granted budget" );
		ok( frame->tic >= 30, "...and spends a real fraction of it" );

		check_alpha_and_ink( frame );

		digestA = digest( frame );
		ticA    = frame->tic;
	}

	/* --- the same copy refuses a second run ------------------------ */
	{
		e.api->Stop();
		ok( e.api->State() == RESODOOM_STOPPED, "Stop parks the engine at STOPPED" );

		ResodoomConfig cfg = base_config( iwad );
		ok( e.api->Start( &cfg ) != 0,
			"a second Start in the same loaded copy is refused" );

		/*
			This is a real constraint stated as a test, not a limitation being
			apologised for. Stop() frees every allocation but cannot put
			doomgeneric's globals back, so a restarted copy would run partway
			into D_DoomMain and then quietly produce nothing -- indistinguishable
			from a bad WAD. Refusing it is what keeps that off the debugger.
		*/
		ok( strstr( e.api->Status(), "fresh copy" ) != NULL,
			"...and says to load a fresh copy instead" );
	}
	engine_close( &e );

	/* --- a fresh copy runs identically ----------------------------- */
	{
		EngineRef e2;
		if( !engine_open( enginePath, &e2 ) )
		{
			free( frame );
			return 1;
		}
		ok( 1, "a second private copy loads alongside the first" );

		ResodoomConfig cfg = base_config( iwad );
		ok( e2.api->Start( &cfg ) == 0, "the fresh copy starts" );

		int frames = run( e2.api, 120, frame, NULL, NULL );
		ok( frames == 120, "and publishes the same number of frames" );

		/*
			Two cold runs, byte for byte. This is the check that catches the
			clock drifting anywhere near real time: the instant a wall-clock
			reading reaches the engine the attract demo diverges and this fails.
		*/
		ok( digest( frame ) == digestA && frame->tic == ticA,
			"two cold runs render identically (the clock really is virtual)" );

		engine_close( &e2 );
	}

	/* --- input reaches the game ------------------------------------ */
	{
		EngineRef e3;
		if( !engine_open( enginePath, &e3 ) )
		{
			free( frame );
			return 1;
		}

		ResodoomConfig cfg = base_config( iwad );
		e3.api->Start( &cfg );
		run( e3.api, 60, frame, NULL, NULL );
		uint64_t before = digest( frame );

		/* Escape opens the menu over whatever is on screen. It is the cheapest
		   input whose effect shows in a pixel digest without depending on
		   which WAD this is or what the demo happens to be doing. */
		e3.api->Key( 27, 1 );
		run( e3.api, 2, frame, NULL, NULL );
		e3.api->Key( 27, 0 );
		run( e3.api, 20, frame, NULL, NULL );

		ok( digest( frame ) != before, "a queued key changes what the engine draws" );

		e3.api->ReleaseAll();
		engine_close( &e3 );
	}

	free( frame );
	printf( "\nresotest: %d checks, %d failures\n", g_checks, g_failures );
	return g_failures == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static void usage( void )
{
	fprintf( stderr,
			 "usage: resotest --iwad PATH [--check]\n"
			 "                [--tics N] [--out FILE.ppm] [--seq PREFIX]\n"
			 "                [--engine PATH] [--warp E M] [--skill N]\n" );
}

int main( int argc, char** argv )
{
	const char* enginePath = RESODOOM_ENGINE_DEFAULT_PATH;
	const char* iwad       = NULL;
	const char* out        = NULL;
	const char* seqPrefix  = NULL;
	int         tics       = 100;
	int         doCheck    = 0;
	int         episode = 0, map = 0, skill = 0;

	for( int i = 1; i < argc; ++i )
	{
		if( !strcmp( argv[ i ], "--check" ) )
			doCheck = 1;
		else if( !strcmp( argv[ i ], "--iwad" ) && i + 1 < argc )
			iwad = argv[ ++i ];
		else if( !strcmp( argv[ i ], "--engine" ) && i + 1 < argc )
			enginePath = argv[ ++i ];
		else if( !strcmp( argv[ i ], "--out" ) && i + 1 < argc )
			out = argv[ ++i ];
		else if( !strcmp( argv[ i ], "--seq" ) && i + 1 < argc )
			seqPrefix = argv[ ++i ];
		else if( !strcmp( argv[ i ], "--tics" ) && i + 1 < argc )
			tics = atoi( argv[ ++i ] );
		else if( !strcmp( argv[ i ], "--skill" ) && i + 1 < argc )
			skill = atoi( argv[ ++i ] );
		else if( !strcmp( argv[ i ], "--warp" ) && i + 2 < argc )
		{
			episode = atoi( argv[ ++i ] );
			map     = atoi( argv[ ++i ] );
		}
		else
		{
			usage();
			return 2;
		}
	}

	if( !iwad )
	{
		usage();
		return 2;
	}

	if( doCheck )
		return self_test( enginePath, iwad );

	EngineRef e;
	if( !engine_open( enginePath, &e ) )
		return 1;

	ResodoomConfig cfg = base_config( iwad );
	cfg.episode        = episode;
	cfg.map            = map;
	cfg.skill          = skill;

	if( e.api->Start( &cfg ) != 0 )
	{
		fprintf( stderr, "resotest: %s\n", e.api->Status() );
		engine_close( &e );
		return 1;
	}

	ResodoomFrame* frame  = (ResodoomFrame*)malloc( sizeof( ResodoomFrame ) );
	SeqWriter      writer = { seqPrefix, 0 };
	int            frames =
		run( e.api, tics, frame, seqPrefix ? seq_write : NULL, seqPrefix ? &writer : NULL );

	if( frames < 0 )
	{
		free( frame );
		engine_close( &e );
		return 1;
	}

	printf( "resotest: %d frames, last seq %u, tic %u\n", frames, frame->seq, frame->tic );
	if( out )
		write_ppm( out, frame );

	free( frame );
	engine_close( &e );
	return 0;
}

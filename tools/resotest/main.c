/*
	resotest -- the engine, on the CPU, with no graphics API anywhere.

	It loads the shipped engine library the way the plugin does: by copying it
	to a uniquely-named file and dlopening *that*. So what this exercises is
	the artefact, through the arrangement it actually ships in, rather than a
	relinked copy of the same code.

	  resotest --iwad W.wad --check          the self-test
	  resotest --iwad W.wad --tics 200 --out /tmp/f.ppm
	  resotest --iwad W.wad --tics 400 --seq /tmp/f_     a PPM per frame
	  resotest --iwad W.wad --tics 35 --menu --out /tmp/m.ppm    the main menu

	Every run here sets the `deterministic` option, which freezes the engine's
	only link to real time. Two cold runs are then byte-identical and a pixel
	digest is a stable thing to assert against.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stagehand/SourceAbi.h"

/*
	The four platform things this harness needs: load a library by path, delete
	a file, sleep briefly, and name a scratch file nothing else will pick.

	Written out here rather than taken from stagehand on purpose -- the point
	of this harness is to exercise the shipped engine through the raw loader,
	with no part of the C++ side in the picture. See the note above
	engine_open().
*/
#if defined( _WIN32 )

	/* See EngineThread.h: LEAN_AND_MEAN drops the RPC and OLE headers, which
	   are where the SDK's `boolean` collides with Doom's. */
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
	#include <io.h>
	#include <process.h>

	#define RD_LIB_EXT ".dll"

	static void* rd_dlopen( const char* path )
	{
		return (void*)LoadLibraryA( path );
	}
	static void* rd_dlsym( void* handle, const char* symbol )
	{
		/* Through uintptr_t: a FARPROC is a function pointer and the direct
		   cast to void* is what MSVC objects to, not the conversion. */
		return (void*)(uintptr_t)GetProcAddress( (HMODULE)handle, symbol );
	}
	static void rd_dlclose( void* handle ) { FreeLibrary( (HMODULE)handle ); }

	static const char* rd_dlerror( void )
	{
		static char text[ 64 ];
		snprintf( text, sizeof( text ), "Windows error %lu",
				  (unsigned long)GetLastError() );
		return text;
	}

	static void rd_unlink( const char* path ) { _unlink( path ); }

	/* Nothing to do: Windows decides executability from the file, not a mode. */
	static void rd_make_loadable( const char* path ) { (void)path; }

	static void rd_sleep_us( unsigned usec )
	{
		/* Sleep takes milliseconds and Sleep(0) yields without waiting, which
		   would spin this harness's poll loop at full tilt. */
		Sleep( usec < 1000u ? 1u : usec / 1000u );
	}

	static int rd_pid( void ) { return _getpid(); }

	static void rd_scratch_dir( char* out, size_t size )
	{
		/* GetTempPathA includes the trailing separator; callers append. */
		if( GetTempPathA( (DWORD)size, out ) == 0 )
			snprintf( out, size, ".\\" );
	}

#else

	#include <dlfcn.h>
	#include <sys/stat.h>
	#include <unistd.h>

	#if defined( __APPLE__ )
		#define RD_LIB_EXT ".dylib"
	#else
		#define RD_LIB_EXT ".so"
	#endif

	static void* rd_dlopen( const char* path )
	{
		return dlopen( path, RTLD_NOW | RTLD_LOCAL );
	}
	static void* rd_dlsym( void* handle, const char* symbol )
	{
		return dlsym( handle, symbol );
	}
	static void        rd_dlclose( void* handle ) { dlclose( handle ); }
	static const char* rd_dlerror( void ) { return dlerror(); }
	static void        rd_unlink( const char* path ) { unlink( path ); }
	static void        rd_make_loadable( const char* path ) { chmod( path, 0755 ); }
	static void        rd_sleep_us( unsigned usec ) { usleep( usec ); }
	static int         rd_pid( void ) { return (int)getpid(); }

	static void rd_scratch_dir( char* out, size_t size )
	{
		snprintf( out, size, "/tmp/" );
	}

#endif

#ifndef RESODOOM_ENGINE_DEFAULT_PATH
	#define RESODOOM_ENGINE_DEFAULT_PATH "./libresodoom_engine" RD_LIB_EXT
#endif

/*
	The geometry the build asked for, checked against the engine's own
	Describe() rather than assumed -- the two disagreeing is exactly the sort
	of thing that produces a sheared picture and no error message.

	These come from CMake, the same two numbers the engine was compiled with,
	so the check stays a real one at any width. Writing 320 here instead would
	make it fail on a widescreen engine that was behaving perfectly; reading
	the engine's own answer back would make it pass on one that was not.
*/
#ifndef RESODOOM_SCREEN_WIDTH
	#define RESODOOM_SCREEN_WIDTH 320
#endif
#ifndef RESODOOM_SCREEN_HEIGHT
	#define RESODOOM_SCREEN_HEIGHT 200
#endif

#define RESODOOM_WIDTH  RESODOOM_SCREEN_WIDTH
#define RESODOOM_HEIGHT RESODOOM_SCREEN_HEIGHT
#define RESODOOM_BYTES  ( RESODOOM_WIDTH * RESODOOM_HEIGHT * 4 )

static int g_failures = 0;
static int g_checks   = 0;

static void ok( int condition, const char* what )
{
	g_checks += 1;
	printf( condition ? "  ok    %s\n" : "  FAIL  %s\n", what );
	if( !condition )
		g_failures += 1;
}

/*
	The harness's own frame. The ABI hands back raw pixels plus two numbers, so
	what used to be a struct in a shared header is now local to each side --
	which is the point of an ABI that does not dictate a layout.
*/
typedef struct Frame
{
	uint32_t seq;
	uint32_t tic;
	uint8_t  pixels[ RESODOOM_BYTES ];
} Frame;

/* ------------------------------------------------------------------ */
/* Loading a private copy                                              */
/* ------------------------------------------------------------------ */

typedef struct EngineRef
{
	void*                     dl;
	char                      path[ 1024 ];
	const StagehandSourceApi* api;
} EngineRef;

static int g_copyCounter = 0;

/*
	dlopen keys on path: opening a library that is already loaded returns the
	SAME image with a bumped refcount, not a second copy of its globals. Doom
	is nothing but globals, so every instance needs its own file on disk. This
	is what stagehand::Sidecar does for the plugin; doing it by hand here keeps
	the harness free of any dependency on the C++ side.
*/
static int engine_open( const char* source, EngineRef* out )
{
	char scratch[ 512 ];
	rd_scratch_dir( scratch, sizeof( scratch ) );

	memset( out, 0, sizeof( *out ) );
	snprintf( out->path, sizeof( out->path ), "%sresodoom-engine-%d-%d" RD_LIB_EXT,
			  scratch, rd_pid(), g_copyCounter++ );

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
	rd_make_loadable( out->path );

	out->dl = rd_dlopen( out->path );
	if( !out->dl )
	{
		fprintf( stderr, "resotest: cannot load '%s': %s\n", out->path, rd_dlerror() );
		rd_unlink( out->path );
		return 0;
	}

	stagehand_source_api_fn entry =
		(stagehand_source_api_fn)rd_dlsym( out->dl, STAGEHAND_SOURCE_ENTRY );
	if( !entry )
	{
		fprintf( stderr, "resotest: no %s in the engine\n", STAGEHAND_SOURCE_ENTRY );
		rd_dlclose( out->dl );
		rd_unlink( out->path );
		return 0;
	}

	out->api = entry();
	if( !out->api || out->api->abiVersion != STAGEHAND_ABI_VERSION )
	{
		fprintf( stderr, "resotest: ABI mismatch -- engine %u, harness %u\n",
				 out->api ? out->api->abiVersion : 0u, STAGEHAND_ABI_VERSION );
		rd_dlclose( out->dl );
		rd_unlink( out->path );
		return 0;
	}
	return 1;
}

static void engine_close( EngineRef* e )
{
	if( e->api )
		e->api->Close();
	if( e->dl )
		rd_dlclose( e->dl );
	if( e->path[ 0 ] )
		rd_unlink( e->path );
	memset( e, 0, sizeof( *e ) );
}

/* ------------------------------------------------------------------ */

static uint64_t digest( const Frame* f )
{
	uint64_t h = 1469598103934665603ull; /* FNV-1a */
	for( size_t i = 0; i < RESODOOM_BYTES; ++i )
	{
		h ^= f->pixels[ i ];
		h *= 1099511628211ull;
	}
	return h;
}

/* Builds the option list every run shares. `store` holds the formatted ints. */
typedef struct Options
{
	StagehandOption opts[ 8 ];
	int             count;
	char            skill[ 8 ];
	char            episode[ 8 ];
	char            map[ 8 ];
} Options;

static void build_options( Options* o, const char* iwad, int skill, int episode, int map )
{
	memset( o, 0, sizeof( *o ) );
	snprintf( o->skill, sizeof( o->skill ), "%d", skill );
	snprintf( o->episode, sizeof( o->episode ), "%d", episode );
	snprintf( o->map, sizeof( o->map ), "%d", map );

	o->opts[ o->count++ ] = ( StagehandOption ){ "iwad", iwad };
	o->opts[ o->count++ ] = ( StagehandOption ){ "heap", "16" };
	/* Freezes the engine's only link to real time -- see the file header. */
	o->opts[ o->count++ ] = ( StagehandOption ){ "deterministic", "1" };
	o->opts[ o->count++ ] = ( StagehandOption ){ "skill", o->skill };
	o->opts[ o->count++ ] = ( StagehandOption ){ "episode", o->episode };
	o->opts[ o->count++ ] = ( StagehandOption ){ "map", o->map };
}

static int engine_start( EngineRef* e, const char* iwad, int skill, int episode, int map )
{
	Options o;
	build_options( &o, iwad, skill, episode, map );
	return e->api->Open( o.opts, o.count ) == 0;
}

/*
	Grants `tics` tics one at a time, waiting after each for the engine to
	SPEND it and park. Returns how many frames arrived, or -1 on failure.

	Waiting for "a frame appeared" instead is the obvious version and it is not
	reproducible: Doom draws several frames while spending one tic's budget, so
	the frame the caller catches depends on how the two threads interleave, and
	two identical runs disagree on the last one. Waiting for the park counter
	means the engine has stopped with nothing left to spend, and there is
	exactly one frame it can be showing.
*/
static int run( const StagehandSourceApi* api, int tics, Frame* out,
				void ( *onFrame )( const Frame*, void* ), void* user )
{
	uint32_t lastSeq = 0;
	int      frames  = 0;

	for( int i = 0; i < tics; ++i )
	{
		uint32_t parked = api->Parks();
		api->Grant( 1 );

		int spins = 0;
		while( api->Parks() == parked )
		{
			if( api->State() == STAGEHAND_FAILED )
			{
				fprintf( stderr, "resotest: %s\n", api->Status() );
				return -1;
			}
			if( ++spins > 100000 ) /* 10 s; the first tic is all of D_DoomMain */
			{
				fprintf( stderr, "resotest: engine never parked after tic %d\n", i );
				return -1;
			}
			rd_sleep_us( 100 );
		}

		if( api->Frame( out->pixels, sizeof( out->pixels ), lastSeq, &out->seq, &out->tic ) )
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

static int write_ppm( const char* path, const Frame* f )
{
	FILE* fp = fopen( path, "wb" );
	if( !fp )
	{
		fprintf( stderr, "resotest: cannot write '%s'\n", path );
		return 0;
	}
	fprintf( fp, "P6\n%d %d\n255\n", RESODOOM_WIDTH, RESODOOM_HEIGHT );

	/* Published frames are bottom-up BGRA, shaped for a texture upload. PPM is
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

static void seq_write( const Frame* f, void* user )
{
	SeqWriter* w = (SeqWriter*)user;
	char       path[ 1024 ];
	snprintf( path, sizeof( path ), "%s%05d.ppm", w->prefix, w->index++ );
	write_ppm( path, f );
}

/* ------------------------------------------------------------------ */

static void check_alpha_and_ink( const Frame* f )
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
	   through, Resolume gets a mostly-transparent layer -- which against a dark
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

	Frame*   frame   = (Frame*)malloc( sizeof( Frame ) );
	uint64_t digestA = 0;
	uint32_t ticA    = 0;

	/* --- a private copy loads at all ------------------------------- */
	EngineRef e;
	if( !engine_open( enginePath, &e ) )
	{
		free( frame );
		return 1;
	}
	ok( 1, "a private copy of the engine loads and its ABI matches" );

	/* --- it describes itself the way the plugin expects ------------ */
	{
		StagehandInfo info;
		memset( &info, 0, sizeof( info ) );
		e.api->Describe( &info );

		ok( info.width == RESODOOM_WIDTH && info.height == RESODOOM_HEIGHT,
			"Describe reports Doom's geometry" );
		ok( info.frameBytes == RESODOOM_BYTES, "...and its frame size" );
		ok( info.rateNumerator == 35 && info.rateDenominator == 1,
			"...and 35 Hz as an exact fraction" );

		/*
			Pixel aspect is WIDTH over HEIGHT, so Doom's is 5/6 and less than
			one. The reciprocal is just as natural to write and gives a 1.92
			display aspect -- a picture wider than 16:9, and every face in the
			game stretched -- so the direction is asserted, not just the value.
		*/
		ok( info.pixelAspect > 0.83f && info.pixelAspect < 0.84f,
			"...and a pixel aspect of 5/6, not its reciprocal" );
	}

	/* --- a bad start is refused, and does not burn the copy -------- */
	{
		ok( e.api->Open( NULL, 0 ) != 0, "Open with no WAD is refused" );
		ok( e.api->State() == STAGEHAND_FAILED, "...and the engine says FAILED" );
		ok( e.api->Status() && e.api->Status()[ 0 ], "...with a message naming why" );
	}

	/* --- a real run ------------------------------------------------ */
	{
		ok( engine_start( &e, iwad, 0, 0, 0 ), "Open with a real WAD is accepted" );

		int frames = run( e.api, 120, frame, NULL, NULL );
		if( frames < 0 )
		{
			ok( 0, "the engine runs 120 tics" );
			engine_close( &e );
			free( frame );
			printf( "\nresotest: %d checks, %d failures\n", g_checks, g_failures );
			return 1;
		}

		ok( e.api->State() == STAGEHAND_RUNNING, "the engine reaches RUNNING" );
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
		e.api->Close();
		ok( e.api->State() == STAGEHAND_CLOSED, "Close parks the engine at CLOSED" );

		ok( !engine_start( &e, iwad, 0, 0, 0 ),
			"a second Open in the same loaded copy is refused" );

		/*
			A real constraint stated as a test, not a limitation being
			apologised for. Close() frees every allocation but cannot put
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
		ok( engine_start( &e2, iwad, 0, 0, 0 ), "the fresh copy starts" );

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

		engine_start( &e3, iwad, 0, 0, 0 );
		run( e3.api, 60, frame, NULL, NULL );
		uint64_t before = digest( frame );

		/* Escape opens the menu over whatever is on screen. It is the cheapest
		   input whose effect shows in a pixel digest without depending on
		   which WAD this is or what the demo happens to be doing. */
		e3.api->Event( 27, 1 );
		run( e3.api, 2, frame, NULL, NULL );
		e3.api->Event( 27, 0 );
		run( e3.api, 20, frame, NULL, NULL );

		ok( digest( frame ) != before, "a queued key changes what the engine draws" );

		e3.api->ReleaseInputs();
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
			 "                [--engine PATH] [--warp E M] [--skill N] [--menu]\n" );
}

int main( int argc, char** argv )
{
	const char* enginePath = RESODOOM_ENGINE_DEFAULT_PATH;
	const char* iwad       = NULL;
	const char* out        = NULL;
	const char* seqPrefix  = NULL;
	int         tics       = 100;
	int         doCheck    = 0;
	int         openMenu   = 0;
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
		else if( !strcmp( argv[ i ], "--menu" ) )
			openMenu = 1;
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

	if( !engine_start( &e, iwad, skill, episode, map ) )
	{
		fprintf( stderr, "resotest: %s\n", e.api->Status() );
		engine_close( &e );
		return 1;
	}

	Frame*    frame  = (Frame*)malloc( sizeof( Frame ) );
	SeqWriter writer = { seqPrefix, 0 };
	int       frames =
		run( e.api, tics, frame, seqPrefix ? seq_write : NULL, seqPrefix ? &writer : NULL );

	if( frames < 0 )
	{
		free( frame );
		engine_close( &e );
		return 1;
	}

	/*
		Open Doom's main menu over whatever is on screen, then let it settle.

		The only way to look at the menu layer without a controller -- and the
		menu is 320-wide artwork positioned in 320-wide coordinates, so it is
		exactly what a non-classic build needs looking at. Same sequence as
		the self-test's key check: down, a couple of tics, up, then enough
		tics for the skull cursor to be drawn.
	*/
	if( openMenu )
	{
		e.api->Event( 27, 1 );
		run( e.api, 2, frame, NULL, NULL );
		e.api->Event( 27, 0 );
		if( run( e.api, 20, frame, NULL, NULL ) < 0 )
		{
			free( frame );
			engine_close( &e );
			return 1;
		}
	}

	printf( "resotest: %d frames, last seq %u, tic %u\n", frames, frame->seq, frame->tic );
	if( out )
		write_ppm( out, frame );

	free( frame );
	engine_close( &e );
	return 0;
}

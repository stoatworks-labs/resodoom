/*
	EngineImpl.c -- doomgeneric's platform layer, plus the C ABI the plugin
	calls. This file IS the engine shared library, together with the upstream
	.c files it is linked against.

	Read AGENTS.md before changing the clock or the teardown path. The short
	version of both:

	**The clock is virtual, and that is the whole design.** Doom decides how
	many tics to run by asking what time it is (`I_GetTime` is
	`DG_GetTicksMs() * 35 / 1000`). If that answered with the wall clock, the
	game would run at Doom's rate regardless of what the composition is doing,
	and the only way to slow it down, pause it or step it in a test would be to
	lie about the time anyway. So there is no real clock here at all: the
	consumer releases a budget of tics, the engine spends it, and speed, pause,
	single-stepping and determinism all fall out of that one rule.

	The budget is spent in `DG_SleepMs`, which is the only place Doom yields.
	Two more obvious-looking places to put it both deadlock -- the comment
	there says exactly how, because each one costs an hour to rediscover.

	**`doomgeneric_Create` does NOT loop.** Despite the name it runs the whole
	of D_DoomMain plus one tick and then returns; the host owns the loop from
	there. Teardown is therefore an ordinary `break`, and the longjmp exists
	only for the engine's own `exit()` path (see ResodoomHooks.h).
*/

#include <stdarg.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "stagehand/SourceAbi.h"
#include "ResodoomHooks.h"

/*
	This translation unit IS the hook layer, so here the names have to mean
	what libc means by them. The build force-includes ResodoomHooks.h into every
	file including this one, and that happens before line 1, so undoing it is
	the file's first real act. Without these six lines every hook below calls
	itself. See the note in ResodoomHooks.h.
*/
#undef exit
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup

/*
	Threading, and deliberately below the #undefs rather than up with the other
	includes: on Windows this pulls in windows.h, which is far too large to
	parse with `malloc` and `free` still rewritten out from under it.
*/
#include "EngineThread.h"

/*
	Doom's own geometry and rate. These used to live in a bespoke header shared
	with the plugin; now the plugin asks through Describe(), so they belong
	here with the engine that actually decides them.

	The geometry comes from the build -- the same two numbers that reach Doom's
	renderer and doomgeneric's output buffer, set once in CMakeLists.txt. The
	fallbacks are Doom's own, so this file still compiles if it is ever built
	outside that project.
*/
#ifndef RESODOOM_SCREEN_WIDTH
	#define RESODOOM_SCREEN_WIDTH 320
#endif
#ifndef RESODOOM_SCREEN_HEIGHT
	#define RESODOOM_SCREEN_HEIGHT 200
#endif

#define RESODOOM_WIDTH   RESODOOM_SCREEN_WIDTH
#define RESODOOM_HEIGHT  RESODOOM_SCREEN_HEIGHT
#define RESODOOM_BYTES   ( RESODOOM_WIDTH * RESODOOM_HEIGHT * 4 )
#define RESODOOM_TICRATE 35

/*
	Pixel aspect: WIDTH over HEIGHT, which for Doom is 5/6 and not 6/5.

	320x200 was always displayed at 4:3, so its pixels are 1.2 times TALLER
	than they are wide -- and the ratio of a pixel that is taller than it is
	wide is less than one. Writing 1.2 here gives a 1.92 display aspect, which
	is wider than 16:9, and every face in the game comes out stretched.

	**It does not move with the buffer width.** A Doom pixel is the same shape
	however many of them there are, and the display aspect follows from the
	count: (W / H) * 5/6 is 4:3 at 320 wide and 16:9 at 426. Scaling this to
	"correct" for a wider buffer corrects a second time for something the
	arithmetic has already handled.
*/
#define RESODOOM_PIXEL_ASPECT ( 5.0f / 6.0f )

/* doomgeneric's own entry points. Declared here rather than including
   doomgeneric.h so this file does not inherit DOOMGENERIC_RESX's default. */
extern void doomgeneric_Create( int argc, char** argv );
extern void doomgeneric_Tick( void );
extern uint32_t* DG_ScreenBuffer;

/* ------------------------------------------------------------------ */
/* Allocation registry                                                 */
/* ------------------------------------------------------------------ */

/*
	Every block the engine allocates carries a 32-byte header and joins a
	doubly-linked list. 32 bytes rather than the 24 the fields need, so the
	payload keeps 16-byte alignment -- Doom's zone code casts freely and an
	under-aligned block is the kind of fault that only shows on one CPU.
*/
#define RESODOOM_ALLOC_MAGIC 0x52444F4Du /* 'RDOM' */

typedef struct AllocHeader
{
	uint32_t            magic;
	uint32_t            pad;
	size_t              size;
	struct AllocHeader* prev;
	struct AllocHeader* next;
} AllocHeader;

_Static_assert( sizeof( AllocHeader ) % 16 == 0,
				"allocation header must preserve 16-byte payload alignment" );

static AllocHeader*    g_allocs      = NULL;
static rd_mutex        g_allocLock   = RD_MUTEX_STATIC_INIT;
static size_t          g_allocBytes  = 0;

static void* header_to_payload( AllocHeader* h ) { return (void*)( h + 1 ); }

static AllocHeader* payload_to_header( void* p )
{
	AllocHeader* h = ( (AllocHeader*)p ) - 1;
	return h->magic == RESODOOM_ALLOC_MAGIC ? h : NULL;
}

static void alloc_track( AllocHeader* h, size_t size )
{
	h->magic = RESODOOM_ALLOC_MAGIC;
	h->size  = size;
	rd_mutex_lock( &g_allocLock );
	h->prev = NULL;
	h->next = g_allocs;
	if( g_allocs )
		g_allocs->prev = h;
	g_allocs     = h;
	g_allocBytes += size;
	rd_mutex_unlock( &g_allocLock );
}

static void alloc_untrack( AllocHeader* h )
{
	rd_mutex_lock( &g_allocLock );
	if( h->prev )
		h->prev->next = h->next;
	else if( g_allocs == h )
		g_allocs = h->next;
	if( h->next )
		h->next->prev = h->prev;
	g_allocBytes -= h->size;
	rd_mutex_unlock( &g_allocLock );
	h->magic = 0;
}

void* resodoom_hook_malloc( size_t size )
{
	AllocHeader* h = (AllocHeader*)malloc( sizeof( AllocHeader ) + size );
	if( !h )
		return NULL;
	alloc_track( h, size );
	return header_to_payload( h );
}

void* resodoom_hook_calloc( size_t count, size_t size )
{
	size_t total = count * size;
	void*  p     = resodoom_hook_malloc( total );
	if( p )
		memset( p, 0, total );
	return p;
}

void* resodoom_hook_realloc( void* ptr, size_t size )
{
	if( !ptr )
		return resodoom_hook_malloc( size );

	AllocHeader* h = payload_to_header( ptr );
	if( !h )
	{
		/* Not ours. Cannot grow it safely into our list, so copy across --
		   correct in every case, and this path is effectively never taken. */
		void* fresh = resodoom_hook_malloc( size );
		if( fresh )
			memcpy( fresh, ptr, size );
		free( ptr );
		return fresh;
	}

	size_t old = h->size;
	alloc_untrack( h );
	AllocHeader* grown = (AllocHeader*)realloc( h, sizeof( AllocHeader ) + size );
	if( !grown )
	{
		/* realloc failed: the original is still live, so put it back. */
		alloc_track( h, old );
		return NULL;
	}
	alloc_track( grown, size );
	return header_to_payload( grown );
}

void resodoom_hook_free( void* ptr )
{
	if( !ptr )
		return;
	AllocHeader* h = payload_to_header( ptr );
	if( !h )
	{
		/* A pointer libc handed the engine directly. Free it the plain way
		   rather than corrupting the heap trying to be clever. */
		free( ptr );
		return;
	}
	alloc_untrack( h );
	free( h );
}

char* resodoom_hook_strdup( const char* s )
{
	if( !s )
		return NULL;
	size_t n   = strlen( s ) + 1;
	char*  out = (char*)resodoom_hook_malloc( n );
	if( out )
		memcpy( out, s, n );
	return out;
}

static void alloc_free_all( void )
{
	rd_mutex_lock( &g_allocLock );
	AllocHeader* h = g_allocs;
	g_allocs       = NULL;
	g_allocBytes   = 0;
	rd_mutex_unlock( &g_allocLock );

	while( h )
	{
		AllocHeader* next = h->next;
		h->magic          = 0;
		free( h );
		h = next;
	}
}

/* ------------------------------------------------------------------ */
/* Engine state                                                        */
/* ------------------------------------------------------------------ */

#define RESODOOM_INPUT_CAPACITY 256
#define RESODOOM_ARGV_MAX       32
#define RESODOOM_STATUS_MAX     512

typedef struct InputEvent
{
	uint8_t key;
	uint8_t down;
} InputEvent;

typedef struct Engine
{
	rd_thread       thread;
	bool            threadLive;

	atomic_int      state;       /* StagehandState                           */
	atomic_bool     quit;

	/* The virtual clock, in two halves. `budgetTics` is how much time the
	   consumer has released; `clockMs` is how much of it the engine has spent.
	   The engine may never spend past the budget, and that single rule is the
	   whole throttle. */
	atomic_uint     budgetTics;
	atomic_uint     clockMs;     /* written by the engine thread only        */

	atomic_uint     seq;
	atomic_uint     tic;
	atomic_uint     parks;       /* budget-exhaustion episodes, not wakeups  */

	/* Triple buffer. The engine writes `write`, publishes it into `ready`,
	   and the consumer swaps `ready` out for its own spare. Lock-free by
	   construction: the consumer must never block, because it is Resolume's
	   render thread and a stall there is a dropped show. */
	uint8_t         slots[ 3 ][ RESODOOM_BYTES ];
	uint32_t        slotSeq[ 3 ];
	uint32_t        slotTic[ 3 ];
	atomic_int      readySlot;   /* -1 when nothing new                      */
	int             writeSlot;
	int             spareSlot;

	rd_mutex        gate;
	rd_cond         gateCv;

	InputEvent      input[ RESODOOM_INPUT_CAPACITY ];
	atomic_uint     inHead;
	atomic_uint     inTail;
	uint8_t         held[ 256 ];

	jmp_buf         escape;
	char            status[ RESODOOM_STATUS_MAX ];

	int             deterministic;
	char            argvStore[ RESODOOM_ARGV_MAX ][ 1024 ];
	char*           argv[ RESODOOM_ARGV_MAX ];
	int             argc;
} Engine;

static Engine g;

static void set_status( const char* fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( g.status, sizeof( g.status ), fmt, ap );
	va_end( ap );
}

/* ------------------------------------------------------------------ */
/* Hooked exit                                                         */
/* ------------------------------------------------------------------ */

void resodoom_hook_exit( int code )
{
	/*
		I_Error has already written its message to stderr by the time it gets
		here, and there is no way to reach into its stack for the text. What we
		can say for certain is the exit code and that the engine stopped on its
		own -- the plugin's diagnostics log carries the engine's stderr, which
		is where the actual "W_GetNumForName: ... not found" line appears.
	*/
	/*
		Overwrite whatever was there. The previous status is "starting", which
		is a placeholder, and there is no way to reach into I_Error's stack for
		the text it just printed -- so the honest thing is to say where the
		real message went rather than to report a stale one.
	*/
	set_status( "Doom stopped itself (exit code %d). Its own message is in the log, "
				"just above this line -- it usually names the WAD",
				code );

	atomic_store( &g.state, STAGEHAND_FAILED );
	longjmp( g.escape, 1 );
}

/* ------------------------------------------------------------------ */
/* doomgeneric platform layer                                          */
/* ------------------------------------------------------------------ */

void DG_Init( void )
{
	/* Nothing to open: there is no window, no device and no input system. */
}

void DG_SetWindowTitle( const char* title )
{
	(void)title; /* there is no window to title */
}

static uint32_t budget_ms( void )
{
	return (uint32_t)( ( (uint64_t)atomic_load( &g.budgetTics ) * 1000u ) / RESODOOM_TICRATE );
}

uint32_t DG_GetTicksMs( void )
{
	/* Doom's only source of time. Never the wall clock -- see DG_SleepMs. */
	return atomic_load( &g.clockMs );
}

void DG_SleepMs( uint32_t ms )
{
	/*
		**This is the throttle, and I_Sleep is the right place for it.**

		The tempting design -- gate in DG_DrawFrame, one frame per granted tic
		-- deadlocks, twice, and both times it presents as a hang with a
		complete startup banner and no picture:

		  - `TryRunTics` will not return a tic until `I_GetTime` moves, so a
		    clock that only moves after a draw waits for a draw that waits for
		    the clock.
		  - Moving the clock on Grant instead fixes the first Tick and then
		    hangs on the second, because `TryRunTics` reads `entertic` on entry
		    and its wait loop needs the clock to advance *during* the call.

		Doom already has a place where it says "I have nothing to do, take the
		CPU": `I_Sleep`. Gating there means the engine blocks exactly where it
		would have idled anyway, and the clock advances in the middle of
		`TryRunTics` where that loop expects it to.

		The rule is just: the engine may spend virtual milliseconds up to the
		budget the consumer has released, and not one past it.
	*/
	if( ms == 0 )
		ms = 1; /* a zero-length sleep must still advance, or this livelocks */

	uint32_t now     = atomic_load( &g.clockMs );
	uint32_t target  = now + ms;
	bool     counted = false; /* one park per episode, not one per wakeup */

	for( ;; )
	{
		if( atomic_load( &g.quit ) )
			longjmp( g.escape, 2 );

		uint32_t ceiling = budget_ms();
		now              = atomic_load( &g.clockMs );
		if( ceiling > now )
		{
			/* Spend what is available, up to what was asked for. */
			atomic_store( &g.clockMs, ( target < ceiling ) ? target : ceiling );
			return;
		}

		if( !counted )
		{
			atomic_fetch_add( &g.parks, 1 );
			counted = true;
		}

		/*
			A short timed wait rather than an indefinite one. Grant signals
			this condvar WITHOUT taking the mutex -- holding a lock that
			Resolume's render thread also wants is how a plugin hangs a host --
			so a missed wakeup has to be survivable. It costs 2 ms, and cannot
			cost more.
		*/
		rd_mutex_lock( &g.gate );
		rd_cond_wait_ms( &g.gateCv, &g.gate, 2 );
		rd_mutex_unlock( &g.gate );
	}
}

int DG_GetKey( int* pressed, unsigned char* key )
{
	unsigned head = atomic_load( &g.inHead );
	unsigned tail = atomic_load( &g.inTail );
	if( head == tail )
		return 0;

	InputEvent ev = g.input[ tail % RESODOOM_INPUT_CAPACITY ];
	atomic_store( &g.inTail, tail + 1 );

	*pressed = ev.down;
	*key     = ev.key;
	return 1;
}

void DG_DrawFrame( void )
{
	if( atomic_load( &g.quit ) )
		longjmp( g.escape, 2 );

	/* --- convert and publish ------------------------------------- */
	uint8_t* dst = g.slots[ g.writeSlot ];

	/*
		Three things happen in this copy, and all three are the kind of mistake
		that renders as "the plugin does nothing":

		- The flip. Doom's framebuffer is top-left origin; a GL texture is
		  bottom-left. Flipping here means the published buffer goes straight
		  into a texture and nothing downstream has to think about it.
		- Alpha. The X in XRGB8888 is undefined, not zero, and Doom leaves
		  stale bits in it. Passed through, Resolume gets a mostly-transparent
		  layer that reads as a black screen.
		- Nothing else. Channel order is already BGRA in memory on a
		  little-endian machine, which is what the texture upload asks for.
	*/
	const uint32_t* src = DG_ScreenBuffer;
	if( src )
	{
		for( int y = 0; y < RESODOOM_HEIGHT; ++y )
		{
			const uint32_t* in  = src + (size_t)y * RESODOOM_WIDTH;
			uint32_t*       out = (uint32_t*)dst
								+ (size_t)( RESODOOM_HEIGHT - 1 - y ) * RESODOOM_WIDTH;
			for( int x = 0; x < RESODOOM_WIDTH; ++x )
				out[ x ] = in[ x ] | 0xFF000000u;
		}
	}

	uint32_t drawnAt =
		(uint32_t)( ( (uint64_t)atomic_load( &g.clockMs ) * RESODOOM_TICRATE ) / 1000u );
	uint32_t seq     = atomic_fetch_add( &g.seq, 1 ) + 1;
	g.slotSeq[ g.writeSlot ] = seq;
	g.slotTic[ g.writeSlot ] = drawnAt;
	atomic_store( &g.tic, drawnAt );
	atomic_store( &g.state, STAGEHAND_RUNNING );

	/* Hand this slot to the consumer and take whichever one it left behind. */
	int previous = atomic_exchange( &g.readySlot, g.writeSlot );
	g.writeSlot  = ( previous >= 0 ) ? previous : g.spareSlot;

	/* No gate here. Publishing is not where the engine should wait -- see
	   DG_SleepMs, which is where Doom itself chooses to idle. */
}

/* ------------------------------------------------------------------ */
/* Engine thread                                                       */
/* ------------------------------------------------------------------ */

static void argv_push( const char* value )
{
	if( g.argc >= RESODOOM_ARGV_MAX )
		return;
	snprintf( g.argvStore[ g.argc ], sizeof( g.argvStore[ 0 ] ), "%s", value );
	g.argv[ g.argc ] = g.argvStore[ g.argc ];
	g.argc += 1;
}

static void argv_push_int( const char* value, int n )
{
	char buf[ 32 ];
	snprintf( buf, sizeof( buf ), "%d", n );
	argv_push( value );
	argv_push( buf );
}

/*
	Options arrive as a key/value list rather than a struct, so this engine can
	grow a setting without the plugin, the harnesses and stagehand's ABI all
	having to agree on a new struct layout first.
*/
static const char* opt( const StagehandOption* options, int count, const char* key )
{
	for( int i = 0; i < count; ++i )
		if( options[ i ].key && strcmp( options[ i ].key, key ) == 0 )
			return options[ i ].value;
	return NULL;
}

static int opt_int( const StagehandOption* options, int count, const char* key, int fallback )
{
	const char* v = opt( options, count, key );
	return ( v && v[ 0 ] ) ? atoi( v ) : fallback;
}

static void build_argv( const StagehandOption* options, int count )
{
	const char* iwad    = opt( options, count, "iwad" );
	const char* pwad    = opt( options, count, "pwad" );
	const int   heapMiB = opt_int( options, count, "heap", 0 );
	const int   skill   = opt_int( options, count, "skill", 0 );
	const int   episode = opt_int( options, count, "episode", 0 );
	const int   map     = opt_int( options, count, "map", 0 );

	g.argc = 0;
	argv_push( "resodoom" );

	/*
		-nogui is not optional. Without it I_Error pops a CoreFoundation alert
		on macOS -- a modal dialog, owned by Resolume, over a live output.
	*/
	argv_push( "-nogui" );
	argv_push( "-nosound" );
	argv_push( "-nomusic" );

	if( iwad && iwad[ 0 ] )
	{
		argv_push( "-iwad" );
		argv_push( iwad );
	}
	if( pwad && pwad[ 0 ] )
	{
		argv_push( "-file" );
		argv_push( pwad );
	}
	if( heapMiB > 0 )
		argv_push_int( "-mb", heapMiB );
	if( skill >= 1 && skill <= 5 )
		argv_push_int( "-skill", skill );
	if( episode > 0 && map > 0 )
	{
		char buf[ 32 ];
		snprintf( buf, sizeof( buf ), "%d", episode );
		argv_push( "-warp" );
		argv_push( buf );
		snprintf( buf, sizeof( buf ), "%d", map );
		argv_push( buf );
	}
	else if( map > 0 )
	{
		argv_push_int( "-warp", map );
	}

	g.argv[ g.argc ] = NULL;
}

static void engine_thread( void )
{
	rd_thread_stay_awake();

	int jumped = setjmp( g.escape );
	if( jumped == 0 )
	{
		/*
			doomgeneric's shape, which is not the one the name suggests:
			`doomgeneric_Create` does the whole of D_DoomMain AND one tick,
			then RETURNS. The host owns the loop from there. Upstream's
			reference ports are all `Create(); while(1) Tick();`.
		*/
		doomgeneric_Create( g.argc, g.argv );

		while( !atomic_load( &g.quit ) )
			doomgeneric_Tick();

		atomic_store( &g.state, STAGEHAND_CLOSED );
	}
	else if( jumped == 2 )
	{
		atomic_store( &g.state, STAGEHAND_CLOSED );
	}
	/* jumped == 1: resodoom_hook_exit already set STAGEHAND_FAILED and the text. */
}

/* ------------------------------------------------------------------ */
/* ABI                                                                 */
/* ------------------------------------------------------------------ */

/*
	Set once the first Start has happened, and never cleared.

	**A loaded copy of this library can run Doom exactly once.** Stop() joins
	the thread and frees every allocation, but it cannot put doomgeneric's
	several hundred file-scope globals back the way it found them -- the wad
	list, the zone pointers, the game state machine and a long tail of
	"already initialised" flags all still refer to the run that just ended, and
	most of them now point at freed memory. A second Start in the same copy
	gets partway through D_DoomMain and then produces no frame at all, which
	from the outside looks exactly like a WAD that failed to load.

	The fix belongs to the caller, not here: load a fresh uniquely-named copy
	of the library. The plugin does that per instance anyway, for the same
	reason two layers need two sets of globals. Refusing loudly is what stops
	that becoming an afternoon.
*/
static bool g_everStarted = false;

static int api_open( const StagehandOption* options, int count )
{
	if( g.threadLive )
		return -1;

	if( g_everStarted )
	{
		set_status( "this engine copy has already run -- load a fresh copy of "
					"the library instead of restarting this one" );
		atomic_store( &g.state, STAGEHAND_FAILED );
		return -1;
	}
	const char* iwad = opt( options, count, "iwad" );
	if( !iwad || !iwad[ 0 ] )
	{
		set_status( "no WAD selected" );
		atomic_store( &g.state, STAGEHAND_FAILED );
		return -1;
	}

	g.deterministic = opt_int( options, count, "deterministic", 0 );
	rd_mutex_init( &g.gate );
	rd_cond_init( &g.gateCv );

	atomic_store( &g.state, STAGEHAND_STARTING );
	atomic_store( &g.quit, false );
	atomic_store( &g.seq, 0 );
	atomic_store( &g.tic, 0 );
	atomic_store( &g.parks, 0 );
	atomic_store( &g.readySlot, -1 );
	atomic_store( &g.inHead, 0 );
	atomic_store( &g.inTail, 0 );
	memset( g.held, 0, sizeof( g.held ) );
	g.writeSlot = 0;
	g.spareSlot = 1;
	atomic_store( &g.clockMs, 0 );
	set_status( "starting" );

	build_argv( options, count );

	/*
		One tic of budget up front. `doomgeneric_Create` runs a tick of its own
		before it returns, and with a budget of zero it would block inside
		startup instead of producing the first frame.
	*/
	atomic_store( &g.budgetTics, 1 );

	/*
		**The stack size is not a tuning knob, it is a correctness fix.** A
		pthread gets 512 KiB by default on macOS where the main thread gets 8
		MiB, and Doom was written for the latter: R_RenderBSPNode recurses
		through the map's BSP tree and several routines hold big locals. The
		failure is a SIGBUS on the first instruction of whatever function
		overran -- a backtrace that points at an innocent leaf and says
		nothing about stacks.
	*/
	char why[ 160 ];
	if( !rd_thread_start( &g.thread, engine_thread, 8u * 1024u * 1024u,
						  why, sizeof( why ) ) )
	{
		set_status( "could not start the engine thread: %s", why );
		atomic_store( &g.state, STAGEHAND_FAILED );
		return -1;
	}
	g.threadLive  = true;
	g_everStarted = true;
	return 0;
}

static void api_close( void )
{
	if( g.threadLive )
	{
		atomic_store( &g.quit, true );
		atomic_fetch_add( &g.budgetTics, 1 ); /* unpark a thread in the gate */
		rd_cond_broadcast( &g.gateCv );
		rd_thread_join( g.thread );
		g.threadLive = false;
		rd_cond_destroy( &g.gateCv );
		rd_mutex_destroy( &g.gate );
	}

	alloc_free_all();
	atomic_store( &g.state, STAGEHAND_CLOSED );
}

static int api_state( void )
{
	return atomic_load( &g.state );
}

static void api_describe( StagehandInfo* out )
{
	if( !out )
		return;
	out->width           = RESODOOM_WIDTH;
	out->height          = RESODOOM_HEIGHT;
	out->rateNumerator   = RESODOOM_TICRATE;
	out->rateDenominator = 1;
	out->pixelAspect     = RESODOOM_PIXEL_ASPECT;
	out->frameBytes      = RESODOOM_BYTES;
}

static const char* api_status( void )
{
	return g.status;
}

/*
	How far the engine is allowed to fall behind its budget before further
	grants are dropped on the floor. Four tics is about a tenth of a second.

	Without a cap, unspent budget accumulates: a clip that sat paused, or a
	composition that stalled, banks thousands of tics and then burns them at
	whatever speed the CPU allows. The operator sees the game fast-forward
	through the part they were waiting for. Dropping the excess costs nothing
	real -- the time has already passed -- and it is what keeps the game on the
	composition's clock rather than on a debt schedule.

	It is OFF under the `deterministic` option, and has to be. The cap compares the
	budget against the wall-clock-free notion of what has been spent, which is
	only meaningful when grants arrive at roughly real time. A harness grants
	as fast as it can step, immediately looks over-granted, and then has every
	further grant dropped -- which presents as the engine mysteriously
	stalling partway through a test rather than as a policy doing its job.
*/
#define RESODOOM_MAX_LAG_TICS 4u

static void api_grant( int tics )
{
	if( tics <= 0 )
		return;

	if( !g.deterministic )
	{
		uint32_t spentTics =
			(uint32_t)( ( (uint64_t)atomic_load( &g.clockMs ) * RESODOOM_TICRATE ) / 1000u );
		uint32_t budget = atomic_load( &g.budgetTics );

		if( budget > spentTics + RESODOOM_MAX_LAG_TICS )
			return;
	}

	atomic_fetch_add( &g.budgetTics, (unsigned)tics );
	/* Signalled without the mutex on purpose -- see the wait in DG_SleepMs. */
	rd_cond_broadcast( &g.gateCv );
}

static int api_frame( void* dst, size_t dstBytes, uint32_t lastSeq, uint32_t* outSeq,
					  uint32_t* outTick )
{
	if( !dst || dstBytes < RESODOOM_BYTES )
		return 0;

	int ready = atomic_exchange( &g.readySlot, -1 );
	if( ready < 0 )
		return 0;

	/* Take the slot; the engine gets our spare in exchange next time it
	   publishes, which is what keeps this copy free of any lock. */
	memcpy( dst, g.slots[ ready ], RESODOOM_BYTES );
	const uint32_t seq = g.slotSeq[ ready ];
	const uint32_t tic = g.slotTic[ ready ];
	g.spareSlot        = ready;

	if( outSeq )
		*outSeq = seq;
	if( outTick )
		*outTick = tic;

	return seq != lastSeq;
}

static void api_event( int doomKey, int down )
{
	/*
		stagehand's event vocabulary is whatever the source documents, and this
		one's is doomkeys.h: the plugin sends KEY_FIRE, KEY_USE and the rest
		straight through. Anything outside a byte is not a Doom key.
	*/
	if( doomKey < 0 || doomKey > 255 )
		return;

	uint8_t k = (uint8_t)doomKey;
	uint8_t d = down ? 1 : 0;

	/* Swallow repeats. Doom treats a second keydown as a fresh press, which
	   in menus means every held key scrolls at the composition's frame rate. */
	if( g.held[ k ] == d )
		return;
	g.held[ k ] = d;

	unsigned head = atomic_load( &g.inHead );
	unsigned tail = atomic_load( &g.inTail );
	if( head - tail >= RESODOOM_INPUT_CAPACITY )
		return; /* full: drop rather than overwrite an unread edge */

	g.input[ head % RESODOOM_INPUT_CAPACITY ].key  = k;
	g.input[ head % RESODOOM_INPUT_CAPACITY ].down = d;
	atomic_store( &g.inHead, head + 1 );
}

static uint32_t api_parks( void )
{
	return atomic_load( &g.parks );
}

static void api_release_inputs( void )
{
	for( int k = 0; k < 256; ++k )
		if( g.held[ k ] )
			api_event( k, 0 );
}

/* Order must match StagehandSourceApi exactly. The struct is positional, so a
   field in the wrong place is a call through the wrong function pointer. */
static const StagehandSourceApi kApi = {
	STAGEHAND_ABI_VERSION,
	api_open,
	api_close,
	api_state,
	api_status,
	api_describe,
	api_grant,
	api_frame,
	api_event,
	api_release_inputs,
	api_parks,
};

const StagehandSourceApi* stagehand_source_api( void )
{
	return &kApi;
}

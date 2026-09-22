/*
	ResodoomHooks.h -- force-included (-include) into every doomgeneric .c file.

	**Upstream is not edited.** doomgeneric is a pristine submodule; everything
	this plugin needs to change about it is changed here, by redefining two
	things the engine reaches for. That keeps `git diff` in the submodule empty
	and makes updating upstream a version bump instead of a merge.

	Three interceptions, all load-bearing:

	1. `exit`. `I_Error` ends in an unconditional `exit(-1)` -- a malformed WAD
	   would close Resolume mid-show. Routed to a longjmp back to the engine
	   thread's entry point, which records the message and parks the instance in
	   RESODOOM_FAILED.

	2. The allocator. A longjmp out of `D_DoomMain` abandons every allocation
	   the engine made, and a VJ who swaps WADs a dozen times in a set would
	   leak a dozen zone heaps (16 MiB each by default). Every block gets a
	   small header and joins an intrusive list, so teardown can free the lot.
	   `free` still tolerates a foreign pointer, because a few reach the engine
	   from libc.

	3. `SCREENWIDTH` and `SCREENHEIGHT`, so the buffer's geometry is a build
	   setting rather than a submodule edit. See the block where it happens.

	Order matters: the system headers that declare these must be included
	BEFORE the macros exist, or their own declarations get macro-expanded.
*/
#ifndef RESODOOM_HOOKS_H
#define RESODOOM_HOOKS_H

/* Pull in every declaration of the functions below while their names still
   mean what libc thinks they mean. Include guards make the engine's own later
   #includes no-ops. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
	The screen geometry, by the same trick and for the same reason.

	`i_video.h` writes `#define SCREENWIDTH 320` outright, so it cannot simply
	be overridden from the compile line -- whoever defines it LAST wins, and
	that is always upstream. Including the header here trips its own
	`__I_VIDEO__` guard, which turns every later `#include "i_video.h"` in the
	engine into a no-op and leaves the definition below standing.

	This works only because this header arrives before the first line of every
	translation unit. A normal #include would be too late for any file that
	had already pulled in i_video.h.

	Both are used to size static arrays -- `floorclip[SCREENWIDTH]`,
	`yslope[SCREENHEIGHT]`, `visplane_t::top[]` -- which is exactly why the
	geometry is fixed at build time and cannot follow a canvas at runtime.
*/
#include "i_video.h"

#ifdef RESODOOM_SCREEN_WIDTH
	#undef SCREENWIDTH
	#define SCREENWIDTH RESODOOM_SCREEN_WIDTH
#endif

#ifdef RESODOOM_SCREEN_HEIGHT
	#undef SCREENHEIGHT
	#define SCREENHEIGHT RESODOOM_SCREEN_HEIGHT
#endif

#ifdef __cplusplus
extern "C" {
#endif

void  resodoom_hook_exit( int code );
void* resodoom_hook_malloc( size_t size );
void* resodoom_hook_calloc( size_t count, size_t size );
void* resodoom_hook_realloc( void* ptr, size_t size );
void  resodoom_hook_free( void* ptr );
char* resodoom_hook_strdup( const char* s );

#ifdef __cplusplus
}
#endif

#define exit( code )        resodoom_hook_exit( code )
#define malloc( n )         resodoom_hook_malloc( n )
#define calloc( n, s )      resodoom_hook_calloc( n, s )
#define realloc( p, n )     resodoom_hook_realloc( p, n )
#define free( p )           resodoom_hook_free( p )
#define strdup( s )         resodoom_hook_strdup( s )

/*
	The file that IMPLEMENTS these hooks must undo them -- see the block of
	`#undef`s at the top of EngineImpl.c.

	It cannot be done with an `#ifndef RESODOOM_HOOKS_IMPL` guard around the
	block above, and that is worth stating because it is the obvious design and
	it silently does not work: `-include` is processed BEFORE the first line of
	the file that receives it, so a `#define RESODOOM_HOOKS_IMPL` in EngineImpl.c
	arrives after the macros already exist, and the include guard then turns
	that file's own `#include` of this header into a no-op. The result is
	`resodoom_hook_malloc` calling itself until the thread's stack runs out: a
	SIGBUS on the function's first instruction, with a stack too far gone for
	the debugger to unwind past frame 0.
*/

#endif /* RESODOOM_HOOKS_H */

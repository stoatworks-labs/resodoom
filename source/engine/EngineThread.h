/*
	EngineThread.h -- the five threading primitives the engine actually uses,
	over pthreads or Win32.

	Small on purpose. EngineImpl.c needs one worker thread, one lock around the
	allocation registry, and one condition variable it waits on for at most a
	couple of milliseconds; everything else it does is `<stdatomic.h>`. A
	general threading layer would be a bigger thing to get right than the
	engine's use of it.

	## What it is shaped by

	**The timed wait is RELATIVE, not absolute.** pthreads takes a deadline and
	Win32 takes a duration, and the engine's only caller wants "wait up to two
	milliseconds" -- so the shim takes the duration and the pthread side does
	the `gettimeofday` arithmetic out of sight. Expressing it the other way
	round would put that arithmetic back in EngineImpl.c and hand the Windows
	side the job of subtracting the current time from it again.

	**A spurious wakeup is not an error here and the return value says so.**
	Both platforms can wake early, and the engine's wait loop re-checks its
	budget on every pass anyway, so there is nothing for a caller to do with
	the distinction between "signalled" and "timed out".

	**Mutex destroy is a no-op on Windows and that is correct.** An SRWLOCK has
	no destructor; the pairs are kept symmetrical so the engine's teardown path
	reads the same on both platforms.
*/
#ifndef RESODOOM_ENGINE_THREAD_H
#define RESODOOM_ENGINE_THREAD_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined( _WIN32 )

	/*
		Include this AFTER EngineImpl.c has undone the allocator hooks. The
		force-included header turns `malloc` and friends into macros, and
		windows.h is large enough to trip over one.

		**WIN32_LEAN_AND_MEAN is not a compile-time saving here, it is what
		makes the file compile at all.** It drops the RPC and OLE headers, and
		those are where the SDK declares `boolean` and `BOOLEAN` -- as
		`unsigned char`, against Doom's own enum of the same name in
		doomtype.h. Without it every translation unit that sees both gets
		"redefinition; different basic types" from a Windows header, naming
		nothing that belongs to this project.
	*/
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	#include <process.h>

	typedef SRWLOCK            rd_mutex;
	typedef CONDITION_VARIABLE rd_cond;
	typedef HANDLE             rd_thread;

	#define RD_MUTEX_STATIC_INIT SRWLOCK_INIT

#else

	#include <pthread.h>
	#include <sys/time.h>

	#if defined( __APPLE__ )
		#include <pthread/qos.h>
	#endif

	typedef pthread_mutex_t rd_mutex;
	typedef pthread_cond_t  rd_cond;
	typedef pthread_t       rd_thread;

	#define RD_MUTEX_STATIC_INIT PTHREAD_MUTEX_INITIALIZER

#endif

/* What a started thread runs. Nothing joins for a value, so there is none. */
typedef void ( *rd_thread_body )( void );

/* ------------------------------------------------------------------ */
/* Mutex                                                               */
/* ------------------------------------------------------------------ */

static inline void rd_mutex_init( rd_mutex* m )
{
#if defined( _WIN32 )
	InitializeSRWLock( m );
#else
	pthread_mutex_init( m, NULL );
#endif
}

static inline void rd_mutex_destroy( rd_mutex* m )
{
#if defined( _WIN32 )
	(void)m; /* an SRWLOCK has no destructor */
#else
	pthread_mutex_destroy( m );
#endif
}

static inline void rd_mutex_lock( rd_mutex* m )
{
#if defined( _WIN32 )
	AcquireSRWLockExclusive( m );
#else
	pthread_mutex_lock( m );
#endif
}

static inline void rd_mutex_unlock( rd_mutex* m )
{
#if defined( _WIN32 )
	ReleaseSRWLockExclusive( m );
#else
	pthread_mutex_unlock( m );
#endif
}

/* ------------------------------------------------------------------ */
/* Condition variable                                                  */
/* ------------------------------------------------------------------ */

static inline void rd_cond_init( rd_cond* c )
{
#if defined( _WIN32 )
	InitializeConditionVariable( c );
#else
	pthread_cond_init( c, NULL );
#endif
}

static inline void rd_cond_destroy( rd_cond* c )
{
#if defined( _WIN32 )
	(void)c; /* likewise */
#else
	pthread_cond_destroy( c );
#endif
}

static inline void rd_cond_broadcast( rd_cond* c )
{
#if defined( _WIN32 )
	WakeAllConditionVariable( c );
#else
	pthread_cond_broadcast( c );
#endif
}

/*
	Wait on `c` with `m` held, for at most `ms`, and return with `m` held.

	The caller must hold `m`. Waking early -- signalled, or spuriously -- is
	ordinary and indistinguishable, so nothing is reported.
*/
static inline void rd_cond_wait_ms( rd_cond* c, rd_mutex* m, unsigned ms )
{
#if defined( _WIN32 )
	SleepConditionVariableSRW( c, m, (DWORD)ms, 0 );
#else
	struct timespec deadline;
	struct timeval  now;
	gettimeofday( &now, NULL );

	/* Microseconds first, so a millisecond figure large enough to carry does
	   so exactly once rather than leaving tv_nsec out of range. */
	long usec       = now.tv_usec + (long)ms * 1000L;
	deadline.tv_sec = now.tv_sec + usec / 1000000L;
	deadline.tv_nsec = ( usec % 1000000L ) * 1000L;

	pthread_cond_timedwait( c, m, &deadline );
#endif
}

/* ------------------------------------------------------------------ */
/* Thread                                                              */
/* ------------------------------------------------------------------ */

/*
	The body is parked in a file static rather than threaded through the
	platform's `void*` argument, because a function pointer does not portably
	survive a round trip through one.

	That is sound here for the reason the whole engine is a separate shared
	library: one loaded copy is one instance, and one instance starts exactly
	one engine thread. See SourceAbi.h.
*/
static rd_thread_body rd_body = NULL;

#if defined( _WIN32 )
static unsigned __stdcall rd_thread_trampoline( void* unused )
{
	(void)unused;
	if( rd_body )
		rd_body();
	return 0;
}
#else
static void* rd_thread_trampoline( void* unused )
{
	(void)unused;
	if( rd_body )
		rd_body();
	return NULL;
}
#endif

/*
	Start `body` on a thread with `stackBytes` of stack, and say why on false.

	**The stack size is a correctness requirement, not a tuning knob**, which
	is why it has no default: a pthread gets 512 KiB on macOS where the main
	thread gets 8 MiB, and Doom was written for the latter. Windows takes the
	main thread's reserve from the PE header instead, so a plugin inherits
	whatever the HOST was linked with -- which is Resolume's business, not this
	engine's. Asking for the size on both is what makes that not matter.
*/
static inline bool rd_thread_start( rd_thread* t, rd_thread_body body,
									size_t stackBytes, char* error, size_t errorSize )
{
	rd_body = body;

#if defined( _WIN32 )
	/*
		_beginthreadex, not CreateThread. Doom reaches for the CRT constantly
		-- printf, the allocator behind the hooks, strtol -- and a thread
		started behind the CRT's back leaks its per-thread state on exit.
	*/
	uintptr_t h = _beginthreadex( NULL, (unsigned)stackBytes, rd_thread_trampoline,
								  NULL, 0, NULL );
	if( h == 0 )
	{
		snprintf( error, errorSize, "_beginthreadex failed (errno %d)", errno );
		return false;
	}
	*t = (HANDLE)h;
	return true;
#else
	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, stackBytes );

	int rc = pthread_create( t, &attr, rd_thread_trampoline, NULL );
	pthread_attr_destroy( &attr );

	if( rc != 0 )
	{
		snprintf( error, errorSize, "%s", strerror( rc ) );
		return false;
	}
	return true;
#endif
}

static inline void rd_thread_join( rd_thread t )
{
#if defined( _WIN32 )
	WaitForSingleObject( t, INFINITE );
	CloseHandle( t );
#else
	pthread_join( t, NULL );
#endif
}

/*
	Ask the scheduler not to demote this thread.

	macOS only, and load-bearing there: App Nap demotes a worker thread in a
	covered window, and a dedicated thread is not on its own enough --
	elsewhere in the fleet that took a 50 Hz loop down to 7. Windows has no
	equivalent that applies to a thread inside somebody else's process.
*/
static inline void rd_thread_stay_awake( void )
{
#if defined( __APPLE__ )
	pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 );
#endif
}

#endif /* RESODOOM_ENGINE_THREAD_H */

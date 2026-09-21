#include "Engine.h"
#include "Diag.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>

#if defined( _WIN32 )
	#include <windows.h>
#else
	#include <dlfcn.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

namespace resodoom
{
namespace
{

std::atomic< unsigned > gCopyCounter { 0 };

uint64_t nowNanoseconds()
{
	return uint64_t( std::chrono::duration_cast< std::chrono::nanoseconds >(
						 std::chrono::steady_clock::now().time_since_epoch() )
						 .count() );
}

/*
	The directory this plugin's own binary lives in.

	`dladdr` on a symbol we own, rather than anything involving the bundle's
	name or a search of Resolume's plugin folders. The operator may have
	renamed the bundle, may have several versions installed, and on Windows the
	whole concept of a bundle is different -- but the loader always knows where
	it got this code from.
*/
std::string binaryDirectory()
{
#if defined( _WIN32 )
	HMODULE module = nullptr;
	if( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
								 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							 reinterpret_cast< LPCSTR >( &nowNanoseconds ), &module ) )
		return {};

	char path[ MAX_PATH ] = { 0 };
	if( GetModuleFileNameA( module, path, MAX_PATH ) == 0 )
		return {};
	return std::filesystem::path( path ).parent_path().string();
#else
	Dl_info info {};
	if( dladdr( reinterpret_cast< const void* >( &nowNanoseconds ), &info ) == 0
		|| info.dli_fname == nullptr )
		return {};
	return std::filesystem::path( info.dli_fname ).parent_path().string();
#endif
}

#if defined( _WIN32 )
constexpr const char* kEngineLeaf = "resodoom_engine.dll";
#elif defined( __APPLE__ )
constexpr const char* kEngineLeaf = "libresodoom_engine.dylib";
#else
constexpr const char* kEngineLeaf = "libresodoom_engine.so";
#endif

bool copyFile( const std::string& from, const std::string& to )
{
	std::error_code ec;
	std::filesystem::copy_file( from, to,
								std::filesystem::copy_options::overwrite_existing, ec );
	if( ec )
		return false;
#if !defined( _WIN32 )
	::chmod( to.c_str(), 0755 );
#endif
	return true;
}

std::string temporaryDirectory()
{
	std::error_code ec;
	auto            dir = std::filesystem::temp_directory_path( ec );
	return ec ? std::string( "/tmp" ) : dir.string();
}

} // namespace

std::string Engine::EngineLibraryPath()
{
	const std::string dir = binaryDirectory();
	if( dir.empty() )
		return {};

	std::error_code ec;

	// Beside the plugin binary is where the build puts it, on every platform.
	const std::string beside = dir + "/" + kEngineLeaf;
	if( std::filesystem::exists( beside, ec ) )
		return beside;

	/*
		A developer build runs the plugin straight out of the CMake build
		directory, where the engine sits one level up from the bundle's
		MacOS folder rather than beside the binary. Looking there costs one
		stat and saves installing on every rebuild.
	*/
	const std::string candidates[] = {
		dir + "/../../../" + kEngineLeaf,
		dir + "/../Resources/" + kEngineLeaf,
	};
	for( const std::string& candidate : candidates )
		if( std::filesystem::exists( candidate, ec ) )
			return std::filesystem::weakly_canonical( candidate, ec ).string();

	return {};
}

Engine::~Engine()
{
	Unload();
}

bool Engine::Load( const ResodoomConfig& cfg )
{
	Unload();

	const std::string source = EngineLibraryPath();
	if( source.empty() )
	{
		mStatus = "the engine library is missing from the plugin bundle";
		diag::error( mStatus );
		return false;
	}

	// One copy per instance, and a name no other instance can collide with:
	// dlopen would otherwise hand back an image somebody else is already
	// playing on. See the class comment.
	char leaf[ 128 ];
	std::snprintf( leaf, sizeof( leaf ), "/resodoom-%d-%u-%s", int( ::getpid() ),
				   gCopyCounter.fetch_add( 1 ), kEngineLeaf );
	mCopyPath = temporaryDirectory() + leaf;

	if( !copyFile( source, mCopyPath ) )
	{
		mStatus   = "could not stage a private copy of the engine at " + mCopyPath;
		mCopyPath.clear();
		diag::error( mStatus );
		return false;
	}

#if defined( _WIN32 )
	mHandle = (void*)LoadLibraryA( mCopyPath.c_str() );
#else
	mHandle = dlopen( mCopyPath.c_str(), RTLD_NOW | RTLD_LOCAL );
#endif
	if( !mHandle )
	{
#if defined( _WIN32 )
		mStatus = "could not load the engine library";
#else
		const char* err = dlerror();
		mStatus = std::string( "could not load the engine library: " )
				+ ( err ? err : "unknown" );
#endif
		diag::error( mStatus );
		Unload();
		return false;
	}

#if defined( _WIN32 )
	auto entry = (resodoom_engine_api_fn)GetProcAddress( (HMODULE)mHandle,
														 RESODOOM_ENGINE_ENTRY );
#else
	auto entry = (resodoom_engine_api_fn)dlsym( mHandle, RESODOOM_ENGINE_ENTRY );
#endif
	if( !entry )
	{
		mStatus = "the engine library has no entry point";
		diag::error( mStatus );
		Unload();
		return false;
	}

	mApi = entry();
	if( !mApi || mApi->abiVersion != RESODOOM_ABI_VERSION )
	{
		// A stale engine beside a new plugin reads a struct of the wrong shape
		// and calls through whatever happens to be at that offset. Refusing is
		// the only safe answer, and the version numbers name the fix.
		char msg[ 160 ];
		std::snprintf( msg, sizeof( msg ),
					   "engine ABI %u does not match the plugin's %u -- the bundle "
					   "has mismatched parts",
					   mApi ? mApi->abiVersion : 0u, RESODOOM_ABI_VERSION );
		mStatus = msg;
		mApi    = nullptr;
		diag::error( mStatus );
		Unload();
		return false;
	}

	if( mApi->Start( &cfg ) != 0 )
	{
		mStatus = mApi->Status();
		diag::error( std::string( "engine refused to start: " ) + mStatus );
		Unload();
		return false;
	}

	mLastSeq      = 0;
	mLastPumpNs   = nowNanoseconds();
	mTicRemainder = 0.0;
	mStatus       = "running";
	diag::info( std::string( "started on " ) + ( cfg.iwad ? cfg.iwad : "?" ) );
	return true;
}

void Engine::Unload()
{
	if( mApi )
	{
		mApi->Stop();
		mApi = nullptr;
	}
	if( mHandle )
	{
#if defined( _WIN32 )
		FreeLibrary( (HMODULE)mHandle );
#else
		dlclose( mHandle );
#endif
		mHandle = nullptr;
	}
	if( !mCopyPath.empty() )
	{
		std::error_code ec;
		std::filesystem::remove( mCopyPath, ec );
		mCopyPath.clear();
	}
	mLastSeq = 0;
}

bool Engine::Running() const
{
	return mApi != nullptr && mApi->State() == RESODOOM_RUNNING;
}

void Engine::Pump( float speed )
{
	if( !mApi )
		return;

	const uint64_t now     = nowNanoseconds();
	uint64_t       elapsed = now - mLastPumpNs;
	mLastPumpNs            = now;

	/*
		A long gap is not time the game owes anybody. A composition that was
		paused, a plugin that was not being rendered, or a machine that slept
		would otherwise hand the engine minutes of budget at once and the
		operator would watch Doom fast-forward. Clamping to a quarter second
		keeps a dropped frame smooth and throws an outage away.
	*/
	constexpr uint64_t kMaxGapNs = 250ull * 1000ull * 1000ull;
	if( elapsed > kMaxGapNs )
		elapsed = kMaxGapNs;

	if( speed <= 0.0f )
		return; // paused: the clock does not move, so neither does the game

	mTicRemainder += ( double( elapsed ) / 1e9 ) * double( RESODOOM_TICRATE ) * double( speed );

	const int whole = int( mTicRemainder );
	if( whole > 0 )
	{
		mTicRemainder -= double( whole );
		mApi->Grant( whole );
	}
}

bool Engine::Frame( ResodoomFrame& out )
{
	if( !mApi )
		return false;
	if( !mApi->Frame( &out, mLastSeq ) )
		return false;
	mLastSeq = out.seq;
	return true;
}

void Engine::Key( int doomKey, bool down )
{
	if( mApi )
		mApi->Key( doomKey, down ? 1 : 0 );
}

void Engine::ReleaseAllKeys()
{
	if( mApi )
		mApi->ReleaseAll();
}

} // namespace resodoom

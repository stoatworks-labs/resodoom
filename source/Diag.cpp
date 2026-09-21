#include "Diag.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#if !defined( _WIN32 )
	#include <unistd.h>
#endif

namespace resodoom
{
namespace diag
{
namespace
{

std::mutex  gLock;
std::string gPath;
bool        gResolved = false;

std::string environmentVariable( const char* name )
{
	const char* value = std::getenv( name );
	return value ? std::string( value ) : std::string();
}

std::string defaultDirectory()
{
	const std::string forced = environmentVariable( "RESODOOM_LOG_DIR" );
	if( !forced.empty() )
		return forced;

#if defined( _WIN32 )
	std::string root = environmentVariable( "LOCALAPPDATA" );
	if( root.empty() )
		root = environmentVariable( "USERPROFILE" );
	return root.empty() ? std::string() : root + "\\Resodoom\\Logs";
#elif defined( __APPLE__ )
	const std::string home = environmentVariable( "HOME" );
	return home.empty() ? std::string() : home + "/Library/Logs/Resodoom";
#else
	const std::string home = environmentVariable( "HOME" );
	return home.empty() ? std::string() : home + "/.local/state/resodoom";
#endif
}

std::string todayStamp()
{
	const std::time_t now = std::time( nullptr );
	std::tm            tm {};
#if defined( _WIN32 )
	localtime_s( &tm, &now );
#else
	localtime_r( &now, &tm );
#endif
	char buf[ 32 ];
	std::strftime( buf, sizeof( buf ), "%Y-%m-%d", &tm );
	return buf;
}

std::string timeStamp()
{
	const auto now  = std::chrono::system_clock::now();
	const auto secs = std::chrono::system_clock::to_time_t( now );
	const auto ms   = std::chrono::duration_cast< std::chrono::milliseconds >(
						  now.time_since_epoch() )
					    .count()
				  % 1000;

	std::tm tm {};
#if defined( _WIN32 )
	localtime_s( &tm, &secs );
#else
	localtime_r( &secs, &tm );
#endif
	char buf[ 32 ];
	std::strftime( buf, sizeof( buf ), "%H:%M:%S", &tm );

	char out[ 48 ];
	std::snprintf( out, sizeof( out ), "%s.%03d", buf, int( ms ) );
	return out;
}

/// Resolves the log path once. Returns empty if the directory is unusable, and
/// logging then becomes a no-op rather than an error the host has to care about.
const std::string& path()
{
	if( gResolved )
		return gPath;
	gResolved = true;

	const std::string dir = defaultDirectory();
	if( dir.empty() )
		return gPath;

	std::error_code ec;
	std::filesystem::create_directories( dir, ec );
	if( !std::filesystem::is_directory( dir, ec ) )
		return gPath;

	gPath = dir + "/resodoom." + todayStamp() + ".log";
	return gPath;
}

void write( const char* level, const std::string& message )
{
	std::lock_guard< std::mutex > guard( gLock );

	const std::string& file = path();
	if( file.empty() )
		return;

	std::ofstream out( file, std::ios::app );
	if( !out )
		return;

	out << timeStamp() << "  " << level << "  " << message << '\n';
}

} // namespace

void info( const std::string& message )  { write( "info ", message ); }
void warn( const std::string& message )  { write( "warn ", message ); }
void error( const std::string& message ) { write( "ERROR", message ); }

std::string LogPath()
{
	std::lock_guard< std::mutex > guard( gLock );
	return path();
}

void CaptureStderr()
{
#if !defined( _WIN32 )
	static bool done = false;
	std::lock_guard< std::mutex > guard( gLock );
	if( done )
		return;

	const std::string& file = path();
	if( file.empty() )
		return;
	done = true;

	/*
		freopen on stderr rather than a pipe and a reader thread.

		A pipe would let each line be timestamped and tagged, which is nicer to
		read -- and it introduces a thread whose only job is to survive Doom
		calling exit(), a buffer that can fill and block the engine mid-frame if
		nothing drains it, and a file descriptor that has to outlive an
		unloaded bundle. The engine's output is rare and arrives in bursts
		around a failure, which is exactly when a plain append is enough.

		Note that this takes the WHOLE process's stderr, Resolume's included.
		That is a real cost, and it is still the right trade: nothing else in
		the host writes to stderr in normal operation, and without it Doom's
		own error message -- the only one that names a bad WAD -- is lost.
	*/
	if( std::freopen( file.c_str(), "a", stderr ) != nullptr )
		setvbuf( stderr, nullptr, _IOLBF, 0 );
#endif
}

} // namespace diag
} // namespace resodoom

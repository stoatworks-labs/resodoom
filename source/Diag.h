#pragma once

#include <string>

/**
	A log file, and deliberately nothing else.

	No crash handler and no console: this runs inside Resolume, where installing
	a signal handler would fight the host for the same signals and where there
	is no stdout for anyone to read.

	It exists because every interesting failure here looks identical from the
	outside -- the layer is black. A WAD that could not be found, a WAD that
	Doom rejected, an engine library missing from the bundle, a shader that did
	not compile, an engine that called I_Error: one symptom, six causes. The log
	is what tells them apart, and the engine's own stderr is forwarded into it
	because Doom's own message is usually the only one that names the real
	problem.

		~/Library/Logs/Resodoom/resodoom.YYYY-MM-DD.log

	`RESODOOM_LOG_DIR` overrides the directory.
*/
namespace resodoom
{
namespace diag
{

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Where the log is being written, for the About block to show. Empty if the
/// directory could not be created.
std::string LogPath();

/*
	Redirect the C-level stderr into the log for the lifetime of the process.

	Doom writes its failures with `fprintf(stderr, ...)` and then exits, from
	deep inside C the plugin does not control. Inside Resolume that output goes
	nowhere at all. This is the only way to get "W_GetNumForName: MAP01 not
	found" in front of whoever has to fix it.
*/
void CaptureStderr();

} // namespace diag
} // namespace resodoom

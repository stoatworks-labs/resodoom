#pragma once

#include "engine/ResodoomEngine.h"

#include <cstdint>
#include <memory>
#include <string>

namespace resodoom
{

/**
	The host side of one Doom instance: finds the engine library, loads a
	private copy of it, and spends its clock against the composition's.

	**Every instance loads its own copy of the library, from its own path on
	disk.** dlopen keys on the path, so opening one that is already loaded
	returns the same image with a bumped refcount rather than a second set of
	globals -- and doomgeneric is nothing but globals. Two layers sharing an
	image would share one game, one player and one framebuffer. The copy is the
	only in-process answer that exists; cartridge reached the same conclusion
	about libretro cores for the same reason.

	A copy also happens to solve reloading. A loaded copy can run Doom exactly
	once (the engine refuses a second Start and says so), so changing WAD means
	throwing the image away and taking a fresh one, which this class does.
*/
class Engine
{
public:
	Engine() = default;
	~Engine();

	Engine( const Engine& )            = delete;
	Engine& operator=( const Engine& ) = delete;

	/// Loads a fresh copy and starts Doom on it. Any previous instance is torn
	/// down first. Returns false and sets Status() on any failure.
	bool Load( const ResodoomConfig& cfg );

	void Unload();

	bool  Running() const;
	/// Never empty once anything has been attempted.
	const std::string& Status() const { return mStatus; }

	/*
		Releases as much of Doom's clock as has passed on the wall, scaled by
		`speed`. Call once per rendered frame.

		Driving from elapsed time rather than counting frames is also what makes
		this immune to the double-render trap that bites every stateful
		generator in the fleet: Resolume renders the same composition frame to
		the output, the preview monitor and the clip thumbnail, and a generator
		that advanced once per call runs at two or three times speed *only while
		the preview is open*. Two calls a microsecond apart release a
		microsecond of Doom between them, which is the correct answer.
	*/
	void Pump( float speed );

	/// Copies the newest frame into `out`. False if there is nothing newer.
	bool Frame( ResodoomFrame& out );

	void Key( int doomKey, bool down );
	void ReleaseAllKeys();

	/// Absolute path to the engine library inside this plugin's bundle, or
	/// empty if it could not be found. Exposed for diagnostics.
	static std::string EngineLibraryPath();

private:
	void*                    mHandle = nullptr;
	const ResodoomEngineApi* mApi    = nullptr;
	std::string              mCopyPath;
	std::string              mStatus = "nothing loaded";

	uint32_t mLastSeq = 0;

	/// Wall-clock nanoseconds at the last Pump, and the leftover fraction of a
	/// tic. Keeping the remainder is what stops a 60 Hz composition (0.583
	/// tics a frame) from truncating to zero and never advancing at all.
	uint64_t mLastPumpNs   = 0;
	double   mTicRemainder = 0.0;
};

} // namespace resodoom

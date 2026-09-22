#include "Plugin.h"

#include <stagehand/Diag.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace resodoom
{
namespace
{

#if defined( _WIN32 )
constexpr const char* kEngineLeaf = "resodoom_engine.dll";
#elif defined( __APPLE__ )
constexpr const char* kEngineLeaf = "libresodoom_engine.dylib";
#else
constexpr const char* kEngineLeaf = "libresodoom_engine.so";
#endif

/*
	What the presenter stands by at before any engine exists.

	InitGL runs before a WAD has been chosen, and the shader has to be built by
	then or the first frame after choosing one is missed -- but the size is not
	known until an engine has been asked. Doom's own geometry is the honest
	guess; anything else reports itself through Describe() and the presenter is
	rebuilt to match.
*/
constexpr uint32_t kStandbyWidth  = 320;
constexpr uint32_t kStandbyHeight = 200;

} // namespace

std::string ResodoomPlugin::EngineLibraryPath()
{
	const std::string dir = stagehand::BinaryDirectory();
	if( dir.empty() )
		return {};

	std::error_code ec;

	/*
		In order:

		1. Beside the binary -- the harnesses, and the Windows and Linux layout.
		2. Contents/Frameworks -- where a shipped macOS bundle keeps it. Not
		   Contents/MacOS: macOS signs inside-out and the fleet's signing pass
		   skips that directory, so an engine there is never signed and signing
		   the bundle fails naming it.
		3. The CMake build tree, so a developer build runs without installing.
		4. Contents/Resources, for a bundle built before the move.
	*/
	const std::string candidates[] = {
		dir + "/" + kEngineLeaf,
		dir + "/../Frameworks/" + kEngineLeaf,
		dir + "/../../../" + kEngineLeaf,
		dir + "/../Resources/" + kEngineLeaf,
	};
	for( const std::string& candidate : candidates )
		if( std::filesystem::exists( candidate, ec ) )
			return std::filesystem::weakly_canonical( candidate, ec ).string();

	return {};
}

ResodoomPlugin::ResodoomPlugin()
{
	stagehand::diag::Init( "Resodoom" );

	/*
		Doom writes its failures to stderr and then exits, from deep inside C
		this plugin does not control. Inside Resolume that output goes nowhere,
		and it is usually the only thing that names a bad WAD.
	*/
	stagehand::diag::CaptureStderr();
	stagehand::diag::info( "plugin instantiated" );

	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	/*
		FF_TYPE_FILE gives the operator a real file picker, which matters more
		here than it looks: the alternative is typing an absolute path into a
		text field in a dark room.

		No WAD ships with this plugin and none ever will -- see AGENTS.md. The
		picker is the whole content story.
	*/
	SetFileParamInfo( PT_IWAD, "WAD", { "wad", "WAD" }, "" );
	SetFileParamInfo( PT_PWAD, "Mod WAD", { "wad", "WAD", "deh", "pk3" }, "" );

	SetParamInfo( PT_RUN, "Run", FF_TYPE_BOOLEAN, true );
	mParams[ PT_RUN ] = 1.0f;

	SetParamInfo( PT_RESTART, "Restart", FF_TYPE_EVENT, false );

	// 0..1 on the host side, mapped in Controls.h -- see the note there about
	// SetParamInfo clamping a ranged default.
	SetParamInfo( PT_SPEED, "Speed", FF_TYPE_STANDARD, 0.5f );
	mParams[ PT_SPEED ] = 0.5f;

	SetOptionParamInfo( PT_SKILL, "Skill", 6, 0.0f );
	SetParamElementInfo( PT_SKILL, 0, "Default", 0.0f );
	SetParamElementInfo( PT_SKILL, 1, "I'm Too Young To Die", 1.0f );
	SetParamElementInfo( PT_SKILL, 2, "Hey, Not Too Rough", 2.0f );
	SetParamElementInfo( PT_SKILL, 3, "Hurt Me Plenty", 3.0f );
	SetParamElementInfo( PT_SKILL, 4, "Ultra-Violence", 4.0f );
	SetParamElementInfo( PT_SKILL, 5, "Nightmare", 5.0f );

	/*
		Episode 0 / Map 0 means "do not warp" -- Doom boots to its title screen
		and runs its attract demos, which is the right default for a layer with
		nothing mapped to it. Anything else jumps straight into a level, which
		is what someone picking a good-looking map for a set wants.
	*/
	SetOptionParamInfo( PT_EPISODE, "Episode", 5, 0.0f );
	SetParamElementInfo( PT_EPISODE, 0, "Title / Demos", 0.0f );
	for( int e = 1; e <= 4; ++e )
	{
		char label[ 16 ];
		std::snprintf( label, sizeof( label ), "Episode %d", e );
		SetParamElementInfo( PT_EPISODE, e, label, float( e ) );
	}

	SetOptionParamInfo( PT_MAP, "Map", 33, 0.0f );
	SetParamElementInfo( PT_MAP, 0, "Title / Demos", 0.0f );
	for( int m = 1; m <= 32; ++m )
	{
		char label[ 16 ];
		std::snprintf( label, sizeof( label ), "Map %d", m );
		SetParamElementInfo( PT_MAP, m, label, float( m ) );
	}

	SetOptionParamInfo( PT_SCALING, "Scaling", 4, 0.0f );
	SetParamElementInfo( PT_SCALING, 0, "Fit", 0.0f );
	SetParamElementInfo( PT_SCALING, 1, "Fill", 1.0f );
	SetParamElementInfo( PT_SCALING, 2, "Stretch", 2.0f );
	SetParamElementInfo( PT_SCALING, 3, "Integer", 3.0f );

	/*
		On by default. Doom's 320x200 was always displayed at 4:3, so its pixels
		are 1.2 times taller than they are wide -- shown square, every face and
		every circular light in the game is visibly squashed.
	*/
	SetParamInfo( PT_PIXEL_ASPECT, "Pixel Aspect", FF_TYPE_BOOLEAN, true );
	mParams[ PT_PIXEL_ASPECT ] = 1.0f;

	SetParamInfo( PT_SMOOTH, "Smoothing", FF_TYPE_BOOLEAN, false );

	for( unsigned p = kFirstButton; p <= kLastButton; ++p )
	{
		SetParamInfo( p, ButtonName( p ), FF_TYPE_BOOLEAN, false );
		SetParamGroup( p, "Controls" );
	}

	/*
		The About block. Declared inline rather than through a helper, because
		SetParamInfo is protected on CFFGLPlugin and nothing outside the class
		can call it.

		The licence shown here is GPL-2.0, which is not what the rest of the
		fleet says -- see AGENTS.md. It comes from the website's data through
		StoatworksAbout.h, so it cannot drift from the repo's actual licence
		without somebody changing the one place it is written down.
	*/
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT,
				  stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned id = PT_ABOUT_FIRST; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );
}

ResodoomPlugin::~ResodoomPlugin()
{
	stagehand::diag::info( "plugin destroyed" );
}

// ---------------------------------------------------------------------------
// GL lifecycle
// ---------------------------------------------------------------------------

FFResult ResodoomPlugin::InitGL( const FFGLViewportStruct* vp )
{
	const GLubyte* version = glGetString( GL_VERSION );
	stagehand::diag::info( std::string( "InitGL, GL " )
						   + ( version ? reinterpret_cast< const char* >( version )
									   : "unknown" ) );

	/*
		Built at the standby size, because the engine that decides the real one
		is not loaded until a WAD is chosen and the shader has to exist before
		then. ApplyPendingLoad rebuilds it the moment an engine says otherwise.
	*/
	if( !EnsurePicture( kStandbyWidth, kStandbyHeight,
						size_t( kStandbyWidth ) * kStandbyHeight * 4 ) )
	{
		DeInitGL();
		return FF_FAIL;
	}

	mViewport = *vp;
	return FF_SUCCESS;
}

/*
	Presenter::Create destroys whatever it was holding before it builds, so
	this is a resize rather than a leak when the size really has changed -- and
	a no-op when it has not, which is every call but the first and the ones
	that follow a load.

	**Sized by frameBytes, not width * height * 4.** The ABI carries the
	packing explicitly so a host need not assume it, and Frame() measures the
	buffer it is handed against exactly that number: derive it independently
	and a source that ever pads its rows writes past the end of this vector.
*/
bool ResodoomPlugin::EnsurePicture( uint32_t width, uint32_t height, size_t frameBytes )
{
	if( width == 0 || height == 0 || frameBytes == 0 )
		return false;

	if( width == mPictureWidth && height == mPictureHeight )
	{
		if( mFrame.size() < frameBytes )
			mFrame.assign( frameBytes, 0 );
		return true;
	}

	std::string error;
	if( !mPresenter.Create( width, height, error ) )
	{
		// The GL strings go next to the failure because a shader that builds on
		// one machine and not another is a driver answer, not a source answer.
		const GLubyte* vendor   = glGetString( GL_VENDOR );
		const GLubyte* renderer = glGetString( GL_RENDERER );
		stagehand::diag::error( "presenter failed on "
								+ std::string( vendor ? (const char*)vendor : "?" ) + " / "
								+ ( renderer ? (const char*)renderer : "?" ) + ": " + error );

		// Nothing is presentable now, and saying so stops ProcessOpenGL
		// drawing through a texture that was just destroyed.
		mPictureWidth  = 0;
		mPictureHeight = 0;
		return false;
	}

	mFrame.assign( frameBytes, 0 );
	mPictureWidth  = width;
	mPictureHeight = height;
	return true;
}

FFResult ResodoomPlugin::DeInitGL()
{
	mEngine.Close();
	mPresenter.Destroy();

	mFrame.clear();
	mFrame.shrink_to_fit();
	mPictureWidth  = 0;
	mPictureHeight = 0;
	mLoadedIwad.clear();
	mLoadedPwad.clear();

	return FF_SUCCESS;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void ResodoomPlugin::ApplyPendingLoad()
{
	mPendingLoad = false;

	mEngine.Close();
	std::memset( mButtonWasDown, 0, sizeof( mButtonWasDown ) );

	mLoadedIwad = mIwad;
	mLoadedPwad = mPwad;

	if( mIwad.empty() )
	{
		// Not a failure worth logging every time -- this is simply what a
		// freshly dropped plugin looks like before anyone picks a WAD.
		mLoadFailed = false;
		return;
	}

	const std::string engine = EngineLibraryPath();
	if( engine.empty() )
	{
		stagehand::diag::error( "the engine library is missing from the plugin bundle" );
		mLoadFailed = true;
		return;
	}

	char skill[ 8 ], episode[ 8 ], map[ 8 ];
	std::snprintf( skill, sizeof( skill ), "%d", SkillFromParam( mParams[ PT_SKILL ] ) );
	std::snprintf( episode, sizeof( episode ), "%d", OptionIndex( mParams[ PT_EPISODE ], 5 ) );
	std::snprintf( map, sizeof( map ), "%d", OptionIndex( mParams[ PT_MAP ], 33 ) );

	std::vector< StagehandOption > options = {
		{ "iwad", mIwad.c_str() },
		{ "skill", skill },
		{ "episode", episode },
		{ "map", map },
		{ "heap", "16" },
	};
	if( !mPwad.empty() )
		options.push_back( { "pwad", mPwad.c_str() } );

	if( !mEngine.Open( engine, options ) )
	{
		// Latched so the render thread does not rebuild a broken engine sixty
		// times a second -- which would also stage sixty copies of the library
		// into the temp directory before anyone noticed.
		mLoadFailed = true;
		return;
	}

	/*
		The engine's geometry, not this plugin's. Open() fills Info() from the
		source's Describe() and refuses a source that reports nothing usable,
		so by here the numbers are known good -- and a 320x200 engine and a
		widescreen one differ by nothing more than what they answer here.
	*/
	const StagehandInfo& info = mEngine.Info();
	if( !EnsurePicture( info.width, info.height, info.frameBytes ) )
	{
		// The engine is fine; this machine cannot present what it produces.
		// Closing it keeps a running game from drawing into a dead texture.
		stagehand::diag::error( "the engine's picture could not be presented" );
		mEngine.Close();
		mLoadFailed = true;
		return;
	}

	stagehand::diag::info( "engine picture " + std::to_string( info.width ) + "x"
						   + std::to_string( info.height ) );
	mLoadFailed = false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void ResodoomPlugin::SendInput()
{
	for( unsigned p = kFirstButton; p <= kLastButton; ++p )
	{
		const bool down = mParams[ p ] >= 0.5f;
		if( down == mButtonWasDown[ p ] )
			continue;

		mButtonWasDown[ p ] = down;
		mEngine.Event( ButtonKey( p ), down ? 1 : 0 );
	}
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

FFResult ResodoomPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	(void)pGL;

	const int width  = int( mViewport.width );
	const int height = int( mViewport.height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	if( mPendingLoad && !mLoadFailed )
		ApplyPendingLoad();

	SendInput();

	/*
		Release Doom's clock against the wall's.

		Run being off grants nothing, which parks the game exactly where it
		stands -- the engine blocks on its budget rather than spinning, so a
		paused layer costs nothing at all.
	*/
	const float speed =
		mParams[ PT_RUN ] >= 0.5f ? SpeedFromParam( mParams[ PT_SPEED ] ) : 0.0f;
	mEngine.Pump( speed );

	/*
		Doom can fail after a successful open, and usually does when it fails
		at all: Open() only reports that the engine thread came up, and the WAD
		is found, read and rejected on that thread a moment later. Without this
		the log says "source open" and then goes quiet, and the operator has a
		black layer with no entry naming the cause.
	*/
	if( !mLoadFailed && mEngine.Failed() )
	{
		mLoadFailed = true;
		stagehand::diag::error( "the engine gave up after starting: "
								+ mEngine.SourceStatus() );
	}

	if( !mFrame.empty() && mEngine.Frame( mFrame.data(), mFrame.size() ) )
		mPresenter.Upload( mFrame.data() );

	mPresenter.SetSmoothing( mParams[ PT_SMOOTH ] >= 0.5f );

	if( !mPresenter.HasPicture() )
	{
		/*
			No WAD, or the engine has not produced its first frame yet. Leave
			the layer alone rather than clearing it: Resolume has already
			cleared the target, and a plugin that drew black here would flash
			on every composition load.
		*/
		return FF_SUCCESS;
	}

	// Pixel Aspect off means square pixels, which is not what the game looked
	// like but is what somebody asking for the raw buffer wants.
	const float par = mParams[ PT_PIXEL_ASPECT ] >= 0.5f
						  ? mEngine.Info().pixelAspect
						  : 1.0f;

	float sx = 1.0f, sy = 1.0f;
	stagehand::ComputeFit( FitFromParam( mParams[ PT_SCALING ] ), width, height,
						   mPictureWidth, mPictureHeight, par, sx, sy );

	mPresenter.Draw( sx, sy );
	return FF_SUCCESS;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

FFResult ResodoomPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS
																			   : FF_FAIL;

	mParams[ index ] = value;

	switch( index )
	{
		case PT_RESTART:
			// An event arrives as a 1 and is never sent back down. Reloading
			// on the rising edge only means holding it does not thrash.
			if( value >= 0.5f )
			{
				mPendingLoad = true;
				mLoadFailed  = false;
			}
			break;

		case PT_SKILL:
		case PT_EPISODE:
		case PT_MAP:
			// These are baked into Doom's argv at startup, so changing one is
			// a reload. There is no way to warp a running engine without
			// going through its menus.
			mPendingLoad = true;
			mLoadFailed  = false;
			break;

		default:
			break;
	}

	return FF_SUCCESS;
}

float ResodoomPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? mParams[ index ] : 0.0f;
}

FFResult ResodoomPlugin::SetTextParameter( unsigned int index, const char* value )
{
	const std::string incoming = value ? value : "";

	/*
		Handled explicitly, and anything else refused.

		An unhandled TEXT or FILE parameter that falls through to a default
		kills the plugin during a host's instantiate sweep -- the whole bundle
		then reports as broken, for a reason that has nothing to do with Doom.
	*/
	/*
		LOAD-BEARING, and its absence is invisible offline: instantiateGL
		pushes every declared default back through the setters and deletes the
		instance the moment one returns FF_FAIL. Omit the About case and the
		plugin cannot be created in any real host while every in-repo harness
		still passes.
	*/
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	switch( index )
	{
		case PT_IWAD:
			if( incoming != mIwad )
			{
				mIwad        = incoming;
				mPendingLoad = true;
				mLoadFailed  = false;
			}
			return FF_SUCCESS;

		case PT_PWAD:
			if( incoming != mPwad )
			{
				mPwad        = incoming;
				mPendingLoad = true;
				mLoadFailed  = false;
			}
			return FF_SUCCESS;

		default:
			return FF_FAIL;
	}
}

char* ResodoomPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		// The returned buffer must outlive the call, and a temporary
		// std::string's is gone before the host reads it.
		static const std::string line = stoatworks::about::textParam( 0 );
		return const_cast< char* >( line.c_str() );
	}

	switch( index )
	{
		case PT_IWAD: return const_cast< char* >( mIwad.c_str() );
		case PT_PWAD: return const_cast< char* >( mPwad.c_str() );
		default:      return nullptr;
	}
}

} // namespace resodoom

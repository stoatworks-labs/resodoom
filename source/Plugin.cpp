#include "Plugin.h"
#include "Diag.h"

#include <algorithm>
#include <cstring>

namespace resodoom
{
namespace
{

/*
	One quad built from gl_VertexID -- no vertex buffer, same as the rest of the
	fleet's shaders. `Scale` letterboxes it.

	Mind the names: `filter`, `active`, `flat`, `input`, `output`, `sample`,
	`common` and `patch` are GLSL reserved words, and a shader that fails to
	compile surfaces only at runtime, as a plugin that appears to do nothing.
	That is what Diag is for.
*/
const char* kVertexShader = R"(#version 410 core
uniform vec2 Scale;

out vec2 vUv;

void main()
{
	// 0,1,2,3 -> the four corners of a triangle strip.
	vec2 corner = vec2( float( gl_VertexID & 1 ), float( ( gl_VertexID >> 1 ) & 1 ) );

	vUv         = corner;
	gl_Position = vec4( ( corner * 2.0 - 1.0 ) * Scale, 0.0, 1.0 );
}
)";

const char* kFragmentShader = R"(#version 410 core
uniform sampler2D Picture;

in vec2 vUv;
out vec4 fragColour;

void main()
{
	fragColour = vec4( texture( Picture, vUv ).rgb, 1.0 );
}
)";

} // namespace

ResodoomPlugin::ResodoomPlugin()
{
	/*
		Doom writes its failures to stderr and then exits, from deep inside C
		this plugin does not control. Inside Resolume that output goes nowhere,
		and it is usually the only thing that names a bad WAD.
	*/
	diag::CaptureStderr();
	diag::info( "plugin instantiated" );

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
}

ResodoomPlugin::~ResodoomPlugin()
{
	diag::info( "plugin destroyed" );
}

// ---------------------------------------------------------------------------
// GL lifecycle
// ---------------------------------------------------------------------------

bool ResodoomPlugin::BuildShader()
{
	if( mShader.Compile( kVertexShader, kFragmentShader ) )
		return true;

	const GLubyte* vendor   = glGetString( GL_VENDOR );
	const GLubyte* renderer = glGetString( GL_RENDERER );
	const GLubyte* version  = glGetString( GL_VERSION );

	// The GL strings go next to the failure because a shader that builds on one
	// machine and not another is a driver answer, not a source answer.
	diag::error( std::string( "shader compile failed on " )
				 + ( vendor ? (const char*)vendor : "?" ) + " / "
				 + ( renderer ? (const char*)renderer : "?" ) + " / "
				 + ( version ? (const char*)version : "?" ) );
	return false;
}

FFResult ResodoomPlugin::InitGL( const FFGLViewportStruct* vp )
{
	const GLubyte* version = glGetString( GL_VERSION );
	diag::info( std::string( "InitGL, GL " )
				+ ( version ? reinterpret_cast< const char* >( version ) : "unknown" ) );

	if( !BuildShader() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	// A core profile refuses to draw with no vertex array bound, even though
	// the shader builds its geometry from gl_VertexID and sources nothing.
	glGenVertexArrays( 1, &mVAO );
	glGenTextures( 1, &mTexture );

	// One frame's worth of staging, allocated once. 256 KB is too much to put
	// on the render thread's stack and too much to allocate per frame.
	mFrame = std::make_unique< ResodoomFrame >();

	mViewport = *vp;
	return FF_SUCCESS;
}

FFResult ResodoomPlugin::DeInitGL()
{
	mEngine.Unload();
	mShader.FreeGLResources();

	if( mVAO != 0 )
	{
		glDeleteVertexArrays( 1, &mVAO );
		mVAO = 0;
	}
	if( mTexture != 0 )
	{
		glDeleteTextures( 1, &mTexture );
		mTexture = 0;
	}

	mFrame.reset();
	mTextureAllocated = false;
	mUploadedSeq      = 0;
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

	mEngine.Unload();
	mUploadedSeq = 0;
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

	const int episode = OptionIndex( mParams[ PT_EPISODE ], 5 );
	const int map     = OptionIndex( mParams[ PT_MAP ], 33 );

	ResodoomConfig cfg {};
	cfg.iwad          = mIwad.c_str();
	cfg.pwad          = mPwad.empty() ? nullptr : mPwad.c_str();
	cfg.skill         = SkillFromParam( mParams[ PT_SKILL ] );
	cfg.episode       = episode;
	cfg.map           = map;
	cfg.heapMiB       = 16;
	cfg.deterministic = 0;

	if( !mEngine.Load( cfg ) )
	{
		// Latched so the render thread does not rebuild a broken engine sixty
		// times a second -- which would also stage sixty copies of the library
		// into the temp directory before anyone noticed.
		mLoadFailed = true;
		return;
	}
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
		mEngine.Key( ButtonKey( p ), down );
	}
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

bool ResodoomPlugin::UpdateTexture()
{
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );

	if( !mTextureAllocated )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, RESODOOM_WIDTH, RESODOOM_HEIGHT, 0,
					  GL_BGRA, GL_UNSIGNED_BYTE, nullptr );

		// CLAMP_TO_EDGE rather than the default REPEAT. The picture fills the
		// texture exactly, so REPEAT would only show at the seam -- but it
		// shows there as a one-pixel stripe of the opposite edge, which on a
		// status bar is a bright line across the bottom of the screen.
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );

		mTextureAllocated = true;
		mUploadedSeq      = 0;
	}

	const GLint filter = mParams[ PT_SMOOTH ] >= 0.5f ? GL_LINEAR : GL_NEAREST;
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );

	/*
		Only upload a frame that has not been uploaded. Doom runs at 35 Hz in a
		composition running at 60, so five frames in twelve are repeats, and a
		paused game repeats all of them.

		GL_BGRA because that is the byte order Doom's XRGB8888 framebuffer
		already has on a little-endian machine; the engine forces the alpha
		byte to 255 as it copies, because the X is undefined and stale bits
		there would give Resolume a transparent layer.
	*/
	if( mEngine.Frame( *mFrame ) )
	{
		glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, RESODOOM_WIDTH, RESODOOM_HEIGHT,
						 GL_BGRA, GL_UNSIGNED_BYTE, mFrame->pixels );
		mUploadedSeq = mFrame->seq;
	}

	// Nothing has ever been uploaded: the engine is still starting up, and
	// drawing the empty texture would flash black over the layer.
	return mUploadedSeq != 0;
}

void ResodoomPlugin::ComputeQuadScale( int vpWidth, int vpHeight, float& sx, float& sy ) const
{
	sx = 1.0f;
	sy = 1.0f;

	if( vpWidth <= 0 || vpHeight <= 0 )
		return;

	const Scaling mode = ScalingFromParam( mParams[ PT_SCALING ] );
	if( mode == Scaling::Stretch )
		return;

	if( mode == Scaling::Integer )
	{
		/*
			Whole-number pixel multiples only. Aspect correction is deliberately
			ignored here: the entire point of this mode is that one Doom pixel
			is an exact square block of output pixels, and a 1.2 correction
			makes that impossible by definition.
		*/
		const int kx = vpWidth / RESODOOM_WIDTH;
		const int ky = vpHeight / RESODOOM_HEIGHT;
		const int k  = std::max( 1, std::min( kx, ky ) );

		sx = float( RESODOOM_WIDTH * k ) / float( vpWidth );
		sy = float( RESODOOM_HEIGHT * k ) / float( vpHeight );

		// A picture larger than the composition cannot be shown at 1x or more;
		// fall through to fitting rather than overflowing silently.
		if( sx <= 1.0f && sy <= 1.0f )
			return;
	}

	/*
		Doom's 320x200 was always displayed at 4:3 -- its pixels are 1.2 times
		taller than wide. Square pixels are an option rather than the default
		because some people want the raw buffer, but they are not what the game
		looked like.
	*/
	const float pictureAspect = mParams[ PT_PIXEL_ASPECT ] >= 0.5f
									? 4.0f / 3.0f
									: float( RESODOOM_WIDTH ) / float( RESODOOM_HEIGHT );

	const float viewAspect = float( vpWidth ) / float( vpHeight );

	const bool wider = pictureAspect > viewAspect;
	const bool fill  = mode == Scaling::Fill;

	// Fit shrinks the long axis to bring the whole picture in; Fill grows the
	// short one until nothing is uncovered. Same comparison, opposite branch.
	if( wider != fill )
		sy = viewAspect / pictureAspect;
	else
		sx = pictureAspect / viewAspect;
}

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
	const float speed = mParams[ PT_RUN ] >= 0.5f ? SpeedFromParam( mParams[ PT_SPEED ] ) : 0.0f;
	mEngine.Pump( speed );

	if( !UpdateTexture() )
	{
		/*
			No WAD, or the engine has not produced its first frame yet. Leave
			the layer alone rather than clearing it: Resolume has already
			cleared the target, and a plugin that drew black here would flash
			on every composition load.
		*/
		return FF_SUCCESS;
	}

	float sx = 1.0f, sy = 1.0f;
	ComputeQuadScale( width, height, sx, sy );

	// Plain glUseProgram and glBindTexture rather than the ffglex Scoped*
	// helpers: every one of those CLEARS its binding to 0 on scope exit instead
	// of restoring what was there. State is put back by hand below.
	glBindVertexArray( mVAO );
	glUseProgram( mShader.GetGLID() );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );

	mShader.Set( "Picture", 0 );
	mShader.Set( "Scale", sx, sy );

	glDisable( GL_BLEND );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	glUseProgram( 0 );
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );

	return FF_SUCCESS;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

FFResult ResodoomPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

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
	switch( index )
	{
		case PT_IWAD: return const_cast< char* >( mIwad.c_str() );
		case PT_PWAD: return const_cast< char* >( mPwad.c_str() );
		default:      return nullptr;
	}
}

} // namespace resodoom

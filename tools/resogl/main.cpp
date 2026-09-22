/*
	resogl -- the real plugin class, through the real FFGL sequence, in a
	headless 4.1 core-profile context.

	This is the only check that catches a shader that will not compile, a
	uniform whose name does not match the GLSL, or a letterbox branch that has
	its comparison the wrong way round. resotest exercises the engine and never
	touches a graphics API; this drives ResodoomPlugin itself.

	  resogl --iwad W.wad --check
	  resogl --iwad W.wad --check --size 720x720
	  resogl --iwad W.wad --out /tmp/f.ppm
	  resogl --iwad W.wad --check --out /tmp/fitted.ppm    the frame Fit is measured on

	Everything here is portable except getting a context, which nothing has
	ever made portable: CGL on macOS, WGL behind a hidden window on Windows.
	Both ask for 4.1 core, because that is what the presenter's shader wants
	and a context that quietly hands back something older fails later, in the
	shader log, looking like a source problem.

	On a machine with no usable GPU driver -- a VM, a CI runner -- put a
	software GL beside the executable and it will be picked up ahead of the
	system one. Mesa's llvmpipe is what Resolume itself ships for that case.
*/
/*
	**On Windows this block must come before Plugin.h and that is load-bearing.**
	FFGL.h defines NOUSER before it includes windows.h, which drops winuser.h
	-- and with it WNDCLASSA, CreateWindowExA and GetDC, every one of which
	this file needs to make a context. It #undefs NOUSER afterwards, but by
	then windows.h has tripped its own include guard and a second #include is
	a no-op, so winuser.h never arrives at all. Getting in first is the only
	order that works.

	GL/glew.h before any GL header for the usual reason: the system opengl32
	exports GL 1.1, and everything this harness draws with is an extension.
*/
#if defined( _WIN32 )
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>

	#include <GL/glew.h>
	#include <GL/wglew.h>
#endif

#include "Plugin.h"

#if defined( __APPLE__ )
	#include <OpenGL/CGLCurrent.h>
	#include <OpenGL/CGLTypes.h>
	#include <OpenGL/OpenGL.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using resodoom::ResodoomPlugin;

namespace
{

int g_checks   = 0;
int g_failures = 0;

/*
	The checks that predate the Aspect parameter pin it to 4:3, so they test
	exactly what they always did -- the classic engine -- whatever canvas they
	run on. The Aspect block below is where the other engines are exercised.
*/
constexpr int   kEngineWidth     = int( resodoom::kClassicEngineWidth );
constexpr int   kEngineHeight    = int( resodoom::kEngineHeight );
constexpr float kDoomPixelAspect = 5.0f / 6.0f;
constexpr float kAspectClassic   = 1.0f; ///< PT_ASPECT's 4:3 element; 0 is Auto

/// Where Fit should put the picture of an engine this wide on a canvas this
/// shape: the share of each axis it covers. How the Aspect checks tell which
/// engine is running without reaching into the plugin.
void ExpectedFitCover( uint32_t engineWidth, unsigned canvasW, unsigned canvasH,
					   float& coverX, float& coverY )
{
	const float picture = resodoom::EngineDisplayAspect( engineWidth );
	const float canvas  = float( canvasW ) / float( canvasH );
	coverX = picture < canvas ? picture / canvas : 1.0f;
	coverY = picture > canvas ? canvas / picture : 1.0f;
}

void ok( bool condition, const char* what )
{
	g_checks += 1;
	std::printf( condition ? "  ok    %s\n" : "  FAIL  %s\n", what );
	if( !condition )
		g_failures += 1;
}

/*
	A current 4.1 core context, or false. `Destroy` puts everything back.

	The two platforms share nothing here but the shape, so each keeps its own
	handles rather than pretending to a common type.
*/
struct Context
{
#if defined( __APPLE__ )
	CGLContextObj cgl = nullptr;
#elif defined( _WIN32 )
	HWND  window = nullptr;
	HDC   dc     = nullptr;
	HGLRC rc     = nullptr;
#endif

	bool Create();
	void Destroy();
};

#if defined( __APPLE__ )

bool Context::Create()
{
	/*
		Accelerated first, software second. A GitHub macOS runner has no
		accelerated CGL context at all, and falling back is what lets this same
		binary run in CI -- where it still catches every shader and uniform
		error, which is the point.
	*/
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, CGLPixelFormatAttribute( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, CGLPixelFormatAttribute( 24 ),
		kCGLPFAAlphaSize, CGLPixelFormatAttribute( 8 ),
		CGLPixelFormatAttribute( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, CGLPixelFormatAttribute( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, CGLPixelFormatAttribute( 24 ),
		kCGLPFAAlphaSize, CGLPixelFormatAttribute( 8 ),
		CGLPixelFormatAttribute( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint             n      = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &n ) != kCGLNoError || format == nullptr )
		CGLChoosePixelFormat( software, &format, &n );
	if( !format )
		return false;

	CGLCreateContext( format, nullptr, &cgl );
	CGLDestroyPixelFormat( format );
	if( !cgl )
		return false;

	CGLSetCurrentContext( cgl );
	return true;
}

void Context::Destroy()
{
	CGLSetCurrentContext( nullptr );
	if( cgl )
		CGLDestroyContext( cgl );
	cgl = nullptr;
}

#elif defined( _WIN32 )

/*
	WGL's bootstrap problem: choosing a modern pixel format and asking for a
	core profile both go through extensions, and an extension pointer can only
	be resolved from a context that already exists. So a throwaway 1.1 context
	is made first, GLEW is initialised against it, and only then can the real
	one be asked for.

	The window is never shown. All the drawing goes to an FBO; the window
	exists because WGL has no way to make a context without a device context,
	and a device context comes from a window.
*/
bool Context::Create()
{
	WNDCLASSA wc   = {};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance   = GetModuleHandleA( nullptr );
	wc.lpszClassName = "resogl";
	RegisterClassA( &wc );

	window = CreateWindowExA( 0, "resogl", "resogl", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
							  nullptr, nullptr, wc.hInstance, nullptr );
	if( !window )
		return false;

	dc = GetDC( window );
	if( !dc )
		return false;

	PIXELFORMATDESCRIPTOR pfd = {};
	pfd.nSize        = sizeof( pfd );
	pfd.nVersion     = 1;
	pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType   = PFD_TYPE_RGBA;
	pfd.cColorBits   = 24;
	pfd.cAlphaBits   = 8;
	pfd.iLayerType   = PFD_MAIN_PLANE;

	const int format = ChoosePixelFormat( dc, &pfd );
	if( format == 0 || !SetPixelFormat( dc, format, &pfd ) )
		return false;

	HGLRC bootstrap = wglCreateContext( dc );
	if( !bootstrap || !wglMakeCurrent( dc, bootstrap ) )
		return false;

	glewExperimental = GL_TRUE;
	if( glewInit() != GLEW_OK )
	{
		wglMakeCurrent( nullptr, nullptr );
		wglDeleteContext( bootstrap );
		return false;
	}

	if( wglewIsSupported( "WGL_ARB_create_context" ) )
	{
		const int attribs[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
			WGL_CONTEXT_MINOR_VERSION_ARB, 1,
			WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
			0
		};
		rc = wglCreateContextAttribsARB( dc, nullptr, attribs );
	}

	if( rc )
	{
		// Only now is the bootstrap expendable, and it has to go current-off
		// before it can be deleted.
		wglMakeCurrent( nullptr, nullptr );
		wglDeleteContext( bootstrap );
		if( !wglMakeCurrent( dc, rc ) )
			return false;

		// GLEW resolved its pointers against the compatibility context. Most
		// drivers hand back the same addresses, but that is not promised.
		glewExperimental = GL_TRUE;
		if( glewInit() != GLEW_OK )
			return false;
	}
	else
	{
		/*
			No core profile available. Keep the 1.1-era context rather than
			failing: a software GL that only offers compatibility still
			compiles the shader and still catches what this harness is for.
		*/
		rc = bootstrap;
	}

	/*
		glewInit leaves a GL_INVALID_ENUM behind on a core profile -- it probes
		with glGetString(GL_EXTENSIONS), which core removed. Swallowing it here
		keeps it from being reported against the first real draw.
	*/
	glGetError();
	return true;
}

void Context::Destroy()
{
	wglMakeCurrent( nullptr, nullptr );
	if( rc )
		wglDeleteContext( rc );
	if( dc && window )
		ReleaseDC( window, dc );
	if( window )
		DestroyWindow( window );
	rc     = nullptr;
	dc     = nullptr;
	window = nullptr;
}

#endif

struct Target
{
	GLuint   fbo    = 0;
	GLuint   colour = 0;
	unsigned width  = 0;
	unsigned height = 0;
};

Target MakeTarget( unsigned width, unsigned height )
{
	Target t;
	t.width  = width;
	t.height = height;

	glGenTextures( 1, &t.colour );
	glBindTexture( GL_TEXTURE_2D, t.colour );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei( width ), GLsizei( height ), 0,
				  GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );

	glGenFramebuffers( 1, &t.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.colour, 0 );

	glBindTexture( GL_TEXTURE_2D, 0 );
	return t;
}

void DestroyTarget( Target& t )
{
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glDeleteFramebuffers( 1, &t.fbo );
	glDeleteTextures( 1, &t.colour );
	t = Target {};
}

std::vector< uint8_t > DrawOnce( ResodoomPlugin& plugin, const Target& target )
{
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, GLsizei( target.width ), GLsizei( target.height ) );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT );

	ProcessOpenGLStruct gl = {};
	gl.numInputTextures    = 0;
	gl.HostFBO             = target.fbo;

	plugin.ProcessOpenGL( &gl );
	glFinish();

	std::vector< uint8_t > rgba( size_t( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glReadPixels( 0, 0, GLsizei( target.width ), GLsizei( target.height ), GL_RGBA,
				  GL_UNSIGNED_BYTE, rgba.data() );
	return rgba;
}

bool IsBlank( const std::vector< uint8_t >& rgba )
{
	for( size_t i = 0; i < rgba.size(); i += 4 )
		if( rgba[ i ] || rgba[ i + 1 ] || rgba[ i + 2 ] )
			return false;
	return true;
}

/*
	Draw until the picture arrives, or give up.

	**Every wait here is on a condition, never on a frame count.** The engine
	runs on its own thread against a wall clock, so how many draws it takes for
	the first frame to appear depends on the machine -- a fixed count is a test
	that passes on this laptop and fails in CI.
*/
std::vector< uint8_t > DrawUntilPicture( ResodoomPlugin& plugin, const Target& target,
										 int maxDraws = 2000 )
{
	std::vector< uint8_t > rgba;
	for( int i = 0; i < maxDraws; ++i )
	{
		rgba = DrawOnce( plugin, target );
		if( !IsBlank( rgba ) )
			return rgba;
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
	return rgba;
}

/*
	Draw until the picture is actually CHANGING between consecutive draws.

	Any test about motion -- pausing, resuming, two instances being at
	different points -- is worthless while the screen is static, and Doom opens
	on a title screen that holds for about five seconds before the first
	attract demo starts. Comparing two identical still frames passes whatever
	the code does, in both directions.

	Callers turn the Speed parameter up first, which is what makes waiting for
	this cheap rather than a five-second sleep per check.
*/
bool AdvanceUntilMoving( ResodoomPlugin& plugin, const Target& target, int maxDraws = 3000 )
{
	auto previous = DrawOnce( plugin, target );
	for( int i = 0; i < maxDraws; ++i )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
		auto current = DrawOnce( plugin, target );
		if( !IsBlank( current ) && current != previous )
			return true;
		previous = std::move( current );
	}
	return false;
}

/// The bounding box of everything non-black, as a fraction of the target.
void InkExtent( const std::vector< uint8_t >& rgba, unsigned w, unsigned h,
				float& coverX, float& coverY )
{
	unsigned minX = w, maxX = 0, minY = h, maxY = 0;
	bool     any = false;

	for( unsigned y = 0; y < h; ++y )
		for( unsigned x = 0; x < w; ++x )
		{
			const size_t i = ( size_t( y ) * w + x ) * 4;
			if( rgba[ i ] || rgba[ i + 1 ] || rgba[ i + 2 ] )
			{
				any = true;
				if( x < minX ) minX = x;
				if( x > maxX ) maxX = x;
				if( y < minY ) minY = y;
				if( y > maxY ) maxY = y;
			}
		}

	if( !any )
	{
		coverX = coverY = 0.0f;
		return;
	}
	coverX = float( maxX - minX + 1 ) / float( w );
	coverY = float( maxY - minY + 1 ) / float( h );
}

bool WritePpm( const char* path, const std::vector< uint8_t >& rgba, unsigned w, unsigned h )
{
	FILE* fp = std::fopen( path, "wb" );
	if( !fp )
		return false;
	std::fprintf( fp, "P6\n%u %u\n255\n", w, h );
	// glReadPixels is bottom-up; PPM is top-down.
	for( int y = int( h ) - 1; y >= 0; --y )
		for( unsigned x = 0; x < w; ++x )
		{
			const size_t i = ( size_t( y ) * w + x ) * 4;
			std::fputc( rgba[ i ], fp );
			std::fputc( rgba[ i + 1 ], fp );
			std::fputc( rgba[ i + 2 ], fp );
		}
	std::fclose( fp );
	return true;
}

/// Brings a plugin up to the point where it is drawing the given WAD.
bool Bring( ResodoomPlugin& plugin, const FFGLViewportStruct& vp, const std::string& iwad )
{
	if( plugin.InitGL( &vp ) != FF_SUCCESS )
		return false;
	plugin.SetTextParameter( resodoom::PT_IWAD, iwad.c_str() );
	return true;
}

int SelfTest( const std::string& iwad, unsigned width, unsigned height,
			  const std::string& dumpFitted )
{
	std::printf( "resogl: %ux%u\n", width, height );
	std::printf( "resogl: iwad %s\n\n", iwad.c_str() );

	const GLubyte* renderer = glGetString( GL_RENDERER );
	std::printf( "resogl: renderer %s\n\n",
				 renderer ? reinterpret_cast< const char* >( renderer ) : "?" );

	Target target = MakeTarget( width, height );
	FFGLViewportStruct vp { 0, 0, width, height };

	/* --- the shader compiles at all -------------------------------- */
	{
		ResodoomPlugin plugin;
		ok( plugin.InitGL( &vp ) == FF_SUCCESS,
			"InitGL succeeds (the shader compiles and the uniforms resolve)" );
		plugin.DeInitGL();
	}

	/* --- no WAD draws nothing, rather than crashing or flashing ---- */
	{
		ResodoomPlugin plugin;
		plugin.InitGL( &vp );
		auto rgba = DrawOnce( plugin, target );
		ok( IsBlank( rgba ), "with no WAD selected the plugin leaves the layer alone" );
		plugin.DeInitGL();
	}

	/* --- a bad path fails quietly and does not wedge --------------- */
	{
		ResodoomPlugin plugin;
		plugin.InitGL( &vp );
		plugin.SetTextParameter( resodoom::PT_IWAD, "/nonexistent/nope.wad" );

		for( int i = 0; i < 10; ++i )
			DrawOnce( plugin, target );

		ok( IsBlank( DrawOnce( plugin, target ) ),
			"a WAD that cannot be loaded leaves the layer alone too" );
		plugin.DeInitGL();
	}

	/* --- the real thing -------------------------------------------- */
	std::vector< uint8_t > fitted;
	{
		ResodoomPlugin plugin;
		plugin.SetFloatParameter( resodoom::PT_ASPECT, kAspectClassic );
		ok( Bring( plugin, vp, iwad ), "InitGL and a real WAD path are accepted" );

		fitted = DrawUntilPicture( plugin, target );

		/*
			The frame every Fit assertion below is measured against. Written
			out on request because a pass here says only that non-black pixels
			reached the edges -- not what they were. A widescreen title page
			is 320-wide art in a wider buffer, and whether its uncovered strip
			is black or stale memory decides the measurement.
		*/
		if( !dumpFitted.empty() )
			WritePpm( dumpFitted.c_str(), fitted, width, height );
		ok( !IsBlank( fitted ), "the game reaches the screen" );

		/*
			Fit must letterbox on exactly one axis. Run this at two aspects: a
			sign error in the fit branch is invisible whenever the picture
			happens to be wider than the frame, and a square target is the
			cheapest way to make the other branch matter.
		*/
		float coverX = 0.0f, coverY = 0.0f;
		InkExtent( fitted, width, height, coverX, coverY );

		/*
			The picture's display aspect, from the geometry this build gave the
			engine rather than from a literal 4:3. A widescreen engine letterboxes
			on the other axis at a given target, and a hardcoded 4:3 here would
			assert the opposite of the correct answer without ever failing to
			compile.
		*/
		const float pictureAspect =
			( float( kEngineWidth ) / float( kEngineHeight ) ) * kDoomPixelAspect;

		const float targetAspect = float( width ) / float( height );

		std::printf( "  ....  picture %dx%d at %.3f, frame %.3f, ink %.3f x %.3f\n",
					 kEngineWidth, kEngineHeight, pictureAspect, targetAspect,
					 coverX, coverY );

		/*
			Three cases, not two, and the third is the whole point of letting
			the engine choose its own geometry: a picture built for the frame's
			aspect has no bars on either axis.

			Two lines of tolerance rather than one. 0.02 on the aspect is about
			a pixel of slack at these sizes, and the ink extent is measured
			against black -- so a frame whose edge column happens to be black
			reads as very slightly short of full.
		*/
		const float aspectGap = pictureAspect > targetAspect ? pictureAspect - targetAspect
															 : targetAspect - pictureAspect;

		if( aspectGap < 0.02f )
		{
			ok( coverX > 0.98f && coverY > 0.98f,
				"Fit fills both axes when the picture already matches the frame" );
		}
		else if( pictureAspect > targetAspect )
		{
			ok( coverX > 0.98f, "Fit fills the width when the picture is the wider one" );
			ok( coverY < 0.99f, "...and letterboxes the height" );
		}
		else
		{
			ok( coverY > 0.98f, "Fit fills the height when the frame is the wider one" );
			ok( coverX < 0.99f, "...and pillarboxes the width" );
		}

		/* --- Stretch covers everything ----------------------------- */
		plugin.SetFloatParameter( resodoom::PT_SCALING, 2.0f );
		auto stretched = DrawUntilPicture( plugin, target );
		InkExtent( stretched, width, height, coverX, coverY );
		ok( coverX > 0.98f && coverY > 0.98f, "Stretch covers the whole frame" );

		/* --- pausing really stops the game ------------------------- */
		plugin.SetFloatParameter( resodoom::PT_SCALING, 0.0f );

		// Speed up, and wait for something on screen to actually move. Doom
		// holds a still title screen for about five seconds first, and every
		// check below would pass trivially against a static picture.
		plugin.SetFloatParameter( resodoom::PT_SPEED, 1.0f );
		ok( AdvanceUntilMoving( plugin, target ), "Speed runs the game on to a moving scene" );

		plugin.SetFloatParameter( resodoom::PT_RUN, 0.0f );

		// Let whatever was already granted drain before sampling.
		for( int i = 0; i < 40; ++i )
			DrawOnce( plugin, target );
		auto held = DrawOnce( plugin, target );

		std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
		for( int i = 0; i < 40; ++i )
			DrawOnce( plugin, target );
		auto stillHeld = DrawOnce( plugin, target );

		ok( held == stillHeld, "Run off holds a moving picture still" );

		plugin.SetFloatParameter( resodoom::PT_RUN, 1.0f );
		bool moved = false;
		for( int i = 0; i < 600 && !moved; ++i )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			moved = DrawOnce( plugin, target ) != stillHeld;
		}
		ok( moved, "...and Run on starts it again" );

		plugin.DeInitGL();
	}

	/* --- two instances are two games ------------------------------- */
	{
		/*
			The claim the whole private-copy arrangement exists to make. Two
			plugins sharing one loaded image would share one player and one
			framebuffer, and would render identically forever.
		*/
		/*
			Two instances, warped to two different levels.

			Comparing two instances of the SAME level is not a test: the engine
			is deterministic, so two copies given the same input render the same
			pixels by design, and any difference would only be a timing
			accident. Worse, Doom cycles through static screens between attract
			demos, and two instances parked on the title screen are identical
			whether the copies worked or not -- which is exactly how this check
			first passed for the wrong reason and then failed for it.

			Different levels cannot coincide. If the two shared one loaded image
			they would share one game, and the second Start would be refused
			outright -- so `b` would draw nothing at all.
		*/
		ResodoomPlugin a, b;
		a.SetFloatParameter( resodoom::PT_ASPECT, kAspectClassic );
		b.SetFloatParameter( resodoom::PT_ASPECT, kAspectClassic );
		a.SetFloatParameter( resodoom::PT_EPISODE, 1.0f );
		a.SetFloatParameter( resodoom::PT_MAP, 1.0f );
		b.SetFloatParameter( resodoom::PT_EPISODE, 1.0f );
		b.SetFloatParameter( resodoom::PT_MAP, 3.0f );

		Bring( a, vp, iwad );
		Bring( b, vp, iwad );

		auto aPic = DrawUntilPicture( a, target );
		auto bPic = DrawUntilPicture( b, target );

		ok( !IsBlank( aPic ), "a first instance is running" );
		ok( !IsBlank( bPic ), "a second instance runs alongside it" );
		ok( aPic != bPic, "the two instances hold two different games" );

		a.DeInitGL();
		b.DeInitGL();
	}

	/* --- Aspect picks the engine ----------------------------------- */
	{
		/*
			Auto against one canvas per engine, then a fixed choice that must
			override Auto, then a change of Aspect on a running layer.

			Which engine is running is read from OUTSIDE, by the shape Fit
			gives its picture: each engine's picture has a different aspect,
			so the ink extents name the engine without the harness reaching
			into the plugin. The canvases are small because only their shape
			matters. Ink counts Doom's (1,1,1) black, so a title page's
			pillarbox reads as picture -- which is right: it is the buffer's
			extent being measured, not the art's.
		*/
		// Not "near": windows.h still defines near and far as empty macros from
		// 16-bit pointers, and `auto near = ...` compiles as `auto = ...`.
		auto closeTo = []( float a, float b ) { return a > b - 0.02f && a < b + 0.02f; };

		struct Case
		{
			unsigned    w, h;
			float       aspect;  // PT_ASPECT: 0 Auto, 1..4 fixed
			uint32_t    engine;  // the width that should be running
			const char* what;
		};
		const Case cases[] = {
			{ 480, 480, 0.0f, 320, "Auto on a square canvas runs the 4:3 engine" },
			{ 640, 400, 0.0f, 384, "Auto on a 16:10 canvas runs the 16:10 engine" },
			{ 640, 360, 0.0f, 426, "Auto on a 16:9 canvas runs the 16:9 engine" },
			{ 840, 360, 0.0f, 568, "Auto on a 21:9 canvas runs the 21:9 engine" },
			{ 480, 480, 3.0f, 426, "a fixed 16:9 overrides Auto on a square canvas" },
		};

		for( const Case& c : cases )
		{
			Target             t   = MakeTarget( c.w, c.h );
			FFGLViewportStruct cvp { 0, 0, c.w, c.h };

			ResodoomPlugin plugin;
			plugin.SetFloatParameter( resodoom::PT_ASPECT, c.aspect );
			Bring( plugin, cvp, iwad );

			auto  rgba = DrawUntilPicture( plugin, t );
			float gotX = 0.0f, gotY = 0.0f, wantX = 0.0f, wantY = 0.0f;
			InkExtent( rgba, c.w, c.h, gotX, gotY );
			ExpectedFitCover( c.engine, c.w, c.h, wantX, wantY );

			std::printf( "  ....  %ux%u canvas: ink %.3f x %.3f; the %u engine gives %.3f x %.3f\n",
						 c.w, c.h, gotX, gotY, c.engine, wantX, wantY );
			ok( closeTo( gotX, wantX ) && closeTo( gotY, wantY ), c.what );

			plugin.DeInitGL();
			DestroyTarget( t );
		}

		/*
			On a running layer: choosing the aspect it already has changes
			nothing, and choosing another swaps the engine.

			The first is the one that matters mid-show -- Resolume re-sends
			every value on composition load and on undo, and a restart there
			would dump the operator back at the title page. It is caught the
			only way a restart shows from outside: the picture going BACK to
			the title page it started on, after the game had moved on.
		*/
		{
			Target             t   = MakeTarget( 640, 360 );
			FFGLViewportStruct cvp { 0, 0, 640, 360 };

			ResodoomPlugin plugin; // Auto, which on 16:9 is the 426 engine
			Bring( plugin, cvp, iwad );
			const auto title = DrawUntilPicture( plugin, t );

			plugin.SetFloatParameter( resodoom::PT_SPEED, 1.0f );
			const bool moving = AdvanceUntilMoving( plugin, t );
			plugin.SetFloatParameter( resodoom::PT_SPEED, 0.5f );

			plugin.SetFloatParameter( resodoom::PT_ASPECT, 3.0f ); // 16:9: the same engine
			bool backAtTitle = false;
			for( int i = 0; i < 40 && !backAtTitle; ++i )
			{
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
				backAtTitle = DrawOnce( plugin, t ) == title;
			}
			ok( moving && !backAtTitle,
				"choosing the aspect a layer already runs does not restart the game" );

			/*
				There and back before the next frame: a MIDI knob swept through
				4:3 and back, or automation crossing values within one frame.
				The first change asks for a reload; the second must withdraw it,
				or the game restarts for a choice that ended where it began.
			*/
			plugin.SetFloatParameter( resodoom::PT_ASPECT, kAspectClassic );
			plugin.SetFloatParameter( resodoom::PT_ASPECT, 3.0f );
			backAtTitle = false;
			for( int i = 0; i < 40 && !backAtTitle; ++i )
			{
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
				backAtTitle = DrawOnce( plugin, t ) == title;
			}
			ok( !backAtTitle,
				"an Aspect change undone before the next frame does not restart the game" );

			plugin.SetFloatParameter( resodoom::PT_ASPECT, kAspectClassic );
			std::vector< uint8_t > rgba;
			float gotX = 0.0f, gotY = 0.0f, wantX = 0.0f, wantY = 0.0f;
			ExpectedFitCover( resodoom::kClassicEngineWidth, 640, 360, wantX, wantY );
			for( int i = 0; i < 2000; ++i )
			{
				rgba = DrawOnce( plugin, t );
				InkExtent( rgba, 640, 360, gotX, gotY );
				if( !IsBlank( rgba ) && closeTo( gotX, wantX ) && closeTo( gotY, wantY ) )
					break;
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
			ok( closeTo( gotX, wantX ) && closeTo( gotY, wantY ),
				"choosing another aspect on a running layer swaps the engine" );

			plugin.DeInitGL();
			DestroyTarget( t );
		}
	}

	std::printf( "\nresogl: %d checks, %d failures\n", g_checks, g_failures );
	return g_failures == 0 ? 0 : 1;
}

} // namespace

int main( int argc, char** argv )
{
	std::string iwad;
	std::string out;
	unsigned    width = 1280, height = 720;
	bool        doCheck = false;

	for( int i = 1; i < argc; ++i )
	{
		if( !std::strcmp( argv[ i ], "--check" ) )
			doCheck = true;
		else if( !std::strcmp( argv[ i ], "--iwad" ) && i + 1 < argc )
			iwad = argv[ ++i ];
		else if( !std::strcmp( argv[ i ], "--out" ) && i + 1 < argc )
			out = argv[ ++i ];
		else if( !std::strcmp( argv[ i ], "--size" ) && i + 1 < argc )
			std::sscanf( argv[ ++i ], "%ux%u", &width, &height );
		else
		{
			std::fprintf( stderr,
						  "usage: resogl --iwad PATH [--check] [--size WxH] [--out F.ppm]\n" );
			return 2;
		}
	}

	if( iwad.empty() )
	{
		std::fprintf( stderr, "resogl: --iwad is required\n" );
		return 2;
	}

	Context context;
	if( !context.Create() )
	{
		std::fprintf( stderr, "resogl: no OpenGL context\n" );
		return 1;
	}

	/*
		Refuse a context too old to run the presenter, rather than letting it
		crash on the first null extension pointer.

		Windows' stock opengl32 is a GL 1.1 software rasteriser with no shader
		entry points at all, and that is what a machine with no GPU driver --
		a VM, a CI runner -- hands back. Without this check the harness dies at
		glCreateProgram with an access violation and no output whatsoever,
		which reads like a bug in the plugin rather than a missing driver.
	*/
	/* Windows only: GLEW makes these function POINTERS, so testing one is
	   meaningful. On macOS they are ordinary functions and the same test is
	   a tautology the compiler rightly warns about. */
#if defined( _WIN32 )
	if( glCreateProgram == nullptr || glGenFramebuffers == nullptr )
	{
		const GLubyte* version  = glGetString( GL_VERSION );
		const GLubyte* renderer = glGetString( GL_RENDERER );
		std::fprintf( stderr,
					  "resogl: this OpenGL cannot run the presenter -- no shader "
					  "or framebuffer entry points.\n"
					  "        renderer: %s\n"
					  "        version:  %s\n"
					  "        Put a software GL (Mesa's opengl32.dll and "
					  "libgallium_wgl.dll) beside this\n"
					  "        executable, or install a GPU driver.\n",
					  renderer ? (const char*)renderer : "?",
					  version ? (const char*)version : "?" );
		context.Destroy();
		return 1;
	}
#endif

	int rc = 0;
	if( doCheck )
	{
		rc = SelfTest( iwad, width, height, out );
	}
	else
	{
		Target             target = MakeTarget( width, height );
		FFGLViewportStruct vp { 0, 0, width, height };
		ResodoomPlugin     plugin;
		Bring( plugin, vp, iwad );

		auto rgba = DrawUntilPicture( plugin, target );
		if( IsBlank( rgba ) )
		{
			std::fprintf( stderr, "resogl: nothing was drawn\n" );
			rc = 1;
		}
		else if( !out.empty() && !WritePpm( out.c_str(), rgba, width, height ) )
		{
			std::fprintf( stderr, "resogl: could not write %s\n", out.c_str() );
			rc = 1;
		}
		plugin.DeInitGL();
	}

	context.Destroy();
	return rc;
}

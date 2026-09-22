/*
	resogl -- the real plugin class, through the real FFGL sequence, in a
	headless CGL 4.1 core-profile context.

	This is the only check that catches a shader that will not compile, a
	uniform whose name does not match the GLSL, or a letterbox branch that has
	its comparison the wrong way round. resotest exercises the engine and never
	touches a graphics API; this drives ResodoomPlugin itself.

	  resogl --iwad W.wad --check
	  resogl --iwad W.wad --check --size 720x720
	  resogl --iwad W.wad --out /tmp/f.ppm

	Everything here is portable except getting a context, which nothing has
	ever made portable: CGL on macOS, WGL behind a hidden window on Windows.
	Both ask for 4.1 core, because that is what the presenter's shader wants
	and a context that quietly hands back something older fails later, in the
	shader log, looking like a source problem.

	On a machine with no usable GPU driver -- a VM, a CI runner -- put a
	software GL beside the executable and it will be picked up ahead of the
	system one. Mesa's llvmpipe is what Resolume itself ships for that case.
*/
#include "Plugin.h"

#if defined( __APPLE__ )
	#include <OpenGL/CGLCurrent.h>
	#include <OpenGL/CGLTypes.h>
	#include <OpenGL/OpenGL.h>
#elif defined( _WIN32 )
	#include <windows.h>
	// After windows.h, and GL/glew.h before any GL call: on Windows the system
	// opengl32 exports GL 1.1 and everything this harness draws with arrives
	// through an extension pointer.
	#include <GL/glew.h>
	#include <GL/wglew.h>
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
	What the engine this harness links against was built to produce. From
	CMake, so the Fit assertions below stay correct at any width rather than
	quietly testing 4:3 against a 16:9 picture.
*/
#ifndef RESODOOM_SCREEN_WIDTH
	#define RESODOOM_SCREEN_WIDTH 320
#endif
#ifndef RESODOOM_SCREEN_HEIGHT
	#define RESODOOM_SCREEN_HEIGHT 200
#endif

constexpr int   kEngineWidth     = RESODOOM_SCREEN_WIDTH;
constexpr int   kEngineHeight    = RESODOOM_SCREEN_HEIGHT;
constexpr float kDoomPixelAspect = 5.0f / 6.0f;

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

int SelfTest( const std::string& iwad, unsigned width, unsigned height )
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
		ok( Bring( plugin, vp, iwad ), "InitGL and a real WAD path are accepted" );

		fitted = DrawUntilPicture( plugin, target );
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
		const bool  pictureWider = pictureAspect > targetAspect;

		if( pictureWider )
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

	int rc = 0;
	if( doCheck )
	{
		rc = SelfTest( iwad, width, height );
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

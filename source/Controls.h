#pragma once

#include <stagehand/Present.h>

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from. An include above the
// SDK's fails with four "unknown type name" errors pointing here rather than
// at the include order.
#include "StoatworksAboutParams.h"

#include <cmath>

/**
	The host-facing parameters, and the conversions off them.

	**Every ranged parameter is 0..1 and mapped here.** `SetParamInfo` clamps an
	`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
	can only be called afterwards -- there is no `SetParamDefault`. A default
	speed of 1.0x declared as a ranged parameter would silently land at the top
	of the range. Same trap as the rest of the fleet, same answer: keep the host
	in 0..1 and do the arithmetic on this side.

	**The twelve control buttons are plain booleans on purpose.** That is what
	makes Resolume MIDI-map them, keyboard-map them and automate them off the
	timeline with no code here at all: a hardware controller becomes a Doom pad,
	and someone who wants to sequence a playthrough as a visual can keyframe it.
	FFGL gives a plugin no keyboard access whatsoever, so this is not a
	convenience -- it is the only input path that exists.
*/
namespace resodoom
{

enum ParamId : unsigned
{
	PT_IWAD = 0, ///< FF_TYPE_FILE -- the game data. Required.
	PT_PWAD,     ///< FF_TYPE_FILE -- an optional mod/level WAD
	PT_RUN,      ///< FF_TYPE_BOOLEAN -- false parks the game where it stands
	PT_RESTART,  ///< FF_TYPE_EVENT -- reload from scratch
	PT_SPEED,    ///< FF_TYPE_STANDARD
	PT_SKILL,    ///< FF_TYPE_OPTION
	PT_EPISODE,  ///< FF_TYPE_OPTION -- 0 = leave Doom at its title screen
	PT_MAP,      ///< FF_TYPE_OPTION -- 0 = ditto
	PT_SCALING,  ///< FF_TYPE_OPTION
	PT_PIXEL_ASPECT, ///< FF_TYPE_BOOLEAN
	PT_SMOOTH,       ///< FF_TYPE_BOOLEAN

	// The controls. Order is what a player expects to read down the inspector.
	PT_FORWARD,
	PT_BACK,
	PT_TURN_LEFT,
	PT_TURN_RIGHT,
	PT_STRAFE_LEFT,
	PT_STRAFE_RIGHT,
	PT_FIRE,
	PT_USE,
	PT_SPRINT,
	PT_MENU,
	PT_CONFIRM,
	PT_AUTOMAP,

	/*
		Which engine to load, by aspect: Auto, or one of kEngineAspects.

		**Here, after the controls, and not beside Scaling where it belongs by
		meaning.** Resolume itself would not mind either way: it addresses
		parameters by name, in saved compositions and in MIDI, keyboard and OSC
		mappings alike (checked in Arena 7.27.1). But FFGL's own ABI is by
		index, and not every host is Resolume, so a new parameter still goes at
		the end, where it shifts nothing any host could have saved.
	*/
	PT_ASPECT,       ///< FF_TYPE_OPTION -- 0 is Auto

	/*
		The Stoatworks About block: one text line and one button per link.

		Last in the enum so that adding a link later -- which is what happens
		when a project gains a user guide -- shifts nothing a saved composition
		already refers to. How many buttons there are is decided by which URLs
		StoatworksAbout.h actually holds, so the count comes from there rather
		than being written out here.
	*/
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
};

constexpr unsigned kFirstButton = PT_FORWARD;
constexpr unsigned kLastButton  = PT_AUTOMAP;

/*
	doomkeys.h codes. Spelled out rather than included, because doomkeys.h
	lives inside the engine's include path and this header is compiled into the
	plugin, which must not see any of doomgeneric's headers -- that is the
	whole point of the ABI.

	The values are part of the ABI in practice: they are what Key() takes.
*/
inline int ButtonKey( unsigned param )
{
	switch( param )
	{
		case PT_FORWARD:      return 0xad; // KEY_UPARROW
		case PT_BACK:         return 0xaf; // KEY_DOWNARROW
		case PT_TURN_LEFT:    return 0xac; // KEY_LEFTARROW
		case PT_TURN_RIGHT:   return 0xae; // KEY_RIGHTARROW
		case PT_STRAFE_LEFT:  return 0xa0; // KEY_STRAFE_L
		case PT_STRAFE_RIGHT: return 0xa1; // KEY_STRAFE_R
		case PT_FIRE:         return 0xa3; // KEY_FIRE
		case PT_USE:          return 0xa2; // KEY_USE
		case PT_SPRINT:       return 0xb6; // KEY_RSHIFT
		case PT_MENU:         return 27;   // KEY_ESCAPE
		case PT_CONFIRM:      return 13;   // KEY_ENTER
		case PT_AUTOMAP:      return 9;    // KEY_TAB
		default:              return 0;
	}
}

/*
	**Every parameter name must be unique, and must never change.** Resolume
	stores and maps parameters by NAME: a saved composition writes
	<Param name="..."> and a MIDI, keyboard or OSC mapping targets
	.../video/source/swresodoom/<name, lower case, no spaces>. In 0.2.0 Sprint
	was called "Run", like the pause switch, and Arena gave the two one address:
	it restored both saved values onto the pause switch, and the control could
	not be mapped on its own. resogl checks every name for this.

	A rename is not free either -- the old name stops matching what compositions
	and mappings already hold -- so this one moved the control rather than the
	pause switch: whatever a 0.2.0 composition saved as "Run" still lands on Run.
*/
inline const char* ButtonName( unsigned param )
{
	switch( param )
	{
		case PT_FORWARD:      return "Forward";
		case PT_BACK:         return "Back";
		case PT_TURN_LEFT:    return "Turn Left";
		case PT_TURN_RIGHT:   return "Turn Right";
		case PT_STRAFE_LEFT:  return "Strafe Left";
		case PT_STRAFE_RIGHT: return "Strafe Right";
		case PT_FIRE:         return "Fire";
		case PT_USE:          return "Use";
		case PT_SPRINT:       return "Sprint";
		case PT_MENU:         return "Menu";
		case PT_CONFIRM:      return "Confirm";
		case PT_AUTOMAP:      return "Automap";
		default:              return "";
	}
}

/*
	The inspector's names for stagehand::Fit. "Fit" and "Fill" are what a VJ
	expects to read in a Resolume inspector; "Contain" and "Cover" are what the
	rest of the world calls the same two things.
*/
inline stagehand::Fit FitFromParam( float value )
{
	switch( int( std::lround( value ) ) )
	{
		case 1:  return stagehand::Fit::Cover;
		case 2:  return stagehand::Fit::Stretch;
		case 3:  return stagehand::Fit::Integer;
		default: return stagehand::Fit::Contain;
	}
}

/*
	Speed, exponential and centred so mid-travel is exactly 1.0x.

	Exponential because the useful range is multiplicative: half speed and
	double speed should sit the same distance either side of normal, and a
	linear control spends most of its travel between 1x and 4x where almost
	nothing interesting happens.
*/
inline float SpeedFromParam( float value )
{
	if( value <= 0.0f )
		return 0.0f; // hard stop at the bottom of the fader, not 1/8 speed
	return std::pow( 8.0f, ( value - 0.5f ) * 2.0f );
}

/// Skill 0 means "leave Doom's own default"; 1..5 are Doom's difficulties.
inline int SkillFromParam( float value )
{
	const int index = int( std::lround( value ) );
	return ( index >= 1 && index <= 5 ) ? index : 0;
}

inline int OptionIndex( float value, int count )
{
	int index = int( std::lround( value ) );
	if( index < 0 )
		index = 0;
	if( index >= count )
		index = count - 1;
	return index;
}

/*
	The engines the plugin ships, one per aspect, in the order the Aspect menu
	lists them after Auto.

	**This is CMakeLists.txt's RESODOOM_ENGINE_WIDTHS -- the same list.** Doom's
	buffer width is compile-time, so each entry is a separately built library in
	the bundle, and verify.sh checks every one is there.
*/
struct EngineAspect
{
	uint32_t    width;
	const char* label;
};

constexpr EngineAspect kEngineAspects[] = {
	{ 320, "4:3" },
	{ 384, "16:10" },
	{ 426, "16:9" },
	{ 568, "21:9" },
};
constexpr int      kEngineAspectCount = int( sizeof( kEngineAspects ) / sizeof( kEngineAspects[ 0 ] ) );
constexpr uint32_t kClassicEngineWidth = 320;
constexpr uint32_t kEngineHeight       = 200;

/// Doom's pixels are 5:6 -- taller than wide -- so the displayed shape of a
/// W x 200 buffer is (W / 200) * 5/6: 4:3 at 320, 16:9 at 426.
inline float EngineDisplayAspect( uint32_t width )
{
	return ( float( width ) / float( kEngineHeight ) ) * ( 5.0f / 6.0f );
}

/*
	The engine whose picture is closest in shape to a canvas.

	Closest as a RATIO: the log of one aspect over the other, so a picture half
	as wide as the canvas is exactly as far off as one twice as wide. A plain
	difference is not symmetric that way and leans narrow near the midpoints.

	**Pixel Aspect is deliberately left out.** Auto matches the shape Doom is
	meant to be seen at, so turning Pixel Aspect off to look at raw pixels never
	swaps the engine -- which would restart the game under the operator.

	A canvas with no size yet (before InitGL) gets the classic engine.
*/
inline uint32_t AutoEngineWidth( uint32_t canvasWidth, uint32_t canvasHeight )
{
	if( canvasWidth == 0 || canvasHeight == 0 )
		return kClassicEngineWidth;

	const float canvas = float( canvasWidth ) / float( canvasHeight );

	uint32_t best     = kClassicEngineWidth;
	float    bestDist = 0.0f;
	for( int i = 0; i < kEngineAspectCount; ++i )
	{
		const float dist =
			std::fabs( std::log( EngineDisplayAspect( kEngineAspects[ i ].width ) / canvas ) );
		if( i == 0 || dist < bestDist )
		{
			best     = kEngineAspects[ i ].width;
			bestDist = dist;
		}
	}
	return best;
}

/// The engine the Aspect parameter asks for: 0 is Auto, 1..N are fixed.
inline uint32_t EngineWidthFromParam( float value, uint32_t canvasWidth,
									  uint32_t canvasHeight )
{
	const int index = OptionIndex( value, kEngineAspectCount + 1 );
	if( index == 0 )
		return AutoEngineWidth( canvasWidth, canvasHeight );
	return kEngineAspects[ index - 1 ].width;
}

} // namespace resodoom

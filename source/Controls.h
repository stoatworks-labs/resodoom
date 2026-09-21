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
		case PT_SPRINT:       return "Run";
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

} // namespace resodoom

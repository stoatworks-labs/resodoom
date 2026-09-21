cmake_minimum_required( VERSION 3.20 )

project( resodoom VERSION 0.1.0 LANGUAGES C CXX )

set( CMAKE_C_STANDARD 11 )
set( CMAKE_C_STANDARD_REQUIRED ON )
set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

option( RESODOOM_BUILD_PLUGIN "Build the FFGL plugin"      ON )
option( RESODOOM_BUILD_TOOLS  "Build the verification tools" ON )

if( APPLE AND NOT CMAKE_OSX_ARCHITECTURES )
	# Universal by default. A single-arch build is a dev convenience and must
	# be asked for: -DCMAKE_OSX_ARCHITECTURES=arm64
	set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE )
endif()

set( DG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/doomgeneric/doomgeneric )

if( NOT EXISTS ${DG_DIR}/doomgeneric.c )
	message( FATAL_ERROR
		"external/doomgeneric is empty. This is a submodule:\n"
		"  git submodule update --init --recursive" )
endif()

# ---------------------------------------------------------------------------
# The engine
#
# Upstream's own source list, minus every doomgeneric_<platform>.c -- those are
# the reference ports and each one carries a main(). source/engine/EngineImpl.c
# is this project's port and replaces them.
# ---------------------------------------------------------------------------
set( DG_SOURCES
	am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c
	d_loop.c d_main.c d_mode.c d_net.c dummy.c f_finale.c f_wipe.c g_game.c
	hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_input.c i_joystick.c
	i_scale.c i_sound.c i_system.c i_timer.c i_video.c memio.c m_argv.c
	m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c
	m_random.c mus2mid.c p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c
	p_lights.c p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c
	p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c p_user.c
	r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c
	sha1.c sounds.c statdump.c st_lib.c st_stuff.c s_sound.c tables.c
	v_video.c wi_stuff.c w_checksum.c w_file.c w_file_stdc.c w_main.c w_wad.c
	z_zone.c doomgeneric.c
)
list( TRANSFORM DG_SOURCES PREPEND ${DG_DIR}/ )

foreach( src ${DG_SOURCES} )
	if( NOT EXISTS ${src} )
		message( FATAL_ERROR "doomgeneric source missing: ${src}" )
	endif()
endforeach()

# A shared library, not an archive, and that is not a packaging preference.
# doomgeneric is one pile of file-scope globals with no instance handle, so two
# layers running two games means two *loaded copies* -- the plugin dlopens a
# uniquely-named copy of this file per instance. See AGENTS.md.
add_library( resodoom_engine SHARED
	${DG_SOURCES}
	source/engine/EngineImpl.c
)

target_include_directories( resodoom_engine PUBLIC
	${DG_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/source/engine
)

target_compile_definitions( resodoom_engine PRIVATE
	NORMALUNIX
	_DEFAULT_SOURCE
	# Doom's framebuffer 1:1. doomgeneric's own integer scaler becomes a
	# straight copy, and every scaling decision belongs to the plugin's shader.
	DOOMGENERIC_RESX=320
	DOOMGENERIC_RESY=200
)

# Force-include the hook header into every translation unit, upstream's
# included. This is what keeps the submodule pristine -- see ResodoomHooks.h.
target_compile_options( resodoom_engine PRIVATE
	-include ${CMAKE_CURRENT_SOURCE_DIR}/source/engine/ResodoomHooks.h
	# Upstream is 1993 C and warns in the hundreds. Silencing it here keeps
	# this project's own warnings readable, which is the point of warnings.
	-w
)

set_target_properties( resodoom_engine PROPERTIES
	C_VISIBILITY_PRESET hidden
	OUTPUT_NAME "resodoom_engine"
)

if( APPLE )
	set_target_properties( resodoom_engine PROPERTIES
		INSTALL_NAME_DIR "@loader_path"
		BUILD_WITH_INSTALL_NAME_DIR ON
	)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if( RESODOOM_BUILD_TOOLS )
	add_executable( resotest tools/resotest/main.c )
	target_include_directories( resotest PRIVATE source/engine )
	target_link_libraries( resotest PRIVATE ${CMAKE_DL_LIBS} )
	# The harness loads the engine exactly as the plugin does, by path, so the
	# thing under test is the shipped artefact and not a relinked copy of it.
	add_dependencies( resotest resodoom_engine )
	target_compile_definitions( resotest PRIVATE
		RESODOOM_ENGINE_DEFAULT_PATH="$<TARGET_FILE:resodoom_engine>"
	)
endif()

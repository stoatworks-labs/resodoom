cmake_minimum_required( VERSION 3.20 )

project( resodoom VERSION 0.1.0 LANGUAGES C CXX )

set( CMAKE_C_STANDARD 11 )
set( CMAKE_C_STANDARD_REQUIRED ON )
set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

option( RESODOOM_BUILD_PLUGIN "Build the FFGL plugin"      ON )
option( RESODOOM_BUILD_TOOLS  "Build the verification tools" ON )

# ---------------------------------------------------------------------------
# Picture geometry
#
# Doom renders 320x200 and always has. Three separate things have to agree on
# the number -- Doom's own renderer, doomgeneric's output buffer and the
# engine's Describe() -- so it is written here once and pushed to all three.
#
# At Doom's 5:6 pixel aspect the display aspect is (W / H) * 5/6, so:
#
#     320 -> 4:3      384 -> 16:10      426 -> 16:9      568 -> 21:9
#
# **A wider buffer on its own would not be widescreen.** Doom's horizontal FOV
# is pinned at 90 degrees and its scale is derived from half the buffer width
# (`projection = centerxfrac`), so widening alone zooms in and crops the top
# and bottom. patches/0001 holds the projection at its 320-wide value, which
# turns the extra columns into extra angle: 106 degrees across at 426, the
# vertical field unchanged. patches/0002 keeps the view at size 11, the only
# size that is safe in a wider buffer. See AGENTS.md for both.
#
# patches/0004 centres the 2D layer -- title and credit pages, menus, help,
# intermission, finale -- which is 320-wide art in 320-wide coordinates:
# V_DrawPatch shifts it by (SCREENWIDTH - 320) / 2 and blacks the strips
# beside a full-screen page. Only the WIDTH is handled; a buffer taller than
# 200 would leave the 2D layer at the top.
# ---------------------------------------------------------------------------
set( RESODOOM_SCREEN_WIDTH  320 CACHE STRING "Doom's render width in pixels." )
set( RESODOOM_SCREEN_HEIGHT 200 CACHE STRING "Doom's render height in pixels." )

# columnofs[MAXWIDTH] and ylookup[MAXHEIGHT] in r_draw.c are the hard ceiling,
# and overrunning them corrupts the heap rather than failing to build.
if( RESODOOM_SCREEN_WIDTH GREATER 1120 OR RESODOOM_SCREEN_HEIGHT GREATER 832 )
	message( FATAL_ERROR
		"RESODOOM_SCREEN_WIDTH/HEIGHT exceed doomgeneric's MAXWIDTH/MAXHEIGHT "
		"(1120x832), which r_draw.c indexes without bounds checks." )
endif()

if( APPLE AND NOT CMAKE_OSX_ARCHITECTURES )
	# Universal by default. A single-arch build is a dev convenience and must
	# be asked for: -DCMAKE_OSX_ARCHITECTURES=arm64
	set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE )
endif()

set( DG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/doomgeneric/doomgeneric )

# ---------------------------------------------------------------------------
# stagehand -- the MIT half.
#
# Everything here that is not about Doom lives there: loading a private copy of
# a source library, paying out its clock, and presenting its frames. It is a
# separate repo because it is genuinely independent and useful without Doom,
# and keeping it separate is what lets it stay MIT while this repo is GPL-2.0.
# MIT links into GPL; the combined binary this repo distributes is GPL-2.0.
# ---------------------------------------------------------------------------
if( NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/stagehand/CMakeLists.txt )
	message( FATAL_ERROR
		"external/stagehand is empty. This is a submodule:\n"
		"  git submodule update --init --recursive" )
endif()
add_subdirectory( external/stagehand EXCLUDE_FROM_ALL )

if( NOT EXISTS ${DG_DIR}/doomgeneric.c )
	message( FATAL_ERROR
		"external/doomgeneric is empty. This is a submodule:\n"
		"  git submodule update --init --recursive" )
endif()

# ---------------------------------------------------------------------------
# The patched engine tree
#
# **Almost everything this project changes about upstream goes through
# ResodoomHooks.h, and that is still the rule.** Interception covers anything
# that is a NAME -- exit, the allocator, SCREENWIDTH. What it cannot reach is
# an edit inside an upstream function body, and the widescreen projection is
# exactly that: four lines in R_ExecuteSetViewSize, R_InitTextureMapping and
# R_ClearPlanes that decide whether a wider buffer means more angle or more
# magnification.
#
# So those are carried as a patch series applied to a COPY, in the build tree.
# The submodule stays byte-identical to upstream -- `git diff` in it is empty,
# which is the property the rule exists to protect -- and taking a newer
# doomgeneric is rebasing sixty lines of diff rather than resolving a merge.
#
# Adding a patch: drop it in patches/, numbered. They apply in name order with
# `git apply -p2`, so generate them with `git diff` from the submodule root.
#
# **GIT_CEILING_DIRECTORIES is load-bearing.** The build tree usually sits
# inside this repository, where `build/` is gitignored -- and `git apply`
# silently SKIPS a path its repository ignores. It prints "Skipped patch" and
# exits 1, which CMake reports as a failed build step for a patch that is
# perfectly good. Stopping git's upward search before it finds a repository at
# all makes it a plain file operation, which is all this ever wanted.
# ---------------------------------------------------------------------------
find_package( Git REQUIRED )

# CONFIGURE_DEPENDS, or a NEW patch is invisible: a plain GLOB is evaluated once
# at configure time, so a file added afterwards is not in the list, is not a
# dependency, and is silently never applied -- the build reports success and the
# change is simply absent until somebody happens to reconfigure.
file( GLOB RESODOOM_PATCHES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch )
list( SORT RESODOOM_PATCHES )

set( DG_PATCHED ${CMAKE_CURRENT_BINARY_DIR}/doomgeneric-patched )
set( DG_STAMP   ${CMAKE_CURRENT_BINARY_DIR}/doomgeneric-patched.stamp )

# The patch SET, as a file whose timestamp only moves when the set does. The
# stamp below depends on each patch's contents, which catches an edited or an
# added patch -- but a DELETED patch is simply no longer a dependency, nothing
# looks out of date, and its changes stay in the build tree indefinitely.
# file(CONFIGURE) rewrites only when the content differs, so an unchanged set
# costs nothing.
set( DG_PATCH_LIST ${CMAKE_CURRENT_BINARY_DIR}/doomgeneric-patches.txt )
string( JOIN "\n" DG_PATCH_LIST_CONTENT ${RESODOOM_PATCHES} )
file( CONFIGURE OUTPUT ${DG_PATCH_LIST} CONTENT "${DG_PATCH_LIST_CONTENT}\n" @ONLY )

# Rebuilt from scratch whenever a patch changes, because `git apply` is not
# idempotent -- re-running it over an already-patched tree fails rather than
# doing nothing, and the failure looks like a corrupt patch.
set( DG_PATCH_COMMANDS
	COMMAND ${CMAKE_COMMAND} -E rm -rf ${DG_PATCHED}
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${DG_DIR} ${DG_PATCHED}
)
foreach( patch ${RESODOOM_PATCHES} )
	list( APPEND DG_PATCH_COMMANDS
		COMMAND ${CMAKE_COMMAND} -E env
				GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}
				${GIT_EXECUTABLE} -C ${DG_PATCHED}
				apply -p2 --whitespace=nowarn ${patch} )
endforeach()

# **Run from the binary dir, not from the patched tree**, and reach into it
# with --directory. The first command deletes that tree, so a WORKING_DIRECTORY
# pointing at it leaves the shell standing in a directory that no longer
# exists, and every later step fails with a `cd` error naming the path it was
# just asked to recreate.
add_custom_command(
	OUTPUT ${DG_STAMP}
	${DG_PATCH_COMMANDS}
	COMMAND ${CMAKE_COMMAND} -E touch ${DG_STAMP}
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	DEPENDS ${RESODOOM_PATCHES} ${DG_PATCH_LIST}
	COMMENT "Patching doomgeneric into the build tree"
	VERBATIM
)
add_custom_target( doomgeneric_patched DEPENDS ${DG_STAMP} )

# Configure-time too: the source list below must name files that exist before
# the first build, or CMake refuses the target outright.
if( NOT EXISTS ${DG_PATCHED}/doomgeneric.c )
	file( REMOVE_RECURSE ${DG_PATCHED} )
	file( COPY ${DG_DIR}/ DESTINATION ${DG_PATCHED} )
	foreach( patch ${RESODOOM_PATCHES} )
		execute_process(
			COMMAND ${CMAKE_COMMAND} -E env
					GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}
					${GIT_EXECUTABLE} -C ${DG_PATCHED}
					apply -p2 --whitespace=nowarn ${patch}
			RESULT_VARIABLE patch_result
			ERROR_VARIABLE  patch_error
		)
		if( NOT patch_result EQUAL 0 )
			message( FATAL_ERROR
				"patch did not apply to this doomgeneric:\n  ${patch}\n"
				"${patch_error}\n"
				"A doomgeneric bump usually means rebasing it." )
		endif()
	endforeach()
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
# From the patched copy, never from the submodule itself.
list( TRANSFORM DG_SOURCES PREPEND ${DG_PATCHED}/ )

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

# Re-patch before compiling anything, on every build where a patch changed.
add_dependencies( resodoom_engine doomgeneric_patched )

target_include_directories( resodoom_engine PUBLIC
	# The patched copy, so a patch that touches a HEADER is seen. DG_DIR is
	# deliberately absent: with both on the path the pristine copy of a patched
	# header could win, and the symptom would be a link error naming neither.
	${DG_PATCHED}
	${CMAKE_CURRENT_SOURCE_DIR}/source/engine
	${CMAKE_CURRENT_SOURCE_DIR}/external/stagehand/include
)

target_compile_definitions( resodoom_engine PRIVATE
	NORMALUNIX
	_DEFAULT_SOURCE
	# Doom's framebuffer 1:1. doomgeneric's own integer scaler becomes a
	# straight copy, and every scaling decision belongs to the plugin's shader.
	# Both pairs come from the one place so they cannot drift apart: a
	# doomgeneric buffer narrower than Doom's renderer is a silent overrun.
	DOOMGENERIC_RESX=${RESODOOM_SCREEN_WIDTH}
	DOOMGENERIC_RESY=${RESODOOM_SCREEN_HEIGHT}
	RESODOOM_SCREEN_WIDTH=${RESODOOM_SCREEN_WIDTH}
	RESODOOM_SCREEN_HEIGHT=${RESODOOM_SCREEN_HEIGHT}
)

# Force-include the hook header into every translation unit, upstream's
# included. This is what keeps the submodule pristine -- see ResodoomHooks.h.
#
# MSVC spells both of these differently, and a build that passes the clang
# spelling to cl.exe does not fail on the flag -- it treats it as a source
# file name and fails much later, complaining about the hook functions.
if( MSVC )
	target_compile_options( resodoom_engine PRIVATE
		/FI${CMAKE_CURRENT_SOURCE_DIR}/source/engine/ResodoomHooks.h
		/w
		# <stdatomic.h> is behind a switch on MSVC and #errors without it.
		# The engine's whole cross-thread story is atomics, so this is not
		# optional -- it needs /std:c11 or later, which CMAKE_C_STANDARD gives.
		/experimental:c11atomics
	)
else()
	target_compile_options( resodoom_engine PRIVATE
		-include ${CMAKE_CURRENT_SOURCE_DIR}/source/engine/ResodoomHooks.h
		# Upstream is 1993 C and warns in the hundreds. Silencing it here keeps
		# this project's own warnings readable, which is the point of warnings.
		-w
	)
endif()

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
# The plugin
# ---------------------------------------------------------------------------
if( RESODOOM_BUILD_PLUGIN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt )
	set( FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE )
	add_subdirectory( external/ffgl EXCLUDE_FROM_ALL )

	# An OBJECT library so the GL harness links the same objects the bundle
	# does -- the harness has to drive the real class, not a copy of it.
	#
	# Note what is NOT in here: SourcePlugin.cpp. That file carries the
	# CFFGLPluginInfo registration and is listed directly in the MODULE target
	# below; in a STATIC archive the linker is entitled to drop it, giving a
	# bundle that loads, exports plugMain and reports no plugins.
	add_library( resodoom_plugin OBJECT
		source/Plugin.h
		source/Plugin.cpp
		source/Controls.h
	)
	target_include_directories( resodoom_plugin PUBLIC source )
	target_link_libraries( resodoom_plugin PUBLIC ffgl::sdk stagehand ${CMAKE_DL_LIBS} )
	set_target_properties( resodoom_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON )

	if( NOT APPLE )
		find_package( GLEW REQUIRED )
		target_link_libraries( resodoom_plugin PUBLIC GLEW::GLEW )
	endif()

	add_library( ResodoomSource MODULE source/SourcePlugin.cpp )
	# Both OBJECT libraries named explicitly: CMake does not propagate the
	# *objects* of one OBJECT library through another, so a transitive link
	# configures happily and then fails with every stagehand symbol undefined.
	target_link_libraries( ResodoomSource PRIVATE resodoom_plugin stagehand )

	if( APPLE )
		set_target_properties( ResodoomSource PROPERTIES
			BUNDLE TRUE
			BUNDLE_EXTENSION "bundle"
			OUTPUT_NAME "Resodoom"
			MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
			MACOSX_BUNDLE_BUNDLE_NAME "Resodoom"
			MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.resodoom"
			MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
		)
		# FFGL hosts resolve plugMain by symbol name, so keep default visibility.
		target_link_libraries( ResodoomSource PRIVATE
			"-framework OpenGL"
			"-framework Carbon"
			"-framework AppKit"
		)
	else()
		set_target_properties( ResodoomSource PROPERTIES OUTPUT_NAME "Resodoom" )
	endif()

	# The engine ships INSIDE the bundle. A bundle without it loads fine and
	# then refuses every WAD, so this is a build step rather than an install
	# step -- a developer build has to be complete too.
	#
	# **Contents/Frameworks, not Contents/MacOS**, and that is a signing
	# requirement rather than a matter of taste. macOS signs inside-out: every
	# nested binary must be signed before the container, and the fleet's
	# release-lib deliberately EXCLUDES Contents/MacOS/* from its nested pass
	# because in an ordinary .app that directory holds only the main executable.
	# Putting the engine there means it is never signed, and signing the bundle
	# then fails with "code object is not signed at all" naming the dylib.
	# Frameworks is where macOS expects nested libraries anyway.
	if( APPLE )
		set( RESODOOM_ENGINE_DEST "$<TARGET_BUNDLE_CONTENT_DIR:ResodoomSource>/Frameworks" )
	else()
		set( RESODOOM_ENGINE_DEST "$<TARGET_FILE_DIR:ResodoomSource>" )
	endif()

	add_custom_command( TARGET ResodoomSource POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${RESODOOM_ENGINE_DEST}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:resodoom_engine>" "${RESODOOM_ENGINE_DEST}/"
		COMMENT "Staging the engine into the bundle"
	)
	add_dependencies( ResodoomSource resodoom_engine )

	# The GL harness: the real plugin class through the real FFGL sequence in a
	# headless core-profile context. The only thing that catches a shader that
	# will not compile or a uniform whose name does not match the GLSL.
	if( RESODOOM_BUILD_TOOLS )
		add_executable( resogl tools/resogl/main.cpp )
		target_link_libraries( resogl PRIVATE resodoom_plugin stagehand )

		if( APPLE )
			target_link_libraries( resogl PRIVATE
				"-framework OpenGL"
				"-framework AppKit"
			)
		else()
			# stagehand already carries OpenGL::GL and GLEW::GLEW forward; what
			# it does not carry is WGL's window, which lives in user32/gdi32.
			find_package( OpenGL REQUIRED )
			target_link_libraries( resogl PRIVATE OpenGL::GL )
			if( WIN32 )
				target_link_libraries( resogl PRIVATE user32 gdi32 )
			endif()
		endif()

		add_dependencies( resogl resodoom_engine )
		target_compile_definitions( resogl PRIVATE
			RESODOOM_ENGINE_BUILD_PATH="$<TARGET_FILE:resodoom_engine>"
			# See the note by kEngineWidth: the Fit checks compare against the
			# picture this build actually produces.
			RESODOOM_SCREEN_WIDTH=${RESODOOM_SCREEN_WIDTH}
			RESODOOM_SCREEN_HEIGHT=${RESODOOM_SCREEN_HEIGHT}
		)
	endif()

	if( APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT )
		set( CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
			 CACHE PATH "" FORCE )
	endif()

	install( TARGETS ResodoomSource
		LIBRARY DESTINATION "."
		BUNDLE  DESTINATION "."
	)
elseif( RESODOOM_BUILD_PLUGIN )
	message( STATUS
		"FFGL SDK not present -- skipping the plugin. "
		"Run: git submodule update --init --recursive" )
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if( RESODOOM_BUILD_TOOLS )
	add_executable( resotest tools/resotest/main.c )
	target_include_directories( resotest PRIVATE
		source/engine
		external/stagehand/include
	)
	target_link_libraries( resotest PRIVATE ${CMAKE_DL_LIBS} )
	# The harness loads the engine exactly as the plugin does, by path, so the
	# thing under test is the shipped artefact and not a relinked copy of it.
	add_dependencies( resotest resodoom_engine )
	target_compile_definitions( resotest PRIVATE
		RESODOOM_ENGINE_DEFAULT_PATH="$<TARGET_FILE:resodoom_engine>"
		# The same geometry the engine was built with, so "does Describe() say
		# what this build asked for" stays a real question at any width.
		RESODOOM_SCREEN_WIDTH=${RESODOOM_SCREEN_WIDTH}
		RESODOOM_SCREEN_HEIGHT=${RESODOOM_SCREEN_HEIGHT}
	)
endif()

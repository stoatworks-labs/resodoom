#pragma once

#include "Controls.h"

#include <stagehand/Present.h>
#include <stagehand/Sidecar.h>

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
	Doom running inside Resolume, as an FFGL source.

	The loading, the clock and the presentation all belong to stagehand; what
	is left here is the part that is actually about Doom -- which WAD, which
	level, which keys, and how it is offered to an operator in an inspector.

	## What this is shaped by

	**FFGL plugins never see a key.** There is no keyboard path in the API at
	all, which is why the twelve controls are boolean parameters: that is the
	only route input can take, and it happens to be the route that also gets
	MIDI mapping, keyboard mapping and timeline automation for free. With
	nothing mapped, Doom plays its own attract demos forever, which is the
	sensible default for a layer nobody is holding a controller for.

	**A crash here takes Resolume with it, and that is why the engine's exit()
	is intercepted rather than trusted.** A malformed WAD would otherwise close
	the host mid-show. What survives is the layer going black with the reason
	in the log; what does not survive is undefined behaviour inside Doom, and
	nothing here can promise otherwise. See AGENTS.md.

	**Nearest filtering is the default and it matters.** 320x200 scaled to a 4K
	output with linear filtering is a blurred mess, and large hard-edged pixels
	are the entire reason this content looks like itself.
*/
namespace resodoom
{

class ResodoomPlugin : public CFFGLPlugin
{
public:
	ResodoomPlugin();
	~ResodoomPlugin() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult DeInitGL() override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float    GetFloatParameter( unsigned int index ) override;

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char*    GetTextParameter( unsigned int index ) override;

private:
	/// Tear down and reload the engine. Render thread only.
	void ApplyPendingLoad();

	/// Push every changed button through as a key edge.
	void SendInput();

	/// Build the presenter and the staging buffer for a picture of this size,
	/// or confirm they are already that size. Render thread only.
	bool EnsurePicture( uint32_t width, uint32_t height, size_t frameBytes );

	/// Absolute path to this bundle's engine for a picture `width` pixels
	/// wide, or empty if that engine is not here.
	static std::string EngineLibraryPath( uint32_t width );

	/// The engine the Aspect parameter asks for, against the current canvas.
	uint32_t ChosenEngineWidth() const;

	stagehand::Sidecar   mEngine;
	stagehand::Presenter mPresenter;

	// Paths as the host gave them, against what is actually loaded. Resolume
	// re-sends the same value on composition load and on undo, and reloading
	// Doom every time it did that would restart the game under the operator.
	std::string mIwad;
	std::string mPwad;
	std::string mLoadedIwad;
	std::string mLoadedPwad;

	bool mPendingLoad = false;

	/// Set when a load failed, cleared when the paths change. Stops the render
	/// thread rebuilding a broken engine sixty times a second -- and, worse,
	/// staging sixty copies of the library into the temp directory.
	bool mLoadFailed = false;

	float mParams[ PT_COUNT ] = { 0.0f };

	/// Last state pushed to the engine, so only edges are sent. Doom treats a
	/// repeated keydown as a fresh press, which in a menu means every held
	/// button scrolls at the composition's frame rate.
	bool mButtonWasDown[ PT_COUNT ] = { false };

	/// One frame's staging, sized to whatever the loaded engine publishes. Too
	/// big for the render thread's stack and too big to allocate per frame.
	std::vector< uint8_t > mFrame;

	/*
		What the presenter and mFrame are currently built for.

		The engine decides this, not the plugin -- an engine compiled for a
		widescreen buffer says so through Describe() and everything here
		follows. Zero until InitGL has run.
	*/
	uint32_t mPictureWidth  = 0;
	uint32_t mPictureHeight = 0;

	/*
		What ChosenEngineWidth() said when the running engine was loaded, or 0
		for none. Compared against a fresh answer to decide whether an Aspect
		change or a new canvas needs a different engine.

		The CHOICE, not the width that ended up loaded: if the 426 engine is
		missing and the 320 one stood in, asking again still says 426 -- and
		comparing against 320 would reload on every re-sent parameter, falling
		back identically each time.
	*/
	uint32_t mEngineChoice = 0;

	FFGLViewportStruct mViewport { 0, 0, 0, 0 };
};

} // namespace resodoom

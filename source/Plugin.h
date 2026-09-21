#pragma once

#include "Controls.h"
#include "Engine.h"

#include <FFGLSDK.h>

#include <memory>
#include <string>

/**
	Doom running inside Resolume, as an FFGL source.

	The engine thread publishes a frame, `ProcessOpenGL` uploads it and draws
	it, and the picture is on screen the same composition frame it was made.

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

	**`ffglex::Scoped*` bindings clear to 0 on scope exit rather than restoring
	what was there**, so the render path uses plain `glUseProgram` and
	`glBindTexture` and puts state back by hand.

	**No FBO is allocated anywhere.** `FFGLFBO::Initialise` allocates under a
	`ScopedTextureBinding` whose destructor clears the binding, and
	`FFGLFBO::Release` leaks its colour texture. A source draws a textured quad
	and needs neither.

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
	bool BuildShader();

	/// Tear down and reload the engine. Render thread only.
	void ApplyPendingLoad();

	/// Push every changed button through to the engine as a key edge.
	void SendInput();

	/// Upload the newest frame if there is one. False if there is nothing to draw.
	bool UpdateTexture();

	void ComputeQuadScale( int vpWidth, int vpHeight, float& sx, float& sy ) const;

	Engine mEngine;

	// Paths as the host gave them, against what is actually loaded. Resolume
	// re-sends the same value on composition load and on undo, and reloading
	// Doom every time it did that would restart the game under the operator.
	std::string mIwad;
	std::string mPwad;
	std::string mLoadedIwad;
	std::string mLoadedPwad;

	bool mPendingLoad = false;

	/// Set when a load failed, cleared when the paths change. Stops the render
	/// thread rebuilding a broken engine sixty times a second and filling the
	/// log -- and, worse, staging sixty copies of the library into /tmp.
	bool mLoadFailed = false;

	float mParams[ PT_COUNT ] = { 0.0f };

	/// Last state pushed to the engine, so only edges are sent. Doom treats a
	/// repeated keydown as a fresh press, which in a menu means every held
	/// button scrolls at the composition's frame rate.
	bool mButtonWasDown[ PT_COUNT ] = { false };

	std::unique_ptr< ResodoomFrame > mFrame;

	ffglex::FFGLShader mShader;
	GLuint             mVAO     = 0;
	GLuint             mTexture = 0;
	bool               mTextureAllocated = false;
	uint32_t           mUploadedSeq      = 0;

	FFGLViewportStruct mViewport { 0, 0, 0, 0 };
};

} // namespace resodoom

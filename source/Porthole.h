#pragma once

#include <FFGLSDK.h>

#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    Porthole -- a lens projection warp for Resolume.

    The effect re-photographs the incoming picture through a different lens.
    Every output pixel stands for a ray at some angle off the optical axis; the
    lens model says where a ray at that angle lands on the image; the picture is
    resampled accordingly. That is the entire mechanism.

    It is worth being clear about what this is *not*: there is no bulge
    function, no radial displacement curve, no polynomial fitted to look about
    right. The single Projection control is the exponent of a one-parameter
    family that contains every named lens projection there is (see
    Projection.h), so each position on that slider is a real optic rather than a
    point on a fade between two looks. Several useful properties follow from
    that on their own rather than having to be arranged:

    - **Rectilinear does nothing.** At the top of the Projection range the map
      is the identity for any field of view, because re-rendering a flat
      picture through a flat lens cannot change it.
    - **Defish is an exact inverse, not an approximation.** The same formula
      run the other way, so a fish followed by a defish at matched settings
      returns the original picture to within resampling error.
      `phtest --roundtrip` measures that.
    - **The frame stays full.** The reference radius is a fixed point of the
      map, so the edge of the picture always lands on the edge of the picture
      and only the interior is redistributed. Strength changes the character of
      the warp rather than shrinking the image into a black field.

    There is no wet/dry mix, deliberately. Cross-fading two different geometries
    double-exposes the picture rather than easing between them; the honest null
    for the warp is Field of View at zero, where every projection agrees.

    Chromatic aberration is not covered by that null, and should not be. Lateral
    chromatic aberration is a change of magnification with wavelength, which a
    lens can have whether or not its distortion is zero -- so it keeps its own
    control and its own zero rather than being scaled by how much the picture is
    being bent. Setting the whole effect out of the way means Field of View and
    Chromatic both at zero.

    See AGENTS.md for the traps.
*/
class Porthole : public CFFGLPlugin
{
public:
	Porthole();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Test hook: the parameter ids a preset covers, in presets::Param
	/// order. Handed out rather than copied into the harness, so a second
	/// list cannot go quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );
	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

private:
	/// The order the host shows them in: what the lens is, how the frame is
	/// fitted to it, and what to do with the pixels the lens cannot supply.
	enum ParamID : FFUInt32
	{
		//Lens
		PT_PROJECTION,
		PT_FIELD_OF_VIEW,
		PT_DEFISH,
		PT_CHROMATIC,

		//Frame
		PT_FIT,
		PT_CENTRE_X,
		PT_CENTRE_Y,
		PT_ZOOM,

		//Output
		PT_EDGES,
		PT_QUALITY,

		//Preset. Declared after the real controls so their IDs — which a saved
		//composition refers to — do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ porthole::presets::kParamCount ] = {
		PT_PROJECTION, PT_FIELD_OF_VIEW, PT_DEFISH, PT_CHROMATIC,
		PT_FIT, PT_ZOOM, PT_EDGES
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes in
	/// rather than the operator moving anything -- in which case it must not
	/// reach params[] and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	void applyPreset( int presetIndex );

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It pushes its own values back down
	/// whenever it likes, and nothing obliges it to act on the value events
	/// applyPreset raises -- Resolume does not. So a preset that writes params[]
	/// and trusts the host to follow is relying on behaviour the specification
	/// never promised, and when the host instead restates the values it still
	/// believes in, the rule that a covered parameter changing means the
	/// operator has taken over fires on the host's own echo and drops straight
	/// back to Custom. Reported against vertigo as its issue #2; the same
	/// pattern had been copied into all seven plugins.
	///
	/// Keeping the host's own last word separately is what tells the two apart.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};

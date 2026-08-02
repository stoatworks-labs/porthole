#pragma once

#include <FFGLSDK.h>

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

		PT_COUNT
	};

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	float params[ PT_COUNT ];
};

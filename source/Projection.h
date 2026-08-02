#pragma once

/**
    The lens projection family, in C++.

    A camera lens is defined by one function: how far from the centre of the
    image a ray arriving at angle theta off the axis lands. Every named
    projection is a different answer to that, and they are all members of a
    single one-parameter family:

        P_k(theta) =  tan( k*theta ) / k      k > 0
                      theta                   k = 0
                      sin( k*theta ) / k      k < 0

    which is continuous and smooth through k = 0, and lands exactly on the five
    projections anyone has a name for:

        k = +1.0   Rectilinear    r = tan(theta)        an ordinary lens
        k = +0.5   Stereographic  r = 2 tan(theta/2)    "little planet", conformal
        k =  0.0   Equidistant    r = theta             the classic fisheye
        k = -0.5   Equisolid      r = 2 sin(theta/2)    most real fisheyes
        k = -1.0   Orthographic   r = sin(theta)        the "mirror ball"

    That is the whole basis of this plugin. Nothing here draws a bulge; the
    picture bulges because it is being re-rendered through a lens whose radial
    mapping is not the one it was shot with. Which is also why k = +1 does
    nothing at all no matter how the other controls are set -- a rectilinear
    picture re-rendered through a rectilinear lens is the same picture, and the
    maths says so without being told.

    This file is the canonical statement of it. `Shaders.cpp` carries a GLSL
    mirror, because the work has to happen per-pixel on the GPU, and
    `tools/phtest --probe` measures the GPU against these functions to catch the
    two copies drifting apart. Change one, change the other, then run the probe.
*/
namespace porthole
{

/// Where a ray at `theta` radians off-axis lands, in focal lengths.
/// Monotonic in theta over the range any of these lenses can see.
double project( double theta, double k );

/// Inverse of project(): the angle whose image radius is `r`.
/// Orthographic and its neighbours cannot see past 90 degrees, so `r` is
/// clamped into the invertible range rather than returning a NaN.
double unproject( double r, double k );

/// The radial map the effect applies, in units where 1.0 is the reference
/// radius (see referenceRadius()).
///
/// `rho` is the output radius, and the return is the source radius to sample.
/// At rho = 1 the return is always 1: the edge of the frame stays the edge of
/// the frame, and it is the *interior* that gets redistributed. That is what
/// keeps the picture full at any strength instead of collapsing to a disc in
/// the middle of a black field.
///
/// With `defish` false the output is the lens image and the source is treated
/// as rectilinear; with it true the roles swap. The two are exact inverses of
/// each other, which `phtest --roundtrip` checks.
double warpRadius( double rho, double k, double thetaMax, bool defish );

/// How the reference radius is fitted to a frame of the given aspect
/// (width / height). Matches the Fit parameter's element order.
enum class Fit
{
	Diagonal = 0,//!< The circle circumscribes the frame. The photographic convention.
	Width    = 1,//!< Touches the left and right edges.
	Height   = 2,//!< Touches the top and bottom edges. The full-frame fisheye look.
	Stretch  = 3 //!< No aspect correction: an ellipse that touches all four edges.
};

/// The reference radius in "height units" -- the space in which x has been
/// multiplied by the aspect ratio so that the warp is circular in pixels
/// rather than circular in UV. Stretch is the exception and works directly in
/// UV, so it reports 0.5 and the caller skips the aspect correction.
double referenceRadius( Fit fit, double aspect );

/// True when this fit works in unscaled UV space rather than in height units.
bool fitIsStretched( Fit fit );

//---------------------------------------------------------------------------
// Parameter mapping.
//
// Every parameter this plugin exposes to the host is a plain 0..1 float, and
// these turn them into the physical quantities above. That is not laziness:
// CFFGLPluginManager::SetParamInfo clamps an FF_TYPE_STANDARD default into
// 0..1 *before* SetParamRange can widen the range (SDK b1afaf9), so a
// parameter declared in degrees cannot state a default in degrees. Keeping the
// host side unitless sidesteps it, and the conversion lives here where the
// harness can print both.
//---------------------------------------------------------------------------

/// Projection slider -> k. 0 is rectilinear, 0.5 equidistant, 1 orthographic,
/// so the slider walks from "no distortion" to "most distortion".
double curveFromParam( float value );

/// Field of View slider -> thetaMax in radians, the half-angle at the
/// reference radius. Capped just short of 90 degrees because the rectilinear
/// side of the map is tan(thetaMax), which is unbounded at exactly 90.
double thetaMaxFromParam( float value );

/// Zoom slider -> a linear scale factor, 0.5 being 1:1. Geometric, so equal
/// slider movements either side of centre are reciprocal magnifications.
double zoomFromParam( float value );

/// Chromatic Aberration slider -> the fraction of the reference radius that
/// red and blue are displaced by at the frame edge.
double chromaticFromParam( float value );

/// Quality option -> the grid order n, giving n*n samples per output pixel.
int tapsFromParam( float value );

} // namespace porthole

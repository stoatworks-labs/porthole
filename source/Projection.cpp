#include "Projection.h"

#include <algorithm>
#include <cmath>

namespace porthole
{
namespace
{
/// Below this the k > 0 and k < 0 branches and the k = 0 limit agree to well
/// past float precision, so the branch is only about avoiding 0/0.
constexpr double kFlat = 1e-6;

/// The rectilinear side of the map is tan(thetaMax), so thetaMax has to stay
/// short of 90 degrees. 89 leaves tan at 57, which is already a stronger warp
/// than anything anybody will use.
constexpr double kMaxTheta = 89.0 * 3.14159265358979323846 / 180.0;

/// A source radius far enough outside the frame that every edge mode does the
/// right thing with it, and finite so it can never become a NaN in a UV.
constexpr double kFarAway = 64.0;
} // namespace

double project( double theta, double k )
{
	if( k > kFlat )
		return std::tan( k * theta ) / k;
	if( k < -kFlat )
		return std::sin( k * theta ) / k;
	return theta;
}

double unproject( double r, double k )
{
	if( k > kFlat )
		return std::atan( k * r ) / k;
	if( k < -kFlat )
		return std::asin( std::clamp( k * r, -1.0, 1.0 ) ) / k;
	return r;
}

double warpRadius( double rho, double k, double thetaMax, bool defish )
{
	// At no field of view every projection agrees, which is the identity. Bail
	// before dividing by tan(0).
	if( thetaMax < 1e-5 )
		return rho;

	const double tanMax = std::tan( thetaMax );
	double result;

	if( !defish )
	{
		// The output is the lens image. Walk out to `rho` along the lens's own
		// radius, find the angle that lands there, and ask where a rectilinear
		// camera would have put the same ray.
		const double theta = unproject( rho * project( thetaMax, k ), k );
		result             = std::tan( theta ) / tanMax;
	}
	else
	{
		// The other direction: the output is rectilinear and the source is the
		// lens image. Undoing a fisheye rather than applying one.
		const double theta = std::atan( rho * tanMax );
		result             = project( theta, k ) / project( thetaMax, k );
	}

	// Past 90 degrees the near-orthographic lenses genuinely cannot see, and
	// unproject() clamps rather than returning a NaN -- so theta saturates and
	// tan(theta) runs away. Those pixels have no source, which is true, and the
	// edge mode is what decides what to show there.
	if( !( result < kFarAway ) )//written to catch NaN as well as overflow
		return kFarAway;

	return result;
}

double referenceRadius( Fit fit, double aspect )
{
	switch( fit )
	{
	case Fit::Width:
		return aspect * 0.5;
	case Fit::Height:
		return 0.5;
	case Fit::Stretch:
		return 0.5;
	case Fit::Diagonal:
	default:
		return 0.5 * std::sqrt( aspect * aspect + 1.0 );
	}
}

bool fitIsStretched( Fit fit )
{
	return fit == Fit::Stretch;
}

double curveFromParam( float value )
{
	return 1.0 - 2.0 * static_cast< double >( value );
}

double thetaMaxFromParam( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 ) * kMaxTheta;
}

double zoomFromParam( float value )
{
	// Two stops either side of unity. Geometric so that 0.25 and 0.75 are
	// exact reciprocals of each other.
	return std::pow( 2.0, ( static_cast< double >( value ) - 0.5 ) * 4.0 );
}

double chromaticFromParam( float value )
{
	// 3% of the reference radius at full travel. Lateral chromatic aberration
	// is a subtle thing on a real lens; more than this reads as a separate
	// "RGB split" effect rather than as glass.
	return static_cast< double >( value ) * 0.03;
}

int tapsFromParam( float value )
{
	const int option = static_cast< int >( std::lround( value ) );
	switch( option )
	{
	case 0:
		return 1;//Fast: one sample, aliases where the warp minifies hard
	case 2:
		return 4;//Best: 16 samples
	case 1:
	default:
		return 2;//Good: 4 samples
	}
}

} // namespace porthole

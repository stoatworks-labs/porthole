/// Headless test of the FxPlug render path.
///
/// `renderDestinationImage:` can only be called by Final Cut Pro or Motion, so
/// the part of it that can go wrong on its own was lifted into PortholeTile.h.
/// This drives that directly — no SDK, no host.
///
/// A warp needs different tests from a per-pixel effect. The properties worth
/// asserting are the ones AGENTS.md says are the model rather than decoration:
/// rectilinear is the identity, the frame stays full, and defish is the exact
/// inverse. Break one of those and the plugin is wrong even if it looks fine.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../PortholeTile.h"

namespace
{
int failures = 0;

void check( bool ok, const std::string& what )
{
	if( !ok )
	{
		std::printf( "  FAIL  %s\n", what.c_str() );
		++failures;
	}
	else
		std::printf( "  ok    %s\n", what.c_str() );
}

const int kW = 64;
const int kH = 48;

/// A source picture with structure in both axes, so a warp that is subtly wrong
/// in one of them still shows. Deliberately not a grey ramp.
std::vector<uint8_t> makeSource()
{
	std::vector<uint8_t> px( size_t( kW ) * kH * 4 );
	for( int y = 0; y < kH; ++y )
		for( int x = 0; x < kW; ++x )
		{
			uint8_t* p = px.data() + ( size_t( y ) * kW + x ) * 4;
			p[ 0 ] = uint8_t( x * 255 / ( kW - 1 ) );
			p[ 1 ] = uint8_t( y * 255 / ( kH - 1 ) );
			p[ 2 ] = uint8_t( ( ( x / 8 + y / 8 ) % 2 ) ? 220 : 30 );// checker
			p[ 3 ] = 255;
		}
	return px;
}

porthole::WarpTileState defaultState()
{
	porthole::WarpTileState s;
	s.curve     = porthole::curveFromParam( 0.0f );// rectilinear
	s.thetaMax  = porthole::thetaMaxFromParam( 0.5f );
	s.chromatic = 0.0;
	s.centreX   = 0.0;
	s.centreY   = 0.0;
	s.zoom      = porthole::zoomFromParam( 0.5f );
	s.fit       = int( porthole::Fit::Diagonal );
	s.edges     = int( porthole::EdgeMode::Transparent );
	s.taps      = 1;
	s.defish    = 0;
	return s;
}

std::vector<uint8_t> render( const std::vector<uint8_t>& src, const porthole::WarpTileState& state )
{
	std::vector<uint8_t> dst( size_t( kW ) * kH * 4, 0 );
	const porthole::SourceImage source( src.data(), size_t( kW ) * 4,
										fxsurface::Layout::RGBA8, kW, kH );
	porthole::warpTile( source, dst.data(), size_t( kW ) * 4, fxsurface::Layout::RGBA8,
						0, 0, kW, kH, kW, kH, 1.0, state );
	return dst;
}

int maxChannelDiff( const std::vector<uint8_t>& a, const std::vector<uint8_t>& b )
{
	int worst = 0;
	for( size_t i = 0; i < a.size(); ++i )
		worst = std::max( worst, std::abs( int( a[ i ] ) - int( b[ i ] ) ) );
	return worst;
}

// ------------------------------------------------------------------- identity

/// The claim AGENTS.md makes first: a flat picture re-photographed through a
/// flat lens is the same picture, at ANY field of view.
void testRectilinearIsIdentity()
{
	std::printf( "rectilinear is the identity\n" );

	const std::vector<uint8_t> src = makeSource();

	for( float fov : { 0.0f, 0.25f, 0.5f, 0.9f } )
	{
		porthole::WarpTileState s = defaultState();
		s.curve    = porthole::curveFromParam( 0.0f );// k = +1, rectilinear
		s.thetaMax = porthole::thetaMaxFromParam( fov );

		const std::vector<uint8_t> out = render( src, s );

		// One bilinear tap of an unmoved coordinate: allow a rounding step, not
		// a visible shift.
		check( maxChannelDiff( src, out ) <= 1,
			   "field of view " + std::to_string( fov ) + " leaves the picture alone" );
	}
}

/// Field of View at zero is the null: every projection agrees there.
void testZeroFieldOfViewIsNull()
{
	std::printf( "zero field of view is the null\n" );

	const std::vector<uint8_t> src = makeSource();

	for( float projection : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f } )
	{
		porthole::WarpTileState s = defaultState();
		s.curve    = porthole::curveFromParam( projection );
		s.thetaMax = porthole::thetaMaxFromParam( 0.0f );

		check( maxChannelDiff( src, render( src, s ) ) <= 1,
			   "projection " + std::to_string( projection ) + " does nothing at FoV 0" );
	}
}

// ---------------------------------------------------------------- frame stays full

/// warpRadius(1) == 1 always, so the picture is redistributed inside its own
/// bounds instead of shrinking into a black field. The failure this catches is
/// a warp that collapses the image to a disc — which looks plausible in a
/// thumbnail and is wrong.
void testFrameStaysFull()
{
	std::printf( "the frame stays full\n" );

	const std::vector<uint8_t> src = makeSource();

	for( float projection : { 0.25f, 0.5f, 0.75f, 1.0f } )
	{
		porthole::WarpTileState s = defaultState();
		s.curve    = porthole::curveFromParam( projection );
		s.thetaMax = porthole::thetaMaxFromParam( 0.8f );

		const std::vector<uint8_t> out = render( src, s );

		// Corners are the hardest place to stay opaque, and the first thing to
		// go transparent if the reference radius is wrong.
		const auto alphaAt = [ &out ]( int x, int y ) {
			return out[ ( size_t( y ) * kW + x ) * 4 + 3 ];
		};
		const bool cornersOpaque = alphaAt( 0, 0 ) > 0 && alphaAt( kW - 1, 0 ) > 0 &&
								   alphaAt( 0, kH - 1 ) > 0 && alphaAt( kW - 1, kH - 1 ) > 0;
		check( cornersOpaque,
			   "projection " + std::to_string( projection ) + " keeps the corners filled" );
	}
}

// --------------------------------------------------------------------- defish

/// Defish is the exact inverse, not a similar-looking curve — the same formula
/// with source and destination swapped. Warping then unwarping must land back
/// where it started.
void testDefishIsTheInverse()
{
	std::printf( "defish is the inverse\n" );

	const std::vector<uint8_t> src = makeSource();

	porthole::WarpTileState fish = defaultState();
	fish.curve    = porthole::curveFromParam( 0.5f );// equidistant
	fish.thetaMax = porthole::thetaMaxFromParam( 0.5f );
	fish.edges    = int( porthole::EdgeMode::Clamp );
	fish.taps     = 2;

	porthole::WarpTileState unfish = fish;
	unfish.defish = 1;

	const std::vector<uint8_t> there = render( src, fish );
	const std::vector<uint8_t> back  = render( there, unfish );

	// Two resamplings of an 8-bit picture, so this is a "recognisably the same
	// picture" bound, not equality. A wrong inverse is off by far more.
	long total = 0;
	int counted = 0;
	for( int y = kH / 4; y < 3 * kH / 4; ++y )
		for( int x = kW / 4; x < 3 * kW / 4; ++x )
			for( int c = 0; c < 3; ++c )
			{
				const size_t i = ( size_t( y ) * kW + x ) * 4 + c;
				total += std::abs( int( src[ i ] ) - int( back[ i ] ) );
				++counted;
			}
	const double meanError = double( total ) / counted;
	std::printf( "        mean round-trip error %.2f/255 over the centre half\n", meanError );
	check( meanError < 12.0, "warp then defish returns the original picture" );
}

// ---------------------------------------------------------------- tile placement

/// The FxPlug-specific one. The warp is defined over the whole image, so a
/// destination tile must be placed by its position in that image. Rendering in
/// strips and rendering whole must agree — if they do not, the picture tears at
/// tile boundaries in the host and nowhere else.
void testTiledMatchesWhole()
{
	std::printf( "tiled render matches whole render\n" );

	const std::vector<uint8_t> src = makeSource();

	porthole::WarpTileState s = defaultState();
	s.curve    = porthole::curveFromParam( 0.75f );
	s.thetaMax = porthole::thetaMaxFromParam( 0.7f );
	s.edges    = int( porthole::EdgeMode::Clamp );

	const std::vector<uint8_t> whole = render( src, s );

	// Same picture, rendered as four horizontal strips.
	std::vector<uint8_t> strips( size_t( kW ) * kH * 4, 0 );
	const porthole::SourceImage source( src.data(), size_t( kW ) * 4,
										fxsurface::Layout::RGBA8, kW, kH );
	const int stripH = kH / 4;
	for( int strip = 0; strip < 4; ++strip )
	{
		const int y0 = strip * stripH;
		porthole::warpTile( source,
							strips.data() + size_t( y0 ) * kW * 4, size_t( kW ) * 4,
							fxsurface::Layout::RGBA8,
							0, y0, kW, stripH, kW, kH, 1.0, s );
	}

	check( maxChannelDiff( whole, strips ) == 0,
		   "four strips are byte-identical to one whole render" );
}

// ------------------------------------------------------------------ edge modes

void testEdgeModes()
{
	std::printf( "edge modes\n" );

	const std::vector<uint8_t> src = makeSource();

	// Reaching outside the frame is fussier than it looks, and both obvious
	// ways of arranging it are wrong:
	//
	//   - Zoom must be BELOW 0.5. zoomFromParam is 2^((v-0.5)*4) and the source
	//     radius is DIVIDED by it, so v > 0.5 magnifies and samples further in.
	//   - A strong projection does not help, it hinders. Orthographic at a wide
	//     field of view is so compressive that output radius 0.98 maps to source
	//     radius 0.09 — the interior collapses toward the centre and only the
	//     very rim reaches the edge, so nothing lands outside at all.
	//
	// Rectilinear is the clean case: its radial map is the identity, so zooming
	// out moves the whole picture outward by the zoom factor and the edge mode
	// is what decides what fills the border.
	porthole::WarpTileState transparent = defaultState();
	transparent.curve    = porthole::curveFromParam( 0.0f );// rectilinear
	transparent.thetaMax = porthole::thetaMaxFromParam( 0.5f );
	transparent.zoom     = porthole::zoomFromParam( 0.1f ); // about a third
	transparent.edges    = int( porthole::EdgeMode::Transparent );

	porthole::WarpTileState black = transparent;
	black.edges = int( porthole::EdgeMode::Black );

	const std::vector<uint8_t> t = render( src, transparent );
	const std::vector<uint8_t> b = render( src, black );

	bool anyTransparent = false;
	for( size_t i = 3; i < t.size(); i += 4 )
		if( t[ i ] < 255 )
			anyTransparent = true;

	bool allOpaque = true;
	for( size_t i = 3; i < b.size(); i += 4 )
		if( b[ i ] < 255 )
			allOpaque = false;

	check( anyTransparent, "Transparent edges let the outside through as alpha" );
	check( allOpaque, "Black edges stay fully opaque" );
}

} // namespace

int main()
{
	std::printf( "Porthole FxPlug tile render tests\n\n" );

	testRectilinearIsIdentity();
	testZeroFieldOfViewIsNull();
	testFrameStaysFull();
	testDefishIsTheInverse();
	testTiledMatchesWhole();
	testEdgeModes();

	std::printf( "\n%s\n", failures == 0 ? "all passed" : "FAILURES PRESENT" );
	return failures == 0 ? 0 : 1;
}

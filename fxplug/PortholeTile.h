#pragma once

/// The warp, over one destination tile, with no FxPlug in it.
///
/// Same split as the Luma Key port: `renderDestinationImage:` can only be
/// called by a host, so everything that can go wrong on its own lives out here
/// where a test can drive it. `FxSurface.h` handles pixel layouts and knows
/// nothing about lenses; `../source/Projection.h` is the lens family itself,
/// shared with the FFGL and OpenFX builds.
///
/// This is a straight transcription of the OpenFX processor
/// (`../source/ofx/PortholeOFX.cpp`) onto FxPlug's surfaces. Where the two
/// differ, the OpenFX one is right and this is the bug — they must sample
/// identically.
///
/// The one thing that is genuinely different from Luma Key: a warp reads from
/// anywhere in the source, so the plug-in declares
/// `kFxPropertyKey_NeedsFullBuffer` and asks for the whole source image rather
/// than the matching tile. The destination may still arrive as a sub-rect, so
/// every output pixel is placed using its position in the FULL image, not its
/// position in the tile.

#include <cmath>

#include "FxSurface.h"

#include "Projection.h"

namespace porthole
{

using fxsurface::Layout;

/// Mirrors the OpenFX build's EdgeMode, and the Edges parameter's option order.
enum class EdgeMode
{
	Transparent = 0,
	Black       = 1,
	Clamp       = 2,
	Mirror      = 3,
	Wrap        = 4
};

/// Everything the warp needs, in the physical units Projection.h works in —
/// the same struct the OpenFX build fills, widened to fixed-width fields
/// because this crosses to the render threads as the raw bytes of the FxPlug
/// pluginState blob.
struct WarpTileState
{
	double curve;    //!< k
	double thetaMax;
	double chromatic;
	double centreX;  //!< offset of the optical axis from frame centre
	double centreY;
	double zoom;
	int32_t fit;     //!< porthole::Fit
	int32_t edges;   //!< EdgeMode
	int32_t taps;    //!< grid order n, n*n samples per output pixel
	uint32_t defish; //!< not bool: fixed width, no padding surprises
};

/// GLSL mod(): x - y*floor(x/y), correct for negatives, which std::fmod isn't.
inline double glslMod( double x, double y )
{
	return x - y * std::floor( x / y );
}

inline double mirrorCoord( double x )
{
	const double m = glslMod( x, 2.0 );
	return ( m > 1.0 ) ? ( 2.0 - m ) : m;
}

/// A source image addressed in picture space, premultiplied RGBA out.
class SourceImage
{
public:
	SourceImage( const uint8_t* base, size_t stride, Layout layout, int width, int height ) :
		_base( base ), _stride( stride ), _layout( layout ),
		_bpp( fxsurface::bytesPerPixel( layout ) ), _width( width ), _height( height )
	{
	}

	int width() const { return _width; }
	int height() const { return _height; }

	/// One texel, premultiplied RGBA in 0..1. FxPlug images are already
	/// premultiplied, so unlike the OpenFX build there is nothing to do here
	/// beyond the read.
	void texel( int x, int y, double out[ 4 ] ) const
	{
		if( x < 0 || y < 0 || x >= _width || y >= _height )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}
		float rgba[ 4 ];
		fxsurface::readPixel( _base + size_t( y ) * _stride + size_t( x ) * _bpp, _layout, rgba );
		for( int c = 0; c < 4; ++c )
			out[ c ] = rgba[ c ];
	}

	/// Fetch at picture-space p. Mirrors the shader: edge mode first, then
	/// never sample nearer than half a texel to the picture edge, then a
	/// bilinear tap.
	void fetch( double px, double py, EdgeMode edges, double out[ 4 ] ) const
	{
		const bool outside = px < 0.0 || py < 0.0 || px > 1.0 || py > 1.0;

		switch( edges )
		{
		case EdgeMode::Transparent:
			if( outside )
			{
				out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
				return;
			}
			break;
		case EdgeMode::Black:
			if( outside )
			{
				out[ 0 ] = out[ 1 ] = out[ 2 ] = 0.0;
				out[ 3 ]                       = 1.0;
				return;
			}
			break;
		case EdgeMode::Clamp:
			px = std::clamp( px, 0.0, 1.0 );
			py = std::clamp( py, 0.0, 1.0 );
			break;
		case EdgeMode::Mirror:
			px = mirrorCoord( px );
			py = mirrorCoord( py );
			break;
		case EdgeMode::Wrap:
			px = glslMod( px, 1.0 );
			py = glslMod( py, 1.0 );
			break;
		}

		double fx = px * _width - 0.5;
		double fy = py * _height - 0.5;
		fx        = std::clamp( fx, 0.0, double( _width - 1 ) );
		fy        = std::clamp( fy, 0.0, double( _height - 1 ) );

		const int x0    = int( fx );
		const int y0    = int( fy );
		const int x1    = std::min( x0 + 1, _width - 1 );
		const int y1    = std::min( y0 + 1, _height - 1 );
		const double tx = fx - x0;
		const double ty = fy - y0;

		double p00[ 4 ], p10[ 4 ], p01[ 4 ], p11[ 4 ];
		texel( x0, y0, p00 );
		texel( x1, y0, p10 );
		texel( x0, y1, p01 );
		texel( x1, y1, p11 );

		for( int c = 0; c < 4; ++c )
		{
			const double top    = p00[ c ] + ( p10[ c ] - p00[ c ] ) * tx;
			const double bottom = p01[ c ] + ( p11[ c ] - p01[ c ] ) * tx;
			out[ c ]            = top + ( bottom - top ) * ty;
		}
	}

private:
	const uint8_t* _base;
	size_t _stride;
	Layout _layout;
	size_t _bpp;
	int _width;
	int _height;
};

/// The warp for one output pixel, in picture space. Split out of the loop so a
/// test can ask about a single coordinate.
class Warp
{
public:
	Warp( const WarpTileState& state, double aspect ) :
		_s( state ), _aspect( aspect ),
		_refRadius( referenceRadius( Fit( state.fit ), aspect ) ),
		_stretched( fitIsStretched( Fit( state.fit ) ) )
	{
	}

	/// One supersample: decompose about the optical axis, warp the radius
	/// through the lens family, recompose, fetch (three times when the
	/// chromatic control is up). Accumulates premultiplied RGBA into `sum`.
	void sample( const SourceImage& src, double px, double py, double sum[ 4 ] ) const
	{
		double cx = px - ( 0.5 + _s.centreX );
		double cy = py - ( 0.5 + _s.centreY );
		if( !_stretched )
			cx *= _aspect;

		const double len  = std::sqrt( cx * cx + cy * cy );
		const double rho  = len / _refRadius;
		const double dirX = ( len > 1e-8 ) ? cx / len : 0.0;
		const double dirY = ( len > 1e-8 ) ? cy / len : 0.0;

		const double base = warpRadius( rho, _s.curve, _s.thetaMax, _s.defish != 0 );

		const EdgeMode edges = EdgeMode( _s.edges );

		if( _s.chromatic > 0.0 )
		{
			// The same lens at three focal lengths; grows with the square of
			// the radius the way the real thing does.
			const double spread = _s.chromatic * rho * rho;

			double green[ 4 ], red[ 4 ], blue[ 4 ];
			src.fetch( recomposeX( dirX, base ), recomposeY( dirY, base ), edges, green );
			src.fetch( recomposeX( dirX, base * ( 1.0 + spread ) ),
					   recomposeY( dirY, base * ( 1.0 + spread ) ), edges, red );
			src.fetch( recomposeX( dirX, base * ( 1.0 - spread ) ),
					   recomposeY( dirY, base * ( 1.0 - spread ) ), edges, blue );

			// Green carries the silhouette; per-channel alpha would fringe the
			// transparency too, which is one aberration too many.
			sum[ 0 ] += red[ 0 ];
			sum[ 1 ] += green[ 1 ];
			sum[ 2 ] += blue[ 2 ];
			sum[ 3 ] += green[ 3 ];
		}
		else
		{
			double s[ 4 ];
			src.fetch( recomposeX( dirX, base ), recomposeY( dirY, base ), edges, s );
			for( int c = 0; c < 4; ++c )
				sum[ c ] += s[ c ];
		}
	}

private:
	double recomposeX( double dirX, double srcRho ) const
	{
		double cx = dirX * srcRho * _refRadius / _s.zoom;
		if( !_stretched )
			cx /= _aspect;
		return cx + 0.5 + _s.centreX;
	}
	double recomposeY( double dirY, double srcRho ) const
	{
		return dirY * srcRho * _refRadius / _s.zoom + 0.5 + _s.centreY;
	}

	const WarpTileState& _s;
	double _aspect;
	double _refRadius;
	bool _stretched;
};

/// The whole render, over one destination tile.
///
/// `dstOriginX/Y` is where this tile sits inside the full destination image and
/// `imageW/H` is that image's size — the warp is defined over the whole picture,
/// so a tile cannot be treated as an image in its own right.
inline void warpTile( const SourceImage& src,
					  uint8_t* dstBase, size_t dstStride, Layout dstLayout,
					  int dstOriginX, int dstOriginY, int tileW, int tileH,
					  int imageW, int imageH, double pixelAspect,
					  const WarpTileState& state )
{
	const size_t dstBpp = fxsurface::bytesPerPixel( dstLayout );

	const double aspect = double( imageW ) * pixelAspect / double( imageH );
	const Warp warp( state, aspect );

	const double invW = 1.0 / double( imageW );
	const double invH = 1.0 / double( imageH );

	const int n          = std::max( state.taps, 1 );
	const double invTaps = 1.0 / double( n );
	const double scale   = 1.0 / double( n * n );

	for( int y = 0; y < tileH; ++y )
	{
		uint8_t* dstRow = dstBase + size_t( y ) * dstStride;

		for( int x = 0; x < tileW; ++x )
		{
			double sum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };

			for( int j = 0; j < n; ++j )
			{
				for( int i = 0; i < n; ++i )
				{
					// The rotated supersample grid, exactly as in the shader:
					// 26.6 degrees off the pixel axes so a regular grid doesn't
					// sample every horizontal edge at the same few heights.
					const double ox = ( ( i + 0.5 ) * invTaps - 0.5 );
					const double oy = ( ( j + 0.5 ) * invTaps - 0.5 );
					const double rx = ox * 0.8944272 - oy * 0.4472136;
					const double ry = ox * 0.4472136 + oy * 0.8944272;

					const double px = ( dstOriginX + x + 0.5 + rx ) * invW;
					const double py = ( dstOriginY + y + 0.5 + ry ) * invH;

					warp.sample( src, px, py, sum );
				}
			}

			// Samples were averaged premultiplied (the correct filter at a
			// transparent edge), and FxPlug wants premultiplied out, so all
			// that is left is to keep the invariant rgb <= a.
			const double a = sum[ 3 ] * scale;
			float out[ 4 ] = {
				float( std::min( sum[ 0 ] * scale, a ) ),
				float( std::min( sum[ 1 ] * scale, a ) ),
				float( std::min( sum[ 2 ] * scale, a ) ),
				float( a )
			};

			fxsurface::writePixel( dstRow + size_t( x ) * dstBpp, dstLayout, out );
		}
	}
}

} // namespace porthole

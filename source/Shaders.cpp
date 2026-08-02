#include "Shaders.h"

namespace porthole
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through. The usual FFGL vertex shader folds MaxUV in here, but a
	//warp has to do its geometry in picture space and scale only at the fetch.
	uv = vUV;
}
)";

const char* const kFragmentShader = R"(#version 410 core

uniform sampler2D InputTexture;

uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 HalfTexel;  //half an input texel, in picture space
uniform vec2 Centre;     //offset of the optical axis from the middle of frame
uniform float Aspect;    //picture width / height
uniform float Curve;     //k: +1 rectilinear, 0 equidistant, -1 orthographic
uniform float ThetaMax;  //half field of view at the reference radius, radians
uniform float Defish;    //0 applies the lens, 1 removes it
uniform float FitMode;   //0 diagonal, 1 width, 2 height, 3 stretch
uniform float Zoom;
uniform float Chromatic;
uniform float EdgeMode;  //0 transparent, 1 black, 2 clamp, 3 mirror, 4 wrap
uniform float Taps;      //grid order n, so n*n samples per output pixel

in vec2 uv;
out vec4 fragColor;

const float kFlat    = 1e-6;
const float kFarAway = 64.0;

//---------------------------------------------------------------------------
// The lens family. Mirror of Projection.cpp -- change both, then run
// `phtest --probe`, which exists to catch exactly this pair drifting apart.
//---------------------------------------------------------------------------
float project( float theta, float k )
{
	if( k > kFlat )
		return tan( k * theta ) / k;
	if( k < -kFlat )
		return sin( k * theta ) / k;
	return theta;
}

float unproject( float r, float k )
{
	if( k > kFlat )
		return atan( k * r ) / k;
	if( k < -kFlat )
		return asin( clamp( k * r, -1.0, 1.0 ) ) / k;
	return r;
}

/// Output radius -> source radius, both in units of the reference radius.
/// Returns 1.0 at 1.0 whatever the settings: the frame edge is a fixed point,
/// and only the interior is redistributed.
float warpRadius( float rho )
{
	if( ThetaMax < 1e-5 )
		return rho;

	float tanMax = tan( ThetaMax );
	float result;

	if( Defish < 0.5 )
	{
		float theta = unproject( rho * project( ThetaMax, Curve ), Curve );
		result      = tan( theta ) / tanMax;
	}
	else
	{
		float theta = atan( rho * tanMax );
		result      = project( theta, Curve ) / project( ThetaMax, Curve );
	}

	//Near-orthographic lenses cannot see past 90 degrees; unproject() clamps
	//there rather than returning a NaN, so tan() of the saturated angle runs
	//away. Those pixels have no source, which is the truth, and the edge mode
	//decides what to show. Written as a < test so a NaN also lands here.
	return ( result < kFarAway ) ? result : kFarAway;
}

//---------------------------------------------------------------------------
// Frame geometry.
//---------------------------------------------------------------------------
float referenceRadius()
{
	if( FitMode < 0.5 )
		return 0.5 * sqrt( Aspect * Aspect + 1.0 );//diagonal
	if( FitMode < 1.5 )
		return Aspect * 0.5;//width
	return 0.5;             //height, and stretch which works in UV directly
}

/// Split an output point into a direction and a normalised radius about the
/// optical axis. x is scaled by the aspect ratio first so the warp is circular
/// in pixels rather than circular in UV -- except under Stretch, which is that
/// correction deliberately left out.
void decompose( vec2 p, out vec2 dir, out float rho, out float R )
{
	vec2 c = p - ( vec2( 0.5 ) + Centre );
	if( FitMode < 2.5 )
		c.x *= Aspect;

	R = referenceRadius();

	float len = length( c );
	rho       = len / R;
	dir       = ( len > 1e-8 ) ? c / len : vec2( 0.0 );
}

vec2 recompose( vec2 dir, float srcRho, float R )
{
	vec2 c = dir * srcRho * R / Zoom;
	if( FitMode < 2.5 )
		c.x /= Aspect;
	return c + vec2( 0.5 ) + Centre;
}

//---------------------------------------------------------------------------
// Fetching.
//---------------------------------------------------------------------------
float mirrorCoord( float x )
{
	//GLSL mod() is x - y*floor(x/y), so this is already correct for negatives.
	float m = mod( x, 2.0 );
	return ( m > 1.0 ) ? ( 2.0 - m ) : m;
}

vec4 fetch( vec2 p )
{
	bool outside = any( lessThan( p, vec2( 0.0 ) ) ) || any( greaterThan( p, vec2( 1.0 ) ) );

	if( EdgeMode < 0.5 )
	{
		if( outside )
			return vec4( 0.0 );//transparent, and already premultiplied
	}
	else if( EdgeMode < 1.5 )
	{
		if( outside )
			return vec4( 0.0, 0.0, 0.0, 1.0 );//opaque black
	}
	else if( EdgeMode < 2.5 )
	{
		p = clamp( p, vec2( 0.0 ), vec2( 1.0 ) );
	}
	else if( EdgeMode < 3.5 )
	{
		p = vec2( mirrorCoord( p.x ), mirrorCoord( p.y ) );
	}
	else
	{
		p = fract( p );
	}

	//Never sample nearer than half a texel to the picture edge. The input
	//texture may be bigger than the picture -- MaxUV is the fraction of it that
	//was actually drawn -- so a linear fetch right at the edge takes half its
	//weight from padding that contains nothing.
	p = clamp( p, HalfTexel, vec2( 1.0 ) - HalfTexel );

	return texture( InputTexture, p * MaxUV );
}

void main()
{
	//The output pixel's size in picture-space units, from the rasteriser. Doing
	//it this way rather than from a resolution uniform means the supersample
	//grid is right whatever size the host is rendering at, and it has to happen
	//in uniform control flow, so it happens first.
	vec2 pixel = vec2( dFdx( uv.x ), dFdy( uv.y ) );

	int n     = int( Taps + 0.5 );
	float inv = 1.0 / float( n );

	vec4 sum = vec4( 0.0 );

	for( int j = 0; j < n; ++j )
	{
		for( int i = 0; i < n; ++i )
		{
			vec2 o = ( vec2( float( i ), float( j ) ) + 0.5 ) * inv - 0.5;

			//Rotate the grid off the pixel axes. An axis-aligned grid samples
			//every horizontal edge at the same few heights, which is where a
			//regular supersample still shows stair-stepping; 26.6 degrees is
			//the standard dodge.
			o = vec2( o.x * 0.8944272 - o.y * 0.4472136,
			          o.x * 0.4472136 + o.y * 0.8944272 );

			vec2 p = uv + o * pixel;

			vec2 dir;
			float rho;
			float R;
			decompose( p, dir, rho, R );

			float base = warpRadius( rho );

			if( Chromatic > 0.0 )
			{
				//Lateral chromatic aberration. This is not an added fringe: it
				//is the same lens at three slightly different focal lengths,
				//so the red, green and blue images come out at marginally
				//different scales. It grows with the square of the radius the
				//way the real thing does, which is why the middle of the frame
				//stays clean and only the corners break up.
				float spread = Chromatic * rho * rho;

				vec4 green = fetch( recompose( dir, base, R ) );
				vec4 red   = fetch( recompose( dir, base * ( 1.0 + spread ), R ) );
				vec4 blue  = fetch( recompose( dir, base * ( 1.0 - spread ), R ) );

				//Green carries the silhouette; taking each channel's own alpha
				//would give a coloured fringe on the transparency as well as
				//on the picture, which is one aberration too many.
				sum += vec4( red.r, green.g, blue.b, green.a );
			}
			else
			{
				sum += fetch( recompose( dir, base, R ) );
			}
		}
	}

	vec4 result = sum / float( n * n );

	//Premultiplied in, premultiplied out. Averaging premultiplied samples is
	//the correct filter -- it is unpremultiplied averaging that goes wrong at a
	//transparent edge -- so there is nothing to undo here. Just hold the
	//invariant the engine expects.
	result.rgb = clamp( result.rgb, vec3( 0.0 ), vec3( result.a ) );

	fragColor = result;
}
)";

} // namespace porthole

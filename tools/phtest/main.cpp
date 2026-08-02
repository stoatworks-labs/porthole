/**
    phtest -- render Porthole offline, and measure what it did.

    A warp is judged on where the pixels went, and that is a measurable thing
    rather than a matter of taste. So this harness is not only a previewer. It
    builds a headless GL 4.1 core context, drives the real Porthole class
    through the real FFGL entry sequence, and offers three ways of looking at
    the result:

        phtest --out /tmp/frame.png              a picture, on a geometry card
        phtest --probe                           where does each radius sample from?
        phtest --roundtrip                       does defish actually undo fish?

    `--probe` is the important one. The lens maths exists twice -- in C++ in
    Projection.cpp and in GLSL in Shaders.cpp -- because it has to run per pixel
    on the GPU but also has to be readable and testable on the CPU. Two copies
    of one formula drift. So the probe feeds in a picture whose brightness *is*
    the normalised radius, reads the output back, and reports the source radius
    the GPU actually sampled from beside the one the C++ predicts. A typo in
    either copy shows up as a column of mismatches.

    `--roundtrip` checks the claim that Defish is an exact inverse rather than
    an approximation: warp a smooth gradient one way, warp it back, and measure
    how far it is from where it started. A smooth picture is used deliberately,
    so that what is being measured is the geometry rather than the resampling.
*/

#include "Porthole.h"
#include "Projection.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace porthole;

namespace
{
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Test pictures. Each one exists to make a particular kind of wrong answer
// visible; none of them is meant to look nice.
//---------------------------------------------------------------------------
/// Writes with y measured from the top, which is how the patterns below are
/// described and how the PNG will be read. GL puts row zero at the bottom of a
/// texture, so the flip happens here rather than somewhere in the middle of the
/// chain where it would be a permanent trap.
///
/// The consequence worth remembering: **the buffers these builders return are
/// bottom-up**, ready for glTexImage2D. readBack() returns top-down. Comparing
/// one against the other index by index silently compares row y with row
/// height-1-y, so anything that does that has to flipRows() first.
void setPixel( std::vector< unsigned char >& image, int width, int height, int x, int y,
               float r, float g, float b, float a = 1.0f )
{
	const size_t i = ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4;
	auto byte      = []( float v ) {
        return static_cast< unsigned char >( std::lround( std::fmin( std::fmax( v, 0.0f ), 1.0f ) * 255.0f ) );
	};
	image[ i + 0 ] = byte( r );
	image[ i + 1 ] = byte( g );
	image[ i + 2 ] = byte( b );
	image[ i + 3 ] = byte( a );
}

/// The geometry card. A warp moves things, so the card is made of things whose
/// position you can read off: a grid whose straightness shows the shape of the
/// distortion, rings at known radii to check that the map is radial and that
/// the reference radius really is a fixed point, a band of fine detail out
/// where the warp compresses hardest so that the Quality control has something
/// to fail at, and white-on-black corners where chromatic aberration shows.
std::vector< unsigned char > buildGeometryCard( int width, int height, double aspect, Fit fit )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const double R         = referenceRadius( fit, aspect );
	const bool stretched   = fitIsStretched( fit );
	const double gridPitch = 1.0 / 16.0;//in height units

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const double px = ( x + 0.5 ) / width;
			const double py = ( y + 0.5 ) / height;

			double cx = px - 0.5;
			const double cy = py - 0.5;
			if( !stretched )
				cx *= aspect;

			const double rho = std::sqrt( cx * cx + cy * cy ) / R;

			float r = 0.02f, g = 0.02f, b = 0.02f;

			//Grid. Lines land on exact fractions of the height, so a bent line
			//is unambiguous.
			const double gx = std::fabs( std::fmod( std::fabs( cx ) + gridPitch * 0.5, gridPitch ) - gridPitch * 0.5 );
			const double gy = std::fabs( std::fmod( std::fabs( cy ) + gridPitch * 0.5, gridPitch ) - gridPitch * 0.5 );
			const double lineHalfWidth = 0.6 / height;
			if( gx < lineHalfWidth || gy < lineHalfWidth )
				r = g = b = 0.85f;

			//Rings at quarters of the reference radius, and a brighter one at
			//the reference radius itself -- the radius the map holds fixed.
			for( int ring = 1; ring <= 4; ++ring )
			{
				const double target = ring * 0.25;
				if( std::fabs( rho - target ) < ( 1.2 / height ) / R )
				{
					const bool outer = ring == 4;
					r                = outer ? 1.0f : 0.0f;
					g                = outer ? 0.55f : 0.9f;
					b                = outer ? 0.0f : 1.0f;
				}
			}

			//A band of fine detail two thirds of the way out, at a frequency
			//that will alias the moment the warp minifies it. This is what the
			//Quality control is for, and what it looks like when it is off.
			if( rho > 0.60 && rho < 0.72 )
			{
				const int checker = ( x / 2 + y / 2 ) & 1;
				r = g = b = checker ? 0.95f : 0.05f;
			}

			//Hard white blocks in the corners: chromatic aberration is a
			//radial displacement, so it shows on a high-contrast edge far from
			//the axis and nowhere near the middle.
			const double bx = std::fabs( cx ) / ( stretched ? 0.5 : aspect * 0.5 );
			const double by = std::fabs( cy ) / 0.5;
			if( bx > 0.80 && bx < 0.95 && by > 0.72 && by < 0.94 )
				r = g = b = 1.0f;

			setPixel( image, width, height, x, y, r, g, b );
		}
	}

	return image;
}

/// Brightness *is* the normalised radius. Feeding this in means the output
/// pixel at radius rho reports, as a number, the radius the shader sampled
/// from -- which is the whole mechanism of --probe.
std::vector< unsigned char > buildRadialRamp( int width, int height, double aspect, Fit fit )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const double R       = referenceRadius( fit, aspect );
	const bool stretched = fitIsStretched( fit );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			double cx       = ( x + 0.5 ) / width - 0.5;
			const double cy = ( y + 0.5 ) / height - 0.5;
			if( !stretched )
				cx *= aspect;

			const double rho = std::sqrt( cx * cx + cy * cy ) / R;
			const float v    = static_cast< float >( std::fmin( rho, 1.0 ) );
			setPixel( image, width, height, x, y, v, v, v );
		}
	}

	return image;
}

/// Smooth in both axes, so resampling it is almost lossless and a round trip
/// measures the geometry rather than the filter.
std::vector< unsigned char > buildGradient( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = static_cast< float >( x + 0.5 ) / width;
			const float v = static_cast< float >( y + 0.5 ) / height;
			setPixel( image, width, height, x, y, u, v, 0.5f * ( u + v ) );
		}
	}

	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shader compiles.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

bool runPass( Porthole& plugin, GLuint source, GLuint targetFBO, int width, int height, int frames )
{
	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = source;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = targetFBO;

	for( int frame = 0; frame < frames; ++frame )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, targetFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			return false;
	}

	return true;
}

/// Turn a bottom-up buffer top-down or the other way about -- the operation is
/// its own inverse. See the note on setPixel(): the test-picture builders and
/// readBack() disagree about which way up they are, and this is what reconciles
/// them.
std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

std::vector< unsigned char > readBack( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

	//GL hands back bottom-up; everything above is written top-down.
	std::vector< unsigned char > flipped( pixels.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             pixels.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

void usage()
{
	std::printf(
		"phtest -- render and measure the Porthole warp\n"
		"\n"
		"  --out PATH        where to write (default /tmp/porthole.png)\n"
		"  --width N         output width (default 1280)\n"
		"  --height N        output height (default 720)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1\n"
		"  --alpha           keep the alpha channel instead of compositing on black\n"
		"  --gradient        render the smooth gradient instead of the geometry card\n"
		"  --measure         print the mean RGB of the middle of the picture\n"
		"  --probe           measure the radial map on the GPU against the C++\n"
		"  --roundtrip       fish, then defish, and report the error\n"
		"  --list            print every parameter and its default, then exit\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/porthole.png";
	int width              = 1280;
	int height             = 720;
	int frames             = 1;//nothing here depends on time
	bool keepAlpha         = false;
	bool listOnly          = false;
	bool measure           = false;
	bool probe             = false;
	bool roundtrip         = false;
	bool gradient          = false;
	std::vector< std::pair< std::string, float > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
			outputPath = next();
		else if( arg == "--width" )
			width = std::atoi( next().c_str() );
		else if( arg == "--height" )
			height = std::atoi( next().c_str() );
		else if( arg == "--alpha" )
			keepAlpha = true;
		else if( arg == "--measure" )
			measure = true;
		else if( arg == "--probe" )
			probe = true;
		else if( arg == "--roundtrip" )
			roundtrip = true;
		else if( arg == "--gradient" )
			gradient = true;
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals          = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "phtest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "phtest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 )
	{
		std::fprintf( stderr, "phtest: width and height must both be positive\n" );
		return 2;
	}

	Porthole plugin;

	//Names come from the plugin's own declaration rather than from a table
	//here, so a parameter that is renamed or reordered cannot leave the harness
	//quietly setting the wrong one.
	auto indexOfParameter = [ & ]( const std::string& name ) -> int {
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
				return static_cast< int >( i );
		}
		return -1;
	};
	auto setParameter = [ & ]( const std::string& name, float value ) -> bool {
		const int index = indexOfParameter( name );
		if( index < 0 )
			return false;
		plugin.SetFloatParameter( static_cast< unsigned int >( index ), value );
		return true;
	};
	auto getParameter = [ & ]( const std::string& name ) -> float {
		const int index = indexOfParameter( name );
		return index < 0 ? 0.0f : plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
	};

	if( listOnly )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			std::printf( "%2u  %-16s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
		return 0;
	}

	for( const auto& override : overrides )
	{
		if( !setParameter( override.first, override.second ) )
		{
			std::fprintf( stderr, "phtest: no parameter named '%s' (try --list)\n", override.first.c_str() );
			return 2;
		}
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "phtest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	std::printf( "GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	const double aspect = static_cast< double >( width ) / static_cast< double >( height );

	FFGLViewportStruct viewport = { 0, 0, static_cast< FFUInt32 >( width ), static_cast< FFUInt32 >( height ) };
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "phtest: InitGL failed -- see the diagnostics log\n" );
		return 1;
	}

	//-----------------------------------------------------------------------
	// --probe: what radius did the GPU actually sample from?
	//-----------------------------------------------------------------------
	if( probe )
	{
		//Everything that would move a sample for a reason other than the radial
		//map is switched off: no chromatic split (it would move red and blue
		//off the reference), no supersampling (nothing to average), no zoom or
		//recentring, and clamped edges so nothing reads as transparent.
		setParameter( "Chromatic", 0.0f );
		setParameter( "Quality", 0.0f );
		setParameter( "Zoom", 0.5f );
		setParameter( "Centre X", 0.5f );
		setParameter( "Centre Y", 0.5f );
		setParameter( "Edges", 2.0f );

		const Fit fit         = static_cast< Fit >( static_cast< int >( std::lround( getParameter( "Fit" ) ) ) );
		const double R        = referenceRadius( fit, aspect );
		const bool stretched  = fitIsStretched( fit );
		const double k        = curveFromParam( getParameter( "Projection" ) );
		const double thetaMax = thetaMaxFromParam( getParameter( "Field of View" ) );
		const bool defish     = getParameter( "Defish" ) >= 0.5f;

		const std::vector< unsigned char > ramp = buildRadialRamp( width, height, aspect, fit );
		const GLuint sourceTexture              = makeTexture( width, height, ramp.data() );
		const GLuint targetTexture              = makeTexture( width, height, nullptr );
		const GLuint targetFBO                  = makeFramebuffer( targetTexture );

		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			std::fprintf( stderr, "phtest: framebuffer incomplete\n" );
			return 1;
		}
		if( !runPass( plugin, sourceTexture, targetFBO, width, height, frames ) )
		{
			std::fprintf( stderr, "phtest: ProcessOpenGL failed\n" );
			return 1;
		}

		const std::vector< unsigned char > out = readBack( targetFBO, width, height );

		std::printf( "\nprojection k=%+.3f  field of view %.1f deg  %s\n",
		             k, thetaMax * 2.0 * 180.0 / kPi, defish ? "defish" : "fish" );
		std::printf( "%8s %12s %12s %10s\n", "rho out", "GPU", "C++", "delta" );

		//The probe reads along the +x axis, so it can only see source radii
		//that exist on that axis inside the frame. Past that the fetch clamps
		//and the reading flattens out -- which looks exactly like a broken map
		//if it is not accounted for. Under a diagonal fit the ceiling is well
		//under 1.0, and the defish direction reaches it quickly, because
		//undoing a fisheye genuinely does want content from outside the frame.
		const double edgeOfFrame = ( stretched ? 0.5 : 0.5 * aspect ) / R;
		const double ceiling     = std::fmin( 1.0, edgeOfFrame ) - 0.01;

		double worst   = 0.0;
		int measured   = 0;
		int unreachable = 0;

		for( int step = 1; step <= 9; ++step )
		{
			const double rho = step * 0.1;

			//Where that radius falls along the +x axis through the centre.
			const double dx = stretched ? rho * R : rho * R / aspect;
			const int px    = static_cast< int >( std::lround( ( 0.5 + dx ) * width - 0.5 ) );
			const int py    = height / 2;
			if( px < 0 || px >= width )
				continue;

			const double predRho = warpRadius( rho, k, thetaMax, defish );
			if( predRho > ceiling )
			{
				std::printf( "%8.2f %12s %12.4f %10s\n", rho, "-", predRho, "off frame" );
				++unreachable;
				continue;
			}

			const size_t i      = ( static_cast< size_t >( py ) * width + px ) * 4;
			const double gpuRho = out[ i ] / 255.0;//brightness is the radius it sampled
			const double delta  = gpuRho - predRho;
			if( std::fabs( delta ) > worst )
				worst = std::fabs( delta );
			++measured;

			std::printf( "%8.2f %12.4f %12.4f %+10.4f\n", rho, gpuRho, predRho, delta );
		}

		if( measured == 0 )
		{
			std::printf( "\nnothing measurable: every radius wanted source from off the frame.\n"
			             "Try --set \"Fit=2\" (height), a narrower Field of View, or the fish direction.\n" );
			plugin.DeInitGL();
			CGLSetCurrentContext( nullptr );
			CGLDestroyContext( context );
			return 1;
		}
		if( unreachable > 0 )
			std::printf( "\n%d of %d radii wanted source from beyond the frame edge and were skipped.\n",
			             unreachable, measured + unreachable );

		//One 8-bit level is 0.0039, and the ramp is quantised twice over, so
		//anything under a couple of levels is the measurement rather than the
		//maths.
		const bool ok = worst < 0.012;
		std::printf( "\nworst |delta| = %.4f (%.1f levels)  %s\n",
		             worst, worst * 255.0, ok ? "OK" : "*** GPU AND C++ DISAGREE ***" );

		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return ok ? 0 : 1;
	}

	//-----------------------------------------------------------------------
	// --roundtrip: does the inverse actually invert?
	//-----------------------------------------------------------------------
	if( roundtrip )
	{
		//Clamped edges, because Transparent throws away the pixels outside the
		//frame and no inverse can bring those back. And no chromatic
		//aberration, which is a real displacement that the inverse is not
		//supposed to undo.
		setParameter( "Edges", 2.0f );
		setParameter( "Chromatic", 0.0f );

		const std::vector< unsigned char > source = buildGradient( width, height );
		const GLuint sourceTexture                = makeTexture( width, height, source.data() );
		const GLuint middleTexture                = makeTexture( width, height, nullptr );
		const GLuint finalTexture                 = makeTexture( width, height, nullptr );
		const GLuint middleFBO                    = makeFramebuffer( middleTexture );
		const GLuint finalFBO                     = makeFramebuffer( finalTexture );

		setParameter( "Defish", 0.0f );
		if( !runPass( plugin, sourceTexture, middleFBO, width, height, frames ) )
		{
			std::fprintf( stderr, "phtest: forward pass failed\n" );
			return 1;
		}

		setParameter( "Defish", 1.0f );
		if( !runPass( plugin, middleTexture, finalFBO, width, height, frames ) )
		{
			std::fprintf( stderr, "phtest: inverse pass failed\n" );
			return 1;
		}

		const std::vector< unsigned char > result = readBack( finalFBO, width, height );

		//readBack() is top-down and buildGradient() is bottom-up, so one of
		//them has to be turned over before they can be compared. Skipping this
		//does not look like a bug: it produces a large, plausible, and
		//completely constant error that does not change when the warp does.
		const std::vector< unsigned char > reference = flipRows( source, width, height );

		const Fit fit         = static_cast< Fit >( static_cast< int >( std::lround( getParameter( "Fit" ) ) ) );
		const double R        = referenceRadius( fit, aspect );
		const bool stretched  = fitIsStretched( fit );
		const double k        = curveFromParam( getParameter( "Projection" ) );
		const double thetaMax = thetaMaxFromParam( getParameter( "Field of View" ) );

		//An exact inverse still cannot recover what the forward pass threw
		//away. Where the fish compresses hard -- and a wide near-orthographic
		//lens compresses its outer band by six to one or worse -- several
		//source pixels land on one output pixel, and no amount of correct
		//geometry brings them back. Measuring there tests the resampler rather
		//than the inverse.
		//
		//The subtlety, and it is easy to get wrong: the loss is not governed by
		//the radius being *written* on the way back, it is governed by the
		//radius being *read* from the intermediate picture. Gating on the
		//forward map's gradient at the output radius passes the whole frame and
		//measures the damage anyway. The inverse's own gradient is the direct
		//statement of it -- D'(rho) < 0.5 means the inverse is stretching a
		//region the fish had squashed past 2:1 -- so gate on that.
		double rhoLimit = 0.0;
		for( double rho = 0.005; rho <= 1.4; rho += 0.005 )
		{
			const double h     = 1e-4;
			const double slope = ( warpRadius( rho + h, k, thetaMax, true )
			                       - warpRadius( rho - h, k, thetaMax, true ) )
			                     / ( 2.0 * h );
			if( slope < 0.5 )
				break;
			rhoLimit = rho;
		}

		double sum     = 0.0;
		double peak    = 0.0;
		size_t counted = 0;
		size_t skipped = 0;

		for( int y = height / 8; y < height * 7 / 8; ++y )
		{
			for( int x = width / 8; x < width * 7 / 8; ++x )
			{
				double cx       = ( x + 0.5 ) / static_cast< double >( width ) - 0.5;
				const double cy = ( y + 0.5 ) / static_cast< double >( height ) - 0.5;
				if( !stretched )
					cx *= aspect;
				const double len = std::sqrt( cx * cx + cy * cy );
				const double rho = len / R;

				if( rho > rhoLimit )
				{
					++skipped;
					continue;
				}

				//And the second reason a pixel cannot come back, which is
				//geometric rather than a matter of sampling density: the
				//inverse reads the intermediate picture at a larger radius than
				//it writes, and under a diagonal fit the larger radii only
				//exist near the corners. Along the axes they are off the frame
				//entirely, so the fetch clamps and returns the wrong pixel. No
				//inverse can fix that -- defishing genuinely wants a picture
				//wider than the one it was given.
				if( len > 1e-9 )
				{
					const double srcRho = warpRadius( rho, k, thetaMax, true );
					double sx           = ( cx / len ) * srcRho * R;
					const double sy     = ( cy / len ) * srcRho * R;
					if( !stretched )
						sx /= aspect;

					if( sx + 0.5 < 0.0 || sx + 0.5 > 1.0 || sy + 0.5 < 0.0 || sy + 0.5 > 1.0 )
					{
						++skipped;
						continue;
					}
				}

				const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
				for( int c = 0; c < 3; ++c )
				{
					const double d = std::fabs( static_cast< double >( result[ i + c ] ) - reference[ i + c ] );
					sum += d;
					if( d > peak )
						peak = d;
					++counted;
				}
			}
		}

		std::printf( "\nfish -> defish round trip over a smooth gradient\n" );
		std::printf( "measured out to rho %.2f, beyond which the fish had already squashed\n"
		             "the picture past 2:1 and the detail is genuinely gone\n", rhoLimit );

		if( counted == 0 )
		{
			std::printf( "nothing measurable: this lens compresses the whole frame past 2:1,\n"
			             "so a round trip here is limited by resampling loss rather than geometry.\n" );
			plugin.DeInitGL();
			CGLSetCurrentContext( nullptr );
			CGLDestroyContext( context );
			return 1;
		}

		const double mean     = sum / static_cast< double >( counted );
		const double fraction = static_cast< double >( counted / 3 )
		                        / static_cast< double >( counted / 3 + skipped );

		std::printf( "mean error %.3f levels, worst %.0f levels (of 255), over %.1f%% of the centre box\n",
		             mean, peak, 100.0 * fraction );

		//A perfect inverse still resamples twice, and two bilinear fetches of an
		//8-bit ramp cost about a level. Much more than that is geometry.
		const bool accurate = mean < 2.0;

		//But a clean number over a handful of pixels is not evidence of
		//anything. At the extreme end of the range the fish destroys or pushes
		//off-frame very nearly everything, and the right answer is to say the
		//test could not run rather than to report a pass on 0.4% of the frame.
		if( fraction < 0.05 )
		{
			std::printf( "INCONCLUSIVE -- too little of the frame survives the fish at these\n"
			             "settings to say whether the inverse is right. Narrow the Field of\n"
			             "View, or use --probe, which measures the map directly.\n" );
			plugin.DeInitGL();
			CGLSetCurrentContext( nullptr );
			CGLDestroyContext( context );
			return 2;
		}

		std::printf( "%s\n", accurate ? "OK -- the inverse inverts" : "*** ROUND TRIP DOES NOT CLOSE ***" );

		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return accurate ? 0 : 1;
	}

	//-----------------------------------------------------------------------
	// Plain render.
	//-----------------------------------------------------------------------
	const Fit fit = static_cast< Fit >( static_cast< int >( std::lround( getParameter( "Fit" ) ) ) );
	const std::vector< unsigned char > picture =
		gradient ? buildGradient( width, height ) : buildGeometryCard( width, height, aspect, fit );

	const GLuint sourceTexture = makeTexture( width, height, picture.data() );
	const GLuint targetTexture = makeTexture( width, height, nullptr );
	const GLuint targetFBO     = makeFramebuffer( targetTexture );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "phtest: output framebuffer is incomplete\n" );
		return 1;
	}

	if( !runPass( plugin, sourceTexture, targetFBO, width, height, frames ) )
	{
		std::fprintf( stderr, "phtest: ProcessOpenGL failed\n" );
		return 1;
	}

	std::vector< unsigned char > out = readBack( targetFBO, width, height );

	const GLenum error = glGetError();
	if( error != GL_NO_ERROR )
		std::fprintf( stderr, "phtest: GL error 0x%04x during render\n", error );

	//The plugin outputs premultiplied alpha, so the colour is already the
	//over-black composite. Flattening is just a matter of forcing alpha opaque.
	if( !keepAlpha )
	{
		for( size_t i = 3; i < out.size(); i += 4 )
			out[ i ] = 255;
	}

	if( measure )
	{
		double sum[ 3 ] = { 0.0, 0.0, 0.0 };
		size_t counted  = 0;
		for( int y = height / 4; y < height * 3 / 4; ++y )
		{
			for( int x = width / 4; x < width * 3 / 4; ++x )
			{
				const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
				sum[ 0 ] += out[ i + 0 ];
				sum[ 1 ] += out[ i + 1 ];
				sum[ 2 ] += out[ i + 2 ];
				++counted;
			}
		}
		const double n = static_cast< double >( counted ) * 255.0;
		std::printf( "mean RGB %.4f %.4f %.4f\n", sum[ 0 ] / n, sum[ 1 ] / n, sum[ 2 ] / n );
	}

	if( !writePng( outputPath, width, height, out ) )
	{
		std::fprintf( stderr, "phtest: could not write %s\n", outputPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%dx%d)\n", outputPath.c_str(), width, height );

	plugin.DeInitGL();
	glDeleteFramebuffers( 1, &targetFBO );
	glDeleteTextures( 1, &targetTexture );
	glDeleteTextures( 1, &sourceTexture );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}

/// The OpenFX build of Porthole, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// Same warp as the FFGL build: the lens family lives once, in
/// Projection.cpp, and this file links it rather than copying it. What *is*
/// mirrored here is the per-pixel machinery of Shaders.cpp — decompose /
/// recompose, the edge modes, the rotated supersample grid, the chromatic
/// fetch triple — because the GPU did that work per fragment and here it runs
/// on the CPU. When editing the fragment shader's pixel machinery, edit this
/// too; the lens maths itself has only the one home.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Projection.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.porthole";
constexpr const char* kPluginName       = "Porthole";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Fisheye and defish lens projection warp.\n\n"
	"Re-renders the picture through a different lens projection. The "
	"Projection slider walks a continuous family from rectilinear through "
	"the classic fisheyes to the mirror-ball, Defish runs the same maths in "
	"reverse, and the chromatic control is the same lens at three focal "
	"lengths, not an added fringe.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamProjection = "projection";
constexpr const char* kParamFov        = "fieldOfView";
constexpr const char* kParamDefish     = "defish";
constexpr const char* kParamChromatic  = "chromatic";
constexpr const char* kParamFit        = "fit";
constexpr const char* kParamCentreX    = "centreX";
constexpr const char* kParamCentreY    = "centreY";
constexpr const char* kParamZoom       = "zoom";
constexpr const char* kParamEdges      = "edges";
constexpr const char* kParamQuality    = "quality";

enum class EdgeMode
{
	Transparent = 0,
	Black       = 1,
	Clamp       = 2,
	Mirror      = 3,
	Wrap        = 4
};

/// Everything the warp needs, in the physical units Projection.h works in.
/// Filled once per render from the 0..1 parameters via the same conversion
/// functions the FFGL build uses.
struct WarpSettings
{
	double curve     = 0.0; //!< k
	double thetaMax  = 0.0;
	bool defish      = false;
	double chromatic = 0.0;
	porthole::Fit fit = porthole::Fit::Diagonal;
	double centreX   = 0.0; //!< offset of the optical axis from frame centre
	double centreY   = 0.0;
	double zoom      = 1.0;
	EdgeMode edges   = EdgeMode::Transparent;
	int taps         = 2;   //!< grid order n, n*n samples per output pixel
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

class PortholeProcessorBase : public OFX::ImageProcessor
{
public:
	explicit PortholeProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setup( OFX::Image* src, const WarpSettings& warpSettings, bool premultipliedValue )
	{
		srcImg        = src;
		warp          = warpSettings;
		premultiplied = premultipliedValue;

		const OfxRectI b = src->getBounds();
		srcW             = b.x2 - b.x1;
		srcH             = b.y2 - b.y1;

		const double par = src->getPixelAspectRatio() > 0.0 ? src->getPixelAspectRatio() : 1.0;
		aspect           = double( srcW ) * par / double( srcH );
		refRadius        = porthole::referenceRadius( warp.fit, aspect );
	}

protected:
	OFX::Image* srcImg = nullptr;
	WarpSettings warp;
	bool premultiplied = false;
	int srcW           = 0;
	int srcH           = 0;
	double aspect      = 1.0;
	double refRadius   = 0.5;
};

template<class PIX, int nComponents, int maxValue>
class PortholeProcessor : public PortholeProcessorBase
{
public:
	explicit PortholeProcessor( OFX::ImageEffect& effect ) :
		PortholeProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI dstBounds = _dstImg->getBounds();
		const double invW        = 1.0 / double( dstBounds.x2 - dstBounds.x1 );
		const double invH        = 1.0 / double( dstBounds.y2 - dstBounds.y1 );

		const int n          = std::max( warp.taps, 1 );
		const double invTaps = 1.0 / double( n );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				double sum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };

				for( int j = 0; j < n; ++j )
				{
					for( int i = 0; i < n; ++i )
					{
						// The rotated supersample grid, exactly as in the
						// shader: 26.6 degrees off the pixel axes so a regular
						// grid doesn't sample every horizontal edge at the
						// same few heights.
						double ox = ( ( i + 0.5 ) * invTaps - 0.5 );
						double oy = ( ( j + 0.5 ) * invTaps - 0.5 );
						const double rx = ox * 0.8944272 - oy * 0.4472136;
						const double ry = ox * 0.4472136 + oy * 0.8944272;

						const double px = ( x - dstBounds.x1 + 0.5 + rx ) * invW;
						const double py = ( y - dstBounds.y1 + 0.5 + ry ) * invH;

						sample( px, py, sum );
					}
				}

				const double scale = 1.0 / double( n * n );
				double r = sum[ 0 ] * scale;
				double g = sum[ 1 ] * scale;
				double b = sum[ 2 ] * scale;
				double a = sum[ 3 ] * scale;

				// Samples were averaged premultiplied (the correct filter at a
				// transparent edge). Premultiplied output just keeps the
				// invariant rgb <= a; straight output unpremultiplies again.
				if( premultiplied || nComponents == 3 )
				{
					r = std::min( r, a );
					g = std::min( g, a );
					b = std::min( b, a );
				}
				else if( a > 0.0 )
				{
					r /= a;
					g /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( g );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	/// One supersample: decompose about the optical axis, warp the radius
	/// through the lens family, recompose, fetch (three times when the
	/// chromatic control is up). Accumulates premultiplied RGBA into `sum`.
	void sample( double px, double py, double sum[ 4 ] ) const
	{
		double cx = px - ( 0.5 + warp.centreX );
		double cy = py - ( 0.5 + warp.centreY );
		if( !porthole::fitIsStretched( warp.fit ) )
			cx *= aspect;

		const double len = std::sqrt( cx * cx + cy * cy );
		const double rho = len / refRadius;
		const double dirX = ( len > 1e-8 ) ? cx / len : 0.0;
		const double dirY = ( len > 1e-8 ) ? cy / len : 0.0;

		const double base = porthole::warpRadius( rho, warp.curve, warp.thetaMax, warp.defish );

		if( warp.chromatic > 0.0 )
		{
			// The same lens at three focal lengths; grows with the square of
			// the radius the way the real thing does.
			const double spread = warp.chromatic * rho * rho;

			double green[ 4 ], red[ 4 ], blue[ 4 ];
			fetch( recomposeX( dirX, base ), recomposeY( dirY, base ), green );
			fetch( recomposeX( dirX, base * ( 1.0 + spread ) ), recomposeY( dirY, base * ( 1.0 + spread ) ), red );
			fetch( recomposeX( dirX, base * ( 1.0 - spread ) ), recomposeY( dirY, base * ( 1.0 - spread ) ), blue );

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
			fetch( recomposeX( dirX, base ), recomposeY( dirY, base ), s );
			for( int c = 0; c < 4; ++c )
				sum[ c ] += s[ c ];
		}
	}

	double recomposeX( double dirX, double srcRho ) const
	{
		double cx = dirX * srcRho * refRadius / warp.zoom;
		if( !porthole::fitIsStretched( warp.fit ) )
			cx /= aspect;
		return cx + 0.5 + warp.centreX;
	}
	double recomposeY( double dirY, double srcRho ) const
	{
		return dirY * srcRho * refRadius / warp.zoom + 0.5 + warp.centreY;
	}

	/// Fetch at picture-space p, premultiplied RGBA out. Mirrors the shader:
	/// edge mode first, then never sample nearer than half a texel to the
	/// picture edge, then a bilinear tap.
	void fetch( double px, double py, double out[ 4 ] ) const
	{
		const bool outside = px < 0.0 || py < 0.0 || px > 1.0 || py > 1.0;

		switch( warp.edges )
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

		// Pixel coordinates of the bilinear tap, kept half a texel inside the
		// picture so the tap never weights anything beyond the edge row.
		double fx = px * srcW - 0.5;
		double fy = py * srcH - 0.5;
		fx        = std::clamp( fx, 0.0, double( srcW - 1 ) );
		fy        = std::clamp( fy, 0.0, double( srcH - 1 ) );

		const int x0    = int( fx );
		const int y0    = int( fy );
		const int x1    = std::min( x0 + 1, srcW - 1 );
		const int y1    = std::min( y0 + 1, srcH - 1 );
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

	/// One texel, premultiplied RGBA in 0..1. Straight-alpha input is
	/// premultiplied here so the averaging above filters correctly.
	void texel( int x, int y, double out[ 4 ] ) const
	{
		const OfxRectI b  = srcImg->getBounds();
		const PIX* srcPix = static_cast<const PIX*>( srcImg->getPixelAddress( b.x1 + x, b.y1 + y ) );
		if( !srcPix )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}

		out[ 0 ] = srcPix[ 0 ] / double( maxValue );
		out[ 1 ] = srcPix[ 1 ] / double( maxValue );
		out[ 2 ] = srcPix[ 2 ] / double( maxValue );
		out[ 3 ] = nComponents == 4 ? srcPix[ 3 ] / double( maxValue ) : 1.0;

		if( !premultiplied && nComponents == 4 )
		{
			out[ 0 ] *= out[ 3 ];
			out[ 1 ] *= out[ 3 ];
			out[ 2 ] *= out[ 3 ];
		}
	}

	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class PortholePlugin : public OFX::ImageEffect
{
public:
	explicit PortholePlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip    = fetchClip( kOfxImageEffectOutputClipName );
		srcClip    = fetchClip( kOfxImageEffectSimpleSourceClipName );
		projection = fetchDoubleParam( kParamProjection );
		fov        = fetchDoubleParam( kParamFov );
		defish     = fetchBooleanParam( kParamDefish );
		chromatic  = fetchDoubleParam( kParamChromatic );
		fit        = fetchChoiceParam( kParamFit );
		centreX    = fetchDoubleParam( kParamCentreX );
		centreY    = fetchDoubleParam( kParamCentreY );
		zoom       = fetchDoubleParam( kParamZoom );
		edges      = fetchChoiceParam( kParamEdges );
		quality    = fetchChoiceParam( kParamQuality );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src( srcClip->fetchImage( args.time ) );

		const WarpSettings warp = settingsAtTime( args.time );
		const bool premultiplied =
			srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run<PortholeProcessor<unsigned char, 4, 255>>( args, dst.get(), src.get(), warp, premultiplied )
				: run<PortholeProcessor<unsigned char, 3, 255>>( args, dst.get(), src.get(), warp, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run<PortholeProcessor<unsigned short, 4, 65535>>( args, dst.get(), src.get(), warp, premultiplied )
				: run<PortholeProcessor<unsigned short, 3, 65535>>( args, dst.get(), src.get(), warp, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run<PortholeProcessor<float, 4, 1>>( args, dst.get(), src.get(), warp, premultiplied )
				: run<PortholeProcessor<float, 3, 1>>( args, dst.get(), src.get(), warp, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		// Rectilinear through a rectilinear lens is the same picture — the
		// maths says so without being told — provided nothing else moves it:
		// zoom at 1:1 and no chromatic spread. Centre cancels on its own.
		if( projection->getValueAtTime( args.time ) <= 0.0
			&& std::abs( zoom->getValueAtTime( args.time ) - 0.5 ) < 1e-9
			&& chromatic->getValueAtTime( args.time ) <= 0.0 )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

private:
	WarpSettings settingsAtTime( double t ) const
	{
		WarpSettings w;
		w.curve     = porthole::curveFromParam( float( projection->getValueAtTime( t ) ) );
		w.thetaMax  = porthole::thetaMaxFromParam( float( fov->getValueAtTime( t ) ) );
		w.defish    = defish->getValueAtTime( t );
		w.chromatic = porthole::chromaticFromParam( float( chromatic->getValueAtTime( t ) ) );

		int fitValue = 0, edgesValue = 0, qualityValue = 1;
		fit->getValueAtTime( t, fitValue );
		edges->getValueAtTime( t, edgesValue );
		quality->getValueAtTime( t, qualityValue );

		w.fit     = porthole::Fit( fitValue );
		w.centreX = centreX->getValueAtTime( t ) - 0.5;
		w.centreY = centreY->getValueAtTime( t ) - 0.5;
		w.zoom    = porthole::zoomFromParam( float( zoom->getValueAtTime( t ) ) );
		w.edges   = EdgeMode( edgesValue );
		w.taps    = porthole::tapsFromParam( float( qualityValue ) );
		return w;
	}

	template<class Processor>
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
			  const WarpSettings& warp, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setup( src, warp, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip           = nullptr;
	OFX::Clip* srcClip           = nullptr;
	OFX::DoubleParam* projection = nullptr;
	OFX::DoubleParam* fov        = nullptr;
	OFX::BooleanParam* defish    = nullptr;
	OFX::DoubleParam* chromatic  = nullptr;
	OFX::ChoiceParam* fit        = nullptr;
	OFX::DoubleParam* centreX    = nullptr;
	OFX::DoubleParam* centreY    = nullptr;
	OFX::DoubleParam* zoom       = nullptr;
	OFX::ChoiceParam* edges      = nullptr;
	OFX::ChoiceParam* quality    = nullptr;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( PortholePluginFactory, {}, {} );

void PortholePluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A warp samples anywhere in the source, so it cannot render from tiles;
	// frames are still independent of each other and of render order.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void PortholePluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so
	// the two inspectors read identically and the docs cover both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	OFX::GroupParamDescriptor* lens = desc.defineGroupParam( "Lens" );
	lens->setLabels( "Lens", "Lens", "Lens" );

	defineSlider( desc, page, kParamProjection, "Projection",
				  "Which lens: 0 rectilinear (no distortion), 0.5 equidistant "
				  "(the classic fisheye), 1 orthographic (the mirror ball).",
				  0.5 )
		->setParent( *lens );
	defineSlider( desc, page, kParamFov, "Field of View",
				  "Half-angle at the reference radius; 0 is no warp at all.", 0.5 )
		->setParent( *lens );

	OFX::BooleanParamDescriptor* defishParam = desc.defineBooleanParam( kParamDefish );
	defishParam->setLabels( "Defish", "Defish", "Defish" );
	defishParam->setHint( "Remove the lens instead of applying it: the exact inverse map." );
	defishParam->setDefault( false );
	defishParam->setParent( *lens );
	page->addChild( *defishParam );

	defineSlider( desc, page, kParamChromatic, "Chromatic",
				  "Lateral chromatic aberration: the same lens at three focal "
				  "lengths, growing with the square of the radius.",
				  0.08 )
		->setParent( *lens );

	OFX::GroupParamDescriptor* frame = desc.defineGroupParam( "Frame" );
	frame->setLabels( "Frame", "Frame", "Frame" );

	OFX::ChoiceParamDescriptor* fitParam = desc.defineChoiceParam( kParamFit );
	fitParam->setLabels( "Fit", "Fit", "Fit" );
	fitParam->setHint( "How the reference circle is fitted to the frame." );
	fitParam->appendOption( "Diagonal" );
	fitParam->appendOption( "Width" );
	fitParam->appendOption( "Height" );
	fitParam->appendOption( "Stretch" );
	fitParam->setDefault( 0 );
	fitParam->setParent( *frame );
	page->addChild( *fitParam );

	defineSlider( desc, page, kParamCentreX, "Centre X", "Optical axis; 0.5 is on axis.", 0.5 )
		->setParent( *frame );
	defineSlider( desc, page, kParamCentreY, "Centre Y", "Optical axis; 0.5 is on axis.", 0.5 )
		->setParent( *frame );
	defineSlider( desc, page, kParamZoom, "Zoom",
				  "Geometric zoom, 0.5 is 1:1; two stops either side of unity.", 0.5 )
		->setParent( *frame );

	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	OFX::ChoiceParamDescriptor* edgesParam = desc.defineChoiceParam( kParamEdges );
	edgesParam->setLabels( "Edges", "Edges", "Edges" );
	edgesParam->setHint( "What to show where the warp looks past the picture." );
	edgesParam->appendOption( "Transparent" );
	edgesParam->appendOption( "Black" );
	edgesParam->appendOption( "Clamp" );
	edgesParam->appendOption( "Mirror" );
	edgesParam->appendOption( "Wrap" );
	edgesParam->setDefault( 0 );
	edgesParam->setParent( *output );
	page->addChild( *edgesParam );

	OFX::ChoiceParamDescriptor* qualityParam = desc.defineChoiceParam( kParamQuality );
	qualityParam->setLabels( "Quality", "Quality", "Quality" );
	qualityParam->setHint( "Supersampling: Fast is 1 sample, Good 4, Best 16." );
	qualityParam->appendOption( "Fast" );
	qualityParam->appendOption( "Good" );
	qualityParam->appendOption( "Best" );
	qualityParam->setDefault( 1 );
	qualityParam->setParent( *output );
	page->addChild( *qualityParam );
}

OFX::ImageEffect* PortholePluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new PortholePlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static PortholePluginFactory* factory =
		new PortholePluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}

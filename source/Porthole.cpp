#include "Porthole.h"

#include "Diag.h"
#include "Presets.h"
#include "Projection.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace porthole;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Porthole >,                     // Create method
	"PH01",                                        // Plugin unique ID of maximum length 4.
	"Porthole",                                    // Plugin name
	2,                                             // API major version number
	1,                                             // API minor version number
	0,                                             // Plugin major version number
	1,                                             // Plugin minor version number
	FF_EFFECT,                                     // Plugin type
	"Fisheye and defish lens projection warp",     // Plugin description
	"Porthole FFGL effect"                         // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

Porthole::Porthole()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// Set to a moderate equidistant fisheye rather than to nothing. An effect
	// that does nothing until three sliders have been found is an effect
	// nobody discovers is any good -- and the null here is Field of View at
	// zero, which is one drag away.
	//---------------------------------------------------------------------
	params[ PT_PROJECTION ]    = 0.5f;//equidistant: the classic fisheye
	params[ PT_FIELD_OF_VIEW ] = 0.5f;//about 89 degrees across the reference circle
	params[ PT_DEFISH ]        = 0.0f;
	params[ PT_CHROMATIC ]     = 0.08f;//just enough to read as glass

	params[ PT_FIT ]           = 0.0f;//diagonal: corners pinned, edges bulge
	params[ PT_CENTRE_X ]      = 0.5f;//0.5 is on axis
	params[ PT_CENTRE_Y ]      = 0.5f;
	params[ PT_ZOOM ]          = 0.5f;//0.5 is 1:1

	params[ PT_EDGES ]         = 0.0f;//transparent
	params[ PT_QUALITY ]       = 1.0f;//good

	params[ PT_PRESET ]        = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every parameter is a plain 0..1 float even where it stands for degrees
	// or a magnification. SetParamRange exists, but SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached
	// (SDK b1afaf9), so a parameter declared in degrees cannot declare a
	// default in degrees. The conversions live in Projection.cpp.
	//---------------------------------------------------------------------
	SetParamInfof( PT_PROJECTION, "Projection", FF_TYPE_STANDARD );
	SetParamInfof( PT_FIELD_OF_VIEW, "Field of View", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEFISH, "Defish", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_CHROMATIC, "Chromatic", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_FIT, "Fit", 4, params[ PT_FIT ] );
	SetParamElementInfo( PT_FIT, 0, "Diagonal", 0.0f );
	SetParamElementInfo( PT_FIT, 1, "Width", 1.0f );
	SetParamElementInfo( PT_FIT, 2, "Height", 2.0f );
	SetParamElementInfo( PT_FIT, 3, "Stretch", 3.0f );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, "" );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	SetParamInfof( PT_CENTRE_X, "Centre X", FF_TYPE_STANDARD );
	SetParamInfof( PT_CENTRE_Y, "Centre Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ZOOM, "Zoom", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_EDGES, "Edges", 5, params[ PT_EDGES ] );
	SetParamElementInfo( PT_EDGES, 0, "Transparent", 0.0f );
	SetParamElementInfo( PT_EDGES, 1, "Black", 1.0f );
	SetParamElementInfo( PT_EDGES, 2, "Clamp", 2.0f );
	SetParamElementInfo( PT_EDGES, 3, "Mirror", 3.0f );
	SetParamElementInfo( PT_EDGES, 4, "Wrap", 4.0f );

	SetOptionParamInfo( PT_QUALITY, "Quality", 3, params[ PT_QUALITY ] );
	SetParamElementInfo( PT_QUALITY, 0, "Fast", 0.0f );
	SetParamElementInfo( PT_QUALITY, 1, "Good", 1.0f );
	SetParamElementInfo( PT_QUALITY, 2, "Best", 2.0f );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the look parameters and raises value events so the
	// host re-reads the sliders. Editing a covered slider flips back to Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + porthole::presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < porthole::presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, porthole::presets::kPresets[ i ].name, float( 1 + i ) );

	//Ten parameters is around the point where an ungrouped list in somebody
	//else's inspector stops being readable.
	SetParamGroup( PT_PROJECTION, "Lens" );
	SetParamGroup( PT_FIELD_OF_VIEW, "Lens" );
	SetParamGroup( PT_DEFISH, "Lens" );
	SetParamGroup( PT_CHROMATIC, "Lens" );

	SetParamGroup( PT_FIT, "Frame" );
	SetParamGroup( PT_CENTRE_X, "Frame" );
	SetParamGroup( PT_CENTRE_Y, "Frame" );
	SetParamGroup( PT_ZOOM, "Frame" );

	SetParamGroup( PT_EDGES, "Output" );
	SetParamGroup( PT_QUALITY, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created Porthole effect" );

	porthole::diag::init();
}

FFResult Porthole::InitGL( const FFGLViewportStruct* vp )
{
	// The GL strings first, and unconditionally: when a shader will not compile
	// it is almost always the driver or the GL version, and knowing which
	// machine reported what is most of the diagnosis.
	porthole::diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	                      + " renderer=" + glStringOrUnknown( GL_RENDERER )
	                      + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !shader.Compile( kVertexShader, kFragmentShader ) )
	{
		// Returning FF_FAIL here is invisible to the operator: the effect
		// simply does nothing in Resolume, with no message anywhere. This line
		// is the only record that it was the shader.
		porthole::diag::error( "shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Porthole: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		porthole::diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Porthole: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	porthole::diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult Porthole::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//FFGL requires the context to be left in a default state on return, so use
	//the scoped bindings for everything touched here.
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateSampler( 0 );
	Scoped2DTextureBinding textureBinding( picture.Handle );

	shader.Set( "InputTexture", 0 );

	//The input texture can be larger than the picture inside it, and its
	//dimensions can change from frame to frame, so both of these are uniforms
	//rather than anything baked into the geometry.
	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
	shader.Set( "MaxUV", maxCoords.s, maxCoords.t );

	//Half an input texel, expressed in picture space. The shader keeps every
	//fetch this far inside the picture so a linear tap at the edge cannot reach
	//into the texture's undrawn padding.
	shader.Set( "HalfTexel",
	            0.5f / static_cast< float >( picture.Width ),
	            0.5f / static_cast< float >( picture.Height ) );

	const float aspect = static_cast< float >( picture.Width ) / static_cast< float >( picture.Height );
	shader.Set( "Aspect", aspect );

	shader.Set( "Curve", static_cast< float >( curveFromParam( params[ PT_PROJECTION ] ) ) );
	shader.Set( "ThetaMax", static_cast< float >( thetaMaxFromParam( params[ PT_FIELD_OF_VIEW ] ) ) );
	shader.Set( "Defish", params[ PT_DEFISH ] );
	shader.Set( "Chromatic", static_cast< float >( chromaticFromParam( params[ PT_CHROMATIC ] ) ) );

	shader.Set( "FitMode", params[ PT_FIT ] );
	shader.Set( "Centre", params[ PT_CENTRE_X ] - 0.5f, params[ PT_CENTRE_Y ] - 0.5f );
	shader.Set( "Zoom", static_cast< float >( zoomFromParam( params[ PT_ZOOM ] ) ) );

	shader.Set( "EdgeMode", params[ PT_EDGES ] );
	shader.Set( "Taps", static_cast< float >( tapsFromParam( params[ PT_QUALITY ] ) ) );

	quad.Draw();

	return FF_SUCCESS;
}

FFResult Porthole::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();

	return FF_SUCCESS;
}

FFResult Porthole::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = int( std::lround( value ) );
		if( chosen != int( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters —
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const float previous = params[ index ];

	//Deliberately not logged. A parameter change is not a diagnostic event: the
	//host already shows the value, and an operator animating a slider would put
	//a line in the log every frame. This log exists for the shader that will not
	//compile, and it is worth nothing if it is buried.
	params[ index ] = value;

	const int active = int( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void Porthole::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = float( presetIndex );

	if( presetIndex <= 0 || presetIndex > porthole::presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const porthole::presets::Preset& preset = porthole::presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < porthole::presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Porthole::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

FFResult Porthole::SetTextParameter( unsigned int index, const char* )
{
	// The About text is generated on read and never stored — but a set of it
	// must SUCCEED. The SDK's instantiateGL pushes every parameter's default
	// into a fresh instance and destroys it on the first FF_FAIL, and the
	// base SetTextParameter returns FF_FAIL — so without this the plugin
	// fails FF_INSTANTIATE_GL in the host, with no message anywhere.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, nullptr );
}

char* Porthole::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

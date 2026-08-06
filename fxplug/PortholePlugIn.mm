/// The FxPlug 4 build of Porthole, for Final Cut Pro and Motion.
///
/// The lens family is not here — it is in source/Projection.{h,cpp}, shared with
/// the FFGL and OpenFX builds — and the warp itself is in PortholeTile.h so it
/// can be tested without a host. What is here is the shape FxPlug demands.
///
/// Two things differ from the Luma Key port, and both come from the same fact:
/// a warp reads from anywhere in the source.
///
///  - `kFxPropertyKey_NeedsFullBuffer` is YES and `-sourceTileRect:...` asks for
///    the whole source image rather than the matching tile. Asking for the
///    matching tile would sample black wherever the lens reaches outside it.
///  - Output pixels are placed by their position in the FULL image, not in the
///    tile, because the warp is defined over the whole picture.

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <FxPlug/FxPlugSDK.h>

#include <algorithm>
#include <cmath>

#include "PortholeTile.h"
#include "Presets.h"

/// FxPlug reserves everything below kFxError_ThirdPartyDeveloperStart, and has
/// no code for "I don't know this pixel format", so that one is ours.
enum {
	kPortholeError_UnsupportedPixelFormat = kFxError_ThirdPartyDeveloperStart + 1,
};

/// Parameter IDs. A saved project refers to a parameter by ID, so changing one
/// silently detaches every existing use of the effect from its value. Never
/// renumber. The order matches the OpenFX build's parameter order.
enum {
	kParamID_Preset     = 1,
	kParamID_LensGroup  = 2,
	kParamID_Projection = 3,
	kParamID_Fov        = 4,
	kParamID_Defish     = 5,
	kParamID_Chromatic  = 6,
	kParamID_FrameGroup = 7,
	kParamID_Fit        = 8,
	kParamID_CentreX    = 9,
	kParamID_CentreY    = 10,
	kParamID_Zoom       = 11,
	kParamID_OutputGroup = 12,
	kParamID_Edges      = 13,
	kParamID_Quality    = 14,
};

using fxsurface::Layout;
using porthole::WarpTileState;

namespace {

/// The one FxPlug-specific piece of the pixel path.
Layout layoutForSurface( IOSurfaceRef surface )
{
	switch( IOSurfaceGetPixelFormat( surface ) )
	{
	case 'BGRA': return Layout::BGRA8;
	case 'RGBA': return Layout::RGBA8;
	case 'RGhA':
	case 'RGbA': return Layout::RGBAh;
	case 'RGfA':
	case 'RGFA': return Layout::RGBAf;
	default:     return Layout::Unsupported;
	}
}

/// The parameter each preset slot drives, in the order Presets.h fixes. The
/// static_assert is the same guard the FFGL and OpenFX builds carry: three
/// lists, one order, and none of them allowed to drift silently.
const UInt32 kPresetParamIDs[] = {
	kParamID_Projection,
	kParamID_Fov,
	kParamID_Defish,
	kParamID_Chromatic,
	kParamID_Fit,
	kParamID_Zoom,
	kParamID_Edges,
};
static_assert( sizeof( kPresetParamIDs ) / sizeof( kPresetParamIDs[ 0 ] )
			   == porthole::presets::kParamCount,
			   "preset parameter list is out of step with Presets.h" );

/// Which preset slots are option menus rather than sliders — they carry an
/// element index, not a 0..1 value, so they are set as ints.
bool presetSlotIsChoice( int slot )
{
	return slot == porthole::presets::kFit || slot == porthole::presets::kEdges;
}

bool presetSlotIsToggle( int slot )
{
	return slot == porthole::presets::kDefish;
}

} // namespace


@interface PortholePlugIn : NSObject <FxTileableEffect>
@end

@implementation PortholePlugIn
{
	__weak id<PROAPIAccessing> _apiManager;
}

- (nullable instancetype)initWithAPIManager:(id<PROAPIAccessing>)apiManager
{
	self = [super init];
	if( self != nil )
		_apiManager = apiManager;
	return self;
}

- (BOOL)addParametersWithError:(NSError**)error
{
	id<FxParameterCreationAPI_v5> params =
		[_apiManager apiForProtocol:@protocol( FxParameterCreationAPI_v5 )];
	if( params == nil )
	{
		if( error != NULL )
			*error = [NSError errorWithDomain:FxPlugErrorDomain
										 code:kFxError_APIUnavailable
									 userInfo:@{ NSLocalizedDescriptionKey :
												 @"Porthole: no parameter creation API" }];
		return NO;
	}

	// Preset first, as in every other build: element 0 is Custom, meaning "the
	// sliders are the truth".
	NSMutableArray<NSString*>* presetNames = [NSMutableArray arrayWithObject:@"Custom"];
	for( int i = 0; i < porthole::presets::kCount; ++i )
		[presetNames addObject:@( porthole::presets::kPresets[ i ].name )];

	if( ![params addPopupMenuWithName:@"Preset"
						  parameterID:kParamID_Preset
						 defaultValue:0
						  menuEntries:presetNames
					   parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	// ------------------------------------------------------------------ Lens
	if( ![params startParameterSubGroup:@"Lens"
							parameterID:kParamID_LensGroup
						 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![self addSlider:params name:@"Projection" paramID:kParamID_Projection default:0.0] )
		return NO;
	if( ![self addSlider:params name:@"Field of View" paramID:kParamID_Fov default:0.5] )
		return NO;
	if( ![params addToggleButtonWithName:@"Defish"
							 parameterID:kParamID_Defish
							defaultValue:NO
						  parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;
	if( ![self addSlider:params name:@"Chromatic" paramID:kParamID_Chromatic default:0.0] )
		return NO;

	if( ![params endParameterSubGroup] )
		return NO;

	// ----------------------------------------------------------------- Frame
	if( ![params startParameterSubGroup:@"Frame"
							parameterID:kParamID_FrameGroup
						 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![params addPopupMenuWithName:@"Fit"
						  parameterID:kParamID_Fit
						 defaultValue:0
						  menuEntries:@[ @"Diagonal", @"Width", @"Height", @"Stretch" ]
					   parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;
	if( ![self addSlider:params name:@"Centre X" paramID:kParamID_CentreX default:0.5] )
		return NO;
	if( ![self addSlider:params name:@"Centre Y" paramID:kParamID_CentreY default:0.5] )
		return NO;
	if( ![self addSlider:params name:@"Zoom" paramID:kParamID_Zoom default:0.5] )
		return NO;

	if( ![params endParameterSubGroup] )
		return NO;

	// ---------------------------------------------------------------- Output
	if( ![params startParameterSubGroup:@"Output"
							parameterID:kParamID_OutputGroup
						 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![params addPopupMenuWithName:@"Edges"
						  parameterID:kParamID_Edges
						 defaultValue:0
						  menuEntries:@[ @"Transparent", @"Black", @"Clamp", @"Mirror", @"Wrap" ]
					   parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;
	if( ![params addPopupMenuWithName:@"Quality"
						  parameterID:kParamID_Quality
						 defaultValue:1
						  menuEntries:@[ @"Fast", @"Good", @"Best" ]
					   parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![params endParameterSubGroup] )
		return NO;

	return YES;
}

/// Every slider this plugin exposes is a plain 0..1 float — see the note in
/// Projection.h about why the host side is deliberately unitless.
- (BOOL)addSlider:(id<FxParameterCreationAPI_v5>)params
			 name:(NSString*)name
		  paramID:(UInt32)paramID
		  default:(double)defaultValue
{
	return [params addFloatSliderWithName:name
							  parameterID:paramID
							 defaultValue:defaultValue
							 parameterMin:0.0
							 parameterMax:1.0
								sliderMin:0.0
								sliderMax:1.0
									delta:0.01
						   parameterFlags:kFxParameterFlag_DEFAULT];
}

- (BOOL)properties:(NSDictionary* _Nonnull* _Nullable)properties error:(NSError**)error
{
	// A warp samples anywhere in the source, so it cannot render from a tile of
	// it. Frames remain independent of each other and of render order.
	*properties = @{
		kFxPropertyKey_NeedsFullBuffer           : @YES,
		kFxPropertyKey_VariesWhenParamsAreStatic : @NO,
		kFxPropertyKey_ChangesOutputSize         : @NO,
	};
	return YES;
}

// --------------------------------------------------------------------- presets

/// Applying a preset, and dropping back to Custom when the operator moves one
/// of the parameters it covers. Same behaviour as the FFGL and OpenFX builds.
- (BOOL)parameterChanged:(UInt32)paramID atTime:(CMTime)time error:(NSError**)error
{
	id<FxParameterRetrievalAPI_v6> get =
		[_apiManager apiForProtocol:@protocol( FxParameterRetrievalAPI_v6 )];
	id<FxParameterSettingAPI_v5> set =
		[_apiManager apiForProtocol:@protocol( FxParameterSettingAPI_v5 )];
	if( get == nil || set == nil )
		return YES;// nothing to do rather than an error: the render is unaffected

	if( paramID == kParamID_Preset )
	{
		int chosen = 0;
		if( ![get getIntValue:&chosen fromParameter:kParamID_Preset atTime:time] )
			return YES;
		if( chosen <= 0 || chosen > porthole::presets::kCount )
			return YES;// Custom, or out of range: leave the sliders alone

		const porthole::presets::Preset& p = porthole::presets::kPresets[ chosen - 1 ];
		for( int slot = 0; slot < porthole::presets::kParamCount; ++slot )
		{
			const UInt32 target = kPresetParamIDs[ slot ];
			const float value   = p.v[ slot ];

			if( presetSlotIsChoice( slot ) )
				[set setIntValue:int( std::lround( value ) ) toParameter:target atTime:time];
			else if( presetSlotIsToggle( slot ) )
				[set setBoolValue:( value >= 0.5f ) toParameter:target atTime:time];
			else
				[set setFloatValue:value toParameter:target atTime:time];
		}
		return YES;
	}

	// A parameter the current preset covers just moved. If it no longer matches
	// the preset, the preset is no longer what you are looking at.
	int current = 0;
	if( ![get getIntValue:&current fromParameter:kParamID_Preset atTime:time] )
		return YES;
	if( current <= 0 || current > porthole::presets::kCount )
		return YES;

	const porthole::presets::Preset& p = porthole::presets::kPresets[ current - 1 ];
	for( int slot = 0; slot < porthole::presets::kParamCount; ++slot )
	{
		if( kPresetParamIDs[ slot ] != paramID )
			continue;

		bool differs = false;
		if( presetSlotIsChoice( slot ) )
		{
			int v = 0;
			[get getIntValue:&v fromParameter:paramID atTime:time];
			differs = v != int( std::lround( p.v[ slot ] ) );
		}
		else if( presetSlotIsToggle( slot ) )
		{
			BOOL v = NO;
			[get getBoolValue:&v fromParameter:paramID atTime:time];
			differs = ( v ? 1 : 0 ) != ( p.v[ slot ] >= 0.5f ? 1 : 0 );
		}
		else
		{
			double v = 0.0;
			[get getFloatValue:&v fromParameter:paramID atTime:time];
			differs = std::fabs( v - p.v[ slot ] ) > 1e-4;
		}

		if( differs )
			[set setIntValue:0 toParameter:kParamID_Preset atTime:time];
		break;
	}

	return YES;
}

// ---------------------------------------------------------------------- render

- (BOOL)pluginState:(NSData* _Nonnull* _Nullable)pluginState
			 atTime:(CMTime)renderTime
			quality:(FxQuality)qualityLevel
			  error:(NSError**)error
{
	id<FxParameterRetrievalAPI_v6> params =
		[_apiManager apiForProtocol:@protocol( FxParameterRetrievalAPI_v6 )];
	if( params == nil )
	{
		if( error != NULL )
			*error = [NSError errorWithDomain:FxPlugErrorDomain
										 code:kFxError_APIUnavailable
									 userInfo:@{ NSLocalizedDescriptionKey :
												 @"Porthole: no parameter retrieval API" }];
		return NO;
	}

	double projection = 0.0, fov = 0.5, chromatic = 0.0, centreX = 0.5, centreY = 0.5, zoom = 0.5;
	BOOL defish = NO;
	int fit = 0, edges = 0, quality = 1;

	BOOL ok = YES;
	ok = ok && [params getFloatValue:&projection fromParameter:kParamID_Projection atTime:renderTime];
	ok = ok && [params getFloatValue:&fov        fromParameter:kParamID_Fov        atTime:renderTime];
	ok = ok && [params getBoolValue:&defish      fromParameter:kParamID_Defish     atTime:renderTime];
	ok = ok && [params getFloatValue:&chromatic  fromParameter:kParamID_Chromatic  atTime:renderTime];
	ok = ok && [params getIntValue:&fit          fromParameter:kParamID_Fit        atTime:renderTime];
	ok = ok && [params getFloatValue:&centreX    fromParameter:kParamID_CentreX    atTime:renderTime];
	ok = ok && [params getFloatValue:&centreY    fromParameter:kParamID_CentreY    atTime:renderTime];
	ok = ok && [params getFloatValue:&zoom       fromParameter:kParamID_Zoom       atTime:renderTime];
	ok = ok && [params getIntValue:&edges        fromParameter:kParamID_Edges      atTime:renderTime];
	ok = ok && [params getIntValue:&quality      fromParameter:kParamID_Quality    atTime:renderTime];

	if( !ok )
	{
		if( error != NULL )
			*error = [NSError errorWithDomain:FxPlugErrorDomain
										 code:kFxError_InvalidParameter
									 userInfo:@{ NSLocalizedDescriptionKey :
												 @"Porthole: could not read parameters" }];
		return NO;
	}

	// The same conversions the FFGL and OpenFX builds use — the host side is
	// unitless and Projection.cpp owns the physical meaning.
	WarpTileState state;
	state.curve     = porthole::curveFromParam( float( projection ) );
	state.thetaMax  = porthole::thetaMaxFromParam( float( fov ) );
	state.chromatic = porthole::chromaticFromParam( float( chromatic ) );
	state.centreX   = centreX - 0.5;
	state.centreY   = centreY - 0.5;
	state.zoom      = porthole::zoomFromParam( float( zoom ) );
	state.fit       = fit;
	state.edges     = edges;
	state.taps      = porthole::tapsFromParam( float( quality ) );
	state.defish    = defish ? 1u : 0u;

	*pluginState = [NSData dataWithBytes:&state length:sizeof( state )];
	return YES;
}

- (BOOL)destinationImageRect:(FxRect*)destinationImageRect
				sourceImages:(NSArray<FxImageTile*>*)sourceImages
			destinationImage:(FxImageTile*)destinationImage
				 pluginState:(nullable NSData*)pluginState
					  atTime:(CMTime)renderTime
					   error:(NSError**)outError
{
	// The frame stays full: warpRadius(1) == 1 always, so the picture is
	// redistributed inside its own bounds and never grows.
	if( sourceImages.count > 0 )
		*destinationImageRect = sourceImages[ 0 ].imagePixelBounds;
	else
		*destinationImageRect = destinationImage.imagePixelBounds;
	return YES;
}

- (BOOL)sourceTileRect:(FxRect*)sourceTileRect
	  sourceImageIndex:(NSUInteger)sourceImageIndex
		  sourceImages:(NSArray<FxImageTile*>*)sourceImages
   destinationTileRect:(FxRect)destinationTileRect
	  destinationImage:(FxImageTile*)destinationImage
		   pluginState:(nullable NSData*)pluginState
				atTime:(CMTime)renderTime
				 error:(NSError**)outError
{
	// The whole picture, not the matching tile: a lens reaches anywhere, and
	// asking for the tile would sample black wherever it reaches outside.
	if( sourceImages.count > sourceImageIndex )
		*sourceTileRect = sourceImages[ sourceImageIndex ].imagePixelBounds;
	else
		*sourceTileRect = destinationTileRect;
	return YES;
}

- (BOOL)renderDestinationImage:(FxImageTile*)destinationImage
				  sourceImages:(NSArray<FxImageTile*>*)sourceImages
				   pluginState:(nullable NSData*)pluginState
						atTime:(CMTime)renderTime
						 error:(NSError**)outError
{
	if( pluginState == nil || pluginState.length != sizeof( WarpTileState ) )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kFxError_InvalidParameter
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Porthole: bad plug-in state" }];
		return NO;
	}

	WarpTileState state;
	[pluginState getBytes:&state length:sizeof( state )];

	if( sourceImages.count < 1 )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kFxError_InvalidParameter
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Porthole: no source image" }];
		return NO;
	}

	FxImageTile* source = sourceImages[ 0 ];

	IOSurfaceRef srcSurface = (__bridge IOSurfaceRef)source.ioSurface;
	IOSurfaceRef dstSurface = (__bridge IOSurfaceRef)destinationImage.ioSurface;
	if( srcSurface == NULL || dstSurface == NULL )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kFxError_InvalidParameter
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Porthole: image tile carried no surface" }];
		return NO;
	}

	const Layout srcLayout = layoutForSurface( srcSurface );
	const Layout dstLayout = layoutForSurface( dstSurface );
	if( srcLayout == Layout::Unsupported || dstLayout == Layout::Unsupported )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kPortholeError_UnsupportedPixelFormat
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Porthole: unsupported pixel format" }];
		return NO;
	}

	IOSurfaceLock( srcSurface, kIOSurfaceLockReadOnly, NULL );
	IOSurfaceLock( dstSurface, 0, NULL );

	const FxRect srcBounds = source.tilePixelBounds;
	const FxRect dstTile   = destinationImage.tilePixelBounds;
	const FxRect dstImage  = destinationImage.imagePixelBounds;

	const porthole::SourceImage src(
		static_cast<const uint8_t*>( IOSurfaceGetBaseAddress( srcSurface ) ),
		IOSurfaceGetBytesPerRow( srcSurface ), srcLayout,
		int( srcBounds.right - srcBounds.left ),
		int( srcBounds.top - srcBounds.bottom ) );

	// Where this tile sits in the full picture. NeedsFullBuffer means it is
	// normally the whole thing, but the warp is defined over the image and a
	// sub-tile must still land in the right place.
	porthole::warpTile( src,
						static_cast<uint8_t*>( IOSurfaceGetBaseAddress( dstSurface ) ),
						IOSurfaceGetBytesPerRow( dstSurface ), dstLayout,
						int( dstTile.left - dstImage.left ),
						int( dstTile.bottom - dstImage.bottom ),
						int( dstTile.right - dstTile.left ),
						int( dstTile.top - dstTile.bottom ),
						int( dstImage.right - dstImage.left ),
						int( dstImage.top - dstImage.bottom ),
						1.0,
						state );

	IOSurfaceUnlock( dstSurface, 0, NULL );
	IOSurfaceUnlock( srcSurface, kIOSurfaceLockReadOnly, NULL );

	return YES;
}

@end

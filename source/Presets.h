#pragma once

/**
    Factory presets: named lens setups an operator can reach in one gesture.

    Both hosts already let a user save their own presets, so what belongs in
    the plugin is the curated set — each entry here is a recognisable optic,
    not a random collection of slider positions.

    The values live in the same 0..1 parameter space both builds expose (the
    FFGL and OFX builds deliberately share it), so ONE table drives both and a
    preset looks identical in Resolume and Resolve. Kept as a header of plain
    data: no logic here, the application machinery lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it is not a preset, it means "the sliders are the truth".

    A preset covers the *look* parameters only. Centre X/Y are framing and
    Quality is a performance choice; a preset that reset those would be
    fighting the operator rather than helping.
*/

namespace porthole
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kProjection,
	kFieldOfView,
	kDefish,
	kChromatic,
	kFit,
	kZoom,
	kEdges,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Values are host-parameter values: Projection 0 is rectilinear, 0.5
// equidistant, 1 orthographic (k = 1 - 2p); Field of View spans 0..~90
// degrees at the reference radius; Zoom 0.5 is 1:1; option params hold the
// element index. See Projection.cpp for the mappings.
inline constexpr Preset kPresets[] = {
	//                       Proj   FoV  Defish Chrom  Fit  Zoom  Edges
	{ "Classic Fisheye", { 0.50f, 0.60f, 0.0f, 0.08f, 0.0f, 0.5f, 0.0f } },  //equidistant, the textbook fish
	{ "Full-Frame Fisheye", { 0.75f, 0.70f, 0.0f, 0.10f, 2.0f, 0.5f, 0.0f } },//equisolid fitted to the height, edges intact
	{ "Action Cam", { 0.75f, 0.55f, 0.0f, 0.15f, 0.0f, 0.5f, 0.0f } },       //equisolid wide, the GoPro look
	{ "Little Planet", { 0.25f, 0.95f, 0.0f, 0.10f, 2.0f, 0.5f, 4.0f } },    //stereographic at full width, wrapped
	{ "Mirror Ball", { 1.00f, 1.00f, 0.0f, 0.20f, 2.0f, 0.5f, 0.0f } },      //orthographic: the scene on a sphere
	{ "Defish Action Cam", { 0.75f, 0.55f, 1.0f, 0.00f, 0.0f, 0.5f, 2.0f } },//the exact inverse of Action Cam
	{ "Cheap Plastic Lens", { 0.60f, 0.35f, 0.0f, 1.00f, 0.0f, 0.5f, 2.0f } },//mild bulge, heavy fringing
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace porthole

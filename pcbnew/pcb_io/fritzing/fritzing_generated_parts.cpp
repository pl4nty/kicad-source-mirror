/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "fritzing_generated_parts.h"

#include <wx/crt.h>
#include <wx/tokenzr.h>

using namespace FRITZING;


/// Parse a Fritzing spacing such as "100mil", "300mil" or "2.54mm" into mils.
static bool parseSpacingMils( const wxString& aSpacing, double& aMils )
{
    double value;

    if( aSpacing.EndsWith( wxS( "mil" ) ) && aSpacing.BeforeLast( 'm' ).ToCDouble( &value ) )
        aMils = value;
    else if( aSpacing.EndsWith( wxS( "mm" ) ) && aSpacing.BeforeLast( 'm' ).BeforeLast( 'm' ).ToCDouble( &value ) )
        aMils = value / 0.0254;
    else if( aSpacing.EndsWith( wxS( "in" ) ) && aSpacing.BeforeLast( 'i' ).ToCDouble( &value ) )
        aMils = value * 1000.0;
    else
        return false;

    return aMils > 0;
}


/// Fritzing's TextUtils::getPinsAndSpacing: the first numeric piece is the pin count, the
/// next piece starting with a digit is the spacing.
static int getPinsAndSpacing( const wxString& aModuleId, double& aSpacingMils )
{
    wxArrayString pieces = wxStringTokenize( aModuleId, wxS( "_" ), wxTOKEN_RET_EMPTY_ALL );
    size_t        pix = 0;
    long          pins = 0;

    while( pix < pieces.size() && !pieces[pix].ToLong( &pins ) )
        pix++;

    if( pix >= pieces.size() || pins <= 0 || pins > 1000 )
        return 0;

    aSpacingMils = 100.0;

    for( ++pix; pix < pieces.size(); pix++ )
    {
        if( !pieces[pix].IsEmpty() && wxIsdigit( pieces[pix][0] ) )
        {
            parseSpacingMils( pieces[pix], aSpacingMils );
            break;
        }
    }

    return static_cast<int>( pins );
}


static wxString num( double aValue )
{
    return wxString::FromCDouble( aValue );
}


static wxString circle( double aX, double aY, int aIndex, double aRadius )
{
    return wxString::Format( wxS( "<circle cx='%s' cy='%s' fill='none' id='connector%dpin' r='%s' "
                                  "stroke='rgb(255, 191, 0)' stroke-width='20'/>\n" ),
                             num( aX ), num( aY ), aIndex, num( aRadius ) );
}


static wxString silkLine( double aX1, double aY1, double aX2, double aY2, double aWidth )
{
    return wxString::Format( wxS( "<line stroke='black' stroke-width='%s' x1='%s' x2='%s' y1='%s' y2='%s'/>\n" ),
                             num( aWidth ), num( aX1 ), num( aX2 ), num( aY1 ), num( aY2 ) );
}


static wxString svgDocument( double aWidth, double aHeight, const wxString& aSilk, const wxString& aCopper )
{
    // Units are mils
    return wxString::Format( wxS( "<?xml version='1.0' encoding='UTF-8'?>\n"
                                  "<svg xmlns='http://www.w3.org/2000/svg' width='%sin' height='%sin' "
                                  "viewBox='0 0 %s %s'>\n"
                                  "<g id='silkscreen'>\n%s</g>\n"
                                  "<g id='copper1'><g id='copper0'>\n%s</g></g>\n"
                                  "</svg>\n" ),
                             num( aWidth / 1000.0 ), num( aHeight / 1000.0 ), num( aWidth ), num( aHeight ),
                             aSilk, aCopper );
}


/// PinHeader::makePcbSvg, for plain single- and double-row through-hole headers and SIPs.
static wxString makeHeaderSvg( int aPins, double aSpacing, bool aDouble )
{
    const double outerBorder = 15;
    const double innerBorder = outerBorder / 2;
    const double silkStrokeWidth = 10;
    const double standardRadius = 27.5;
    const double radius = 29;
    const double copperStrokeWidth = 20;

    double totalWidth = ( outerBorder * 2 ) + ( silkStrokeWidth * 2 ) + ( innerBorder * 2 ) + ( standardRadius * 2 )
                        + copperStrokeWidth;
    double center = totalWidth / 2;
    double originalTotalWidth = totalWidth;
    double totalHeight = totalWidth + ( aPins * aSpacing ) - aSpacing;

    wxString copper;

    if( aDouble )
    {
        for( int i = 0; i < aPins / 2; i++ )
            copper += circle( center, center + i * aSpacing, i, radius );

        for( int i = aPins / 2; i < aPins; i++ )
            copper += circle( center + 100, center + ( aPins - i - 1 ) * aSpacing, i, radius );

        totalHeight = totalWidth + ( aSpacing * aPins / 2 ) - aSpacing;
        totalWidth += 100;
    }
    else
    {
        for( int i = 0; i < aPins; i++ )
            copper += circle( center, center + i * aSpacing, i, radius );
    }

    double right = totalWidth - outerBorder - ( silkStrokeWidth / 2 );
    double bottom = totalHeight - outerBorder - ( silkStrokeWidth / 2 );
    double notch = originalTotalWidth - outerBorder - ( silkStrokeWidth / 2 );

    wxString silk = silkLine( 20, 20, 20, bottom, silkStrokeWidth ) + silkLine( 20, bottom, right, bottom, silkStrokeWidth )
                    + silkLine( right, bottom, notch, 20, silkStrokeWidth )
                    + silkLine( notch, 20, 20, 20, silkStrokeWidth )
                    + silkLine( 20, 55, 55, 20, silkStrokeWidth / 2 );

    return svgDocument( totalWidth, totalHeight, silk, copper );
}


/// MysteryPart::makePcbDipSvg: two rows, pin 1 top-left, numbered counter-clockwise.
static wxString makeDipSvg( int aPins, double aSpacing )
{
    const double outerBorder = 10;
    const double silkSplitTop = 100;
    const double offsetX = 60;
    const double offsetY = 60;

    double totalWidth = 120 + aSpacing;
    double totalHeight = ( 100 * ( aPins / 2 ) ) + ( outerBorder * 2 );
    double center = totalWidth / 2;
    double right = totalWidth - outerBorder;
    double bottom = totalHeight - outerBorder;

    wxString silk = silkLine( 10, 10, 10, bottom, 10 ) + silkLine( 10, bottom, right, bottom, 10 )
                    + silkLine( right, bottom, right, 10, 10 )
                    + silkLine( 10, 10, center - silkSplitTop / 2, 10, 10 )
                    + silkLine( center + silkSplitTop / 2, 10, right, 10, 10 );

    wxString copper;
    double   y = offsetY;

    for( int i = 0; i < aPins / 2; i++ )
    {
        copper += circle( offsetX, y, i, 27.5 );
        copper += circle( totalWidth - offsetX, y, aPins - 1 - i, 27.5 );
        y += 100;
    }

    return svgDocument( totalWidth, totalHeight, silk, copper );
}


/// ScrewTerminal::makePcbSvg: one column of pins.
static wxString makeScrewTerminalSvg( int aPins, double aSpacing )
{
    double width = aSpacing < 150 ? 256 : 480;
    double verticalX = aSpacing < 150 ? 196 : 380;
    double centerX = ( verticalX / 2 ) + 10;
    double initialY = ( aSpacing / 2 ) + 25;
    double height = aPins * aSpacing + 40;
    double right = width - 20;

    wxString silk = silkLine( 20, 20, 20, height - 10, 10 ) + silkLine( 20, height - 10, right, height - 10, 10 )
                    + silkLine( right, height - 10, right, 20, 10 ) + silkLine( right, 20, 20, 20, 10 )
                    + silkLine( verticalX, height - 10, verticalX, 20, 5 );

    wxString copper;

    for( int i = 0; i < aPins; i++ )
        copper += circle( centerX, initialY + i * aSpacing, i, 30 );

    return svgDocument( width, height, silk, copper );
}


/// Strip a Fritzing "hole size" suffix, which only changes the drill.
static wxString withoutHoleSize( const wxString& aName )
{
    wxString name = aName.Lower();

    if( int hs = name.Find( wxS( "_hs" ) ); hs != wxNOT_FOUND )
        name.Truncate( hs );

    return name;
}


bool GenerateFritzingPcbSvg( const wxString& aImage, std::string& aSvg )
{
    // Fritzing's PartFactory::getSvgFilename picks the generator from the file name.
    wxString name = withoutHoleSize( aImage );
    name.Replace( wxS( "\\" ), wxS( "/" ) );

    if( !name.StartsWith( wxS( "pcb/" ), &name ) )
        return false;

    double spacing = 100.0;
    int    pins = getPinsAndSpacing( name, spacing );

    if( pins <= 0 )
        return false;

    wxString svg;

    if( name.StartsWith( wxS( "dip_" ) ) || name.StartsWith( wxS( "generic_ic_dip_" ) )
        || ( name.StartsWith( wxS( "mystery_part_" ) ) && name.Contains( wxS( "dip" ) ) ) )
    {
        if( pins % 2 )
            return false;

        svg = makeDipSvg( pins, spacing );
    }
    else if( name.StartsWith( wxS( "mystery_part_" ) ) || name.StartsWith( wxS( "generic_sip_" ) )
             || name.StartsWith( wxS( "jumper_" ) ) || name.StartsWith( wxS( "nsjumper_" ) ) )
    {
        // Only one spacing for mystery parts
        if( name.StartsWith( wxS( "mystery_part_" ) ) )
            spacing = 100.0;

        bool isDouble = name.Contains( wxS( "double" ) );

        if( isDouble && pins % 2 )
            return false;

        svg = makeHeaderSvg( pins, spacing, isDouble );
    }
    else if( name.StartsWith( wxS( "screw_terminal_" ) ) )
    {
        svg = makeScrewTerminalSvg( pins, spacing );
    }
    else
    {
        return false;
    }

    aSvg = svg.ToStdString( wxConvUTF8 );
    return true;
}


bool GenerateFritzingPart( const wxString& aModuleId, PART& aPart, std::string& aPcbSvg )
{
    wxString id = withoutHoleSize( aModuleId );

    // Surface-mount and specially shaped headers use other generators.
    for( const wxChar* unsupported : { wxS( "smd" ), wxS( "shrouded" ), wxS( "longpad" ), wxS( "molex" ) } )
    {
        if( id.Contains( unsupported ) )
            return false;
    }

    double   spacing = 100.0;
    int      pins = getPinsAndSpacing( id, spacing );
    wxString spacingName = wxString::Format( wxS( "%smil" ), wxString::FromCDouble( spacing ) );
    wxString image;

    if( pins <= 0 )
        return false;

    if( id.Contains( wxS( "_dip_" ) ) || id.StartsWith( wxS( "generic_ic_dip" ) ) )
        image = wxString::Format( wxS( "pcb/dip_%d_%s_pcb.svg" ), pins, spacingName );
    else if( id.StartsWith( wxS( "mystery_part" ) ) )
        image = wxString::Format( wxS( "pcb/mystery_part_sip_%d_100mil_pcb.svg" ), pins );
    else if( id.StartsWith( wxS( "screw_terminal" ) ) )
        image = wxString::Format( wxS( "pcb/screw_terminal_%d_%s_pcb.svg" ), pins, spacingName );
    else if( id.Contains( wxS( "pin_header" ) ) || id.Contains( wxS( "sip" ) ) )
        image = wxString::Format( wxS( "pcb/jumper_%d_%s%s_pcb.svg" ), pins, spacingName,
                                  id.Contains( wxS( "double" ) ) ? wxS( "_double" ) : wxS( "" ) );
    else
        return false;

    if( !GenerateFritzingPcbSvg( image, aPcbSvg ) )
        return false;

    aPart = PART();
    aPart.moduleId = aModuleId;
    aPart.title = aModuleId;
    aPart.label = image.Contains( wxS( "dip" ) ) ? wxS( "U" ) : wxS( "J" );
    aPart.pcbImage = image;
    aPart.pcbLayers = { wxS( "copper0" ), wxS( "silkscreen" ), wxS( "copper1" ) };

    for( int i = 0; i < pins; i++ )
    {
        PART_CONNECTOR& connector = aPart.connectors.emplace_back();
        connector.id = wxString::Format( wxS( "connector%d" ), i );
        connector.name = wxString::Format( wxS( "pin %d" ), i + 1 );
        connector.pcbSvgIds[wxS( "copper0" )] = connector.id + wxS( "pin" );
        connector.pcbSvgIds[wxS( "copper1" )] = connector.id + wxS( "pin" );
    }

    return true;
}

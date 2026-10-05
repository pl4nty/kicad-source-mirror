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

#include "fritzing_svg.h"
#include "fritzing_model.h"

#include <cmath>
#include <set>

#include <locale_io.h>
#include <nanosvg.h>

#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/tokenzr.h>
#include <wx/xml/xml.h>


static const std::set<wxString> DRAWABLE_TAGS = { wxS( "rect" ),    wxS( "circle" ),   wxS( "ellipse" ),
                                                  wxS( "line" ),    wxS( "polyline" ), wxS( "polygon" ),
                                                  wxS( "path" ) };

/// Elements kept even when outside the extracted subtree, because others may refer to them.
static const std::set<wxString> SHARED_TAGS = { wxS( "defs" ), wxS( "style" ) };


FRITZING_SVG::FRITZING_SVG() = default;


FRITZING_SVG::~FRITZING_SVG() = default;


bool FRITZING_SVG::Load( const std::string& aSvg )
{
    wxLogNull           suppressXmlErrors;
    wxMemoryInputStream stream( aSvg.data(), aSvg.size() );

    m_doc = std::make_unique<wxXmlDocument>();

    // Fritzing's TextUtils::isIllustratorFile
    m_illustrator = aSvg.find( "Generator: Adobe Illustrator" ) != std::string::npos;

    if( !m_doc->Load( stream ) || !m_doc->GetRoot() || m_doc->GetRoot()->GetName() != wxS( "svg" ) )
    {
        m_doc.reset();
        return false;
    }

    return true;
}


bool FRITZING_SVG::ParseLength( const wxString& aLength, double& aInches, bool aIllustrator )
{
    wxString str = aLength;
    str.Trim().Trim( false );

    size_t split = 0;

    while( split < str.length() && ( wxIsdigit( str[split] ) || str[split] == '.' || str[split] == '-'
                                     || str[split] == '+' || str[split] == 'e' || str[split] == 'E' ) )
    {
        // Don't swallow the "e" of an "em"/"ex" unit
        if( ( str[split] == 'e' || str[split] == 'E' ) && split + 1 < str.length()
            && !wxIsdigit( str[split + 1] ) && str[split + 1] != '-' && str[split + 1] != '+' )
        {
            break;
        }

        split++;
    }

    double value;

    if( split == 0 || !str.Left( split ).ToCDouble( &value ) || !std::isfinite( value ) )
        return false;

    wxString unit = str.Mid( split ).Lower();

    if( unit == wxS( "in" ) )
        aInches = value;
    else if( unit == wxS( "mm" ) )
        aInches = value / 25.4;
    else if( unit == wxS( "cm" ) )
        aInches = value / 2.54;
    else if( unit == wxS( "pt" ) )
        aInches = value / 72.0;
    else if( unit == wxS( "pc" ) )
        aInches = value / 6.0;
    else if( unit == wxS( "mil" ) )
        aInches = value / 1000.0;
    else if( unit == wxS( "px" ) )
        aInches = value / ( aIllustrator ? 72.0 : FRITZING::SCENE_DPI );
    else if( unit.IsEmpty() )
        aInches = value / FRITZING::SCENE_DPI;
    else
        return false;

    return true;
}


static bool parseViewBox( const wxXmlNode* aRoot, double aViewBox[4] )
{
    wxString str;

    if( !aRoot->GetAttribute( wxS( "viewBox" ), &str ) )
        return false;

    wxStringTokenizer tokens( str, wxS( " ,\t\r\n" ), wxTOKEN_STRTOK );

    for( int i = 0; i < 4; i++ )
    {
        if( !tokens.HasMoreTokens() || !tokens.GetNextToken().ToCDouble( &aViewBox[i] ) )
            return false;
    }

    return aViewBox[2] > 0 && aViewBox[3] > 0;
}


bool FRITZING_SVG::GetSizeInches( double& aWidth, double& aHeight ) const
{
    if( !m_doc )
        return false;

    const wxXmlNode* root = m_doc->GetRoot();
    double           viewBox[4];
    bool             hasViewBox = parseViewBox( root, viewBox );

    auto getLength =
            [&]( const wxString& aAttr, int aViewBoxIndex, double& aResult )
            {
                wxString str;

                if( root->GetAttribute( aAttr, &str ) && !str.Contains( wxS( "%" ) )
                    && ParseLength( str, aResult, m_illustrator ) && aResult > 0 )
                {
                    return true;
                }

                if( hasViewBox )
                {
                    aResult = viewBox[aViewBoxIndex] / FRITZING::SCENE_DPI;
                    return true;
                }

                return false;
            };

    return getLength( wxS( "width" ), 2, aWidth ) && getLength( wxS( "height" ), 3, aHeight );
}


static wxXmlNode* findById( wxXmlNode* aNode, const wxString& aId )
{
    if( aNode->GetType() != wxXML_ELEMENT_NODE )
        return nullptr;

    if( aNode->GetAttribute( wxS( "id" ) ) == aId )
        return aNode;

    for( wxXmlNode* child = aNode->GetChildren(); child; child = child->GetNext() )
    {
        if( wxXmlNode* found = findById( child, aId ) )
            return found;
    }

    return nullptr;
}


bool FRITZING_SVG::HasElement( const wxString& aId ) const
{
    return m_doc && findById( m_doc->GetRoot(), aId );
}


static bool isHidden( const wxXmlNode* aNode )
{
    if( aNode->GetAttribute( wxS( "display" ) ) == wxS( "none" )
        || aNode->GetAttribute( wxS( "visibility" ) ) == wxS( "hidden" ) )
    {
        return true;
    }

    wxString style = aNode->GetAttribute( wxS( "style" ) );
    style.Replace( wxS( " " ), wxEmptyString );

    return style.Contains( wxS( "display:none" ) );
}


static bool contains( const wxXmlNode* aNode, const wxXmlNode* aTarget )
{
    for( const wxXmlNode* node = aTarget; node; node = node->GetParent() )
    {
        if( node == aNode )
            return true;
    }

    return false;
}


/**
 * Remove everything that is neither the target, inside it, an ancestor of it, nor shared
 * definitions.  When aTags is given, give each drawable element of the target a unique id
 * "fz-N" and record its element name at index N.
 */
static void prune( wxXmlNode* aNode, const wxXmlNode* aTarget, bool aInsideTarget,
                   std::vector<wxString>* aTags )
{
    wxXmlNode* child = aNode->GetChildren();

    while( child )
    {
        wxXmlNode* next = child->GetNext();

        if( child->GetType() == wxXML_ELEMENT_NODE )
        {
            bool inside = aInsideTarget || child == aTarget;

            if( isHidden( child ) )
            {
                aNode->RemoveChild( child );
                delete child;
            }
            else if( inside || contains( child, aTarget ) )
            {
                if( inside && aTags && DRAWABLE_TAGS.count( child->GetName() ) )
                {
                    wxString id = wxString::Format( wxS( "fz-%zu" ), aTags->size() );
                    aTags->push_back( child->GetName() );

                    if( child->HasAttribute( wxS( "id" ) ) )
                        child->DeleteAttribute( wxS( "id" ) );

                    child->AddAttribute( wxS( "id" ), id );
                }

                prune( child, aTarget, inside, aTags );
            }
            else if( !SHARED_TAGS.count( child->GetName() ) )
            {
                aNode->RemoveChild( child );
                delete child;
            }
        }

        child = next;
    }
}


static void setAttribute( wxXmlNode* aNode, const wxString& aName, const wxString& aValue )
{
    if( aNode->HasAttribute( aName ) )
        aNode->DeleteAttribute( aName );

    aNode->AddAttribute( aName, aValue );
}


static std::string extractDoc( const wxXmlDocument& aDoc, double aWidthIn, double aHeightIn,
                               const wxString& aId, double aWidthMM, double aHeightMM,
                               std::vector<wxString>* aTags )
{
    wxXmlDocument doc( aDoc );
    wxXmlNode*    root = doc.GetRoot();
    wxXmlNode*    target = aId.IsEmpty() ? root : findById( root, aId );

    if( !target )
        return std::string();

    if( target == root && aTags && DRAWABLE_TAGS.count( root->GetName() ) )
        aTags->push_back( root->GetName() );

    prune( root, target, target == root, aTags );

    double viewBox[4];

    if( !parseViewBox( root, viewBox ) )
    {
        setAttribute( root, wxS( "viewBox" ),
                      wxString::Format( wxS( "0 0 %s %s" ),
                                        wxString::FromCDouble( aWidthIn * FRITZING::SCENE_DPI ),
                                        wxString::FromCDouble( aHeightIn * FRITZING::SCENE_DPI ) ) );
    }

    if( aWidthMM > 0 && aHeightMM > 0 )
    {
        setAttribute( root, wxS( "width" ), wxString::FromCDouble( aWidthMM ) + wxS( "mm" ) );
        setAttribute( root, wxS( "height" ), wxString::FromCDouble( aHeightMM ) + wxS( "mm" ) );
    }
    else
    {
        setAttribute( root, wxS( "width" ), wxString::FromCDouble( aWidthIn ) + wxS( "in" ) );
        setAttribute( root, wxS( "height" ), wxString::FromCDouble( aHeightIn ) + wxS( "in" ) );
    }

    // nanosvg would otherwise honour the aspect ratio rather than our explicit size.
    setAttribute( root, wxS( "preserveAspectRatio" ), wxS( "none" ) );

    wxMemoryOutputStream out;

    if( !doc.Save( out, wxXML_NO_INDENTATION ) )
        return std::string();

    std::string result( out.GetLength(), '\0' );
    out.CopyTo( result.data(), result.size() );
    return result;
}


std::string FRITZING_SVG::Extract( const wxString& aId, double aWidthMM, double aHeightMM ) const
{
    double width, height;

    if( !GetSizeInches( width, height ) )
        return std::string();

    return extractDoc( *m_doc, width, height, aId, aWidthMM, aHeightMM, nullptr );
}


std::vector<FRITZING_SVG::PAD_PRIMITIVE> FRITZING_SVG::GetPrimitives( const wxString& aId ) const
{
    std::vector<PAD_PRIMITIVE> result;
    std::vector<wxString>      tags;
    double                     width, height;

    if( !GetSizeInches( width, height ) )
        return result;

    std::string svg = extractDoc( *m_doc, width, height, aId, 0.0, 0.0, &tags );

    if( svg.empty() )
        return result;

    LOCALE_IO   toggle;
    NSVGimage*  image = nsvgParse( svg.data(), "mm", 96 );

    if( !image )
        return result;

    for( NSVGshape* shape = image->shapes; shape; shape = shape->next )
    {
        if( !( shape->flags & NSVG_FLAGS_VISIBLE ) )
            continue;

        PAD_PRIMITIVE prim;
        unsigned long index;
        wxString      id = wxString::FromUTF8( shape->id );

        if( id.StartsWith( wxS( "fz-" ) ) && id.Mid( 3 ).ToULong( &index ) && index < tags.size() )
            prim.tag = tags[index];

        prim.bounds = BOX2D( VECTOR2D( shape->bounds[0], shape->bounds[1] ),
                             VECTOR2D( shape->bounds[2] - shape->bounds[0],
                                       shape->bounds[3] - shape->bounds[1] ) );
        prim.strokeWidth = shape->stroke.type != NSVG_PAINT_NONE ? shape->strokeWidth : 0.0;
        prim.filled = shape->fill.type != NSVG_PAINT_NONE;

        result.push_back( prim );
    }

    nsvgDelete( image );
    return result;
}

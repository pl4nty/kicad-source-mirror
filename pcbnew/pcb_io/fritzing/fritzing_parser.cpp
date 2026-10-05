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

#include "fritzing_parser.h"

#include <cmath>
#include <memory>

#include <ki_exception.h>
#include <math/util.h>
#include <reporter.h>

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/translation.h>
#include <wx/wfstream.h>
#include <wx/xml/xml.h>
#include <wx/zipstrm.h>

using namespace FRITZING;


double TRANSFORM::RotationDegrees() const
{
    // Remove a horizontal mirror (x -> -x applied first) before reading the rotation.
    double a = IsMirrored() ? -m11 : m11;
    double b = IsMirrored() ? -m12 : m12;

    return std::atan2( b, a ) * 180.0 / M_PI;
}


static double attrDouble( const wxXmlNode* aNode, const wxString& aName, double aDefault = 0.0 )
{
    wxString value;
    double   result;

    if( aNode->GetAttribute( aName, &value ) && value.Trim().Trim( false ).ToCDouble( &result )
        && std::isfinite( result ) )
    {
        return result;
    }

    return aDefault;
}


static wxXmlNode* findChild( const wxXmlNode* aNode, const wxString& aName )
{
    for( wxXmlNode* child = aNode->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() == wxXML_ELEMENT_NODE && child->GetName() == aName )
            return child;
    }

    return nullptr;
}


static bool loadXml( const std::string& aXml, wxXmlDocument& aDoc )
{
    wxLogNull           suppressXmlErrors;
    wxMemoryInputStream stream( aXml.data(), aXml.size() );

    return aDoc.Load( stream ) && aDoc.GetRoot();
}


bool FRITZING_PARSER::ReadFile( const wxString& aFileName, std::string& aContents, size_t aMaxBytes )
{
    wxFFileInputStream stream( aFileName );

    if( !stream.IsOk() )
        return false;

    aContents.clear();

    char buffer[65536];

    while( !stream.Eof() && ( aMaxBytes == 0 || aContents.size() < aMaxBytes ) )
    {
        stream.Read( buffer, sizeof( buffer ) );
        size_t got = stream.LastRead();

        if( got == 0 )
            break;

        aContents.append( buffer, got );
    }

    if( aMaxBytes && aContents.size() > aMaxBytes )
        aContents.resize( aMaxBytes );

    return true;
}


bool FRITZING_PARSER::ReadArchive( const wxString& aFileName, std::map<wxString, std::string>& aFiles )
{
    wxLogNull          suppressZipErrors;
    wxFFileInputStream in( aFileName );

    if( !in.IsOk() )
        return false;

    wxZipInputStream zip( in );

    if( !zip.IsOk() )
        return false;

    std::unique_ptr<wxZipEntry> entry;
    bool                        any = false;

    while( entry.reset( zip.GetNextEntry() ), entry )
    {
        if( entry->IsDir() )
            continue;

        std::string data;
        char        buffer[65536];

        while( zip.CanRead() )
        {
            zip.Read( buffer, sizeof( buffer ) );
            size_t got = zip.LastRead();

            if( got == 0 )
                break;

            data.append( buffer, got );
        }

        aFiles[wxFileName( entry->GetName() ).GetFullName()] = std::move( data );
        any = true;
    }

    return any;
}


bool FRITZING_PARSER::Sniff( const wxString& aFileName )
{
    wxFileName fn( aFileName );
    wxString   ext = fn.GetExt().Lower();

    auto looksLikeSketch =
            []( const std::string& aHead )
            {
                return aHead.find( "<module" ) != std::string::npos
                       && aHead.find( "fritzingVersion" ) != std::string::npos;
            };

    if( ext == wxS( "fz" ) )
    {
        std::string head;
        return ReadFile( aFileName, head, 4096 ) && looksLikeSketch( head );
    }

    if( ext == wxS( "fzz" ) )
    {
        wxLogNull          suppressZipErrors;
        wxFFileInputStream in( aFileName );

        if( !in.IsOk() )
            return false;

        wxZipInputStream            zip( in );
        std::unique_ptr<wxZipEntry> entry;

        if( !zip.IsOk() )
            return false;

        while( entry.reset( zip.GetNextEntry() ), entry )
        {
            if( entry->GetName().Lower().EndsWith( wxS( ".fz" ) ) )
                return true;
        }
    }

    return false;
}


bool FRITZING_PARSER::ParseSketchFile( const wxString& aFileName, SKETCH& aSketch )
{
    wxFileName  fn( aFileName );
    std::string xml;

    if( fn.GetExt().Lower() == wxS( "fzz" ) )
    {
        std::map<wxString, std::string> files;

        if( !ReadArchive( aFileName, files ) )
            THROW_IO_ERRORF( _( "Could not read Fritzing archive '%s'." ), aFileName );

        // Prefer the sketch named after the archive; fall back to the first one found.
        for( auto& [name, data] : files )
        {
            if( !name.Lower().EndsWith( wxS( ".fz" ) ) )
                continue;

            if( xml.empty() || wxFileName( name ).GetName() == fn.GetName() )
                xml = data;
        }

        if( xml.empty() )
            return false;

        for( auto& [name, data] : files )
        {
            if( !name.Lower().EndsWith( wxS( ".fz" ) ) )
                aSketch.bundledFiles[name] = std::move( data );
        }
    }
    else if( !ReadFile( aFileName, xml ) )
    {
        THROW_IO_ERRORF( _( "Could not read file '%s'." ), aFileName );
    }

    return ParseSketch( xml, aSketch );
}


bool FRITZING_PARSER::ParseSketch( const std::string& aXml, SKETCH& aSketch )
{
    wxXmlDocument doc;

    if( !loadXml( aXml, doc ) )
        return false;

    wxXmlNode* root = doc.GetRoot();

    if( root->GetName() != wxS( "module" ) )
        return false;

    aSketch.fritzingVersion = root->GetAttribute( wxS( "fritzingVersion" ) );

    wxXmlNode* instances = findChild( root, wxS( "instances" ) );

    if( !instances )
        return true;

    for( wxXmlNode* node = instances->GetChildren(); node; node = node->GetNext() )
    {
        if( node->GetType() != wxXML_ELEMENT_NODE || node->GetName() != wxS( "instance" ) )
            continue;

        INSTANCE& instance = aSketch.instances.emplace_back();
        parseInstance( node, instance );
    }

    return true;
}


void FRITZING_PARSER::parseInstance( wxXmlNode* aNode, INSTANCE& aInstance )
{
    aInstance.moduleIdRef = aNode->GetAttribute( wxS( "moduleIdRef" ) );
    aInstance.modelIndex = aNode->GetAttribute( wxS( "modelIndex" ) );
    aInstance.path = aNode->GetAttribute( wxS( "path" ) );

    for( wxXmlNode* child = aNode->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() != wxXML_ELEMENT_NODE )
            continue;

        if( child->GetName() == wxS( "property" ) )
        {
            aInstance.properties[child->GetAttribute( wxS( "name" ) )] =
                    child->GetAttribute( wxS( "value" ) );
        }
        else if( child->GetName() == wxS( "title" ) )
        {
            aInstance.title = child->GetNodeContent().Trim().Trim( false );
        }
        else if( child->GetName() == wxS( "views" ) )
        {
            for( wxXmlNode* view = child->GetChildren(); view; view = view->GetNext() )
            {
                if( view->GetType() == wxXML_ELEMENT_NODE )
                    parseView( view, aInstance.views[view->GetName()] );
            }
        }
    }
}


void FRITZING_PARSER::parseView( wxXmlNode* aNode, VIEW& aView )
{
    aView.layer = aNode->GetAttribute( wxS( "layer" ) );
    aView.bottom = aNode->GetAttribute( wxS( "bottom" ) ) == wxS( "true" );

    if( wxXmlNode* geometry = findChild( aNode, wxS( "geometry" ) ) )
    {
        aView.pos = VECTOR2D( attrDouble( geometry, wxS( "x" ) ), attrDouble( geometry, wxS( "y" ) ) );
        aView.lineStart = VECTOR2D( attrDouble( geometry, wxS( "x1" ) ), attrDouble( geometry, wxS( "y1" ) ) );
        aView.lineEnd = VECTOR2D( attrDouble( geometry, wxS( "x2" ) ), attrDouble( geometry, wxS( "y2" ) ) );
        aView.wireFlags = KiROUND( attrDouble( geometry, wxS( "wireFlags" ) ) );

        if( wxXmlNode* transform = findChild( geometry, wxS( "transform" ) ) )
        {
            aView.transform.m11 = attrDouble( transform, wxS( "m11" ), 1.0 );
            aView.transform.m12 = attrDouble( transform, wxS( "m12" ) );
            aView.transform.m21 = attrDouble( transform, wxS( "m21" ) );
            aView.transform.m22 = attrDouble( transform, wxS( "m22" ), 1.0 );
            aView.transform.dx = attrDouble( transform, wxS( "m31" ) );
            aView.transform.dy = attrDouble( transform, wxS( "m32" ) );
        }
    }

    if( wxXmlNode* extras = findChild( aNode, wxS( "wireExtras" ) ) )
    {
        aView.wireMils = attrDouble( extras, wxS( "mils" ) );

        // Curved wires and traces carry their control points here.
        wxXmlNode* bezier = findChild( extras, wxS( "bezier" ) );
        wxXmlNode* cp0 = bezier ? findChild( bezier, wxS( "cp0" ) ) : nullptr;
        wxXmlNode* cp1 = bezier ? findChild( bezier, wxS( "cp1" ) ) : nullptr;

        if( cp0 && cp1 )
        {
            aView.curved = true;
            aView.bezierCp0 = VECTOR2D( attrDouble( cp0, wxS( "x" ) ), attrDouble( cp0, wxS( "y" ) ) );
            aView.bezierCp1 = VECTOR2D( attrDouble( cp1, wxS( "x" ) ), attrDouble( cp1, wxS( "y" ) ) );
        }
    }

    if( wxXmlNode* title = findChild( aNode, wxS( "titleGeometry" ) ) )
    {
        aView.titleVisible = title->GetAttribute( wxS( "visible" ) ) == wxS( "true" );
        aView.titlePos = VECTOR2D( attrDouble( title, wxS( "x" ) ), attrDouble( title, wxS( "y" ) ) );
        aView.titleFontSize = attrDouble( title, wxS( "fontSize" ) );
    }

    if( wxXmlNode* connectors = findChild( aNode, wxS( "connectors" ) ) )
    {
        for( wxXmlNode* conn = connectors->GetChildren(); conn; conn = conn->GetNext() )
        {
            if( conn->GetType() != wxXML_ELEMENT_NODE || conn->GetName() != wxS( "connector" ) )
                continue;

            std::vector<CONNECT>& list = aView.connectors[conn->GetAttribute( wxS( "connectorId" ) )];

            wxXmlNode* connects = findChild( conn, wxS( "connects" ) );

            if( !connects )
                continue;

            for( wxXmlNode* c = connects->GetChildren(); c; c = c->GetNext() )
            {
                if( c->GetType() != wxXML_ELEMENT_NODE || c->GetName() != wxS( "connect" ) )
                    continue;

                list.push_back( { c->GetAttribute( wxS( "connectorId" ) ),
                                  c->GetAttribute( wxS( "modelIndex" ) ),
                                  c->GetAttribute( wxS( "layer" ) ) } );
            }
        }
    }
}


bool FRITZING_PARSER::ParsePart( const std::string& aXml, PART& aPart )
{
    wxXmlDocument doc;

    if( !loadXml( aXml, doc ) )
        return false;

    wxXmlNode* root = doc.GetRoot();

    if( root->GetName() != wxS( "module" ) )
        return false;

    aPart.moduleId = root->GetAttribute( wxS( "moduleId" ) );

    for( wxXmlNode* child = root->GetChildren(); child; child = child->GetNext() )
    {
        if( child->GetType() != wxXML_ELEMENT_NODE )
            continue;

        const wxString& name = child->GetName();

        if( name == wxS( "title" ) )
        {
            aPart.title = child->GetNodeContent().Trim().Trim( false );
        }
        else if( name == wxS( "label" ) )
        {
            aPart.label = child->GetNodeContent().Trim().Trim( false );
        }
        else if( name == wxS( "properties" ) )
        {
            for( wxXmlNode* prop = child->GetChildren(); prop; prop = prop->GetNext() )
            {
                if( prop->GetType() == wxXML_ELEMENT_NODE && prop->GetName() == wxS( "property" ) )
                {
                    aPart.properties[prop->GetAttribute( wxS( "name" ) ).Lower()] =
                            prop->GetNodeContent().Trim().Trim( false );
                }
            }
        }
        else if( name == wxS( "views" ) )
        {
            wxXmlNode* pcbView = findChild( child, wxS( "pcbView" ) );
            wxXmlNode* layers = pcbView ? findChild( pcbView, wxS( "layers" ) ) : nullptr;

            if( layers )
            {
                aPart.pcbImage = layers->GetAttribute( wxS( "image" ) );

                for( wxXmlNode* layer = layers->GetChildren(); layer; layer = layer->GetNext() )
                {
                    if( layer->GetType() == wxXML_ELEMENT_NODE && layer->GetName() == wxS( "layer" ) )
                        aPart.pcbLayers.push_back( layer->GetAttribute( wxS( "layerId" ) ) );
                }
            }
        }
        else if( name == wxS( "connectors" ) )
        {
            for( wxXmlNode* conn = child->GetChildren(); conn; conn = conn->GetNext() )
            {
                if( conn->GetType() != wxXML_ELEMENT_NODE || conn->GetName() != wxS( "connector" ) )
                    continue;

                PART_CONNECTOR& connector = aPart.connectors.emplace_back();
                connector.id = conn->GetAttribute( wxS( "id" ) );
                connector.name = conn->GetAttribute( wxS( "name" ) );

                if( wxXmlNode* desc = findChild( conn, wxS( "description" ) ) )
                    connector.description = desc->GetNodeContent().Trim().Trim( false );

                wxXmlNode* views = findChild( conn, wxS( "views" ) );
                wxXmlNode* pcbView = views ? findChild( views, wxS( "pcbView" ) ) : nullptr;

                if( !pcbView )
                    continue;

                for( wxXmlNode* p = pcbView->GetChildren(); p; p = p->GetNext() )
                {
                    if( p->GetType() != wxXML_ELEMENT_NODE || p->GetName() != wxS( "p" ) )
                        continue;

                    if( p->GetAttribute( wxS( "hybrid" ) ) == wxS( "yes" ) )
                    {
                        connector.hybrid = true;
                        continue;
                    }

                    connector.pcbSvgIds[p->GetAttribute( wxS( "layer" ) )] = p->GetAttribute( wxS( "svgId" ) );
                }
            }
        }
        else if( name == wxS( "buses" ) )
        {
            for( wxXmlNode* bus = child->GetChildren(); bus; bus = bus->GetNext() )
            {
                if( bus->GetType() != wxXML_ELEMENT_NODE || bus->GetName() != wxS( "bus" ) )
                    continue;

                std::vector<wxString> members;

                for( wxXmlNode* m = bus->GetChildren(); m; m = m->GetNext() )
                {
                    if( m->GetType() == wxXML_ELEMENT_NODE && m->GetName() == wxS( "nodeMember" ) )
                        members.push_back( m->GetAttribute( wxS( "connectorId" ) ) );
                }

                if( members.size() > 1 )
                    aPart.buses.push_back( std::move( members ) );
            }
        }
    }

    return !aPart.moduleId.IsEmpty();
}

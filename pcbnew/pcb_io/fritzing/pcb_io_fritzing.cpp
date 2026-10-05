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

#include "pcb_io_fritzing.h"
#include "fritzing_parser.h"
#include "fritzing_part_library.h"
#include "fritzing_svg.h"

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_track.h>
#include <reporter.h>
#include <geometry/shape.h>
#include <lib_id.h>
#include <math/util.h>
#include <import_gfx/graphics_importer_pcbnew.h>
#include <import_gfx/svg_import_plugin.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>

#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/translation.h>

using namespace FRITZING;


/// Fritzing draws via and hole images this many pixels larger than the copper ring.
static constexpr double HOLE_IMAGE_MARGIN_PX = 4.0;

/// Default "hole size" of Fritzing vias, used when the property is missing.
static constexpr double DEFAULT_VIA_HOLE_MM = 0.4;
static constexpr double DEFAULT_VIA_RING_MM = 0.3;

/// Fallback board outline width.
static constexpr double EDGE_WIDTH_MM = 0.1;


PCB_IO_FRITZING::PCB_IO_FRITZING() :
        PCB_IO( wxS( "Fritzing" ) )
{
}


PCB_IO_FRITZING::~PCB_IO_FRITZING()
{
    for( std::unique_ptr<FOOTPRINT>& fp : m_libFootprints | std::views::values )
    {
        if( fp )
            fp->SetParent( nullptr );
    }
}


bool PCB_IO_FRITZING::CanReadBoard( const wxString& aFileName ) const
{
    if( !PCB_IO::CanReadBoard( aFileName ) )
        return false;

    return FRITZING_PARSER::Sniff( aFileName );
}


std::vector<FOOTPRINT*> PCB_IO_FRITZING::GetImportedCachedLibraryFootprints()
{
    std::vector<FOOTPRINT*> result;

    for( std::unique_ptr<FOOTPRINT>& fp : m_libFootprints | std::views::values )
    {
        if( fp )
            result.push_back( static_cast<FOOTPRINT*>( fp->Clone() ) );
    }

    return result;
}


REPORTER& PCB_IO_FRITZING::reporter() const
{
    return m_reporter ? *m_reporter : NULL_REPORTER::GetInstance();
}


void PCB_IO_FRITZING::loadBoard( const wxString& aFileName, BOARD& aBoard, bool aIsNewLoad,
                                 const std::map<std::string, UTF8>* aProperties, PROJECT* aProject )
{
    m_props = aProperties;
    m_board = &aBoard;

    m_sketch = FRITZING::SKETCH();
    m_parent.clear();
    m_nets.clear();
    m_traceEnds.clear();
    m_libFootprints.clear();
    m_libSizesPx.clear();
    m_unresolvedParts.clear();
    m_padsWithoutGeometry = 0;

    FRITZING_PARSER parser( &reporter() );

    if( !parser.ParseSketchFile( aFileName, m_sketch ) )
        THROW_IO_ERRORF( _( "'%s' is not a valid Fritzing sketch." ), aFileName );

    wxString extraRoots;
    bool     searchDefaults = true;

    if( m_props )
    {
        auto it = m_props->find( FRITZING_PART_LIBRARY::PARTS_PATH_PROPERTY );

        if( it != m_props->end() )
            extraRoots = it->second.wx_str();

        it = m_props->find( FRITZING_PART_LIBRARY::SEARCH_DEFAULTS_PROPERTY );

        if( it != m_props->end() && it->second == "0" )
            searchDefaults = false;
    }

    m_library = std::make_unique<FRITZING_PART_LIBRARY>( m_sketch, wxFileName( aFileName ).GetPath(),
                                                         extraRoots, searchDefaults );

    buildBoard();

    for( const auto& [moduleId, count] : m_unresolvedParts )
    {
        reporter().Report( wxString::Format( _( "Fritzing part '%s' (%d instances) was not found in the "
                                                "parts library; pads were placed at its trace ends "
                                                "only." ),
                                             moduleId, count ),
                           RPT_SEVERITY_WARNING );
    }

    if( !m_unresolvedParts.empty() )
    {
        reporter().Report( wxString::Format( _( "Set the %s environment variable to a copy of the "
                                                "fritzing-parts library to import part footprints." ),
                                             wxString::FromUTF8( FRITZING_PART_LIBRARY::PARTS_PATH_ENV ) ),
                           RPT_SEVERITY_INFO );
    }

    if( m_padsWithoutGeometry > 0 )
    {
        reporter().Report( wxString::Format( _( "%d Fritzing connectors had no pad in their part's PCB "
                                                "image and were skipped." ),
                                             m_padsWithoutGeometry ),
                           RPT_SEVERITY_WARNING );
    }
}


int PCB_IO_FRITZING::pxToIU( double aPx )
{
    return KiROUND( pcbIUScale.mmToIU( aPx * 25.4 / SCENE_DPI ) );
}


int PCB_IO_FRITZING::mmToIU( double aMm )
{
    return KiROUND( pcbIUScale.mmToIU( aMm ) );
}


VECTOR2I PCB_IO_FRITZING::toBoard( const VECTOR2D& aScenePx ) const
{
    VECTOR2D delta = aScenePx - m_originPx;
    return m_originIU + VECTOR2I( pxToIU( delta.x ), pxToIU( delta.y ) );
}


PCB_LAYER_ID PCB_IO_FRITZING::mapLayer( const wxString& aLayer )
{
    if( aLayer == wxS( "copper0" ) || aLayer == wxS( "copper0trace" ) || aLayer == wxS( "groundplane" )
        || aLayer == wxS( "groundplane0" ) )
    {
        return B_Cu;
    }

    if( aLayer == wxS( "copper1" ) || aLayer == wxS( "copper1trace" ) || aLayer == wxS( "groundplane1" ) )
        return F_Cu;

    if( aLayer == wxS( "silkscreen" ) || aLayer == wxS( "silkscreen1" ) )
        return F_SilkS;

    if( aLayer == wxS( "silkscreen0" ) )
        return B_SilkS;

    if( aLayer == wxS( "board" ) )
        return Edge_Cuts;

    return UNDEFINED_LAYER;
}


wxString PCB_IO_FRITZING::padNumber( const wxString& aConnectorId )
{
    // Fritzing connector ids are conventionally "connector<N>", numbered from 0.
    unsigned long index;

    if( aConnectorId.StartsWith( wxS( "connector" ) ) && aConnectorId.Mid( 9 ).ToULong( &index ) )
        return wxString::Format( wxS( "%lu" ), index + 1 );

    return aConnectorId;
}


wxString PCB_IO_FRITZING::findRoot( const wxString& aKey )
{
    wxString root = aKey;

    for( auto it = m_parent.find( root ); it != m_parent.end() && it->second != root; it = m_parent.find( root ) )
        root = it->second;

    // Path compression
    wxString node = aKey;

    while( node != root )
    {
        wxString next = m_parent[node];
        m_parent[node] = root;
        node = next;
    }

    return root;
}


void PCB_IO_FRITZING::unite( const wxString& aA, const wxString& aB )
{
    m_parent.try_emplace( aA, aA );
    m_parent.try_emplace( aB, aB );

    wxString rootA = findRoot( aA );
    wxString rootB = findRoot( aB );

    if( rootA != rootB )
        m_parent[std::max( rootA, rootB )] = std::min( rootA, rootB );
}


NETINFO_ITEM* PCB_IO_FRITZING::netFor( const wxString& aModelIndex, const wxString& aConnectorId )
{
    wxString k = key( aModelIndex, aConnectorId );

    if( !m_parent.count( k ) )
        return nullptr;

    auto it = m_nets.find( findRoot( k ) );
    return it == m_nets.end() ? nullptr : it->second;
}


void PCB_IO_FRITZING::buildNets()
{
    for( const INSTANCE& inst : m_sketch.instances )
    {
        // Fritzing keeps the three views in sync, so a connection drawn in any of them is part
        // of the netlist.
        for( const VIEW& view : inst.views | std::views::values )
        {
            for( const auto& [connectorId, connects] : view.connectors )
            {
                for( const CONNECT& connect : connects )
                    unite( key( inst.modelIndex, connectorId ), key( connect.modelIndex, connect.connectorId ) );
            }
        }

        // A wire is a conductor between its two ends.
        if( inst.moduleIdRef == wxS( "WireModuleID" ) )
            unite( key( inst.modelIndex, wxS( "connector0" ) ), key( inst.modelIndex, wxS( "connector1" ) ) );
    }

    // Connectors joined inside a part (e.g. the strips of a breadboard)
    for( const INSTANCE& inst : m_sketch.instances )
    {
        if( inst.moduleIdRef == wxS( "WireModuleID" ) || inst.path.StartsWith( wxS( ":" ) ) )
            continue;

        if( const PART* part = m_library->FindPart( inst ) )
        {
            for( const std::vector<wxString>& bus : part->buses )
            {
                for( size_t i = 1; i < bus.size(); i++ )
                    unite( key( inst.modelIndex, bus[0] ), key( inst.modelIndex, bus[i] ) );
            }
        }
    }

    // Name the nets.  Net labels and power symbols give names; other nets are named after
    // their first pad, like KiCad's own unnamed nets.
    std::map<wxString, wxString>              labels;      // root -> name
    std::map<wxString, std::vector<wxString>> pads;        // root -> "REF-PadN"
    std::map<wxString, int>                   copperItems; // root -> pads, traces and vias

    for( const INSTANCE& inst : m_sketch.instances )
    {
        const VIEW* pcb = inst.PcbView();
        wxString    moduleId = inst.moduleIdRef;

        auto rootOf =
                [&]( const wxString& aConnectorId ) -> wxString
                {
                    wxString k = key( inst.modelIndex, aConnectorId );
                    return m_parent.count( k ) ? findRoot( k ) : wxString();
                };

        if( moduleId.Contains( wxS( "NetLabel" ) ) || moduleId.Contains( wxS( "PowerLabel" ) ) )
        {
            wxString label = inst.Property( wxS( "label" ) );

            if( label.IsEmpty() )
                label = inst.Property( wxS( "voltage" ) );

            if( wxString root = rootOf( wxS( "connector0" ) ); !root.IsEmpty() && !label.IsEmpty() )
                labels.try_emplace( root, label );

            continue;
        }

        if( moduleId == wxS( "GroundModuleID" ) || moduleId == wxS( "PowerModuleID" )
            || moduleId == wxS( "DCPowerModuleID" ) )
        {
            wxString label = wxS( "GND" );

            if( moduleId != wxS( "GroundModuleID" ) )
            {
                double volts;
                wxString voltage = inst.Property( wxS( "voltage" ) );

                label = voltage.ToCDouble( &volts ) ? wxString::Format( wxS( "+%gV" ), volts ) : wxString( "VCC" );
            }

            for( const VIEW& view : inst.views | std::views::values )
            {
                for( const wxString& connectorId : view.connectors | std::views::keys )
                {
                    if( wxString root = rootOf( connectorId ); !root.IsEmpty() )
                        labels.try_emplace( root, label );
                }
            }

            continue;
        }

        if( !pcb )
            continue;

        if( moduleId == wxS( "WireModuleID" ) )
        {
            if( ( pcb->wireFlags & WIRE_PCB_TRACE ) && IsCopperLayer( mapLayer( pcb->layer ) ) )
            {
                if( wxString root = rootOf( wxS( "connector0" ) ); !root.IsEmpty() )
                    copperItems[root]++;
            }

            continue;
        }

        if( mapLayer( pcb->layer ) == UNDEFINED_LAYER && moduleId != wxS( "HoleModuleID" ) )
            continue;

        // A part's connections may be drawn in another view only (still unrouted on the PCB).
        std::set<wxString> connectorIds;

        for( const VIEW& view : inst.views | std::views::values )
        {
            for( const wxString& connectorId : view.connectors | std::views::keys )
                connectorIds.insert( connectorId );
        }

        for( const wxString& connectorId : connectorIds )
        {
            if( wxString root = rootOf( connectorId ); !root.IsEmpty() )
            {
                copperItems[root]++;

                if( moduleId != wxS( "ViaModuleID" ) && moduleId != wxS( "GroundPlaneModuleID" ) )
                    pads[root].push_back( inst.title + wxS( "-Pad" ) + padNumber( connectorId ) );
            }
        }
    }

    std::set<wxString> roots;

    for( const auto& [k, parent] : m_parent )
        roots.insert( findRoot( k ) );

    std::set<wxString> usedNames;

    for( const wxString& root : roots )
    {
        wxString name;

        if( auto label = labels.find( root ); label != labels.end() )
        {
            name = label->second;
        }
        else if( pads.count( root ) && copperItems[root] > 1 )
        {
            std::vector<wxString>& list = pads[root];
            std::sort( list.begin(), list.end() );
            name = wxS( "Net-(" ) + list.front() + wxS( ")" );
        }
        else
        {
            continue;
        }

        // Labels may name the same net several times; keep names unique per root.
        wxString unique = name;

        for( int i = 1; usedNames.count( unique ) && !labels.count( root ); i++ )
            unique = wxString::Format( wxS( "%s_%d" ), name, i );

        NETINFO_ITEM* net = m_board->FindNet( unique );

        if( !net )
        {
            net = new NETINFO_ITEM( m_board, unique );
            m_board->Add( net );
        }

        usedNames.insert( unique );
        m_nets[root] = net;
    }
}


void PCB_IO_FRITZING::computeOrigin()
{
    m_boardRectPx = BOX2D();

    std::optional<BOX2D> allItems;

    for( const INSTANCE& inst : m_sketch.instances )
    {
        const VIEW* pcb = inst.PcbView();

        if( !pcb || mapLayer( pcb->layer ) == UNDEFINED_LAYER )
            continue;

        // Older board parts (e.g. Arduino shields) sit on the silkscreen layer but carry a
        // "board" layer in their image.
        bool isBoard = pcb->layer == wxS( "board" );

        if( !isBoard && !inst.path.StartsWith( wxS( ":" ) ) && inst.moduleIdRef != wxS( "WireModuleID" ) )
        {
            const PART* part = m_library->FindPart( inst );
            isBoard = part && part->HasPcbLayer( wxS( "board" ) );
        }

        if( isBoard && m_boardRectPx.GetWidth() <= 0 )
        {
            double   width = 0, height = 0;
            wxString w = inst.Property( wxS( "width" ) );
            wxString h = inst.Property( wxS( "height" ) );

            if( w.ToCDouble( &width ) && h.ToCDouble( &height ) )
            {
                width = width / 25.4 * SCENE_DPI;
                height = height / 25.4 * SCENE_DPI;
            }
            else if( const PART* part = m_library->FindPart( inst ) )
            {
                std::string  svgText;
                FRITZING_SVG svg;

                if( m_library->LoadPcbSvg( *part, svgText ) && svg.Load( svgText )
                    && svg.GetSizeInches( width, height ) )
                {
                    width *= SCENE_DPI;
                    height *= SCENE_DPI;
                }
            }

            if( width > 0 && height > 0 )
                m_boardRectPx = BOX2D( pcb->pos, VECTOR2D( width, height ) );
        }

        if( !allItems )
            allItems = BOX2D( pcb->pos, VECTOR2D( 0, 0 ) );
        else
            allItems->Merge( pcb->pos );
    }

    if( m_boardRectPx.GetWidth() > 0 )
        m_originPx = m_boardRectPx.GetOrigin();
    else if( allItems )
        m_originPx = allItems->GetOrigin();
    else
        m_originPx = VECTOR2D();

    // Put the board in the middle of the page when the page size is known.
    m_originIU = VECTOR2I( mmToIU( 25.4 ), mmToIU( 25.4 ) );

    if( m_props && m_boardRectPx.GetWidth() > 0 )
    {
        auto width = m_props->find( "page_width" );
        auto height = m_props->find( "page_height" );

        if( width != m_props->end() && height != m_props->end() )
        {
            long long pageW = std::atoll( width->second.c_str() );
            long long pageH = std::atoll( height->second.c_str() );
            long long boardW = pxToIU( m_boardRectPx.GetWidth() );
            long long boardH = pxToIU( m_boardRectPx.GetHeight() );

            if( pageW > boardW && pageH > boardH )
                m_originIU = VECTOR2I( ( pageW - boardW ) / 2, ( pageH - boardH ) / 2 );
        }
    }
}


void PCB_IO_FRITZING::collectTraceEnds()
{
    for( const INSTANCE& inst : m_sketch.instances )
    {
        const VIEW* pcb = inst.PcbView();

        if( inst.moduleIdRef != wxS( "WireModuleID" ) || !pcb || !( pcb->wireFlags & WIRE_PCB_TRACE ) )
            continue;

        PCB_LAYER_ID layer = mapLayer( pcb->layer );

        if( !IsCopperLayer( layer ) )
            continue;

        for( const auto& [connectorId, connects] : pcb->connectors )
        {
            VECTOR2D end = scenePoint( *pcb, connectorId == wxS( "connector0" ) ? pcb->lineStart : pcb->lineEnd );

            for( const CONNECT& connect : connects )
            {
                if( connect.layer.EndsWith( wxS( "trace" ) ) )
                    continue;   // another wire

                m_traceEnds[key( connect.modelIndex, connect.connectorId )].push_back(
                        { end, layer, pcb->wireMils * SCENE_DPI / 1000.0 } );
            }
        }
    }
}


void PCB_IO_FRITZING::buildBoard()
{
    m_board->SetCopperLayerCount( 2 );

    computeOrigin();
    buildNets();
    collectTraceEnds();

    for( const INSTANCE& inst : m_sketch.instances )
    {
        const VIEW* pcb = inst.PcbView();

        if( !pcb )
            continue;

        const wxString& moduleId = inst.moduleIdRef;

        if( moduleId == wxS( "WireModuleID" ) )
            importTrace( inst, *pcb );
        else if( moduleId == wxS( "ViaModuleID" ) )
            importVia( inst, *pcb );
        else if( moduleId == wxS( "HoleModuleID" ) )
            importHole( inst, *pcb );
        else if( mapLayer( pcb->layer ) == UNDEFINED_LAYER )
            continue;   // breadboard, schematic-only or unsupported items
        else if( moduleId == wxS( "PadModuleID" ) )
            importCopperPad( inst, *pcb );
        else if( moduleId.Contains( wxS( "LogoText" ) ) )
            importLogoText( inst, *pcb );
        else if( inst.properties.count( wxS( "shape" ) ) )
            importShapeSvg( std::string( inst.Property( wxS( "shape" ) ).utf8_str() ), inst, *pcb,
                            pcb->layer == wxS( "board" ) );
        else if( moduleId == wxS( "GroundPlaneModuleID" ) && inst.properties.count( wxS( "svg" ) ) )
            importShapeSvg( std::string( inst.Property( wxS( "svg" ) ).utf8_str() ), inst, *pcb, false );
        else if( moduleId.Contains( wxS( "RectanglePCBModuleID" ) ) )
            importRectangleBoard( inst, *pcb );
        else
            importPart( inst, *pcb );
    }

    assignGraphicNets();

    if( m_boardRectPx.GetWidth() > 0 )
    {
        // Fritzing's pick-and-place origin is the bottom-left board corner.
        VECTOR2I origin = toBoard( m_boardRectPx.GetOrigin() + VECTOR2D( 0, m_boardRectPx.GetHeight() ) );
        m_board->GetDesignSettings().SetAuxOrigin( origin );
        m_board->GetDesignSettings().SetGridOrigin( origin );
    }
}


void PCB_IO_FRITZING::assignGraphicNets()
{
    // Copper logos carry no net in Fritzing, but are often wired into the circuit, e.g. a
    // ground pour drawn as an image.  An SVG path becomes many shapes, so group the touching
    // shapes and give each group the net of the copper it touches, if there is only one.  A
    // group touching two nets (e.g. a coil antenna between two vias) keeps no net.
    std::vector<PCB_SHAPE*> shapes;

    for( BOARD_ITEM* item : m_board->Drawings() )
    {
        PCB_SHAPE* shape = dynamic_cast<PCB_SHAPE*>( item );

        if( shape && IsCopperLayer( shape->GetLayer() ) && shape->GetNetCode() <= 0 )
            shapes.push_back( shape );
    }

    if( shapes.empty() )
        return;

    std::vector<BOARD_CONNECTED_ITEM*> netted;

    for( PCB_TRACK* track : m_board->Tracks() )
    {
        if( track->GetNetCode() > 0 )
            netted.push_back( track );
    }

    for( FOOTPRINT* fp : m_board->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 )
                netted.push_back( pad );
        }
    }

    std::vector<std::shared_ptr<SHAPE>> geoms( shapes.size() );
    std::vector<BOX2I>                  boxes( shapes.size() );

    for( size_t i = 0; i < shapes.size(); i++ )
    {
        geoms[i] = shapes[i]->GetEffectiveShape( shapes[i]->GetLayer() );
        boxes[i] = shapes[i]->GetBoundingBox();
    }

    auto touches =
            [&]( size_t a, size_t b )
            {
                return shapes[a]->GetLayer() == shapes[b]->GetLayer() && boxes[a].Intersects( boxes[b] )
                       && geoms[a]->Collide( geoms[b].get() );
            };

    std::vector<int> group( shapes.size(), -1 );
    int              groups = 0;

    for( size_t seed = 0; seed < shapes.size(); seed++ )
    {
        if( group[seed] >= 0 )
            continue;

        std::vector<size_t> stack = { seed };
        group[seed] = groups;

        while( !stack.empty() )
        {
            size_t current = stack.back();
            stack.pop_back();

            for( size_t other = 0; other < shapes.size(); other++ )
            {
                if( group[other] < 0 && touches( current, other ) )
                {
                    group[other] = groups;
                    stack.push_back( other );
                }
            }
        }

        groups++;
    }

    std::vector<std::set<NETINFO_ITEM*>> groupNets( groups );

    for( size_t i = 0; i < shapes.size(); i++ )
    {
        PCB_LAYER_ID layer = shapes[i]->GetLayer();

        for( BOARD_CONNECTED_ITEM* other : netted )
        {
            if( other->IsOnLayer( layer ) && boxes[i].Intersects( other->GetBoundingBox() )
                && geoms[i]->Collide( other->GetEffectiveShape( layer ).get() ) )
            {
                groupNets[group[i]].insert( other->GetNet() );
            }
        }
    }

    for( size_t i = 0; i < shapes.size(); i++ )
    {
        if( groupNets[group[i]].size() == 1 )
            shapes[i]->SetNet( *groupNets[group[i]].begin() );
    }
}


void PCB_IO_FRITZING::placeItem( BOARD_ITEM* aItem, const VIEW& aView )
{
    const TRANSFORM& t = aView.transform;

    if( t.IsMirrored() )
        aItem->Mirror( VECTOR2I( 0, 0 ), FLIP_DIRECTION::LEFT_RIGHT );

    double angle = t.RotationDegrees();

    // Qt rotates clockwise on screen, KiCad counter-clockwise.
    if( std::abs( angle ) > 1e-6 )
        aItem->Rotate( VECTOR2I( 0, 0 ), EDA_ANGLE( -angle, DEGREES_T ) );

    aItem->Move( toBoard( aView.pos + VECTOR2D( t.dx, t.dy ) ) );
}


void PCB_IO_FRITZING::importTrace( const INSTANCE& aInst, const VIEW& aView )
{
    if( !( aView.wireFlags & WIRE_PCB_TRACE ) || ( aView.wireFlags & WIRE_RATSNEST ) )
        return;

    PCB_LAYER_ID layer = mapLayer( aView.layer );

    if( !IsCopperLayer( layer ) )
        return;

    // KiCad tracks are straight, so follow a curved Fritzing trace with short segments.
    std::vector<VECTOR2D> points = { aView.lineStart };

    if( aView.curved )
    {
        const VECTOR2D& p0 = aView.lineStart;
        const VECTOR2D& p1 = aView.bezierCp0;
        const VECTOR2D& p2 = aView.bezierCp1;
        const VECTOR2D& p3 = aView.lineEnd;

        double hull = ( p1 - p0 ).EuclideanNorm() + ( p2 - p1 ).EuclideanNorm() + ( p3 - p2 ).EuclideanNorm();
        int    count = std::clamp( KiROUND( hull / 2.0 ), 4, 64 );   // about one per 0.6 mm

        for( int i = 1; i < count; i++ )
        {
            double t = static_cast<double>( i ) / count;
            double u = 1.0 - t;

            points.push_back( p0 * ( u * u * u ) + p1 * ( 3 * u * u * t ) + p2 * ( 3 * u * t * t )
                              + p3 * ( t * t * t ) );
        }
    }

    points.push_back( aView.lineEnd );

    NETINFO_ITEM* net = netFor( aInst.modelIndex, wxS( "connector0" ) );

    for( size_t i = 1; i < points.size(); i++ )
    {
        VECTOR2I start = toBoard( scenePoint( aView, points[i - 1] ) );
        VECTOR2I end = toBoard( scenePoint( aView, points[i] ) );

        if( start == end )
            continue;

        PCB_TRACK* track = new PCB_TRACK( m_board );
        track->SetStart( start );
        track->SetEnd( end );
        track->SetLayer( layer );
        track->SetWidth( std::max( 1, KiROUND( pcbIUScale.MilsToIU( aView.wireMils ) ) ) );

        if( net )
            track->SetNet( net );

        m_board->Add( track, ADD_MODE::APPEND );
    }
}


/// Parse a Fritzing "hole size" property, e.g. "0.4mm,0.3mm": hole diameter and ring width.
static bool parseHoleSize( const wxString& aValue, double& aHoleMM, double& aRingMM )
{
    wxString hole = aValue.BeforeFirst( ',' );
    wxString ring = aValue.AfterFirst( ',' );
    double   holeIn, ringIn;

    if( !FRITZING_SVG::ParseLength( hole, holeIn ) || !FRITZING_SVG::ParseLength( ring, ringIn ) )
        return false;

    // A bare number is millimetres here, not pixels.
    aHoleMM = hole.Trim().Trim( false ).find_first_not_of( wxS( "0123456789.+-eE" ) ) == wxString::npos
                      ? holeIn * SCENE_DPI
                      : holeIn * 25.4;
    aRingMM = ring.Trim().Trim( false ).find_first_not_of( wxS( "0123456789.+-eE" ) ) == wxString::npos
                      ? ringIn * SCENE_DPI
                      : ringIn * 25.4;

    return aHoleMM > 0 && aRingMM >= 0;
}


void PCB_IO_FRITZING::importVia( const INSTANCE& aInst, const VIEW& aView )
{
    double holeMM = DEFAULT_VIA_HOLE_MM;
    double ringMM = DEFAULT_VIA_RING_MM;

    parseHoleSize( aInst.Property( wxS( "hole size" ) ), holeMM, ringMM );

    double   sizePx = ( holeMM + 2 * ringMM ) / 25.4 * SCENE_DPI + HOLE_IMAGE_MARGIN_PX;
    VECTOR2D center = scenePoint( aView, VECTOR2D( sizePx / 2, sizePx / 2 ) );

    PCB_VIA* via = new PCB_VIA( m_board );
    via->SetPosition( toBoard( center ) );
    via->SetViaType( VIATYPE::THROUGH );
    via->SetLayerPair( F_Cu, B_Cu );
    via->SetDrill( mmToIU( holeMM ) );
    via->SetWidth( PADSTACK::ALL_LAYERS, mmToIU( holeMM + 2 * ringMM ) );

    if( NETINFO_ITEM* net = netFor( aInst.modelIndex, wxS( "connector0" ) ) )
        via->SetNet( net );

    m_board->Add( via, ADD_MODE::APPEND );
}


FOOTPRINT* PCB_IO_FRITZING::newFootprint( const INSTANCE& aInst, const wxString& aName )
{
    FOOTPRINT* fp = new FOOTPRINT( m_board );
    fp->SetFPID( LIB_ID( wxEmptyString, LIB_ID::FixIllegalChars( aName, false ).wx_str() ) );
    fp->SetReference( aInst.title );
    fp->SetValue( aName );
    fp->Reference().SetVisible( false );
    fp->Value().SetVisible( false );
    return fp;
}


void PCB_IO_FRITZING::importHole( const INSTANCE& aInst, const VIEW& aView )
{
    double holeMM = 0;
    double ringMM = 0;

    if( !parseHoleSize( aInst.Property( wxS( "hole size" ) ), holeMM, ringMM ) )
        return;

    double   sizePx = ( holeMM + 2 * ringMM ) / 25.4 * SCENE_DPI + HOLE_IMAGE_MARGIN_PX;
    VECTOR2D center = scenePoint( aView, VECTOR2D( sizePx / 2, sizePx / 2 ) );

    FOOTPRINT* fp = newFootprint( aInst, wxS( "MountingHole" ) );
    PAD*       pad = new PAD( fp );

    pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::CIRCLE );
    pad->SetDrillSize( VECTOR2I( mmToIU( holeMM ), mmToIU( holeMM ) ) );

    if( ringMM > 0 )
    {
        pad->SetNumber( wxS( "1" ) );
        pad->SetAttribute( PAD_ATTRIB::PTH );
        pad->SetLayerSet( PAD::PTHMask() );
        pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( mmToIU( holeMM + 2 * ringMM ), mmToIU( holeMM + 2 * ringMM ) ) );

        if( NETINFO_ITEM* net = netFor( aInst.modelIndex, wxS( "connector0" ) ) )
            pad->SetNet( net );
    }
    else
    {
        pad->SetAttribute( PAD_ATTRIB::NPTH );
        pad->SetLayerSet( PAD::UnplatedHoleMask() );
        pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( mmToIU( holeMM ), mmToIU( holeMM ) ) );
    }

    fp->Add( pad );
    fp->SetAttributes( FP_THROUGH_HOLE );
    fp->SetPosition( toBoard( center ) );
    m_board->Add( fp, ADD_MODE::APPEND );
}


void PCB_IO_FRITZING::importCopperPad( const INSTANCE& aInst, const VIEW& aView )
{
    double widthMM, heightMM;

    if( !aInst.Property( wxS( "width" ) ).ToCDouble( &widthMM )
        || !aInst.Property( wxS( "height" ) ).ToCDouble( &heightMM ) || widthMM <= 0 || heightMM <= 0 )
    {
        return;
    }

    PCB_LAYER_ID layer = mapLayer( aView.layer );

    if( !IsCopperLayer( layer ) )
        return;

    FOOTPRINT* fp = newFootprint( aInst, wxS( "Pad" ) );
    PAD*       pad = new PAD( fp );

    pad->SetNumber( wxS( "1" ) );
    pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::RECTANGLE );
    pad->SetAttribute( PAD_ATTRIB::SMD );
    pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( mmToIU( widthMM ), mmToIU( heightMM ) ) );
    pad->SetLayerSet( PAD::SMDMask() );

    if( NETINFO_ITEM* net = netFor( aInst.modelIndex, wxS( "connector0" ) ) )
        pad->SetNet( net );

    fp->Add( pad );
    fp->SetAttributes( FP_SMD );

    VECTOR2D sizePx( widthMM / 25.4 * SCENE_DPI, heightMM / 25.4 * SCENE_DPI );
    placeFootprint( fp, aInst, aView, sizePx / 2, layer == B_Cu );
    m_board->Add( fp, ADD_MODE::APPEND );
}


void PCB_IO_FRITZING::importLogoText( const INSTANCE& aInst, const VIEW& aView )
{
    wxString     text = aInst.Property( wxS( "logo" ) );
    PCB_LAYER_ID layer = mapLayer( aView.layer );
    double       widthMM, heightMM;

    if( text.IsEmpty() || layer == UNDEFINED_LAYER || layer == Edge_Cuts
        || !aInst.Property( wxS( "width" ) ).ToCDouble( &widthMM )
        || !aInst.Property( wxS( "height" ) ).ToCDouble( &heightMM ) )
    {
        return;
    }

    // Fritzing renders logo text in OCR-A at 10/13 of the item height; capitals are about 0.7
    // of the font size, which is what KiCad's text height measures.
    double textMM = heightMM * 10.0 / 13.0 * 0.7;

    PCB_TEXT* item = new PCB_TEXT( m_board );
    item->SetLayer( layer );
    item->SetText( text );
    item->SetTextSize( VECTOR2I( mmToIU( textMM ), mmToIU( textMM ) ) );
    item->SetTextThickness( mmToIU( textMM * 0.15 ) );
    item->SetHorizJustify( GR_TEXT_H_ALIGN_CENTER );
    item->SetVertJustify( GR_TEXT_V_ALIGN_CENTER );
    item->SetMirrored( IsBackLayer( layer ) );
    item->SetKeepUpright( false );

    VECTOR2D center( widthMM / 25.4 * SCENE_DPI / 2, heightMM / 25.4 * SCENE_DPI / 2 );
    item->SetTextPos( toBoard( scenePoint( aView, center ) ) );
    item->SetTextAngle( EDA_ANGLE( -aView.transform.RotationDegrees(), DEGREES_T ) );

    m_board->Add( item, ADD_MODE::APPEND );
}


std::vector<std::unique_ptr<PCB_SHAPE>> PCB_IO_FRITZING::svgToShapes( const std::string& aSvg,
                                                                      BOARD_ITEM_CONTAINER* aParent,
                                                                      PCB_LAYER_ID aLayer )
{
    std::vector<std::unique_ptr<PCB_SHAPE>> result;

    if( aSvg.empty() )
        return result;

    SVG_IMPORT_PLUGIN        plugin;
    GRAPHICS_IMPORTER_PCBNEW importer( aParent );

    plugin.SetImporter( &importer );
    importer.SetLayer( aLayer );

    wxMemoryBuffer buffer;
    buffer.AppendData( aSvg.data(), aSvg.size() );

    if( !plugin.LoadFromMemory( buffer ) || !plugin.Import() )
        return result;

    for( std::unique_ptr<EDA_ITEM>& item : importer.GetItems() )
    {
        PCB_SHAPE* shape = dynamic_cast<PCB_SHAPE*>( item.get() );

        if( !shape )
            continue;

        // SVG elements are filled black by default, so nanosvg reports an outline-only line as
        // filled too; that comes back as a duplicate, invisible zero-width copy.
        if( !shape->IsSolidFill() && shape->GetWidth() <= 0 && aLayer != Edge_Cuts )
            continue;

        // Board items take their colour from the layer, not from the SVG.
        STROKE_PARAMS stroke = shape->GetStroke();
        stroke.SetColor( COLOR4D::UNSPECIFIED );
        shape->SetStroke( stroke );

        item.release();
        result.emplace_back( shape );
    }

    return result;
}


void PCB_IO_FRITZING::importShapeSvg( const std::string& aSvg, const INSTANCE& aInst, const VIEW& aView,
                                      bool aIsBoard )
{
    FRITZING_SVG svg;

    if( !svg.Load( aSvg ) )
        return;

    // Logo items may have been resized in Fritzing without updating their SVG.
    double widthMM = 0, heightMM = 0;
    aInst.Property( wxS( "width" ) ).ToCDouble( &widthMM );
    aInst.Property( wxS( "height" ) ).ToCDouble( &heightMM );

    std::vector<std::pair<wxString, PCB_LAYER_ID>> layers;

    if( aIsBoard )
    {
        layers.emplace_back( svg.HasElement( wxS( "board" ) ) ? wxS( "board" ) : wxS( "" ), Edge_Cuts );
        layers.emplace_back( wxS( "silkscreen" ), F_SilkS );
        layers.emplace_back( wxS( "silkscreen1" ), F_SilkS );
        layers.emplace_back( wxS( "silkscreen0" ), B_SilkS );
    }
    else
    {
        layers.emplace_back( wxS( "" ), mapLayer( aView.layer ) );
    }

    for( const auto& [groupId, layer] : layers )
    {
        if( !groupId.IsEmpty() && !svg.HasElement( groupId ) )
            continue;

        for( std::unique_ptr<PCB_SHAPE>& shape : svgToShapes( svg.Extract( groupId, widthMM, heightMM ), m_board,
                                                               layer ) )
        {
            if( layer == Edge_Cuts )
            {
                shape->SetFilled( false );

                if( shape->GetWidth() <= 0 )
                    shape->SetWidth( mmToIU( EDGE_WIDTH_MM ) );
            }

            // Ground fills are connected to the net they touch.
            if( IsCopperLayer( layer ) )
            {
                if( NETINFO_ITEM* net = netFor( aInst.modelIndex, wxS( "connector0" ) ) )
                    shape->SetNet( net );
            }

            placeItem( shape.get(), aView );
            m_board->Add( shape.release(), ADD_MODE::APPEND );
        }
    }
}


void PCB_IO_FRITZING::importRectangleBoard( const INSTANCE& aInst, const VIEW& aView )
{
    double widthMM, heightMM;

    if( !aInst.Property( wxS( "width" ) ).ToCDouble( &widthMM )
        || !aInst.Property( wxS( "height" ) ).ToCDouble( &heightMM ) )
    {
        return;
    }

    PCB_SHAPE* rect = new PCB_SHAPE( m_board, SHAPE_T::RECTANGLE );
    rect->SetLayer( Edge_Cuts );
    rect->SetStart( VECTOR2I( 0, 0 ) );
    rect->SetEnd( VECTOR2I( mmToIU( widthMM ), mmToIU( heightMM ) ) );
    rect->SetWidth( mmToIU( EDGE_WIDTH_MM ) );
    placeItem( rect, aView );
    m_board->Add( rect, ADD_MODE::APPEND );
}


std::unique_ptr<FOOTPRINT> PCB_IO_FRITZING::buildFootprint( const PART& aPart, const std::string& aSvgText,
                                                             VECTOR2D& aSizePx )
{
    FRITZING_SVG svg;
    double       widthIn, heightIn;

    if( !svg.Load( aSvgText ) || !svg.GetSizeInches( widthIn, heightIn ) )
        return nullptr;

    aSizePx = VECTOR2D( widthIn * SCENE_DPI, heightIn * SCENE_DPI );

    // Footprint coordinates are relative to the image center, which is also the point
    // Fritzing rotates parts about.
    VECTOR2D centerMM( widthIn * 25.4 / 2, heightIn * 25.4 / 2 );
    VECTOR2I centerIU( mmToIU( centerMM.x ), mmToIU( centerMM.y ) );

    wxString name = aPart.moduleId;

    auto fp = std::make_unique<FOOTPRINT>( m_board );
    fp->SetFPID( LIB_ID( wxEmptyString, LIB_ID::FixIllegalChars( name, false ).wx_str() ) );
    fp->SetLibDescription( aPart.title );
    fp->SetReference( aPart.label.IsEmpty() ? wxString( wxS( "REF**" ) ) : aPart.label + wxS( "**" ) );
    fp->SetValue( name );

    bool anyTht = false;

    for( const PART_CONNECTOR& connector : aPart.connectors )
    {
        auto top = connector.pcbSvgIds.find( wxS( "copper1" ) );
        auto bottom = connector.pcbSvgIds.find( wxS( "copper0" ) );

        if( top == connector.pcbSvgIds.end() && bottom == connector.pcbSvgIds.end() )
            continue;

        bool     tht = top != connector.pcbSvgIds.end() && bottom != connector.pcbSvgIds.end();
        wxString svgId = top != connector.pcbSvgIds.end() ? top->second : bottom->second;

        std::vector<FRITZING_SVG::PAD_PRIMITIVE> prims = svg.GetPrimitives( svgId );

        if( prims.empty() && tht )
            prims = svg.GetPrimitives( bottom->second );

        if( prims.empty() )
        {
            m_padsWithoutGeometry++;
            continue;
        }

        // The outer shape is the largest primitive including its stroke.
        auto outerSize =
                []( const FRITZING_SVG::PAD_PRIMITIVE& aPrim )
                {
                    return aPrim.bounds.GetSize() + VECTOR2D( aPrim.strokeWidth, aPrim.strokeWidth );
                };

        const FRITZING_SVG::PAD_PRIMITIVE* outer = &prims.front();

        for( const FRITZING_SVG::PAD_PRIMITIVE& prim : prims )
        {
            VECTOR2D a = outerSize( prim );
            VECTOR2D b = outerSize( *outer );

            if( a.x * a.y > b.x * b.y )
                outer = &prim;
        }

        VECTOR2D sizeMM = outerSize( *outer );
        VECTOR2D posMM = outer->bounds.GetCenter();

        if( sizeMM.x <= 0 || sizeMM.y <= 0 )
        {
            m_padsWithoutGeometry++;
            continue;
        }

        PAD* pad = new PAD( fp.get() );
        pad->SetNumber( padNumber( connector.id ) );
        pad->SetPinFunction( connector.name );
        pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( mmToIU( sizeMM.x ), mmToIU( sizeMM.y ) ) );

        bool round = outer->tag == wxS( "circle" ) || outer->tag == wxS( "ellipse" );

        if( round )
        {
            pad->SetShape( PADSTACK::ALL_LAYERS, std::abs( sizeMM.x - sizeMM.y ) < 0.01 ? PAD_SHAPE::CIRCLE
                                                                                          : PAD_SHAPE::OVAL );
        }
        else
        {
            pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::RECTANGLE );
        }

        if( tht )
        {
            // Fritzing draws through-hole pads as a stroked circle: the stroke is the copper ring
            // and the inside is the hole.
            double holeMM = 0;

            for( const FRITZING_SVG::PAD_PRIMITIVE& prim : prims )
            {
                if( ( prim.tag == wxS( "circle" ) || prim.tag == wxS( "ellipse" ) ) && prim.strokeWidth > 0 )
                {
                    holeMM = std::min( prim.bounds.GetWidth(), prim.bounds.GetHeight() ) - prim.strokeWidth;
                    break;
                }
            }

            double minSize = std::min( sizeMM.x, sizeMM.y );

            if( holeMM <= 0 || holeMM >= minSize )
                holeMM = minSize * 0.6;

            pad->SetAttribute( PAD_ATTRIB::PTH );
            pad->SetDrillSize( VECTOR2I( mmToIU( holeMM ), mmToIU( holeMM ) ) );
            pad->SetLayerSet( PAD::PTHMask() );
            anyTht = true;
        }
        else
        {
            pad->SetAttribute( PAD_ATTRIB::SMD );

            if( top != connector.pcbSvgIds.end() )
                pad->SetLayerSet( PAD::SMDMask() );
            else
                pad->SetLayerSet( LSET( { B_Cu, B_Paste, B_Mask } ) );
        }

        pad->SetPosition( VECTOR2I( mmToIU( posMM.x ), mmToIU( posMM.y ) ) - centerIU );
        fp->Add( pad );
    }

    // Fritzing drills every ring in the bottom copper layer, including mounting lugs that are
    // not connectors.  Add those as unnumbered plated holes.
    if( aPart.HasPcbLayer( wxS( "copper0" ) ) )
    {
        for( const FRITZING_SVG::PAD_PRIMITIVE& prim : svg.GetPrimitives( wxS( "copper0" ) ) )
        {
            if( prim.tag != wxS( "circle" ) || prim.strokeWidth <= 0 )
                continue;

            VECTOR2I pos = VECTOR2I( mmToIU( prim.bounds.GetCenter().x ), mmToIU( prim.bounds.GetCenter().y ) )
                           - centerIU;
            bool     isPad = false;

            for( PAD* pad : fp->Pads() )
                isPad |= ( pad->GetPosition() - pos ).EuclideanNorm() < mmToIU( 0.05 );

            double holeMM = prim.bounds.GetWidth() - prim.strokeWidth;
            double sizeMM = prim.bounds.GetWidth() + prim.strokeWidth;

            if( isPad || holeMM <= 0 )
                continue;

            PAD* pad = new PAD( fp.get() );
            pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::CIRCLE );
            pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( mmToIU( sizeMM ), mmToIU( sizeMM ) ) );
            pad->SetAttribute( PAD_ATTRIB::PTH );
            pad->SetDrillSize( VECTOR2I( mmToIU( holeMM ), mmToIU( holeMM ) ) );
            pad->SetLayerSet( PAD::PTHMask() );
            pad->SetPosition( pos );
            fp->Add( pad );
        }
    }

    static const std::vector<std::pair<wxString, PCB_LAYER_ID>> graphicLayers = {
        { wxS( "silkscreen" ), F_SilkS },
        { wxS( "silkscreen1" ), F_SilkS },
        { wxS( "silkscreen0" ), B_SilkS },
    };

    for( const auto& [groupId, layer] : graphicLayers )
    {
        if( !svg.HasElement( groupId ) )
            continue;

        for( std::unique_ptr<PCB_SHAPE>& shape : svgToShapes( svg.Extract( groupId ), fp.get(), layer ) )
        {
            shape->Move( -centerIU );
            fp->Add( shape.release() );
        }
    }

    fp->SetAttributes( anyTht ? FP_THROUGH_HOLE : FP_SMD );
    return fp;
}


void PCB_IO_FRITZING::placeFootprint( FOOTPRINT* aFootprint, const INSTANCE& aInst, const VIEW& aView,
                                      const VECTOR2D& aCenterPx, bool aBottom )
{
    // Fritzing draws bottom-side parts mirrored about their own center.
    if( aBottom )
        aFootprint->Flip( aFootprint->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

    double angle = aView.transform.RotationDegrees();

    if( std::abs( angle ) > 1e-6 )
        aFootprint->Rotate( aFootprint->GetPosition(), EDA_ANGLE( -angle, DEGREES_T ) );

    aFootprint->SetPosition( toBoard( scenePoint( aView, aCenterPx ) ) );

    PCB_FIELD& ref = aFootprint->Reference();
    ref.SetVisible( aView.titleVisible );

    if( aView.titleVisible )
    {
        // Part labels are drawn in silkscreen with their top-left corner at the title position.
        double textMM = std::max( 0.8, aView.titleFontSize * 25.4 / 72.0 * 0.7 );

        ref.SetTextSize( VECTOR2I( mmToIU( textMM ), mmToIU( textMM ) ) );
        ref.SetTextThickness( mmToIU( textMM * 0.15 ) );
        ref.SetTextAngle( ANGLE_0 );
        ref.SetHorizJustify( aBottom ? GR_TEXT_H_ALIGN_RIGHT : GR_TEXT_H_ALIGN_LEFT );
        ref.SetVertJustify( GR_TEXT_V_ALIGN_TOP );
        ref.SetTextPos( toBoard( aView.titlePos ) );
    }
}


void PCB_IO_FRITZING::importPart( const INSTANCE& aInst, const VIEW& aView )
{
    const PART* part = m_library->FindPart( aInst );

    if( !part )
    {
        importUnresolvedPart( aInst, aView );
        return;
    }

    if( part->HasPcbLayer( wxS( "board" ) ) )
    {
        std::string svg;

        if( m_library->LoadPcbSvg( *part, svg ) )
            importShapeSvg( svg, aInst, aView, true );

        return;
    }

    // Some stock parts, e.g. resistors, choose their footprint from a "pin spacing" property.
    PART     variant = *part;
    wxString spacing = aInst.Property( wxS( "pin spacing" ) );
    wxRegEx  spacingInImage( wxS( "_([0-9]+)mil" ) );

    spacing.Replace( wxS( " " ), wxEmptyString );
    spacing.Replace( wxS( "mil" ), wxEmptyString );

    if( !spacing.IsEmpty() && spacing.IsNumber() && spacingInImage.Matches( variant.pcbImage ) )
    {
        wxString image = variant.pcbImage;
        spacingInImage.ReplaceFirst( &image, wxS( "_" ) + spacing + wxS( "mil" ) );

        if( image != variant.pcbImage )
        {
            variant.pcbImage = image;
            variant.moduleId += wxS( "_" ) + spacing + wxS( "mil" );
        }
    }

    if( !m_libFootprints.count( variant.moduleId ) )
    {
        std::string                svg;
        std::unique_ptr<FOOTPRINT> proto;
        VECTOR2D                   sizePx;

        if( m_library->LoadPcbSvg( variant, svg ) )
            proto = buildFootprint( variant, svg, sizePx );

        m_libFootprints[variant.moduleId] = std::move( proto );
        m_libSizesPx[variant.moduleId] = sizePx;
    }

    FOOTPRINT* proto = m_libFootprints[variant.moduleId].get();

    if( !proto )
    {
        importUnresolvedPart( aInst, aView );
        return;
    }

    FOOTPRINT* fp = static_cast<FOOTPRINT*>( proto->Duplicate( IGNORE_PARENT_GROUP ) );
    fp->SetParent( m_board );
    fp->SetReference( aInst.title );

    // Prefer the properties that name a part's value over its generic title.
    static const wxChar* const valueProperties[] = { wxS( "resistance" ), wxS( "capacitance" ),
                                                     wxS( "inductance" ), wxS( "chip label" ),
                                                     wxS( "color" ),      wxS( "voltage" ) };

    wxString value;

    for( const wxChar* name : valueProperties )
    {
        value = aInst.Property( name );

        if( value.IsEmpty() )
        {
            auto it = part->properties.find( name );

            if( it != part->properties.end() )
                value = it->second;
        }

        if( !value.IsEmpty() )
            break;
    }

    fp->SetValue( value.IsEmpty() ? part->title : value );

    for( PAD* pad : fp->Pads() )
    {
        for( const PART_CONNECTOR& connector : part->connectors )
        {
            if( padNumber( connector.id ) == pad->GetNumber() )
            {
                if( NETINFO_ITEM* net = netFor( aInst.modelIndex, connector.id ) )
                    pad->SetNet( net );

                break;
            }
        }
    }

    bool smdOnly = !part->HasPcbLayer( wxS( "copper0" ) );
    bool bottom = aView.bottom || ( smdOnly && aView.layer == wxS( "copper0" ) );

    placeFootprint( fp, aInst, aView, m_libSizesPx[variant.moduleId] / 2, bottom );
    m_board->Add( fp, ADD_MODE::APPEND );
}


void PCB_IO_FRITZING::importUnresolvedPart( const INSTANCE& aInst, const VIEW& aView )
{
    m_unresolvedParts[aInst.moduleIdRef]++;

    struct PAD_SITE
    {
        wxString  connectorId;
        TRACE_END end;
        bool      bothSides;
    };

    std::vector<PAD_SITE> sites;
    VECTOR2D              sum;

    for( const wxString& connectorId : aView.connectors | std::views::keys )
    {
        auto it = m_traceEnds.find( key( aInst.modelIndex, connectorId ) );

        if( it == m_traceEnds.end() || it->second.empty() )
            continue;

        const TRACE_END& end = it->second.front();
        bool             bothSides = false;

        for( const TRACE_END& other : it->second )
            bothSides |= other.layer != end.layer;

        sites.push_back( { connectorId, end, bothSides } );
        sum += end.pos;
    }

    if( sites.empty() )
        return;

    FOOTPRINT* fp = newFootprint( aInst, aInst.moduleIdRef );

    // Anchor the footprint between its pads.
    fp->SetPosition( toBoard( sum / static_cast<double>( sites.size() ) ) );

    for( const PAD_SITE& site : sites )
    {
        int  size = std::max( mmToIU( 0.6 ), pxToIU( site.end.width * 1.5 ) );
        PAD* pad = new PAD( fp );

        pad->SetNumber( padNumber( site.connectorId ) );
        pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::CIRCLE );
        pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( size, size ) );

        if( site.bothSides )
        {
            pad->SetAttribute( PAD_ATTRIB::PTH );
            pad->SetDrillSize( VECTOR2I( size / 2, size / 2 ) );
            pad->SetLayerSet( PAD::PTHMask() );
        }
        else
        {
            pad->SetAttribute( PAD_ATTRIB::SMD );
            pad->SetLayerSet( site.end.layer == B_Cu ? LSET( { B_Cu, B_Paste, B_Mask } ) : PAD::SMDMask() );
        }

        if( NETINFO_ITEM* net = netFor( aInst.modelIndex, site.connectorId ) )
            pad->SetNet( net );

        fp->Add( pad );
        pad->SetPosition( toBoard( site.end.pos ) );
    }

    m_board->Add( fp, ADD_MODE::APPEND );
}

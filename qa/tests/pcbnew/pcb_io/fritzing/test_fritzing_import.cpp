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

/**
 * @file test_fritzing_import.cpp
 * Test suite for import of Fritzing sketches (.fzz).
 *
 * The sample is the LF/HF RFID field detector from https://github.com/pl4nty/RFID-Field-Detector.
 * Expected coordinates come from the drill, Gerber and pick-and-place files Fritzing exported
 * for the same sketch, which measure from the bottom-left corner of the board in mils.
 */

#include <pcbnew_utils/board_test_utils.h>
#include <pcbnew_utils/board_file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <pcbnew/pcb_io/fritzing/pcb_io_fritzing.h>
#include <pcbnew/pcb_io/fritzing/fritzing_part_library.h>

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_track.h>
#include <reporter.h>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>


struct FRITZING_IMPORT_FIXTURE
{
    PCB_IO_FRITZING m_plugin;

    std::string path( const std::string& aName )
    {
        return KI_TEST::GetPcbnewTestDataDir() + "plugins/fritzing/" + aName;
    }

    std::unique_ptr<BOARD> load( bool aWithParts, REPORTER* aReporter = nullptr )
    {
        std::map<std::string, UTF8> props;

        if( aWithParts )
            props[FRITZING_PART_LIBRARY::PARTS_PATH_PROPERTY] = path( "fritzing-parts" );

        m_plugin.SetReporter( aReporter );
        return m_plugin.LoadBoard( path( "LF-HF-RFID-Detector.fzz" ), &props );
    }

    /// Position relative to the board's bottom-left corner, y up, in mils.
    static VECTOR2D fromCorner( const BOARD& aBoard, const VECTOR2I& aPos )
    {
        VECTOR2I origin = aBoard.GetDesignSettings().GetAuxOrigin();
        return VECTOR2D( pcbIUScale.IUToMils( aPos.x - origin.x ), pcbIUScale.IUToMils( origin.y - aPos.y ) );
    }
};


BOOST_FIXTURE_TEST_SUITE( FritzingImport, FRITZING_IMPORT_FIXTURE )


BOOST_AUTO_TEST_CASE( SniffRecognizesSketch )
{
    BOOST_CHECK( m_plugin.CanReadBoard( path( "LF-HF-RFID-Detector.fzz" ) ) );

    // Right content check, wrong extension
    BOOST_CHECK( !m_plugin.CanReadBoard( path( "README.md" ) ) );
    BOOST_CHECK( !m_plugin.CanReadBoard( path( "fritzing-parts/core/sparkfun-passives-cap-0805.fzp" ) ) );
}


/// Part centers must match Fritzing's own pick-and-place output.
BOOST_AUTO_TEST_CASE( FootprintPlacement )
{
    std::unique_ptr<BOARD> board = load( true );
    BOOST_REQUIRE( board );

    BOOST_CHECK_EQUAL( board->Footprints().size(), 11 );

    const std::map<std::string, VECTOR2D> expected = {
        { "C1", { 160.77, 2022.71 } },    { "C2", { 274.937, 2022.71 } },   { "C3", { 274.937, 1951.84 } },
        { "C4", { 274.937, 1880.97 } },   { "C5", { 274.937, 1810.11 } },   { "C6", { 3062.35, 2022.71 } },
        { "C7", { 3062.35, 1951.84 } },   { "C8", { 3062.35, 1880.97 } },   { "C9", { 3062.35, 1810.11 } },
        { "LED1", { 265.094, 2085.7 } },  { "LED2", { 3064.32, 2085.7 } },
    };

    for( const auto& [ref, pos] : expected )
    {
        BOOST_TEST_CONTEXT( ref )
        {
            FOOTPRINT* fp = board->FindFootprintByReference( ref );
            BOOST_REQUIRE( fp );

            VECTOR2D actual = fromCorner( *board, fp->GetPosition() );
            BOOST_CHECK_CLOSE_FRACTION( actual.x, pos.x, 0.002 );
            BOOST_CHECK_CLOSE_FRACTION( actual.y, pos.y, 0.002 );

            BOOST_CHECK( fp->GetLayer() == F_Cu );
            BOOST_CHECK_EQUAL( fp->Pads().size(), 2 );
        }
    }
}


/// Pads come from the parts' footprint SVGs: check sizes and positions against the mask Gerber.
BOOST_AUTO_TEST_CASE( PadGeometry )
{
    std::unique_ptr<BOARD> board = load( true );
    BOOST_REQUIRE( board );

    FOOTPRINT* c1 = board->FindFootprintByReference( "C1" );
    BOOST_REQUIRE( c1 );

    PAD* pad1 = c1->FindPadByNumber( "1" );
    PAD* pad2 = c1->FindPadByNumber( "2" );
    BOOST_REQUIRE( pad1 && pad2 );

    BOOST_CHECK( pad1->GetAttribute() == PAD_ATTRIB::SMD );
    BOOST_CHECK( pad1->IsOnLayer( F_Cu ) && !pad1->IsOnLayer( B_Cu ) );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( pad1->GetSize( F_Cu ).x ), 0.8, 0.5 );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( pad1->GetSize( F_Cu ).y ), 1.2, 0.5 );

    // Mask flashes at X125Y2023 and X196Y2023 (whole mils)
    VECTOR2D p1 = fromCorner( *board, pad1->GetPosition() );
    VECTOR2D p2 = fromCorner( *board, pad2->GetPosition() );
    BOOST_CHECK_SMALL( std::min( std::abs( p1.x - 125 ), std::abs( p2.x - 125 ) ), 1.0 );
    BOOST_CHECK_SMALL( std::min( std::abs( p1.x - 196 ), std::abs( p2.x - 196 ) ), 1.0 );
    BOOST_CHECK_SMALL( p1.y - 2023, 1.0 );

    PAD* led = board->FindFootprintByReference( "LED1" )->FindPadByNumber( "1" );
    BOOST_REQUIRE( led );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( led->GetSize( F_Cu ).x ), 1.2, 0.5 );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( led->GetSize( F_Cu ).y ), 1.1, 0.5 );
}


/// Vias must match the drill file: 0.4 mm holes, 0.3 mm rings, at the drilled positions.
BOOST_AUTO_TEST_CASE( Vias )
{
    std::unique_ptr<BOARD> board = load( true );
    BOOST_REQUIRE( board );

    std::vector<PCB_VIA*> vias;

    for( PCB_TRACK* track : board->Tracks() )
    {
        if( track->Type() == PCB_VIA_T )
            vias.push_back( static_cast<PCB_VIA*>( track ) );
    }

    BOOST_REQUIRE_EQUAL( vias.size(), 5 );

    const std::vector<VECTOR2D> drilled = {
        { 1692.26, 1062.07 }, { 656.829, 1069.95 }, { 656.829, 1129 }, { 2967.85, 1617.19 }, { 3286.75, 2034.52 }
    };

    for( const VECTOR2D& hole : drilled )
    {
        double best = std::numeric_limits<double>::max();

        for( PCB_VIA* via : vias )
            best = std::min( best, ( fromCorner( *board, via->GetPosition() ) - hole ).EuclideanNorm() );

        BOOST_CHECK_SMALL( best, 1.0 );
    }

    for( PCB_VIA* via : vias )
    {
        BOOST_CHECK_EQUAL( via->GetDrillValue(), pcbIUScale.mmToIU( 0.4 ) );
        BOOST_CHECK_EQUAL( via->GetWidth( F_Cu ), pcbIUScale.mmToIU( 1.0 ) );
    }
}


/// Traces, nets, the board outline, logos and text.
BOOST_AUTO_TEST_CASE( BoardContents )
{
    std::unique_ptr<BOARD> board = load( true );
    BOOST_REQUIRE( board );

    int frontTraces = 0;
    int backTraces = 0;
    int unconnectedTraces = 0;

    for( PCB_TRACK* track : board->Tracks() )
    {
        if( track->Type() != PCB_TRACE_T )
            continue;

        if( track->GetLayer() == F_Cu )
            frontTraces++;
        else if( track->GetLayer() == B_Cu )
            backTraces++;

        if( track->GetNetCode() <= 0 )
            unconnectedTraces++;
    }

    BOOST_CHECK_EQUAL( frontTraces, 29 );
    BOOST_CHECK_EQUAL( backTraces, 2 );
    BOOST_CHECK_EQUAL( unconnectedTraces, 0 );

    // Both capacitor terminals are wired, so each pad has a net, and they differ.
    FOOTPRINT* c1 = board->FindFootprintByReference( "C1" );
    BOOST_REQUIRE( c1 );
    BOOST_CHECK_GT( c1->FindPadByNumber( "1" )->GetNetCode(), 0 );
    BOOST_CHECK_GT( c1->FindPadByNumber( "2" )->GetNetCode(), 0 );
    BOOST_CHECK_NE( c1->FindPadByNumber( "1" )->GetNetCode(), c1->FindPadByNumber( "2" )->GetNetCode() );

    // The credit-card outline: 85.6 x 53.98 mm with rounded corners
    BOX2I outline;
    int   edgeShapes = 0;
    int   frontCopperShapes = 0;
    int   backCopperShapes = 0;
    int   silkShapes = 0;
    std::set<wxString> texts;

    for( BOARD_ITEM* item : board->Drawings() )
    {
        if( item->Type() == PCB_SHAPE_T )
        {
            PCB_SHAPE* shape = static_cast<PCB_SHAPE*>( item );

            switch( shape->GetLayer() )
            {
            case Edge_Cuts:
                outline.Merge( shape->GetBoundingBox() );
                edgeShapes++;
                break;
            case F_Cu: frontCopperShapes++; break;
            case B_Cu: backCopperShapes++; break;
            case F_SilkS:
            case B_SilkS: silkShapes++; break;
            default: break;
            }
        }
        else if( item->Type() == PCB_TEXT_T )
        {
            texts.insert( static_cast<PCB_TEXT*>( item )->GetText() );
        }
    }

    BOOST_CHECK_GT( edgeShapes, 0 );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( outline.GetWidth() ), 85.6, 0.5 );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( outline.GetHeight() ), 53.98, 0.5 );

    // The bottom-left of the outline is the auxiliary origin
    VECTOR2I origin = board->GetDesignSettings().GetAuxOrigin();
    BOOST_CHECK_SMALL( pcbIUScale.IUTomm( outline.GetLeft() - origin.x ), 0.2 );
    BOOST_CHECK_SMALL( pcbIUScale.IUTomm( outline.GetBottom() - origin.y ), 0.2 );

    // The antenna coils are copper logos on both sides
    BOOST_CHECK_GT( frontCopperShapes, 0 );
    BOOST_CHECK_GT( backCopperShapes, 0 );
    BOOST_CHECK_GT( silkShapes, 0 );

    for( const wxString& text : { wxS( "HF" ), wxS( "LF" ), wxS( "125kHz" ), wxS( "13.56MHz" ), wxS( "Rev 0.01" ) } )
        BOOST_CHECK_MESSAGE( texts.count( text ), "missing text " << text );
}


/// Without the parts library, parts still get pads where their traces end, and the user is told.
BOOST_AUTO_TEST_CASE( MissingPartsLibrary )
{
    // Load a copy, away from the parts library kept next to the test sketch
    wxFileName copy( wxFileName::GetTempDir(), wxEmptyString );
    copy.AppendDir( wxString::Format( wxS( "qa_fritzing_%lu" ), wxGetProcessId() ) );
    copy.SetFullName( wxS( "LF-HF-RFID-Detector.fzz" ) );
    BOOST_REQUIRE( wxFileName::Mkdir( copy.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    BOOST_REQUIRE( wxCopyFile( path( "LF-HF-RFID-Detector.fzz" ), copy.GetFullPath() ) );

    WX_STRING_REPORTER     reporter;
    std::unique_ptr<BOARD> board;

    m_plugin.SetReporter( &reporter );
    BOOST_REQUIRE_NO_THROW( board = m_plugin.LoadBoard( copy.GetFullPath() ) );
    wxFileName::Rmdir( copy.GetPath(), wxPATH_RMDIR_RECURSIVE );
    BOOST_REQUIRE( board );

    BOOST_CHECK_EQUAL( board->Footprints().size(), 11 );
    BOOST_CHECK( reporter.GetMessages().Contains( wxS( "SparkFun-Passives-CAP-0805" ) ) );

    FOOTPRINT* c1 = board->FindFootprintByReference( "C1" );
    BOOST_REQUIRE( c1 );
    BOOST_CHECK_EQUAL( c1->Pads().size(), 2 );

    for( PAD* pad : c1->Pads() )
        BOOST_CHECK_GT( pad->GetNetCode(), 0 );
}


/**
 * A hand-made sketch covering what the RFID board does not: parts bundled in the .fzz, a
 * through-hole part rotated 90 degrees, an SMD part on the bottom, a rectangular board and a
 * schematic ground symbol naming a net.
 */
BOOST_AUTO_TEST_CASE( SyntheticSketch )
{
    WX_STRING_REPORTER reporter;
    m_plugin.SetReporter( &reporter );

    std::unique_ptr<BOARD> board = m_plugin.LoadBoard( path( "synthetic.fzz" ) );
    BOOST_REQUIRE( board );
    BOOST_CHECK( !reporter.GetMessages().Contains( wxS( "not found" ) ) );

    // Board-relative millimetres, from the top-left corner of the 50 x 30 mm board
    VECTOR2I topLeft = board->GetDesignSettings().GetAuxOrigin() - VECTOR2I( 0, pcbIUScale.mmToIU( 30 ) );

    auto mm =
            [&]( const VECTOR2I& aPos )
            {
                return VECTOR2D( pcbIUScale.IUTomm( aPos.x - topLeft.x ), pcbIUScale.IUTomm( aPos.y - topLeft.y ) );
            };

    BOX2I outline;

    for( BOARD_ITEM* item : board->Drawings() )
    {
        if( item->GetLayer() == Edge_Cuts )
            outline.Merge( item->GetBoundingBox() );
    }

    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( outline.GetWidth() ), 50.0, 0.5 );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( outline.GetHeight() ), 30.0, 0.5 );

    // The part is rotated a quarter turn clockwise on screen about its center (19, 14.5) px
    FOOTPRINT* tht = board->FindFootprintByReference( "THT1" );
    BOOST_REQUIRE( tht );
    BOOST_CHECK_CLOSE( tht->GetOrientation().Normalize().AsDegrees(), 270.0, 0.01 );
    BOOST_CHECK_SMALL( ( mm( tht->GetPosition() ) - VECTOR2D( 5.3622, 4.0922 ) ).EuclideanNorm(), 0.01 );
    BOOST_CHECK( tht->Reference().IsVisible() );

    PAD* pinA = tht->FindPadByNumber( "1" );
    PAD* pinB = tht->FindPadByNumber( "2" );
    BOOST_REQUIRE( pinA && pinB );
    BOOST_CHECK_EQUAL( pinA->GetPinFunction(), wxS( "A" ) );
    BOOST_CHECK_SMALL( ( mm( pinA->GetPosition() ) - VECTOR2D( 5.3622, 2.8222 ) ).EuclideanNorm(), 0.01 );
    BOOST_CHECK_SMALL( ( mm( pinB->GetPosition() ) - VECTOR2D( 5.3622, 5.3622 ) ).EuclideanNorm(), 0.01 );
    BOOST_CHECK( pinA->GetAttribute() == PAD_ATTRIB::PTH );
    BOOST_CHECK( pinA->GetShape( F_Cu ) == PAD_SHAPE::CIRCLE );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( pinA->GetSize( F_Cu ).x ), 2.032, 0.5 );
    BOOST_CHECK_CLOSE( pcbIUScale.IUTomm( pinA->GetDrillSize().x ), 1.016, 0.5 );

    // The bottom part is mirrored: its first pad, on the left in the image, ends up on the right
    FOOTPRINT* smd = board->FindFootprintByReference( "SMD1" );
    BOOST_REQUIRE( smd );
    BOOST_CHECK( smd->GetLayer() == B_Cu );
    BOOST_CHECK( !smd->Reference().IsVisible() );
    BOOST_CHECK_SMALL( ( mm( smd->GetPosition() ) - VECTOR2D( 18.935, 9.468 ) ).EuclideanNorm(), 0.01 );

    PAD* smdPad = smd->FindPadByNumber( "1" );
    BOOST_REQUIRE( smdPad );
    BOOST_CHECK( smdPad->IsOnLayer( B_Cu ) && !smdPad->IsOnLayer( F_Cu ) );
    BOOST_CHECK_SMALL( ( mm( smdPad->GetPosition() ) - VECTOR2D( 20.435, 9.468 ) ).EuclideanNorm(), 0.01 );

    // Nets: the trace joins pin A to the SMD pad; the ground symbol names pin B's net
    BOOST_CHECK_EQUAL( pinA->GetNetname(), wxS( "Net-(SMD1-Pad1)" ) );
    BOOST_CHECK_EQUAL( smdPad->GetNetname(), wxS( "Net-(SMD1-Pad1)" ) );
    BOOST_CHECK_EQUAL( pinB->GetNetname(), wxS( "GND" ) );

    BOOST_REQUIRE_EQUAL( board->Tracks().size(), 1 );
    BOOST_CHECK_EQUAL( board->Tracks().front()->GetNetname(), wxS( "Net-(SMD1-Pad1)" ) );
}


BOOST_AUTO_TEST_SUITE_END()

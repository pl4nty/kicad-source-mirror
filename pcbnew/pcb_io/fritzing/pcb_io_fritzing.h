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

#ifndef PCB_IO_FRITZING_H_
#define PCB_IO_FRITZING_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <layer_ids.h>
#include <math/box2.h>
#include <pcb_io/pcb_io.h>
#include <pcb_io/pcb_io_mgr.h>

#include "fritzing_model.h"

class BOARD_ITEM;
class BOARD_ITEM_CONTAINER;
class FOOTPRINT;
class FRITZING_PART_LIBRARY;
class NETINFO_ITEM;
class PCB_SHAPE;


/**
 * Imports the PCB view of a Fritzing sketch (.fzz archive or bare .fz file).
 *
 * Copper traces, vias, holes, copper and silkscreen logos, the board outline and part
 * footprints are imported.  Nets are derived from the connections of all three Fritzing
 * views, since Fritzing keeps them in sync.  Footprints come from the part's PCB SVG, so stock
 * parts need a copy of the fritzing-parts library; see FRITZING_PART_LIBRARY for where it is
 * looked for.  Parts that cannot be found get pads where their traces end.
 */
class PCB_IO_FRITZING : public PCB_IO
{
public:
    PCB_IO_FRITZING();
    ~PCB_IO_FRITZING() override;

    const IO_BASE::IO_FILE_DESC GetBoardFileDesc() const override
    {
        return IO_BASE::IO_FILE_DESC( _HKI( "Fritzing sketch files" ), { "fzz", "fz" }, {}, true,
                                      /* aCanRead */ true, /* aCanWrite */ false );
    }

    const IO_BASE::IO_FILE_DESC GetLibraryDesc() const override { return IO_BASE::IO_FILE_DESC( wxEmptyString, {} ); }

    bool CanReadBoard( const wxString& aFileName ) const override;

    std::vector<FOOTPRINT*> GetImportedCachedLibraryFootprints() override;

    long long GetLibraryTimestamp( const wxString& aLibraryPath ) const override { return 0; }

protected:
    void loadBoard( const wxString& aFileName, BOARD& aBoard, bool aIsNewLoad,
                    const std::map<std::string, UTF8>* aProperties = nullptr, PROJECT* aProject = nullptr ) override;

private:
    /// A trace end that touches a part connector, used to place pads of unresolved parts.
    struct TRACE_END
    {
        VECTOR2D     pos;     ///< Scene pixels.
        PCB_LAYER_ID layer;
        double       width;   ///< Scene pixels.
    };

    void buildBoard();
    void computeOrigin();
    void buildNets();
    void collectTraceEnds();

    void importTrace( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );
    void importVia( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );
    void importHole( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );
    void importCopperPad( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );
    void importLogoText( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );
    void importShapeSvg( const std::string& aSvg, const FRITZING::INSTANCE& aInst,
                         const FRITZING::VIEW& aView, bool aIsBoard );
    void importRectangleBoard( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );
    void importPart( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );
    void importUnresolvedPart( const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView );

    /// Build a library footprint, at the origin and on the front, from a part's PCB image.
    std::unique_ptr<FOOTPRINT> buildFootprint( const FRITZING::PART& aPart, const std::string& aSvg,
                                               VECTOR2D& aSizePx );

    /// Convert an SVG to board shapes on aLayer, in item-local coordinates (IU from the SVG origin).
    std::vector<std::unique_ptr<PCB_SHAPE>> svgToShapes( const std::string& aSvg, BOARD_ITEM_CONTAINER* aParent,
                                                         PCB_LAYER_ID aLayer );

    /// Move an item from item-local coordinates to its place on the board.
    void placeItem( BOARD_ITEM* aItem, const FRITZING::VIEW& aView );

    /// Place a footprint built at the origin; aCenterPx is its center in item-local pixels.
    void placeFootprint( FOOTPRINT* aFootprint, const FRITZING::INSTANCE& aInst, const FRITZING::VIEW& aView,
                         const VECTOR2D& aCenterPx, bool aBottom );

    FOOTPRINT* newFootprint( const FRITZING::INSTANCE& aInst, const wxString& aName );

    VECTOR2D scenePoint( const FRITZING::VIEW& aView, const VECTOR2D& aLocalPx ) const
    {
        return aView.pos + aView.transform.Map( aLocalPx );
    }

    VECTOR2I toBoard( const VECTOR2D& aScenePx ) const;
    static int pxToIU( double aPx );
    static int mmToIU( double aMm );

    static PCB_LAYER_ID mapLayer( const wxString& aFritzingLayer );
    static wxString padNumber( const wxString& aConnectorId );
    static wxString key( const wxString& aModelIndex, const wxString& aConnectorId )
    {
        return aModelIndex + wxS( "/" ) + aConnectorId;
    }

    // Union-find over connector keys.
    wxString findRoot( const wxString& aKey );
    void     unite( const wxString& aA, const wxString& aB );
    NETINFO_ITEM* netFor( const wxString& aModelIndex, const wxString& aConnectorId );

    REPORTER& reporter() const;

    FRITZING::SKETCH                       m_sketch;
    std::unique_ptr<FRITZING_PART_LIBRARY> m_library;

    VECTOR2D m_originPx;           ///< Scene point placed at m_originIU.
    VECTOR2I m_originIU;
    BOX2D    m_boardRectPx;        ///< First board's extent in scene pixels; empty if none.

    std::map<wxString, wxString>              m_parent;     ///< Union-find links.
    std::map<wxString, NETINFO_ITEM*>         m_nets;       ///< By union-find root.
    std::map<wxString, std::vector<TRACE_END>> m_traceEnds; ///< By part connector key.

    std::map<wxString, std::unique_ptr<FOOTPRINT>> m_libFootprints;  ///< By module id.
    std::map<wxString, VECTOR2D>                   m_libSizesPx;     ///< Image size, by module id.

    std::map<wxString, int> m_unresolvedParts;  ///< Module id -> instance count.
    int                     m_padsWithoutGeometry = 0;
};

#endif // PCB_IO_FRITZING_H_

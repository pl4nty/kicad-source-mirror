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
 * @file fritzing_model.h
 * Plain data model of a Fritzing sketch (.fz) and of Fritzing part definitions (.fzp).
 *
 * Fritzing stores scene coordinates in "pixels" at 90 DPI.  Each item has a position (the
 * top-left corner of its SVG image in the scene) and an optional QTransform which is applied
 * to item-local coordinates before the position is added.  Part graphics live in SVG files
 * referenced from the .fzp part definition; image and text "logo" items carry their SVG
 * inline in a "shape" property.
 */

#ifndef FRITZING_MODEL_H_
#define FRITZING_MODEL_H_

#include <map>
#include <string>
#include <vector>

#include <math/vector2d.h>
#include <wx/string.h>


namespace FRITZING
{

/// Fritzing scene units are pixels at this resolution.
constexpr double SCENE_DPI = 90.0;

/// Wire flags, from Fritzing's ViewGeometry::WireFlag.
enum WIRE_FLAGS
{
    WIRE_ROUTED = 2,
    WIRE_PCB_TRACE = 4,
    WIRE_RATSNEST = 8,
    WIRE_AUTOROUTABLE = 16,
    WIRE_NORMAL = 32,
    WIRE_SCHEMATIC_TRACE = 128
};


/**
 * The affine part of a Qt QTransform, mapping item-local pixels to scene pixels relative
 * to the item position:  x' = m11 * x + m21 * y + dx,  y' = m12 * x + m22 * y + dy.
 */
struct TRANSFORM
{
    double m11 = 1.0;
    double m12 = 0.0;
    double m21 = 0.0;
    double m22 = 1.0;
    double dx = 0.0;
    double dy = 0.0;

    VECTOR2D Map( const VECTOR2D& aPt ) const
    {
        return VECTOR2D( m11 * aPt.x + m21 * aPt.y + dx, m12 * aPt.x + m22 * aPt.y + dy );
    }

    bool IsMirrored() const { return m11 * m22 - m12 * m21 < 0.0; }

    /// Rotation in degrees, in Qt's sense (clockwise on screen), after removing any mirror.
    double RotationDegrees() const;
};


/// One end of a connection, as written in a <connect> element.
struct CONNECT
{
    wxString connectorId;
    wxString modelIndex;
    wxString layer;
};


/// Placement and connectivity of an instance in one view (breadboard, schematic or PCB).
struct VIEW
{
    wxString  layer;               ///< Fritzing view layer the item lives on.
    bool      bottom = false;      ///< "bottom" attribute; set for items on the bottom side.
    VECTOR2D  pos;                 ///< Scene position, in pixels.
    TRANSFORM transform;

    // Wire geometry (wires only), relative to pos.
    VECTOR2D  lineStart;
    VECTOR2D  lineEnd;
    int       wireFlags = 0;
    double    wireMils = 0.0;      ///< Wire / trace width.

    // Part label.
    bool      titleVisible = false;
    VECTOR2D  titlePos;
    double    titleFontSize = 0.0;

    /// Connections, keyed by this item's connector id.
    std::map<wxString, std::vector<CONNECT>> connectors;
};


/// One <instance> element of a sketch.
struct INSTANCE
{
    wxString moduleIdRef;
    wxString modelIndex;
    wxString path;                 ///< Path of the .fzp on the author's machine.
    wxString title;                ///< The part label, e.g. "R1".
    std::map<wxString, wxString> properties;
    std::map<wxString, VIEW>     views;  ///< Keyed by view name, e.g. "pcbView".

    const VIEW* PcbView() const
    {
        auto it = views.find( wxS( "pcbView" ) );
        return it == views.end() ? nullptr : &it->second;
    }

    wxString Property( const wxString& aName ) const
    {
        auto it = properties.find( aName );
        return it == properties.end() ? wxString() : it->second;
    }
};


/// The contents of a .fz sketch, plus any files bundled with it in a .fzz archive.
struct SKETCH
{
    wxString              fritzingVersion;
    std::vector<INSTANCE> instances;

    /// Files bundled in the .fzz (custom part definitions and their SVGs), by file name.
    std::map<wxString, std::string> bundledFiles;
};


/// A connector of a part definition.
struct PART_CONNECTOR
{
    wxString id;
    wxString name;
    wxString description;
    bool     hybrid = false;

    /// SVG element id of this connector in the PCB image, keyed by layer (copper0, copper1).
    std::map<wxString, wxString> pcbSvgIds;
};


/// The subset of a .fzp part definition used for PCB import.
struct PART
{
    wxString moduleId;
    wxString title;
    wxString label;                ///< Default reference prefix, e.g. "R".
    std::map<wxString, wxString> properties;

    wxString              pcbImage;   ///< e.g. "pcb/foo.svg".
    std::vector<wxString> pcbLayers;  ///< Layers the PCB image provides.

    std::vector<PART_CONNECTOR>        connectors;
    std::vector<std::vector<wxString>> buses;   ///< Internally connected connector ids.

    wxString fzpPath;              ///< Where the definition was found; empty if bundled.

    bool HasPcbLayer( const wxString& aLayer ) const
    {
        for( const wxString& layer : pcbLayers )
        {
            if( layer == aLayer )
                return true;
        }

        return false;
    }
};

} // namespace FRITZING

#endif // FRITZING_MODEL_H_

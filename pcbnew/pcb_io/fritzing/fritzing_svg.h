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

#ifndef FRITZING_SVG_H_
#define FRITZING_SVG_H_

#include <memory>
#include <string>
#include <vector>

#include <math/box2.h>
#include <wx/string.h>

class wxXmlDocument;


/**
 * Helpers for the SVG files Fritzing uses for part footprints, board shapes and logos.
 *
 * A Fritzing SVG holds one group per layer (e.g. <g id="copper1">, <g id="silkscreen">) and
 * names each connector's pad by an element id.  This class cuts an SVG down to one layer or
 * one element so it can be handed to nanosvg, and normalises the document size the way
 * Fritzing reads it (unitless lengths are pixels at 90 DPI).
 */
class FRITZING_SVG
{
public:
    FRITZING_SVG();
    ~FRITZING_SVG();

    bool Load( const std::string& aSvg );

    /// Physical size of the document, in inches.  False if it cannot be determined.
    bool GetSizeInches( double& aWidth, double& aHeight ) const;

    /// True if an element with this id exists.
    bool HasElement( const wxString& aId ) const;

    /**
     * Return an SVG containing only the element with id aId (with its ancestors' transforms),
     * or the whole visible document if aId is empty.  Hidden (display:none) elements are
     * dropped.  The root size is rewritten in absolute units, so the result can be parsed by
     * nanosvg in mm.  When aWidthMM and aHeightMM are positive, the document is resized to
     * that physical size.
     *
     * @return an empty string if aId was not found.
     */
    std::string Extract( const wxString& aId, double aWidthMM = 0.0, double aHeightMM = 0.0 ) const;

    /**
     * Geometry of the drawable elements of an extracted connector pad, in mm from the SVG
     * top-left corner.
     */
    struct PAD_PRIMITIVE
    {
        wxString tag;          ///< SVG element name: rect, circle, ellipse, path, ...
        BOX2D    bounds;       ///< Geometry bounds, excluding the stroke.
        double   strokeWidth;  ///< 0 if not stroked.
        bool     filled;
    };

    /// Return the drawable primitives of element aId, empty if it does not exist.
    std::vector<PAD_PRIMITIVE> GetPrimitives( const wxString& aId ) const;

    /**
     * Parse an SVG length, e.g. "0.1in", "2.54mm" or "9" (pixels at 90 DPI), into inches.
     */
    static bool ParseLength( const wxString& aLength, double& aInches );

private:
    std::unique_ptr<wxXmlDocument> m_doc;
};

#endif // FRITZING_SVG_H_

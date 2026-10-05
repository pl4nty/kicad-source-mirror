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

#ifndef FRITZING_PART_LIBRARY_H_
#define FRITZING_PART_LIBRARY_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/string.h>

#include "fritzing_model.h"


/**
 * Resolves the part definitions (.fzp) and PCB images referenced by a sketch.
 *
 * A .fzz archive bundles only parts that are not in Fritzing's own library, so stock parts
 * have to be found in a copy of the fritzing-parts repository.  Parts are searched for, in
 * order:
 *  - among the files bundled in the .fzz;
 *  - at the absolute path recorded in the sketch, which works when the sketch is opened on
 *    the machine that created it;
 *  - under each parts root: the ones given in the "fritzing_parts_path" load property and in
 *    the FRITZING_PARTS_PATH environment variable, a "fritzing-parts" folder next to the
 *    sketch, and the default install locations of Fritzing.
 *
 * Parts that Fritzing generates on the fly (pin headers, SIPs, DIPs) are recreated.
 *
 * A parts root is a folder laid out like the fritzing-parts repository: core/, contrib/,
 * user/ and obsolete/ hold .fzp files and svg/<folder>/pcb/ holds the footprint images.
 */
class FRITZING_PART_LIBRARY
{
public:
    /// Name of the load property holding extra parts roots, separated by ';'.
    static constexpr const char* PARTS_PATH_PROPERTY = "fritzing_parts_path";

    /// Name of the load property that, when "0", searches only the roots it is given and the
    /// sketch: not the environment, the sketch's folder or Fritzing's install locations.
    static constexpr const char* SEARCH_DEFAULTS_PROPERTY = "fritzing_search_default_paths";

    /// Environment variable holding extra parts roots, separated by the platform path separator.
    static constexpr const char* PARTS_PATH_ENV = "FRITZING_PARTS_PATH";

    FRITZING_PART_LIBRARY( const FRITZING::SKETCH& aSketch, const wxString& aSketchDir,
                           const wxString& aExtraRoots, bool aSearchDefaults = true );

    /// The definition of an instance's part, or nullptr if it cannot be found.
    const FRITZING::PART* FindPart( const FRITZING::INSTANCE& aInstance );

    /// Load the PCB image of a part.  False if it cannot be found.
    bool LoadPcbSvg( const FRITZING::PART& aPart, std::string& aSvg );

    const std::vector<wxString>& GetRoots() const { return m_roots; }

private:
    void addRoot( const wxString& aPath );
    std::optional<FRITZING::PART> loadFzpFile( const wxString& aPath );
    wxString findInRoots( const wxString& aModuleId, const wxString& aFileName );
    void indexRoot( const wxString& aRoot );

    const FRITZING::SKETCH& m_sketch;
    std::vector<wxString>   m_roots;

    std::map<wxString, std::optional<FRITZING::PART>> m_cache;            ///< By moduleId.
    std::map<wxString, wxString>                      m_bundledFzp;       ///< moduleId -> file.
    std::map<wxString, std::map<wxString, wxString>>  m_rootIndex;        ///< Root -> moduleId -> path.
    std::map<wxString, std::string>                   m_generatedSvgs;    ///< PCB image -> SVG.
};

#endif // FRITZING_PART_LIBRARY_H_

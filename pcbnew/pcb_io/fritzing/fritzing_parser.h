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

#ifndef FRITZING_PARSER_H_
#define FRITZING_PARSER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/string.h>

#include "fritzing_model.h"

class REPORTER;
class wxXmlNode;


/**
 * Reads Fritzing sketches (.fz, or .fzz archives holding a .fz plus bundled parts) and
 * Fritzing part definitions (.fzp) into the plain model of fritzing_model.h.
 */
class FRITZING_PARSER
{
public:
    explicit FRITZING_PARSER( REPORTER* aReporter ) : m_reporter( aReporter ) {}

    /**
     * Read a .fz or .fzz file.
     *
     * @return false if the file is not a Fritzing sketch.
     * @throw IO_ERROR if the file cannot be read.
     */
    bool ParseSketchFile( const wxString& aFileName, FRITZING::SKETCH& aSketch );

    /// Parse the XML text of a .fz sketch.
    bool ParseSketch( const std::string& aXml, FRITZING::SKETCH& aSketch );

    /// Parse the XML text of a .fzp part definition.
    static bool ParsePart( const std::string& aXml, FRITZING::PART& aPart );

    /// Return true if aFileName looks like a Fritzing sketch, by content rather than extension.
    static bool Sniff( const wxString& aFileName );

    /// Read every file of a zip archive into memory, keyed by file name.
    static bool ReadArchive( const wxString& aFileName, std::map<wxString, std::string>& aFiles );

    /// Read a whole file into memory.
    static bool ReadFile( const wxString& aFileName, std::string& aContents, size_t aMaxBytes = 0 );

private:
    void parseInstance( wxXmlNode* aNode, FRITZING::INSTANCE& aInstance );
    void parseView( wxXmlNode* aNode, FRITZING::VIEW& aView );

    REPORTER* m_reporter;
};

#endif // FRITZING_PARSER_H_

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

#include "fritzing_part_library.h"
#include "fritzing_parser.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

using namespace FRITZING;


/// Sub-folders of a parts root holding .fzp files, in lookup order.
static const wxChar* const PART_FOLDERS[] = { wxS( "core" ), wxS( "contrib" ), wxS( "user" ),
                                              wxS( "obsolete" ) };


static wxString fileNameOf( const wxString& aPath )
{
    wxString name = aPath;
    name.Replace( wxS( "\\" ), wxS( "/" ) );
    return name.AfterLast( '/' );
}


FRITZING_PART_LIBRARY::FRITZING_PART_LIBRARY( const SKETCH& aSketch, const wxString& aSketchDir,
                                              const wxString& aExtraRoots ) :
        m_sketch( aSketch )
{
    for( const auto& [name, data] : aSketch.bundledFiles )
    {
        if( !name.Lower().EndsWith( wxS( ".fzp" ) ) )
            continue;

        PART part;

        if( FRITZING_PARSER::ParsePart( data, part ) )
            m_bundledFzp[part.moduleId] = name;
    }

    wxStringTokenizer extra( aExtraRoots, wxS( ";" ), wxTOKEN_STRTOK );

    while( extra.HasMoreTokens() )
        addRoot( extra.GetNextToken() );

    wxString env;

    if( wxGetEnv( wxString::FromUTF8( PARTS_PATH_ENV ), &env ) )
    {
#ifdef __WINDOWS__
        wxStringTokenizer envTokens( env, wxS( ";" ), wxTOKEN_STRTOK );
#else
        wxStringTokenizer envTokens( env, wxS( ";:" ), wxTOKEN_STRTOK );
#endif

        while( envTokens.HasMoreTokens() )
            addRoot( envTokens.GetNextToken() );
    }

    if( !aSketchDir.IsEmpty() )
        addRoot( aSketchDir + wxFileName::GetPathSeparator() + wxS( "fritzing-parts" ) );

    // The user's own parts folder holds user/ and svg/user/ like a parts root.
    addRoot( wxStandardPaths::Get().GetDocumentsDir() + wxFileName::GetPathSeparator()
             + wxS( "Fritzing" ) + wxFileName::GetPathSeparator() + wxS( "parts" ) );

#if defined( __WINDOWS__ )
    addRoot( wxS( "C:\\Program Files\\Fritzing\\fritzing-parts" ) );
    addRoot( wxS( "C:\\Program Files (x86)\\Fritzing\\fritzing-parts" ) );
#elif defined( __WXMAC__ )
    addRoot( wxS( "/Applications/Fritzing.app/Contents/MacOS/fritzing-parts" ) );
#else
    addRoot( wxS( "/usr/share/fritzing/fritzing-parts" ) );
    addRoot( wxS( "/usr/share/fritzing-parts" ) );
    addRoot( wxS( "/usr/local/share/fritzing/fritzing-parts" ) );
    addRoot( wxS( "/usr/local/share/fritzing-parts" ) );
#endif
}


void FRITZING_PART_LIBRARY::addRoot( const wxString& aPath )
{
    if( aPath.IsEmpty() || !wxDir::Exists( aPath ) )
        return;

    wxFileName fn = wxFileName::DirName( aPath );
    fn.Normalize( wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS );
    wxString path = fn.GetPath();

    for( const wxString& root : m_roots )
    {
        if( root == path )
            return;
    }

    m_roots.push_back( path );
}


std::optional<PART> FRITZING_PART_LIBRARY::loadFzpFile( const wxString& aPath )
{
    std::string data;
    PART        part;

    if( !wxFileName::FileExists( aPath ) || !FRITZING_PARSER::ReadFile( aPath, data )
        || !FRITZING_PARSER::ParsePart( data, part ) )
    {
        return std::nullopt;
    }

    part.fzpPath = aPath;
    return part;
}


void FRITZING_PART_LIBRARY::indexRoot( const wxString& aRoot )
{
    if( m_rootIndex.count( aRoot ) )
        return;

    std::map<wxString, wxString>& index = m_rootIndex[aRoot];

    for( const wxChar* folder : PART_FOLDERS )
    {
        wxString dirPath = aRoot + wxFileName::GetPathSeparator() + folder;
        wxDir    dir;

        if( !wxDir::Exists( dirPath ) || !dir.Open( dirPath ) )
            continue;

        wxString name;

        for( bool more = dir.GetFirst( &name, wxS( "*.fzp" ), wxDIR_FILES ); more; more = dir.GetNext( &name ) )
        {
            wxString    path = dirPath + wxFileName::GetPathSeparator() + name;
            std::string head;

            if( !FRITZING_PARSER::ReadFile( path, head, 4096 ) )
                continue;

            size_t start = head.find( "moduleId=" );

            if( start == std::string::npos || start + 10 >= head.size() )
                continue;

            char   quote = head[start + 9];
            size_t end = head.find( quote, start + 10 );

            if( end != std::string::npos )
                index.emplace( wxString::FromUTF8( head.substr( start + 10, end - start - 10 ) ), path );
        }
    }
}


wxString FRITZING_PART_LIBRARY::findInRoots( const wxString& aModuleId, const wxString& aFileName )
{
    for( const wxString& root : m_roots )
    {
        if( aFileName.IsEmpty() )
            break;

        for( const wxChar* folder : PART_FOLDERS )
        {
            wxString path = root + wxFileName::GetPathSeparator() + folder + wxFileName::GetPathSeparator()
                            + aFileName;

            if( std::optional<PART> part = loadFzpFile( path ); part && part->moduleId == aModuleId )
                return path;
        }
    }

    // The file may have been renamed between library versions; look it up by module id.
    for( const wxString& root : m_roots )
    {
        indexRoot( root );

        auto it = m_rootIndex[root].find( aModuleId );

        if( it != m_rootIndex[root].end() )
            return it->second;
    }

    return wxEmptyString;
}


const PART* FRITZING_PART_LIBRARY::FindPart( const INSTANCE& aInstance )
{
    auto cached = m_cache.find( aInstance.moduleIdRef );

    if( cached != m_cache.end() )
        return cached->second ? &*cached->second : nullptr;

    std::optional<PART>& result = m_cache[aInstance.moduleIdRef];

    if( auto bundled = m_bundledFzp.find( aInstance.moduleIdRef ); bundled != m_bundledFzp.end() )
    {
        PART part;

        if( FRITZING_PARSER::ParsePart( m_sketch.bundledFiles.at( bundled->second ), part ) )
        {
            result = std::move( part );
            return &*result;
        }
    }

    // Fritzing's built-in resources (":/resources/...") are compiled into Fritzing itself.
    if( !aInstance.path.StartsWith( wxS( ":" ) ) )
    {
        if( std::optional<PART> part = loadFzpFile( aInstance.path );
            part && part->moduleId == aInstance.moduleIdRef )
        {
            result = std::move( part );
            return &*result;
        }
    }

    wxString path = findInRoots( aInstance.moduleIdRef, fileNameOf( aInstance.path ) );

    if( !path.IsEmpty() )
        result = loadFzpFile( path );

    return result ? &*result : nullptr;
}


bool FRITZING_PART_LIBRARY::LoadPcbSvg( const PART& aPart, std::string& aSvg )
{
    if( aPart.pcbImage.IsEmpty() )
        return false;

    wxString image = aPart.pcbImage;
    image.Replace( wxS( "\\" ), wxS( "/" ) );

    // Bundled images are stored flat as e.g. "svg.pcb.foo.svg".
    wxString bundledName = wxS( "svg." ) + image;
    bundledName.Replace( wxS( "/" ), wxS( "." ) );

    for( const wxString& name : { bundledName, fileNameOf( image ) } )
    {
        auto it = m_sketch.bundledFiles.find( name );

        if( it != m_sketch.bundledFiles.end() )
        {
            aSvg = it->second;
            return true;
        }
    }

    std::vector<wxString> candidates;
    wxString              nativeImage = image;
    nativeImage.Replace( wxS( "/" ), wxFileName::GetPathSeparator() );

    auto addCandidates =
            [&]( const wxString& aRoot, const wxString& aPreferredFolder )
            {
                wxString svgDir = aRoot + wxFileName::GetPathSeparator() + wxS( "svg" )
                                  + wxFileName::GetPathSeparator();

                if( !aPreferredFolder.IsEmpty() )
                    candidates.push_back( svgDir + aPreferredFolder + wxFileName::GetPathSeparator() + nativeImage );

                for( const wxChar* folder : PART_FOLDERS )
                    candidates.push_back( svgDir + folder + wxFileName::GetPathSeparator() + nativeImage );
            };

    if( !aPart.fzpPath.IsEmpty() )
    {
        wxFileName fzp( aPart.fzpPath );
        wxString   folder = fzp.GetDirs().IsEmpty() ? wxString() : fzp.GetDirs().Last();

        fzp.RemoveLastDir();
        addCandidates( fzp.GetPath(), folder );
    }

    for( const wxString& root : m_roots )
        addCandidates( root, wxEmptyString );

    for( const wxString& candidate : candidates )
    {
        if( wxFileName::FileExists( candidate ) && FRITZING_PARSER::ReadFile( candidate, aSvg ) )
            return true;
    }

    return false;
}

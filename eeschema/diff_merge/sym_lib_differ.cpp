/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/gpl-3.0.html
 * or you may search the http://www.gnu.org website for the version 3 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "sym_lib_differ.h"

#include <diff_merge/property_diff.h>
#include <diff_merge/sym_item_diff.h>
#include <lib_symbol.h>
#include <sch_item.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_parser.h>
#include <richio.h>
#include <symbol_library_common.h>

#include <wx/arrstr.h>
#include <wx/filename.h>
#include <wx/intl.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>


namespace KICAD_DIFF
{

SYM_LIB_DIFFER::SYM_LIB_DIFFER( const SYMBOL_MAP& aBefore, const SYMBOL_MAP& aAfter, const wxString& aPath ) :
        m_before( aBefore ),
        m_after( aAfter ),
        m_path( aPath )
{
}


SYM_LIB_DIFFER::~SYM_LIB_DIFFER() = default;


std::pair<std::vector<std::unique_ptr<LIB_SYMBOL>>, SYM_LIB_DIFFER::SYMBOL_MAP>
SYM_LIB_DIFFER::LoadLibrary( const wxString& aPath )
{
    std::vector<std::unique_ptr<LIB_SYMBOL>> owners;
    SYMBOL_MAP                               map;

    // The s-expression library plugin asserts on relative paths; absolute the
    // input so CLI callers can pass `./` paths without crashing.
    wxFileName absPath( aPath );
    absPath.MakeAbsolute();
    wxString absStr = absPath.GetFullPath();

    SCH_IO_KICAD_SEXPR       io;
    std::vector<LIB_SYMBOL*> rawList;

    // EnumerateSymbolLib can throw IO_ERROR / std::exception on a corrupt or
    // unreadable file. Let the exception escape so callers can distinguish a
    // load failure from a deliberately-empty library; swallowing here was
    // misreported as a valid empty diff downstream.
    io.EnumerateSymbolLib( rawList, absStr, nullptr );

    // EnumerateSymbolLib returns pointers owned by the IO plugin's cache,
    // which is destroyed when the SCH_IO_KICAD_SEXPR goes out of scope. Clone
    // each symbol so we own a stable copy.
    for( LIB_SYMBOL* sym : rawList )
    {
        if( !sym )
            continue;

        std::unique_ptr<LIB_SYMBOL> owner( new LIB_SYMBOL( *sym ) );
        map[owner->GetName()] = owner.get();
        owners.push_back( std::move( owner ) );
    }

    // The clones still point their parent at the IO cache, which dies with the
    // plugin below. Re-link derived symbols to the copies we own so Flatten can
    // still pull in the inherited pins and fields.
    std::map<wxString, LIB_SYMBOL*> byName;

    for( const std::unique_ptr<LIB_SYMBOL>& owner : owners )
        byName[owner->GetName()] = owner.get();

    for( const std::unique_ptr<LIB_SYMBOL>& owner : owners )
    {
        if( owner->GetParentName().IsEmpty() )
            continue;

        auto it = byName.find( owner->GetParentName() );

        if( it != byName.end() )
            owner->SetParent( it->second );
    }

    return { std::move( owners ), std::move( map ) };
}


std::pair<std::vector<std::unique_ptr<LIB_SYMBOL>>, SYM_LIB_DIFFER::SYMBOL_MAP>
SYM_LIB_DIFFER::LoadLibraryText( const std::string& aText )
{
    LIB_SYMBOL_MAP                           symbols;
    std::vector<std::unique_ptr<LIB_SYMBOL>> owners;
    SYMBOL_MAP                               map;
    STRING_LINE_READER                       reader( aText, wxS( "library" ) );
    SCH_IO_KICAD_SEXPR_PARSER                parser( &reader );

    try
    {
        parser.ParseLib( symbols );
    }
    catch( ... )
    {
        for( auto& [name, symbol] : symbols )
            delete symbol;

        throw;
    }

    for( auto& [name, symbol] : symbols )
    {
        map[symbol->GetName()] = symbol;
        owners.emplace_back( symbol );
    }

    if( !parser.GetParseWarnings().empty() )
        THROW_IO_ERROR( parser.GetParseWarnings().front() );

    // An unlinked symbol would flatten without its parent's pins and fields
    for( auto& [name, symbol] : symbols )
    {
        if( symbol->GetParentName().IsEmpty() )
            continue;

        auto it = symbols.find( symbol->GetParentName() );

        if( it == symbols.end() )
            THROW_IO_ERROR( wxString::Format( _( "No parent for extended symbol %s" ), name ) );

        symbol->SetParent( it->second );
    }

    return { std::move( owners ), std::move( map ) };
}


namespace
{
    std::string flatText( const LIB_SYMBOL* aSymbol )
    {
        std::unique_ptr<LIB_SYMBOL> flat = aSymbol->Flatten();
        STRING_FORMATTER            formatter;

        SCH_IO_KICAD_SEXPR_LIB_CACHE::SaveSymbol( flat.get(), formatter );
        return formatter.GetString();
    }
} // namespace


bool SYM_LIB_DIFFER::DiffLibraryText( const std::string& aBefore, const std::string& aAfter, DOCUMENT_DIFF* aDiff )
{
    using LOADED = std::pair<std::vector<std::unique_ptr<LIB_SYMBOL>>, SYMBOL_MAP>;

    try
    {
        LOADED before = aBefore.empty() ? LOADED() : LoadLibraryText( aBefore );
        LOADED after = aAfter.empty() ? LOADED() : LoadLibraryText( aAfter );

        // Not Diff(), whose property deltas read the property manager the GUI thread rebuilds
        *aDiff = DiffLibraryByName(
                before.second, after.second, wxEmptyString, wxS( "kicad_sym" ), wxS( "LIB_SYMBOL" ), OPTIONS(),
                []( const LIB_SYMBOL* )
                {
                    return BOX2I();
                },
                []( const LIB_SYMBOL* aA, const LIB_SYMBOL* aB )
                {
                    // Flattening drops the parent, and a symbol moved to an equal parent still changed
                    return aA->GetParentName() != aB->GetParentName() || flatText( aA ) != flatText( aB );
                } );

        return true;
    }
    catch( const IO_ERROR& )
    {
        return false;
    }
}


namespace
{
    // Compare also sees symbol state that no property or element carries
    constexpr int SYMBOL_COMPARE_FLAGS = ~( SCH_ITEM::COMPARE_FLAGS::UUID | SCH_ITEM::COMPARE_FLAGS::IDENTITY );


    std::vector<PROPERTY_DELTA> symbolDeltas( const LIB_SYMBOL* aBefore, const LIB_SYMBOL* aAfter )
    {
        std::vector<PROPERTY_DELTA> deltas = DiffItemProperties( aBefore, aAfter );
        wxString                    before = wxJoin( aBefore->GetFPFilters(), ' ' );
        wxString                    after = wxJoin( aAfter->GetFPFilters(), ' ' );

        if( before != after )
        {
            deltas.push_back(
                    { wxS( "Footprint Filters" ), DIFF_VALUE::FromString( before ), DIFF_VALUE::FromString( after ) } );
        }

        // Unit and body style names are not properties, so a merge could drop one unnoticed
        auto unitNames = []( const LIB_SYMBOL* aSymbol )
        {
            wxArrayString names;

            for( const auto& [unit, name] : aSymbol->GetUnitDisplayNames() )
                names.Add( wxString::Format( wxS( "%d: %s" ), unit, name ) );

            return wxJoin( names, ',' );
        };

        before = unitNames( aBefore );
        after = unitNames( aAfter );

        if( before != after )
        {
            deltas.push_back(
                    { wxS( "Unit Names" ), DIFF_VALUE::FromString( before ), DIFF_VALUE::FromString( after ) } );
        }

        auto bodyStyleNames = []( const LIB_SYMBOL* aSymbol )
        {
            wxArrayString names;

            for( const wxString& name : aSymbol->GetBodyStyleNames() )
                names.Add( name );

            return wxJoin( names, ',' );
        };

        before = bodyStyleNames( aBefore );
        after = bodyStyleNames( aAfter );

        if( before != after )
        {
            deltas.push_back( { wxS( "Body Style Names" ), DIFF_VALUE::FromString( before ),
                                DIFF_VALUE::FromString( after ) } );
        }

        return deltas;
    }


    bool hasSymbolDifferences( const LIB_SYMBOL* aBefore, const LIB_SYMBOL* aAfter )
    {
        std::unique_ptr<LIB_SYMBOL> before = aBefore->Flatten();
        std::unique_ptr<LIB_SYMBOL> after = aAfter->Flatten();

        return before->Compare( *after, SYMBOL_COMPARE_FLAGS ) != 0
               || !symbolDeltas( before.get(), after.get() ).empty()
               || !DiffSymbolElements( before.get(), after.get() ).empty();
    }
} // namespace


DOCUMENT_DIFF SYM_LIB_DIFFER::Diff()
{
    DOCUMENT_DIFF result = DiffLibraryByName(
            m_before, m_after, m_path, wxS( "kicad_sym" ), wxS( "LIB_SYMBOL" ), m_options,
            []( const LIB_SYMBOL* aSym )
            {
                return aSym->GetUnitBoundingBox( 0, 0 );
            },
            []( const LIB_SYMBOL* aA, const LIB_SYMBOL* aB )
            {
                return hasSymbolDifferences( aA, aB );
            } );

    for( ITEM_CHANGE& change : result.changes )
    {
        if( change.kind != CHANGE_KIND::MODIFIED || !change.refdes )
            continue;

        auto beforeIt = m_before.find( *change.refdes );
        auto afterIt = m_after.find( *change.refdes );

        if( beforeIt == m_before.end() || afterIt == m_after.end() )
            continue;

        std::unique_ptr<LIB_SYMBOL> before = beforeIt->second->Flatten();
        std::unique_ptr<LIB_SYMBOL> after = afterIt->second->Flatten();

        change.properties = symbolDeltas( before.get(), after.get() );
        AppendElementChanges( change, DiffSymbolElements( before.get(), after.get() ) );
    }

    return result;
}


bool SYM_LIB_DIFFER::MergeKeepsBothSides( const SYMBOL_MAP& aAncestor, const SYMBOL_MAP& aOurs,
                                          const SYMBOL_MAP& aTheirs, const SYMBOL_MAP& aMerged )
{
    std::set<wxString> settled;

    for( const auto& [name, ours] : aOurs )
    {
        auto theirs = aTheirs.find( name );
        auto merged = aMerged.find( name );

        if( theirs == aTheirs.end() || merged == aMerged.end() )
            continue;

        const std::string text = SavedSymbolText( *ours );

        // Both sides made the same edit, which reads as nothing applied against something wanted
        if( SavedSymbolText( *theirs->second ) == text && SavedSymbolText( *merged->second ) == text )
            settled.insert( name );
    }

    auto unsettled = [&]( const SYMBOL_MAP& aSide )
    {
        SYMBOL_MAP out;

        for( const auto& [name, symbol] : aSide )
        {
            if( !settled.contains( name ) )
                out.emplace( name, symbol );
        }

        return out;
    };

    // A settled symbol normalizes to its merged state and every other symbol to its ancestor
    auto normalizedFrom = [&]( const wxString& aName ) -> const LIB_SYMBOL*
    {
        const SYMBOL_MAP& from = settled.contains( aName ) ? aMerged : aAncestor;
        auto              it = from.find( aName );

        return it == from.end() ? nullptr : it->second;
    };

    // The names aName derives from once normalized, nearest first, stopping at a loop
    auto lineage = [&]( const wxString& aName )
    {
        std::vector<wxString> chain;

        for( const LIB_SYMBOL* symbol = normalizedFrom( aName ); symbol && !symbol->GetParentName().IsEmpty(); )
        {
            const wxString& parent = symbol->GetParentName();

            if( parent == aName || std::ranges::find( chain, parent ) != chain.end() )
                break;

            chain.push_back( parent );
            symbol = normalizedFrom( parent );
        }

        return chain;
    };

    // A symbol derived from a settled one inherits its merged state, not the ancestor's, so the settled names,
    // what derives from them and what they derive from are copied and relinked to each other
    std::set<wxString> copied;

    auto isSettled = [&]( const wxString& aName )
    {
        return settled.contains( aName );
    };

    for( const auto& [name, symbol] : aAncestor )
    {
        const std::vector<wxString> chain = lineage( name );

        if( isSettled( name ) || std::ranges::any_of( chain, isSettled ) )
        {
            copied.insert( name );
            copied.insert( chain.begin(), chain.end() );
        }
    }

    std::vector<std::unique_ptr<LIB_SYMBOL>> owned;
    std::map<wxString, LIB_SYMBOL*>          copies;
    SYMBOL_MAP                               normalized = aAncestor;

    for( const wxString& name : copied )
    {
        if( !aAncestor.contains( name ) )
            continue;

        owned.push_back( std::make_unique<LIB_SYMBOL>( *normalizedFrom( name ) ) );
        copies[name] = owned.back().get();
        normalized[name] = owned.back().get();
    }

    for( const auto& [name, copy] : copies )
    {
        auto parent = copies.find( copy->GetParentName() );

        if( !copy->GetParentName().IsEmpty() && parent != copies.end() )
            copy->SetParent( parent->second );
    }

    const SYMBOL_MAP ancestor = unsettled( normalized );
    const SYMBOL_MAP merged = unsettled( aMerged );

    auto carries = [&]( const SYMBOL_MAP& aSide, const SYMBOL_MAP& aOther )
    {
        const SYMBOL_MAP                 side = unsettled( aSide );
        const SYMBOL_MAP                 other = unsettled( aOther );
        std::map<KIID_PATH, ITEM_CHANGE> applied;
        std::map<KIID_PATH, ITEM_CHANGE> wanted;

        for( ITEM_CHANGE& change : SYM_LIB_DIFFER( side, merged ).Diff().changes )
            applied.emplace( change.id, std::move( change ) );

        for( ITEM_CHANGE& change : SYM_LIB_DIFFER( ancestor, other ).Diff().changes )
            wanted.emplace( change.id, std::move( change ) );

        // The bounding box only places a change on screen, and comes from a different symbol on each side
        return std::equal( applied.begin(), applied.end(), wanted.begin(), wanted.end(),
                           []( const auto& aA, const auto& aB )
                           {
                               return aA.first == aB.first && aA.second.kind == aB.second.kind
                                      && aA.second.properties == aB.second.properties
                                      && aA.second.children == aB.second.children;
                           } );
    };

    return carries( aOurs, aTheirs ) && carries( aTheirs, aOurs );
}

} // namespace KICAD_DIFF

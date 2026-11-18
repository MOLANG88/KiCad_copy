/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <boost/test/unit_test.hpp>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <map>
#include <memory>
#include <set>

#include <lib_symbol.h>
#include <sch_io/cadstar/sch_io_cadstar_archive.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/settings_manager.h>


BOOST_AUTO_TEST_CASE( CadstarPartWithoutDefinitionKeepsGatesTogether )
{
    SETTINGS_MANAGER           manager;
    std::unique_ptr<SCHEMATIC> schematic( new SCHEMATIC( nullptr ) );

    manager.LoadProject( "" );
    schematic->SetProject( &manager.Prj() );
    schematic->CurrentSheet().clear();
    schematic->CurrentSheet().push_back( &schematic->Root() );

    SCH_IO_CADSTAR_ARCHIVE plugin;
    plugin.LoadSchematicFile( KI_TEST::GetEeschemaTestDataDir() + "io/cadstar/issue26087_no_part_definition.csa",
                              schematic.get() );

    SCH_SHEET_LIST        sheets = schematic->BuildSheetListSortedByPageNumbers();
    const SCH_SHEET_PATH& path = sheets.front();

    std::map<int, SCH_SYMBOL*> u1ByUnit;

    for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->GetRef( &path, false ) == wxS( "U1" ) )
            u1ByUnit[symbol->GetUnitSelection( &path )] = symbol;
    }

    BOOST_REQUIRE_EQUAL( u1ByUnit.size(), 2 );
    BOOST_REQUIRE( u1ByUnit.count( 1 ) );
    BOOST_REQUIRE( u1ByUnit.count( 2 ) );

    const LIB_SYMBOL& libA = *u1ByUnit.at( 1 )->GetLibSymbolRef();
    const LIB_SYMBOL& libB = *u1ByUnit.at( 2 )->GetLibSymbolRef();

    BOOST_CHECK_EQUAL( libA.GetUnitCount(), 2 );
    BOOST_CHECK_EQUAL( libB.GetUnitCount(), 2 );
    BOOST_CHECK_EQUAL( libA.Compare( libB ), 0 );

    auto pinNumbersOfUnit =
            []( const LIB_SYMBOL& aSymbol, int aUnit )
            {
                std::set<wxString> numbers;

                for( SCH_PIN* pin : aSymbol.GetGraphicalPins( aUnit, 0 ) )
                {
                    if( pin->GetUnit() == aUnit )
                        numbers.insert( pin->GetNumber() );
                }

                return numbers;
            };

    BOOST_CHECK( pinNumbersOfUnit( libA, 1 ) == ( std::set<wxString>{ wxS( "1" ), wxS( "2" ) } ) );
    BOOST_CHECK( pinNumbersOfUnit( libA, 2 ) == ( std::set<wxString>{ wxS( "3" ), wxS( "4" ) } ) );
}

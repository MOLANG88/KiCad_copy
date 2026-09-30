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
 * @file test_easyeda_import.cpp
 * Test suite for import of EasyEDA standard-format boards.
 */

#include <pcbnew_utils/board_file_utils.h>
#include <pcbnew_utils/board_test_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <pcbnew/pcb_io/easyeda/pcb_io_easyeda_parser.h>
#include <pcbnew/pcb_io/easyeda/pcb_io_easyeda_plugin.h>
#include <pcbnew/pcb_io/pcb_io.h>
#include <pcbnew/pcb_io/pcb_io_mgr.h>

#include <board.h>
#include <board_design_settings.h>
#include <layer_ids.h>
#include <netclass.h>
#include <project/net_settings.h>

#include <json_common.h>

#include <wx/filefn.h>
#include <wx/filename.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>


struct EASYEDA_IMPORT_FIXTURE
{
    EASYEDA_IMPORT_FIXTURE() {}
};


BOOST_FIXTURE_TEST_SUITE( EasyedaImport, EASYEDA_IMPORT_FIXTURE )


BOOST_AUTO_TEST_CASE( BoardLoadImportsDefaultClearance )
{
    wxString dataPath = wxString::FromUTF8( KI_TEST::GetPcbnewTestDataDir()
                                            + "plugins/easyeda/PCB_PCB_ESP32-PICO-D4 smart watch_2023-09-02.json" );

    IO_RELEASER<PCB_IO> plugin( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::EASYEDA ) );
    BOOST_REQUIRE( plugin );

    std::unique_ptr<BOARD> board( plugin->LoadBoard( dataPath, nullptr ) );
    BOOST_REQUIRE( board );

    std::shared_ptr<NETCLASS> defaultNetclass = board->GetDesignSettings().m_NetSettings->GetDefaultNetclass();

    BOOST_CHECK_EQUAL( defaultNetclass->GetClearance(), PCB_IO_EASYEDA_PARSER( nullptr ).ScaleSize( 0.6 ) );
    BOOST_CHECK( board->m_LegacyNetclassesLoaded );
}


BOOST_AUTO_TEST_SUITE_END()


namespace
{

wxString MakeEasyEdaBoardWithAllInnerLayersEnabled()
{
    const std::string sourcePath =
            KI_TEST::GetPcbnewTestDataDir() + "plugins/easyeda/PCB_USBMETER-PD QC修改版_2023-09-02.json";
    std::ifstream source( sourcePath, std::ios::binary );
    BOOST_REQUIRE( source.is_open() );

    nlohmann::json document = nlohmann::json::parse( source );
    auto&          layers = document.at( "layers" );

    // The fixture has all 32 inner layer records but enables only Inner1 and Inner2
    for( auto& layer : layers )
    {
        std::string& record = layer.get_ref<std::string&>();

        if( record.find( "~Inner" ) == std::string::npos )
            continue;

        static const std::string disabledFields = "~false~false~false~";
        size_t                   disabled = record.find( disabledFields );

        if( disabled != std::string::npos )
            record.replace( disabled, disabledFields.size(), "~true~false~true~" );
    }

    // Layer IDs determine the copper count even when the records arrive out of order.
    std::reverse( layers.begin(), layers.end() );

    wxString tempBase = wxFileName::CreateTempFileName( wxS( "kicad_qa_easyeda_layers_" ) );
    wxRemoveFile( tempBase );
    wxString      tempPath = tempBase + wxS( ".json" );
    std::ofstream output( tempPath.ToStdString(), std::ios::binary );
    BOOST_REQUIRE( output.is_open() );
    output << document;
    BOOST_REQUIRE( output.good() );
    output.close();

    return tempPath;
}

} // namespace


BOOST_AUTO_TEST_SUITE( EasyEdaStdImport )


BOOST_AUTO_TEST_CASE( RealFixturePreservesFinalInnerLayerNames )
{
    wxString               tempPath = MakeEasyEdaBoardWithAllInnerLayersEnabled();
    PCB_IO_EASYEDA         plugin;
    std::unique_ptr<BOARD> board = plugin.LoadBoard( tempPath );

    BOOST_CHECK_EQUAL( board->GetCopperLayerCount(), 34 );
    BOOST_CHECK_EQUAL( board->GetLayerName( In31_Cu ), wxString( "Inner31" ) );
    BOOST_CHECK_EQUAL( board->GetLayerName( In32_Cu ), wxString( "Inner32" ) );
    wxRemoveFile( tempPath );
}


BOOST_AUTO_TEST_SUITE_END()

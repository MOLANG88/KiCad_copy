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

#include <boost/test/unit_test.hpp>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <footprint.h>
#include <layer_ids.h>
#include <locale_io.h>
#include <netclass.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_shape.h>
#include <project/net_settings.h>
#include <pcb_track.h>
#include <zone.h>
#include <pcbnew_utils/board_file_utils.h>
#include <pcbnew_utils/board_test_utils.h>
#include <specctra_import_export/specctra.h>
#include <tool/tool_manager.h>
#include <reporter.h>
#include <ranges>

#include <wx/filename.h>
#include <wx/string.h>

using namespace DSN;


namespace
{
// Unique per-test scratch file that survives BOOST_REQUIRE early-outs and cleans up on scope exit.
class TEMP_FILE
{
public:
    explicit TEMP_FILE( const wxString& aPrefix ) :
            m_path( std::string( wxFileName::CreateTempFileName( aPrefix ).ToUTF8() ) )
    {
    }

    ~TEMP_FILE()
    {
        std::error_code ec;
        std::filesystem::remove( m_path, ec );
    }

    const std::filesystem::path& Path() const { return m_path; }
    std::string                  Str() const { return m_path.string(); }
    wxString                     WxStr() const { return wxString( m_path.string() ); }

private:
    std::filesystem::path m_path;
};


std::string slurp( const std::filesystem::path& aPath )
{
    std::ifstream     in( aPath );
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}


std::unique_ptr<BOARD> loadTestBoard()
{
    const std::filesystem::path boardPath =
            std::filesystem::path( KI_TEST::GetPcbnewTestDataDir() ) / "issue3812.kicad_pcb";

    BOOST_REQUIRE( std::filesystem::exists( boardPath ) );

    return KI_TEST::RequireBoardFromPcbnewQAData( "issue3812.kicad_pcb" );
}


int countTraces( BOARD* aBoard )
{
    int n = 0;

    for( PCB_TRACK* track : aBoard->Tracks() )
    {
        if( track->Type() == PCB_TRACE_T )
            ++n;
    }

    return n;
}


void importSession( BOARD* aBoard, const wxString& aSesPath )
{
    TOOL_MANAGER toolMgr;
    toolMgr.SetEnvironment( aBoard, nullptr, nullptr, nullptr, nullptr );
    toolMgr.RegisterTool( new KI_TEST::DUMMY_TOOL() );

    BOARD_COMMIT commit( &toolMgr, true, false );
    ImportSpecctraSession( aBoard, aSesPath, commit );
    commit.Push( wxT( "Import Specctra Session" ), SKIP_UNDO );
}


void importSession( BOARD* aBoard, const wxString& aSesPath, WX_STRING_REPORTER* aReporter,
                    int* aSkipped )
{
    TOOL_MANAGER toolMgr;
    toolMgr.SetEnvironment( aBoard, nullptr, nullptr, nullptr, nullptr );
    toolMgr.RegisterTool( new KI_TEST::DUMMY_TOOL() );

    BOARD_COMMIT commit( &toolMgr, true, false );
    ImportSpecctraSession( aBoard, aSesPath, commit, aReporter, aSkipped );
    commit.Push( wxT( "Import Specctra Session" ), SKIP_UNDO );
}
} // namespace


/*
 * A family of Specctra DSN quoting/naming defects that made otherwise-valid boards un-exportable
 * or produced structurally invalid interchange files (#24946/7/8).
 */


// #24946: a field payload holding the string delimiter (an inch mark in the Value/PN) must not
// terminate the quoted string early and desync a conforming reader.
BOOST_AUTO_TEST_CASE( SpecctraExportQuoteInField )
{
    LOCALE_IO              toggle;
    std::unique_ptr<BOARD> board = loadTestBoard();

    BOOST_REQUIRE( !board->Footprints().empty() );
    board->Footprints().front()->SetValue( wxT( "2x20 pin 0.1\" female header" ) );

    TEMP_FILE out( wxT( "kicad_specctra_24946_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( board.get(), out.Str() ) );

    const std::string dsn = slurp( out.Path() );

    // The delimiter must no longer appear inside the payload, and the transformed form is emitted.
    BOOST_CHECK( dsn.find( "0.1\" female" ) == std::string::npos );
    BOOST_CHECK( dsn.find( "0.1'' female" ) != std::string::npos );
}


// #24947: duplicate and empty reference designators (unannotated REF** fiducials, intentional
// duplicates, case-only differences) must not abort the export; they are uniquified on the fly,
// case-insensitively to match the exported case_sensitive default.
BOOST_AUTO_TEST_CASE( SpecctraExportDuplicateEmptyRefs )
{
    LOCALE_IO              toggle;
    std::unique_ptr<BOARD> board = loadTestBoard();

    BOOST_REQUIRE_GE( board->Footprints().size(), 5u );

    board->Footprints()[0]->SetReference( wxEmptyString );
    board->Footprints()[1]->SetReference( wxEmptyString );
    board->Footprints()[2]->SetReference( wxT( "U1" ) );
    board->Footprints()[3]->SetReference( wxT( "U1" ) );
    board->Footprints()[4]->SetReference( wxT( "u1" ) );

    TEMP_FILE out( wxT( "kicad_specctra_24947_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( board.get(), out.Str() ) );

    const std::string dsn = slurp( out.Path() );

    // Empty ids take a REF** base; case-insensitive collisions are all suffixed distinctly.
    BOOST_CHECK( dsn.find( "(place REF**_1 " ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(place U1 " ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(place U1_1 " ) != std::string::npos );
    BOOST_CHECK( dsn.find( "(place u1_2 " ) != std::string::npos );
}


// Allegro Specctra writes zone fills as (wire (poly ...)) — an abbreviation of (polygon).
// Parsing must accept that token and skip the pour geometry (zones stay on the board).
// A rule area inside a rotated footprint must be exported in the footprint's frame, like its pads:
// the keepout polygon of the image is the library-frame outline, whatever the placement.
BOOST_AUTO_TEST_CASE( SpecctraExportRotatedFootprintKeepout )
{
    LOCALE_IO toggle;
    BOARD     board;

    // A board outline, which the export requires.
    const VECTOR2I corners[4] = { { 0, 0 }, { pcbIUScale.mmToIU( 150 ), 0 },
                                  { pcbIUScale.mmToIU( 150 ), pcbIUScale.mmToIU( 100 ) },
                                  { 0, pcbIUScale.mmToIU( 100 ) } };

    for( int i = 0; i < 4; ++i )
    {
        PCB_SHAPE* edge = new PCB_SHAPE( &board, SHAPE_T::SEGMENT );
        edge->SetLayer( Edge_Cuts );
        edge->SetStart( corners[i] );
        edge->SetEnd( corners[( i + 1 ) % 4] );
        board.Add( edge );
    }

    FOOTPRINT* footprint = new FOOTPRINT( &board );
    footprint->SetReference( wxS( "K1" ) );
    footprint->SetPosition( { pcbIUScale.mmToIU( 100 ), pcbIUScale.mmToIU( 50 ) } );
    footprint->SetOrientation( ANGLE_90 );

    // Library frame: x 1..3 mm, y 0.5..1.5 mm.
    ZONE* keepout = new ZONE( footprint );
    keepout->SetIsRuleArea( true );
    keepout->SetDoNotAllowTracks( true );
    keepout->SetDoNotAllowVias( true );
    keepout->SetLayer( F_Cu );
    keepout->AppendCorner( { pcbIUScale.mmToIU( 1 ), pcbIUScale.mmToIU( 0.5 ) }, -1 );
    keepout->AppendCorner( { pcbIUScale.mmToIU( 3 ), pcbIUScale.mmToIU( 0.5 ) }, -1 );
    keepout->AppendCorner( { pcbIUScale.mmToIU( 3 ), pcbIUScale.mmToIU( 1.5 ) }, -1 );
    keepout->AppendCorner( { pcbIUScale.mmToIU( 1 ), pcbIUScale.mmToIU( 1.5 ) }, -1 );
    footprint->Add( keepout );
    board.Add( footprint );

    TEMP_FILE out( wxT( "kicad_specctra_keepout_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( &board, out.Str() ) );

    const std::string dsn = slurp( out.Path() );
    const size_t      image = dsn.find( "(image " );
    BOOST_REQUIRE( image != std::string::npos );
    const std::string imageText = dsn.substr( image, dsn.find( "(network", image ) - image );

    // DSN points are in micrometres with y up: the library-frame corners, not a rotated copy.
    BOOST_CHECK_MESSAGE( imageText.find( "(keepout" ) != std::string::npos, imageText );
    BOOST_CHECK_MESSAGE( imageText.find( "1000 -500" ) != std::string::npos, imageText );
    BOOST_CHECK_MESSAGE( imageText.find( "3000 -1500" ) != std::string::npos, imageText );
    BOOST_CHECK_MESSAGE( imageText.find( "3000 -500" ) != std::string::npos, imageText );
    BOOST_CHECK_MESSAGE( imageText.find( "1000 -1500" ) != std::string::npos, imageText );
}


BOOST_AUTO_TEST_CASE( SpecctraImportAllegroPolyWire )
{
    LOCALE_IO              toggle;
    std::unique_ptr<BOARD> board = loadTestBoard();

    TEMP_FILE ses( wxT( "kicad_specctra_allegro_poly_" ) );

    const std::string validLayer = std::string( board->GetLayerName( F_Cu ).ToUTF8() );

    std::ofstream out( ses.Path() );
    out << "(session test.ses\n"
        << "  (base_design test.dsn)\n"
        << "  (routes\n"
        << "    (resolution um 10)\n"
        << "    (library_out )\n"
        << "    (network_out\n"
        << "      (net \"/ALLPST\"\n"
        << "        (wire (path \"" << validLayer << "\" 1600  0 0  1000 0)\n"
        << "          (type route))\n"
        << "        (wire (poly \"" << validLayer << "\" 0  0 0  1000 0  1000 1000  0 1000  0 0)\n"
        << "          (type normal))\n"
        << "      )\n"
        << "    )\n"
        << "  )\n"
        << ")\n";
    out.close();

    BOOST_REQUIRE_NO_THROW( importSession( board.get(), ses.Str() ) );

    // Path wire imported; poly wire ignored (zone fill), so exactly one trace.
    BOOST_CHECK_EQUAL( countTraces( board.get() ), 1 );
}


// #24948: single unresolved session items (an unknown wire layer or a placement referencing a
// uniquified id the board never had) must be skipped rather than sinking the whole import.
BOOST_AUTO_TEST_CASE( SpecctraImportSkipsInvalidWireLayer )
{
    LOCALE_IO              toggle;
    std::unique_ptr<BOARD> board = loadTestBoard();

    TEMP_FILE ses( wxT( "kicad_specctra_24948_" ) );

    // The valid wire uses a layer name taken from the loaded board so this is a faithful
    // router-output fragment; the placement id and the second wire's layer are deliberately absent.
    const std::string validLayer = std::string( board->GetLayerName( F_Cu ).ToUTF8() );

    std::ofstream out( ses.Path() );
    out << "(session test.ses\n"
        << "  (base_design test.dsn)\n"
        << "  (placement\n"
        << "    (resolution um 10)\n"
        << "    (component test\n"
        << "      (place Nonexistent_Ref 0 0 front 0)\n"
        << "    )\n"
        << "  )\n"
        << "  (routes\n"
        << "    (resolution um 10)\n"
        << "    (library_out )\n"
        << "    (network_out\n"
        << "      (net \"/ALLPST\"\n"
        << "        (wire (path \"" << validLayer << "\" 1600  0 0  1000 0))\n"
        << "        (wire (path Bogus_Missing_Layer 1600  0 0  1000 1000))\n"
        << "      )\n"
        << "    )\n"
        << "  )\n"
        << ")\n";
    out.close();

#ifdef __WXMAC__
    // Don't convert log messages to throws, as they will trip up BOOST_REQUIRE_NO_THROW
    wxLog::SetTimestamp( wxEmptyString );
#endif

    BOOST_REQUIRE_NO_THROW( importSession( board.get(), ses.Str() ) );

    // The session replaces all unlocked traces; only the valid wire survives, the bogus wire and
    // the unresolved placement are dropped rather than throwing.
    BOOST_CHECK_EQUAL( countTraces( board.get() ), 1 );
}


/*
 * DSN export must produce a file that the DSN parser can read back.
 */
BOOST_AUTO_TEST_CASE( SpecctraDsnExportParseable )
{
    LOCALE_IO              toggle;
    std::unique_ptr<BOARD> board = loadTestBoard();

    TEMP_FILE out( wxT( "kicad_specctra_parseable_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( board.get(), out.Str() ) );

    SPECCTRA_DB db;
    try
    {
        db.LoadPCB( out.WxStr() );
        BOOST_CHECK( db.GetPCB() != nullptr );
    }
    catch( const IO_ERROR& ioe )
    {
        BOOST_FAIL( ioe.What().ToStdString() );
    }
}


BOOST_AUTO_TEST_CASE( SpecctraDsnExportContent )
{
    LOCALE_IO              toggle;
    std::unique_ptr<BOARD> board = loadTestBoard();

    BOOST_REQUIRE( !board->Footprints().empty() );

    TEMP_FILE out( wxT( "kicad_specctra_content_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( board.get(), out.Str() ) );

    const std::string dsn = slurp( out.Path() );

    // The unit declaration the exporter writes (mm-based resolution; um units).
    BOOST_CHECK( dsn.find( "(unit um" ) != std::string::npos );

    // At least one (net "<name>" header matches an existing net.
    bool netHeaderFound = false;

    for( const NETINFO_ITEM* net : board->GetNetInfo().NetsByNetcode() | std::views::values )
    {
        if( net->GetNetname().IsEmpty() )
            continue;

        if( dsn.find( "(net \"" + std::string( net->GetNetname().ToUTF8() ) + "\"" ) != std::string::npos )
            netHeaderFound = true;
    }

    BOOST_CHECK( netHeaderFound );
}


BOOST_AUTO_TEST_CASE( SpecctraExportReporterCapturesOutlineWarning )
{
    LOCALE_IO toggle;

    std::unique_ptr<BOARD> board = std::make_unique<BOARD>();

    WX_STRING_REPORTER reporter;
    TEMP_FILE out( wxT( "kicad_specctra_reporter_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( board.get(), out.WxStr(), &reporter ) );

    BOOST_CHECK( reporter.GetMessages().Contains( wxT( "Board outline is malformed" ) ) );
}


/*
 * Custom design rules (.kicad_dru) must reach the router: the export evaluates their clearances
 * with the DRC engine and writes them as Specctra classes, class_class rules and pin clearance
 * classes.
 */
namespace
{
/// A node of a DSN s-expression: an atom or a list.
struct DSN_NODE
{
    bool                  m_isList = false;
    std::string           m_atom;
    std::vector<DSN_NODE> m_children;

    bool Is( const std::string& aHead ) const
    {
        return m_isList && !m_children.empty() && !m_children[0].m_isList
               && m_children[0].m_atom == aHead;
    }

    const DSN_NODE* Child( const std::string& aHead ) const
    {
        for( const DSN_NODE& child : m_children )
        {
            if( child.Is( aHead ) )
                return &child;
        }

        return nullptr;
    }

    const std::string& Atom( size_t aIndex ) const
    {
        static const std::string empty;

        if( aIndex < m_children.size() && !m_children[aIndex].m_isList )
            return m_children[aIndex].m_atom;

        return empty;
    }
};


/// Parse the DSN from its (placement section on, to avoid the bare quote of (string_quote ").
DSN_NODE parseDsnBody( const std::string& aDsn )
{
    std::vector<std::vector<DSN_NODE>> stack( 1 );
    size_t                             pos = aDsn.find( "(placement" );

    BOOST_REQUIRE( pos != std::string::npos );

    while( pos < aDsn.size() )
    {
        const char c = aDsn[pos];

        if( std::isspace( static_cast<unsigned char>( c ) ) )
        {
            ++pos;
        }
        else if( c == '(' )
        {
            stack.emplace_back();
            ++pos;
        }
        else if( c == ')' )
        {
            if( stack.size() == 1 )
                break; // closes the (pcb list

            DSN_NODE list;
            list.m_isList = true;
            list.m_children = std::move( stack.back() );
            stack.pop_back();
            stack.back().push_back( std::move( list ) );
            ++pos;
        }
        else
        {
            DSN_NODE atom;
            size_t   end;

            if( c == '"' )
            {
                end = aDsn.find( '"', pos + 1 );
                BOOST_REQUIRE( end != std::string::npos );
                atom.m_atom = aDsn.substr( pos + 1, end - pos - 1 );
                ++end;
            }
            else
            {
                end = pos;

                while( end < aDsn.size() && !std::isspace( static_cast<unsigned char>( aDsn[end] ) )
                       && aDsn[end] != '(' && aDsn[end] != ')' )
                {
                    ++end;
                }

                atom.m_atom = aDsn.substr( pos, end - pos );
            }

            stack.back().push_back( std::move( atom ) );
            pos = end;
        }
    }

    DSN_NODE root;
    root.m_isList = true;
    root.m_children = std::move( stack.front() );
    return root;
}


void collectLists( const DSN_NODE& aNode, const std::string& aHead,
                   std::vector<const DSN_NODE*>& aLists )
{
    if( aNode.Is( aHead ) )
        aLists.push_back( &aNode );

    for( const DSN_NODE& child : aNode.m_children )
    {
        if( child.m_isList )
            collectLists( child, aHead, aLists );
    }
}


/// The clearance of a (rule (clearance N)), in DSN units (um here), or -1.
int ruleClearance( const DSN_NODE* aRule )
{
    const DSN_NODE* clearance = aRule ? aRule->Child( "clearance" ) : nullptr;

    if( !clearance || clearance->m_children.size() != 2 )
        return -1;

    return static_cast<int>( std::lround( std::stod( clearance->Atom( 1 ) ) ) );
}


/// Name of the (class ...) listing \a aNet, or an empty string.
std::string classOfNet( const DSN_NODE& aRoot, const std::string& aNet )
{
    std::vector<const DSN_NODE*> classes;
    collectLists( aRoot, "class", classes );

    for( const DSN_NODE* clazz : classes )
    {
        for( size_t ii = 2; ii < clazz->m_children.size(); ++ii )
        {
            if( clazz->Atom( ii ) == aNet )
                return clazz->Atom( 1 );
        }
    }

    return std::string();
}


/// Clearance of a class to itself, from its own rule.
int selfClearance( const DSN_NODE& aRoot, const std::string& aClass )
{
    std::vector<const DSN_NODE*> classes;
    collectLists( aRoot, "class", classes );

    for( const DSN_NODE* clazz : classes )
    {
        if( clazz->Atom( 1 ) == aClass )
            return ruleClearance( clazz->Child( "rule" ) );
    }

    return -1;
}


/// Clearance between two classes, from their (class_class ...) rule.
int pairClearance( const DSN_NODE& aRoot, const std::string& aClassA, const std::string& aClassB )
{
    std::vector<const DSN_NODE*> pairs;
    collectLists( aRoot, "class_class", pairs );

    for( const DSN_NODE* pair : pairs )
    {
        const DSN_NODE* classes = pair->Child( "classes" );

        if( !classes || classes->m_children.size() != 3 )
            continue;

        if( ( classes->Atom( 1 ) == aClassA && classes->Atom( 2 ) == aClassB )
            || ( classes->Atom( 1 ) == aClassB && classes->Atom( 2 ) == aClassA ) )
        {
            return ruleClearance( pair->Child( "rule" ) );
        }
    }

    return -1;
}


bool endsWith( const std::string& aText, const std::string& aSuffix )
{
    return aText.size() > aSuffix.size()
           && aText.compare( aText.size() - aSuffix.size(), aSuffix.size(), aSuffix ) == 0;
}


/// The (clearance_class ...) of a pin in the (place ...) of \a aRef, or an empty string.
std::string pinClass( const DSN_NODE& aRoot, const std::string& aRef, const std::string& aPin )
{
    std::vector<const DSN_NODE*> places;
    collectLists( aRoot, "place", places );

    for( const DSN_NODE* place : places )
    {
        if( place->Atom( 1 ) != aRef )
            continue;

        for( const DSN_NODE& child : place->m_children )
        {
            if( child.Is( "pin" ) && child.Atom( 1 ) == aPin )
            {
                if( const DSN_NODE* clearanceClass = child.Child( "clearance_class" ) )
                    return clearanceClass->Atom( 1 );
            }
        }
    }

    return std::string();
}


/**
 * A small mains board: two relay channels (MAINS netclass, nets /Relais1/... and
 * /Relais2/...) and two low voltage nets.  K1 is a relay (contact pad 1, coil pad 2), U1 a
 * fine pitch SMD part, J1 a terminal with one contact of each channel.
 */
class GRUMPF_MAINS_BOARD
{
public:
    GRUMPF_MAINS_BOARD()
    {
        BOARD_DESIGN_SETTINGS&         bds = m_board.GetDesignSettings();
        std::shared_ptr<NET_SETTINGS>& netSettings = bds.m_NetSettings;

        netSettings->GetDefaultNetclass()->SetClearance( pcbIUScale.mmToIU( 0.2 ) );

        std::shared_ptr<NETCLASS> mains = std::make_shared<NETCLASS>( wxS( "MAINS" ) );
        mains->SetClearance( pcbIUScale.mmToIU( 2 ) );
        mains->SetTrackWidth( pcbIUScale.mmToIU( 2 ) );
        netSettings->SetNetclass( wxS( "MAINS" ), mains );
        netSettings->SetNetclassPatternAssignment( wxS( "/Relais*" ), wxS( "MAINS" ) );

        const VECTOR2I corners[4] = { { 0, 0 }, { pcbIUScale.mmToIU( 100 ), 0 },
                                      { pcbIUScale.mmToIU( 100 ), pcbIUScale.mmToIU( 100 ) },
                                      { 0, pcbIUScale.mmToIU( 100 ) } };

        for( int i = 0; i < 4; ++i )
        {
            PCB_SHAPE* edge = new PCB_SHAPE( &m_board, SHAPE_T::SEGMENT );
            edge->SetLayer( Edge_Cuts );
            edge->SetStart( corners[i] );
            edge->SetEnd( corners[( i + 1 ) % 4] );
            m_board.Add( edge );
        }

        const wxString netNames[] = { wxS( "SIG" ), wxS( "+5V" ), wxS( "/Relais1/COM" ),
                                      wxS( "/Relais1/NO" ), wxS( "/Relais2/COM" ) };
        int            netCode = 0;

        for( const wxString& name : netNames )
        {
            NETINFO_ITEM* net = new NETINFO_ITEM( &m_board, name, ++netCode );
            m_board.Add( net );
            m_nets[name] = net;
        }

        addFootprint( wxS( "K1" ), { 20, 20 }, false, { wxS( "/Relais1/COM" ), wxS( "+5V" ) } );
        addFootprint( wxS( "U1" ), { 60, 20 }, true, { wxS( "SIG" ), wxS( "+5V" ) } );
        addFootprint( wxS( "J1" ), { 20, 70 }, false,
                      { wxS( "/Relais1/NO" ), wxS( "/Relais2/COM" ) } );

        // SynchronizeNetsAndNetClasses() needs a project, which this board has not: resolve the
        // netclass patterns here, as it would.
        for( NETINFO_ITEM* net : m_board.GetNetInfo() )
            net->SetNetClass( netSettings->GetEffectiveNetClass( net->GetNetname() ) );
    }

    /// Load \a aRules as the board's custom rules.
    void SetRules( const std::string& aRules )
    {
        {
            std::ofstream out( m_rulesFile.Path() );
            out << aRules;
        }

        BOARD_DESIGN_SETTINGS&      bds = m_board.GetDesignSettings();
        std::shared_ptr<DRC_ENGINE> engine = std::make_shared<DRC_ENGINE>( &m_board, &bds );

        BOOST_REQUIRE_NO_THROW( engine->InitEngine( wxFileName( m_rulesFile.WxStr() ) ) );
        bds.m_DRCEngine = engine;
    }

    BOARD* Board() { return &m_board; }

private:
    void addFootprint( const wxString& aRef, const VECTOR2I& aPosMm, bool aSmd,
                       const std::vector<wxString>& aNets )
    {
        FOOTPRINT* footprint = new FOOTPRINT( &m_board );
        footprint->SetReference( aRef );
        footprint->SetPosition( { pcbIUScale.mmToIU( aPosMm.x ), pcbIUScale.mmToIU( aPosMm.y ) } );

        for( size_t ii = 0; ii < aNets.size(); ++ii )
        {
            PAD* pad = new PAD( footprint );
            pad->SetNumber( wxString::Format( wxS( "%d" ), static_cast<int>( ii + 1 ) ) );
            pad->SetSize( ::PADSTACK::ALL_LAYERS,
                          VECTOR2I( pcbIUScale.mmToIU( 1 ), pcbIUScale.mmToIU( 1 ) ) );

            if( aSmd )
            {
                pad->SetAttribute( PAD_ATTRIB::SMD );
                pad->SetLayerSet( PAD::SMDMask() );
                pad->SetDrillSize( VECTOR2I( 0, 0 ) );
            }

            pad->SetFPRelativePosition( { pcbIUScale.mmToIU( 5 ) * static_cast<int>( ii ), 0 } );
            pad->SetNet( m_nets.at( aNets[ii] ) );
            footprint->Add( pad );
        }

        m_board.Add( footprint );
    }

    BOARD                             m_board;
    std::map<wxString, NETINFO_ITEM*> m_nets;
    TEMP_FILE                         m_rulesFile{ wxT( "kicad_specctra_rules_" ) };
};


// The rules of a grumpf mains board, in the order that gives each its precedence.
const char* const GRUMPF_MAINS_RULES = R"RULES((version 1)
(rule "mains_tbts"
    (condition "A.NetClass == 'MAINS' && B.NetClass != 'MAINS'")
    (constraint clearance (min 8mm))
    (constraint creepage (min 8mm)))
(rule "between_channels"
    (condition "A.NetClass == 'MAINS' && B.NetClass == 'MAINS'")
    (constraint clearance (min 3mm)))
(rule "same_channel_1"
    (condition "A.NetName == '/Relais1/*' && B.NetName == '/Relais1/*'")
    (constraint clearance (min 2mm)))
(rule "fine_pitch_u1"
    (condition "A.NetClass != 'MAINS' && B.NetClass != 'MAINS' && (A.memberOfFootprint('U1') || B.memberOfFootprint('U1'))")
    (constraint clearance (min 0.125mm)))
(rule "coil_contacts_k1"
    (condition "A.NetClass == 'MAINS' && B.NetClass != 'MAINS' && A.memberOfFootprint('K1') && B.memberOfFootprint('K1')")
    (constraint clearance (min 7.8mm)))
(rule "wide"
    (constraint track_width (min 0.3mm)))
)RULES";
} // namespace


BOOST_AUTO_TEST_CASE( SpecctraExportCustomRuleClearances )
{
    LOCALE_IO          toggle;
    GRUMPF_MAINS_BOARD fixture;
    fixture.SetRules( GRUMPF_MAINS_RULES );

    WX_STRING_REPORTER reporter;
    TEMP_FILE          out( wxT( "kicad_specctra_rules_dsn_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( fixture.Board(), out.Str(), &reporter ) );

    const std::string dsn = slurp( out.Path() );
    const DSN_NODE    root = parseDsnBody( dsn );

    // A netclass split by a net name rule: one class per channel.
    const std::string channel1 = classOfNet( root, "/Relais1/COM" );
    const std::string channel2 = classOfNet( root, "/Relais2/COM" );
    const std::string lowVoltage = classOfNet( root, "SIG" );

    BOOST_REQUIRE_MESSAGE( !channel1.empty() && !channel2.empty() && !lowVoltage.empty(), dsn );
    BOOST_CHECK_EQUAL( classOfNet( root, "/Relais1/NO" ), channel1 );
    BOOST_CHECK( channel1 != channel2 );
    BOOST_CHECK_EQUAL( classOfNet( root, "+5V" ), lowVoltage );

    // The Default netclass keeps its usual DSN name.
    BOOST_CHECK_EQUAL( lowVoltage, "kicad_default" );

    // NetClass == / != rules and the net name wildcard, as class and class_class rules.
    BOOST_CHECK_EQUAL( selfClearance( root, channel1 ), 2000 );
    BOOST_CHECK_EQUAL( selfClearance( root, channel2 ), 3000 );
    BOOST_CHECK_EQUAL( selfClearance( root, lowVoltage ), 200 );
    BOOST_CHECK_EQUAL( pairClearance( root, channel1, channel2 ), 3000 );
    BOOST_CHECK_EQUAL( pairClearance( root, channel1, lowVoltage ), 8000 );
    BOOST_CHECK_EQUAL( pairClearance( root, channel2, lowVoltage ), 8000 );

    // memberOfFootprint rules, as pin clearance classes.  A pin names the per-item copy
    // Freerouting makes of its class: -pin for a through hole pad, -smd for an SMD pad.
    const std::string contactPin = pinClass( root, "K1", "1" );
    const std::string coilPin = pinClass( root, "K1", "2" );
    const std::string finePitchPin = pinClass( root, "U1", "1" );

    BOOST_REQUIRE_MESSAGE( endsWith( contactPin, "-pin" ) && endsWith( coilPin, "-pin" )
                                   && endsWith( finePitchPin, "-smd" ),
                           dsn );
    BOOST_CHECK_EQUAL( pinClass( root, "U1", "2" ), finePitchPin );

    // The class_class rules name the classes themselves.
    const std::string contact = contactPin.substr( 0, contactPin.size() - 4 );
    const std::string coil = coilPin.substr( 0, coilPin.size() - 4 );
    const std::string finePitch = finePitchPin.substr( 0, finePitchPin.size() - 4 );

    BOOST_CHECK( contact != coil );
    BOOST_CHECK_EQUAL( pairClearance( root, contact, coil ), 7800 );
    BOOST_CHECK_EQUAL( selfClearance( root, contact ), 2000 );
    BOOST_CHECK_EQUAL( pairClearance( root, contact, channel2 ), 3000 );
    BOOST_CHECK_EQUAL( pairClearance( root, contact, lowVoltage ), 8000 );
    BOOST_CHECK_EQUAL( pairClearance( root, coil, channel1 ), 8000 );
    BOOST_CHECK_EQUAL( pairClearance( root, coil, lowVoltage ), 200 );
    BOOST_CHECK_EQUAL( pairClearance( root, finePitch, lowVoltage ), 125 );
    BOOST_CHECK_EQUAL( pairClearance( root, finePitch, coil ), 125 );
    BOOST_CHECK_EQUAL( pairClearance( root, finePitch, channel1 ), 8000 );

    // Pads which behave like the tracks of their net keep their net's class.
    BOOST_CHECK( pinClass( root, "J1", "1" ).empty() );
    BOOST_CHECK( pinClass( root, "J1", "2" ).empty() );

    // What has no Specctra equivalent is reported, rule by rule.
    const wxString& messages = reporter.GetMessages();
    BOOST_CHECK_MESSAGE( messages.Contains( wxS( "'mains_tbts'" ) )
                                 && messages.Contains( wxS( "creepage" ) ),
                         messages );
    BOOST_CHECK_MESSAGE( messages.Contains( wxS( "'wide'" ) ), messages );
    BOOST_CHECK_MESSAGE( !messages.Contains( wxS( "'between_channels'" ) ), messages );

    // The file, class_class and pin clearance classes included, reads back.
    SPECCTRA_DB db;

    try
    {
        db.LoadPCB( out.WxStr() );
        BOOST_CHECK( db.GetPCB() != nullptr );
    }
    catch( const IO_ERROR& ioe )
    {
        BOOST_FAIL( ioe.What().ToStdString() );
    }
}


// A layer-specific clearance rule cannot be written per layer: it is exported with its largest
// value, and reported.
BOOST_AUTO_TEST_CASE( SpecctraExportLayerSpecificRule )
{
    LOCALE_IO          toggle;
    GRUMPF_MAINS_BOARD fixture;
    fixture.SetRules( R"RULES((version 1)
(rule "outer_mains"
    (layer outer)
    (condition "A.NetClass == 'MAINS' && B.NetClass != 'MAINS'")
    (constraint clearance (min 6mm)))
)RULES" );

    WX_STRING_REPORTER reporter;
    TEMP_FILE          out( wxT( "kicad_specctra_layer_rule_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( fixture.Board(), out.Str(), &reporter ) );

    const DSN_NODE root = parseDsnBody( slurp( out.Path() ) );

    BOOST_CHECK_EQUAL( pairClearance( root, classOfNet( root, "/Relais1/COM" ),
                                      classOfNet( root, "SIG" ) ),
                       6000 );
    BOOST_CHECK_MESSAGE( reporter.GetMessages().Contains( wxS( "'outer_mains'" ) ),
                         reporter.GetMessages() );
}


// Without custom clearance rules the netclasses are exported as before.
BOOST_AUTO_TEST_CASE( SpecctraExportWithoutCustomRules )
{
    LOCALE_IO          toggle;
    GRUMPF_MAINS_BOARD fixture;
    fixture.SetRules( "(version 1)\n" );

    TEMP_FILE out( wxT( "kicad_specctra_no_rules_" ) );
    BOOST_REQUIRE_NO_THROW( ExportBoardToSpecctraFile( fixture.Board(), out.Str() ) );

    const std::string dsn = slurp( out.Path() );
    const DSN_NODE    root = parseDsnBody( dsn );

    BOOST_CHECK( dsn.find( "(class_class" ) == std::string::npos );
    BOOST_CHECK( dsn.find( "clearance_class" ) == std::string::npos );
    BOOST_CHECK_EQUAL( classOfNet( root, "SIG" ), "kicad_default" );
    BOOST_CHECK_EQUAL( classOfNet( root, "/Relais1/COM" ), classOfNet( root, "/Relais2/COM" ) );
}

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

#define BOOST_TEST_MODULE MdwGit
#include <boost/test/unit_test.hpp>

#include <mdw/mdw_backend.h>
#include <mdw/mdw_backend_supervisor.h>
#include <mdw/mdw_dirty_tracker.h>
#include <mdw/mdw_event_bus.h>
#include <mdw/mdw_lock_tracker.h>
#include <mdw/mdw_session.h>
#include <mdw/mdw_workspace_service.h>
#include <mdw/client/mdw_op.h>
#include <mdw/client/mdw_source.h>

#include <mdw_runtime/mdw_backend_server.h>

#include <api/mdw/backend_base.pb.h>

#include <git2.h>

#include <qa_utils/env_var_utils.h>
#include <qa_utils/file_utils.h>

#include <wx/filename.h>
#include <wx/init.h>
#include <wx/utils.h>

#include "mdw_git_test_utils.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace
{

struct PROCESS_TEMP_DIR_FIXTURE
{
    KI_TEST::SCOPED_PROCESS_TEMP_DIR m_dir{ "kicad_qa_mdw_git" };
};


// MDW_FRAMEWORK finds its backends through wxStandardPaths, which asks the app for its traits
struct WX_APP_FIXTURE
{
    wxInitializer m_wx;
};

} // namespace


BOOST_GLOBAL_FIXTURE( PROCESS_TEMP_DIR_FIXTURE );
BOOST_GLOBAL_FIXTURE( WX_APP_FIXTURE );
BOOST_GLOBAL_FIXTURE( GIT_HOST_ISOLATION );
BOOST_GLOBAL_FIXTURE( UI_EVENT_ISOLATION );


BOOST_AUTO_TEST_SUITE( MdwGitAuth )


// A backend that locks but cannot list its locks reports them only as events, so free items must not read as unknown
BOOST_AUTO_TEST_CASE( UnlistedLocksAreKnownFromEvents )
{
    SCOPED_DATA_DIR    dataDir;
    MDW_BACKEND_SERVER server;
    const std::string  url = "ipc://" + ( dataDir.Path() / "control" ).string();

    server.RegisterHandler<kiapi::mdw::Hello, kiapi::mdw::HelloReply>(
            []( const kiapi::mdw::Hello&, kiapi::mdw::HelloReply& aReply )
            {
                aReply.mutable_capabilities()->add_groups( kiapi::workspace::CG_LOCKING );
                return MdwOk();
            } );

    BOOST_REQUIRE( server.Start( url, 1 ) );

    auto backend = std::make_shared<MDW_BACKEND>( url, wxEmptyString );
    BOOST_REQUIRE( backend->Start() );
    BOOST_REQUIRE( backend->Handshake( kiapi::mdw::Hello(), 8000 ) );

    auto                  locks = std::make_shared<MDW_LOCK_TRACKER>();
    MDW_WORKSPACE_SERVICE service( backend, std::make_shared<MDW_EVENT_BUS>(), locks,
                                   std::make_shared<MDW_DIRTY_TRACKER>(), wxS( "project" ), wxS( "carol" ) );

    BOOST_CHECK( service.ListLocks().error == MDW_ERROR_CODE::NOT_SUPPORTED );
    BOOST_CHECK( locks->Get( wxS( "board.kicad_sch" ) ).known );
}


// An upload backend can refuse a stale check in only if it learns which revision the file was edited from
BOOST_AUTO_TEST_CASE( SourceCheckinSendsItsBase )
{
    MDW_BACKEND_MANIFEST manifest;
    MDW_BACKEND_CONTEXT  context;

    manifest.backend_id = wxS( "org.kicad.mdw.qa.realm" );
    manifest.executable_path = wxString::FromUTF8( MDW_REALM_EXECUTABLE );
    context.workspaceId = wxS( "realm" );

    auto supervisor = std::make_unique<MDW_BACKEND_SUPERVISOR>( manifest, context );
    BOOST_REQUIRE( supervisor->Launch( 8000 ) );

    auto session = std::make_shared<MDW_SESSION>(
            MDW_PROJECT_BINDING{ manifest.backend_id, context.workspaceId, wxEmptyString }, std::move( supervisor ) );
    auto source = std::make_shared<MDW_SOURCE>( session );

    MDW_CHECKIN_REQUEST request;
    request.item_id = wxS( "1898/amp-board.kicad_sch" );
    request.local_path = wxS( "amp-board.kicad_sch" );
    request.base_revision_id = wxS( "7001" );

    auto [result, revision] = MdwSync<MDW_REVISION>(
            [&]( MDW_DONE<MDW_REVISION> aDone )
            {
                return source->Checkin( request, std::move( aDone ) );
            } );

    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK( revision.label == wxS( "7001" ) );
}


// A reply that names no path wrote where the checkout asked, so the caller still learns the working copy
BOOST_AUTO_TEST_CASE( SourceCheckoutNamesItsPathWhenTheReplyOmitsIt )
{
    MDW_BACKEND_MANIFEST manifest;
    MDW_BACKEND_CONTEXT  context;

    manifest.backend_id = wxS( "org.kicad.mdw.qa.realm" );
    manifest.executable_path = wxString::FromUTF8( MDW_REALM_EXECUTABLE );
    context.workspaceId = wxS( "realm" );

    auto supervisor = std::make_unique<MDW_BACKEND_SUPERVISOR>( manifest, context );
    BOOST_REQUIRE( supervisor->Launch( 8000 ) );

    auto session = std::make_shared<MDW_SESSION>(
            MDW_PROJECT_BINDING{ manifest.backend_id, context.workspaceId, wxEmptyString }, std::move( supervisor ) );
    auto source = std::make_shared<MDW_SOURCE>( session );

    MDW_CHECKOUT_REQUEST request;
    request.item_id = wxS( "1898/amp-board.kicad_sch" );
    request.local_path = wxS( "amp-board.kicad_sch" );

    auto [result, checkedOut] = MdwSync<MDW_CHECKOUT_RESULT>(
            [&]( MDW_DONE<MDW_CHECKOUT_RESULT> aDone )
            {
                return source->Checkout( request, std::move( aDone ) );
            } );

    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK( checkedOut.local_path == request.local_path );
}


// A local source sends no login form, so only the server check can sign its session in
BOOST_AUTO_TEST_CASE( MissingLocalSourceIsNotSignedIn )
{
    SCOPED_DATA_DIR     dataDir;
    MDW_BACKEND_CONTEXT library;
    library.workspaceId = wxS( "library" );
    library.locator = wxString::FromUTF8( ( dataDir.Path() / "missing" ).string() );

    auto supervisor = std::make_unique<MDW_BACKEND_SUPERVISOR>( gitManifest(), library );
    BOOST_REQUIRE( supervisor->Launch( 8000 ) );
    BOOST_REQUIRE( !supervisor->Backend()->AuthChallenge() );

    MDW_PROJECT_BINDING binding{ wxS( "org.kicad.mdw.git" ), library.workspaceId, library.locator };
    MDW_SESSION         session( binding, std::move( supervisor ) );

    BOOST_CHECK( session.Authenticate( MDW_TYPED_CREDENTIALS(), 8000 ).error == MDW_ERROR_CODE::NETWORK );
    BOOST_CHECK( session.Status().phase == MDW_CONNECTION::OFFLINE );
}


// A backend too old to check its server names no auth form and stays stopped, whatever its other calls answer
BOOST_AUTO_TEST_CASE( StaleBackendStaysStopped )
{
    KI_TEST::SCOPED_PROCESS_ENV_VAR stale( wxS( "QA_MDW_STALE" ), wxS( "1" ) );
    SCOPED_DATA_DIR                 dataDir;
    MDW_BACKEND_MANIFEST            manifest;
    manifest.backend_id = realmBinding().backend_id;
    manifest.executable_path = wxString::FromUTF8( MDW_REALM_EXECUTABLE );

    MDW_BACKEND_CONTEXT context;
    context.workspaceId = wxS( "stale" );

    auto supervisor = std::make_unique<MDW_BACKEND_SUPERVISOR>( manifest, context );
    BOOST_REQUIRE( supervisor->Launch( 8000 ) );

    MDW_SESSION session( { manifest.backend_id, context.workspaceId, wxEmptyString }, std::move( supervisor ) );

    BOOST_CHECK( !session.Backend()->AuthChallenge() );
    BOOST_REQUIRE( session.Reconnect( 8000 ).error == MDW_ERROR_CODE::PROTOCOL );
    BOOST_REQUIRE( session.Status().phase == MDW_CONNECTION::BACKEND_STOPPED );

    const MDW_RESULT tooOld = session.Status().reason;

    kiapi::mdw::AuthenticateRequest request;
    kiapi::mdw::AuthenticateReply   reply;
    BOOST_REQUIRE( session.Backend()->CallSync( request, reply, 8000 ).error == MDW_ERROR_CODE::NETWORK );

    BOOST_CHECK( session.Status().phase == MDW_CONNECTION::BACKEND_STOPPED );
    BOOST_CHECK( session.Status().reason.error == MDW_ERROR_CODE::PROTOCOL );
    BOOST_CHECK( session.Status().reason.message == tooOld.message );
}


BOOST_AUTO_TEST_SUITE_END()

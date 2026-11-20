/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2026 KiCad Developers, see AUTHORS.txt for contributors.
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


// A backend whose challenge names the server in QA_MDW_REALM and which takes any password, or with QA_MDW_STALE one
// built before CheckConnection reached the server, as an outdated install leaves one. With QA_MDW_KEY_FILE its form
// also asks for an SSH key and that key alone signs in

#include <mdw_runtime/mdw_backend_server.h>

#include <api/mdw/backend_base.pb.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#if defined( _WIN32 )
#include <process.h>
#else
#include <unistd.h>
#endif


int main( int argc, char** argv )
{
    MDW_BACKEND_LAUNCH     launch = MDW_ParseLaunch( argc, argv );
    MDW_BACKEND_SERVER     server;
    MDW_BACKEND_DESCRIPTOR descriptor;
    std::atomic<bool>      signedIn = false;

    // Tells a test which process to watch for its exit
    if( const char* pidFile = std::getenv( "QA_MDW_PID_FILE" ) )
    {
#if defined( _WIN32 )
        std::ofstream( pidFile ) << _getpid();
#else
        std::ofstream( pidFile ) << getpid();
#endif
    }

    descriptor.info.set_backend_id( "org.kicad.mdw.qa.realm" );
    descriptor.idempotency_scope = []( const kiapi::mdw::Hello& )
    {
        return std::string( "qa" );
    };

    const bool stale = std::getenv( "QA_MDW_STALE" ) != nullptr;

    if( !stale )
    {
        descriptor.auth = []( const kiapi::mdw::Hello& ) -> std::optional<kiapi::workspace::AuthChallenge>
        {
            kiapi::workspace::AuthChallenge challenge;
            const char*                     realm = std::getenv( "QA_MDW_REALM" );

            challenge.set_realm( realm ? realm : "" );

            if( std::getenv( "QA_MDW_KEY_FILE" ) )
                challenge.add_fields()->set_field_id( "ssh_key_path" );

            return challenge;
        };
    }

    server.RegisterHandler<kiapi::mdw::CheckConnectionRequest, kiapi::mdw::CheckConnectionReply>(
            [&]( const kiapi::mdw::CheckConnectionRequest&, kiapi::mdw::CheckConnectionReply& aReply )
            {
                // An old runtime answered NOT_SUPPORTED for a handler it lacked
                if( stale )
                    return MdwFail( kiapi::workspace::WE_NOT_SUPPORTED, "Not supported" );

                // A server that never answers, which the file tells a test the check has reached
                if( const char* hangFile = std::getenv( "QA_MDW_HANG_FILE" ) )
                {
                    std::ofstream( hangFile ) << "checking";
                    std::this_thread::sleep_for( std::chrono::seconds( 60 ) );
                }

                if( !signedIn )
                    return MdwFail( kiapi::workspace::WE_NOT_AUTHENTICATED, "Sign in" );

                aReply.mutable_account()->set_user_id( "carol" );
                return MdwOk();
            } );

    server.RegisterHandler<kiapi::mdw::AuthenticateRequest, kiapi::mdw::AuthenticateReply>(
            [&]( const kiapi::mdw::AuthenticateRequest& aRequest, kiapi::mdw::AuthenticateReply& aReply )
            {
                // Any other call fails as an unreachable server would
                if( stale )
                    return MdwFail( kiapi::workspace::WE_NETWORK, "Server unreachable" );

                const auto& fields = aRequest.credentials().fields();
                auto        password = fields.find( "password" );
                auto        key = fields.find( "ssh_key_path" );
                const char* keyFile = std::getenv( "QA_MDW_KEY_FILE" );

                signedIn = ( password != fields.end() && !password->second.empty() )
                           || ( keyFile && key != fields.end() && key->second == keyFile );
                aReply.set_authenticated( signedIn );
                aReply.mutable_account()->set_user_id( "carol" );

                // Tells a test which author the host named
                aReply.mutable_account()->set_display_name( server.HelloContext().author().display_name() );
                return MdwOk();
            } );

    // Names the base it was sent as the new revision's label, so a test sees what the client sent
    server.RegisterHandler<kiapi::mdw::CheckinRequest, kiapi::mdw::CheckinReply>(
            []( const kiapi::mdw::CheckinRequest& aRequest, kiapi::mdw::CheckinReply& aReply )
            {
                aReply.mutable_revision()->set_revision_id( "7002" );
                aReply.mutable_revision()->set_label( aRequest.base_revision_id() );
                return MdwOk();
            } );

    // Writes where it was asked and says nothing about it, as a backend without a working copy does
    server.RegisterHandler<kiapi::mdw::CheckoutRequest, kiapi::mdw::CheckoutReply>(
            []( const kiapi::mdw::CheckoutRequest& aRequest, kiapi::mdw::CheckoutReply& aReply )
            {
                aReply.mutable_item()->set_item_id( aRequest.item_id() );
                return MdwOk();
            } );

    return server.Run( launch, descriptor, nullptr );
}

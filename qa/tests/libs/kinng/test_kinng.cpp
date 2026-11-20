/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
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

#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <kinng.h>

#include <import_export.h>
#include <api/common/envelope.pb.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

// Windows sleeps whole 15.6 ms timer ticks, which would push every stop past every request
static void spinFor( std::chrono::microseconds aDuration )
{
    const auto until = std::chrono::steady_clock::now() + aDuration;

    while( std::chrono::steady_clock::now() < until )
        std::this_thread::yield();
}

// Counts wx assertions instead of reporting them, since these fire on a KINNG thread and are expected
struct COUNT_ASSERTIONS
{
    COUNT_ASSERTIONS() : m_previous( wxSetAssertHandler( &count ) ) { s_count = 0; }

    ~COUNT_ASSERTIONS() { wxSetAssertHandler( m_previous ); }

    static void count( const wxString&, int, const wxString&, const wxString&, const wxString& ) { ++s_count; }

    static inline std::atomic<int> s_count = 0;
    wxAssertHandler_t              m_previous;
};

BOOST_AUTO_TEST_SUITE( KiNNG )

BOOST_AUTO_TEST_CASE( CreateIPCResponder )
{
    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );

    KINNG_REQUEST_SERVER server( tempDir.Path().string() );
}

BOOST_AUTO_TEST_CASE( StopEndsPendingReceive, *boost::unit_test::timeout( 30 ) )
{
    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );
    KINNG_REQUEST_SERVER     server( "ipc://" + ( tempDir.Path() / "server" ).string() );

    BOOST_REQUIRE( server.Start() );

    // The listener is then inside a receive, which ends only at its 100 ms timeout
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

    auto start = std::chrono::steady_clock::now();
    server.Stop();
    auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_CHECK( !server.Running() );
    BOOST_CHECK_LT( std::chrono::duration_cast<std::chrono::milliseconds>( elapsed ).count(), 250 );
}

BOOST_AUTO_TEST_CASE( StopWhileDeadlinesExpire, *boost::unit_test::timeout( 30 ) )
{
    using STATUS = KINNG_REQUEST_CLIENT::STATUS;

    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );
    const std::string        url = "ipc://" + ( tempDir.Path() / "server" ).string();

    // A peer that takes requests and never answers, so every request ends by its deadline or by Stop
    KINNG_REQUEST_SERVER server( url );
    server.SetCallback( []( std::string* ) {} );
    BOOST_REQUIRE( server.Start() );

    std::atomic<int> accepted = 0;
    std::atomic<int> finished = 0;
    std::atomic<int> closed = 0;
    std::atomic<int> unexpected = 0;

    auto onDone = [&]( const KINNG_REQUEST_CLIENT::RESULT& aResult )
    {
        if( aResult.status == STATUS::CLOSED )
            ++closed;
        else if( aResult.status != STATUS::TIMED_OUT )
            ++unexpected;

        ++finished;
    };

    for( int i = 0; i < 200; ++i )
    {
        KINNG_REQUEST_CLIENT client( url );
        BOOST_REQUIRE( client.Start() );

        std::atomic<bool> issuing = false;

        // Requests keep arriving while Stop runs, so some are in flight and some race the close
        std::thread caller(
                [&]()
                {
                    for( int r = 0; r < 16; ++r )
                    {
                        if( client.Request( "ping", 1 + r % 3, onDone ) )
                            ++accepted;

                        issuing = true;
                        spinFor( std::chrono::microseconds( 100 ) );
                    }
                } );

        // Timed from the first request, since a thread can take milliseconds to start on Windows
        while( !issuing.load() )
            std::this_thread::yield();

        // Spread the stops across the deadlines so some land while nng is expiring a request
        spinFor( std::chrono::microseconds( ( i * 37 ) % 2000 ) );
        client.Stop();
        caller.join();
    }

    BOOST_CHECK_GT( accepted.load(), 0 );
    BOOST_CHECK_EQUAL( finished.load(), accepted.load() );
    BOOST_CHECK_GT( closed.load(), 0 );
    BOOST_CHECK_EQUAL( unexpected.load(), 0 );
}

BOOST_AUTO_TEST_CASE( StopEndsRequestsBeforeTheirDeadlines, *boost::unit_test::timeout( 30 ) )
{
    using STATUS = KINNG_REQUEST_CLIENT::STATUS;

    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );
    const std::string        url = "ipc://" + ( tempDir.Path() / "server" ).string();

    KINNG_REQUEST_SERVER server( url );
    server.SetCallback( []( std::string* ) {} );
    BOOST_REQUIRE( server.Start() );

    std::atomic<int> accepted = 0;
    std::atomic<int> closed = 0;

    auto onDone = [&]( const KINNG_REQUEST_CLIENT::RESULT& aResult )
    {
        if( aResult.status == STATUS::CLOSED )
            ++closed;
    };

    for( int i = 0; i < 200; ++i )
    {
        KINNG_REQUEST_CLIENT client( url );
        BOOST_REQUIRE( client.Start() );

        std::atomic<bool> issuing = false;

        // A deadline far past Stop's bound, so a request Stop misses fails the check below instead of hanging
        std::thread caller(
                [&]()
                {
                    for( int r = 0; r < 16; ++r )
                    {
                        if( client.Request( "ping", 10000, onDone ) )
                            ++accepted;

                        issuing = true;
                        spinFor( std::chrono::microseconds( 100 ) );
                    }
                } );

        while( !issuing.load() )
            std::this_thread::yield();

        spinFor( std::chrono::microseconds( ( i * 37 ) % 2000 ) );
        auto start = std::chrono::steady_clock::now();
        client.Stop();
        auto elapsed = std::chrono::steady_clock::now() - start;
        caller.join();

        BOOST_REQUIRE_LT( std::chrono::duration_cast<std::chrono::milliseconds>( elapsed ).count(), 1000 );
    }

    BOOST_CHECK_GT( accepted.load(), 0 );
    BOOST_CHECK_EQUAL( closed.load(), accepted.load() );
}

BOOST_AUTO_TEST_CASE( ServerStoppedFromItsCallbackAsserts, *boost::unit_test::timeout( 30 ) )
{
    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );
    const std::string        url = "ipc://" + ( tempDir.Path() / "server" ).string();
    COUNT_ASSERTIONS         assertions;
    std::promise<void>       called;
    std::future<void>        returned = called.get_future();

    KINNG_REQUEST_SERVER server( url );
    server.SetCallback(
            [&]( std::string* )
            {
                server.Stop();
                called.set_value();
            } );
    BOOST_REQUIRE( server.Start() );

    KINNG_REQUEST_CLIENT client( url );
    BOOST_REQUIRE( client.Start() );
    BOOST_REQUIRE( client.Request( "ping", 10000, []( const KINNG_REQUEST_CLIENT::RESULT& ) {} ) );

    BOOST_REQUIRE( returned.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready );
    BOOST_CHECK_EQUAL( COUNT_ASSERTIONS::s_count.load(), 1 );
    BOOST_CHECK( server.Running() );

    client.Stop();
    server.Stop();
    BOOST_CHECK( !server.Running() );
}

BOOST_AUTO_TEST_CASE( ClientStoppedFromItsCompletionAsserts, *boost::unit_test::timeout( 30 ) )
{
    KI_TEST::SCOPED_TEMP_DIR tempDir( "kicad_qa_kinng" );
    COUNT_ASSERTIONS         assertions;
    std::promise<void>       called;
    std::future<void>        returned = called.get_future();

    // Nothing listens, so the request times out on the reaper
    KINNG_REQUEST_CLIENT client( "ipc://" + ( tempDir.Path() / "server" ).string() );
    BOOST_REQUIRE( client.Start() );
    BOOST_REQUIRE( client.Request( "ping", 1,
                                   [&]( const KINNG_REQUEST_CLIENT::RESULT& )
                                   {
                                       client.Stop();
                                       called.set_value();
                                   } ) );

    BOOST_REQUIRE( returned.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready );
    BOOST_CHECK_EQUAL( COUNT_ASSERTIONS::s_count.load(), 1 );

    client.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

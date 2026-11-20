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

#include "mdw_git_library_fixture.h"

#include <api/mdw/backend_base.pb.h>
#include <api/mdw/backend_lifecycle.pb.h>
#include <api/mdw/backend_locking.pb.h>
#include <api/mdw/backend_revisions.pb.h>
#include <api/mdw/backend_shared_libs.pb.h>
#include <diff_merge/kicad_diff_types.h>
#include <git/git_library_members.h>
#include <git/kigit_symbol_split.h>
#include <mdw/client/mdw_op.h>

#include <json_common.h>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace mdw = kiapi::mdw;


namespace
{

const std::string ICS = "ics.kicad_sym";


std::map<std::string, mdw::LibraryMember> listMembers( MDW_BACKEND& aBackend )
{
    mdw::ListMembersRequest request;
    mdw::ListMembersReply   reply;
    request.set_library_id( LIBRARY );

    MDW_RESULT result = aBackend.CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    std::map<std::string, mdw::LibraryMember> members;

    for( const mdw::LibraryMember& member : reply.members() )
        members[member.member_id()] = member;

    BOOST_REQUIRE_EQUAL( members.size(), 2u );
    return members;
}


/// aText with every line ending as aEnd, CRLF as a save on Windows writes it or LF as the blob holds it
std::string lineEnds( const std::string& aText, const std::string& aEnd )
{
    std::string out;

    for( size_t ii = 0; ii < aText.size(); ++ii )
    {
        if( aText[ii] == '\r' && ii + 1 < aText.size() && aText[ii + 1] == '\n' )
            continue;

        if( aText[ii] == '\n' )
            out += aEnd;
        else
            out += aText[ii];
    }

    return out;
}


void checkIn( const MDW_GIT_LIBRARIAN& aLibrarian, const std::string& aId, const std::string& aIdempotency )
{
    const kiapi::workspace::WorkspaceError error = tryCheckIn( aLibrarian, aId, aIdempotency );
    BOOST_REQUIRE_MESSAGE( error.code() == kiapi::workspace::WE_OK, error.message() );
}


/// How aLibrarian's copy lists member aId among its local changes, if at all
std::optional<mdw::ChangeType> listedChange( const MDW_GIT_LIBRARIAN& aLibrarian, const std::string& aId )
{
    mdw::ListLocalChangesRequest request;
    mdw::ListLocalChangesReply   reply;
    request.set_local_root( aLibrarian.Copy().string() );

    MDW_RESULT result = aLibrarian.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    for( const mdw::LocalChange& change : reply.changes() )
    {
        if( change.member().item_id() == aId )
            return change.change();
    }

    return std::nullopt;
}


/// How many references of aRepo match aGlob
size_t refCount( git_repository* aRepo, const char* aGlob = "refs/kicad/kept/*" )
{
    size_t count = 0;

    git_reference_foreach_glob(
            aRepo, aGlob,
            []( const char*, void* aCount )
            {
                ++*static_cast<size_t*>( aCount );
                return 0;
            },
            &count );

    return count;
}


/// Alice and Bob, each with a copy of the passives library
class TWO_LIBRARIANS : public ONE_LIBRARIAN
{
public:
    TWO_LIBRARIANS()
    {
        m_bob = Connect( wxS( "bob" ) );

        m_bob.SyncOk();

        m_bob.SetAuthor( "bob" );
    }

    /// Alice releases L, edits it again while Bob checks in its keywords, and keeps her version as she gets his
    void keepMine()
    {
        const fs::path aliceFile = m_alice.Copy() / LIBRARY;

        mdw::ReleaseRevisionReply released;

        edit( aliceFile, "Inductor", "Inductor, 10uH" );
        MDW_RESULT result = release( *m_alice.backend, aliceFile, L, "", "v1:inductor-10u", released );
        BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

        MDW_LIBRARY_SYNC_RESULT updated = m_bob.SyncOk();

        edit( m_bob.Copy() / LIBRARY, L_KEYWORDS, "inductor choke" );
        claim( m_bob, L );
        checkIn( m_bob, L, "bob-keywords" );

        edit( aliceFile, "Inductor, 10uH", "Inductor, 22uH" );
        updated = m_alice.SyncOk();
        BOOST_REQUIRE_EQUAL( updated.conflicts.size(), 1u );

        const MDW_UPDATE_CONFLICT& conflict = updated.conflicts.front();

        updated = m_alice.Sync( { { conflict.item.item_id, conflict.theirs_revision_id, std::nullopt,
                                    conflict.ours_token } } );
        BOOST_REQUIRE_MESSAGE( updated.result.IsOk(), updated.result.message );
        BOOST_REQUIRE( updated.conflicts.empty() );
    }

    MDW_GIT_LIBRARIAN m_bob;
};

} // namespace


BOOST_AUTO_TEST_SUITE( MdwGitRelease )


// The listing names a member's release, never its last commit, so an unreleased part reads unreleased
BOOST_FIXTURE_TEST_CASE( ListingNamesOnlyReleases, MDW_GIT_LIBRARY_FIXTURE )
{
    fs::copy_file( Seed() / LIBRARY, Source() / LIBRARY );
    Publish( wxS( "Import acme-library" ) );
    MDW_GIT_LIBRARIAN librarian = Connect();

    librarian.SyncOk();

    MDW_BACKEND&                              backend = *librarian.backend;
    const fs::path                            file = librarian.Copy() / LIBRARY;
    std::map<std::string, mdw::LibraryMember> before = listMembers( backend );

    for( const auto& [id, member] : before )
    {
        BOOST_TEST_CONTEXT( id )
        {
            BOOST_CHECK_EQUAL( member.revision_id(), "" );
            BOOST_CHECK_EQUAL( member.revision_label(), "" );
            BOOST_CHECK_EQUAL( member.release_digest(), "" );
            BOOST_CHECK_EQUAL( member.releases_size(), 0 );
            BOOST_CHECK( !member.version().empty() );
        }
    }

    mdw::ReleaseRevisionReply released;
    MDW_RESULT                result = release( backend, file, C, "", "v1:capacitor", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    std::map<std::string, mdw::LibraryMember> after = listMembers( backend );

    BOOST_CHECK_EQUAL( after[C].revision_id(), released.revision().revision_id() );
    BOOST_CHECK_EQUAL( after[C].revision_label(), "1" );
    BOOST_CHECK_EQUAL( after[C].release_digest(), "v1:capacitor" );
    BOOST_CHECK_EQUAL( after[C].version(), before[C].version() );
    BOOST_CHECK_EQUAL( after[L].revision_id(), "" );
    BOOST_CHECK_EQUAL( after[L].version(), before[L].version() );

    // An edit released from the listing names the release it started from, none for L
    edit( file, "Inductor", "Inductor, 10uH" );
    result = release( backend, file, L, after[L].revision_id(), "v1:inductor-10u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    std::map<std::string, mdw::LibraryMember> edited = listMembers( backend );

    BOOST_CHECK_EQUAL( edited[L].revision_id(), released.revision().revision_id() );
    BOOST_CHECK_EQUAL( edited[L].version(), released.revision().revision_id() );
    BOOST_CHECK_NE( edited[L].version(), before[L].version() );

    edit( file, "Inductor, 10uH", "Inductor, 22uH" );
    result = release( backend, file, L, edited[L].revision_id(), "v1:inductor-22u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    // Placed parts are weighed against every release, so an outdated one is not read as unreleased
    std::map<std::string, mdw::LibraryMember> twice = listMembers( backend );

    BOOST_REQUIRE_EQUAL( twice[L].releases_size(), 2 );
    BOOST_CHECK_EQUAL( twice[L].releases( 0 ).label(), "1" );
    BOOST_CHECK_EQUAL( twice[L].releases( 0 ).content_digest(), "v1:inductor-10u" );
    BOOST_CHECK_EQUAL( twice[L].releases( 1 ).label(), "2" );
    BOOST_CHECK_EQUAL( twice[L].releases( 1 ).content_digest(), "v1:inductor-22u" );
    BOOST_CHECK_EQUAL( twice[C].releases_size(), 1 );

    // Rev 1 is superseded, so an edit from it would publish over Rev 2 unseen
    edit( file, "Inductor, 22uH", "Inductor, 47uH" );
    result = release( backend, file, L, edited[L].revision_id(), "v1:inductor-47u", released );
    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
}


// A teammate's release of C moves the branch, which must not refuse a release of L in the same file
BOOST_FIXTURE_TEST_CASE( ReleaseOfOneSymbolSurvivesTeammateReleaseOfAnotherInSameFile, TWO_LIBRARIANS )
{
    const fs::path aliceFile = m_alice.Copy() / LIBRARY;
    const fs::path bobFile = m_bob.Copy() / LIBRARY;

    mdw::ReleaseRevisionReply released;

    edit( aliceFile, C_FILTERS, "C? Capacitor*" );
    MDW_RESULT result = release( *m_alice.backend, aliceFile, C, "", "v1:capacitor-filters", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    edit( bobFile, "Inductor", "Inductor, 10uH" );
    result = release( *m_bob.backend, bobFile, L, "", "v1:inductor-10u", released );
    BOOST_CHECK_MESSAGE( result.error == MDW_ERROR_CODE::COPY_BEHIND, result.message );

    MDW_LIBRARY_SYNC_RESULT updated = m_bob.SyncOk();
    BOOST_REQUIRE( updated.conflicts.empty() );

    result = release( *m_bob.backend, bobFile, L, "", "v1:inductor-10u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK_EQUAL( released.revision().label(), "1" );

    const std::string shared = Upstream( LIBRARY );
    BOOST_CHECK( shared.find( "\"C? Capacitor*\"" ) != std::string::npos );
    BOOST_CHECK( shared.find( "\"Inductor, 10uH\"" ) != std::string::npos );
}


// A check-in since the release is what the copy holds, so an edit made after syncing it releases
BOOST_FIXTURE_TEST_CASE( ReleaseAfterSyncUsesTheCopyAsBase, TWO_LIBRARIANS )
{
    const fs::path aliceFile = m_alice.Copy() / LIBRARY;
    const fs::path bobFile = m_bob.Copy() / LIBRARY;

    mdw::ReleaseRevisionReply released;

    edit( aliceFile, "Inductor", "Inductor, 10uH" );
    MDW_RESULT result = release( *m_alice.backend, aliceFile, L, "", "v1:inductor-10u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    MDW_LIBRARY_SYNC_RESULT updated = m_bob.SyncOk();

    edit( bobFile, L_KEYWORDS, "inductor choke" );
    claim( m_bob, L );
    checkIn( m_bob, L, "bob-keywords" );

    updated = m_alice.SyncOk();
    BOOST_REQUIRE( updated.conflicts.empty() );

    edit( aliceFile, "Inductor, 10uH", "Inductor, 22uH" );
    std::map<std::string, mdw::LibraryMember> listed = listMembers( *m_alice.backend );

    result = release( *m_alice.backend, aliceFile, L, listed[L].revision_id(), "v1:inductor-22u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK_EQUAL( released.revision().label(), "2" );

    const std::string shared = Upstream( LIBRARY );
    BOOST_CHECK( shared.find( "\"Inductor, 22uH\"" ) != std::string::npos );
    BOOST_CHECK( shared.find( "\"inductor choke\"" ) != std::string::npos );
}


// A copy behind would be updated and released again, publishing over the first librarian's edit
BOOST_FIXTURE_TEST_CASE( SameMemberEditedByTwoLibrariansIsAConflict, TWO_LIBRARIANS )
{
    const fs::path aliceFile = m_alice.Copy() / LIBRARY;
    const fs::path bobFile = m_bob.Copy() / LIBRARY;

    mdw::ReleaseRevisionReply released;

    edit( aliceFile, "Inductor", "Inductor, 10uH" );
    edit( bobFile, "Inductor", "Inductor, 22uH" );

    MDW_RESULT result = release( *m_alice.backend, aliceFile, L, "", "v1:inductor-10u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    result = release( *m_bob.backend, bobFile, L, "", "v1:inductor-22u", released );
    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
    BOOST_CHECK_MESSAGE( result.message.Contains( wxS( "alice" ) ), result.message );

    // Naming Alice's release, as Bob's listing does once it heard of it, still leaves his edit begun before it
    std::map<std::string, mdw::LibraryMember> listed = listMembers( *m_bob.backend );
    BOOST_REQUIRE( !listed[L].revision_id().empty() );

    result = release( *m_bob.backend, bobFile, L, listed[L].revision_id(), "v1:inductor-22u", released );
    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
    BOOST_CHECK_MESSAGE( result.message.Contains( wxS( "alice" ) ), result.message );
    BOOST_CHECK( Upstream( LIBRARY ).find( "\"Inductor, 10uH\"" ) != std::string::npos );
}


// Content edited before a check-in that changed it would publish over that check-in unseen
BOOST_FIXTURE_TEST_CASE( StaleEditAfterPostReleaseCheckInIsRefused, TWO_LIBRARIANS )
{
    const fs::path file = m_alice.Copy() / LIBRARY;

    mdw::ReleaseRevisionReply released;

    edit( file, "Inductor", "Inductor, 10uH" );
    MDW_RESULT result = release( *m_alice.backend, file, L, "", "v1:inductor-10u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    // An editor still holding Rev 1 edits it while Bob checks in L's keywords
    edit( file, "Inductor, 10uH", "Inductor, 47uH" );
    const std::string stale = ReadFile( file );

    edit( file, "Inductor, 47uH", "Inductor, 10uH" );
    BOOST_REQUIRE( m_bob.Sync().result.IsOk() );
    edit( m_bob.Copy() / LIBRARY, L_KEYWORDS, "inductor choke" );
    claim( m_bob, L );
    checkIn( m_bob, L, "bob-keywords" );
    BOOST_REQUIRE( m_alice.Sync().result.IsOk() );

    std::ofstream( file, std::ios::binary | std::ios::trunc ) << stale;
    std::map<std::string, mdw::LibraryMember> listed = listMembers( *m_alice.backend );

    result = release( *m_alice.backend, file, L, listed[L].revision_id(), "v1:inductor-47u", released );
    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
    BOOST_CHECK_MESSAGE( result.message.Contains( wxS( "bob" ) ), result.message );

    const std::string shared = Upstream( LIBRARY );
    BOOST_CHECK( shared.find( "\"inductor choke\"" ) != std::string::npos );
    BOOST_CHECK( shared.find( "\"Inductor, 47uH\"" ) == std::string::npos );
}


// A member never released has no release to judge from, yet a stale edit must not publish over a check-in
BOOST_FIXTURE_TEST_CASE( StaleEditOfUnreleasedMemberIsRefused, TWO_LIBRARIANS )
{
    const fs::path file = m_alice.Copy() / LIBRARY;

    mdw::ReleaseRevisionReply released;

    edit( file, "Inductor", "Inductor, 47uH" );
    const std::string stale = ReadFile( file );

    edit( file, "Inductor, 47uH", "Inductor" );
    edit( m_bob.Copy() / LIBRARY, L_KEYWORDS, "inductor choke" );
    checkIn( m_bob, L, "bob-keywords" );
    BOOST_REQUIRE( m_alice.Sync().result.IsOk() );

    std::ofstream( file, std::ios::binary | std::ios::trunc ) << stale;

    MDW_RESULT result = release( *m_alice.backend, file, L, "", "v1:inductor-47u", released );
    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
    BOOST_CHECK_MESSAGE( result.message.Contains( wxS( "bob" ) ), result.message );

    const std::string shared = Upstream( LIBRARY );
    BOOST_CHECK( shared.find( "\"inductor choke\"" ) != std::string::npos );
    BOOST_CHECK( shared.find( "\"Inductor, 47uH\"" ) == std::string::npos );
}


// Keeping my version over a teammate's check-in must not leave the release refused while the copy holds theirs
BOOST_FIXTURE_TEST_CASE( KeepMineSyncThenReleaseSucceeds, TWO_LIBRARIANS )
{
    const fs::path file = m_alice.Copy() / LIBRARY;

    keepMine();

    git_repository* repo = nullptr;
    BOOST_REQUIRE_EQUAL( git_repository_open( &repo, m_alice.Copy().string().c_str() ), GIT_OK );
    KIGIT::GitRepositoryPtr   repoPtr( repo );
    mdw::ReleaseRevisionReply released;

    BOOST_REQUIRE_EQUAL( refCount( repo ), 1u );

    std::map<std::string, mdw::LibraryMember> listed = listMembers( *m_alice.backend );
    MDW_RESULT result = release( *m_alice.backend, file, L, listed[L].revision_id(), "v1:inductor-22u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK_EQUAL( released.revision().label(), "2" );

    const std::string shared = Upstream( LIBRARY );
    BOOST_CHECK( shared.find( "\"Inductor, 22uH\"" ) != std::string::npos );
    BOOST_CHECK( shared.find( "\"inductor choke\"" ) == std::string::npos );

    // The answer is this copy's alone and spent by the release
    BOOST_CHECK_EQUAL( refCount( repo ), 0u );
    BOOST_CHECK_EQUAL( refCount( m_upstreamRepo.get() ), 0u );
}


// A kept answer covers the version it was given against, not a check-in made after it
BOOST_FIXTURE_TEST_CASE( KeptAnswerDoesNotCoverALaterCheckIn, TWO_LIBRARIANS )
{
    const fs::path file = m_alice.Copy() / LIBRARY;

    keepMine();

    const std::string mine = ReadFile( file );
    const std::string theirs = Upstream( LIBRARY );

    edit( m_bob.Copy() / LIBRARY, "Choke_* *Coil* Inductor_* L_*", "L_*" );
    claim( m_bob, L );
    checkIn( m_bob, L, "bob-filters" );

    mdw::ReleaseRevisionReply                 released;
    std::map<std::string, mdw::LibraryMember> listed = listMembers( *m_alice.backend );
    MDW_RESULT result = release( *m_alice.backend, file, L, listed[L].revision_id(), "v1:inductor-22u", released );

    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
    BOOST_CHECK_MESSAGE( result.message.Contains( wxS( "bob" ) ), result.message );

    // Her edit dropped, the update brings Bob's check-in without a conflict, and a stale editor saves hers again
    std::ofstream( file, std::ios::binary | std::ios::trunc ) << theirs;
    BOOST_REQUIRE( m_alice.Sync().result.IsOk() );
    std::ofstream( file, std::ios::binary | std::ios::trunc ) << mine;

    listed = listMembers( *m_alice.backend );
    result = release( *m_alice.backend, file, L, listed[L].revision_id(), "v1:inductor-22u", released );

    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
    BOOST_CHECK_MESSAGE( result.message.Contains( wxS( "bob" ) ), result.message );
    BOOST_CHECK( Upstream( LIBRARY ).find( "\"Inductor, 22uH\"" ) == std::string::npos );
}


// Behind the team in the same file, a member nobody else touched is behind rather than in conflict
BOOST_FIXTURE_TEST_CASE( StaleCheckInIsCopyBehind, TWO_LIBRARIANS )
{
    edit( m_bob.Copy() / LIBRARY, L_KEYWORDS, "inductor choke" );
    checkIn( m_bob, L, "bob-keywords" );

    edit( m_alice.Copy() / LIBRARY, C_FILTERS, "C_*" );

    const std::string head = m_alice.Head();

    BOOST_CHECK_EQUAL( tryCheckIn( m_alice, C, "alice-filters" ).code(), kiapi::workspace::WE_COPY_BEHIND );
    BOOST_CHECK_EQUAL( m_alice.Head(), head );
    BOOST_CHECK( listedChange( m_alice, C ) == mdw::CT_MODIFIED );
}


// A footprint check-in behind the team commits nothing, so the edit stays listed
BOOST_FIXTURE_TEST_CASE( StaleFootprintCheckInIsCopyBehind, TWO_LIBRARIANS )
{
    const std::string name = "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal";
    const std::string file = "fp_resistors.pretty/" + name + ".kicad_mod";
    const std::string axial = "fp_resistors.pretty#" + name;
    const fs::path    pads = fs::path( KI_TEST::GetTestDataRootDir() ) / "pcbnew/exchange_pad_changes.pretty";

    fs::create_directories( ( Source() / file ).parent_path() );
    fs::copy_file( pads / ( name + ".kicad_mod" ), Source() / file );
    Publish( wxS( "Add the axial footprint" ) );
    m_alice.SyncOk();
    m_bob.SyncOk();

    edit( m_bob.Copy() / LIBRARY, L_KEYWORDS, "inductor choke" );
    checkIn( m_bob, L, "bob-keywords" );

    const fs::path footprint = m_alice.Copy() / file;
    std::string    text = ReadFile( footprint );
    const size_t   at = text.find( "(at 10.16 0)" );

    BOOST_REQUIRE( at != std::string::npos );
    text.replace( at, 12, "(at 10.16 1.27)" );
    std::ofstream( footprint, std::ios::binary | std::ios::trunc ) << text;

    const std::string head = m_alice.Head();

    BOOST_CHECK_EQUAL( tryCheckIn( m_alice, axial, "alice-pad" ).code(), kiapi::workspace::WE_COPY_BEHIND );
    BOOST_CHECK_EQUAL( m_alice.Head(), head );
    BOOST_CHECK( listedChange( m_alice, axial ) == mdw::CT_MODIFIED );
}


// Two edits of one member conflict by naming the teammate, and Alice keeps no commit
BOOST_FIXTURE_TEST_CASE( CheckInOverTeammateEditNamesTheTeammate, TWO_LIBRARIANS )
{
    edit( m_bob.Copy() / LIBRARY, L_KEYWORDS, "inductor choke" );
    checkIn( m_bob, L, "bob-keywords" );

    edit( m_alice.Copy() / LIBRARY, L_KEYWORDS, "inductor coil" );

    const std::string                      head = m_alice.Head();
    const kiapi::workspace::WorkspaceError refused = tryCheckIn( m_alice, L, "alice-keywords" );

    BOOST_CHECK_EQUAL( refused.code(), kiapi::workspace::WE_VERSION_CONFLICT );
    BOOST_CHECK_MESSAGE( refused.message().find( "bob" ) != std::string::npos, refused.message() );
    BOOST_CHECK_EQUAL( m_alice.Head(), head );
    BOOST_CHECK( Upstream( LIBRARY ).find( "\"inductor coil\"" ) == std::string::npos );
}


// A release pushes only its tag, so a copy that has not fetched it must still refuse an unclaimed edit
BOOST_FIXTURE_TEST_CASE( CheckInOfAMemberReleasedSinceIsDenied, TWO_LIBRARIANS )
{
    mdw::ReleaseRevisionReply released;
    MDW_RESULT result = release( *m_bob.backend, m_bob.Copy() / LIBRARY, L, "", "v1:inductor", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    edit( m_alice.Copy() / LIBRARY, L_KEYWORDS, "inductor choke" );

    const std::string head = m_alice.Head();

    BOOST_CHECK_EQUAL( tryCheckIn( m_alice, L, "alice-keywords" ).code(), kiapi::workspace::WE_PERMISSION_DENIED );
    BOOST_CHECK_EQUAL( m_alice.Head(), head );
    BOOST_CHECK( listedChange( m_alice, L ) == mdw::CT_MODIFIED );
}


// A state tag makes a released part obsolete, and a teammate reads it back
BOOST_FIXTURE_TEST_CASE( ObsoleteStateTagIsReadBack, TWO_LIBRARIANS )
{
    const fs::path aliceFile = m_alice.Copy() / LIBRARY;

    mdw::ReleaseRevisionReply released;

    edit( aliceFile, C_FILTERS, "C? Capacitor*" );
    MDW_RESULT result = release( *m_alice.backend, aliceFile, C, "", "v1:capacitor-filters", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    mdw::TransitionLifecycleRequest request;
    mdw::TransitionLifecycleReply   reply;
    request.set_item_id( C );
    request.set_target_state_id( "obsolete" );
    request.set_comment( "Replaced by C_Small" );
    request.set_idempotency_id( "alice-obsoletes-c" );

    result = m_alice.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK( reply.state().category() == kiapi::workspace::LC_RESTRICTED );

    m_bob.SyncOk();

    std::map<std::string, mdw::LibraryMember> members = listMembers( *m_bob.backend );

    BOOST_CHECK_EQUAL( members[C].state().state_id(), "obsolete" );
    BOOST_CHECK( members[C].state().category() == kiapi::workspace::LC_RESTRICTED );
    BOOST_CHECK_EQUAL( members[C].revision_label(), "1" );
    BOOST_CHECK( members[L].state().category() == kiapi::workspace::LC_EDITABLE );

    mdw::GetAvailableTransitionsRequest available;
    mdw::GetAvailableTransitionsReply   transitions;
    available.set_item_id( C );

    result = m_bob.backend->CallSync( available, transitions, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    std::set<std::string> targets;

    for( const kiapi::workspace::LifecycleTransition& transition : transitions.transitions() )
        targets.insert( transition.target_state_id() );

    BOOST_CHECK( targets == std::set<std::string>( { "released", "not_for_new_designs" } ) );
}


/// Move aLibrarian's newest release of C to aState under the retry key aOperation
MDW_RESULT transitionC( const MDW_GIT_LIBRARIAN& aLibrarian, const std::string& aState, const std::string& aOperation )
{
    mdw::TransitionLifecycleRequest request;
    mdw::TransitionLifecycleReply   reply;
    request.set_item_id( C );
    request.set_target_state_id( aState );
    request.set_idempotency_id( aOperation );

    return aLibrarian.backend->CallSync( request, reply, 30000 );
}


// The design check reads a release's state, so an obsolete part is not usable
BOOST_FIXTURE_TEST_CASE( ObsoletePartIsNotUsableInDesignCheck, TWO_LIBRARIANS )
{
    mdw::ReleaseRevisionReply released;

    edit( m_alice.Copy() / LIBRARY, C_FILTERS, "C? Capacitor*" );
    MDW_RESULT result = release( *m_alice.backend, m_alice.Copy() / LIBRARY, C, "", "v1:capacitor-filters", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    result = transitionC( m_alice, "obsolete", "alice-obsoletes-c" );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    mdw::CheckPlacedPartsRequest check;
    mdw::CheckPlacedPartsReply   facts;
    mdw::PlacedPart*             placed = check.add_parts();
    placed->mutable_item()->set_item_id( C );
    placed->set_content_digest( "v1:capacitor-filters" );

    result = m_bob.backend->CallSync( check, facts, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_REQUIRE_EQUAL( facts.facts_size(), 1 );
    BOOST_CHECK_EQUAL( facts.facts( 0 ).matched_state().state_id(), "obsolete" );

    const auto& statuses = facts.facts( 0 ).statuses();
    BOOST_CHECK( std::find( statuses.begin(), statuses.end(), mdw::PPS_NOT_USABLE ) != statuses.end() );
}


// Two librarians moving one release each take their own sequence
BOOST_FIXTURE_TEST_CASE( ConcurrentStateChangesTakeTheNextSequence, TWO_LIBRARIANS )
{
    mdw::ReleaseRevisionReply released;

    edit( m_alice.Copy() / LIBRARY, C_FILTERS, "C? Capacitor*" );
    MDW_RESULT result = release( *m_alice.backend, m_alice.Copy() / LIBRARY, C, "", "v1:capacitor-filters", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    result = transitionC( m_alice, "obsolete", "alice-obsoletes-c" );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    // Bob's copy has never seen Alice's tag, so his own numbering would reuse hers
    result = transitionC( m_bob, "not_for_new_designs", "bob-nrnd-c" );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK_EQUAL( refCount( m_upstreamRepo.get(), "refs/tags/kicad/state/*" ), 2u );

    m_alice.SyncOk();
    BOOST_CHECK_EQUAL( listMembers( *m_alice.backend )[C].state().state_id(), "not_for_new_designs" );
}


BOOST_FIXTURE_TEST_CASE( TeammateClaimShowsInItemInfo, TWO_LIBRARIANS )
{
    claim( m_alice, L );

    mdw::GetItemInfoRequest request;
    mdw::GetItemInfoReply   reply;
    request.set_item_id( L );

    MDW_RESULT result = m_bob.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_CHECK( reply.item().lock().is_locked() );
    BOOST_CHECK( !reply.item().lock().locked_by_me() );
}


/// The text of aFile at the copy's HEAD
std::string headText( const fs::path& aCopy, const std::string& aFile )
{
    git_repository* repo = nullptr;
    BOOST_REQUIRE_EQUAL( git_repository_open( &repo, aCopy.string().c_str() ), GIT_OK );
    KIGIT::GitRepositoryPtr repoPtr( repo );

    git_object* blob = nullptr;
    BOOST_REQUIRE_EQUAL( git_revparse_single( &blob, repo, ( "HEAD:" + aFile ).c_str() ), GIT_OK );
    KIGIT::GitObjectPtr blobPtr( blob );

    return std::string( static_cast<const char*>( git_blob_rawcontent( reinterpret_cast<git_blob*>( blob ) ) ),
                        git_blob_rawsize( reinterpret_cast<git_blob*>( blob ) ) );
}


/// Whether aFile at the copy's HEAD is what the copy stores for aOnDisk, line endings filtered as git does
bool headHolds( const fs::path& aCopy, const std::string& aFile, const fs::path& aOnDisk )
{
    return KIGIT::BlobDigest( headText( aCopy, aFile ) )
           == MDW_GIT_LIBRARY_FIXTURE::StoredDigest( aCopy, aFile, aOnDisk );
}


/// Files with a modified entry in the copy's worktree
size_t modifiedFiles( const fs::path& aCopy )
{
    git_repository* repo = nullptr;
    BOOST_REQUIRE_EQUAL( git_repository_open( &repo, aCopy.string().c_str() ), GIT_OK );
    KIGIT::GitRepositoryPtr repoPtr( repo );

    git_status_list* status = nullptr;
    BOOST_REQUIRE_EQUAL( git_status_list_new( &status, repo, nullptr ), GIT_OK );

    size_t count = git_status_list_entrycount( status );
    git_status_list_free( status );
    return count;
}


/// Alice with a copy of ics.kicad_sym and passives.kicad_sym, and the ics file as a save on another machine writes
/// it, with the same symbols at a different indent
class REFORMATTED_ICS : public MDW_GIT_LIBRARY_FIXTURE
{
public:
    REFORMATTED_ICS()
    {
        fs::copy_file( Seed() / ICS, Source() / ICS );
        fs::copy_file( Seed() / LIBRARY, Source() / LIBRARY );
        Publish( wxS( "Import acme-library" ) );
        m_alice = Connect( wxS( "alice" ) );

        m_alice.SyncOk();

        m_alice.SetAuthor( "alice" );

        std::string text = ReadFile( Seed() / ICS );

        for( size_t at = text.find( '\t' ); at != std::string::npos; at = text.find( '\t', at + 2 ) )
            text.replace( at, 1, "  " );

        m_reformatted = m_temp.Path() / ICS;
        std::ofstream( m_reformatted, std::ios::binary ) << text;
    }

    /// Save the reformatted file over alice's copy of it
    void reformat() const { fs::copy_file( m_reformatted, m_alice.Copy() / ICS, fs::copy_options::overwrite_existing ); }

    MDW_GIT_LIBRARIAN m_alice;
    fs::path          m_reformatted;
};


/// A check-in naming aWhole, each file with the digest of the bytes on disk unless aDigests overrides it
mdw::CheckInSharedLibraryRequest checkInWhole( const MDW_GIT_LIBRARIAN& aLibrarian, const std::set<std::string>& aWhole,
                                               const std::vector<std::string>& aItems = {} )
{
    mdw::CheckInSharedLibraryRequest request;
    request.set_local_root( aLibrarian.Copy().string() );
    request.set_comment( "Adopt re-saved files" );
    request.set_idempotency_id( "whole-" + std::to_string( aWhole.size() ) + std::to_string( aItems.size() ) );

    for( const std::string& id : aItems )
        request.add_item_ids( id );

    for( const std::string& file : aWhole )
        ( *request.mutable_whole_files() )[file] =
                KIGIT::BlobDigest( MDW_GIT_LIBRARY_FIXTURE::ReadFile( aLibrarian.Copy() / file ) );

    return request;
}


// A re-save on another OS rewrites the file but no symbol, so the check-in adopts the file as it is
BOOST_FIXTURE_TEST_CASE( CheckInAdoptsReformattedFile, REFORMATTED_ICS )
{
    reformat();

    mdw::CheckInSharedLibraryRequest request = checkInWhole( m_alice, { ICS } );
    mdw::CheckInSharedLibraryReply   reply;

    MDW_RESULT result = m_alice.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    BOOST_CHECK( headHolds( m_alice.Copy(), ICS, m_reformatted ) );
    BOOST_CHECK_EQUAL( modifiedFiles( m_alice.Copy() ), 0u );
}


// A member checked in by name is spliced, so a file naming it as whole is not committed over it
BOOST_FIXTURE_TEST_CASE( WholeFileYieldsToASplicedMember, REFORMATTED_ICS )
{
    reformat();

    mdw::CheckInSharedLibraryRequest request = checkInWhole( m_alice, { ICS }, { ICS + "#24C16" } );
    mdw::CheckInSharedLibraryReply   reply;

    MDW_RESULT result = m_alice.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    BOOST_CHECK( !headHolds( m_alice.Copy(), ICS, m_reformatted ) );
}


// Another file's edit and an adopted file go in one commit, each as its own rules say
BOOST_FIXTURE_TEST_CASE( WholeFileRidesWithAnotherFilesEdit, REFORMATTED_ICS )
{
    reformat();
    edit( m_alice.Copy() / LIBRARY, "Inductor", "Inductor, 10uH" );

    mdw::CheckInSharedLibraryRequest request = checkInWhole( m_alice, { ICS }, { L } );
    mdw::CheckInSharedLibraryReply   reply;

    MDW_RESULT result = m_alice.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    BOOST_CHECK( headHolds( m_alice.Copy(), ICS, m_reformatted ) );
    BOOST_CHECK( headText( m_alice.Copy(), LIBRARY ).find( "Inductor, 10uH" ) != std::string::npos );
    BOOST_CHECK_EQUAL( modifiedFiles( m_alice.Copy() ), 0u );
}


// Ticking only a formatting row must not check in the edits left unticked beside it
BOOST_FIXTURE_TEST_CASE( WholeFileAloneLeavesUntickedEditsAlone, REFORMATTED_ICS )
{
    reformat();
    edit( m_alice.Copy() / LIBRARY, "Inductor", "Inductor, 10uH" );

    mdw::CheckInSharedLibraryRequest request = checkInWhole( m_alice, { ICS } );
    mdw::CheckInSharedLibraryReply   reply;

    MDW_RESULT result = m_alice.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    BOOST_CHECK( headHolds( m_alice.Copy(), ICS, m_reformatted ) );
    BOOST_CHECK( headText( m_alice.Copy(), LIBRARY ).find( "Inductor, 10uH" ) == std::string::npos );
    BOOST_CHECK_EQUAL( modifiedFiles( m_alice.Copy() ), 1u );
}


// A file edited after the dialog listed it is not the file the user saw
BOOST_FIXTURE_TEST_CASE( WholeFileRefusedWhenItChangedSinceListed, REFORMATTED_ICS )
{
    reformat();

    mdw::CheckInSharedLibraryRequest request = checkInWhole( m_alice, { ICS } );
    std::ofstream( m_alice.Copy() / ICS, std::ios::binary | std::ios::app ) << "\n";

    mdw::CheckInSharedLibraryReply reply;
    MDW_RESULT                     result = m_alice.backend->CallSync( request, reply, 30000 );

    BOOST_CHECK( result.error == MDW_ERROR_CODE::VERSION_CONFLICT );
    BOOST_CHECK( headHolds( m_alice.Copy(), ICS, Seed() / ICS ) );
}


// A refused push of files adopted whole is an error, or the adoption would read as done
BOOST_FIXTURE_TEST_CASE( WholeFileRefusedPushIsAnError, REFORMATTED_ICS )
{
    edit( Source() / LIBRARY, L_KEYWORDS, "inductor choke" );
    Publish( wxS( "L: fewer keywords" ) );
    reformat();

    const std::string              head = m_alice.Head();
    mdw::CheckInSharedLibraryReply reply;
    MDW_RESULT result = m_alice.backend->CallSync( checkInWhole( m_alice, { ICS } ), reply, 30000 );

    BOOST_CHECK( !result.IsOk() );
    BOOST_CHECK_EQUAL( m_alice.Head(), head );
    BOOST_CHECK( headHolds( m_alice.Copy(), ICS, Seed() / ICS ) );
}


// A released member's file takes the release rules, which a re-save cannot skip by being named whole
BOOST_FIXTURE_TEST_CASE( WholeFileWithAReleasedMemberIsNotAdopted, TWO_LIBRARIANS )
{
    const fs::path aliceFile = m_alice.Copy() / LIBRARY;

    mdw::ReleaseRevisionReply released;

    edit( aliceFile, "Inductor", "Inductor, 10uH" );
    MDW_RESULT result = release( *m_alice.backend, aliceFile, L, "", "v1:inductor-10u", released );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    const std::string before = headText( m_alice.Copy(), LIBRARY );

    edit( aliceFile, "Inductor, 10uH", "Inductor, 22uH" );

    mdw::CheckInSharedLibraryRequest request = checkInWhole( m_alice, { LIBRARY } );
    mdw::CheckInSharedLibraryReply   reply;

    result = m_alice.backend->CallSync( request, reply, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    BOOST_CHECK( headText( m_alice.Copy(), LIBRARY ) == before );
    BOOST_REQUIRE_EQUAL( reply.skipped_files_size(), 1 );
    BOOST_CHECK_EQUAL( reply.skipped_files( 0 ).path(), LIBRARY );
    BOOST_CHECK( !reply.skipped_files( 0 ).reason().empty() );
}


/// A differ that reads a symbol as edited when its text holds EDITED, standing in for the symbol differ on a
/// re-saved file
KIGIT::LIBRARY_DIFF editedWordDiff()
{
    return []( const std::string&, const std::string& aAfter, KICAD_DIFF::DOCUMENT_DIFF* aDiff )
    {
        if( aAfter.find( "EDITED" ) != std::string::npos )
            aDiff->changes.emplace_back();

        return true;
    };
}


/// What the check-in dialog lists for aLibrarian's copy
MDW_LIBRARY_CHANGES_RESULT listChanges( const MDW_GIT_LIBRARIAN& aLibrarian, const wxString& aMarker )
{
    aLibrarian.session->Probe( 30000 );
    BOOST_REQUIRE( aLibrarian.source->Info().connection.phase == MDW_CONNECTION::ONLINE );

    return MDW_SETUP::LibraryChangesFor( aLibrarian.source, *aLibrarian.sources->Copy( aMarker ), editedWordDiff() );
}


// A file with only reformatted symbols lists as one row that adopts it, and a check-in of that row alone commits it
BOOST_FIXTURE_TEST_CASE( LibraryListsReformattedFileAsOneRow, REFORMATTED_ICS )
{
    reformat();

    const wxString             marker = MDW_LIBRARY_SOURCES::Marker( m_alice.config );
    MDW_LIBRARY_CHANGES_RESULT listed = listChanges( m_alice, marker );
    BOOST_REQUIRE_MESSAGE( listed.result.IsOk(), listed.result.message );
    BOOST_REQUIRE_EQUAL( listed.changes.size(), 1u );
    BOOST_CHECK_EQUAL( listed.changes[0].relative_path, wxString::FromUTF8( ICS ) );
    BOOST_CHECK_EQUAL( listed.changes[0].format_digest,
                       wxString::FromUTF8( KIGIT::BlobDigest( ReadFile( m_reformatted ) ) ) );

    MDW_LIBRARY_CHECKIN_RESULT checkedIn = MDW_SETUP::CheckInLibraryCopy(
            *m_alice.sources, m_alice.source, *m_alice.sources->Copy( marker ), listed.changes, wxS( "Adopt" ) );

    BOOST_REQUIRE_MESSAGE( checkedIn.result.IsOk(), checkedIn.result.message );
    BOOST_CHECK( headHolds( m_alice.Copy(), ICS, m_reformatted ) );
    BOOST_CHECK_EQUAL( modifiedFiles( m_alice.Copy() ), 0u );
    BOOST_CHECK( m_alice.sources->Copy( marker )->snapshot.token == checkedIn.checkin.copy_snapshot.token );
}


// A file with a real edit lists through its members, never as a file adopted whole
BOOST_FIXTURE_TEST_CASE( LibraryDoesNotListAnEditedFileAsFormatOnly, REFORMATTED_ICS )
{
    std::string  text = ReadFile( m_reformatted );
    const size_t at = text.find( "\"24C16\"" );

    BOOST_REQUIRE( at != std::string::npos );
    text.replace( at, 7, "\"24C16 EDITED\"" );
    std::ofstream( m_alice.Copy() / ICS, std::ios::binary | std::ios::trunc ) << text;

    const wxString             marker = MDW_LIBRARY_SOURCES::Marker( m_alice.config );
    MDW_LIBRARY_CHANGES_RESULT listed = listChanges( m_alice, marker );
    BOOST_REQUIRE_MESSAGE( listed.result.IsOk(), listed.result.message );

    bool edited = false;

    for( const MDW_LOCAL_CHANGE& change : listed.changes )
    {
        BOOST_CHECK( change.format_digest.IsEmpty() );
        edited |= change.name == wxS( "24C16" );
    }

    BOOST_CHECK( edited );
}


// A part saved back on Windows is CRLF on disk and still matches the team library's LF blob of it
BOOST_FIXTURE_TEST_CASE( SavedBackPartMatchesAcrossLineEndings, MDW_GIT_LIBRARY_FIXTURE )
{
    fs::copy_file( Seed() / LIBRARY, Source() / LIBRARY );
    Publish( wxS( "Import acme-library" ) );
    MDW_GIT_LIBRARIAN librarian = Connect();

    librarian.SyncOk();

    const mdw::LibraryMember member = listMembers( *librarian.backend ).at( L );
    const fs::path           saved = librarian.Copy() / LIBRARY;
    MDW_CONFLICT_SIDES       side;

    side.conflict.item.item_id = wxString::FromUTF8( L );
    side.conflict.name = wxString::FromUTF8( member.name() );
    side.conflict.relative_path = wxString::FromUTF8( LIBRARY );
    side.oursFile = wxString::FromUTF8( saved.string() );
    side.theirsFile = m_temp.ChildPathStr( wxS( "theirs.kicad_sym" ) );

    auto [downloaded, revision] = MdwSync<MDW_REVISION>(
            [&]( MDW_DONE<MDW_REVISION> aDone )
            {
                return librarian.source->Download( side.conflict.item, wxString::FromUTF8( member.version() ),
                                                   side.theirsFile, std::move( aDone ) );
            } );
    BOOST_REQUIRE_MESSAGE( downloaded.IsOk(), downloaded.message );

    const std::string crlf = lineEnds( ReadFile( saved ), "\r\n" );

    std::ofstream( saved, std::ios::binary | std::ios::trunc ) << crlf;

    BOOST_CHECK( MDW_SETUP::SidesMatch( side ) );
}


// An LF answer written into a part saved on Windows keeps the whole file CRLF
BOOST_FIXTURE_TEST_CASE( AnsweredPartKeepsLineEndings, MDW_GIT_LIBRARY_FIXTURE )
{
    const fs::path      saved = m_temp.Path() / LIBRARY;
    const fs::path      answered = m_temp.Path() / "answer.kicad_sym";
    MDW_UPDATE_CONFLICT conflict;

    conflict.name = wxS( "L" );
    conflict.relative_path = wxString::FromUTF8( LIBRARY );

    std::ofstream( answered, std::ios::binary ) << lineEnds( ReadFile( Seed() / LIBRARY ), "\n" );
    edit( answered, "Inductor", "Inductor, 10uH" );
    std::ofstream( saved, std::ios::binary ) << lineEnds( ReadFile( Seed() / LIBRARY ), "\r\n" );

    BOOST_REQUIRE( MDW_SETUP::WriteMember( wxString::FromUTF8( saved.string() ), conflict, ReadFile( answered ) ) );
    BOOST_CHECK( ReadFile( saved ) == lineEnds( ReadFile( answered ), "\r\n" ) );
}


// A claim git cannot enforce can be broken with a reason, and the holder hears who broke it and why
BOOST_FIXTURE_TEST_CASE( AdvisoryClaimBrokenWithReasonIsAnnouncedToHolder, TWO_LIBRARIANS )
{
    claim( m_alice, L );

    mdw::GetLockInfoRequest query;
    mdw::GetLockInfoReply   held;
    query.set_item_id( L );

    MDW_RESULT result = m_bob.backend->CallSync( query, held, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_REQUIRE( held.lock().is_locked() );
    BOOST_CHECK( held.lock().advisory() );
    BOOST_CHECK( held.lock().lock_timestamp_unix() != 0 );

    mdw::UnlockRequest release;
    mdw::UnlockReply   released;
    release.set_item_id( L );
    release.set_force( true );
    release.set_reason( "left company" );
    release.set_idempotency_id( "bob-releases-l" );

    result = m_bob.backend->CallSync( release, released, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );

    // Bob breaks her claim to take it, so Alice's next fetch finds his claim where hers was
    claim( m_bob, L );

    // Alice restarts, so her backend asks the remote instead of answering from its last fetch
    Reconnect( m_alice );
    BOOST_REQUIRE( m_alice.session->StartEventIngress() );

    std::mutex               mutex;
    std::condition_variable  arrived;
    std::optional<MDW_EVENT> unlocked;

    MDW_SUBSCRIPTION events = m_alice.session->EventBus().Subscribe(
            [&]( const MDW_EVENT& aEvent )
            {
                std::lock_guard<std::mutex> lock( mutex );

                if( aEvent.type == MDW_EVENT_TYPE::ITEM_UNLOCKED && aEvent.item_id == wxString::FromUTF8( L ) )
                    unlocked = aEvent;

                arrived.notify_all();
            } );

    mdw::ListLocksRequest list;
    mdw::ListLocksReply   locks;

    result = m_alice.backend->CallSync( list, locks, 30000 );
    BOOST_REQUIRE_MESSAGE( result.IsOk(), result.message );
    BOOST_REQUIRE_EQUAL( locks.locks_size(), 1 );
    BOOST_CHECK_EQUAL( locks.locks( 0 ).lock().owner_display_name(), "bob" );

    std::unique_lock<std::mutex> lock( mutex );

    BOOST_REQUIRE( arrived.wait_for( lock, std::chrono::seconds( 10 ),
                                     [&]()
                                     {
                                         return unlocked.has_value();
                                     } ) );

    nlohmann::json payload = nlohmann::json::parse( unlocked->payload.utf8_string(), nullptr, false );

    BOOST_REQUIRE( payload.is_object() );
    BOOST_CHECK_EQUAL( payload.value( "owner_display_name", "" ), "alice" );
    BOOST_CHECK_EQUAL( payload.value( "released_by", "" ), "bob" );
    BOOST_CHECK_EQUAL( payload.value( "reason", "" ), "left company" );
    BOOST_CHECK( payload.value( "locked_by_me", false ) );
    BOOST_CHECK( !payload.value( "released_by_me", true ) );
}


// The tracker is the only store of claims, so Alice must learn of Bob's without asking, and not read it as free before
BOOST_FIXTURE_TEST_CASE( ClaimsAreReadOnceEventsFlow, TWO_LIBRARIANS )
{
    claim( m_bob, L );
    Reconnect( m_alice );

    const wxString    item = wxString::FromUTF8( L );
    MDW_LOCK_TRACKER& tracker = m_alice.session->LockTracker();

    BOOST_CHECK( !tracker.Get( item ).known );

    BOOST_REQUIRE( m_alice.session->StartEventIngress() );
    BOOST_REQUIRE( m_alice.session->Reconnect( 30000 ).IsOk() );

    for( int i = 0; i < 200 && !tracker.Get( item ).is_locked; ++i )
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );

    BOOST_CHECK_EQUAL( tracker.Get( item ).owner_display_name, wxS( "bob" ) );

    // Bob may let go while no events arrive, so his claim must not read as current then
    tracker.Invalidate();
    BOOST_CHECK( !tracker.Get( item ).known );
}


// Undo Check Out names no file, as Odoo then restores none, so the saved edits must still read as unreleased
BOOST_FIXTURE_TEST_CASE( UndoCheckOutKeepsEditsMarked, TWO_LIBRARIANS )
{
    MDW_SOURCE&        source = *m_alice.source;
    MDW_DIRTY_TRACKER& dirty = m_alice.session->DirtyTracker();
    const MDW_ITEM_REF item{ wxString::FromUTF8( LIBRARY ), wxString(), source.Info().locator };

    dirty.SetDirty( item.item_id );

    auto [cancelled, none] = MdwSync<MDW_NONE>(
            [&]( MDW_DONE<MDW_NONE> aDone )
            {
                return source.CancelCheckout( item, wxEmptyString, std::move( aDone ) );
            } );

    BOOST_REQUIRE_MESSAGE( cancelled.IsOk(), cancelled.message );
    BOOST_CHECK( dirty.IsDirty( item.item_id ) );
}

BOOST_AUTO_TEST_SUITE_END()

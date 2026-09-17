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
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <widgets/mdw_part_controller.h>

#include <limits>

#include <gestfich.h>
#include <libraries/library_table.h>
#include <mdw/client/mdw_baseline_design.h>
#include <mdw/client/mdw_client.h>
#include <mdw/client/mdw_library_sources.h>
#include <mdw/client/mdw_overwritten_parts.h>
#include <mdw/client/mdw_paged_read.h>
#include <mdw/client/mdw_part.h>
#include <mdw/client/mdw_pending_changes.h>
#include <mdw/client/mdw_source.h>
#include <confirm.h>
#include <eda_base_frame.h>
#include <dialogs/dialog_mdw_release.h>
#include <libraries/library_manager.h>
#include <lib_id.h>
#include <pgm_base.h>
#include <widgets/mdw_editing.h>

#include <wx/choicdlg.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/textdlg.h>

#include <algorithm>


namespace
{

/// History is read a page at a time until the latest release, giving up after HISTORY_PAGES pages
constexpr unsigned HISTORY_PAGE = 50;
constexpr int      HISTORY_PAGES = 20;


// An attempt still awaiting approval, or one turned down, keeps the release kind but not a released state
bool isRelease( const MDW_REVISION& aRevision )
{
    return aRevision.kind == MDW_REVISION_KIND::RELEASE
           && ( aRevision.state.category == MDW_LIFECYCLE_CATEGORY::RELEASED
                || aRevision.state.category == MDW_LIFECYCLE_CATEGORY::RESTRICTED );
}


/// A copy of aPath under its own name in a private folder, which goes with the last reference to it
std::shared_ptr<const wxString> snapshotForUpload( const wxString& aPath )
{
    struct SNAPSHOT
    {
        SCOPED_TEMP_PATH folder;
        wxString         path;
    };

    auto snapshot = std::make_shared<SNAPSHOT>();

    if( !snapshot->folder.MakeDirectory( wxS( "mdw-release" ), 0700 ) )
        return nullptr;

    snapshot->path = wxFileName( snapshot->folder.Path(), wxFileName( aPath ).GetFullName() ).GetFullPath();

    if( !wxCopyFile( aPath, snapshot->path, true ) )
        return nullptr;

    return std::shared_ptr<const wxString>( snapshot, &snapshot->path );
}

/// The revision aTarget's next release becomes, when its listed label is a number
wxString nextRevision( const MDW_PART_TARGET& aTarget )
{
    const wxString label = aTarget.listed ? aTarget.listed->member.revision_label : wxString();
    long           number = 0;

    if( label.IsEmpty() )
        return wxString::Format( _( "Rev %d" ), 1 );

    if( label.ToLong( &number ) )
        return wxString::Format( _( "Rev %ld" ), number + 1 );

    return _( "Next revision" );
}


/// What releasing the symbol aItem should know of the footprint it names, which a team may release later
struct FOOTPRINT_FACT
{
    wxString text;
    bool     confirm = false; ///< The release needs the user's say-so
};


/// True when the footprint library row names a file for aId
bool footprintFileExists( const LIB_ID& aId )
{
    std::optional<wxString> uri = Pgm().GetLibraryManager().GetFullURI( LIBRARY_TABLE_TYPE::FOOTPRINT,
                                                                        aId.GetLibNickname() );

    return uri
           && wxFileExists( wxFileName( *uri, aId.GetUniStringLibItemName() + wxS( ".kicad_mod" ) ).GetFullPath() );
}


FOOTPRINT_FACT footprintFact( const MDW_RELEASE_ITEM& aItem )
{
    LIB_ID footprintId;

    if( aItem.footprint.IsEmpty() || footprintId.Parse( aItem.footprint ) >= 0 || !footprintId.IsValid() )
        return {};

    std::optional<MDW_PART_FACTS> facts =
            MDW_MEMBER_INDEX::Get().LatestFacts( MDW_LIBRARY_TYPE::FOOTPRINT, footprintId );

    if( !facts || facts->statuses.front() == kiapi::mdw::PPS_RELEASED )
        return {};

    const bool exists = facts->statuses.front() != kiapi::mdw::PPS_UNKNOWN || footprintFileExists( footprintId );

    return { wxString::Format( _( "%s uses the footprint %s, %s." ), aItem.name, footprintId.GetUniStringLibId(),
                               MdwFootprintFactText( *facts, exists ) ),
             MdwReleaseNeedsConfirmation( *facts ) };
}

} // namespace


wxString MdwFootprintFactText( const MDW_PART_FACTS& aFacts, bool aExists )
{
    const kiapi::mdw::PlacedPartStatus status = aFacts.statuses.front();

    if( status == kiapi::mdw::PPS_UNKNOWN && !aExists )
        return _( "which does not exist" );

    if( status == kiapi::mdw::PPS_UNVERIFIED )
        return wxString::Format( _( "whose release could not be checked: %s" ), MdwPlacedStatusText( aFacts ) );

    return wxString::Format( _( "which is not released: %s" ), MdwPlacedStatusText( aFacts ) );
}


MDW_RELEASE_PROMPT MdwReleasePrompt( const std::vector<MDW_RELEASE_ITEM>& aItems )
{
    MDW_RELEASE_PROMPT prompt;
    bool               concerned = false;
    bool               saves = false;

    for( const MDW_RELEASE_ITEM& item : aItems )
    {
        prompt.rows.emplace_back( item.name, nextRevision( item.target ) );
        prompt.concerns.emplace_back();

        if( item.modified )
        {
            saves = true;
            prompt.notes.Add(
                    wxString::Format( _( "%s has unsaved changes. Releasing saves them first." ), item.name ) );
        }

        // The library is saved whole, though only this part is uploaded
        if( !item.unsaved.empty() )
        {
            saves = true;
            prompt.notes.Add( wxString::Format( _( "Saving %s also saves %s from the same library, unreleased." ),
                                                item.name, wxJoin( item.unsaved, ',' ) ) );
        }

        if( FOOTPRINT_FACT fact = footprintFact( item ); fact.confirm )
            prompt.concerns.back() = fact.text;
        else if( !fact.text.IsEmpty() )
            prompt.notes.Add( fact.text );

        concerned |= !prompt.concerns.back().IsEmpty();
    }

    if( saves )
        prompt.okLabel = concerned ? _( "Save and Release Anyway" ) : _( "Save and Release" );
    else
        prompt.okLabel = concerned ? _( "Release Anyway" ) : _( "Release" );

    return prompt;
}


MDW_RELEASE_TALLY MdwReleaseTally( const std::vector<MDW_RELEASE_ROW>& aRows, size_t aStopAt )
{
    MDW_RELEASE_TALLY tally;

    for( size_t row = 0; row < aRows.size(); ++row )
    {
        if( aRows[row].held )
            continue;

        if( row >= aStopAt )
            ++tally.stopped;
        else if( aRows[row].outcome == MDW_RELEASE_ROW::OUTCOME::RELEASED )
            ++tally.released;
        else if( aRows[row].outcome == MDW_RELEASE_ROW::OUTCOME::FAILED )
            ++tally.failed;
    }

    return tally;
}


bool MdwReleaseCanStop( const std::vector<MDW_RELEASE_ROW>& aRows, size_t aRow )
{
    for( size_t row = aRow + 1; row < aRows.size(); ++row )
    {
        if( !aRows[row].held )
            return true;
    }

    return false;
}


wxString MdwReleaseSummary( const MDW_RELEASE_TALLY& aTally )
{
    if( aTally.stopped == 0 )
        return wxString();

    wxString summary = wxString::Format( wxPLURAL( "%zu part released, ", "%zu parts released, ", aTally.released ),
                                         aTally.released );

    if( aTally.failed > 0 )
    {
        summary += wxString::Format( wxPLURAL( "%zu part failed, ", "%zu parts failed, ", aTally.failed ),
                                     aTally.failed );
    }

    return summary + wxString::Format( wxPLURAL( "%zu part stopped before release.",
                                                 "%zu parts stopped before release.", aTally.stopped ),
                                       aTally.stopped );
}


MDW_PART_STATE MDW_PART_TARGET::ListedState() const
{
    MDW_PART_STATE state;
    state.lifecycle = source && source->Has( MDW_CAPABILITY_GROUP::LIFECYCLE );
    state.listed = listed.has_value();

    if( !listed )
        return state;

    state.state = listed->member.state;
    state.known = true;
    state.lock = listed->lock;

    if( !listed->member.revision_id.IsEmpty() )
    {
        MDW_REVISION release;
        release.revision_id = listed->member.revision_id;
        release.label = listed->member.revision_label;
        release.kind = MDW_REVISION_KIND::RELEASE;
        release.content_digest = listed->member.release_digest;
        state.release = release;
    }

    return state;
}


std::vector<MDW_PART_ACTION> MDW_PART_TARGET::ListedActions() const
{
    using ID = MDW_PART_ACTION::ID;

    const MDW_PART_STATE         state = ListedState();
    std::vector<MDW_PART_ACTION> actions = state.Actions();

    if( !listed || !state.lifecycle )
        return actions;

    // Only a release has a state to leave, and the source names the moves once the part opens
    if( state.release && state.state.category != MDW_LIFECYCLE_CATEGORY::PENDING_REVIEW )
        actions.emplace_back( ID::TRANSITION );

    if( source->Has( MDW_CAPABILITY_GROUP::REVISIONS ) )
        actions.emplace_back( ID::HISTORY );

    return actions;
}


std::optional<MDW_PART_TARGET> MdwPartTarget( MDW_LIBRARY_TYPE aType, const LIBRARY_TABLE_ROW& aRow,
                                              const wxString& aName, const wxString& aLocalPath )
{
    std::optional<MDW_TEAM_LIBRARY> binding = MdwTeamLibrary( aType, aRow );

    if( !binding )
        return std::nullopt;

    const std::map<wxString, MDW_MEMBER_ENTRY>* members = MDW_MEMBER_INDEX::Get().Members( *binding );

    if( !members )
        return std::nullopt;

    return MdwPartTarget( *binding, *members, aName, aLocalPath, MDW_LIBRARY_SOURCES::MarkerOf( aRow.Options() ) );
}


MDW_PART_TARGET MdwPartTarget( const MDW_TEAM_LIBRARY& aBinding, const std::map<wxString, MDW_MEMBER_ENTRY>& aMembers,
                               const wxString& aName, const wxString& aLocalPath, const wxString& aMarker )
{
    MDW_PART_TARGET target;
    target.source = aBinding.source;
    target.item.source = aBinding.source->Info().locator;
    target.localPath = aLocalPath;
    target.marker = aMarker;

    if( auto entry = aMembers.find( aName ); entry != aMembers.end() )
    {
        target.item = MdwMemberRef( aBinding, entry->second.member );
        target.listed = entry->second;
    }

    return target;
}


MDW_PART_CONTROLLER::MDW_PART_CONTROLLER( wxEvtHandler* aOwner, MDW_PART_EDITOR& aEditor,
                                          std::function<void()> aChanged ) :
        m_owner( aOwner ),
        m_editor( aEditor ),
        m_changed( std::move( aChanged ) ),
        m_lifetime( std::make_shared<bool>( true ) ),
        m_integration( MDW_LIBRARY_SOURCES::TeamLibrariesEnabled() )
{
    // Releases left pending by an earlier session are asked about once their sources connect
    MDW_PENDING_CHANGES::Get().Follow();
}


MDW_PART_CONTROLLER::~MDW_PART_CONTROLLER()
{
    m_lifetime.reset();

    // A release the backend already took must still be remembered when it answers
    for( auto& [key, op] : m_partOps )
        op.Detach();
}


void MDW_PART_CONTROLLER::SetPart( std::optional<MDW_PART_TARGET> aTarget, const wxString& aName, const wxString& aKey )
{
    bool samePart = m_target && aTarget && m_target->source == aTarget->source
                    && m_target->item.item_id == aTarget->item.item_id;

    // An outcome and a pending release belong to their part and survive only a reread of it
    std::optional<MDW_PENDING_CHANGE> pending = samePart ? m_state.pending : std::nullopt;

    if( !samePart )
    {
        m_outcome.clear();
        m_offerGetLatest = false;
        m_pendingRead = MDW_OP();
        m_chooseTransition = false;
    }

    m_reads.clear();
    m_transitionsRead = false;
    m_events.Reset();
    m_target = std::move( aTarget );
    m_name = aName;
    m_key = aKey;
    m_state = MDW_PART_STATE();
    m_states.clear();
    m_readError.clear();

    if( !m_target || !m_target->source )
    {
        m_target.reset();
        return;
    }

    m_state = m_target->ListedState();
    m_state.pending = pending;

    // A release still waiting on its approver from an earlier session
    if( !m_state.pending )
    {
        for( const MDW_PENDING_ENTRY& entry : MDW_PENDING_CHANGES::Get().For( m_target->item ) )
        {
            if( entry.kind == MDW_PENDING_KIND::PART_REVISION )
                m_state.pending = entry.change;
        }
    }

    const wxString locator = m_target->source->Info().locator;

    m_events = MDW_CLIENT::Get().Subscribe( m_owner,
            [this, locator]( const MDW_EVENT& aEvent )
            {
                if( !m_target || aEvent.source != locator )
                    return;

                if( aEvent.type == MDW_EVENT_TYPE::PENDING_RESOLVED && m_state.pending
                    && aEvent.change_id == m_state.pending->change_id )
                {
                    readPending( aEvent.change_id );
                }
                else if( aEvent.item_id == m_target->item.item_id
                         || ( aEvent.type == MDW_EVENT_TYPE::LIBRARY_CHANGED && m_target->listed
                              && aEvent.item_id == m_target->listed->member.library_id )
                         || aEvent.type == MDW_EVENT_TYPE::WORKSPACE_RECONNECTED
                         || aEvent.type == MDW_EVENT_TYPE::CAPABILITIES_CHANGED )
                {
                    Refresh();
                }
            } );

    read();
}


void MDW_PART_CONTROLLER::Refresh()
{
    // A replaced source answers nothing, and reading it would drop a released part's protection
    if( !m_target || SourceReplaced() )
        return;

    m_reads.clear();
    m_state.lifecycle = m_target->source->Has( MDW_CAPABILITY_GROUP::LIFECYCLE );
    read();
}


void MDW_PART_CONTROLLER::read()
{
    std::shared_ptr<MDW_SOURCE> source = m_target->source;
    MDW_PART                    part = source->Part( m_target->item );

    // A part its library does not list has no item to read yet, only the source's permission to check it in
    if( !m_target->listed )
    {
        if( source->Has( MDW_CAPABILITY_GROUP::PERMISSIONS ) )
        {
            m_reads.push_back( source->Permissions(
                    MDW_ITEM_REF(), wxEmptyString,
                    [this]( const MDW_RESULT& aResult, const std::vector<kiapi::workspace::Permission>& aPermissions )
                    {
                        if( !aResult.IsOk() )
                            return;

                        m_state.sourcePermissions = aPermissions;
                        changed();
                    } ) );
        }

        return;
    }

    m_reads.push_back( source->Item( m_target->item, wxEmptyString,
            [this]( const MDW_RESULT& aResult, const MDW_ITEM_INFO& aItem )
            {
                if( !aResult.IsOk() )
                {
                    m_readError = MdwErrorText( aResult );
                    changed();
                    return;
                }

                // The reply's id is canonical
                m_target->item.item_id = aItem.item_id;
                m_readError.clear();
                m_state.state = aItem.state;
                m_state.known = true;
                m_state.lock = aItem.lock;
                changed();
            } ) );

    if( source->Has( MDW_CAPABILITY_GROUP::REVISIONS ) )
        readHistory();

    if( source->Has( MDW_CAPABILITY_GROUP::PERMISSIONS ) )
    {
        // Read each time, so a new sign-in is seen without a restart
        m_reads.push_back( source->Permissions(
                MDW_ITEM_REF(), wxEmptyString,
                [this]( const MDW_RESULT& aResult, const std::vector<kiapi::workspace::Permission>& aPermissions )
                {
                    if( !aResult.IsOk() )
                        return;

                    m_state.sourcePermissions = aPermissions;
                    changed();
                } ) );

        m_reads.push_back( source->Permissions(
                m_target->item, wxEmptyString,
                [this]( const MDW_RESULT& aResult, const std::vector<kiapi::workspace::Permission>& aPermissions )
                {
                    if( !aResult.IsOk() )
                        return;

                    m_state.permissions = aPermissions;
                    changed();
                } ) );
    }

    if( m_state.lifecycle )
    {
        m_reads.push_back( part.Definition(
                [this]( const MDW_RESULT& aResult, const kiapi::workspace::LifecycleDefinition& aDefinition )
                {
                    if( !aResult.IsOk() )
                        return;

                    m_states.assign( aDefinition.states().begin(), aDefinition.states().end() );
                    changed();
                } ) );

        m_reads.push_back( part.Transitions(
                wxEmptyString,
                [this, alive = std::weak_ptr<bool>( m_lifetime )](
                        const MDW_RESULT&                                         aResult,
                        const std::vector<kiapi::workspace::LifecycleTransition>& aTransitions )
                {
                    m_transitionsRead = true;

                    if( aResult.IsOk() )
                        m_state.transitions = aTransitions;

                    // Deferred, since the choice is a modal dialog
                    if( std::exchange( m_chooseTransition, false ) )
                    {
                        m_owner->CallAfter(
                                [this, alive]()
                                {
                                    if( !alive.expired() )
                                        chooseTransition();
                                } );
                    }

                    changed();
                } ) );
    }
}


void MDW_PART_CONTROLLER::readHistory()
{
    // Check-ins since the release can fill whole pages, so pages are read until one holds it
    m_reads.push_back( MdwReadAllPages<MDW_REVISION>(
            [src = m_target->source, item = m_target->item]( const MDW_PAGE&                        aPage,
                                                             MDW_DONE<MDW_PAGED<MDW_REVISION>> aDone )
            {
                return src->History( item, aPage, std::move( aDone ) );
            },
            HISTORY_PAGE,
            [this]( const MDW_RESULT& aResult, const MDW_PAGED<MDW_REVISION>& aHistory )
            {
                if( !aResult.IsOk() )
                    return;

                auto release = std::find_if( aHistory.rows.begin(), aHistory.rows.end(), isRelease );

                // History without a release proves nothing against the listing, which may name one
                if( release != aHistory.rows.end() )
                    m_state.release = *release;

                changed();
            },
            MDW_READ_CAP::PARTIAL, HISTORY_PAGES,
            []( const MDW_PAGED<MDW_REVISION>& aPage )
            {
                return std::any_of( aPage.rows.begin(), aPage.rows.end(), isRelease );
            } ) );
}


void MDW_PART_CONTROLLER::readPending( const wxString& aChangeId )
{
    m_pendingRead = m_target->source->PendingChange( aChangeId,
            [this, aChangeId]( const MDW_RESULT& aResult, const MDW_PENDING_INFO& aInfo )
            {
                if( !aResult.IsOk() || aInfo.status == MDW_PENDING_STATUS::PENDING )
                    return;

                wxString change = aInfo.change.display_id.IsEmpty() ? m_name : aInfo.change.display_id;

                if( aInfo.status == MDW_PENDING_STATUS::APPLIED )
                    m_outcome = wxString::Format( _( "Released %s as Rev %s" ), m_name, aInfo.revision.label );
                else if( aInfo.reason.IsEmpty() )
                    m_outcome = wxString::Format( _( "%s was not approved." ), change );
                else
                    m_outcome = wxString::Format( _( "%s was not approved: %s" ), change, aInfo.reason );

                MDW_PENDING_CHANGES::Get().Forget( m_target->source->Info().locator, aChangeId );
                m_state.pending.reset();
                Refresh();
                changed();
            } );
}


void MDW_PART_CONTROLLER::SetDigest( const wxString& aDigest, const wxString& aMemberDigest )
{
    m_state.digest = aDigest;
    m_state.memberDigest = aMemberDigest;
}


MDW_PART_CONTROLLER::PART_KEY MDW_PART_CONTROLLER::partKey() const
{
    return { m_target->source->Info().locator, m_target->item.item_id };
}


MDW_PART_CONTROLLER::PROVENANCE* MDW_PART_CONTROLLER::provenance()
{
    return m_target ? &m_provenance[partKey()] : nullptr;
}


const MDW_PART_CONTROLLER::PROVENANCE* MDW_PART_CONTROLLER::provenance() const
{
    if( !m_target )
        return nullptr;

    auto it = m_provenance.find( partKey() );
    return it == m_provenance.end() ? nullptr : &it->second;
}


bool MDW_PART_CONTROLLER::busy( const PART_KEY& aKey ) const
{
    auto running = m_partOps.find( aKey );
    return running != m_partOps.end() && !running->second.IsDone();
}


bool MDW_PART_CONTROLLER::IsReleased( const std::optional<MDW_PART_TARGET>& aTarget )
{
    return aTarget && aTarget->ListedState().IsReadOnly();
}


bool MDW_PART_CONTROLLER::RefuseReleased( wxWindow* aParent, bool aReleased, const wxString& aName )
{
    if( aReleased )
    {
        DisplayInfoMessage( aParent, wxString::Format( _( "%s is released. Open it and choose Revise to change it." ),
                                                       aName ) );
    }

    return aReleased;
}


void MDW_PART_CONTROLLER::ListenForListings( const std::function<void()>&                          aResolve,
                                             const std::function<void( const MDW_MEMBER_CHANGES& )>& aTail )
{
    if( m_listing )
        return;

    // A library listed after its part opened, or a reconnect's new source, retargets the open part
    m_listing = MDW_MEMBER_INDEX::Get().Listen(
            [this, aResolve, aTail]( const MDW_MEMBER_CHANGES& aChanges )
            {
                // Each source stopping at quit notifies, and a closed editor's tree is never shown again
                if( m_editor.PartFrame()->IsClosing() )
                    return;

                if( NeedsResolving() )
                    aResolve();

                aTail( aChanges );
            } );
}


bool MDW_PART_CONTROLLER::IsReadOnly() const
{
    const PROVENANCE* known = provenance();

    // A claim the backend reports must not unlock content the editor has not reloaded, and the
    // file a release is uploading must not change under it
    return m_target
           && ( ( known && ( known->reloadPending || known->releasing || known->settling ) ) || m_state.IsReadOnly() );
}


bool MDW_PART_CONTROLLER::SourceReplaced() const
{
    if( !m_target || !m_target->source )
        return false;

    std::shared_ptr<MDW_SOURCE> current = MDW_CLIENT::Get().FindSource( m_target->source->Info().locator );

    return current && current != m_target->source;
}


std::vector<MDW_PART_ACTION> MDW_PART_CONTROLLER::Actions() const
{
    using ID = MDW_PART_ACTION::ID;

    if( !m_target )
        return {};

    const PROVENANCE*            known = provenance();
    std::vector<MDW_PART_ACTION> actions = m_state.Actions();

    // The editor still shows the content before the checkout, or the file is being uploaded
    if( known && ( known->reloadPending || known->releasing ) )
    {
        std::erase_if( actions,
                       []( const MDW_PART_ACTION& aAction )
                       {
                           return aAction.id == ID::RELEASE || aAction.id == ID::CANCEL_REVISION;
                       } );
    }

    // The claim is held and only the reload failed, so Revise retries it
    if( known && known->reloadPending && m_state.IsRevising() )
        actions.insert( actions.begin(), MDW_PART_ACTION( ID::REVISE ) );

    if( m_target->listed && m_state.lifecycle && m_target->source->Has( MDW_CAPABILITY_GROUP::REVISIONS ) )
        actions.emplace_back( ID::HISTORY );

    return actions;
}


bool MDW_PART_CONTROLLER::Allows( MDW_PART_ACTION::ID aId ) const
{
    return std::ranges::any_of( Actions(),
                                [aId]( const MDW_PART_ACTION& aAction )
                                {
                                    return aAction.id == aId && aAction.Allowed();
                                } );
}


wxString MDW_PART_CONTROLLER::Message() const
{
    if( !m_target )
        return wxEmptyString;

    MDW_PART_STATE state = m_state;

    auto named = std::find_if( m_states.begin(), m_states.end(),
                               [&]( const kiapi::workspace::CustomState& aState )
                               {
                                   return aState.state_id() == state.state.state_id.utf8_string();
                               } );

    if( named != m_states.end() )
        state.stateName = wxString::FromUTF8( named->display_name() );

    wxString line = state.Describe( m_name );

    if( !m_readError.IsEmpty() && state.lifecycle )
        line += wxS( "\n" ) + wxString::Format( _( "Could not read the state of %s: %s" ), m_name, m_readError );

    wxString outcome = m_outcome;

    // Showing another part clears the outcome that said so
    if( outcome.IsEmpty() && SettlePending() )
        outcome = wxString::Format( _( "Release %s to load the version you chose for it." ), m_name );

    if( !outcome.IsEmpty() )
        line += ( line.IsEmpty() ? wxString() : wxString( wxS( "\n" ) ) ) + outcome;

    return line;
}


wxString MDW_PART_CONTROLLER::Label( const MDW_PART_ACTION& aAction )
{
    switch( aAction.id )
    {
    case MDW_PART_ACTION::ID::CHECK_IN:        return _( "Check In..." );
    case MDW_PART_ACTION::ID::REVISE:          return _( "Revise" );
    case MDW_PART_ACTION::ID::CANCEL_REVISION: return _( "Cancel Revision" );
    case MDW_PART_ACTION::ID::RELEASE:         return _( "Release..." );
    case MDW_PART_ACTION::ID::HISTORY:         return _( "History..." );
    case MDW_PART_ACTION::ID::TRANSITION:
        return aAction.label.IsEmpty() ? wxString( _( "Change State..." ) ) : aAction.label;
    }

    return wxEmptyString;
}


wxString MDW_PART_CONTROLLER::Refusal( const MDW_PART_ACTION& aAction )
{
    switch( aAction.id )
    {
    case MDW_PART_ACTION::ID::CHECK_IN:
        return wxString::Format( _( "You may not check it in: %s" ), aAction.reason );

    case MDW_PART_ACTION::ID::REVISE:
        return wxString::Format( _( "You may not revise it: %s" ), aAction.reason );

    case MDW_PART_ACTION::ID::RELEASE:
        return wxString::Format( _( "You may not release it: %s" ), aAction.reason );

    default:
        return wxString::Format( _( "%s is not allowed: %s" ), Label( aAction ), aAction.reason );
    }
}


std::vector<MDW_PART_ACTION> MDW_PART_CONTROLLER::ActionsOf( const MDW_PART_TARGET& aTarget ) const
{
    if( m_target && aTarget.source == m_target->source && aTarget.item.item_id == m_target->item.item_id )
        return Actions();

    return aTarget.ListedActions();
}


std::optional<MDW_PART_ACTION> MDW_PART_CONTROLLER::Offered( const std::vector<MDW_PART_TARGET>& aTargets,
                                                             MDW_PART_ACTION::ID aId ) const
{
    std::optional<MDW_PART_ACTION> offered;

    for( const MDW_PART_TARGET& target : aTargets )
    {
        for( const MDW_PART_ACTION& action : ActionsOf( target ) )
        {
            if( action.id == aId && ( !offered || ( !offered->Allowed() && action.Allowed() ) ) )
                offered = action;
        }
    }

    return offered;
}


void MDW_PART_CONTROLLER::RefuseEdit( wxWindow* aParent, std::function<bool()> aRestore )
{
    // One edit can be refused several times, as the properties dialog does, and the user needs telling once
    if( m_refusalQueued )
        return;

    m_refusalQueued = true;

    // Deferred so the dialog or panel that made the edit has finished with it
    aParent->CallAfter(
            [this, aParent, restore = std::move( aRestore )]()
            {
                m_refusalQueued = false;

                if( restore() )
                {
                    DisplayInfoMessage( aParent, wxString::Format( _( "%s is released, so the change was not kept. "
                                                                      "Choose Revise to change it." ),
                                                                   m_name ) );
                }
            } );
}


bool MDW_PART_CONTROLLER::IntegrationToggled()
{
    bool enabled = MDW_LIBRARY_SOURCES::TeamLibrariesEnabled();
    bool toggled = enabled != m_integration;

    m_integration = enabled;
    return toggled;
}


void MDW_PART_CONTROLLER::Revise()
{
    if( !m_target || !Allows( MDW_PART_ACTION::ID::REVISE ) )
        return;

    const PART_KEY key = partKey();
    PROVENANCE&    known = m_provenance[key];

    // The claim is already held; only the reload failed
    if( known.reloadPending && m_state.IsRevising() )
    {
        finishRevise();
        return;
    }

    if( busy( key ) )
        return;

    // Raised before the checkout, which may still land after the editor moves to another part
    known.reloadPending = true;
    known.checkoutPath.clear();

    m_outcome = wxString::Format( _( "Opening %s for edits..." ), m_name );
    m_offerGetLatest = false;

    m_partOps[key] = m_target->source->Part( m_target->item ).Revise( m_target->localPath,
            [this, alive = std::weak_ptr<bool>( m_lifetime ), key, locator = m_target->source->Info().locator](
                    const MDW_RESULT& aResult, const MDW_CHECKOUT_RESULT& aCheckout )
            {
                if( alive.expired() )
                    return;

                // The claim fetched, so the release must answer from a listing that holds any release it brought
                if( aResult.IsOk() )
                    MDW_MEMBER_INDEX::Get().Forget( locator );

                PROVENANCE& checkout = m_provenance[key];
                const bool  current = m_target && partKey() == key;

                if( !aResult.IsOk() )
                {
                    checkout.reloadPending = false;

                    if( current )
                    {
                        m_outcome = wxString::Format( _( "Could not revise %s: %s" ), m_name,
                                                      MdwErrorText( aResult ) );
                        changed();
                    }

                    return;
                }

                checkout.checkoutPath = aCheckout.local_path;
                checkout.lock = aCheckout.item.lock;

                if( !current )
                    return;

                m_state.state = aCheckout.item.state;
                m_state.lock = aCheckout.item.lock;
                finishRevise();
            } );

    changed();
}


void MDW_PART_CONTROLLER::CancelRevision()
{
    if( !m_target || !Allows( MDW_PART_ACTION::ID::CANCEL_REVISION ) || busy( partKey() ) )
        return;

    m_outcome.clear();
    m_partOps[partKey()] = m_target->source->Unlock( m_target->item, false,
            [this, alive = std::weak_ptr<bool>( m_lifetime ), key = partKey(), name = m_name](
                    const MDW_RESULT& aResult, const MDW_LOCK_INFO& aLock )
            {
                if( alive.expired() )
                    return;

                if( !aResult.IsOk() )
                {
                    failed( key, wxString::Format( _( "Could not cancel the revision of %s: %s" ), name,
                                                   MdwErrorText( aResult ) ) );
                }

                if( !m_target || partKey() != key )
                    return;

                if( aResult.IsOk() )
                    m_state.lock = aLock;

                Refresh();
                changed();
            } );
}


void MDW_PART_CONTROLLER::Transition( const wxString& aTargetStateId )
{
    if( !m_target )
        return;

    if( aTargetStateId.IsEmpty() )
    {
        if( m_transitionsRead )
            chooseTransition();
        else
            m_chooseTransition = true;

        return;
    }

    auto chosen = std::ranges::find_if( m_state.transitions,
                                        [&]( const kiapi::workspace::LifecycleTransition& aTransition )
                                        {
                                            return aTransition.target_state_id() == aTargetStateId.utf8_string();
                                        } );

    wxString comment;

    if( chosen != m_state.transitions.end() && chosen->requires_comment() )
    {
        wxTextEntryDialog dlg( dynamic_cast<wxWindow*>( m_owner ),
                               wxString::Format( _( "Why is %s moving to %s?" ), m_name,
                                                 wxString::FromUTF8( chosen->display_name() ) ),
                               _( "Change State" ), wxEmptyString, wxOK | wxCANCEL | wxTE_MULTILINE );

        if( dlg.ShowModal() != wxID_OK )
            return;

        comment = dlg.GetValue();
    }

    transition( aTargetStateId, comment );
}


void MDW_PART_CONTROLLER::chooseTransition()
{
    wxArrayString         names;
    std::vector<wxString> states;

    for( const MDW_PART_ACTION& action : Actions() )
    {
        if( action.id == MDW_PART_ACTION::ID::TRANSITION && action.Allowed() )
        {
            names.Add( action.label );
            states.push_back( action.target );
        }
    }

    if( states.empty() )
    {
        m_outcome = wxString::Format( _( "%s cannot change state now." ), m_name );
        changed();
        return;
    }

    wxSingleChoiceDialog dlg( dynamic_cast<wxWindow*>( m_owner ), wxString::Format( _( "Move %s to:" ), m_name ),
                              _( "Change State" ), names );

    if( dlg.ShowModal() == wxID_OK )
        Transition( states[dlg.GetSelection()] );
}


void MDW_PART_CONTROLLER::transition( const wxString& aTargetStateId, const wxString& aComment )
{
    if( !m_target || busy( partKey() ) )
        return;

    m_outcome.clear();
    m_partOps[partKey()] = m_target->source->Part( m_target->item ).Transition( wxEmptyString, aTargetStateId,
            aComment,
            [this, alive = std::weak_ptr<bool>( m_lifetime ), key = partKey(), name = m_name](
                    const MDW_RESULT& aResult, const MDW_TRANSITION& aTransition )
            {
                if( alive.expired() )
                    return;

                if( !aResult.IsOk() )
                {
                    failed( key, wxString::Format( _( "Could not change the state of %s: %s" ), name,
                                                   MdwErrorText( aResult ) ) );
                }

                if( !m_target || partKey() != key )
                    return;

                if( aResult.IsOk() && aTransition.outcome == MDW_CHANGE_OUTCOME::PENDING )
                    m_state.pending = aTransition.pending;

                Refresh();
                changed();
            } );
}


void MDW_PART_CONTROLLER::failed( const PART_KEY& aKey, const wxString& aMessage )
{
    // The user moved on while the change ran, so the infobar of the part now open is no place for it
    if( !m_target || partKey() != aKey )
        MdwReportFailure( dynamic_cast<wxWindow*>( m_owner ), aMessage );
    else
        m_outcome = aMessage;
}


void MDW_PART_CONTROLLER::finishRevise()
{
    const PART_KEY key = partKey();
    PROVENANCE     known = *provenance();

    if( !known.checkoutPath.IsEmpty()
        && !wxFileName( known.checkoutPath ).SameAs( wxFileName( m_target->localPath ) ) )
    {
        m_outcome = wxString::Format( _( "%s was checked out to %s, not to the library file the editor shows." ),
                                      m_name, known.checkoutPath );
        changed();
        return;
    }

    if( !m_editor.ReloadPart() )
    {
        m_outcome = wxString::Format( _( "%s changed on disk but was not reloaded over unsaved work or an open "
                                         "tool. Finish that work and revise it again." ),
                                      m_name );
        changed();
        return;
    }

    // The reload rereads the part, and may have opened another one
    if( !m_target || partKey() != key )
        return;

    PROVENANCE& reloaded = m_provenance[key];
    reloaded.reloadPending = false;

    if( !m_state.IsRevising() )
        m_state.lock = known.lock;

    m_outcome.clear();
    changed();
}


void MDW_PART_CONTROLLER::ReleaseParts( wxWindow* aParent, std::vector<MDW_RELEASE_ITEM> aItems )
{
    // The editor must show the version settling chose before anything is saved over it or released
    if( SettlePending() )
    {
        ReloadSettled();
        return;
    }

    MDW_RELEASE_PROMPT prompt = MdwReleasePrompt( aItems );

    struct RUN
    {
        std::vector<MDW_RELEASE_ITEM> items;
        wxString                      comment;
        DIALOG_MDW_RELEASE*           dialog = nullptr;
        std::weak_ptr<bool>           shown;
        wxWindow*                     parent = nullptr;
        std::vector<MDW_RELEASE_ROW>  rows;
        size_t                        stopAt = std::numeric_limits<size_t>::max();
        std::function<void( size_t )> next;
    };

    auto run = std::make_shared<RUN>();
    run->items = std::move( aItems );
    run->rows.resize( run->items.size() );
    run->parent = aParent;

    auto status = []( RUN& aRun, size_t aRow, const wxString& aStatus )
    {
        if( !aRun.shown.expired() )
            aRun.dialog->SetStatus( aRow, aStatus );
    };

    // One release at a time, held by the one in flight
    run->next = [this, status, weak = std::weak_ptr<RUN>( run ), alive = std::weak_ptr<bool>( m_lifetime )](
                        size_t aRow )
    {
        std::shared_ptr<RUN> held = weak.lock();

        if( !held || alive.expired() )
            return;

        // Releases are irreversible, so a cancel or a closed dialog stops only those not yet started
        if( aRow < held->items.size() && ( held->shown.expired() || held->dialog->CancelRequested() ) )
        {
            // A held part keeps the reason it was held, since it would not have been released anyway
            for( size_t row = aRow; row < held->items.size(); ++row )
            {
                if( held->items[row].held.IsEmpty() )
                    status( *held, row, _( "Cancelled, not released" ) );
            }

            held->stopAt = aRow;
            aRow = held->items.size();
        }

        if( aRow >= held->items.size() )
        {
            if( !held->shown.expired() )
            {
                for( size_t row = 0; row < held->items.size(); ++row )
                    held->rows[row].held = !held->items[row].held.IsEmpty();

                held->dialog->Finish( MdwReleaseSummary( MdwReleaseTally( held->rows, held->stopAt ) ) );
            }

            return;
        }

        const MDW_RELEASE_ITEM& item = held->items[aRow];

        held->rows[aRow].held = !item.held.IsEmpty();

        if( held->rows[aRow].held )
        {
            status( *held, aRow, item.held );
            held->next( aRow + 1 );
            return;
        }

        for( size_t row = aRow + 1; row < held->items.size(); ++row )
            held->rows[row].held = !held->items[row].held.IsEmpty();

        if( !held->shown.expired() )
            held->dialog->SetInFlight( MdwReleaseCanStop( held->rows, aRow ) );

        status( *held, aRow, _( "Releasing..." ) );

        release( item, held->comment, true,
                 [held, status, aRow]( const wxString& aOutcome, bool aReleased )
                 {
                     status( *held, aRow, aOutcome );
                     held->rows[aRow].outcome = aReleased ? MDW_RELEASE_ROW::OUTCOME::RELEASED
                                                          : MDW_RELEASE_ROW::OUTCOME::FAILED;

                     if( !aReleased && held->shown.expired() )
                         MdwReportFailure( held->parent, aOutcome );

                     held->next( aRow + 1 );
                 } );
    };

    DIALOG_MDW_RELEASE dlg( aParent, prompt,
                            [this, run]( const wxString& aComment )
                            {
                                run->comment = aComment;

                                if( !m_editor.SaveForRelease( run->items ) )
                                {
                                    for( size_t row = 0; row < run->items.size(); ++row )
                                        run->dialog->SetStatus( row, _( "Not released, not saved" ) );

                                    run->dialog->Finish();
                                    return;
                                }

                                run->next( 0 );
                            } );

    run->dialog = &dlg;
    run->shown = dlg.Alive();

    // The footprint listings this prompt starts may land after the dialog opens, and the concerns follow them
    auto listening = MDW_MEMBER_INDEX::Get().Listen(
            [weak = std::weak_ptr<RUN>( run )]( const MDW_MEMBER_CHANGES& )
            {
                std::shared_ptr<RUN> held = weak.lock();

                if( held && !held->shown.expired() )
                    held->dialog->SetPrompt( MdwReleasePrompt( held->items ) );
            } );

    dlg.ShowModal();
}


void MDW_PART_CONTROLLER::release( const MDW_RELEASE_ITEM& aItem, const wxString& aComment, bool aMayUpdate,
                                   RELEASED aDone )
{
    const MDW_PART_TARGET& target = aItem.target;
    const PART_KEY         key{ target.source->Info().locator, target.item.item_id };
    const bool             current = m_target && partKey() == key;

    // The infobar says so too, rather than still saying it is getting the latest version
    auto refuse = [&]( const wxString& aOutcome )
    {
        if( current )
        {
            m_outcome = aOutcome;
            changed();
        }

        aDone( aOutcome, false );
    };

    std::vector<MDW_PART_ACTION> actions;

    if( current )
    {
        SetDigest( aItem.digest, aItem.memberDigest );
        actions = Actions();
    }
    else
    {
        MDW_PART_STATE listed = target.ListedState();
        listed.digest = aItem.digest;
        listed.memberDigest = aItem.memberDigest;
        actions = listed.Actions();
    }

    auto offered = std::ranges::find_if( actions,
                                         []( const MDW_PART_ACTION& aAction )
                                         {
                                             return aAction.id == MDW_PART_ACTION::ID::RELEASE;
                                         } );

    if( offered == actions.end() || ( current && SettlePending() ) )
    {
        refuse( wxString::Format( _( "%s has nothing new to release." ), aItem.name ) );
        return;
    }

    if( !offered->Allowed() )
    {
        refuse( wxString::Format( _( "Not released: %s" ), offered->reason ) );
        return;
    }

    if( busy( key ) )
    {
        refuse( wxString::Format( _( "%s is already being released." ), aItem.name ) );
        return;
    }

    // Content saved over a newer version would be published over it unseen
    if( MDW_OVERWRITTEN_PARTS::Get().Has( target.marker, target.item.item_id ) )
    {
        refuse( wxString::Format( _( "%s was saved over a newer version in the team library. Release it again "
                                     "to compare the two first." ),
                                  aItem.name ) );
        return;
    }

    // Edits saved while the backend works must not join this release
    std::shared_ptr<const wxString> upload = snapshotForUpload( target.localPath );

    if( !upload )
    {
        refuse( wxString::Format( _( "Could not read %s to release it." ), target.localPath ) );
        return;
    }

    MDW_RELEASE_REQUEST request;
    request.base_revision_id = target.listed ? target.listed->member.revision_id : wxString();
    request.local_path = *upload;
    request.content_digest = aItem.digest;
    request.member_digest = aItem.memberDigest;
    request.comment = aComment;

    m_provenance[key].releasing = true;

    std::shared_ptr<const MDW_LIBRARY_COPY> copy = MDW_LIBRARY_SOURCES::Get().SharedCopy( target.marker );
    wxString                                askedAt = copy ? copy->snapshot.token : wxString();

    if( current )
    {
        m_outcome = wxString::Format( _( "Releasing %s..." ), aItem.name );
        m_offerGetLatest = false;
        changed();
    }

    m_partOps[key] = target.source->Part( target.item ).Release( request,
            [this, alive = std::weak_ptr<bool>( m_lifetime ), key, aItem, aComment, upload, askedAt, aMayUpdate,
             done = std::move( aDone )]( const MDW_RESULT& aResult, const MDW_CHANGE& aChange )
            {
                const bool pending = aResult.IsOk() && aChange.outcome == MDW_CHANGE_OUTCOME::PENDING;

                // Listings read the copy's snapshot, which another release may have moved since this one began
                if( aResult.IsOk() && aChange.outcome == MDW_CHANGE_OUTCOME::APPLIED )
                    MDW_SETUP::AdvanceLibraryCopy( aItem.target.marker, aChange.base_snapshot, askedAt,
                                                   aChange.snapshot );

                if( alive.expired() )
                    return;

                const bool stillOpen = m_target && partKey() == key;
                wxString   outcome;

                m_provenance[key].releasing = false;

                // Upstream moved without touching the part, so the copy is brought up to date and released again
                if( aResult.error == MDW_ERROR_CODE::COPY_BEHIND && aMayUpdate && stillOpen )
                {
                    m_outcome = wxString::Format( _( "Getting the latest version of %s..." ), aItem.name );
                    changed();

                    // Deferred, since getting the latest shows dialogs and reloads the editor
                    m_owner->CallAfter(
                            [this, alive, key, aItem, aComment, done]()
                            {
                                if( alive.expired() )
                                    return;

                                // Get Latest updates the open part's library, which may no longer be this one's
                                if( m_target && partKey() == key )
                                    m_editor.GetLatest();

                                release( aItem, aComment, false, done );
                            } );
                    return;
                }

                if( !aResult.IsOk() )
                {
                    outcome = wxString::Format( _( "Not released: %s" ), MdwErrorText( aResult ) );
                }
                else if( pending )
                {
                    outcome = aChange.pending.reason;

                    if( stillOpen )
                        m_state.pending = aChange.pending;
                }
                else
                {
                    outcome = wxString::Format( _( "Released %s as Rev %s" ), aItem.name, aChange.revision.label );

                    // The release gave up the claim; the reread only confirms it
                    if( stillOpen )
                    {
                        m_state.known = true;
                        m_state.state = aChange.revision.state;
                        m_state.lock = MDW_LOCK_INFO();
                        m_state.release = aChange.revision;
                    }
                }

                if( stillOpen )
                {
                    m_outcome = outcome;
                    m_offerGetLatest = aResult.error == MDW_ERROR_CODE::VERSION_CONFLICT
                                       || aResult.error == MDW_ERROR_CODE::COPY_BEHIND;
                    m_refusedAt = copyToken();
                    Refresh();
                    changed();
                }

                done( outcome, aResult.IsOk() );
            } );
}


wxString MDW_PART_CONTROLLER::copyToken() const
{
    if( !m_target )
        return wxString();

    std::shared_ptr<const MDW_LIBRARY_COPY> copy = MDW_LIBRARY_SOURCES::Get().SharedCopy( m_target->marker );

    return copy ? copy->snapshot.token : wxString();
}


void MDW_PART_CONTROLLER::GetLatest()
{
    m_editor.GetLatest();

    // A cancelled or failed update leaves the copy where the refusal found it
    if( m_offerGetLatest && copyToken() != m_refusedAt )
    {
        m_outcome.clear();
        m_offerGetLatest = false;
        changed();
    }
}


void MDW_PART_CONTROLLER::Settle()
{
    if( !m_target )
        return;

    m_provenance[partKey()].settling = true;
    ReloadSettled();
}


bool MDW_PART_CONTROLLER::SettlePending() const
{
    const PROVENANCE* known = provenance();

    return known && known->settling;
}


void MDW_PART_CONTROLLER::ReloadSettled()
{
    if( !SettlePending() )
        return;

    const PART_KEY key = partKey();
    const bool     loaded = m_editor.ReloadPart();

    if( loaded )
        m_provenance[key].settling = false;

    // The reload rereads the part, and may have opened another one
    if( !m_target || partKey() != key )
        return;

    if( loaded )
    {
        m_outcome = wxString::Format( _( "%s now holds the version you chose. Review it, then release it again." ),
                                      m_name );
    }
    else
    {
        m_outcome = wxString::Format( _( "%s holds the version you chose, which was not loaded over unsaved work "
                                         "or an open tool. Finish that work and release it again to load it." ),
                                      m_name );
    }

    changed();
}


void MDW_PART_CONTROLLER::changed()
{
    if( m_changed )
        m_changed();
}

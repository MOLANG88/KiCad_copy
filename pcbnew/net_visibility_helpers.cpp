/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>

#include <board.h>
#include <pcb_base_edit_frame.h>
#include <pcb_draw_panel_gal.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <widgets/appearance_controls.h>
#include "net_visibility_helpers.h"

namespace
{

std::set<int> getAllNetCodes( BOARD* aBoard )
{
    std::set<int> allCodes;

    for( NETINFO_ITEM* net : aBoard->GetNetInfo() )
        allCodes.insert( net->GetNetCode() );

    return allCodes;
}


// Run the show/hide actions for whole net sets inside a single batch: the per-net canvas
// and grid refreshes are suppressed and applied once at the end. Without a frame (unit
// tests) the actions are simply run one by one.
void applyNetVisibility( TOOL_MANAGER* aToolManager, const std::set<int>& aShowCodes,
                         const std::set<int>& aHideCodes )
{
    PCB_BASE_EDIT_FRAME* frame = dynamic_cast<PCB_BASE_EDIT_FRAME*>( aToolManager->GetToolHolder() );
    APPEARANCE_CONTROLS* appearance = frame ? frame->GetAppearancePanel() : nullptr;

    if( appearance )
        appearance->BeginBatchNetVisibilityChange();

    for( int netCode : aShowCodes )
        aToolManager->RunAction( PCB_ACTIONS::showNetInRatsnest, netCode );

    for( int netCode : aHideCodes )
        aToolManager->RunAction( PCB_ACTIONS::hideNetInRatsnest, netCode );

    if( appearance )
    {
        appearance->EndBatchNetVisibilityChange();

        if( frame->GetCanvas() )
        {
            frame->GetCanvas()->RedrawRatsnest();
            frame->GetCanvas()->Refresh();
        }
    }
}

} // namespace


namespace NET_VISIBILITY_HELPERS
{

NET_VISIBILITY_STATE GetVisibilityState( BOARD* aBoard, const std::set<int>& aSelectedCodes,
                                        const std::set<int>& aHiddenNets )
{
    NET_VISIBILITY_STATE state;

    std::set<int> allCodes = getAllNetCodes( aBoard );

    state.anySelected = !aSelectedCodes.empty();

    if( state.anySelected )
    {
        state.allSelectedVisible = std::all_of( aSelectedCodes.begin(), aSelectedCodes.end(),
                [&]( int c ) { return aHiddenNets.count( c ) == 0; } );
        state.allSelectedHidden  = std::all_of( aSelectedCodes.begin(), aSelectedCodes.end(),
                [&]( int c ) { return aHiddenNets.count( c ) > 0; } );

        std::set<int> othersHidden;

        for( int c : allCodes )
        {
            if( !aSelectedCodes.count( c ) )
                othersHidden.insert( c );
        }

        state.onlySelectedShown = aHiddenNets == othersHidden;
    }

    state.allShown  = aHiddenNets.empty();
    state.allHidden = aHiddenNets.size() >= allCodes.size();

    return state;
}


NET_VISIBILITY_MENU_STATE GetMenuState( const NET_VISIBILITY_STATE& aState )
{
    NET_VISIBILITY_MENU_STATE menu;

    menu.enableShowSelected     = aState.anySelected && !aState.allSelectedVisible;
    menu.enableHideSelected     = aState.anySelected && !aState.allSelectedHidden;
    menu.enableShowOnlySelected = aState.anySelected && !aState.onlySelectedShown;
    menu.enableShowAll          = !aState.allShown;
    menu.enableHideAll          = !aState.allHidden;

    return menu;
}


void ShowNets( TOOL_MANAGER* aToolManager, const std::set<int>& aNetCodes )
{
    applyNetVisibility( aToolManager, aNetCodes, {} );
}


void HideNets( TOOL_MANAGER* aToolManager, const std::set<int>& aNetCodes )
{
    applyNetVisibility( aToolManager, {}, aNetCodes );
}


void ShowAllNets( TOOL_MANAGER* aToolManager, BOARD* aBoard )
{
    applyNetVisibility( aToolManager, getAllNetCodes( aBoard ), {} );
}


void HideAllNets( TOOL_MANAGER* aToolManager, BOARD* aBoard )
{
    applyNetVisibility( aToolManager, {}, getAllNetCodes( aBoard ) );
}


void ShowOnlyNets( TOOL_MANAGER* aToolManager, BOARD* aBoard, const std::set<int>& aSelectedCodes )
{
    std::set<int> otherNetCodes;

    for( int netCode : getAllNetCodes( aBoard ) )
    {
        if( !aSelectedCodes.count( netCode ) )
            otherNetCodes.insert( netCode );
    }

    applyNetVisibility( aToolManager, aSelectedCodes, otherNetCodes );
}

} // namespace NET_VISIBILITY_HELPERS

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

#ifndef PANEL_ZONE_FILL_REPORT_H
#define PANEL_ZONE_FILL_REPORT_H

#include <functional>
#include <vector>

#include <wx/panel.h>
#include <wx/dataview.h>

#include <zone_fill_log.h>


/**
 * Lists how long each zone took to fill, slowest first, with the worst in bold.
 */
class PANEL_ZONE_FILL_REPORT : public wxPanel
{
public:
    using FOCUS_HANDLER = std::function<void( const KIID&, PCB_LAYER_ID )>;

    PANEL_ZONE_FILL_REPORT( wxWindow* aParent, wxWindowID aId = wxID_ANY, const wxPoint& aPos = wxDefaultPosition,
                            const wxSize& aSize = wxDefaultSize, long aStyle = wxTAB_TRAVERSAL );

    /// Called with the zone and layer of whichever row is selected.
    void SetFocusHandler( FOCUS_HANDLER aHandler ) { m_focusHandler = std::move( aHandler ); }

    void Populate( const ZONE_FILL_LOG& aLog );

    void Clear();

    int GetRowCount() const { return (int) m_totals.size(); }

private:
    void onSelected( wxDataViewEvent& aEvent );

    wxDataViewListCtrl*          m_list;
    FOCUS_HANDLER                m_focusHandler;
    std::vector<ZONE_FILL_TOTAL> m_totals;
};

#endif // PANEL_ZONE_FILL_REPORT_H

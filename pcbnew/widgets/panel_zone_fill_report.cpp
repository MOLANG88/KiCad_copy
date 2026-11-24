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

#include <wx/sizer.h>
#include <wx/translation.h>

#include <board.h>
#include <widgets/panel_zone_fill_report.h>

/// How much slower than the typical fill a row has to be to stand out.
static const double OUTLIER_FACTOR = 4.0;

/// Short fills are never worth pointing at, whatever the ratio says.
static const double OUTLIER_FLOOR_SECONDS = 0.1;


/**
 * Store that bolds the fills worth looking at.
 */
class ZONE_FILL_LIST_STORE : public wxDataViewListStore
{
public:
    bool GetAttrByRow( unsigned int aRow, unsigned int aCol, wxDataViewItemAttr& aAttr ) const override
    {
        if( GetItemData( GetItem( aRow ) ) == 0 )
            return false;

        aAttr.SetBold( true );
        return true;
    }
};


PANEL_ZONE_FILL_REPORT::PANEL_ZONE_FILL_REPORT( wxWindow* aParent, wxWindowID aId, const wxPoint& aPos,
                                                const wxSize& aSize, long aStyle ) :
        wxPanel( aParent, aId, aPos, aSize, aStyle ),
        m_list( nullptr )
{
    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );

    m_list = new wxDataViewListCtrl( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_NO_HEADER );

    // Replace the default store with the bolding variant before adding columns
    ZONE_FILL_LIST_STORE* store = new ZONE_FILL_LIST_STORE();
    m_list->AssociateModel( store );
    store->DecRef();

    m_list->AppendTextColumn( _( "Zone" ), wxDATAVIEW_CELL_INERT, 240 );
    m_list->AppendTextColumn( _( "Layer" ), wxDATAVIEW_CELL_INERT, 160 );
    m_list->AppendTextColumn( _( "Time" ), wxDATAVIEW_CELL_INERT, 100, wxALIGN_RIGHT );
    m_list->AppendTextColumn( _( "Fills" ), wxDATAVIEW_CELL_INERT, 80, wxALIGN_RIGHT );

    sizer->Add( m_list, 1, wxALL | wxEXPAND, 5 );
    sizer->Add( 0, 8, 0, wxEXPAND, 5 );

    SetSizer( sizer );
    Layout();

    m_list->Bind( wxEVT_DATAVIEW_SELECTION_CHANGED, &PANEL_ZONE_FILL_REPORT::onSelected, this );
}


void PANEL_ZONE_FILL_REPORT::Populate( const ZONE_FILL_LOG& aLog )
{
    m_totals = aLog.Totals();
    m_list->DeleteAllItems();

    // m_totals is sorted, so the middle entry is the typical fill.
    double median = m_totals.empty() ? 0.0 : m_totals[m_totals.size() / 2].m_record.m_seconds;

    for( const ZONE_FILL_TOTAL& total : m_totals )
    {
        wxVector<wxVariant> row;

        row.push_back( wxVariant( total.m_record.m_label ) );
        row.push_back( wxVariant( aLog.Board()->GetLayerName( total.m_record.m_layer ) ) );
        row.push_back( wxVariant( ZONE_FILL_LOG::FormatTime( total.m_record.m_seconds ) ) );
        row.push_back( wxVariant( total.m_fills > 1 ? wxString::Format( wxT( "%d" ), total.m_fills ) : wxString() ) );

        bool outlier = total.m_record.m_seconds >= OUTLIER_FLOOR_SECONDS
                       && total.m_record.m_seconds >= median * OUTLIER_FACTOR;

        m_list->AppendItem( row, outlier ? 1 : 0 );
    }
}


void PANEL_ZONE_FILL_REPORT::Clear()
{
    m_totals.clear();
    m_list->DeleteAllItems();
}


void PANEL_ZONE_FILL_REPORT::onSelected( wxDataViewEvent& aEvent )
{
    int row = m_list->GetSelectedRow();

    if( !m_focusHandler || row == wxNOT_FOUND || row >= (int) m_totals.size() )
        return;

    m_focusHandler( m_totals[row].m_record.m_zoneId, m_totals[row].m_record.m_layer );
}

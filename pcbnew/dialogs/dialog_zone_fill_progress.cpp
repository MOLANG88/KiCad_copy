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

#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/gauge.h>
#include <wx/notebook.h>
#include <wx/sizer.h>

#include <dialogs/dialog_zone_fill_progress.h>
#include <pcb_edit_frame.h>
#include <pcbnew_settings.h>
#include <pgm_base.h>
#include <widgets/wx_html_report_box.h>


DIALOG_ZONE_FILL_PROGRESS::DIALOG_ZONE_FILL_PROGRESS( PCB_EDIT_FRAME* aFrame ) :
        DIALOG_SHIM( aFrame, wxID_ANY, _( "Fill All Zones" ), wxDefaultPosition, wxSize( 560, 420 ),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
        PROGRESS_REPORTER_BASE( 5 ),
        m_frame( aFrame ),
        m_notebook( nullptr ),
        m_log( nullptr ),
        m_reportPanel( nullptr ),
        m_gauge( nullptr ),
        m_autoClose( nullptr ),
        m_closeButton( nullptr ),
        m_running( true ),
        m_drainThrottle( std::chrono::milliseconds( 1000 ) ),
        m_createdAt( std::chrono::steady_clock::now() ),
        m_updateThrottle( std::chrono::milliseconds( 100 ) ),
        m_yieldThrottle( std::chrono::milliseconds( 2000 ) )
{
    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    m_notebook = new wxNotebook( this, wxID_ANY );

    wxPanel*    logPanel = new wxPanel( m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL );
    wxBoxSizer* logSizer = new wxBoxSizer( wxVERTICAL );

    m_log = new WX_HTML_REPORT_BOX( logPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHW_SCROLLBAR_AUTO );
    m_log->SetImmediateMode();

    logSizer->Add( m_log, 1, wxALL | wxEXPAND, 5 );
    logSizer->Add( 0, 8, 0, wxEXPAND, 5 );
    logPanel->SetSizer( logSizer );

    m_notebook->AddPage( logPanel, _( "Progress" ), true );

    m_fillLog.SetBoard( m_frame->GetBoard() );

    m_reportPanel = new PANEL_ZONE_FILL_REPORT( m_notebook );
    m_reportPanel->SetFocusHandler(
            [this]( const KIID& aZoneId, PCB_LAYER_ID aLayer )
            {
                focusOnZone( aZoneId, aLayer );
            } );

    // Nothing to rank until the fill ends, so this page is added then.
    m_reportPanel->Hide();

    mainSizer->Add( m_notebook, 1, wxEXPAND | wxALL, 5 );

    m_gauge = new wxGauge( this, wxID_ANY, 1000 );
    mainSizer->Add( m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    m_autoClose = new wxCheckBox( this, wxID_ANY, _( "Automatically close when done" ) );
    mainSizer->Add( m_autoClose, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    m_closeButton = new wxButton( this, wxID_CANCEL, _( "Cancel" ) );

    wxBoxSizer* buttonSizer = new wxBoxSizer( wxHORIZONTAL );
    buttonSizer->AddStretchSpacer();
    buttonSizer->Add( m_closeButton, 0, wxALL, 5 );
    mainSizer->Add( buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5 );

    SetSizer( mainSizer );
    Layout();

    if( PCBNEW_SETTINGS* cfg = m_frame->GetPcbNewSettings() )
        m_autoClose->SetValue( cfg->m_ZoneFillDialog.auto_close );

    m_closeButton->Bind( wxEVT_BUTTON, &DIALOG_ZONE_FILL_PROGRESS::onCancel, this );
    m_autoClose->Bind( wxEVT_CHECKBOX, &DIALOG_ZONE_FILL_PROGRESS::onAutoClose, this );
    m_log->Bind( wxEVT_HTML_LINK_CLICKED, &DIALOG_ZONE_FILL_PROGRESS::onLogLink, this );
    Bind( wxEVT_CLOSE_WINDOW, &DIALOG_ZONE_FILL_PROGRESS::onClose, this );
}


DIALOG_ZONE_FILL_PROGRESS::~DIALOG_ZONE_FILL_PROGRESS()
{
}


void DIALOG_ZONE_FILL_PROGRESS::onAutoClose( wxCommandEvent& aEvent )
{
    if( PCBNEW_SETTINGS* cfg = m_frame->GetPcbNewSettings() )
        cfg->m_ZoneFillDialog.auto_close = m_autoClose->GetValue();
}


void DIALOG_ZONE_FILL_PROGRESS::onLogLink( wxHtmlLinkEvent& aEvent )
{
    KIID         zoneId;
    PCB_LAYER_ID layer;

    if( ZONE_FILL_LOG::ParseHref( aEvent.GetLinkInfo().GetHref(), zoneId, layer ) )
        focusOnZone( zoneId, layer );
}


void DIALOG_ZONE_FILL_PROGRESS::focusOnZone( const KIID& aZoneId, PCB_LAYER_ID aLayer )
{
    // Mid-fill the workers are still rewriting the polygons a repaint would read.
    if( m_running )
    {
        m_pendingFocus = std::make_pair( aZoneId, aLayer );
        return;
    }

    BOARD_ITEM* zone = m_frame->GetBoard()->ResolveItem( aZoneId, true );

    if( zone )
        m_frame->FocusOnItems( { zone }, aLayer );
}


void DIALOG_ZONE_FILL_PROGRESS::QueueRecord( const ZONE_FILL_RECORD& aRecord )
{
    m_fillLog.QueueRecord( aRecord );
}


void DIALOG_ZONE_FILL_PROGRESS::drainLog()
{
    // Each report in immediate mode rebuilds the whole page, so write the batch with it off.
    m_log->SetImmediateMode( false );
    m_fillLog.Drain( m_log );
    m_log->SetImmediateMode( true );
    m_log->Flush();
    m_log->ScrollToBottom();
}


void DIALOG_ZONE_FILL_PROGRESS::Report( const wxString& aMessage )
{
    PROGRESS_REPORTER_BASE::Report( aMessage );

    // Through the same queue as the fills, so a heading keeps its place among them.
    if( m_messageChanged )
    {
        m_fillLog.QueueMessage( aMessage );
        m_messageChanged = false;
    }
}


bool DIALOG_ZONE_FILL_PROGRESS::updateUI()
{
    if( m_drainThrottle.Ready() )
        drainLog();

    // Do not flash a window for a fill that is over in a moment.
    if( !IsShown() && std::chrono::steady_clock::now() - m_createdAt > std::chrono::milliseconds( 500 ) )
        Show( true );

    if( m_maxProgress > 0 )
    {
        double cur = std::clamp( (double) m_progress.load() / m_maxProgress, 0.0, 1.0 );

        m_gauge->SetValue( KiROUND( cur * 1000.0 ) );
    }

    if( m_updateThrottle.Ready() )
        Update();

    // Yielding is what keeps Cancel and the checkbox live while the fill runs.
    if( m_yieldThrottle.Ready() )
        Pgm().App().SafeYieldFor( this, wxEVT_CATEGORY_NATIVE_EVENTS );

    return !m_cancelled;
}


void DIALOG_ZONE_FILL_PROGRESS::Restart()
{
    m_running = true;
    m_cancelled.store( false );
    m_createdAt = std::chrono::steady_clock::now();
    m_pendingFocus.reset();

    m_fillLog.Clear();
    m_log->Clear();
    m_log->Flush();
    m_reportPanel->Clear();

    if( m_notebook->GetPageCount() > 1 )
        m_notebook->RemovePage( 1 );

    m_reportPanel->Hide();
    m_notebook->ChangeSelection( 0 );

    m_closeButton->SetLabel( _( "Cancel" ) );
    m_closeButton->Enable( true );
    m_gauge->SetValue( 0 );

    SetNumPhases( 5 );
    BeginPhase( 0 );
}


void DIALOG_ZONE_FILL_PROGRESS::DisableCancel()
{
    m_closeButton->Enable( false );
}


bool DIALOG_ZONE_FILL_PROGRESS::FillComplete()
{
    m_running = false;

    drainLog();

    wxString summary = m_fillLog.Summary();

    if( !summary.IsEmpty() )
        m_log->Report( summary );

    m_reportPanel->Populate( m_fillLog );

    if( m_pendingFocus )
    {
        focusOnZone( m_pendingFocus->first, m_pendingFocus->second );
        m_pendingFocus.reset();
    }

    if( m_autoClose->GetValue() )
    {
        Destroy();
        return true;
    }

    if( m_reportPanel->GetRowCount() > 0 )
    {
        m_reportPanel->Show();
        m_notebook->AddPage( m_reportPanel, _( "Zone Fills" ), true );
    }

    m_closeButton->SetLabel( _( "Close" ) );
    m_closeButton->Enable( true );
    m_gauge->SetValue( 1000 );

    if( !IsShown() )
        Show( true );

    return false;
}


void DIALOG_ZONE_FILL_PROGRESS::onCancel( wxCommandEvent& aEvent )
{
    if( m_running )
    {
        m_cancelled.store( true );
        return;
    }

    Close();
}


void DIALOG_ZONE_FILL_PROGRESS::onClose( wxCloseEvent& aEvent )
{
    // The fill owns this window until it finishes, and tears it down itself.
    if( m_running )
    {
        m_cancelled.store( true );
        aEvent.Veto();
        return;
    }

    Destroy();
}

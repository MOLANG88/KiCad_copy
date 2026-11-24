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

#ifndef DIALOG_ZONE_FILL_PROGRESS_H
#define DIALOG_ZONE_FILL_PROGRESS_H

#include <optional>

#include <core/throttle.h>
#include <dialog_shim.h>
#include <widgets/panel_zone_fill_report.h>
#include <zone_fill_log.h>
#include <widgets/progress_reporter_base.h>

class PCB_EDIT_FRAME;
class WX_HTML_REPORT_BOX;
class wxHtmlLinkEvent;
class wxButton;
class wxCheckBox;
class wxGauge;
class wxNotebook;


/**
 * Progress window for Fill All Zones.
 *
 * Lists each fill as it lands, then the same fills ordered by how long they took. Modeless
 * so that selecting one can move the board view behind it.
 */
class DIALOG_ZONE_FILL_PROGRESS : public DIALOG_SHIM, public PROGRESS_REPORTER_BASE
{
public:
    DIALOG_ZONE_FILL_PROGRESS( PCB_EDIT_FRAME* aFrame );
    ~DIALOG_ZONE_FILL_PROGRESS();

    /// Safe to call from a fill worker thread.
    void QueueRecord( const ZONE_FILL_RECORD& aRecord );

    /// The fill proper is over, so nothing is cancellable any more.
    void DisableCancel();

    /// @return true if the window closed itself, so the caller must not touch it again.
    bool FillComplete();

    void SetTitle( const wxString& aTitle ) override { DIALOG_SHIM::SetTitle( aTitle ); }

    void Report( const wxString& aMessage ) override;

private:
    bool updateUI() override;

    void onCancel( wxCommandEvent& aEvent );
    void onClose( wxCloseEvent& aEvent );
    void onAutoClose( wxCommandEvent& aEvent );
    void onLogLink( wxHtmlLinkEvent& aEvent );

    void drainLog();

    void focusOnZone( const KIID& aZoneId, PCB_LAYER_ID aLayer );

    PCB_EDIT_FRAME*         m_frame;
    ZONE_FILL_LOG           m_fillLog;
    wxNotebook*             m_notebook;
    WX_HTML_REPORT_BOX*     m_log;
    PANEL_ZONE_FILL_REPORT* m_reportPanel;
    wxGauge*                m_gauge;
    wxCheckBox*             m_autoClose;
    wxButton*               m_closeButton;

    bool                                  m_running;
    THROTTLE                              m_drainThrottle;
    std::chrono::steady_clock::time_point m_createdAt;

    /// A click that arrived mid-fill, honoured once the fill is off the stack.
    std::optional<std::pair<KIID, PCB_LAYER_ID>> m_pendingFocus;

    THROTTLE m_updateThrottle;
    THROTTLE m_yieldThrottle;
};

#endif // DIALOG_ZONE_FILL_PROGRESS_H

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

#ifndef ZONE_FILL_LOG_H
#define ZONE_FILL_LOG_H

#include <mutex>
#include <optional>
#include <vector>

#include <zone_filler.h>

class BOARD;
class REPORTER;


/// Every fill of one zone on one layer, added together.
struct ZONE_FILL_TOTAL
{
    ZONE_FILL_RECORD m_record;
    int              m_fills;
};


/**
 * Collects what a zone fill reports and hands it out in the order it happened.
 *
 * The fill runs on worker threads, so records and messages wait in a guarded queue until the
 * main thread drains them. Both share one queue so that a heading cannot land in the middle
 * of the fills it introduces.
 */
class ZONE_FILL_LOG
{
public:
    ZONE_FILL_LOG( BOARD* aBoard = nullptr ) :
            m_board( aBoard )
    {
    }

    void   SetBoard( BOARD* aBoard ) { m_board = aBoard; }
    BOARD* Board() const { return m_board; }

    /// Safe to call from a fill worker thread.
    void QueueRecord( const ZONE_FILL_RECORD& aRecord );

    /// Safe to call from a fill worker thread.
    void QueueMessage( const wxString& aMessage );

    /**
     * Write everything queued since the last call to \a aReporter, which may be null, and
     * keep the records. Main thread only.
     */
    void Drain( REPORTER* aReporter );

    void Clear();

    /// One entry per zone layer, slowest first.
    std::vector<ZONE_FILL_TOTAL> Totals() const;

    /// One line naming the worst fill, or empty when nothing was filled.
    wxString Summary() const;

    static wxString FormatTime( double aSeconds );

    /// Recover the zone and layer from a line Drain() wrote.
    static bool ParseHref( const wxString& aHref, KIID& aZoneId, PCB_LAYER_ID& aLayer );

private:
    struct ENTRY
    {
        wxString                        m_text;
        std::optional<ZONE_FILL_RECORD> m_record;
    };

    wxString formatLine( const ZONE_FILL_RECORD& aRecord ) const;

    BOARD*                        m_board;
    std::mutex                    m_mutex;
    std::vector<ENTRY>            m_queue;
    std::vector<ZONE_FILL_RECORD> m_records;
};

#endif // ZONE_FILL_LOG_H

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

#include <algorithm>
#include <map>

#include <wx/translation.h>

#include <board.h>
#include <math/util.h>
#include <reporter.h>
#include <string_utils.h>
#include <zone_fill_log.h>

#define ZONE_FILL_HREF_PREFIX wxT( "$ZONE_FILL:" )
#define ZONE_FILL_LINE_INDENT wxT( "&nbsp;&nbsp;&nbsp;&nbsp;" )


void ZONE_FILL_LOG::QueueRecord( const ZONE_FILL_RECORD& aRecord )
{
    std::lock_guard<std::mutex> lock( m_mutex );

    m_queue.push_back( { wxString(), aRecord } );
}


void ZONE_FILL_LOG::QueueMessage( const wxString& aMessage )
{
    std::lock_guard<std::mutex> lock( m_mutex );

    m_queue.push_back( { aMessage, std::nullopt } );
}


void ZONE_FILL_LOG::Drain( REPORTER* aReporter )
{
    std::vector<ENTRY> drained;

    {
        std::lock_guard<std::mutex> lock( m_mutex );

        if( m_queue.empty() )
            return;

        drained.swap( m_queue );
    }

    for( const ENTRY& entry : drained )
    {
        if( entry.m_record )
        {
            m_records.push_back( *entry.m_record );

            if( aReporter )
                aReporter->Report( formatLine( *entry.m_record ) );
        }
        else if( aReporter )
        {
            aReporter->Report( entry.m_text );
        }
    }
}


void ZONE_FILL_LOG::Clear()
{
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_queue.clear();
    }

    m_records.clear();
}


std::vector<ZONE_FILL_TOTAL> ZONE_FILL_LOG::Totals() const
{
    std::map<std::pair<KIID, PCB_LAYER_ID>, ZONE_FILL_TOTAL> byZoneLayer;

    for( const ZONE_FILL_RECORD& record : m_records )
    {
        auto key = std::make_pair( record.m_zoneId, record.m_layer );
        auto [it, inserted] = byZoneLayer.try_emplace( key, ZONE_FILL_TOTAL{ record, 1 } );

        if( !inserted )
        {
            it->second.m_record.m_seconds += record.m_seconds;
            it->second.m_fills++;
        }
    }

    std::vector<ZONE_FILL_TOTAL> totals;

    for( const auto& [key, total] : byZoneLayer )
        totals.push_back( total );

    std::sort( totals.begin(), totals.end(),
               []( const ZONE_FILL_TOTAL& aLeft, const ZONE_FILL_TOTAL& aRight )
               {
                   return aLeft.m_record.m_seconds > aRight.m_record.m_seconds;
               } );

    return totals;
}


wxString ZONE_FILL_LOG::Summary() const
{
    std::vector<ZONE_FILL_TOTAL> totals = Totals();

    if( totals.empty() )
        return wxEmptyString;

    const ZONE_FILL_RECORD& slowest = totals.front().m_record;

    return wxString::Format( _( "Filled %d zone layers in %d fills. Slowest was %s on %s at %s." ), (int) totals.size(),
                             (int) m_records.size(), EscapeHTML( slowest.m_label ),
                             EscapeHTML( m_board->GetLayerName( slowest.m_layer ) ),
                             FormatTime( slowest.m_seconds ) );
}


wxString ZONE_FILL_LOG::FormatTime( double aSeconds )
{
    if( aSeconds < 1.0 )
        return wxString::Format( _( "%d ms" ), KiROUND( aSeconds * 1000.0 ) );

    return wxString::Format( _( "%.2f s" ), aSeconds );
}


wxString ZONE_FILL_LOG::formatLine( const ZONE_FILL_RECORD& aRecord ) const
{
    // A zone or net name can hold anything the user typed, including angle brackets.
    return wxString::Format( wxT( "%s<a href='%s%s:%d'>%s</a>&nbsp;&nbsp;%s&nbsp;&nbsp;%s" ), ZONE_FILL_LINE_INDENT,
                             ZONE_FILL_HREF_PREFIX, aRecord.m_zoneId.AsString(), (int) aRecord.m_layer,
                             EscapeHTML( aRecord.m_label ),
                             EscapeHTML( m_board->GetLayerName( aRecord.m_layer ) ),
                             FormatTime( aRecord.m_seconds ) );
}


bool ZONE_FILL_LOG::ParseHref( const wxString& aHref, KIID& aZoneId, PCB_LAYER_ID& aLayer )
{
    if( !aHref.StartsWith( ZONE_FILL_HREF_PREFIX ) )
        return false;

    wxString target = aHref.Mid( wxString( ZONE_FILL_HREF_PREFIX ).Length() );
    long     layerId = 0;

    if( !target.AfterLast( ':' ).ToLong( &layerId ) )
        return false;

    aZoneId = KIID( target.BeforeLast( ':' ) );
    aLayer = ToLAYER_ID( (int) layerId );

    return true;
}

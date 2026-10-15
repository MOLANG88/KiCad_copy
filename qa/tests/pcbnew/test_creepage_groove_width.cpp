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

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <board.h>
#include <pcb_shape.h>
#include <drc/drc_creepage_utils.h>

BOOST_AUTO_TEST_CASE( CreepageGrooveWidthUsesWideSquare )
{
    BOARD board;
    PCB_SHAPE first( &board, SHAPE_T::SEGMENT );
    PCB_SHAPE second( &board, SHAPE_T::SEGMENT );
    first.SetStart( { 20000, -10000 } );
    first.SetEnd( { 20000, 10000 } );
    second.SetStart( { 70000, -10000 } );
    second.SetEnd( { 70000, 10000 } );
    std::vector<BOARD_ITEM*> edges = { &first, &second };

    BOOST_CHECK( !SegmentIntersectsBoard( { 0, 0 }, { 100000, 0 }, edges, {}, 49999 ) );
    BOOST_CHECK( SegmentIntersectsBoard( { 0, 0 }, { 100000, 0 }, edges, {}, 50000 ) );
    BOOST_CHECK( SegmentIntersectsBoard( { 0, 0 }, { 100000, 0 }, edges, {}, 60000 ) );
    BOOST_CHECK( SegmentIntersectsBoard( { 0, 0 }, { 100000, 0 }, edges, {}, 100000 ) );
}

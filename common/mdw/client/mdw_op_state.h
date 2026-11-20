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

/**
 * @file mdw_op_state.h
 * What an MDW_OP shares with the code completing it. Client implementations only.
 */

#ifndef MDW_OP_STATE_H
#define MDW_OP_STATE_H

#include <kicommon.h>
#include <mdw/client/mdw_op.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class KICOMMON_API MDW_OP_STATE : public std::enable_shared_from_this<MDW_OP_STATE>
{
public:
    /// Started inside an MDW_SYNC_SCOPE, the op completes on the client worker
    MDW_OP_STATE();

    /// Set how to abandon the request; called at most once, on the first Cancel()
    void SetCanceller( std::function<void()> aCanceller );

    /// Set how to ask the backend to stop while still taking its answer; called at most once
    void SetStopper( std::function<void()> aStopper );

    /**
     * Hand aCallback to the client's delivery, which runs it unless the op was cancelled first.
     * Any thread, including the one that started the op, since delivery is always queued.
     */
    void Complete( std::function<void()> aCallback );

    void Cancel();

    /// Ask the backend to stop, leaving the callback to run. False when there is no way to ask
    bool RequestStop();

    bool IsDone() const;

    /// Block until the callback has returned or been suppressed, or the client workers have stopped
    void Wait();

    /// Wait() for at most aTimeout, true when done or the client workers have stopped
    bool WaitFor( std::chrono::milliseconds aTimeout );

    /**
     * Run aTask, the session bookkeeping for a client op, on the one thread that settles client ops,
     * in the order posted. Never the transport's thread, so a session-bus subscriber may wait on
     * another op.
     */
    static void Settle( std::function<void()> aTask );

private:
    void deliver( const std::function<void()>& aCallback );

    /// Wait until done or, once the workers stop, past being delivered, which then suppresses the callback.
    /// False when aTimeout passed first
    bool waitSettled( std::optional<std::chrono::milliseconds> aTimeout );

    bool onDeliveryThread() const;

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::function<void()>   m_canceller;
    std::function<void()>   m_stopper;
    std::thread::id         m_runningOn;
    bool                    m_running = false;
    bool                    m_cancelled = false;
    bool                    m_answered = false; ///< Complete() ran, so the transport has nothing to withdraw
    bool                    m_done = false;
    const bool              m_direct;
};

/// Complete a new op at once with aResult, through the same delivery as any other completion
template <typename T>
MDW_OP MdwFailNow( const MDW_RESULT& aResult, MDW_DONE<T> aDone )
{
    auto state = std::make_shared<MDW_OP_STATE>();

    state->Complete(
            [result = aResult, done = std::move( aDone )]()
            {
                if( done )
                    done( result, T() );
            } );

    return MDW_OP( state );
}


template <typename T>
MDW_OP MdwNotSupported( MDW_DONE<T> aDone )
{
    return MdwFailNow<T>( MDW_RESULT::Fail( MDW_ERROR_CODE::NOT_SUPPORTED ), std::move( aDone ) );
}

#endif // MDW_OP_STATE_H

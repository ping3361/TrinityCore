/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Transaction.h"
#include "Errors.h"
#include "Log.h"
#include "MySQLConnection.h"
#include "PreparedStatement.h"
#include "Timer.h"
#include <mysqld_error.h>
#include <sstream>
#include <thread>
#include <cstring>

std::mutex TransactionTask::_deadlockLock;

#define DEADLOCK_MAX_RETRY_TIME_MS 60000

TransactionData::~TransactionData() = default;

//- Append a raw ad-hoc query to the transaction
void TransactionBase::Append(char const* sql)
{
    ASSERT(sql);
    m_queries.emplace_back(std::in_place_type<std::string>, sql);
}

//- Append a prepared statement to the transaction
void TransactionBase::AppendPreparedStatement(PreparedStatementBase* stmt)
{
    ASSERT(stmt);
    m_queries.emplace_back(std::in_place_type<std::unique_ptr<PreparedStatementBase>>, stmt);
}

void TransactionBase::Cleanup()
{
    // This might be called by explicit calls to Cleanup or by the auto-destructor
    if (_cleanedUp)
        return;

    m_queries.clear();
    _cleanedUp = true;
}

bool TransactionTask::Execute(MySQLConnection* conn, std::shared_ptr<TransactionBase> trans)
{
    int errorCode = TryExecute(conn, trans);
    if (!errorCode)
        return true;

    if (errorCode == ER_LOCK_DEADLOCK)
    {
        std::string threadId = []()
        {
            // wrapped in lambda to fix false positive analysis warning C26115
            std::ostringstream threadIdStream;
            threadIdStream << std::this_thread::get_id();
            return threadIdStream.str();
        }();

        // Make sure only 1 async thread retries a transaction so they don't keep dead-locking each other
        std::lock_guard<std::mutex> lock(_deadlockLock);

        for (uint32 loopDuration = 0, startMSTime = getMSTime(); loopDuration <= DEADLOCK_MAX_RETRY_TIME_MS; loopDuration = GetMSTimeDiffToNow(startMSTime))
        {
            if (!TryExecute(conn, trans))
                return true;

            TC_LOG_WARN("sql.sql", "Deadlocked SQL Transaction, retrying. Loop timer: {} ms, Thread Id: {}", loopDuration, threadId);
        }

        TC_LOG_ERROR("sql.sql", "Fatal deadlocked SQL Transaction, it will not be retried anymore. Thread Id: {}", threadId);
    }

    // Clean up now.
    trans->Cleanup();

    return false;
}

int TransactionTask::TryExecute(MySQLConnection* conn, std::shared_ptr<TransactionBase> trans)
{
    return conn->ExecuteTransaction(trans);
}

bool TransactionCallback::InvokeIfReady()
{
    if (m_future.valid() && m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        m_callback(m_future.get());
        return true;
    }

    return false;
}

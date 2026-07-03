#pragma once
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/coroutine.h>
#include <chrono>
#include <string>

namespace wikore::postgres {

// Convenience alias: a request deadline as a monotonic instant. The sentinel
// max() means "no deadline" (unbounded).
using Deadline = std::chrono::steady_clock::time_point;

inline Deadline no_deadline() { return Deadline::max(); }

// ---------------------------------------------------------------------------
// exec_until - run one statement bounded by a request deadline.
//
// drogon exposes no per-call query timeout, and DbClient::setTimeout is
// client-global (it would race across the shared connection pool). The only
// safe per-request bound is a Postgres transaction-local statement_timeout:
// pin a connection with a transaction, set statement_timeout to the
// milliseconds remaining before `deadline`, then run the query. A query that
// overruns is cancelled server-side (SQLSTATE 57014) and surfaces as a
// DrogonDbException, exactly like any other query failure.
//
// A max() deadline means unbounded: the statement runs on the shared client
// with no transaction, preserving the fast path for callers that carry no
// deadline (background workers, tests).
// ---------------------------------------------------------------------------
template <typename... Args>
drogon::Task<drogon::orm::Result>
exec_until(drogon::orm::DbClientPtr db,
           Deadline                 deadline,
           std::string              sql,
           Args... args)
{
    if (deadline == no_deadline())
        co_return co_await db->execSqlCoro(sql, args...);

    // Acquire the pooled connection FIRST, then derive the timeout from what is
    // left of the budget. Computing it before newTransactionCoro would not
    // charge the query for time spent waiting on a busy pool, letting it run
    // past the deadline. (The pool-wait itself is not separately bounded:
    // drogon's coroutine API exposes no acquisition timeout, and racing it
    // against a timer would orphan the connection. The orchestrator's
    // between-step deadline checks catch a wait that overran, and the query
    // below is always bounded to the post-acquisition remainder.)
    auto trans = co_await db->newTransactionCoro();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()).count();

    // Budget spent (during acquisition or before): reject WITHOUT running the
    // query. A clamp-to-1ms would let a sub-millisecond statement (e.g.
    // SELECT 1) finish and let the request continue past its deadline, because
    // statement_timeout caps execution time - it does not reject an
    // already-expired deadline. TimeoutError is a DrogonDbException, so it flows
    // through the same catch sites as a real cancellation and maps to 503.
    if (ms <= 0)
        throw drogon::orm::TimeoutError("deadline exceeded before query execution");

    // Transaction-local statement_timeout (third arg = is_local). It resets
    // when the transaction ends, so nothing leaks back to the pooled
    // connection.
    co_await trans->execSqlCoro(
        "SELECT set_config('statement_timeout', $1, true)", std::to_string(ms));
    co_return co_await trans->execSqlCoro(sql, args...);
}

} // namespace wikore::postgres

#pragma once

// Database: an async + (truly) sync connector exposed to Lua as a clean userdata
// class. The engine specifics live behind the `Connection` backend (SQLite, MySQL,
// ...); a POOL of `pool_size` worker threads each own one Connection.
//
//  * async (callback): result lands on a queue that `poll()` drains on the main
//    thread, firing the callback.
//  * sync (blocking): the caller BLOCKS on a condition variable until a worker
//    fills the result, then returns it — like nanos's synchronous methods.

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <sol/sol.hpp>

#include "connection.hpp" // brings dbtypes (Value, Completion, ...) + the backend interface

namespace normdb {

// A blocking-sync rendezvous: the worker fills `result` and signals; the caller
// (main thread) waits on `cv`. Lives on the caller's stack for the call's duration.
struct SyncSlot {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    Completion result;
};

// A unit of work queued from Lua. Exactly one of `cb` (async) / `sync_slot` (sync).
struct Job {
    JobKind kind = JobKind::Execute;
    std::string sql;
    std::vector<Value> params;
    std::vector<std::vector<Value>> param_sets;
    std::vector<Statement> statements;
    sol::protected_function cb;     // async delivery
    SyncSlot* sync_slot = nullptr;  // sync delivery (blocking rendezvous)
};

class Database {
public:
    Database(int engine, std::string connection, int pool_size, bool numbered,
             sol::protected_function on_ready);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void select(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void execute(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void insert(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void update(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void prepare(std::string sql, std::vector<std::vector<Value>> sets, sol::protected_function cb);
    void transaction(std::vector<Statement> statements, sol::protected_function cb);

    void submit_sync(JobKind kind, std::string sql, std::vector<Value> args,
                     std::vector<std::vector<Value>> sets, std::vector<Statement> statements,
                     SyncSlot* slot);

    int poll(lua_State* L);
    void close();

private:
    void enqueue(Job&& job);
    void worker_loop(int index);

    int engine_;
    std::string connection_;
    int pool_size_;
    bool numbered_;
    sol::protected_function ready_cb_;

    std::vector<std::thread> workers_;
    std::mutex jobs_mtx_;
    std::condition_variable jobs_cv_;
    std::queue<Job> jobs_;
    bool stop_ = false;

    std::mutex done_mtx_;
    std::queue<Completion> done_; // async results only
};

// Lua <-> Value conversions + result shaping.
sol::object value_to_lua(lua_State* L, const Value& v);
std::vector<Value> params_from_lua(const sol::object& obj);
std::tuple<sol::object, sol::object> make_result(lua_State* L, Completion& c);

} // namespace normdb

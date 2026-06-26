#pragma once

// Database: an async + (truly) sync SQLite connector exposed to Lua as a clean
// userdata class, matching the official nanos `Database` (variadic args, :N/?
// placeholders, a connection pool, Execute -> affectedRows, Insert -> insertId).
//
// Threading: a POOL of `pool_size` worker threads, each owning its own sqlite3
// connection. Workers pull from one shared job queue; a job runs entirely on one
// connection (so a transaction is atomic and last_insert_rowid() is reliable).
// Workers NEVER touch the Lua state.
//
//  * async (callback): result lands on a queue that `poll()` drains on the main
//    thread, firing the callback.
//  * sync (blocking): the caller BLOCKS on a condition variable until a worker
//    fills the result, then returns it — like nanos's synchronous methods.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include <sol/sol.hpp>

struct sqlite3;      // forward decls: the C API types stay out of the header
struct sqlite3_stmt;

namespace normdb {

using Value = std::variant<std::nullptr_t, int64_t, double, std::string>;

struct Column {
    std::string name;
    Value value;
};
using Row = std::vector<Column>;

struct Statement {
    std::string sql;
    std::vector<Value> params;
};

enum class JobKind {
    Select, Execute, Insert, Update, Prepare, Transaction, Ready, Close
};

// The result of a Job, produced on a worker thread (no Lua here). For async jobs it
// carries the callback (moved across threads, only ever called/destroyed on main).
struct Completion {
    JobKind kind = JobKind::Execute;
    sol::protected_function cb; // async only
    bool ok = true;
    std::string error;
    std::vector<Row> rows; // Select
    int64_t affected = 0;  // Execute/Update/Prepare/Transaction
    int64_t insert_id = 0; // Insert
};

// A blocking-sync rendezvous: the worker fills `result` and signals; the caller
// (main thread) waits on `cv`. Lives on the caller's stack for the call's duration.
struct SyncSlot {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    Completion result;
};

// A unit of work queued from Lua. Exactly one of `cb` (async) / `sync_slot` (sync)
// is set. `cb` is a Lua reference: moved across threads but only ever called /
// destroyed on the Lua main thread.
struct Job {
    JobKind kind = JobKind::Execute;
    std::string sql;
    std::vector<Value> params;                  // bind args (single statement)
    std::vector<std::vector<Value>> param_sets; // prepare (batch)
    std::vector<Statement> statements;          // transaction
    sol::protected_function cb;                 // async delivery
    SyncSlot* sync_slot = nullptr;              // sync delivery (blocking rendezvous)
};

class Database {
public:
    Database(int engine, std::string connection, int pool_size, bool numbered,
             sol::protected_function on_ready);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // --- async (callback). args are variadic, bound per placeholder mode ---
    void select(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void execute(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void insert(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void update(std::string sql, std::vector<Value> args, sol::protected_function cb);
    void prepare(std::string sql, std::vector<std::vector<Value>> sets, sol::protected_function cb);
    void transaction(std::vector<Statement> statements, sol::protected_function cb);

    // --- sync (blocking): enqueue a job that fills `slot` and signals it ---
    void submit_sync(JobKind kind, std::string sql, std::vector<Value> args,
                     std::vector<std::vector<Value>> sets, std::vector<Statement> statements,
                     SyncSlot* slot);

    int poll(lua_State* L); // drain async completions, fire callbacks (main thread)
    void close();

private:
    void enqueue(Job&& job);
    void worker_loop(int index);

    sqlite3* open_connection(std::string& err);
    void bind_args(sqlite3_stmt* st, const std::vector<Value>& args) const; // :N named / ? positional
    Completion run_select(sqlite3* h, const std::string& sql, const std::vector<Value>& args);
    Completion run_write(sqlite3* h, const std::string& sql, const std::vector<Value>& args);
    Completion run_prepare(sqlite3* h, const std::string& sql, const std::vector<std::vector<Value>>& sets);
    Completion run_transaction(sqlite3* h, const std::vector<Statement>& statements);

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

// Lua <-> Value conversions + result shaping (defined in module.cpp / database.cpp).
sol::object value_to_lua(lua_State* L, const Value& v);
std::vector<Value> params_from_lua(const sol::object& obj);
// Build the (result, err) pair a completion maps to (Execute->affected, Insert->id,
// Select->rows, ...). Used by both async (call cb) and sync (return) delivery.
std::tuple<sol::object, sol::object> make_result(lua_State* L, Completion& c);

} // namespace normdb

#include "database.hpp"

#include <cstdio>
#include <exception>
#include <string>

#include <sqlite3.h>

namespace normdb {

// ---------------------------------------------------------------------------
// sqlite helpers (worker thread only) — REAL parameter binding (no string
// interpolation), so :N / ? are injection-safe.
// ---------------------------------------------------------------------------

static void bind_one(sqlite3_stmt* st, int idx, const Value& v) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) sqlite3_bind_null(st, idx);
        else if constexpr (std::is_same_v<T, int64_t>)   sqlite3_bind_int64(st, idx, arg);
        else if constexpr (std::is_same_v<T, double>)    sqlite3_bind_double(st, idx, arg);
        else                                             sqlite3_bind_text(st, idx, arg.c_str(),
                                                              static_cast<int>(arg.size()), SQLITE_TRANSIENT);
    }, v);
}

static void bind_positional(sqlite3_stmt* st, const std::vector<Value>& args) {
    for (size_t i = 0; i < args.size(); ++i) bind_one(st, static_cast<int>(i) + 1, args[i]);
}

// :N mode: bind arg i to the named parameter ":i" if present. Real binding, not
// substitution -> injection-safe; do NOT quote `:0` in the SQL.
static void bind_named(sqlite3_stmt* st, const std::vector<Value>& args) {
    char name[24];
    for (size_t i = 0; i < args.size(); ++i) {
        std::snprintf(name, sizeof(name), ":%zu", i);
        const int idx = sqlite3_bind_parameter_index(st, name);
        if (idx > 0) bind_one(st, idx, args[i]);
    }
}

static Value read_column(sqlite3_stmt* st, int col) {
    switch (sqlite3_column_type(st, col)) {
        case SQLITE_INTEGER: return static_cast<int64_t>(sqlite3_column_int64(st, col));
        case SQLITE_FLOAT:   return sqlite3_column_double(st, col);
        case SQLITE_NULL:    return nullptr;
        case SQLITE_TEXT:
        case SQLITE_BLOB:
        default: {
            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
            const int n = sqlite3_column_bytes(st, col);
            return std::string(txt ? txt : "", static_cast<size_t>(n));
        }
    }
}

// ---------------------------------------------------------------------------
// Result shaping (main thread): a completion -> (result, err) Lua pair.
// Execute/Update/Prepare -> affectedRows, Insert -> insertId, Select -> rows,
// Transaction -> ok, Ready -> ok. (matches nanos)
// ---------------------------------------------------------------------------

std::tuple<sol::object, sol::object> make_result(lua_State* L, Completion& c) {
    sol::state_view lua(L);
    const sol::object nil = sol::lua_nil;

    if (c.kind == JobKind::Ready) {
        return { sol::make_object(lua, c.ok), c.ok ? nil : sol::make_object(lua, c.error) };
    }
    if (!c.ok) {
        return { nil, sol::make_object(lua, c.error) };
    }
    switch (c.kind) {
        case JobKind::Select: {
            sol::table arr = lua.create_table(static_cast<int>(c.rows.size()), 0);
            for (size_t i = 0; i < c.rows.size(); ++i) {
                const Row& row = c.rows[i];
                sol::table t = lua.create_table(0, static_cast<int>(row.size()));
                for (const Column& col : row) t[col.name] = value_to_lua(L, col.value);
                arr[i + 1] = t;
            }
            return { arr, nil };
        }
        case JobKind::Insert:
            return { sol::make_object(lua, c.insert_id), nil };
        case JobKind::Transaction:
            return { sol::make_object(lua, true), nil };
        case JobKind::Execute:
        case JobKind::Update:
        case JobKind::Prepare:
        default:
            return { sol::make_object(lua, c.affected), nil };
    }
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::Database(int engine, std::string connection, int pool_size, bool numbered,
                   sol::protected_function on_ready)
    : engine_(engine), connection_(std::move(connection)),
      pool_size_(pool_size < 1 ? 1 : pool_size), numbered_(numbered),
      ready_cb_(std::move(on_ready)) {
    workers_.reserve(static_cast<size_t>(pool_size_));
    for (int i = 0; i < pool_size_; ++i) {
        workers_.emplace_back(&Database::worker_loop, this, i);
    }
}

Database::~Database() { close(); }

void Database::close() {
    {
        std::lock_guard<std::mutex> lk(jobs_mtx_);
        if (stop_) return;
        stop_ = true;
    }
    jobs_cv_.notify_all();
    for (auto& w : workers_) if (w.joinable()) w.join();
    workers_.clear();
}

sqlite3* Database::open_connection(std::string& err) {
    if (engine_ != 0) {
        err = "norm_database: only SQLite (engine 0) is supported for now";
        return nullptr;
    }
    sqlite3* h = nullptr;
    if (sqlite3_open(connection_.c_str(), &h) != SQLITE_OK) {
        err = h ? sqlite3_errmsg(h) : "cannot open database";
        if (h) sqlite3_close(h);
        return nullptr;
    }
    sqlite3_busy_timeout(h, 5000);
    sqlite3_exec(h, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return h;
}

void Database::bind_args(sqlite3_stmt* st, const std::vector<Value>& args) const {
    if (numbered_) bind_named(st, args);
    else           bind_positional(st, args);
}

void Database::enqueue(Job&& job) {
    {
        std::lock_guard<std::mutex> lk(jobs_mtx_);
        if (stop_) return;
        jobs_.push(std::move(job));
    }
    jobs_cv_.notify_one();
}

void Database::select(std::string sql, std::vector<Value> args, sol::protected_function cb) {
    Job j; j.kind = JobKind::Select; j.sql = std::move(sql); j.params = std::move(args); j.cb = std::move(cb);
    enqueue(std::move(j));
}
void Database::execute(std::string sql, std::vector<Value> args, sol::protected_function cb) {
    Job j; j.kind = JobKind::Execute; j.sql = std::move(sql); j.params = std::move(args); j.cb = std::move(cb);
    enqueue(std::move(j));
}
void Database::insert(std::string sql, std::vector<Value> args, sol::protected_function cb) {
    Job j; j.kind = JobKind::Insert; j.sql = std::move(sql); j.params = std::move(args); j.cb = std::move(cb);
    enqueue(std::move(j));
}
void Database::update(std::string sql, std::vector<Value> args, sol::protected_function cb) {
    Job j; j.kind = JobKind::Update; j.sql = std::move(sql); j.params = std::move(args); j.cb = std::move(cb);
    enqueue(std::move(j));
}
void Database::prepare(std::string sql, std::vector<std::vector<Value>> sets, sol::protected_function cb) {
    Job j; j.kind = JobKind::Prepare; j.sql = std::move(sql); j.param_sets = std::move(sets); j.cb = std::move(cb);
    enqueue(std::move(j));
}
void Database::transaction(std::vector<Statement> statements, sol::protected_function cb) {
    Job j; j.kind = JobKind::Transaction; j.statements = std::move(statements); j.cb = std::move(cb);
    enqueue(std::move(j));
}

void Database::submit_sync(JobKind kind, std::string sql, std::vector<Value> args,
                           std::vector<std::vector<Value>> sets, std::vector<Statement> statements,
                           SyncSlot* slot) {
    Job j;
    j.kind = kind;
    j.sql = std::move(sql);
    j.params = std::move(args);
    j.param_sets = std::move(sets);
    j.statements = std::move(statements);
    j.sync_slot = slot;
    enqueue(std::move(j));
}

Completion Database::run_select(sqlite3* h, const std::string& sql, const std::vector<Value>& args) {
    Completion c;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        c.ok = false; c.error = sqlite3_errmsg(h); return c;
    }
    bind_args(st, args);
    const int cols = sqlite3_column_count(st);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        Row row; row.reserve(static_cast<size_t>(cols));
        for (int i = 0; i < cols; ++i) {
            const char* name = sqlite3_column_name(st, i);
            row.push_back(Column{ name ? name : "", read_column(st, i) });
        }
        c.rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) { c.ok = false; c.error = sqlite3_errmsg(h); }
    sqlite3_finalize(st);
    return c;
}

Completion Database::run_write(sqlite3* h, const std::string& sql, const std::vector<Value>& args) {
    Completion c;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        c.ok = false; c.error = sqlite3_errmsg(h); return c;
    }
    bind_args(st, args);
    int rc = sqlite3_step(st);
    while (rc == SQLITE_ROW) rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        c.ok = false; c.error = sqlite3_errmsg(h); sqlite3_finalize(st); return c;
    }
    c.affected = sqlite3_changes(h);
    c.insert_id = sqlite3_last_insert_rowid(h); // same connection + thread -> reliable
    sqlite3_finalize(st);
    return c;
}

Completion Database::run_prepare(sqlite3* h, const std::string& sql, const std::vector<std::vector<Value>>& sets) {
    Completion c;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        c.ok = false; c.error = sqlite3_errmsg(h); return c;
    }
    int64_t total = 0;
    for (const auto& set : sets) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        bind_args(st, set);
        int rc = sqlite3_step(st);
        while (rc == SQLITE_ROW) rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) { c.ok = false; c.error = sqlite3_errmsg(h); break; }
        total += sqlite3_changes(h);
    }
    sqlite3_finalize(st);
    if (c.ok) { c.affected = total; c.insert_id = sqlite3_last_insert_rowid(h); }
    return c;
}

Completion Database::run_transaction(sqlite3* h, const std::vector<Statement>& statements) {
    Completion c;
    char* emsg = nullptr;
    if (sqlite3_exec(h, "BEGIN", nullptr, nullptr, &emsg) != SQLITE_OK) {
        c.ok = false; c.error = emsg ? emsg : "BEGIN failed"; sqlite3_free(emsg); return c;
    }
    int64_t total = 0;
    for (const auto& s : statements) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h, s.sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            c.ok = false; c.error = sqlite3_errmsg(h); break;
        }
        bind_args(st, s.params);
        int rc = sqlite3_step(st);
        while (rc == SQLITE_ROW) rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) { c.ok = false; c.error = sqlite3_errmsg(h); sqlite3_finalize(st); break; }
        total += sqlite3_changes(h);
        sqlite3_finalize(st);
    }
    if (c.ok) {
        if (sqlite3_exec(h, "COMMIT", nullptr, nullptr, &emsg) != SQLITE_OK) {
            c.ok = false; c.error = emsg ? emsg : "COMMIT failed"; sqlite3_free(emsg);
            sqlite3_exec(h, "ROLLBACK", nullptr, nullptr, nullptr);
        } else {
            c.affected = total;
        }
    } else {
        sqlite3_exec(h, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return c;
}

void Database::worker_loop(int index) {
    std::string open_err;
    sqlite3* h = open_connection(open_err);
    const bool opened = (h != nullptr);

    if (index == 0) { // one ready notification for the pool (async, via done_)
        Completion ready; ready.kind = JobKind::Ready; ready.ok = opened; ready.error = open_err;
        ready.cb = std::move(ready_cb_);
        std::lock_guard<std::mutex> lk(done_mtx_);
        done_.push(std::move(ready));
    }

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(jobs_mtx_);
            jobs_cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_ && jobs_.empty()) break;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        if (job.kind == JobKind::Close) break;

        Completion c;
        if (!opened) {
            c.ok = false;
            c.error = open_err.empty() ? "database not open" : open_err;
        } else {
            try {
                switch (job.kind) {
                    case JobKind::Select:      c = run_select(h, job.sql, job.params); break;
                    case JobKind::Execute:
                    case JobKind::Insert:
                    case JobKind::Update:      c = run_write(h, job.sql, job.params); break;
                    case JobKind::Prepare:     c = run_prepare(h, job.sql, job.param_sets); break;
                    case JobKind::Transaction: c = run_transaction(h, job.statements); break;
                    default:                   c.ok = false; c.error = "unknown job kind"; break;
                }
            } catch (const std::exception& e) {
                c = Completion{}; c.ok = false; c.error = std::string("C++ exception: ") + e.what();
            } catch (...) {
                c = Completion{}; c.ok = false; c.error = "unknown C++ exception";
            }
        }
        c.kind = job.kind;

        if (job.sync_slot) { // sync: hand the result to the waiting caller
            {
                std::lock_guard<std::mutex> lk(job.sync_slot->m);
                job.sync_slot->result = std::move(c);
                job.sync_slot->done = true;
            }
            job.sync_slot->cv.notify_one();
        } else {             // async: queue for poll()
            c.cb = std::move(job.cb);
            std::lock_guard<std::mutex> lk(done_mtx_);
            done_.push(std::move(c));
        }
    }

    if (h) sqlite3_close(h);
}

int Database::poll(lua_State* L) {
    std::queue<Completion> local;
    {
        std::lock_guard<std::mutex> lk(done_mtx_);
        std::swap(local, done_);
    }

    int fired = 0;
    while (!local.empty()) {
        Completion c = std::move(local.front()); local.pop();
        if (!c.cb.valid()) continue; // async job with no callback

        try {
            std::tuple<sol::object, sol::object> r = make_result(L, c);
            sol::protected_function_result pr = c.cb(std::get<0>(r), std::get<1>(r));
            if (!pr.valid()) {
                sol::error e = pr;
                sol::state_view lua(L);
                sol::protected_function printer = lua["print"];
                if (printer.valid()) printer(std::string("[norm_database] callback error: ") + e.what());
            }
        } catch (const std::exception& e) {
            sol::state_view lua(L);
            sol::protected_function printer = lua["print"];
            if (printer.valid()) printer(std::string("[norm_database] poll exception: ") + e.what());
        } catch (...) {}
        ++fired;
    }
    return fired;
}

} // namespace normdb

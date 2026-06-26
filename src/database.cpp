#include "database.hpp"

#include <exception>
#include <memory>
#include <string>

namespace normdb {

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
        case JobKind::Insert:      return { sol::make_object(lua, c.insert_id), nil };
        case JobKind::Transaction: return { sol::make_object(lua, true), nil };
        case JobKind::Execute:
        case JobKind::Update:
        case JobKind::Prepare:
        default:                   return { sol::make_object(lua, c.affected), nil };
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

void Database::worker_loop(int index) {
    std::unique_ptr<Connection> conn = make_connection(engine_);
    std::string open_err;
    const bool opened = conn->open(ConnInfo{ connection_, numbered_ }, open_err);

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
                    case JobKind::Select:      c = conn->run_select(job.sql, job.params); break;
                    case JobKind::Execute:
                    case JobKind::Insert:
                    case JobKind::Update:      c = conn->run_write(job.sql, job.params); break;
                    case JobKind::Prepare:     c = conn->run_prepare(job.sql, job.param_sets); break;
                    case JobKind::Transaction: c = conn->run_transaction(job.statements); break;
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

    conn->close();
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
        if (!c.cb.valid()) continue;

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

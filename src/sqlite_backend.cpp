#include "connection.hpp"

#include <sqlite3.h>

namespace normdb {
namespace {

void bind_positional(sqlite3_stmt* st, const std::vector<Value>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        const int idx = static_cast<int>(i) + 1;
        std::visit([&](auto&& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) sqlite3_bind_null(st, idx);
            else if constexpr (std::is_same_v<T, int64_t>)   sqlite3_bind_int64(st, idx, a);
            else if constexpr (std::is_same_v<T, double>)    sqlite3_bind_double(st, idx, a);
            else                                             sqlite3_bind_text(st, idx, a.c_str(),
                                                                  static_cast<int>(a.size()), SQLITE_TRANSIENT);
        }, args[i]);
    }
}

Value read_column(sqlite3_stmt* st, int col) {
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

class SqliteConnection : public Connection {
public:
    bool open(const ConnInfo& info, std::string& err) override {
        numbered_ = info.numbered;
        if (sqlite3_open(info.connection.c_str(), &h_) != SQLITE_OK) {
            err = h_ ? sqlite3_errmsg(h_) : "cannot open database";
            if (h_) { sqlite3_close(h_); h_ = nullptr; }
            return false;
        }
        sqlite3_busy_timeout(h_, 5000);
        sqlite3_exec(h_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        return true;
    }

    void close() override {
        if (h_) { sqlite3_close(h_); h_ = nullptr; }
    }

    Completion run_select(const std::string& sql, const std::vector<Value>& args) override {
        Completion c;
        std::string q; std::vector<Value> a;
        prepare_query(numbered_, sql, args, q, a);
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h_, q.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            c.ok = false; c.error = sqlite3_errmsg(h_); return c;
        }
        bind_positional(st, a);
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
        if (rc != SQLITE_DONE) { c.ok = false; c.error = sqlite3_errmsg(h_); }
        sqlite3_finalize(st);
        return c;
    }

    Completion run_write(const std::string& sql, const std::vector<Value>& args) override {
        Completion c;
        std::string q; std::vector<Value> a;
        prepare_query(numbered_, sql, args, q, a);
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h_, q.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            c.ok = false; c.error = sqlite3_errmsg(h_); return c;
        }
        bind_positional(st, a);
        int rc = sqlite3_step(st);
        while (rc == SQLITE_ROW) rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) { c.ok = false; c.error = sqlite3_errmsg(h_); sqlite3_finalize(st); return c; }
        c.affected = sqlite3_changes(h_);
        c.insert_id = sqlite3_last_insert_rowid(h_);
        sqlite3_finalize(st);
        return c;
    }

    Completion run_prepare(const std::string& sql, const std::vector<std::vector<Value>>& sets) override {
        Completion c;
        // The rewritten SQL (and the ? order) is identical for every set, so prepare once.
        std::string q; std::vector<Value> ignore;
        prepare_query(numbered_, sql, sets.empty() ? std::vector<Value>{} : sets[0], q, ignore);
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h_, q.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            c.ok = false; c.error = sqlite3_errmsg(h_); return c;
        }
        int64_t total = 0;
        for (const auto& set : sets) {
            std::string q2; std::vector<Value> a;
            prepare_query(numbered_, sql, set, q2, a); // reorder args for this set
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            bind_positional(st, a);
            int rc = sqlite3_step(st);
            while (rc == SQLITE_ROW) rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) { c.ok = false; c.error = sqlite3_errmsg(h_); break; }
            total += sqlite3_changes(h_);
        }
        sqlite3_finalize(st);
        if (c.ok) { c.affected = total; c.insert_id = sqlite3_last_insert_rowid(h_); }
        return c;
    }

    Completion run_transaction(const std::vector<Statement>& statements) override {
        Completion c;
        char* emsg = nullptr;
        if (sqlite3_exec(h_, "BEGIN", nullptr, nullptr, &emsg) != SQLITE_OK) {
            c.ok = false; c.error = emsg ? emsg : "BEGIN failed"; sqlite3_free(emsg); return c;
        }
        int64_t total = 0;
        for (const auto& s : statements) {
            std::string q; std::vector<Value> a;
            prepare_query(numbered_, s.sql, s.params, q, a);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(h_, q.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                c.ok = false; c.error = sqlite3_errmsg(h_); break;
            }
            bind_positional(st, a);
            int rc = sqlite3_step(st);
            while (rc == SQLITE_ROW) rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) { c.ok = false; c.error = sqlite3_errmsg(h_); sqlite3_finalize(st); break; }
            total += sqlite3_changes(h_);
            sqlite3_finalize(st);
        }
        if (c.ok) {
            if (sqlite3_exec(h_, "COMMIT", nullptr, nullptr, &emsg) != SQLITE_OK) {
                c.ok = false; c.error = emsg ? emsg : "COMMIT failed"; sqlite3_free(emsg);
                sqlite3_exec(h_, "ROLLBACK", nullptr, nullptr, nullptr);
            } else {
                c.affected = total;
            }
        } else {
            sqlite3_exec(h_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        return c;
    }

private:
    sqlite3* h_ = nullptr;
    bool numbered_ = true;
};

} // namespace

std::unique_ptr<Connection> make_sqlite_connection() {
    return std::make_unique<SqliteConnection>();
}

} // namespace normdb

#pragma once

// Connection: the per-connection backend abstraction. One concrete backend per
// engine (SQLite, MySQL/MariaDB, ...). Each worker thread owns one Connection.
//
// Placeholders are normalised ONCE, engine-agnostically, by `prepare_query`: in
// ":N" mode the `:0/:1` named refs are rewritten to positional `?` and the args
// reordered (string literals are skipped, so `'12:00'` is untouched). Backends
// therefore only ever bind positional `?` parameters — real binding, no injection.

#include <memory>
#include <string>
#include <vector>

#include "dbtypes.hpp"

namespace normdb {

struct ConnInfo {
    std::string connection; // SQLite: a file path. MySQL: "host=.. port=.. user=.. password=.. db=..".
    bool numbered = true;   // ":N" mode (true) vs "?" positional (false)
};

class Connection {
public:
    virtual ~Connection() = default;

    virtual bool open(const ConnInfo& info, std::string& err) = 0;
    virtual void close() = 0;

    virtual Completion run_select(const std::string& sql, const std::vector<Value>& args) = 0;
    virtual Completion run_write(const std::string& sql, const std::vector<Value>& args) = 0;
    virtual Completion run_prepare(const std::string& sql, const std::vector<std::vector<Value>>& sets) = 0;
    virtual Completion run_transaction(const std::vector<Statement>& statements) = 0;
};

// Normalise placeholders. In numbered mode, rewrite `:K` -> `?` (skipping `'...'`
// string literals) and emit the matching args in `?` order; otherwise pass through.
void prepare_query(bool numbered, const std::string& sql, const std::vector<Value>& args,
                   std::string& out_sql, std::vector<Value>& out_args);

// Backend factories (each defined in its own .cpp).
std::unique_ptr<Connection> make_sqlite_connection();
std::unique_ptr<Connection> make_mysql_connection();
// Dispatch on the nanos DatabaseEngine id (SQLite=0, MySQL=1, PostgreSQL=2).
std::unique_ptr<Connection> make_connection(int engine);

} // namespace normdb

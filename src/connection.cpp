#include "connection.hpp"

#include <cctype>

namespace normdb {

void prepare_query(bool numbered, const std::string& sql, const std::vector<Value>& args,
                   std::string& out_sql, std::vector<Value>& out_args) {
    if (!numbered) {           // ? mode: the driver counts `?` placeholders itself
        out_sql = sql;
        out_args = args;
        return;
    }
    out_sql.clear();
    out_sql.reserve(sql.size());
    out_args.clear();

    bool in_str = false; // inside a '...' string literal
    for (size_t i = 0; i < sql.size();) {
        const char c = sql[i];
        if (in_str) {
            out_sql += c;
            if (c == '\'') {
                if (i + 1 < sql.size() && sql[i + 1] == '\'') { out_sql += '\''; i += 2; continue; } // '' escape
                in_str = false;
            }
            ++i;
            continue;
        }
        if (c == '\'') { in_str = true; out_sql += c; ++i; continue; }
        if (c == ':' && i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
            size_t j = i + 1, n = 0;
            while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j]))) {
                n = n * 10 + static_cast<size_t>(sql[j] - '0');
                ++j;
            }
            out_sql += '?';
            out_args.push_back(n < args.size() ? args[n] : Value(nullptr));
            i = j;
            continue;
        }
        out_sql += c;
        ++i;
    }
}

std::unique_ptr<Connection> make_connection(int engine) {
    switch (engine) {
        case 0: return make_sqlite_connection(); // SQLite
        case 1: return make_mysql_connection();  // MySQL / MariaDB
        // case 2: PostgreSQL — not implemented yet
        default: return make_sqlite_connection();
    }
}

} // namespace normdb

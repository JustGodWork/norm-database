#if defined(_WIN32)
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
#endif

#include "connection.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <mysql.h> // MariaDB Connector/C

namespace normdb {
namespace {

struct MysqlParams {
    std::string host = "127.0.0.1";
    std::string user = "root";
    std::string pass;
    std::string db;
    unsigned int port = 3306;
};

// Parse a nanos-style "host=.. port=.. user=.. password=.. db=.." string.
MysqlParams parse_params(const std::string& s) {
    MysqlParams p;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i <= start) continue;
        const std::string tok = s.substr(start, i - start);
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq);
        const std::string v = tok.substr(eq + 1);
        if      (k == "host")                       p.host = v;
        else if (k == "port")                       p.port = static_cast<unsigned int>(std::atoi(v.c_str()));
        else if (k == "user" || k == "username")    p.user = v;
        else if (k == "password" || k == "pass")    p.pass = v;
        else if (k == "db" || k == "database" || k == "dbname") p.db = v;
    }
    return p;
}

// Storage backing a MYSQL_BIND[] for parameters (must outlive mysql_stmt_execute).
struct ParamBuffers {
    std::vector<MYSQL_BIND> binds;
    std::vector<long long> i64;
    std::vector<double> dbl;
    std::vector<std::string> str;
    std::vector<unsigned long> len;
    std::vector<my_bool> isnull;

    void fill(const std::vector<Value>& args) {
        const size_t n = args.size();
        binds.assign(n, MYSQL_BIND{});
        i64.assign(n, 0);
        dbl.assign(n, 0.0);
        str.assign(n, std::string());
        len.assign(n, 0);
        isnull.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            MYSQL_BIND& b = binds[i];
            std::memset(&b, 0, sizeof(b));
            std::visit([&](auto&& a) {
                using T = std::decay_t<decltype(a)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    b.buffer_type = MYSQL_TYPE_NULL;
                    isnull[i] = 1; b.is_null = &isnull[i];
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    i64[i] = static_cast<long long>(a);
                    b.buffer_type = MYSQL_TYPE_LONGLONG; b.buffer = &i64[i];
                } else if constexpr (std::is_same_v<T, double>) {
                    dbl[i] = a;
                    b.buffer_type = MYSQL_TYPE_DOUBLE; b.buffer = &dbl[i];
                } else { // string
                    str[i] = a;
                    len[i] = static_cast<unsigned long>(str[i].size());
                    b.buffer_type = MYSQL_TYPE_STRING;
                    b.buffer = const_cast<char*>(str[i].data());
                    b.buffer_length = len[i];
                    b.length = &len[i];
                }
            }, args[i]);
        }
    }
};

class MysqlConnection : public Connection {
public:
    ~MysqlConnection() override { close(); }

    bool open(const ConnInfo& info, std::string& err) override {
        numbered_ = info.numbered;
        mysql_thread_init();
        const MysqlParams p = parse_params(info.connection);
        m_ = mysql_init(nullptr);
        if (!m_) { err = "mysql_init failed"; return false; }
        mysql_options(m_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (!mysql_real_connect(m_, p.host.c_str(), p.user.c_str(), p.pass.c_str(),
                                p.db.empty() ? nullptr : p.db.c_str(), p.port, nullptr, 0)) {
            err = mysql_error(m_);
            mysql_close(m_); m_ = nullptr;
            return false;
        }
        return true;
    }

    void close() override {
        if (m_) { mysql_close(m_); m_ = nullptr; mysql_thread_end(); }
    }

    Completion run_select(const std::string& sql, const std::vector<Value>& args) override { return exec_one(sql, args); }
    Completion run_write(const std::string& sql, const std::vector<Value>& args) override { return exec_one(sql, args); }

    Completion run_prepare(const std::string& sql, const std::vector<std::vector<Value>>& sets) override {
        Completion c;
        std::string q; std::vector<Value> ignore;
        prepare_query(numbered_, sql, sets.empty() ? std::vector<Value>{} : sets[0], q, ignore);
        MYSQL_STMT* st = mysql_stmt_init(m_);
        if (!st) { c.ok = false; c.error = mysql_error(m_); return c; }
        if (mysql_stmt_prepare(st, q.c_str(), static_cast<unsigned long>(q.size()))) {
            c.ok = false; c.error = mysql_stmt_error(st); mysql_stmt_close(st); return c;
        }
        int64_t total = 0;
        for (const auto& set : sets) {
            std::string q2; std::vector<Value> a;
            prepare_query(numbered_, sql, set, q2, a);
            ParamBuffers pb; pb.fill(a);
            if (!a.empty() && mysql_stmt_bind_param(st, pb.binds.data())) { c.ok = false; c.error = mysql_stmt_error(st); break; }
            if (mysql_stmt_execute(st)) { c.ok = false; c.error = mysql_stmt_error(st); break; }
            total += static_cast<int64_t>(mysql_stmt_affected_rows(st));
        }
        if (c.ok) { c.affected = total; c.insert_id = static_cast<int64_t>(mysql_stmt_insert_id(st)); }
        mysql_stmt_close(st);
        return c;
    }

    Completion run_transaction(const std::vector<Statement>& statements) override {
        Completion c;
        if (mysql_query(m_, "START TRANSACTION")) { c.ok = false; c.error = mysql_error(m_); return c; }
        int64_t total = 0;
        for (const auto& s : statements) {
            Completion r = exec_one(s.sql, s.params);
            if (!r.ok) { c.ok = false; c.error = r.error; break; }
            total += r.affected;
        }
        if (c.ok) {
            if (mysql_query(m_, "COMMIT")) { c.ok = false; c.error = mysql_error(m_); mysql_query(m_, "ROLLBACK"); }
            else c.affected = total;
        } else {
            mysql_query(m_, "ROLLBACK");
        }
        return c;
    }

private:
    // One prepared statement: bind params, execute, read rows OR affected/insertId.
    Completion exec_one(const std::string& sql, const std::vector<Value>& args) {
        Completion c;
        std::string q; std::vector<Value> a;
        prepare_query(numbered_, sql, args, q, a);

        MYSQL_STMT* st = mysql_stmt_init(m_);
        if (!st) { c.ok = false; c.error = mysql_error(m_); return c; }
        if (mysql_stmt_prepare(st, q.c_str(), static_cast<unsigned long>(q.size()))) {
            c.ok = false; c.error = mysql_stmt_error(st); mysql_stmt_close(st); return c;
        }
        ParamBuffers pb; pb.fill(a);
        if (!a.empty() && mysql_stmt_bind_param(st, pb.binds.data())) {
            c.ok = false; c.error = mysql_stmt_error(st); mysql_stmt_close(st); return c;
        }
        if (mysql_stmt_execute(st)) {
            c.ok = false; c.error = mysql_stmt_error(st); mysql_stmt_close(st); return c;
        }
        read_result(st, c);
        mysql_stmt_close(st);
        return c;
    }

    static void read_result(MYSQL_STMT* st, Completion& c) {
        MYSQL_RES* meta = mysql_stmt_result_metadata(st);
        if (!meta) { // not a result set -> write
            c.affected = static_cast<int64_t>(mysql_stmt_affected_rows(st));
            c.insert_id = static_cast<int64_t>(mysql_stmt_insert_id(st));
            return;
        }
        my_bool yes = 1;
        mysql_stmt_attr_set(st, STMT_ATTR_UPDATE_MAX_LENGTH, &yes);
        if (mysql_stmt_store_result(st)) {
            c.ok = false; c.error = mysql_stmt_error(st); mysql_free_result(meta); return;
        }
        const unsigned int ncol = mysql_num_fields(meta);
        MYSQL_FIELD* fields = mysql_fetch_fields(meta);

        std::vector<MYSQL_BIND> rb(ncol);
        std::vector<long long> ibuf(ncol, 0);
        std::vector<double> dbuf(ncol, 0.0);
        std::vector<std::vector<char>> sbuf(ncol);
        std::vector<unsigned long> rlen(ncol, 0);
        std::vector<my_bool> rnull(ncol, 0);
        std::vector<int> kind(ncol, 2); // 0=int 1=double 2=string
        std::vector<std::string> names(ncol);

        for (unsigned int i = 0; i < ncol; ++i) {
            std::memset(&rb[i], 0, sizeof(MYSQL_BIND));
            rb[i].is_null = &rnull[i];
            rb[i].length = &rlen[i];
            names[i] = fields[i].name ? fields[i].name : "";
            switch (fields[i].type) {
                case MYSQL_TYPE_TINY: case MYSQL_TYPE_SHORT: case MYSQL_TYPE_LONG:
                case MYSQL_TYPE_LONGLONG: case MYSQL_TYPE_INT24: case MYSQL_TYPE_YEAR:
                    kind[i] = 0; rb[i].buffer_type = MYSQL_TYPE_LONGLONG; rb[i].buffer = &ibuf[i]; break;
                case MYSQL_TYPE_FLOAT: case MYSQL_TYPE_DOUBLE:
                    kind[i] = 1; rb[i].buffer_type = MYSQL_TYPE_DOUBLE; rb[i].buffer = &dbuf[i]; break;
                default: {
                    kind[i] = 2;
                    unsigned long cap = fields[i].max_length + 1;
                    if (cap < 1) cap = 1;
                    sbuf[i].assign(cap, 0);
                    rb[i].buffer_type = MYSQL_TYPE_STRING;
                    rb[i].buffer = sbuf[i].data();
                    rb[i].buffer_length = cap;
                    break;
                }
            }
        }
        if (mysql_stmt_bind_result(st, rb.data())) {
            c.ok = false; c.error = mysql_stmt_error(st); mysql_free_result(meta); return;
        }

        int rc;
        while ((rc = mysql_stmt_fetch(st)) == 0 || rc == MYSQL_DATA_TRUNCATED) {
            Row row; row.reserve(ncol);
            for (unsigned int i = 0; i < ncol; ++i) {
                if (rnull[i]) { row.push_back(Column{ names[i], nullptr }); continue; }
                if (kind[i] == 0)      row.push_back(Column{ names[i], static_cast<int64_t>(ibuf[i]) });
                else if (kind[i] == 1) row.push_back(Column{ names[i], dbuf[i] });
                else {
                    unsigned long n = rlen[i];
                    if (n > sbuf[i].size()) n = static_cast<unsigned long>(sbuf[i].size());
                    row.push_back(Column{ names[i], std::string(sbuf[i].data(), n) });
                }
            }
            c.rows.push_back(std::move(row));
        }
        mysql_free_result(meta);
    }

    MYSQL* m_ = nullptr;
    bool numbered_ = true;
};

std::once_flag g_lib_once;

} // namespace

std::unique_ptr<Connection> make_mysql_connection() {
    std::call_once(g_lib_once, [] { mysql_library_init(0, nullptr, nullptr); });
    return std::make_unique<MysqlConnection>();
}

} // namespace normdb

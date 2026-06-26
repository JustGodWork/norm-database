// module.cpp — the C-ABI boundary (luaopen_*) and the sol2 bindings that expose
// the Database userdata class to Lua. Everything here runs on the Lua main thread.
//
// API shape mirrors the official nanos `Database`: variadic bind args, `:0/:1` or
// `?` placeholders (constructor option), Execute -> affectedRows, Insert -> insertId.
//   * `Foo`      = synchronous (BLOCKS the main thread until done, then returns).
//   * `FooAsync` = asynchronous (callback fired from Poll/PollAll).

#include <cstdio>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include <sol/sol.hpp>

#include "database.hpp"

namespace normdb {

    enum LogLevel { LogNone, LogError, LogWarn, LogInfo, LogDebug };

    [[maybe_unused]] static void print(lua_State* L, LogLevel level, const char* msg) {
        sol::state_view lua(L);
        sol::protected_function printer = lua["print"];
        std::string full_prefix = "[norm_database] ";
        switch (level) {
            case LogError: full_prefix += "ERROR: "; break;
            case LogWarn:  full_prefix += "WARN: ";  break;
            case LogInfo:  full_prefix += "INFO: ";  break;
            case LogDebug: full_prefix += "DEBUG: "; break;
            default: break;
        }
        if (printer.valid()) printer(full_prefix + (msg ? msg : "?"));
        else                 fprintf(stdout, "%s%s\n", full_prefix.c_str(), msg ? msg : "?");
    }

    // --- Lua -> Value -----------------------------------------------------------

    static Value value_from_object(const sol::object& o) {
        switch (o.get_type()) {
            case sol::type::number:
                return o.is<int64_t>() ? Value(o.as<int64_t>()) : Value(o.as<double>());
            case sol::type::string:  return Value(o.as<std::string>());
            case sol::type::boolean: return Value(static_cast<int64_t>(o.as<bool>() ? 1 : 0));
            default:                 return Value(nullptr);
        }
    }

    static std::vector<Value> args_from_variadic(const sol::variadic_args& va) {
        std::vector<Value> out; out.reserve(va.size());
        for (auto it = va.begin(); it != va.end(); ++it) {
            sol::object o = *it; // stack_proxy -> object
            out.push_back(value_from_object(o));
        }
        return out;
    }

    std::vector<Value> params_from_lua(const sol::object& obj) {
        std::vector<Value> out;
        if (!obj.valid() || obj.get_type() != sol::type::table) return out;
        sol::table t = obj.as<sol::table>();
        const size_t n = t.size();
        out.reserve(n);
        for (size_t i = 1; i <= n; ++i) {
            sol::object e = t[i];
            out.push_back(value_from_object(e));
        }
        return out;
    }

    // --- Value -> Lua -----------------------------------------------------------

    sol::object value_to_lua(lua_State* L, const Value& v) {
        sol::state_view lua(L);
        return std::visit([&](auto&& arg) -> sol::object {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) return sol::lua_nil;
            else return sol::make_object(lua, arg);
        }, v);
    }

    // --- transaction / prepare shape helpers ------------------------------------

    static sol::protected_function callback_at(const sol::variadic_args& va, size_t index) {
        if (va.size() > index) {
            sol::object o = va[index];
            if (o.is<sol::protected_function>()) return o.as<sol::protected_function>();
        }
        return sol::protected_function();
    }

    static std::vector<Statement> statements_from_lua(const sol::object& obj) {
        std::vector<Statement> out;
        if (obj.get_type() != sol::type::table) return out;
        sol::table arr = obj.as<sol::table>();
        const size_t n = arr.size();
        out.reserve(n);
        for (size_t i = 1; i <= n; ++i) {
            sol::object e = arr[i];
            if (e.get_type() != sol::type::table) continue;
            sol::table st = e.as<sol::table>();
            Statement s;
            s.sql = st.get_or("sql", std::string());
            s.params = params_from_lua(st["params"]);
            out.push_back(std::move(s));
        }
        return out;
    }

    static std::vector<std::vector<Value>> param_sets_from_lua(const sol::object& obj) {
        std::vector<std::vector<Value>> out;
        if (obj.get_type() != sol::type::table) return out;
        sol::table arr = obj.as<sol::table>();
        const size_t n = arr.size();
        out.reserve(n);
        for (size_t i = 1; i <= n; ++i) out.push_back(params_from_lua(arr[i]));
        return out;
    }

    // --- sync helper: enqueue + BLOCK until a worker fills the result -----------
    static std::tuple<sol::object, sol::object> run_sync(lua_State* L, Database& d, JobKind kind,
            std::string sql, std::vector<Value> args,
            std::vector<std::vector<Value>> sets, std::vector<Statement> stmts) {
        SyncSlot slot;
        d.submit_sync(kind, std::move(sql), std::move(args), std::move(sets), std::move(stmts), &slot);
        {
            std::unique_lock<std::mutex> lk(slot.m);
            slot.cv.wait(lk, [&] { return slot.done; });
        }
        return make_result(L, slot.result); // built on the main thread
    }

    // --- registry: lets NormDatabase.PollAll() drain every open connection -------

    static std::mutex g_registry_mtx;
    static std::vector<std::weak_ptr<Database>> g_registry;

    static std::shared_ptr<Database> make_database(sol::table opts, sol::variadic_args va) {
        const int engine = opts.get_or("engine", 0);
        std::string conn = opts.get_or("connection", std::string(":memory:"));
        const int pool = opts.get_or("pool_size", 10);
        const std::string ph = opts.get_or("placeholders", std::string(":"));
        const bool numbered = (ph != "?");
        sol::protected_function ready = callback_at(va, 0);

        auto db = std::make_shared<Database>(engine, std::move(conn), pool, numbered, std::move(ready));
        {
            std::lock_guard<std::mutex> lk(g_registry_mtx);
            g_registry.push_back(db);
        }
        return db;
    }

    static int poll_all(lua_State* L) {
        std::vector<std::shared_ptr<Database>> alive;
        {
            std::lock_guard<std::mutex> lk(g_registry_mtx);
            for (auto it = g_registry.begin(); it != g_registry.end();) {
                if (auto sp = it->lock()) { alive.push_back(std::move(sp)); ++it; }
                else it = g_registry.erase(it);
            }
        }
        int fired = 0;
        for (auto& d : alive) fired += d->poll(L);
        return fired;
    }

} // namespace normdb

// ---------------------------------------------------------------------------
// C-ABI entry point.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
    #define NORMDB_EXPORT extern "C" __declspec(dllexport)
#else
    #define NORMDB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

NORMDB_EXPORT int luaopen_norm_database(lua_State* L) {
    using namespace normdb;
    sol::state_view lua(L);

    lua.new_usertype<Database>("NormDatabase",
        sol::call_constructor, sol::factories(&make_database),

        // --- sync (BLOCKING): db:Select(sql, ...args) -> result, err ---
        "Select", [](Database& d, std::string sql, sol::this_state ts, sol::variadic_args va) {
            return run_sync(ts, d, JobKind::Select, std::move(sql), args_from_variadic(va), {}, {});
        },
        "Execute", [](Database& d, std::string sql, sol::this_state ts, sol::variadic_args va) {
            return run_sync(ts, d, JobKind::Execute, std::move(sql), args_from_variadic(va), {}, {});
        },
        "Insert", [](Database& d, std::string sql, sol::this_state ts, sol::variadic_args va) {
            return run_sync(ts, d, JobKind::Insert, std::move(sql), args_from_variadic(va), {}, {});
        },
        "Update", [](Database& d, std::string sql, sol::this_state ts, sol::variadic_args va) {
            return run_sync(ts, d, JobKind::Update, std::move(sql), args_from_variadic(va), {}, {});
        },
        "Prepare", [](Database& d, std::string sql, sol::object sets, sol::this_state ts) {
            return run_sync(ts, d, JobKind::Prepare, std::move(sql), {}, param_sets_from_lua(sets), {});
        },
        "Transaction", [](Database& d, sol::object statements, sol::this_state ts) {
            return run_sync(ts, d, JobKind::Transaction, std::string(), {}, {}, statements_from_lua(statements));
        },

        // --- async (callback FIRST, then variadic bind args) ---
        "SelectAsync", [](Database& d, std::string sql, sol::protected_function cb, sol::variadic_args va) {
            d.select(std::move(sql), args_from_variadic(va), std::move(cb));
        },
        "ExecuteAsync", [](Database& d, std::string sql, sol::protected_function cb, sol::variadic_args va) {
            d.execute(std::move(sql), args_from_variadic(va), std::move(cb));
        },
        "InsertAsync", [](Database& d, std::string sql, sol::protected_function cb, sol::variadic_args va) {
            d.insert(std::move(sql), args_from_variadic(va), std::move(cb));
        },
        "UpdateAsync", [](Database& d, std::string sql, sol::protected_function cb, sol::variadic_args va) {
            d.update(std::move(sql), args_from_variadic(va), std::move(cb));
        },
        "PrepareAsync", [](Database& d, std::string sql, sol::object sets, sol::variadic_args va) {
            d.prepare(std::move(sql), param_sets_from_lua(sets), callback_at(va, 0));
        },
        "TransactionAsync", [](Database& d, sol::object statements, sol::variadic_args va) {
            d.transaction(statements_from_lua(statements), callback_at(va, 0));
        },

        "Poll", [](Database& d, sol::this_state ts) { return d.poll(ts); },
        "Close", [](Database& d) { d.close(); }
    );

    sol::table cls = lua["NormDatabase"];
    cls["Engine"] = lua.create_table_with("SQLite", 0, "MySQL", 1, "PostgreSQL", 2);
    cls["PollAll"] = [](sol::this_state ts) { return poll_all(ts); };
    cls["poll_all"] = [](sol::this_state ts) { return poll_all(ts); }; // legacy alias

    // Polling is Lua-driven: each consumer wires `Server.Subscribe("Tick",
    // NormDatabase.PollAll)`. A C++ module is just a library (no VM / package
    // lifecycle of its own), so it cannot own a Tick subscription.

    return sol::stack::push(L, cls);
}

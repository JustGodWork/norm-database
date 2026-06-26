# norm-database

A native (C++) **database connector** for nanos world, exposed to Lua as a clean
userdata class. It exists to give [Norm](https://github.com/JustGodWork/norm) what
the built-in `Database` lacks: **transactions**, **reliable insertId**, and
**prepared statements**. Both **async** (callback) and **sync** (yielding, inside a
coroutine) variants of every method, plus a connection **pool**.

> First engine: **SQLite**. MySQL / PostgreSQL are planned behind the same API.

## How it works

- Built as a C++ shared library; only the `luaopen_norm_database` entry point is
  `extern "C"` (the rest is full C++: [sol2](https://github.com/ThePhD/sol2) +
  `std::thread` + STL).
- Queries run on a **pool** of `pool_size` worker threads, each owning its own
  SQLite connection. Workers **never touch the Lua state**. Results land on a
  mutex-protected queue; **`Poll`/`PollAll`** (called each tick on the main thread)
  drains it and fires callbacks / resumes coroutines. → Lua stays single-threaded,
  DB I/O stays off the game thread.
- A job runs entirely on one connection, so a **transaction** is atomic and
  `insertId` is the real `last_insert_rowid()` of that connection.

## API

The shape mirrors the official nanos `Database`: **variadic** bind args, `:0/:1`
placeholders (or `?`), `Execute` → affectedRows, `Insert` → insertId.

```lua
-- `NormDatabase` is a GLOBAL once the module loads as a package dependency
-- (no `require` needed). Instantiate by calling the class table:
local db = NormDatabase({
    engine       = NormDatabase.Engine.SQLite,
    connection   = "game.db",
    pool_size    = 4,        -- worker/connection count (default 1)
    placeholders = ":",      -- ":" => :0/:1 named binding (default) | "?" => positional binding
}, function(ok, err) end)    -- optional on_ready(ok, err)

-- async: callback FIRST, then variadic bind args
db:SelectAsync(sql, cb, ...)        -- cb(rows, err)
db:ExecuteAsync(sql, cb, ...)       -- cb(affectedRows, err)
db:InsertAsync(sql, cb, ...)        -- cb(insertId, err)
db:UpdateAsync(sql, cb, ...)        -- cb(affectedRows, err)
db:PrepareAsync(sql, paramSets, cb) -- cb(affectedRows, err)  paramSets = { {..}, {..} } (batch)
db:TransactionAsync(statements, cb) -- cb(ok, err)            statements = { { sql=, params= }, ... }

db:Poll()                           -- drain THIS db
NormDatabase.PollAll()              -- drain EVERY open db
db:Close()
```

> **Wire the polling from Lua** (once, in any live script):
> `Server.Subscribe("Tick", NormDatabase.PollAll)`. `PollAll` drains every open db,
> so one live subscription serves them all. A C++ module is just a library (no VM /
> package lifecycle of its own), so it can't own the Tick subscription itself.

```lua
-- :0/:1 and ? are BOTH real bound parameters (injection-safe — do NOT quote them).
db:ExecuteAsync("INSERT INTO t VALUES (:0, :1)", function(affected) end, 123, "MyValue")
db:SelectAsync("SELECT * FROM t WHERE name = :0", function(rows) end, "Val")
db:Select("SELECT * FROM t WHERE name = ?", "Val") -- placeholders = "?" mode
```

> `:N` maps arg *i* to the SQLite named parameter `:i` via `sqlite3_bind_parameter_index`
> (real binding, no string interpolation). Drop-in for nanos `Database` queries.

### Sync (blocking) — call anywhere, like nanos

Every operation has a **no-`Async`** variant that **BLOCKS** the main thread until a
worker finishes, then returns `(result, err)` directly. No coroutine, callable
anywhere — a drop-in for nanos `Database`'s synchronous methods.

```lua
db:Execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)")
local id   = db:Insert("INSERT INTO users(name) VALUES(:0)", "Zoe")  -- -> insertId
local rows = db:Select("SELECT * FROM users WHERE id = :0", id)      -- -> rows
local ok   = db:Transaction({
    { sql = "UPDATE users SET name = :0 WHERE id = :1", params = { "Z", id } },
    { sql = "INSERT INTO users(name) VALUES(:0)",       params = { "Max" } },
})
```

> Sync blocks the game thread while the query runs (same as nanos). For heavy queries
> prefer the `*Async` variants. Note: `Poll`/`PollAll` (the Lua-wired Tick) is only
> needed to deliver **async** callbacks — sync calls don't depend on it.

> **Why poll (for async)?** DB I/O runs on worker threads, but Lua can only be touched
> on the main thread. `PollAll` is the hand-off that fires async callbacks. Wire it:
> `Server.Subscribe("Tick", NormDatabase.PollAll)`.

Type definitions: `norm_database.meta.lua`.

## Build

Requires CMake ≥ 3.16, a C++17 compiler, and network access on first configure
(CMake `FetchContent` pulls sol2 + the SQLite amalgamation).

```
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

Output: `norm_database.dll` (Windows) / `norm_database.so` (Linux). Drop it where
nanos loads modules; it sets the global `NormDatabase` (no `require` needed).

## Status / roadmap

- [x] SQLite engine, callback API, poll-drain queue, batch transaction, prepare.
- [ ] MySQL / PostgreSQL engines (same API, behind libmariadb / libpq).
- [ ] Interactive transactions (a pinned connection for logic-between-statements).
- [ ] A Norm adapter (`Norm.adapters.norm_database`) wiring `supports_transactions`
      / `supports_returning` to `true`.

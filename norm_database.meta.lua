---@meta

-- Type definitions for the `norm_database` native module. Loaded by
-- lua-language-server for autocomplete only; never executed. `NormDatabase` is a
-- global (set when the native module loads).

---@class NormDatabaseOptions
---@field engine integer          One of NormDatabase.Engine.* (SQLite / MySQL / PostgreSQL).
---@field connection string       SQLite: a file path (":memory:" for an in-memory db).
---@field pool_size? integer      Worker/connection count (default 10).
---@field placeholders? string    ":" => :0/:1 named binding (default) | "?" => positional binding.

---@class NormDatabaseStatement
---@field sql string
---@field params? any[]

---@alias NormDatabaseRow table<string, any>
---@alias NormDatabaseReadyCb fun(ok: boolean, err: string|nil)
---@alias NormDatabaseSelectCb fun(rows: NormDatabaseRow[]|nil, err: string|nil)
---@alias NormDatabaseCountCb fun(affectedRows: integer|nil, err: string|nil)
---@alias NormDatabaseInsertCb fun(insertId: integer|nil, err: string|nil)
---@alias NormDatabaseTxCb fun(ok: boolean|nil, err: string|nil)

---@class NormDatabaseEngines
---@field SQLite integer
---@field MySQL integer
---@field PostgreSQL integer

--- An async database connection (native userdata).
--- Construct it by calling the class table: `local db = NormDatabase(opts, on_ready?)`.
--- Bind values are passed as VARIADIC args; reference them with `:0 :1 ...` (default)
--- or `?` (when `placeholders = "?"`).
---@class NormDatabase
---@overload fun(options: NormDatabaseOptions, on_ready?: NormDatabaseReadyCb): NormDatabase
NormDatabase = {}

--- Engine ids (mirror nanos `DatabaseEngine`).
---@type NormDatabaseEngines
NormDatabase.Engine = {}

-- ===== async (callback FIRST, then variadic bind args) ======================
-- Each callback runs inside a fresh coroutine, so you MAY call the sync (yielding)
-- methods (db:Select/Insert/...) directly from within an *Async callback.

---@param sql string
---@param cb NormDatabaseSelectCb
---@param ... any  Bind values.
function NormDatabase:SelectAsync(sql, cb, ...) end

---@param sql string
---@param cb NormDatabaseCountCb   Receives affectedRows.
---@param ... any
function NormDatabase:ExecuteAsync(sql, cb, ...) end

---@param sql string
---@param cb NormDatabaseInsertCb  Receives insertId.
---@param ... any
function NormDatabase:InsertAsync(sql, cb, ...) end

---@param sql string
---@param cb NormDatabaseCountCb
---@param ... any
function NormDatabase:UpdateAsync(sql, cb, ...) end

---@param sql string
---@param paramSets any[][]        One param array per row (batch).
---@param cb? NormDatabaseCountCb
function NormDatabase:PrepareAsync(sql, paramSets, cb) end

---@param statements NormDatabaseStatement[]
---@param cb? NormDatabaseTxCb
function NormDatabase:TransactionAsync(statements, cb) end

-- ===== sync (BLOCKING — like nanos; call anywhere, no coroutine) ============
-- Each blocks the main thread until a worker finishes, then returns (result, err).

---@param sql string
---@param ... any
---@return NormDatabaseRow[]|nil rows
---@return string|nil err
function NormDatabase:Select(sql, ...) end

---@param sql string
---@param ... any
---@return integer|nil affectedRows
---@return string|nil err
function NormDatabase:Execute(sql, ...) end

---@param sql string
---@param ... any
---@return integer|nil insertId
---@return string|nil err
function NormDatabase:Insert(sql, ...) end

---@param sql string
---@param ... any
---@return integer|nil affectedRows
---@return string|nil err
function NormDatabase:Update(sql, ...) end

---@param sql string
---@param paramSets any[][]
---@return integer|nil affectedRows
---@return string|nil err
function NormDatabase:Prepare(sql, paramSets) end

---@param statements NormDatabaseStatement[]
---@return boolean|nil ok
---@return string|nil err
function NormDatabase:Transaction(statements) end

-- ===== lifecycle ============================================================

--- Drain THIS connection's finished queries and fire their callbacks. Main thread.
---@return integer fired
function NormDatabase:Poll() end

--- Drain EVERY open connection. Wire it to the tick from any live script:
--- `Server.Subscribe("Tick", NormDatabase.PollAll)` — it serves every open db.
---@return integer fired
function NormDatabase.PollAll() end

--- Stop the workers and close the connections.
function NormDatabase:Close() end

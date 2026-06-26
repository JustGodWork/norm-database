--- norm_adapter.lua — a Norm ORM adapter for the `norm_database` native module.
---
--- Bridges the connector to Norm so `Norm.new({ adapter = Norm.adapters.norm_database.new(...) })`
--- gets reliable insertId (via `INSERT ... RETURNING` on SQLite/MariaDB) and the
--- full pool/sync/async machinery. Lives HERE (not in Norm core) so the ORM stays
--- portable and unaware of this nanos-only module.
---
--- Load order: Norm and the norm_database module must already be loaded (both set
--- their globals). Then:
---     require 'norm-database/lua/norm_adapter.lua'   -- registers Norm.adapters.norm_database
---
---     local db = Norm.new({
---         adapter = Norm.adapters.norm_database.new({
---             engine     = NormDatabase.Engine.MySQL,        -- or .SQLite
---             connection = "host=127.0.0.1 port=3306 user=root password=.. db=mydb",
---             pool_size  = 10,
---             -- returning = true,   -- override the auto-detected RETURNING support
---         }),
---     })
---
--- Fully ASYNC / non-blocking. The adapter wires `Server.Subscribe("Tick",
--- NormDatabase.PollAll)` for you (consumer context, survives reloads). Query results
--- settle Norm's promises one frame later (see `defer`), so an ORM call never blocks
--- the game thread — important when many queries fire at once.

assert(Norm, "[norm_database] load Norm before this adapter");
assert(NormDatabase, "[norm_database] the norm_database native module must be loaded first");

local class <const> = Norm.class;
local NormAdapter <const> = Norm.Adapter;

---@class NormDatabaseAdapterOptions: NormAdapterOptions
---@field engine integer        NormDatabase.Engine.SQLite or .MySQL.
---@field connection string     SQLite path, or MySQL "host=.. port=.. user=.. password=.. db=..".
---@field pool_size? integer    Worker/connection count (default 10).
---@field returning? boolean    Force INSERT...RETURNING support (else auto-detected).
---@field on_ready? fun(ok: boolean, err: string|nil)

---@return boolean, NormDatabaseAdapter|nil
local adapter_exists, NormDatabaseAdapter = pcall(function() return class["NormDatabaseAdapter"] end);

-- Wire the Tick poll ONCE (in the consumer's package context, so a reload re-wires
-- it). PollAll drains every open db, so one subscription serves them all.
local poll_wired = false;
local function wire_poll()
    if (poll_wired) then return; end
    if (Server and Server.Subscribe and NormDatabase.PollAll) then
        Server.Subscribe("Tick", NormDatabase.PollAll);
        poll_wired = true;
    end
end

-- Norm's built-in promise resumes the awaiting coroutine SYNCHRONOUSLY when it
-- settles. If we settled it straight from our Tick poll, that resume would run while
-- our C `PollAll` is on the stack — and nanos crashes (Access Violation) when you
-- resume a coroutine from inside a native callback. `defer` bounces the work to the
-- NEXT frame (a plain-Lua Timer callback, off our C stack) — the standard way event
-- loops marshal callbacks (cf. JS setImmediate / process.nextTick). Cost: one frame
-- of latency per query; benefit: we never resume a coroutine from C.
local function defer(fn)
    Timer.SetTimeout(fn, 0);
end

-- MariaDB >= 10.5 and SQLite >= 3.35 support `INSERT ... RETURNING`; real MySQL does
-- not. SQLite (our bundled amalgamation) is always recent enough. For MySQL we probe
-- the server version synchronously (db:Select blocks until the connection is up).
local function detect_returning(db, engine)
    if (engine ~= NormDatabase.Engine.MySQL) then return true; end
    local rows, err = db:Select("SELECT VERSION() AS v");
    if (err or not rows or not rows[1]) then return false; end
    local v = tostring(rows[1].v or "");
    if (not v:find("MariaDB")) then return false; end -- real MySQL: no RETURNING
    local maj, min = 0, 0;
    for a, b in v:gmatch("(%d+)%.(%d+)") do
        local A, B = tonumber(a), tonumber(b);
        if (A > maj or (A == maj and B > min)) then maj, min = A, B; end
    end
    return (maj > 10) or (maj == 10 and min >= 5);
end

---@class NormDatabaseAdapter: NormAdapter
NormDatabaseAdapter = adapter_exists and NormDatabaseAdapter or class.extend("NormDatabaseAdapter", NormAdapter);

---@private
---@param options NormDatabaseAdapterOptions
function NormDatabaseAdapter:__init(options)
    options = options or {};
    self.engine = options.engine or NormDatabase.Engine.SQLite;
    -- Pin the dialect EXPLICITLY (don't depend on get_dialect_name being resolved):
    -- a SQLite engine MUST get the sqlite dialect, not mysql.
    if (options.dialect == nil) then
        options.dialect = (self.engine == NormDatabase.Engine.MySQL) and "mysql" or "sqlite";
    end
    NormAdapter.__init(self, options);

    -- Norm emits positional `?` SQL, so open in "?" placeholder mode.
    self.db = NormDatabase({
        engine = self.engine,
        connection = options.connection,
        pool_size = options.pool_size,
        placeholders = "?",
    }, options.on_ready);

    wire_poll();

    if (options.returning ~= nil) then
        self._returning = options.returning and true or false;
    else
        self._returning = detect_returning(self.db, self.engine);
    end
end

---@return "mysql"|"sqlite"
function NormDatabaseAdapter:get_dialect_name()
    if (self.engine == NormDatabase.Engine.MySQL) then return "mysql"; end
    return "sqlite";
end

--- INSERT...RETURNING support (SQLite >= 3.35, MariaDB >= 10.5) -> pool-safe insertId.
---@return boolean
function NormDatabaseAdapter:supports_returning()
    return self._returning == true;
end

--- The connector only has BATCH transactions; Norm's interactive `transaction(body)`
--- needs a pinned connection (a connector roadmap item), so it's not supported yet.
--- Use `db:Transaction({ ... })` on the raw connector for atomic batches meanwhile.
---@return boolean
function NormDatabaseAdapter:supports_transactions()
    return false;
end

--- Use Norm's BUILT-IN (pure-Lua) promise provider (returning nil = the default).
--- Its :await() is plain coroutine.yield/resume, which we settle via `defer` (next
--- frame) — never from inside our C poll, so nanos doesn't crash. The native nanos
--- `Promise` is intentionally avoided: it binds resumption to nanos's own scheduler.
---@return NormPromiseProvider|nil
function NormDatabaseAdapter:default_provider()
    return nil;
end

---@param query string
---@param params any[]
---@param callback NormQueryCallback
function NormDatabaseAdapter:raw_query(query, params, callback)
    params = params or {};
    self.db:SelectAsync(query, function(rows, err)
        defer(function() callback(err, rows or {}); end);
    end, table.unpack(params, 1, #params));
end

---@param query string
---@param params any[]
---@param callback NormExecuteCallback
function NormDatabaseAdapter:raw_execute(query, params, callback)
    params = params or {};
    self.db:ExecuteAsync(query, function(affected, err)
        defer(function() callback(err, { affectedRows = affected }); end);
    end, table.unpack(params, 1, #params));
end

---@class NormDatabaseAdapterModule
---@field class NormDatabaseAdapter
local M = {};

--- Create a norm_database adapter. Pass it to `Norm.new`.
---@param options NormDatabaseAdapterOptions
---@return NormDatabaseAdapter
function M.new(options) return NormDatabaseAdapter(options); end
M.class = NormDatabaseAdapter;

return M;

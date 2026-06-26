--- usage.lua — driving the norm_database module from Lua (nanos-style API).
---
--- `NormDatabase` is a GLOBAL (set when the native module loads as a package
--- dependency) — no `require` needed. Bind values are VARIADIC; reference them
--- with :0/:1 (default) or ? (placeholders = "?"). :0/:1 are REAL bound parameters
--- (injection-safe — do NOT quote them).

-- ===== ASYNC (callback): needs the Lua-wired Tick poll to deliver =====
Server.Subscribe("Tick", NormDatabase.PollAll)

local db = NormDatabase({
    engine     = NormDatabase.Engine.SQLite,
    connection = "game.db",
    pool_size  = 10,
})

-- ===== SYNC (blocking, like nanos): no callback, no coroutine, no poll =====
db:Execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT, coins INTEGER DEFAULT 0)")

local id = db:Insert("INSERT INTO users (name, coins) VALUES (:0, :1)", "Zoe", 100) -- -> insertId
db:Execute("UPDATE users SET coins = :0 WHERE id = :1", 250, id)                    -- -> affectedRows

local rows = db:Select("SELECT * FROM users WHERE id = :0", id)                     -- -> rows
Console.Log(("user %d = %s, %d coins"):format(rows[1].id, rows[1].name, rows[1].coins))

local ok = db:Transaction({                                                        -- -> boolean (atomic)
    { sql = "UPDATE users SET coins = coins - 100 WHERE id = :0", params = { id } },
    { sql = "INSERT INTO users (name, coins) VALUES (:0, :1)",    params = { "Bank", 100 } },
})

db:SelectAsync("SELECT * FROM users WHERE coins > :0", function(r, err)
    if (r) then Console.Log(("%d rich users"):format(#r)); end
end, 50) -- callback FIRST, then variadic bind args

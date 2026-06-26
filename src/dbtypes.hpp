#pragma once

// Shared value/result types used by the Database and by every connection backend.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <sol/sol.hpp> // Completion carries the async callback

namespace normdb {

// A single SQL value crossing the C++/driver boundary.
using Value = std::variant<std::nullptr_t, int64_t, double, std::string>;

struct Column {
    std::string name;
    Value value;
};
using Row = std::vector<Column>;

// One statement inside a transaction.
struct Statement {
    std::string sql;
    std::vector<Value> params;
};

enum class JobKind {
    Select, Execute, Insert, Update, Prepare, Transaction, Ready, Close
};

// The result of running a job (produced by a backend on a worker thread; the `cb`
// is filled by the Database for async delivery and is otherwise empty/unused).
struct Completion {
    JobKind kind = JobKind::Execute;
    sol::protected_function cb; // async only (never touched by backends)
    bool ok = true;
    std::string error;
    std::vector<Row> rows; // Select
    int64_t affected = 0;  // Execute/Update/Prepare/Transaction
    int64_t insert_id = 0; // Insert
};

} // namespace normdb

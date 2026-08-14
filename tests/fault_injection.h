#pragma once

#include <sqlite3.h>

#include <stdexcept>
#include <string>

// Makes a database write fail at a chosen point, so a test can enter the window
// an interrupted operation leaves behind instead of reading it off the
// transaction boundaries.
//
// Both injectors are pure SQL against a test's connection: production code has
// no seam and needs none. `BlockedWrites` is the `PRAGMA query_only` trick
// tests/test_organize_exec.cpp already uses, lifted so other files can reach
// it; `BlockedImageDeletes` narrows the same idea to one statement, which is
// what tells a per-row commit apart from a batch that rolls back whole.
//
// A test must assert that its fault fired. One whose fault silently stopped
// firing passes forever while pinning nothing.

// Whether a blocked delete is counted as well as refused.
enum class FaultWitness {
    // The trigger calls a counting function registered on the connection
    // passed in, and `fired()` reports how many deletes it stopped. Use this
    // whenever the code under test writes through that same connection.
    Counted,
    // No counter. A SQL function is per-connection, so an operation that opens
    // its own — cmd_rm does — could not compile the trigger body and its delete
    // would fail while preparing rather than while aborting. The witness is
    // then the state: rows that a completed run would have deleted are still
    // there, and the run threw.
    StateOnly,
};

// Aborts DELETE on `images`, either every one or only the row at one path.
//
// A path-scoped blocker is how a test distinguishes "the deletes commit as a
// single transaction" from "each row commits on its own": block the second of
// two rows and the first one's fate is the answer.
//
// The count lives in C++ rather than in a table because an abort rolls back
// everything the statement wrote, a trigger's own inserts included — and
// because delete_paths_logged issues its ROLLBACK before it reads
// sqlite3_errmsg, so the abort text never reaches the exception either.
class BlockedImageDeletes {
   public:
    // The text RAISE(ABORT) reports. Reaches a caller only where nothing else
    // runs on the connection in between, so `fired()` is the assertion to make.
    static constexpr const char* kMessage = "phig-test-fault: image delete blocked";

    // `path` empty blocks every delete; otherwise only the row whose path
    // matches. Throws if the trigger could not be installed, so a test never
    // runs believing a fault is armed when it is not.
    explicit BlockedImageDeletes(sqlite3* db, const std::string& path = {},
                                 FaultWitness witness = FaultWitness::Counted)
        : db_(db), counted_(witness == FaultWitness::Counted) {
        if (counted_ && sqlite3_create_function_v2(db_, kMarker, 0, SQLITE_UTF8, this, &mark,
                                                   nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to register the delete blocker's marker");
        std::string sql = "CREATE TRIGGER " + std::string(kTrigger) + " BEFORE DELETE ON images\n";
        if (!path.empty()) sql += "WHEN OLD.path = '" + quote(path) + "'\n";
        sql += "BEGIN\n";
        if (counted_) sql += "SELECT " + std::string(kMarker) + "();\n";
        sql += "SELECT RAISE(ABORT, '" + std::string(kMessage) + "');\nEND";
        exec_or_throw(sql, "install the delete blocker");
        if (!installed()) throw std::runtime_error("delete blocker did not install");
        armed_ = true;
    }

    ~BlockedImageDeletes() { release(); }

    BlockedImageDeletes(const BlockedImageDeletes&) = delete;
    BlockedImageDeletes& operator=(const BlockedImageDeletes&) = delete;

    // Drops the trigger early, so the same run can be repeated unblocked. The
    // count survives, so a test can release and still assert the fault fired.
    void release() {
        if (!armed_) return;
        armed_ = false;
        sqlite3_exec(db_, ("DROP TRIGGER IF EXISTS " + std::string(kTrigger)).c_str(), nullptr,
                     nullptr, nullptr);
        if (counted_)
            sqlite3_create_function_v2(db_, kMarker, 0, SQLITE_UTF8, nullptr, nullptr, nullptr,
                                       nullptr, nullptr);
    }

    // How many deletes the trigger has aborted, under FaultWitness::Counted.
    // Zero means the fault never fired and whatever the test observed proves
    // nothing. Always zero under FaultWitness::StateOnly.
    int fired() const { return fired_; }

    // Whether the trigger is in the schema right now — it is stored in the
    // database file, so every connection to it sees the same answer.
    bool installed() const {
        sqlite3_stmt* s = nullptr;
        const char* q = "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name=?";
        if (sqlite3_prepare_v2(db_, q, -1, &s, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(s, 1, kTrigger, -1, SQLITE_STATIC);
        int n = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int(s, 0) : 0;
        sqlite3_finalize(s);
        return n == 1;
    }

   private:
    static constexpr const char* kTrigger = "phig_test_block_image_delete";
    static constexpr const char* kMarker = "phig_test_fault_mark";

    static void mark(sqlite3_context* ctx, int, sqlite3_value**) {
        static_cast<BlockedImageDeletes*>(sqlite3_user_data(ctx))->fired_++;
        sqlite3_result_int(ctx, 1);
    }

    static std::string quote(const std::string& s) {
        std::string out;
        for (char c : s) {
            out += c;
            if (c == '\'') out += '\'';
        }
        return out;
    }

    void exec_or_throw(const std::string& sql, const std::string& what) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("Failed to " + what + ": " + msg);
        }
    }

    sqlite3* db_;
    bool counted_;
    bool armed_ = false;
    int fired_ = 0;
};

// Fails every write on this connection: the point where the disk is already
// changed and only the database write is left. Unlike a blocked delete, the
// refusal reaches the caller intact, so its message is the witness.
class BlockedWrites {
   public:
    // SQLite's own wording for a query_only refusal.
    static constexpr const char* kMessage = "attempt to write a readonly database";

    explicit BlockedWrites(sqlite3* db) : db_(db) {
        if (sqlite3_exec(db_, "PRAGMA query_only=ON", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to set query_only");
        armed_ = true;
    }

    ~BlockedWrites() { release(); }

    BlockedWrites(const BlockedWrites&) = delete;
    BlockedWrites& operator=(const BlockedWrites&) = delete;

    void release() {
        if (!armed_) return;
        armed_ = false;
        sqlite3_exec(db_, "PRAGMA query_only=OFF", nullptr, nullptr, nullptr);
    }

   private:
    sqlite3* db_;
    bool armed_ = false;
};

// The what() of the exception `fn` threw, or "" if it returned normally.
// Checking the result for a message asserts both halves at once: that the
// operation failed, and that it failed where the fault was injected.
template <typename Fn>
inline std::string thrown_message(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "<non-std exception>";
    }
    return "";
}

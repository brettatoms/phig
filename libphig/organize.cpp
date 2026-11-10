#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "internal.h"

namespace {

// Empty for a value outside the enum, which every caller refuses rather than
// mapping to a default (R73).
std::optional<ConflictResolution> from_abi(phig_conflict_resolution r) {
    switch (r) {
        case PHIG_CONFLICT_SKIP:
            return ConflictResolution::Skip;
        case PHIG_CONFLICT_OVERWRITE:
            return ConflictResolution::Overwrite;
        case PHIG_CONFLICT_RENAME:
            return ConflictResolution::Rename;
    }
    return std::nullopt;
}

phig_conflict_resolution to_abi(ConflictResolution r) {
    switch (r) {
        case ConflictResolution::Skip:
            return PHIG_CONFLICT_SKIP;
        case ConflictResolution::Overwrite:
            return PHIG_CONFLICT_OVERWRITE;
        case ConflictResolution::Rename:
            return PHIG_CONFLICT_RENAME;
    }
    return PHIG_CONFLICT_RENAME;
}

// Runs `body` on the operation's thread, translating however it ends into the
// handle's terminal state. Mirrors operations.cpp's spawn for the organize
// kinds, which use their own progress queue.
template <typename Body>
void spawn_organize(phig_op* op, const Body& body) {
    op->worker = std::thread([op, body]() {
        phig_op_state final_state = PHIG_OP_DONE;
        try {
            final_state = body() ? PHIG_OP_DONE : PHIG_OP_CANCELLED;
        } catch (const std::exception& e) {
            op->failure = e.what();
            final_state = PHIG_OP_FAILED;
        } catch (...) {
            op->failure = "unknown internal error";
            final_state = PHIG_OP_FAILED;
        }
        op->organize_progress.for_each([op](const OrganizeProgress& p) {
            if (!p.error.empty()) op->retain_error(p.error);
        });
        op->organize_progress.close();
        op->state.store(final_state, std::memory_order_release);
    });
}

// Shared start for copy and move: snapshot the caller's plan, then run the
// matching executor.
phig_status start_apply(phig_ctx* ctx, const phig_organize_plan* plan, bool is_move,
                        phig_op** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    PHIG_TRY(ctx)
    if (plan == nullptr) return PHIG_ERR_INVALID_ARG;
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;

    auto op = std::make_unique<phig_op>(is_move ? phig_op::Kind::Move : phig_op::Kind::Copy);
    op->db = std::make_unique<Database>(ctx->db_path);
    // Snapshot: the caller may free its plan as soon as this returns.
    op->organize_plan = plan->plan;

    OrganizeOptions opts;
    opts.db_path = ctx->db_path;
    opts.log_enabled = true;
    opts.argv_str = is_move ? "libphig move" : "libphig copy";

    phig_op* raw = op.get();
    spawn_organize(raw, [raw, opts, is_move]() {
        OrganizeEngine engine(opts, *raw->db);
        raw->organize_result =
            engine.apply(raw->organize_plan, is_move, raw->cancel, &raw->organize_progress);
        return !raw->organize_result.cancelled;
    });
    *out = op.release();
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

}  // namespace

phig_status phig_op_organize_plan_start(phig_ctx* ctx, const phig_organize_options* opts,
                                        phig_op** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    PHIG_TRY(ctx)
    if (opts == nullptr || opts->destination == nullptr) return PHIG_ERR_INVALID_ARG;
    check_id_array(opts->ids, opts->id_count, "organize");
    std::optional<ConflictResolution> policy = from_abi(opts->on_conflict);
    if (!policy) {
        throw InvalidOption("organize: unknown conflict resolution " +
                            std::to_string(static_cast<int>(opts->on_conflict)));
    }

    // Planning writes nothing, so a read-only context may compute one. Only the
    // apply entry points require PHIG_READ_WRITE.
    auto op = std::make_unique<phig_op>(phig_op::Kind::Plan);
    op->db = std::make_unique<Database>(ctx->db_path);

    OrganizeOptions core;
    core.destination = opts->destination;
    core.format = opts->format != nullptr ? opts->format : "%Y/%m/%original";
    // By value: the caller's array need only outlive this call, while the
    // engine runs on a worker thread afterwards.
    if (opts->ids != nullptr && opts->id_count > 0) {
        core.ids.assign(opts->ids, opts->ids + opts->id_count);
    }
    core.on_conflict = *policy;
    core.db_path = ctx->db_path;
    core.log_enabled = false;  // planning opens no operation
    validate(core);

    phig_op* raw = op.get();
    spawn_organize(raw, [raw, core]() {
        OrganizeEngine engine(core, *raw->db);
        raw->organize_plan = engine.plan(raw->cancel, &raw->organize_progress);
        return !raw->cancel.stop();
    });
    *out = op.release();
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_op_organize_plan_take(phig_op* op, phig_organize_plan** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (op == nullptr || op->kind != phig_op::Kind::Plan) return PHIG_ERR_INVALID_ARG;
    if (op->state.load(std::memory_order_acquire) == PHIG_OP_RUNNING) return PHIG_ERR_INVALID_ARG;
    try {
        auto plan = std::make_unique<phig_organize_plan>();
        plan->plan = op->organize_plan;  // copy, so the operation may be freed
        *out = plan.release();
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

void phig_organize_plan_free(phig_organize_plan* plan) { delete plan; }

size_t phig_organize_plan_count(const phig_organize_plan* plan) {
    return plan == nullptr ? 0 : plan->plan.size();
}

const char* phig_organize_plan_source_at(const phig_organize_plan* plan, size_t i) {
    if (plan == nullptr || i >= plan->plan.size()) return "";
    return plan->plan.at(i).source.c_str();
}

const char* phig_organize_plan_dest_at(const phig_organize_plan* plan, size_t i) {
    if (plan == nullptr || i >= plan->plan.size()) return "";
    return plan->plan.at(i).dest.c_str();
}

bool phig_organize_plan_conflicts_at(const phig_organize_plan* plan, size_t i) {
    if (plan == nullptr || i >= plan->plan.size()) return false;
    return plan->plan.at(i).conflicts;
}

phig_conflict_resolution phig_organize_plan_resolution_at(const phig_organize_plan* plan,
                                                          size_t i) {
    if (plan == nullptr || i >= plan->plan.size()) return PHIG_CONFLICT_RENAME;
    return to_abi(plan->plan.at(i).resolution);
}

bool phig_organize_plan_occupant_tracked_at(const phig_organize_plan* plan, size_t i) {
    if (plan == nullptr || i >= plan->plan.size()) return false;
    return plan->plan.at(i).occupant_tracked;
}

phig_status phig_organize_plan_set_resolution(phig_organize_plan* plan, size_t i,
                                              phig_conflict_resolution resolution) {
    std::optional<ConflictResolution> r = from_abi(resolution);
    if (plan == nullptr || !r) return PHIG_ERR_INVALID_ARG;
    try {
        return plan->plan.set_resolution(i, *r) ? PHIG_OK : PHIG_ERR_INVALID_ARG;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

phig_status phig_op_copy_start(phig_ctx* ctx, const phig_organize_plan* plan, phig_op** out) {
    return start_apply(ctx, plan, /*is_move=*/false, out);
}

phig_status phig_op_move_start(phig_ctx* ctx, const phig_organize_plan* plan, phig_op** out) {
    return start_apply(ctx, plan, /*is_move=*/true, out);
}

phig_status phig_op_organize_result(phig_op* op, phig_organize_result* out) {
    if (op == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    if (op->kind != phig_op::Kind::Copy && op->kind != phig_op::Kind::Move) {
        return PHIG_ERR_INVALID_ARG;
    }
    phig_op_state s = op->state.load(std::memory_order_acquire);
    if (s == PHIG_OP_RUNNING) return PHIG_ERR_INVALID_ARG;
    if (s == PHIG_OP_FAILED) return PHIG_ERR_INTERNAL;

    out->applied = op->organize_result.applied;
    out->skipped = op->organize_result.skipped;
    out->errors = op->organize_result.errors;
    out->attachment_errors = op->organize_result.attachment_errors;
    out->files_written = op->organize_result.files_written;
    out->cancelled = op->organize_result.cancelled;
    out->completed = op->organize_result.completed;
    return PHIG_OK;
}

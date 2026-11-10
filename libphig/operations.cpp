#include <string>
#include <vector>

#include "internal.h"

namespace {

// Shared preamble for the three _start entry points: validate, build the
// operation, and give it its own database connection.
phig_status make_op(phig_ctx* ctx, phig_op::Kind kind, std::unique_ptr<phig_op>& op) {
    if (ctx == nullptr) return PHIG_ERR_INVALID_ARG;
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
    op = std::make_unique<phig_op>(kind);
    op->db = std::make_unique<Database>(ctx->db_path);
    return PHIG_OK;
}

// Retains the error text of every update still sitting in `q`. Runs once the
// engine has stopped pushing, so that a caller who never polled — or who polled
// too slowly to reach the tail — still ends up with the failure list the header
// promises. Non-consuming: whatever a polling caller has not read yet stays
// readable, and retain_error suppresses the update both producers see.
template <typename T>
void retain_pending(phig_op* op, const ProgressQueue<T>& q) {
    q.for_each([op](const T& p) {
        if (!p.error.empty()) op->retain_error(p.error);
    });
}

// Runs `body` on the operation's thread, translating however it ends into the
// handle's terminal state. The store to `state` is release-ordered so a caller
// that observes a terminal state also sees the result — and `failure` — written
// before it.
template <typename Body>
void spawn(phig_op* op, const Body& body) {
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
        // Only the queue this operation's kind uses ever holds anything; the
        // other two are always empty, so sweeping all three costs nothing.
        retain_pending(op, op->index_progress);
        retain_pending(op, op->thumbs_progress);
        retain_pending(op, op->faces_progress);
        op->index_progress.close();
        op->thumbs_progress.close();
        op->faces_progress.close();
        op->state.store(final_state, std::memory_order_release);
    });
}

// Fills the caller's snapshot from one engine progress record. `path` and
// `error` are staged in the handle so the pointers stay valid until the next
// poll, which is what the header promises.
void fill(phig_op* op, phig_op_progress* out, phig_op_stage stage, size_t done, size_t total,
          size_t processed, size_t skipped, size_t errors, const std::string& path,
          const std::string& error) {
    op->poll_path = path;
    op->poll_error = error;
    out->stage = stage;
    out->done = done;
    out->total = total;
    out->processed = processed;
    out->skipped = skipped;
    out->errors = errors;
    out->path = op->poll_path.c_str();
    out->error = op->poll_error.c_str();
}

}  // namespace

extern "C" {

phig_status phig_op_index_start(phig_ctx* ctx, const phig_index_options* opts, phig_op** out) {
    // Cleared before anything else can fail, because the header promises *out is
    // NULL on every failure — a caller reusing the variable would otherwise be
    // left holding the handle from its previous call.
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (opts == nullptr || opts->directory == nullptr || opts->directory[0] == '\0') {
        return PHIG_ERR_INVALID_ARG;
    }
    PHIG_TRY(ctx)
    std::unique_ptr<phig_op> op;
    phig_status st = make_op(ctx, phig_op::Kind::Index, op);
    if (st != PHIG_OK) return st;

    ScanOptions sopts;
    sopts.directory = opts->directory;
    sopts.recursive = opts->recursive;
    sopts.include_hidden = opts->include_hidden;
    sopts.parallel = opts->parallel;
    sopts.db_path = ctx->db_path;
    if (opts->force) sopts.force.set_all();
    switch (opts->media) {
        case PHIG_MEDIA_IMAGE:
            sopts.media = "image";
            break;
        case PHIG_MEDIA_VIDEO:
            sopts.media = "video";
            break;
        case PHIG_MEDIA_ANY:
            sopts.media = "";
            break;
        default:
            throw InvalidOption("index: unknown media type " +
                                std::to_string(static_cast<int>(opts->media)));
    }
    // Never set: the ABI models indexing, thumbnails, and faces as three
    // independent operations, and these flags would fold all three into one
    // pass. See the note on ScanOptions in core/scan_engine.h.
    sopts.faces = false;
    sopts.thumbs = false;
    validate(sopts);

    phig_op* raw = op.get();
    // run_workers, not run(): the reconcile phase deletes rows for files that
    // have disappeared, polls no cancel token, and is plan 041's concern. The
    // ABI's index operation is the additive half only.
    spawn(raw, [raw, sopts]() {
        ScanEngine engine(sopts, *raw->db);
        raw->index_result = engine.run_workers(raw->cancel, &raw->index_progress);
        return raw->index_result.completed;
    });
    // Released only once the thread exists. If spawn throws, `op` still owns the
    // handle and destroys it rather than leaking one the caller never receives.
    *out = op.release();
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_op_thumbs_start(phig_ctx* ctx, const phig_thumbs_options* opts, phig_op** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (opts == nullptr) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    std::unique_ptr<phig_op> op;
    phig_status st = make_op(ctx, phig_op::Kind::Thumbs, op);
    if (st != PHIG_OK) return st;

    check_id_array(opts->ids, opts->id_count, "thumbs");
    if (opts->size < 0) {
        throw InvalidOption("thumbs: size must be positive, or 0 for the default; got " +
                            std::to_string(opts->size));
    }
    ThumbsOptions topts;
    // By value: the caller's array need only outlive this call, while the
    // engine runs on a worker thread afterwards.
    if (opts->ids != nullptr && opts->id_count > 0) {
        topts.ids.assign(opts->ids, opts->ids + opts->id_count);
    }
    topts.force = opts->force;
    topts.parallel = opts->parallel;
    if (opts->size > 0) topts.size = opts->size;
    validate(topts);

    phig_op* raw = op.get();
    spawn(raw, [raw, topts]() {
        ThumbsEngine engine(topts, *raw->db);
        raw->thumbs_result = engine.run(raw->cancel, &raw->thumbs_progress);
        return raw->thumbs_result.completed;
    });
    *out = op.release();
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_op_faces_start(phig_ctx* ctx, const phig_faces_options* opts, phig_op** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (opts == nullptr) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    std::unique_ptr<phig_op> op;
    phig_status st = make_op(ctx, phig_op::Kind::Faces, op);
    if (st != PHIG_OK) return st;

    check_id_array(opts->ids, opts->id_count, "faces");
    FacesRebuildOptions fopts;
    if (opts->ids != nullptr && opts->id_count > 0) {
        fopts.ids.assign(opts->ids, opts->ids + opts->id_count);
    }
    fopts.parallel = opts->parallel;
    validate(fopts);

    phig_op* raw = op.get();
    spawn(raw, [raw, fopts]() {
        FacesEngine engine(fopts, *raw->db);
        raw->faces_result = engine.run(raw->cancel, &raw->faces_progress);
        return raw->faces_result.completed;
    });
    *out = op.release();
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_op_state phig_op_status(phig_op* op) {
    if (op == nullptr) return PHIG_OP_FAILED;
    return op->state.load(std::memory_order_acquire);
}

bool phig_op_poll(phig_op* op, phig_op_progress* out) {
    if (op == nullptr || out == nullptr) return false;

    switch (op->kind) {
        case phig_op::Kind::Index: {
            ScanProgress p;
            if (!op->index_progress.try_pop(p)) return false;
            phig_op_stage stage = PHIG_STAGE_FILE_DONE;
            if (p.stage == ScanStage::Walked) {
                stage = PHIG_STAGE_WALKED;
            } else if (p.stage == ScanStage::LoadingFaceModels) {
                stage = PHIG_STAGE_LOADING_FACE_MODELS;
            }
            if (!p.error.empty()) op->retain_error(p.error);
            fill(op, out, stage, p.done, p.total, p.processed, p.skipped, p.errors, p.path,
                 p.error);
            return true;
        }
        case phig_op::Kind::Thumbs: {
            ThumbsProgress p;
            if (!op->thumbs_progress.try_pop(p)) return false;
            phig_op_stage stage =
                p.stage == ThumbsStage::Selected ? PHIG_STAGE_SELECTED : PHIG_STAGE_FILE_DONE;
            if (!p.error.empty()) op->retain_error(p.error);
            // A decode that succeeded with a complaint is reported, not counted:
            // `warning` rides the same field a caller renders, while `errors`
            // stays the count of files that actually failed.
            const std::string& text = p.error.empty() ? p.warning : p.error;
            fill(op, out, stage, p.done, p.total, p.generated, p.skipped, p.errors, p.path, text);
            return true;
        }
        case phig_op::Kind::Faces: {
            FacesProgress p;
            if (!op->faces_progress.try_pop(p)) return false;
            phig_op_stage stage = PHIG_STAGE_FILE_DONE;
            if (p.stage == FacesStage::Selected) {
                stage = PHIG_STAGE_SELECTED;
            } else if (p.stage == FacesStage::LoadingFaceModels) {
                stage = PHIG_STAGE_LOADING_FACE_MODELS;
            }
            if (!p.error.empty()) op->retain_error(p.error);
            const std::string& text = p.error.empty() ? p.warning : p.error;
            // Faces reports no processed/skipped split — a selected file is
            // either detected or failed — so those stay 0. See the header.
            fill(op, out, stage, p.done, p.total, 0, 0, p.errors, p.path, text);
            return true;
        }
        case phig_op::Kind::Plan:
        case phig_op::Kind::Copy:
        case phig_op::Kind::Move: {
            OrganizeProgress p;
            if (!op->organize_progress.try_pop(p)) return false;
            phig_op_stage stage =
                p.stage == OrganizeStage::Planned ? PHIG_STAGE_SELECTED : PHIG_STAGE_FILE_DONE;
            if (!p.error.empty()) op->retain_error(p.error);
            // `done`/`total` count parents — the photos the caller selected —
            // while `processed` carries files actually written, including the
            // sidecars and attached JPEGs that travel with a parent (plan 042
            // decision 3). A caller renders "412 of 1,204 photos, 1,238 files".
            fill(op, out, stage, p.done, p.total, p.files_written, p.skipped, p.errors, p.path,
                 p.error);
            return true;
        }
    }
    return false;
}

void phig_op_cancel(phig_op* op) {
    if (op == nullptr) return;
    op->cancel.cancel();
}

void phig_op_wait(phig_op* op) {
    if (op == nullptr) return;
    if (op->worker.joinable()) op->worker.join();
}

phig_status phig_op_index_result(phig_op* op, phig_index_result* out) {
    if (op == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    if (op->kind != phig_op::Kind::Index) return PHIG_ERR_INVALID_ARG;
    phig_op_state st = op->state.load(std::memory_order_acquire);
    if (st == PHIG_OP_RUNNING) return PHIG_ERR_INVALID_ARG;
    if (st == PHIG_OP_FAILED) return PHIG_ERR_INTERNAL;
    const ScanResult& r = op->index_result;
    out->total_files = r.total_files;
    out->processed = r.processed;
    out->skipped = r.skipped;
    out->errors = r.errors;
    out->images = r.images;
    out->videos = r.videos;
    out->completed = r.completed;
    return PHIG_OK;
}

phig_status phig_op_thumbs_result(phig_op* op, phig_thumbs_result* out) {
    if (op == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    if (op->kind != phig_op::Kind::Thumbs) return PHIG_ERR_INVALID_ARG;
    phig_op_state st = op->state.load(std::memory_order_acquire);
    if (st == PHIG_OP_RUNNING) return PHIG_ERR_INVALID_ARG;
    if (st == PHIG_OP_FAILED) return PHIG_ERR_INTERNAL;
    const ThumbsResult& r = op->thumbs_result;
    out->total_selected = r.total_selected;
    out->generated = r.generated;
    out->skipped = r.skipped;
    out->errors = r.errors;
    out->completed = r.completed;
    return PHIG_OK;
}

phig_status phig_op_faces_result(phig_op* op, phig_faces_result* out) {
    if (op == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    if (op->kind != phig_op::Kind::Faces) return PHIG_ERR_INVALID_ARG;
    phig_op_state st = op->state.load(std::memory_order_acquire);
    if (st == PHIG_OP_RUNNING) return PHIG_ERR_INVALID_ARG;
    if (st == PHIG_OP_FAILED) return PHIG_ERR_INTERNAL;
    const FacesResult& r = op->faces_result;
    out->total_selected = r.total_selected;
    out->faces_detected = r.faces_detected;
    out->errors = r.errors;
    out->labels_dropped = r.labels_dropped;
    out->completed = r.completed;
    return PHIG_OK;
}

size_t phig_op_error_count(phig_op* op) {
    if (op == nullptr) return 0;
    std::lock_guard<std::mutex> g(op->errors_mutex);
    return op->retained_errors.size();
}

const char* phig_op_error_at(phig_op* op, size_t i) {
    if (op == nullptr) return "";
    std::lock_guard<std::mutex> g(op->errors_mutex);
    if (i >= op->retained_errors.size()) return "";
    return op->retained_errors[i].c_str();
}

const char* phig_op_error(phig_op* op) {
    if (op == nullptr) return "";
    // The worker writes `failure` before it stores the terminal state, so this
    // acquire load is what makes the string safe to read. Reading it while the
    // operation still runs would race with that write — hence "" until then,
    // which is also the honest answer: nothing has failed yet.
    if (op->state.load(std::memory_order_acquire) == PHIG_OP_RUNNING) return "";
    return op->failure.c_str();
}

void phig_op_free(phig_op* op) {
    if (op == nullptr) return;
    // Joins rather than detaches: the worker holds a raw pointer to this handle
    // and to its Database, so returning while it still runs would leave it
    // writing through freed memory. A caller that does not want to wait calls
    // phig_op_cancel first.
    if (op->worker.joinable()) op->worker.join();
    delete op;
}

}  // extern "C"

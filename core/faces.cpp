#include "faces.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>
#include <stdexcept>

#include "hasher.h"
#include "models.h"

// ---- FaceDetector (per-thread instance) ----

struct FaceDetector::Impl {
    cv::Ptr<cv::FaceDetectorYN> detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    int last_width = 0;
    int last_height = 0;

    Impl() {
        // Before the models check, not after: below the floor the models are
        // beside the point, and telling someone to download them would send
        // them the wrong way. Every faces entry point constructs one of these
        // before it touches a file, so both failures arrive with nothing
        // written.
        if (!PHIG_FACES_OPENCV_OK) {
            std::string required = PHIG_FACES_MIN_OPENCV;
            throw std::runtime_error("Face detection needs OpenCV " + required +
                                     " or newer; this phig was built against " + CV_VERSION +
                                     ".\n"
                                     "The YuNet detection model loads on that version and then "
                                     "fails on the first detection.\n"
                                     "Install OpenCV " +
                                     required + " or newer and rebuild phig.");
        }

        if (!face_models_exist()) {
            throw std::runtime_error(
                "Face recognition models not found.\n"
                "The --faces flag requires face recognition models to be downloaded first.\n"
                "Run 'phig models download' to download them.");
        }

        detector = cv::FaceDetectorYN::create(yunet_model_path(), "", cv::Size(320, 320),
                                              0.5f,  // score threshold
                                              0.3f,  // NMS threshold
                                              5000   // top K
        );

        recognizer = cv::FaceRecognizerSF::create(sface_model_path(), "");
    }

    void ensure_input_size(int width, int height) {
        if (width != last_width || height != last_height) {
            detector->setInputSize(cv::Size(width, height));
            last_width = width;
            last_height = height;
        }
    }
};

FaceDetector::FaceDetector() : impl_(std::make_unique<Impl>()) {}
FaceDetector::~FaceDetector() = default;

std::vector<FaceInfo> FaceDetector::detect(const cv::Mat& img) {
    if (img.empty()) return {};

    // Resize large images for faster detection (faces don't need full resolution)
    const int MAX_DIM = 1920;
    cv::Mat detect_img = img;
    float scale = 1.0f;
    if (img.cols > MAX_DIM || img.rows > MAX_DIM) {
        scale = static_cast<float>(MAX_DIM) / std::max(img.cols, img.rows);
        cv::resize(img, detect_img, cv::Size(), scale, scale);
    }

    impl_->ensure_input_size(detect_img.cols, detect_img.rows);

    // Detect faces on (possibly resized) image
    cv::Mat faces_mat;
    impl_->detector->detect(detect_img, faces_mat);
    if (faces_mat.empty()) return {};

    // If we resized, scale face coordinates back to original and use
    // original image for embedding (better quality)
    cv::Mat embed_faces = faces_mat.clone();
    if (scale != 1.0f) {
        float inv_scale = 1.0f / scale;
        for (int i = 0; i < embed_faces.rows; i++) {
            // Scale bounding box (x, y, w, h) and landmarks
            for (int j = 0; j < 14; j++) {
                embed_faces.at<float>(i, j) *= inv_scale;
            }
            // Score (column 14) stays as-is
        }
    }

    std::vector<FaceInfo> results;
    for (int i = 0; i < embed_faces.rows; i++) {
        // Align and compute embedding from original resolution
        cv::Mat aligned;
        impl_->recognizer->alignCrop(img, embed_faces.row(i), aligned);

        cv::Mat feature;
        impl_->recognizer->feature(aligned, feature);

        FaceInfo fi;
        const float* fdata = feature.ptr<float>(0);
        for (int j = 0; j < 128; j++) {
            fi.embedding[j] = fdata[j];
        }

        fi.x = static_cast<int>(embed_faces.at<float>(i, 0));
        fi.y = static_cast<int>(embed_faces.at<float>(i, 1));
        fi.width = static_cast<int>(embed_faces.at<float>(i, 2));
        fi.height = static_cast<int>(embed_faces.at<float>(i, 3));

        results.push_back(fi);
    }

    return results;
}

std::vector<FaceInfo> FaceDetector::detect(const std::string& image_path) {
    cv::Mat img = decode_image(image_path);
    if (img.empty()) return {};
    return detect(img);
}

// ---- Global convenience functions ----

namespace {

FaceDetector& get_global_detector() {
    static FaceDetector detector;
    return detector;
}

std::once_flag global_init_flag;

}  // anonymous namespace

void preload_face_models() {
    std::call_once(global_init_flag, []() { get_global_detector(); });
}

std::vector<FaceInfo> detect_faces(const std::string& image_path) {
    preload_face_models();
    return get_global_detector().detect(image_path);
}

std::vector<FaceInfo> detect_faces(const cv::Mat& cv_img) {
    preload_face_models();
    return get_global_detector().detect(cv_img);
}

float embedding_distance(const std::array<float, 128>& a, const std::array<float, 128>& b) {
    float sum = 0;
    for (int i = 0; i < 128; i++) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

float box_overlap(const FaceInfo& a, const FaceInfo& b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) return 0.0f;
    // 64-bit so a box near INT_MAX cannot overflow its far edge.
    int64_t ix = std::min<int64_t>(int64_t{a.x} + a.width, int64_t{b.x} + b.width) -
                 std::max<int64_t>(a.x, b.x);
    int64_t iy = std::min<int64_t>(int64_t{a.y} + a.height, int64_t{b.y} + b.height) -
                 std::max<int64_t>(a.y, b.y);
    if (ix <= 0 || iy <= 0) return 0.0f;
    double inter = static_cast<double>(ix) * static_cast<double>(iy);
    double area_a = static_cast<double>(a.width) * a.height;
    double area_b = static_cast<double>(b.width) * b.height;
    return static_cast<float>(inter / (area_a + area_b - inter));
}

LabelCarry carry_labels(const std::vector<LabelledFace>& labelled,
                        const std::vector<FaceInfo>& detected, float min_overlap) {
    LabelCarry out;
    out.person_ids.assign(detected.size(), -1);

    struct Pair {
        float overlap;
        size_t old_i;
        size_t new_i;
    };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < labelled.size(); ++i) {
        for (size_t j = 0; j < detected.size(); ++j) {
            float o = box_overlap(labelled[i].face, detected[j]);
            if (o >= min_overlap) pairs.push_back({o, i, j});
        }
    }
    // Stable, so equal overlaps resolve in index order and a run is repeatable.
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const Pair& x, const Pair& y) { return x.overlap > y.overlap; });

    std::vector<bool> old_taken(labelled.size(), false);
    std::vector<bool> new_taken(detected.size(), false);
    for (const Pair& p : pairs) {
        if (old_taken[p.old_i] || new_taken[p.new_i]) continue;
        old_taken[p.old_i] = true;
        new_taken[p.new_i] = true;
        out.person_ids[p.new_i] = labelled[p.old_i].person_id;
    }
    for (bool taken : old_taken) {
        if (!taken) ++out.dropped;
    }
    return out;
}

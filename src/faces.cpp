#include "faces.h"
#include "models.h"

#include <opencv2/objdetect/face.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <stdexcept>
#include <mutex>

// ---- FaceDetector (per-thread instance) ----

struct FaceDetector::Impl {
    cv::Ptr<cv::FaceDetectorYN> detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    int last_width = 0;
    int last_height = 0;

    Impl() {
        if (!face_models_exist()) {
            throw std::runtime_error(
                "Face recognition models not found.\n"
                "The --faces flag requires face recognition models to be downloaded first.\n"
                "Run 'phig models download' to download them.");
        }

        detector = cv::FaceDetectorYN::create(
            yunet_model_path(), "", cv::Size(320, 320),
            0.5f,   // score threshold
            0.3f,   // NMS threshold
            5000    // top K
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

        results.push_back(std::move(fi));
    }

    return results;
}

std::vector<FaceInfo> FaceDetector::detect(const std::string& image_path) {
    cv::Mat img = cv::imread(image_path);
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

} // anonymous namespace

void preload_face_models() {
    std::call_once(global_init_flag, []() {
        get_global_detector();
    });
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

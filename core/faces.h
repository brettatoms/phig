#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/version.hpp>
#include <string>
#include <vector>

// The face path needs a newer OpenCV than the rest of phig does. YuNet
// 2023mar — the detection model `phig models download` fetches — has an ONNX
// graph that older importers accept and then cannot run: FaceDetectorYN::create
// succeeds and the first detect() throws "(-204:Requested object was not found)
// Layer with requested id=-1 not found in function 'getLayerData'". Measured
// against that model file: 4.6.0 and 4.7.0 both throw, 4.8.0 and 4.10.0 both run
// it clean. FaceRecognizerSF::create works on all four, so the detector is what
// sets the floor.
//
// Compile-time, because the OpenCV phig links against is fixed when phig is
// built: a runtime probe would re-answer a question that cannot change.
inline constexpr const char* PHIG_FACES_MIN_OPENCV = "4.8.0";
inline constexpr bool PHIG_FACES_OPENCV_OK =
    CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 8);

struct FaceInfo {
    std::array<float, 128> embedding;
    int x, y, width, height;  // bounding box
};

// A stored face that carries a person label, as a rebuild reads it before
// replacing the image's faces.
struct LabelledFace {
    FaceInfo face;
    int64_t person_id;
};

// The labels a rebuild carries onto an image's new detections. `person_ids` is
// parallel to the detections: the label to write, or -1 for none. `dropped`
// counts labelled faces that matched no detection.
struct LabelCarry {
    std::vector<int64_t> person_ids;
    size_t dropped = 0;
};

// Minimum box overlap (intersection over union) for a new detection to take an
// old face's label. Re-detecting 564 stored faces with the same model over the
// same pixels reproduced every box exactly, so this value was not exercised by
// that measurement; see docs/plans/2026-11-07-060-r60-label-carry.md.
inline constexpr float kLabelCarryMinOverlap = 0.5f;

// Per-thread face detector using OpenCV DNN (YuNet + SFace)
class FaceDetector {
   public:
    FaceDetector();
    ~FaceDetector();

    FaceDetector(const FaceDetector&) = delete;
    FaceDetector& operator=(const FaceDetector&) = delete;

    // Detect faces from already-decoded OpenCV image
    std::vector<FaceInfo> detect(const cv::Mat& cv_img);

    // Detect faces from file path
    std::vector<FaceInfo> detect(const std::string& image_path);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience functions using a shared global detector (single-threaded use)
std::vector<FaceInfo> detect_faces(const std::string& image_path);
std::vector<FaceInfo> detect_faces(const cv::Mat& cv_img);

// Ensure global models are loaded
void preload_face_models();

// Compute Euclidean distance between two embeddings
float embedding_distance(const std::array<float, 128>& a, const std::array<float, 128>& b);

// Intersection over union of two face boxes, in [0, 1]. Zero when either box
// has no area.
float box_overlap(const FaceInfo& a, const FaceInfo& b);

// Matches an image's labelled faces to that image's new detections by box
// overlap, greedily from the highest overlap down and one-to-one, accepting a
// pair only at or above `min_overlap`. Box overlap is the only test: with the
// same model and pixels the boxes reproduce exactly (measured; embeddings were
// not), and with a different model embeddings are not comparable, so an
// embedding check would drop every label in a model-change rebuild.
LabelCarry carry_labels(const std::vector<LabelledFace>& labelled,
                        const std::vector<FaceInfo>& detected,
                        float min_overlap = kLabelCarryMinOverlap);

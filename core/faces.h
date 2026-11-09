#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/version.hpp>
#include <string>
#include <vector>

#include "decode_result.h"
#include "types.h"

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

// Where a face's box and embedding came from. Empty strings and zero sizes
// mean unknown, as on rows written before schema v10.
struct FaceProvenance {
    std::string frame_source;  // decode_source_name(), or empty
    int frame_width = 0, frame_height = 0;
    std::string frame_sha256;  // bytes decoded; empty if unknown
    std::string model_sha256;  // recognition model file; empty if unknown
};

// The provenance to record for faces detected in `frame` of an image whose own
// file has sha256 `image_sha256`, under the recognition model whose file hash is
// `model_sha256`. A frame with no hash of its own is the image file as stored, so
// it takes the image's hash.
FaceProvenance face_provenance(const DecodeFrame& frame, const std::string& image_sha256,
                               const std::string& model_sha256);

// The row's sha256 describes the file only while its size and mtime still match the row,
// the same trust a scan's skip uses; otherwise "" so the frame matches no other frame.
std::string trusted_row_sha(const ImageInfo& img);

// A stored face that carries a person label, as a rebuild reads it before
// replacing the image's faces.
struct LabelledFace {
    FaceInfo face;
    int64_t person_id;
    FaceProvenance provenance;
};

// The labels a rebuild carries onto an image's new detections. `person_ids` is
// parallel to the detections: the label to write, or -1 for none. `dropped`
// counts labelled faces that matched no detection.
struct LabelCarry {
    std::vector<int64_t> person_ids;
    size_t dropped = 0;
};

// Minimum box overlap (intersection over union) for a new detection to take an
// old face's label, used only when the old face and the new detection come from
// an identical decoded frame under different recognition models.
inline constexpr float kLabelCarryMinOverlap = 0.5f;

// Largest L2 distance between unit-normalized embeddings at which two faces are
// taken to be the same person when a label is carried across a rebuild.
inline constexpr float kLabelCarryMaxDistance = 1.128f;

// sha256 of the recognition model file, read afresh on every call (the file can
// be replaced while a process runs). Call it once per run, outside any lock.
// Throws if the file cannot be read, so call it only once the models exist.
std::string recognition_model_sha256();

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

// L2 distance between the unit-normalized embeddings, in [0, 2]. A zero vector
// has no direction and is at distance 2 from everything.
float normalized_embedding_distance(const std::array<float, 128>& a,
                                    const std::array<float, 128>& b);

// True when both describe the same decoded bytes: a known hash and equal size.
bool same_frame(const FaceProvenance& a, const FaceProvenance& b);

// Matches an image's labelled faces to that image's new detections, one-to-one
// and greedily. A labelled face stored under the model `current` names (an
// empty stored model is taken as the SFace model phig first shipped) matches
// detections by normalized embedding distance within `max_distance`, whatever
// the boxes. Under a different model embeddings are not comparable, so it
// matches by box overlap at or above `min_overlap`, and only when its stored
// frame is the same frame as `current`'s; otherwise it matches nothing and
// is counted in `dropped`. Embedding pairs are assigned first, nearest first,
// then overlap pairs, largest first.
LabelCarry carry_labels(const std::vector<LabelledFace>& labelled,
                        const std::vector<FaceInfo>& detected, const FaceProvenance& current,
                        float max_distance = kLabelCarryMaxDistance,
                        float min_overlap = kLabelCarryMinOverlap);

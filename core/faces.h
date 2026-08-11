#pragma once

#include <array>
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

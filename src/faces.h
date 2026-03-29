#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <opencv2/core.hpp>

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

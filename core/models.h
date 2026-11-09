#pragma once

#include <string>

// sha256 of face_recognition_sface_2021dec.onnx, the recognition model.
inline constexpr const char* kSfaceModelSha256 =
    "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79";

// Returns true if face recognition models are downloaded
bool face_models_exist();

// Download face recognition models. Returns true on success.
bool download_face_models();

// Paths to specific model files
std::string yunet_model_path();
std::string sface_model_path();

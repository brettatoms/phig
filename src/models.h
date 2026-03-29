#pragma once

#include <string>

// Returns the directory where models are stored (~/.local/share/phig/models/)
std::string models_dir();

// Returns true if face recognition models are downloaded
bool face_models_exist();

// Download face recognition models. Returns true on success.
bool download_face_models();

// Paths to specific model files
std::string yunet_model_path();
std::string sface_model_path();

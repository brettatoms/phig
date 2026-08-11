#pragma once

#include <string>

#include "faces.h"
#include "models.h"

// Why the face path cannot run here, or "" when it can. Two conditions, in the
// order FaceDetector checks them: below the OpenCV floor the models are beside
// the point, so the version reason wins. Shared by the faces-engine and
// scan-engine suites so the two cannot drift apart on what "available" means.
inline std::string face_path_blocker() {
    if (!PHIG_FACES_OPENCV_OK) {
        return std::string("OpenCV ") + CV_VERSION + " is below " + PHIG_FACES_MIN_OPENCV +
               ", which the face path needs";
    }
    if (!face_models_exist()) {
        return "face models not found at " + yunet_model_path();
    }
    return "";
}

#include "models.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <curl/curl.h>

namespace fs = std::filesystem;

namespace {

size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* stream = static_cast<std::ofstream*>(userdata);
    stream->write(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

bool download_file(const std::string& url, const std::string& dest) {
    std::cout << "Downloading: " << url << "\n";
    std::cout << "         to: " << dest << "\n";

    std::ofstream file(dest, std::ios::binary);
    if (!file) {
        std::cerr << "Error: cannot create file: " << dest << "\n";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Error: failed to initialize curl\n";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    file.close();

    if (res != CURLE_OK || http_code != 200) {
        std::cerr << "Error: download failed (HTTP " << http_code << ")\n";
        fs::remove(dest);
        return false;
    }

    std::cout << "Done.\n\n";
    return true;
}

} // anonymous namespace

std::string models_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            throw std::runtime_error("Cannot determine home directory");
        }
        base = std::string(home) + "/.local/share";
    }
    return base + "/phig/models";
}

std::string yunet_model_path() {
    return models_dir() + "/face_detection_yunet_2023mar.onnx";
}

std::string sface_model_path() {
    return models_dir() + "/face_recognition_sface_2021dec.onnx";
}

bool face_models_exist() {
    return fs::exists(yunet_model_path()) && fs::exists(sface_model_path());
}

bool download_face_models() {
    std::string dir = models_dir();
    fs::create_directories(dir);

    struct ModelFile {
        std::string url;
        std::string filename;
    };

    std::vector<ModelFile> models = {
        {"https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx",
         "face_detection_yunet_2023mar.onnx"},
        {"https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx",
         "face_recognition_sface_2021dec.onnx"},
    };

    for (const auto& model : models) {
        std::string dest = dir + "/" + model.filename;
        if (fs::exists(dest)) {
            std::cout << "Already exists: " << dest << "\n\n";
            continue;
        }

        if (!download_file(model.url, dest)) {
            return false;
        }
    }

    return true;
}

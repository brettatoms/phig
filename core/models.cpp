#include "models.h"

#include <curl/curl.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "hasher.h"
#include "paths.h"

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

    // Download to a temp path and rename into place only on full success, so an
    // interrupted transfer never leaves a partial model at `dest` that
    // face_models_exist() would report as present.
    std::string tmp = dest + ".phig-tmp";
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "Error: cannot create file: " << tmp << "\n";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Error: failed to initialize curl\n";
        fs::remove(tmp);
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
    file.flush();
    bool write_ok = file.good();
    file.close();

    if (res != CURLE_OK || http_code != 200 || !write_ok) {
        std::cerr << "Error: download failed (HTTP " << http_code << ")\n";
        fs::remove(tmp);
        return false;
    }

    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::cerr << "Error: could not finalize download: " << ec.message() << "\n";
        fs::remove(tmp);
        return false;
    }

    std::cout << "Done.\n\n";
    return true;
}

}  // anonymous namespace

std::string yunet_model_path() { return models_dir() + "/face_detection_yunet_2023mar.onnx"; }

std::string sface_model_path() { return models_dir() + "/face_recognition_sface_2021dec.onnx"; }

bool face_models_exist() {
    return fs::exists(yunet_model_path()) && fs::exists(sface_model_path());
}

bool download_face_models() {
    std::string dir = models_dir();
    fs::create_directories(dir);

    // The hash sits beside the URL because opencv_zoo serves `main`, not a tag:
    // a file replaced upstream under the same name would change what phig
    // detects with nothing in any diff to say so. It also covers a file that is
    // present but wrong — truncated, half-copied, or restored from a poisoned
    // cache — which existence alone cannot tell from a good one.
    struct ModelFile {
        std::string url;
        std::string filename;
        std::string sha256;
    };

    std::vector<ModelFile> models = {
        {"https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/"
         "face_detection_yunet_2023mar.onnx",
         "face_detection_yunet_2023mar.onnx",
         "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"},
        {"https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/"
         "face_recognition_sface_2021dec.onnx",
         "face_recognition_sface_2021dec.onnx",
         "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79"},
    };

    for (const auto& model : models) {
        std::string dest = dir + "/" + model.filename;
        bool replacing_mismatch = false;
        if (fs::exists(dest)) {
            if (compute_sha256(dest) == model.sha256) {
                std::cout << "Already exists: " << dest << "\n\n";
                continue;
            }
            // Re-download rather than refuse: making the models present and
            // correct is the whole job of this command, and a wrong file is
            // not something the person who typed it can be expected to fix by
            // hand. If the fresh copy is wrong too, that is upstream having
            // moved, and the check below says so instead of looping.
            std::cout << "Checksum mismatch, re-downloading: " << dest << "\n";
            replacing_mismatch = true;
        }

        if (!download_file(model.url, dest)) {
            if (replacing_mismatch) {
                // download_file removes only its own temporary, so the file
                // that failed the hash is still at dest and face_models_exist()
                // still reports it. Kept on purpose: a mismatch is as likely to
                // mean phig's pin has gone stale against a model opencv_zoo
                // replaced as it is to mean the file is damaged, and deleting a
                // working model because the network was down is the worse of
                // the two mistakes. Said out loud rather than left implicit,
                // because the next faces run will load it.
                std::cerr << "Warning: keeping " << dest
                          << ", which does not match the expected sha256\n";
            }
            return false;
        }

        std::string got = compute_sha256(dest);
        if (got != model.sha256) {
            std::cerr << "Error: " << model.filename << " sha256 is " << got << ", expected "
                      << model.sha256 << "\n";
            // This one is removed: it arrived just now and was never a working
            // model, so nothing is lost, and leaving it would let
            // face_models_exist() report a file phig has just refused.
            fs::remove(dest);
            return false;
        }
    }

    return true;
}

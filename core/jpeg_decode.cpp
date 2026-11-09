#include "jpeg_decode.h"

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <span>
#include <string>
#include <utility>

extern "C" {
#include <jpeglib.h>
}

extern "C" {
#include <jerror.h>
}

namespace {

// OpenCV's validateInputImageSize default; a larger image makes imread throw.
constexpr uint64_t kMaxPixels = uint64_t{1} << 30;

struct ErrorManager {
    jpeg_error_mgr pub;
    std::jmp_buf jump;
    std::string* messages;
};

// Called from inside libjpeg, so nothing may propagate out of it; a message
// that cannot be stored is dropped.
void append_message(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    try {
        if (!err->messages->empty()) *err->messages += "; ";
        *err->messages += buf;
    } catch (...) {
    }
}

// libjpeg's default emit_message stays in place, so this receives exactly what
// OpenCV's decoder printed to stderr: the first warning only.
void output_message(j_common_ptr cinfo) { append_message(cinfo); }

[[noreturn]] void error_exit(j_common_ptr cinfo) {
    append_message(cinfo);
    std::longjmp(reinterpret_cast<ErrorManager*>(cinfo->err)->jump, 1);
}

// cv::imdecode's source: it suspends instead of inventing an EOI marker when
// the bytes run out, so a short buffer fails rather than decoding partially.
void source_stub(j_decompress_ptr) {}

boolean source_fill(j_decompress_ptr) { return FALSE; }

void source_skip(j_decompress_ptr cinfo, long num_bytes) {
    jpeg_source_mgr* src = cinfo->src;
    if (num_bytes > static_cast<long>(src->bytes_in_buffer)) {
        src->next_input_byte += src->bytes_in_buffer;
        src->bytes_in_buffer = 0;
    } else {
        src->bytes_in_buffer -= num_bytes;
        src->next_input_byte += num_bytes;
    }
}

// OpenCV's ExifReader, reduced to what decides the orientation it applies. Its
// IFD0 walk aborts at the first out-of-range read, keeping the tags parsed so
// far, and the tags it parses ahead of 0x0112 can abort it, so those reads are
// repeated here with the same bounds and the same integer widths.
class ExifIfd0 {
   public:
    struct OutOfRange : std::exception {};

    ExifIfd0(const unsigned char* data, size_t size) : d_(data), n_(size) {
        if (n_ > 1 && d_[0] != d_[1]) {
            intel_ = false;
        } else {
            intel_ = d_[0] == 'I';
        }
    }

    // Adds each IFD0 tag not already in seen, with the u16 at its value field
    // for the orientation tag. Stops at the first read OpenCV would reject.
    void parse(std::map<uint16_t, uint16_t>& seen) const {
        try {
            if (u16(2) != 0x2A) return;
            uint32_t offset = u32(4);
            size_t count = u16(offset);
            offset += 2;
            for (size_t i = 0; i < count; ++i) {
                uint16_t tag = entry(offset);
                seen.emplace(tag, tag == 0x0112 ? u16(size_t{offset} + 8) : 0);
                offset += 12;
            }
        } catch (const OutOfRange&) {
        }
    }

   private:
    uint16_t u16(size_t o) const {
        if (o + 1 >= n_) throw OutOfRange{};
        return intel_ ? static_cast<uint16_t>(d_[o] | (d_[o + 1] << 8))
                      : static_cast<uint16_t>((d_[o] << 8) | d_[o + 1]);
    }

    uint32_t u32(size_t o) const {
        if (o + 3 >= n_) throw OutOfRange{};
        const uint32_t b0 = d_[o], b1 = d_[o + 1], b2 = d_[o + 2], b3 = d_[o + 3];
        return intel_ ? b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
                      : (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }

    void rational(size_t o) const {
        u32(o);
        u32(o + 4);
    }

    // Rational arrays advance a uint32_t offset, which wraps as OpenCV's does.
    void rationals(size_t entry_offset, int count) const {
        uint32_t o = u32(entry_offset + 8);
        for (int i = 0; i < count; ++i, o += 8) rational(o);
    }

    // The tag OpenCV records for the entry at offset, after the reads its
    // parser makes for that tag. Unrecognised tags record as 0xFFFF.
    uint16_t entry(size_t offset) const {
        const uint16_t tag = u16(offset);
        switch (tag) {
            case 0x010E:    // image description
            case 0x010F:    // make
            case 0x0110:    // model
            case 0x0131:    // software
            case 0x0132:    // date time
            case 0x8298: {  // copyright
                size_t size = u32(offset + 4);
                size_t data_offset = 8;
                if (size > 4) data_offset = u32(offset + 8);
                if (data_offset > n_ || data_offset + size > n_) throw OutOfRange{};
                return tag;
            }
            case 0x0112:  // orientation
            case 0x0128:  // resolution unit
            case 0x0213:  // YCbCr positioning
                u16(offset + 8);
                return tag;
            case 0x011A:  // x resolution
            case 0x011B:  // y resolution
                rational(u32(offset + 8));
                return tag;
            case 0x013E: {  // white point
                uint32_t o = u32(offset + 8);
                rational(o);
                rational(static_cast<uint32_t>(o + 8));
                return tag;
            }
            case 0x013F:  // primary chromaticities
            case 0x0214:  // reference black/white
                rationals(offset, 6);
                return tag;
            case 0x0211:  // YCbCr coefficients
                rationals(offset, 3);
                return tag;
            case 0x8769:  // Exif sub-IFD offset
                return tag;
            default:
                return 0xFFFF;
        }
    }

    const unsigned char* d_;
    size_t n_;
    bool intel_;
};

// The orientation the linked OpenCV reads, 0 when there is none. OpenCV 5
// takes the first 0x0112 tag across every APP1 marker that starts with
// "Exif\0\0", in file order. OpenCV 4 parses only the first APP1 marker, past
// its first 6 bytes, whatever it holds, so an XMP marker ahead of the Exif one
// hides the orientation. The rule follows the runtime version because stored
// hashes must match cv::imread on the machine that decodes.
int exif_orientation(const jpeg_decompress_struct& cinfo) {
    const bool first_app1_only = cv::getVersionMajor() < 5;
    std::map<uint16_t, uint16_t> seen;
    for (jpeg_saved_marker_ptr m = cinfo.marker_list; m != nullptr; m = m->next) {
        if (m->marker != JPEG_APP0 + 1) continue;
        if (first_app1_only) {
            if (m->data_length > 6) ExifIfd0(m->data + 6, m->data_length - 6).parse(seen);
            break;
        }
        if (m->data_length <= 6 || std::memcmp(m->data, "Exif\0\0", 6) != 0) continue;
        ExifIfd0(m->data + 6, m->data_length - 6).parse(seen);
    }
    auto it = seen.find(0x0112);
    return it == seen.end() ? 0 : it->second;
}

// OpenCV's ExifTransform: the same calls in the same order.
void apply_orientation(cv::Mat& img, int orientation) {
    switch (orientation) {
        case 2:
            cv::flip(img, img, 1);
            break;
        case 3:
            cv::flip(img, img, -1);
            break;
        case 4:
            cv::flip(img, img, 0);
            break;
        case 5:
            cv::transpose(img, img);
            break;
        case 6:
            cv::transpose(img, img);
            cv::flip(img, img, 1);
            break;
        case 7:
            cv::transpose(img, img);
            cv::flip(img, img, -1);
            break;
        case 8:
            cv::transpose(img, img);
            cv::flip(img, img, 0);
            break;
        default:
            break;
    }
}

// OpenCV's icvCvt_CMYK2BGR_8u_C4C3R, for libjpeg's inverted Adobe CMYK output.
void cmyk_to_bgr(const unsigned char* cmyk, unsigned char* bgr, int width) {
    for (int i = 0; i < width; ++i, cmyk += 4, bgr += 3) {
        int c = cmyk[0], m = cmyk[1], y = cmyk[2], k = cmyk[3];
        c = k - ((255 - c) * k >> 8);
        m = k - ((255 - m) * k >> 8);
        y = k - ((255 - y) * k >> 8);
        bgr[2] = static_cast<unsigned char>(c);
        bgr[1] = static_cast<unsigned char>(m);
        bgr[0] = static_cast<unsigned char>(y);
    }
}

// State shared across the setjmp guard. Every libjpeg call that can longjmp
// runs in a function holding no object with a destructor, so the jump skips
// nothing that needs one; everything that does lives here.
struct Decoder {
    std::FILE* file = nullptr;    // decodes from bytes when null
    bool file_semantics = false;  // bytes that end early get an EOI, as a file's do
    unsigned char eoi[2] = {0xFF, JPEG_EOI};
    std::span<const unsigned char> bytes;
    jpeg_decompress_struct cinfo{};
    ErrorManager err{};
    jpeg_source_mgr memory_source{};
    std::string warning;
    cv::Mat img;
    int orientation = 0;
    bool ended_early = false;  // the source ran out before the last scanline
};

// libjpeg's stdio source at end of file: warn, then supply an EOI so the decode
// finishes with what it has. The buffer lives in the Decoder, whose address is
// in cinfo.client_data.
boolean source_fill_eof(j_decompress_ptr cinfo) {
    auto* d = static_cast<Decoder*>(cinfo->client_data);
    WARNMS(cinfo, JWRN_JPEG_EOF);
    cinfo->src->next_input_byte = d->eoi;
    cinfo->src->bytes_in_buffer = 2;
    return TRUE;
}

void source_skip_eof(j_decompress_ptr cinfo, long num_bytes) {
    jpeg_source_mgr* src = cinfo->src;
    if (num_bytes <= 0) return;
    while (num_bytes > static_cast<long>(src->bytes_in_buffer)) {
        num_bytes -= static_cast<long>(src->bytes_in_buffer);
        src->fill_input_buffer(cinfo);
    }
    src->next_input_byte += num_bytes;
    src->bytes_in_buffer -= num_bytes;
}

// The source imread uses for a file, or the one imdecode uses for bytes.
void attach_source(Decoder& d) {
    if (d.file != nullptr) {
        jpeg_stdio_src(&d.cinfo, d.file);
        return;
    }
    d.memory_source.init_source = source_stub;
    d.memory_source.fill_input_buffer = d.file_semantics ? source_fill_eof : source_fill;
    d.memory_source.skip_input_data = d.file_semantics ? source_skip_eof : source_skip;
    d.memory_source.resync_to_restart = jpeg_resync_to_restart;
    d.memory_source.term_source = source_stub;
    d.memory_source.next_input_byte = d.bytes.data();
    d.memory_source.bytes_in_buffer = d.bytes.size();
    d.cinfo.src = &d.memory_source;
}

// OpenCV's JpegDecoder::readHeader.
bool read_header(Decoder& d) {
    jpeg_create_decompress(&d.cinfo);
    attach_source(d);
    jpeg_save_markers(&d.cinfo, JPEG_APP0 + 1, 0xffff);
    jpeg_read_header(&d.cinfo, TRUE);
    d.orientation = exif_orientation(d.cinfo);
    d.cinfo.scale_num = 1;
    d.cinfo.scale_denom = 1;
    jpeg_calc_output_dimensions(&d.cinfo);
    return d.cinfo.output_width != 0 && d.cinfo.output_height != 0;
}

// OpenCV's JpegDecoder::readData for a 3-channel destination, up to (not
// including) jpeg_finish_decompress. False where readData returns false.
bool read_pixels(Decoder& d) {
    jpeg_decompress_struct& cinfo = d.cinfo;
    const bool cmyk = cinfo.num_components == 4;
    cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_BGR;
    cinfo.out_color_components = cmyk ? 4 : 3;
    jpeg_start_decompress(&cinfo);
    const int width = static_cast<int>(d.img.cols);
    if (!cmyk) {
        for (int y = 0; y < d.img.rows; ++y) {
            JSAMPROW row = d.img.ptr<unsigned char>(y);
            if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
                d.ended_early = true;
                return false;
            }
        }
        return true;
    }
    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&cinfo),
                                                   JPOOL_IMAGE, width * 4, 1);
    for (int y = 0; y < d.img.rows; ++y) {
        if (jpeg_read_scanlines(&cinfo, buffer, 1) != 1) {
            d.ended_early = true;
            return false;
        }
        cmyk_to_bgr(buffer[0], d.img.ptr<unsigned char>(y), width);
    }
    return true;
}

bool guarded_header(Decoder& d) {
    if (setjmp(d.err.jump) != 0) return false;
    return read_header(d);
}

bool guarded_pixels(Decoder& d) {
    if (setjmp(d.err.jump) != 0) return false;
    return read_pixels(d);
}

// readData sets its result before jpeg_finish_decompress, so an error there
// keeps the image.
void guarded_finish(Decoder& d) {
    if (setjmp(d.err.jump) != 0) return;
    jpeg_finish_decompress(&d.cinfo);
}

DecodeResult run_unchecked(Decoder& d) {
    DecodeResult result;
    bool ok = guarded_header(d);
    if (ok) {
        const uint64_t pixels = uint64_t{d.cinfo.output_width} * d.cinfo.output_height;
        if (pixels > kMaxPixels) {
            if (!d.warning.empty()) d.warning += "; ";
            d.warning += "image too large to decode";
            ok = false;
        }
    }
    if (ok) {
        d.img.create(static_cast<int>(d.cinfo.output_height),
                     static_cast<int>(d.cinfo.output_width), CV_8UC3);
        ok = guarded_pixels(d);
        // A suspending source stops without a libjpeg message, so give one.
        if (d.ended_early) {
            if (!d.warning.empty()) d.warning += "; ";
            d.warning += "JPEG data ends before the image does";
        }
        if (ok) guarded_finish(d);
    }
    jpeg_destroy_decompress(&d.cinfo);
    if (ok) {
        apply_orientation(d.img, d.orientation);
        result.img = d.img;
        result.frame.width = result.img.cols;
        result.frame.height = result.img.rows;
    }
    result.warning = std::move(d.warning);
    return result;
}

// Decodes, destroying the decompressor whatever happens. An exception from
// OpenCV or an allocation becomes an empty image and a message.
DecodeResult run(Decoder& d) {
    d.cinfo.err = jpeg_std_error(&d.err.pub);
    d.cinfo.client_data = &d;
    d.err.pub.error_exit = error_exit;
    d.err.pub.output_message = output_message;
    d.err.messages = &d.warning;
    try {
        return run_unchecked(d);
    } catch (const std::exception& e) {
        jpeg_destroy_decompress(&d.cinfo);
        DecodeResult result;
        result.warning = std::move(d.warning);
        if (!result.warning.empty()) result.warning += "; ";
        result.warning += e.what();
        return result;
    }
}

}  // namespace

bool is_jpeg_signature(std::span<const unsigned char> head) {
    return head.size() >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
}

DecodeResult decode_jpeg_memory(std::span<const unsigned char> bytes) {
    Decoder d;
    d.bytes = bytes;
    return run(d);
}

DecodeResult decode_jpeg_file_bytes(std::span<const unsigned char> bytes) {
    Decoder d;
    d.bytes = bytes;
    d.file_semantics = true;
    return run(d);
}

DecodeResult decode_jpeg_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return DecodeResult{{}, "cannot open " + path, {}};
    Decoder d;
    d.file = f;
    DecodeResult result = run(d);
    std::fclose(f);
    return result;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "pdf.h"
extern "C" {
#include "base.h"
#include "fs.h"
}
#include <PDFDoc.h>
#include <GlobalParams.h>
#include <GfxState.h>
#include <Stream.h>
#include <TextOutputDev.h>
#include <SplashOutputDev.h>
#include <splash/SplashBitmap.h>
#include <png.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unistd.h>

namespace {
struct Input {
    int fd = -1, stopped = 0;
    int64_t size = 0;
    uint64_t started = 0, reads = 0;
    int (*pump)(void *, unsigned int) = nullptr;
    void *opaque = nullptr;
    struct snag_buf *text = nullptr;
    ~Input() { if (fd >= 0) close(fd); }
    bool stop() {
        if (!stopped && pump) stopped = pump(opaque, 0u);
        if (!stopped && snag_monotonic_ms() - started >= 60000u) stopped = -1;
        return stopped != 0;
    }
};
class Snapshot final : public BaseSeekInputStream {
    std::shared_ptr<Input> input;
    Goffset position = 0;
    Goffset currentPos() const override { return position; }
    void setCurrentPos(Goffset offset) override {
        if (offset < 0 || offset > input->size) { input->stopped = -1; return; }
        position = offset;
    }
    Goffset read(char *buffer, Goffset count) override {
        if (input->stop() || count < 0) return 0;
        count = std::min(count, input->size - position);
        if (static_cast<uint64_t>(count) > 1024u * 1024u * 1024u - input->reads) {
            input->stopped = -1; return 0;
        }
        ssize_t n;
        do { n = snag_pread(input->fd, buffer, static_cast<size_t>(count), position); } while (n < 0 && errno == EINTR);
        if (n < 0) { input->stopped = -1; return 0; }
        position += n; input->reads += static_cast<uint64_t>(n);
        return n;
    }
public:
    Snapshot(std::shared_ptr<Input> in, Goffset at, bool bounded, Goffset length, Object &&dict)
        : BaseSeekInputStream(at, bounded, length, std::move(dict)), input(std::move(in)) { }
#if HAVE_POPPLER_NEW_API
    std::unique_ptr<BaseStream> copy() override {
        return std::make_unique<Snapshot>(input, start, limited, length, dict.copy());
    }
#else
    BaseStream *copy() override { return new Snapshot(input, start, limited, length, dict.copy()); }
#endif
    std::unique_ptr<Stream> makeSubStream(Goffset at, bool bounded, Goffset size, Object &&dict) override {
        if (at < 0 || size < 0 || at > input->size || (bounded && size > input->size - at)) {
            input->stopped = -1; at = 0; bounded = true; size = 0;
        }
        return std::make_unique<Snapshot>(input, at, bounded, size, std::move(dict));
    }
};
static std::once_flag initialized;
class Render final : public SplashOutputDev {
public:
    using SplashOutputDev::SplashOutputDev;
    bool missing_font = false;
    void drawChar(GfxState *state, double x, double y, double dx, double dy,
                  double originX, double originY, CharCode code, int nBytes,
                  const Unicode *u, int uLen) override {
        SplashOutputDev::drawChar(state, x, y, dx, dy, originX, originY, code, nBytes, u, uLen);
        // Splash silently skips visible text when it cannot load a font.
        if (state->getRender() != 3 && !getCurrentFont()) missing_font = true;
    }
};
static bool abort_page(void *opaque) { return static_cast<Input *>(opaque)->stop(); }
static void text_write(void *opaque, const char *data, int size) {
    auto *input = static_cast<Input *>(opaque);
    if (input->stop()) return;
    if (size < 0 || snag_buf_append(input->text, data, static_cast<size_t>(size)) < 0) input->stopped = -1;
}
static void png_error_cb(png_structp png, png_const_charp) { png_longjmp(png, 1); }
static void png_warning_cb(png_structp, png_const_charp) { }
static void png_write_cb(png_structp png, png_bytep data, png_size_t size) {
    if (snag_buf_append(static_cast<snag_buf *>(png_get_io_ptr(png)), data, size) < 0) png_error(png, "output bound");
}
static void png_flush_cb(png_structp) { }
static int bitmap_png(SplashBitmap *bitmap, Input *input, snag_buf *output) {
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, png_error_cb, png_warning_cb);
    if (!png) return -1;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); return -1; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); return -1; }
    png_set_write_fn(png, output, png_write_cb, png_flush_cb);
    png_set_IHDR(png, info, bitmap->getWidth(), bitmap->getHeight(), 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < bitmap->getHeight(); ++y) {
        if (input->stop()) png_error(png, "cancelled");
        png_write_row(png, bitmap->getDataPtr() + static_cast<size_t>(y) * bitmap->getRowSize());
    }
    png_write_end(png, info); png_destroy_write_struct(&png, &info);
    return 0;
}
}
struct snag_pdf {
    std::shared_ptr<Input> input;
    std::unique_ptr<PDFDoc> doc;
};
void snag_pdf_close(struct snag_pdf *pdf) { delete pdf; }
int
snag_pdf_open(const char *path, int (*pump)(void *, unsigned int), void *opaque,
               struct snag_pdf **out, unsigned int *pages, char *error, size_t size)
{
    *out = nullptr;
    try {
        std::call_once(initialized, [] {
            if (!globalParams) globalParams = std::make_unique<GlobalParams>();
            globalParams->setTextEncoding("UTF-8"); globalParams->setErrQuiet(true);
        });
        auto pdf = std::make_unique<snag_pdf>();
        pdf->input = std::make_shared<Input>();
        auto &in = *pdf->input;
        in.pump = pump; in.opaque = opaque; in.started = snag_monotonic_ms();
        in.fd = snag_open_inspect_path("/", path);
        snag_file_info st;
        if (in.fd < 0 || snag_fstat(in.fd, &st) < 0 || !S_ISREG(st.st_mode) ||
            st.st_size <= 0 || st.st_size > 256 * 1024 * 1024) throw 0;
        in.size = st.st_size;
#if HAVE_POPPLER_NEW_API
        pdf->doc = std::make_unique<PDFDoc>(std::make_unique<Snapshot>(pdf->input, 0, false, in.size, Object::null()));
#else
        pdf->doc = std::make_unique<PDFDoc>(new Snapshot(pdf->input, 0, false, in.size, Object::null()));
#endif
        if (in.stop()) { snag_errorf(error, size, "PDF loading interrupted or bounded input exhausted"); return in.stopped == 2 ? 2 : -1; }
        if (!pdf->doc->isOk() || pdf->doc->isEncrypted() || pdf->doc->getNumPages() < 1 || pdf->doc->getNumPages() > 100000) throw 0;
        *pages = static_cast<unsigned int>(pdf->doc->getNumPages()); *out = pdf.release();
        return 0;
    } catch (...) { snag_errorf(error, size, "Cannot load retained PDF: malformed, encrypted or resource limit"); return -1; }
}
int
snag_pdf_page(struct snag_pdf *pdf, unsigned int page, struct snag_buf *text,
               struct snag_buf *image, char *error, size_t size)
{
    size_t text_start = text->len, image_start = image->len;
    bool missing_font = false;
    try {
        auto &in = *pdf->input;
        if (!page || page > static_cast<unsigned int>(pdf->doc->getNumPages()) || in.stop()) throw 0;
        double width = pdf->doc->getPageCropWidth(page), height = pdf->doc->getPageCropHeight(page);
        if (!(width > 0 && width <= 14400 && height > 0 && height <= 14400)) throw 0;
        in.text = text;
        TextOutputDev extraction(text_write, &in, true, 0.0, false);
        pdf->doc->displayPage(&extraction, page, 72.0, 72.0, 0, false, true, false, abort_page, &in);
        if (in.stop() || (text->len != text_start && !snag_utf8_valid(text->data + text_start, text->len - text_start, true))) throw 0;
        double dpi = 72.0 * 1600.0 / std::max(width, height);
        SplashColor white = {255, 255, 255};
#if HAVE_POPPLER_NEW_API
        Render render(splashModeRGB8, 4, white, true);
#else
        Render render(splashModeRGB8, 4, false, white, true);
#endif
        render.startDoc(pdf->doc.get());
        pdf->doc->displayPage(&render, page, dpi, dpi, 0, false, true, false, abort_page, &in);
        missing_font = render.missing_font;
        if (missing_font) throw 0;
        SplashBitmap *bitmap = render.getBitmap();
        if (in.stop() || !bitmap || bitmap->getWidth() < 1 || bitmap->getHeight() < 1 ||
            bitmap->getWidth() > 1601 || bitmap->getHeight() > 1601 || bitmap_png(bitmap, &in, image) < 0) throw 0;
        return 0;
    } catch (...) {
        text->len = text_start; image->len = image_start;
        snag_errorf(error, size, "%s", missing_font ?
            "PDF page font unavailable or unusable; install suitable system fonts or use a PDF with embedded fonts" :
            "PDF page failed, interrupted or exceeded its text/image bounds");
        return pdf->input->stopped == 2 ? 2 : -1;
    }
}

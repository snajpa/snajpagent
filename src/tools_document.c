/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools.h"
#include "store.h"
#include "media.h"
#include "convert.h"
#include "fs.h"
#include "av.h"
#include "pdf.h"
#include "office.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
text_part(json_t *parts, const char *text)
{
    return json_array_append_new(parts, json_pack("{s:s,s:s}", "type", "input_text", "text", text));
}

static int
image_part(int fd, json_t *parts, const struct snag_buf *image, char *error, size_t size)
{
    json_t *asset = NULL;
    if (image->len < 8u || memcmp(image->data, "\211PNG\r\n\032\n", 8u)) {
        snag_errorf(error, size, "Converter did not produce a PNG image"); return -1;
    }
    if (snag_media_save(fd, image->data, image->len, "image/png", &asset, error, size) < 0) return -1;
    return json_array_append_new(parts, json_pack("{s:s,s:o}", "type", "input_image", "asset", asset));
}

static bool
number(const json_t *args, const char *key, uint64_t fallback, uint64_t lo, uint64_t hi, uint64_t *out)
{
    json_t *v = json_object_get(args, key);
    *out = fallback;
    return !v || json_is_null(v) ||
        (snag_json_integer_u64(args, key, out) == 0 && *out >= lo && *out <= hi);
}

static int
pdf_pages(struct snag_session *session, const char *path, uint64_t first, uint64_t last,
           json_t *parts, snag_tool_pump_fn pump, void *opaque, snag_wake_fd wake,
           uint64_t page_offset, const char *page_kind, char *error, size_t size)
{
    struct snag_pdf *pdf = NULL;
    unsigned int count = 0;
    (void)wake;
    int rc = snag_pdf_open(path, pump, opaque, &pdf, &count, error, size);
    if (rc) return rc;
    if (page_kind && (first!=1u || last!=count)) {
        snag_errorf(error,size,"Office PDF page count does not match the selection");
        snag_pdf_close(pdf);return -1;
    }
    if (last > count) {
        snag_errorf(error, size, "PDF has %u pages; select an existing page range", count);
        snag_pdf_close(pdf); return -1;
    }
    for (uint64_t page = first; page <= last; ++page) {
        struct snag_buf text, image;
        char label[128];
        if (page_kind) (void)snprintf(label, sizeof(label), "%s %llu (extracted text, then rendered page):",
                                       page_kind,(unsigned long long)(page+page_offset));
        else (void)snprintf(label, sizeof(label), "Document page %llu of %u (extracted text, then rendered page):",
                            (unsigned long long)page, count);
        rc = text_part(parts, label);
        snag_buf_init(&text, 256u * 1024u); snag_buf_init(&image, SNAG_MEDIA_REQUEST_MAX);
        if (!rc) rc = snag_pdf_page(pdf, (unsigned int)page, &text, &image, error, size);
        if (!rc) rc = snag_buf_terminate(&text);
        if (!rc) rc = text_part(parts, text.len > 1u ? (char *)text.data : "No extracted text; inspect the page image.");
        if (!rc) rc = image_part(session->dir_fd, parts, &image, error, size);
        snag_buf_free(&text); snag_buf_free(&image);
        if (rc) break;
    }
    snag_pdf_close(pdf);
    return rc;
}

static int
text_rows(const char *path, const char *mime, uint64_t first, uint64_t last,
           json_t *parts, snag_tool_pump_fn pump, void *opaque, char *error, size_t size)
{
    int fd = snag_open_inspect_path("/", path);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "rb");
    if (!file) { close(fd); return -1; }
    struct snag_buf out, record;
    snag_buf_init(&out, 256u * 1024u); snag_buf_init(&record, 256u * 1024u);
    bool csv = !strcmp(mime, "text/csv"), quoted = false, field_start = true, after_quote = false;
    uint64_t row = 1, read_bytes = 0;
    int rc = -1, c;
    while ((c = fgetc(file)) != EOF) {
        if (++read_bytes > SNAG_MEDIA_FILE_MAX) goto out;
        if (!(read_bytes % 32768u) && pump && (rc = pump(opaque, 0u))) goto out;
        rc = -1;
        if (c == 0 || snag_buf_putc(&record, (unsigned char)c) < 0) goto out;
        if (csv) {
            if (quoted) {
                if (c == '"') { quoted = false; after_quote = true; }
                continue;
            }
            if (after_quote && c == '"') { quoted = true; after_quote = false; continue; }
            if (field_start && c == '"') { quoted = true; field_start = false; continue; }
            if (after_quote && c != ',' && c != '\r' && c != '\n') {
                snag_errorf(error, size, "Malformed quoted CSV field at row %llu", (unsigned long long)row); goto out;
            }
            if (!field_start && !after_quote && c == '"') {
                snag_errorf(error, size, "Unexpected quote in CSV row %llu", (unsigned long long)row); goto out;
            }
            field_start = c == ',' || c == '\n';
            if (c == ',') after_quote = false;
        }
        if (c != '\n') continue;
        if (row >= first && (snag_buf_printf(&out, "%llu: ", (unsigned long long)row) < 0 ||
            snag_buf_append(&out, record.data, record.len) < 0)) goto out;
        snag_buf_reset(&record); after_quote = false; field_start = true;
        if (row++ >= last) break;
    }
    if (ferror(file) || quoted) { snag_errorf(error, size, "Document read failed or quoted CSV record is incomplete"); goto out; }
    if (record.len && row >= first && row <= last &&
        (snag_buf_printf(&out, "%llu: ", (unsigned long long)row) < 0 ||
         snag_buf_append(&out, record.data, record.len) < 0)) goto out;
    if (!snag_utf8_valid(out.data, out.len, true) || snag_buf_terminate(&out) < 0) goto out;
    rc = text_part(parts, (char *)out.data);
out:
    if (rc < 0 && !error[0]) snag_errorf(error, size, "Text selection is invalid UTF-8 or exceeds 256 KiB; select a smaller range");
    fclose(file); snag_buf_free(&record); snag_buf_free(&out);
    return rc;
}

int
snag_tools_document(const struct snag_response_item *call, struct snag_session *session,
                    snag_tool_pump_fn pump, void *opaque, snag_wake_fd wake, json_t **result)
{
    struct snag_sheet_range range={0},*selection=NULL;
    const char *path = snag_json_string(call->arguments, "path");
    uint64_t first = 0, last = 0;
    json_t *source = NULL, *parts = NULL;
    char label[320];
    char *retained = NULL, error[256] = "Invalid document arguments: path, first, last (inclusive, 1-based).";
    int rc = -1;
    *result = NULL;
    if ((!snag_json_exact_keys(call->arguments,"path first last") &&
         !snag_json_exact_keys(call->arguments,"path first last sheet_range")) || !path ||
        !number(call->arguments, "first", 1u, 1u, 1000000u, &first) ||
        !number(call->arguments, "last", first, first, 1000000u, &last)) goto out;
    bool page_selection=!json_is_null(json_object_get(call->arguments,"first")) ||
        !json_is_null(json_object_get(call->arguments,"last"));
    json_t *sel=json_object_get(call->arguments,"sheet_range");
    if(sel && !json_is_null(sel)) {
        static const char *const names[]={"sheet","row","column","rows","columns"};
        uint64_t v[5];
        if(page_selection || !snag_json_exact_keys(sel,"sheet row column rows columns"))goto out;
        for(size_t i=0;i<5u;++i)if(snag_json_integer_u64(sel,names[i],&v[i])<0 || v[i]>1048576u)goto out;
        range=(struct snag_sheet_range){(uint32_t)v[0],(uint32_t)v[1],(uint32_t)v[2],(uint32_t)v[3],(uint32_t)v[4]};
        if(!snag_sheet_range_valid(&range))goto out;
        selection=&range;
    }
    const char *mime = snag_media_mime(path);
    if (!mime && strncmp(path, "asset:", 6u)) { strcpy(error, "Unsupported document filename/type"); goto out; }
    error[0] = '\0';
    if (snag_session_media(session, path, mime, pump, opaque, &source, &retained, error, sizeof(error)) < 0) goto out;
    parts = json_pack("[{s:s,s:O}]", "type", "file", "asset", source);
    if (!parts) goto out;
    mime = snag_json_string(source, "mime_type");
    bool workbook=!strcmp(mime,"application/vnd.oasis.opendocument.spreadsheet") ||
        !strcmp(mime,"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
    if(selection && !workbook){strcpy(error,"sheet_range requires XLSX or ODS");goto out;}
    if(workbook && !page_selection && !selection) {
        range=(struct snag_sheet_range){1u,1u,1u,20u,8u};selection=&range;
    }
    if (!strcmp(mime, "application/pdf")) {
        if (last - first >= 4u) { strcpy(error, "Select at most four PDF pages per call"); goto out; }
        rc = pdf_pages(session, retained, first, last, parts, pump, opaque, wake, 0u, NULL, error, sizeof(error));
    } else if (!strncmp(mime, "text/", 5u)) {
        if (last - first >= 200u) { strcpy(error, "Select at most 200 text/CSV records per call"); goto out; }
        rc = text_rows(retained, mime, first, last, parts, pump, opaque, error, sizeof(error));
    } else {
        if (last-first >= 4u || last > 100000u) { strcpy(error,"Select at most four Office pages, numbered 1..100000"); goto out; }
        struct snag_buf pdf;
        snag_buf_init(&pdf,32u*1024u*1024u);
        json_t *meta=NULL,*derived=NULL;
        rc=snag_office_export(session,retained,mime,(unsigned int)first,(unsigned int)last,selection,pump,opaque,wake,
                               &pdf,&meta,error,sizeof(error));
        if(!rc)rc=text_part(parts,snag_json_string(meta,"coverage"));
        if(!rc && selection) {
            char summary[1024];
            const char *name=snag_json_string(meta,"sheet_name"), *cells=snag_json_string(meta,"cells");
            if(!name || !cells) {strcpy(error,"Office sheet metadata is missing");rc=-1;}
            else {
                snprintf(summary,sizeof(summary),"Sheet %u of %lld: %s. One-based row %u, column %u; %u rows by %u columns. "
                    "Cell values are JSON-quoted; covered cells identify their merged anchor. Other sheets/cells uninspected.",
                    range.sheet,(long long)json_integer_value(json_object_get(meta,"sheet_count")),name,
                    range.row,range.column,range.rows,range.columns);
                rc=text_part(parts,summary);
                if(!rc)rc=text_part(parts,cells);
                if(!rc)rc=image_part(session->dir_fd,parts,&pdf,error,sizeof(error));
            }
        } else if(!rc) {
            rc=snag_media_save(session->dir_fd,pdf.data,pdf.len,"application/pdf",&derived,error,sizeof(error));
            char *dir=NULL,*prepared=NULL;
            if(!rc) {
                dir=snag_path_join(session->dir_path,"media");
                prepared=dir?snag_path_join(dir,snag_json_string(derived,"id")):NULL;
                if(!prepared)rc=-1;
            }
            if(!rc)rc=json_array_append_new(parts,json_pack("{s:s,s:O}","type","file","asset",derived));
            const char *kind=snag_json_string(meta,"page_kind");
            if(!rc && (!kind || (strcmp(kind,"Source slide") && strcmp(kind,"Imported workbook print page") &&
                strcmp(kind,"Imported document page")))) {strcpy(error,"Office page provenance is missing");rc=-1;}
            if(!rc)rc=pdf_pages(session,prepared,1u,last-first+1u,parts,pump,opaque,wake,first-1u,kind,error,sizeof(error));
            free(dir);free(prepared);
        }
        snag_buf_free(&pdf);json_decref(meta);json_decref(derived);
    }
out:
    (void)snprintf(label, sizeof(label), "Document selection %llu-%llu only; other pages/records were not inspected. Source asset:%s",
        (unsigned long long)(source ? first : 0), (unsigned long long)(source ? last : 0), source ? snag_json_string(source, "id") : "none");
    if(selection && source)(void)snprintf(label,sizeof(label),"Sheet %u range row %u column %u (%u x %u) only. Source asset:%s",
        range.sheet,range.row,range.column,range.rows,range.columns,snag_json_string(source,"id"));
    if (rc == 0 && !snag_media_content_valid(parts)) { strcpy(error, "Selected document pages exceed the media budget"); rc = -1; }
    *result = snag_tool_result_terminal(rc == 0, rc == 0 ? label : error[0] ? error : "Document conversion failed");
    if (rc == 0 && snag_json_set_new(*result, "content", json_incref(parts)) < 0) { json_decref(*result); *result = NULL; }
    json_decref(parts); json_decref(source); free(retained);
    return !*result ? -1 : rc == 2 ? 2 : 0;
}

int
snag_tools_video(const struct snag_response_item *call, struct snag_session *session,
                 snag_tool_pump_fn pump, void *opaque, snag_wake_fd wake,
                 snag_video_audio_fn audio, json_t **result)
{
    const char *path = snag_json_string(call->arguments, "path");
    uint64_t start = 0, end = 0, frames = 0;
    char error[256] = "Invalid video arguments: bounded integer start_s/end_s and 1-8 frames.";
    char *retained = NULL;
    json_t *source = NULL, *parts = NULL;
    struct snag_av_video *video = NULL;
    struct snag_av_video_info info;
    int rc = -1;
    *result = NULL;
    (void)wake;
    if (!snag_json_exact_keys(call->arguments,"path start_s end_s frames") || !path ||
        !number(call->arguments, "start_s", 0u, 0u, 86400u, &start) ||
        !number(call->arguments, "end_s", start + 30u, start + 1u, start + 30u, &end) ||
        !number(call->arguments, "frames", 8u, 1u, 8u, &frames)) goto out;
    const char *mime = snag_media_mime(path);
    if (!mime && strncmp(path, "asset:", 6u)) goto out;
    error[0] = '\0';
    if (snag_session_media(session, path, mime, pump, opaque, &source, &retained, error, sizeof(error)) < 0) goto out;
    if (strncmp(snag_json_string(source, "mime_type"), "video/", 6u)) { strcpy(error, "Expected a video file"); goto out; }
    parts = json_pack("[{s:s,s:O}]", "type", "file", "asset", source);
    if (!parts) goto out;
    rc = snag_av_video_open(retained, pump, opaque, &video, &info, error, sizeof(error));
    if (rc) goto out;
    if ((double)start >= info.duration) { strcpy(error, "Video start is beyond the duration"); rc = -1; goto out; }
    double stop = (double)end < info.duration ? (double)end : info.duration;
    bool has_audio = info.has_audio;
    char coverage[192];
    (void)snprintf(coverage, sizeof(coverage), "Video duration %.6fs; selected [%llu, %.6f)s, %llu samples. %s",
        info.duration, (unsigned long long)start, stop, (unsigned long long)frames,
        has_audio ? "Audio stream present; separate coverage follows." : "No audio stream.");
    if (text_part(parts, coverage) < 0) { rc = -1; goto out; }
    for (uint64_t i = 0; i < frames; ++i) {
        double t = frames == 1u ? (double)start :
            (double)start + (stop - (double)start) * (double)i / (double)(frames - 1u);
        if (i && i == frames - 1u) t = stop - 0.000001;
        struct snag_buf image;
        char label[256], transform[96]; double actual = 0;
        snag_buf_init(&image, SNAG_MEDIA_REQUEST_MAX);
        rc = snag_av_video_frame(video, t, (double)start, stop, &image, &actual,
                                  transform, sizeof(transform), error, sizeof(error));
        if (!rc) {
            (void)snprintf(label, sizeof(label), "Frame source PTS %.6fs (seek %.6fs); %s. Events between samples can be missed.", actual, t, transform);
            rc = text_part(parts, label);
        }
        if (!rc) rc = image_part(session->dir_fd, parts, &image, error, sizeof(error));
        snag_buf_free(&image);
        if (rc) goto out;
    }
    if (has_audio) {
        json_t *transcript = NULL;
        int audio_rc = audio ? audio(opaque, source, start, end, &transcript) : 0;
        const char *status = snag_json_string(transcript, "status");
        bool ok = !audio_rc && status && !strcmp(status, "succeeded");
        if (text_part(parts, ok ? "Aligned speech transcript for the selected video interval:" :
                      "Video audio uninspected: transcription unavailable, failed or interrupted.") < 0) rc = -1;
        const char *detail = snag_json_string(transcript, "model_text");
        if (!rc && detail && text_part(parts, detail) < 0) rc = -1;
        if (!rc && ok) {
            json_t *content = json_object_get(transcript, "content");
            /* The original source is already the first video part. */
            for (size_t i = 1u; i < json_array_size(content); ++i)
                if (json_array_append(parts, json_array_get(content, i)) < 0) { rc = -1; break; }
        }
        json_decref(transcript);
        if (audio_rc == 2) {
            strcpy(error, "Video audio transcription interrupted; no combined result accepted");
            rc = 2;
        }
    }
out:
    if (!rc && !snag_media_content_valid(parts)) { strcpy(error, "Selected frames exceed the image budget"); rc = -1; }
    *result = snag_tool_result_terminal(rc == 0, rc == 0 ?
        "Sampled video frames; audio coverage is stated in the result." :
        error[0] ? error : "Video inspection failed");
    if (!rc && snag_json_set_new(*result, "content", json_incref(parts)) < 0) { json_decref(*result); *result = NULL; }
    snag_av_video_close(video);
    json_decref(source); json_decref(parts); free(retained);
    return !*result ? -1 : rc == 2 ? 2 : 0;
}

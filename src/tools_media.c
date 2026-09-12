/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools.h"
#include "media.h"
#include "store.h"
#include "av.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct image_pump {
    snag_tool_pump_fn callback;
    void *opaque;
    int interrupted;
};

static int
image_checkpoint(void *opaque, unsigned int timeout_ms)
{
    struct image_pump *state = opaque;
    state->interrupted = state->callback ? state->callback(state->opaque, timeout_ms) : 0;
    return state->interrupted;
}

int
snag_image_prepare(struct snag_session *session, const char *path, uint32_t frame,
                    const struct snag_image_crop *crop, snag_tool_pump_fn pump, void *opaque,
                    json_t **part, char *error, size_t size)
{
    json_t *source = NULL, *derived = NULL;
    char *retained = NULL, note[768];
    struct snag_buf png;
    struct image_pump state = {.callback = pump, .opaque = opaque};
    *part = NULL;
    snag_buf_init(&png, SNAG_MEDIA_REQUEST_MAX);
    int rc = snag_session_media(session, path, NULL, image_checkpoint, &state, &source, &retained, error, size);
    if (rc < 0) goto out;
    const char *mime = snag_json_string(source, "mime_type");
    if (!mime || strncmp(mime, "image/", 6u)) {
        snag_errorf(error, size, "view_image requires an image source"); rc = -1; goto out;
    }
#if !SNAJPAGENT_AV
    if (frame == 0u && !crop && (!strcmp(mime, "image/png") || !strcmp(mime, "image/jpeg")) &&
        json_integer_value(json_object_get(source, "bytes")) <= SNAG_MEDIA_REQUEST_MAX) {
        *part = json_pack("{s:s,s:O}", "type", "input_image", "asset", source);
        rc = *part ? 0 : -1; goto out;
    }
#endif
    rc = snag_av_image(retained, frame, crop, &png, note, sizeof(note), image_checkpoint, &state, error, size);
    if (rc) goto out;
    if (snag_media_save(session->dir_fd, png.data, png.len, "image/png", &derived, error, size) < 0) { rc = -1; goto out; }
    *part = json_pack("{s:s,s:O,s:O,s:s}", "type", "input_image", "asset", derived, "source", source, "note", note);
    rc = *part ? 0 : -1;
out:
    if (state.interrupted == 2) rc = 2;
    json_decref(source); json_decref(derived); free(retained); snag_buf_free(&png);
    return rc;
}

int
snag_tools_image(const struct snag_response_item *call, struct snag_session *session,
                 snag_tool_pump_fn pump, void *opaque, json_t **result)
{
    const char *path = snag_json_string(call->arguments, "path");
    uint64_t frame = 0;
    json_t *crop_value = json_object_get(call->arguments, "crop"), *part = NULL;
    struct snag_image_crop crop, *selection = NULL;
    char message[1024] = "view_image requires path, optional frame 0..999 and crop {x,y,width,height} in source pixels.";
    *result = NULL;
    if (!snag_json_arg_keys(call->arguments, "path", "frame crop", message, sizeof(message)) ||
        !snag_json_arg_text(call->arguments, "path", 1u, SNAG_PATH_MAX_BYTES, false,
                            &path, message, sizeof(message)) ||
        !snag_json_arg_uint(call->arguments, "frame", 0u, 0u, 999u, &frame,
                            message, sizeof(message))) goto invalid;
    if (crop_value && !json_is_null(crop_value)) {
        static const char *const coords[] = {"x", "y", "width", "height"};
        uint64_t values[4];
        if (!snag_json_exact_keys(crop_value,"x y width height")) goto invalid;
        for (size_t i = 0; i < 4u; ++i)
            if (snag_json_integer_u64(crop_value, coords[i], &values[i]) < 0 || values[i] > 16777216u) goto invalid;
        if (!values[2] || !values[3]) goto invalid;
        crop = (struct snag_image_crop){(uint32_t)values[0], (uint32_t)values[1], (uint32_t)values[2], (uint32_t)values[3]};
        selection = &crop;
    }
    int rc = snag_image_prepare(session, path, (uint32_t)frame, selection, pump, opaque, &part, message, sizeof(message));
    if (rc) {
        *result = snag_tool_result_terminal(false, message);
        return !*result ? -1 : rc == 2 ? 2 : 0;
    }
    const char *note = snag_json_string(part, "note");
    (void)snprintf(message, sizeof(message), "Image prepared as asset:%s. %s",
        snag_json_string(json_object_get(part, "asset"), "id"), note ? note : "Native PNG/JPEG; custom build without normalization.");
    *result = snag_tool_result_terminal(true, message);
    json_t *content = json_pack("[O]", part);
    json_decref(part);
    if (!*result || snag_json_set_new(*result, "content", content) < 0) {
        if (!*result) json_decref(content);
        json_decref(*result); *result = NULL; return -1;
    }
    return 0;
invalid:
    *result = snag_tool_result_terminal(false, message);
    return *result ? 0 : -1;
}

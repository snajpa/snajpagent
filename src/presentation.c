/* SPDX-License-Identifier: GPL-2.0-only */
#include "presentation.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int
snag_presentation_apply(struct snag_render *render, const struct snag_ui_command *command,
    struct snag_buf *delivered)
{
    switch (command->kind) {
    case SNAG_UI_HOST:
        return snag_render_host(render, command->text);
    case SNAG_UI_HELP:
        return snag_render_help(render, command->text);
    case SNAG_UI_RUNTIME:
        return snag_render_runtime(render, command->text);
    case SNAG_UI_ERROR:
        return snag_render_error_ctx(render, command->text);
    case SNAG_UI_WARNING:
        return snag_render_warning_ctx(render, command->text);
    case SNAG_UI_ROLLOUT_END:
        return snag_render_rollout_end(render);
    case SNAG_UI_ROLLOUT_ABORT:
        return snag_render_rollout_abort(render);
    case SNAG_UI_SUBMITTED:
        return command->data.value ?
            snag_render_input_submitted(render, command->label, command->text) :
            snag_render_submitted(render, command->label, command->text);
    case SNAG_UI_BEFORE_PROMPT:
        return snag_render_before_prompt(render);
    case SNAG_UI_PUBLIC_BEGIN:
        return snag_render_rollout_begin(render, command->data.public.fd,
            command->label, command->data.public.kind);
    case SNAG_UI_PUBLIC:
        return snag_render_rollout(render, command->text, command->len, delivered);
    case SNAG_UI_ORIENTATION:
        return snag_render_orientation(render, command->text, command->label,
            command->data.orientation.turns, command->data.orientation.queued,
            command->data.orientation.resumed, command->data.orientation.queue_armed);
    case SNAG_UI_HISTORY:
        return snag_render_history(render, command->data.replay.turn,
            command->data.replay.shown, command->data.replay.completed, command->data.replay.total);
    case SNAG_UI_IRC:
        return snag_render_irc_event(render, command->data.irc);
    case SNAG_UI_VOICE_EVENT:
        return snag_render_voice_event(render, command->data.voice, 0u, 0u);
    case SNAG_UI_DURABLE:
        return snag_render_durable(render, command->data.durable.fd,
            command->data.durable.source, command->text,
            command->data.durable.timeout_ms, command->data.durable.max_output_bytes);
    case SNAG_UI_EVENT:
        return snag_render_event(render, command->data.seq, command->text);
    case SNAG_UI_RESUME:
        return snag_render_resume_hint(render, command->text, command->len);
    case SNAG_UI_PROTOCOL:
        return snag_render_protocol(render, command->label, command->text, command->len);
    case SNAG_UI_TRANSPORT:
        return snag_render_transport(render, (char)command->data.value,
            command->text, command->len);
    default:
        return 1;
    }
}

static const struct presentation_operation {
    enum snag_ui_operation kind;
    const char *name;
} operations[] = {
    {SNAG_UI_HOST, "host"}, {SNAG_UI_HELP, "help"}, {SNAG_UI_RUNTIME, "runtime"},
    {SNAG_UI_ERROR, "error"}, {SNAG_UI_WARNING, "warning"},
    {SNAG_UI_ROLLOUT_END, "end"}, {SNAG_UI_ROLLOUT_ABORT, "abort"},
    {SNAG_UI_SUBMITTED, "submitted"}, {SNAG_UI_BEFORE_PROMPT, "prompt"},
    {SNAG_UI_PUBLIC_BEGIN, "begin"},
    {SNAG_UI_PUBLIC, "public"}, {SNAG_UI_ORIENTATION, "orientation"},
    {SNAG_UI_HISTORY, "history"}, {SNAG_UI_IRC, "irc"},
    {SNAG_UI_VOICE_EVENT, "voice"}, {SNAG_UI_DURABLE, "durable"},
    {SNAG_UI_EVENT, "event"}, {SNAG_UI_RESUME, "resume"},
    {SNAG_UI_PROTOCOL, "protocol"}, {SNAG_UI_TRANSPORT, "transport"}
};

static json_t *
encode_bytes(const void *bytes, size_t length)
{
    struct snag_buf base64 = {.max = SNAG_MAX_EVENT_LINE};
    json_t *value = NULL;
    if (snag_base64_append(&base64, bytes, length) == 0)
        value = json_stringn(base64.data ? (const char *)base64.data : "", base64.len);
    snag_buf_free(&base64);
    return value;
}

static json_t *
encode_source(const struct snag_render_source *source)
{
    const struct snag_binary_anchor *anchor = &source->native_boundary;
    return json_pack("{s:I,s:I,s:I,s:[I,I,I,I,o]}",
        "offset", (json_int_t)source->offset, "length", (json_int_t)source->len,
        "sequence", (json_int_t)source->native_sequence,
        "boundary", (json_int_t)anchor->end, (json_int_t)anchor->next_seq,
        (json_int_t)anchor->turns, (json_int_t)anchor->previous,
        encode_bytes(anchor->digest, sizeof(anchor->digest)));
}

int
snag_presentation_encode(const struct snag_ui_command *command, json_t **out)
{
    size_t index = 0u;
    while (index < sizeof(operations) / sizeof(*operations) &&
        operations[index].kind != command->kind) ++index;
    if (index == sizeof(operations) / sizeof(*operations)) return 1;
    json_t *data = NULL;
    switch (command->kind) {
    case SNAG_UI_SUBMITTED:
    case SNAG_UI_TRANSPORT:
        data = json_integer(command->data.value);
        break;
    case SNAG_UI_PUBLIC_BEGIN:
        data = json_pack("[i,i]", command->data.public.fd, command->data.public.kind);
        break;
    case SNAG_UI_ORIENTATION:
        data = json_pack("[I,I,b,b]", (json_int_t)command->data.orientation.turns,
            (json_int_t)command->data.orientation.queued, command->data.orientation.resumed,
            command->data.orientation.queue_armed);
        break;
    case SNAG_UI_HISTORY: {
        const struct snag_history_turn *turn = command->data.replay.turn;
        json_t *row = turn ? json_pack("{s:s?,s:s?,s:s?,s:b,s:b,s:b}",
            "user", turn->user, "assistant", turn->assistant, "status", turn->status,
            "timer", turn->timer, "partial", turn->partial, "continuation", turn->continuation) :
            json_null();
        data = json_pack("[I,I,I,o]", (json_int_t)command->data.replay.shown,
            (json_int_t)command->data.replay.completed,
            (json_int_t)command->data.replay.total, row);
        break;
    }
    case SNAG_UI_IRC:
        data = snag_irc_event_data(command->data.irc);
        break;
    case SNAG_UI_VOICE_EVENT:
        data = json_incref((json_t *)command->data.voice);
        break;
    case SNAG_UI_DURABLE:
        data = json_pack("[o,I,I]", encode_source(&command->data.durable.source),
            (json_int_t)command->data.durable.timeout_ms,
            (json_int_t)command->data.durable.max_output_bytes);
        break;
    case SNAG_UI_EVENT:
        data = json_integer(command->data.seq);
        break;
    default:
        data = json_null();
        break;
    }
    if (!data) return -1;
    bool bytes = command->kind == SNAG_UI_PUBLIC || command->kind == SNAG_UI_PROTOCOL ||
        command->kind == SNAG_UI_TRANSPORT || command->kind == SNAG_UI_RESUME;
    json_t *text = bytes ? encode_bytes(command->text, command->len) :
        command->text ? json_string(command->text) : json_null();
    json_t *record = json_pack("{s:s,s:o,s:s?,s:o}", "op", operations[index].name,
        "text", text, "label", command->label, "data", data);
    if (!record) return -1;
    *out = record;
    return 0;
}

static bool
unsigned_at(const json_t *array, size_t index, uint64_t maximum, uint64_t *out)
{
    const json_t *value = json_array_get(array, index);
    if (!json_is_integer(value) || json_integer_value(value) < 0 ||
        (uint64_t)json_integer_value(value) > maximum) return false;
    *out = (uint64_t)json_integer_value(value);
    return true;
}

static int
read_source(const json_t *data, struct snag_render_source *source)
{
    uint64_t offset, length, sequence;
    const json_t *boundary = json_object_get(data, "boundary");
    struct snag_binary_anchor *anchor = &source->native_boundary;
    if (snag_json_integer_u64(data, "offset", &offset) < 0 || offset > INT64_MAX ||
        snag_json_integer_u64(data, "length", &length) < 0 || length > SIZE_MAX ||
        snag_json_integer_u64(data, "sequence", &sequence) < 0 ||
        json_array_size(boundary) != 5u ||
        !unsigned_at(boundary, 0u, INT64_MAX, &anchor->end) ||
        !unsigned_at(boundary, 1u, INT64_MAX, &anchor->next_seq) ||
        !unsigned_at(boundary, 2u, INT64_MAX, &anchor->turns) ||
        !unsigned_at(boundary, 3u, INT64_MAX, &anchor->previous)) return snag_errno(EINVAL);
    const char *digest = json_string_value(json_array_get(boundary, 4u));
    struct snag_buf decoded = {.max = sizeof(anchor->digest)};
    int rc = digest ? snag_base64_decode(&decoded, digest) : -1;
    if (!rc && decoded.len == sizeof(anchor->digest)) {
        memcpy(anchor->digest, decoded.data, sizeof(anchor->digest));
        source->offset = (int64_t)offset;
        source->len = (size_t)length;
        source->native_sequence = sequence;
    } else rc = snag_errno(EINVAL);
    snag_buf_free(&decoded);
    return rc;
}

int
snag_presentation_replay(struct snag_render *render, const json_t *record, int journal_fd)
{
    const char *op = snag_json_string(record, "op");
    size_t index = 0u;
    while (op && index < sizeof(operations) / sizeof(*operations) &&
        strcmp(operations[index].name, op)) ++index;
    if (!op || index == sizeof(operations) / sizeof(*operations)) return snag_errno(EINVAL);
    struct snag_ui_command command = {.kind = operations[index].kind,
        .text = snag_json_string(record, "text"), .label = snag_json_string(record, "label")};
    const json_t *data = json_object_get(record, "data");
    struct snag_history_turn turn = {0};
    struct snag_irc_event irc;
    uint64_t values[3] = {0};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    bool encoded = command.kind == SNAG_UI_PUBLIC || command.kind == SNAG_UI_PROTOCOL ||
        command.kind == SNAG_UI_TRANSPORT || command.kind == SNAG_UI_RESUME;
    int rc = -1;
    if (encoded) {
        if (!command.text || snag_base64_decode(&bytes, command.text) < 0) goto out;
        command.text = (const char *)bytes.data;
        command.len = bytes.len;
    }
    switch (command.kind) {
    case SNAG_UI_SUBMITTED:
    case SNAG_UI_TRANSPORT:
        if (!json_is_integer(data) || json_integer_value(data) < 0 ||
            json_integer_value(data) > UINT_MAX) goto invalid;
        command.data.value = (unsigned int)json_integer_value(data);
        break;
    case SNAG_UI_PUBLIC_BEGIN:
        if (json_array_size(data) != 2u || !unsigned_at(data, 0u, 2u, &values[0]) ||
            !values[0] || !unsigned_at(data, 1u, SNAG_PRESENT_FEEDBACK, &values[1])) goto invalid;
        command.data.public.fd = (int)values[0];
        command.data.public.kind = (enum snag_presentation)values[1];
        break;
    case SNAG_UI_ORIENTATION:
        if (json_array_size(data) != 4u || !unsigned_at(data, 0u, INT64_MAX, &values[0]) ||
            !unsigned_at(data, 1u, SIZE_MAX, &values[1]) ||
            !json_is_boolean(json_array_get(data, 2u)) ||
            !json_is_boolean(json_array_get(data, 3u))) goto invalid;
        command.data.orientation.turns = values[0];
        command.data.orientation.queued = (size_t)values[1];
        command.data.orientation.resumed = json_is_true(json_array_get(data, 2u));
        command.data.orientation.queue_armed = json_is_true(json_array_get(data, 3u));
        break;
    case SNAG_UI_HISTORY: {
        if (json_array_size(data) != 4u || !unsigned_at(data, 0u, INT64_MAX, &values[0]) ||
            !unsigned_at(data, 1u, INT64_MAX, &values[1]) ||
            !unsigned_at(data, 2u, INT64_MAX, &values[2])) goto invalid;
        const json_t *row = json_array_get(data, 3u);
        if (!json_is_null(row)) {
            if (!json_is_object(row)) goto invalid;
            turn.user = (char *)snag_json_string(row, "user");
            turn.assistant = (char *)snag_json_string(row, "assistant");
            turn.status = snag_json_string(row, "status");
            turn.timer = json_is_true(json_object_get(row, "timer"));
            turn.partial = json_is_true(json_object_get(row, "partial"));
            turn.continuation = json_is_true(json_object_get(row, "continuation"));
            command.data.replay.turn = &turn;
        }
        command.data.replay.shown = values[0];
        command.data.replay.completed = values[1];
        command.data.replay.total = values[2];
        break;
    }
    case SNAG_UI_IRC:
        if (snag_irc_event_payload_read(data, &irc) < 0) goto out;
        command.data.irc = &irc;
        break;
    case SNAG_UI_VOICE_EVENT:
        if (!json_is_object(data)) goto invalid;
        command.data.voice = data;
        break;
    case SNAG_UI_DURABLE:
        if (json_array_size(data) != 3u ||
            read_source(json_array_get(data, 0u), &command.data.durable.source) < 0 ||
            !unsigned_at(data, 1u, UINT32_MAX, &values[0]) ||
            !unsigned_at(data, 2u, UINT32_MAX, &values[1])) goto invalid;
        const json_t *response = json_object_get(record, "response");
        if (response && read_source(response, &render->response_source) < 0) goto invalid;
        command.data.durable.fd = journal_fd;
        command.data.durable.timeout_ms = (uint32_t)values[0];
        command.data.durable.max_output_bytes = (uint32_t)values[1];
        break;
    case SNAG_UI_EVENT:
        if (!json_is_integer(data) || json_integer_value(data) < 0) goto invalid;
        command.data.seq = (uint64_t)json_integer_value(data);
        break;
    default:
        break;
    }
    if (snag_string_in(op, "host help runtime error warning submitted orientation durable event") &&
        !command.text) goto invalid;
    if (snag_string_in(op, "submitted orientation protocol") && !command.label) goto invalid;
    struct snag_render_origin previous = render->sink.source;
    if (render->sink.text && command.text && command.kind != SNAG_UI_DURABLE) {
        size_t length = encoded ? command.len : strlen(command.text);
        render->sink.source = (struct snag_render_origin){
            (const unsigned char *)command.text, length, previous.byte, NULL};
        previous.byte += length;
    }
    rc = snag_presentation_apply(render, &command, NULL);
    render->sink.source = previous;
    goto out;
invalid:
    errno = EINVAL;
out:
    snag_buf_free(&bytes);
    return rc;
}

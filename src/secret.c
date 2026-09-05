/* SPDX-License-Identifier: GPL-2.0-only */
#include "secret.h"
#include "base.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

void
snag_secret_set_free(struct snag_secret_set *set)
{
    for (size_t i = 0; i < set->wire.count; ++i)
        snag_secret_bytes_free((char *)set->values[i]);
    memset(set, 0, sizeof(*set));
}

/* Takes ownership even on failure. */
static int
append_secret(struct snag_secret_set *set, char *value)
{
    if (!value)
        return -1;
    for (size_t i = 0; i < set->wire.count; ++i)
        if (strcmp(value, set->values[i]) == 0) {
            snag_secret_bytes_free(value);
            return 0;
        }
    if (set->wire.count >= SNAG_SECRET_VALUES_MAX) {
        snag_secret_bytes_free(value);
        return -1;
    }
    set->values[set->wire.count++] = value;
    return 0;
}

int
snag_secret_set_build(struct snag_secret_set *set,
                     const struct snag_config *config,
                     const struct snag_credential *credential,
                     char *error, size_t error_size)
{
    set->wire.values = set->values;
    if (credential && credential->len &&
        append_secret(set, snag_strdup_checked(credential->value, SNAG_WIRE_SECRET_MAX)) < 0)
        goto failed;
    if (!config)
        return 0;
    for (size_t i = 0; i < config->provider_count; ++i) {
        const struct snag_secret_source *source = &config->providers[i].api_key;
        char *value = NULL;
        if (source->kind == SNAG_SECRET_NONE ||
            snag_secret_source_resolve(source, &value, NULL, 0u) < 0)
            continue; /* Inactive providers do not gate the selected one. */
        if (append_secret(set, value) < 0)
            goto failed;
    }
    for (size_t i = 0; i < config->secret_count; ++i) {
        char *value = NULL;
        if (snag_secret_source_resolve(&config->secrets[i], &value, error, error_size) < 0)
            return -1;
        if (append_secret(set, value) < 0)
            goto failed;
    }
    return 0;
failed:
    snag_errorf(error, error_size, "cannot retain secret protection snapshot");
    return -1;
}

int
snag_secret_result(const struct snag_secret_set *set, json_t *result,
                   char *error, size_t error_size)
{
    json_t *text = json_object_get(result, "model_text");
    json_t *wrapper = NULL, *redacted = NULL;
    struct snag_buf encoded, clean;
    int rc = -1;

    if (!json_is_string(text))
        return 0;
    snag_buf_init(&encoded, SNAG_WIRE_BODY_MAX);
    snag_buf_init(&clean, SNAG_WIRE_BODY_MAX);
    wrapper = json_object();
    if (wrapper && json_object_set_new(wrapper, "text", json_incref(text)) == 0 &&
        snag_json_canonical(wrapper, &encoded) == 0 &&
        snag_wire_json_redact(encoded.data, encoded.len, &set->wire, &clean, error, error_size) == 0)
        redacted = json_loadb((const char *)clean.data, clean.len, JSON_REJECT_DUPLICATES, NULL);
    if (redacted && json_is_string(json_object_get(redacted, "text")))
        rc = json_object_set_new(result, "model_text", json_incref(json_object_get(redacted, "text")));
    if (rc < 0)
        snag_errorf(error, error_size, "cannot redact native tool result safely");
    json_decref(wrapper);
    json_decref(redacted);
    snag_secret_clear(encoded.data, encoded.len);
    snag_buf_free(&encoded);
    snag_buf_free(&clean);
    return rc;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_JANSSON_ABI_DECL_H
#define SNAJPAGENT_JANSSON_ABI_DECL_H

/* First-party declaration shim for building against a system libjansson runtime.
 * This file is not an upstream Jansson header copy and contains no implementation.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_INTEGER,
    JSON_REAL,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL
} json_type;

typedef long long json_int_t;

typedef struct json_t {
    json_type type;
    volatile size_t refcount;
} json_t;

#define json_typeof(json)     ((json)->type)
#define json_is_object(json)  ((json) && json_typeof(json) == JSON_OBJECT)
#define json_is_array(json)   ((json) && json_typeof(json) == JSON_ARRAY)
#define json_is_string(json)  ((json) && json_typeof(json) == JSON_STRING)
#define json_is_integer(json) ((json) && json_typeof(json) == JSON_INTEGER)
#define json_is_real(json)    ((json) && json_typeof(json) == JSON_REAL)
#define json_is_number(json)  (json_is_integer(json) || json_is_real(json))
#define json_is_true(json)    ((json) && json_typeof(json) == JSON_TRUE)
#define json_is_false(json)   ((json) && json_typeof(json) == JSON_FALSE)
#define json_boolean_value    json_is_true
#define json_is_boolean(json) (json_is_true(json) || json_is_false(json))
#define json_is_null(json)    ((json) && json_typeof(json) == JSON_NULL)

#define JSON_ERROR_TEXT_LENGTH   160
#define JSON_ERROR_SOURCE_LENGTH 80

typedef struct json_error_t {
    int line;
    int column;
    int position;
    char source[JSON_ERROR_SOURCE_LENGTH];
    char text[JSON_ERROR_TEXT_LENGTH];
} json_error_t;

json_t *json_object(void);
json_t *json_array(void);
json_t *json_string(const char *value);
json_t *json_stringn(const char *value, size_t len);
json_t *json_integer(json_int_t value);
json_t *json_real(double value);
json_t *json_true(void);
json_t *json_false(void);
json_t *json_null(void);
#define json_boolean(val) ((val) ? json_true() : json_false())

void json_delete(json_t *json);
static inline json_t *json_incref(json_t *json)
{
    if (json && json->refcount != (size_t)-1)
        ++json->refcount;
    return json;
}
static inline void json_decref(json_t *json)
{
    if (json && json->refcount != (size_t)-1 && --json->refcount == 0)
        json_delete(json);
}

size_t json_object_size(const json_t *object);
json_t *json_object_get(const json_t *object, const char *key);
json_t *json_object_getn(const json_t *object, const char *key, size_t key_len);
int json_object_set_new(json_t *object, const char *key, json_t *value);
int json_object_setn_new(json_t *object, const char *key, size_t key_len, json_t *value);
int json_object_del(json_t *object, const char *key);
void *json_object_iter(json_t *object);
void *json_object_iter_next(json_t *object, void *iter);
const char *json_object_iter_key(void *iter);
size_t json_object_iter_key_len(void *iter);
json_t *json_object_iter_value(void *iter);

static inline int json_object_set(json_t *object, const char *key, json_t *value)
{
    return json_object_set_new(object, key, json_incref(value));
}
static inline int json_object_setn(json_t *object, const char *key,
                                   size_t key_len, json_t *value)
{
    return json_object_setn_new(object, key, key_len, json_incref(value));
}

size_t json_array_size(const json_t *array);
json_t *json_array_get(const json_t *array, size_t index);
int json_array_append_new(json_t *array, json_t *value);
int json_array_set_new(json_t *array, size_t index, json_t *value);
int json_array_remove(json_t *array, size_t index);
int json_array_clear(json_t *array);
static inline int json_array_append(json_t *array, json_t *value)
{
    return json_array_append_new(array, json_incref(value));
}

const char *json_string_value(const json_t *string);
size_t json_string_length(const json_t *string);
json_int_t json_integer_value(const json_t *integer);
double json_real_value(const json_t *real);

int json_equal(const json_t *value1, const json_t *value2);
json_t *json_deep_copy(const json_t *value);

#define JSON_REJECT_DUPLICATES  0x1
#define JSON_DISABLE_EOF_CHECK  0x2
#define JSON_DECODE_ANY         0x4
#define JSON_DECODE_INT_AS_REAL 0x8
#define JSON_ALLOW_NUL          0x10

json_t *json_loadb(const char *buffer, size_t buflen, size_t flags,
                   json_error_t *error);

#ifdef __cplusplus
}
#endif

#endif

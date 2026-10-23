/* SPDX-License-Identifier: GPL-2.0-only */
#include "skills.h"
#include "base.h"
#include "fs.h"
#include "json.h"
#include "instructions.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yaml.h>

struct yaml_data {
    yaml_document_t *document;
    unsigned char *visiting;
    size_t bytes;
    char *error;
    size_t size;
};

bool
snag_skill_bindings_valid(const json_t *active)
{
    if (!json_is_array(active)) return false;
    for (size_t i = 0; i < json_array_size(active); ++i) {
        const json_t *entry = json_array_get(active, i);
        const char *fields[] = {"id", "name", "description", "body", "arguments",
            "instructions", "root", "digest", "turn_id", "goal_id"};
        for (size_t j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j) {
            if (!snag_json_string(entry, fields[j])) return false;
        }
        const char *visit = snag_json_string(entry, "visit_id");
        if (json_object_get(entry, "visit_id") && (!visit ||
            strlen(visit) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(visit, SNAG_ID_HEX_LEN)))
            return false;
        const char *manifest = snag_json_string(entry, "manifest_digest");
        if (json_object_get(entry, "manifest_digest") && (!manifest ||
            strlen(manifest) != SNAG_SHA256_HEX_LEN ||
            !snag_hex_is_lower(manifest, SNAG_SHA256_HEX_LEN))) return false;
        const char *digest = snag_json_string(entry, "digest");
        const char *turn = snag_json_string(entry, "turn_id");
        const char *goal = snag_json_string(entry, "goal_id");
        if (!*snag_json_string(entry, "id") || !*snag_json_string(entry, "name") ||
            !snag_path_root_len(snag_json_string(entry, "root")) ||
            strlen(digest) != SNAG_SHA256_HEX_LEN ||
            !snag_hex_is_lower(digest, SNAG_SHA256_HEX_LEN) ||
            strlen(turn) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(turn, SNAG_ID_HEX_LEN) ||
            (*goal && (strlen(goal) != SNAG_ID_HEX_LEN ||
                !snag_hex_is_lower(goal, SNAG_ID_HEX_LEN)))) return false;
    }
    return true;
}

static json_t *
yaml_value(struct yaml_data *data, int index, size_t depth)
{
    yaml_node_t *node = yaml_document_get_node(data->document, index);
    json_t *value = NULL;
    if (!node || depth > 48u || data->bytes > SNAG_MAX_EVENT_LINE) {
        (void)snag_errorf(data->error, data->size, "YAML exceeds JSON record bounds");
        return NULL;
    }
    if (data->visiting[index]) {
        (void)snag_errorf(data->error, data->size, "cyclic YAML alias");
        return NULL;
    }
    const char *tag = (const char *)node->tag;
    if (strcmp(tag, YAML_STR_TAG) && strcmp(tag, YAML_BOOL_TAG) &&
        strcmp(tag, YAML_NULL_TAG) && strcmp(tag, YAML_INT_TAG) &&
        strcmp(tag, YAML_FLOAT_TAG) && strcmp(tag, YAML_SEQ_TAG) &&
        strcmp(tag, YAML_MAP_TAG)) {
        (void)snag_errorf(data->error, data->size, "unsafe or unsupported YAML tag: %s", tag);
        return NULL;
    }
    data->visiting[index] = 1;
    data->bytes += 8u;
    if (node->type == YAML_SCALAR_NODE) {
        size_t length = node->data.scalar.length;
        data->bytes += length;
        if (memchr(node->data.scalar.value, 0, length)) {
            (void)snag_errorf(data->error, data->size, "NUL in YAML scalar");
        } else if (!strcmp(tag, YAML_BOOL_TAG)) {
            const char *text = (const char *)node->data.scalar.value;
            if (!strcmp(text, "true")) value = json_true();
            else if (!strcmp(text, "false")) value = json_false();
            else (void)snag_errorf(data->error, data->size, "invalid YAML boolean");
        } else if (!strcmp(tag, YAML_NULL_TAG)) {
            value = json_null();
        } else {
            value = json_stringn((const char *)node->data.scalar.value, length);
        }
    } else if (node->type == YAML_SEQUENCE_NODE) {
        value = json_array();
        for (yaml_node_item_t *item = node->data.sequence.items.start;
             value && item < node->data.sequence.items.top; ++item) {
            json_t *child = yaml_value(data, *item, depth + 1u);
            if (!child || json_array_append_new(value, child) < 0) {
                json_decref(value);
                value = NULL;
            }
        }
    } else if (node->type == YAML_MAPPING_NODE) {
        value = json_object();
        for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
             value && pair < node->data.mapping.pairs.top; ++pair) {
            yaml_node_t *key = yaml_document_get_node(data->document, pair->key);
            if (!key || key->type != YAML_SCALAR_NODE ||
                memchr(key->data.scalar.value, 0, key->data.scalar.length)) {
                (void)snag_errorf(data->error, data->size, "YAML mapping requires string keys");
                json_decref(value);
                value = NULL;
                break;
            }
            const char *name = (const char *)key->data.scalar.value;
            if (json_object_get(value, name)) {
                (void)snag_errorf(data->error, data->size, "duplicate YAML key: %s", name);
                json_decref(value);
                value = NULL;
                break;
            }
            json_t *child = yaml_value(data, pair->value, depth + 1u);
            if (!child || json_object_set_new(value, name, child) < 0) {
                json_decref(value);
                value = NULL;
            }
        }
    }
    data->visiting[index] = 0;
    return value;
}

static json_t *
parse_yaml(const unsigned char *text, size_t length, char *error, size_t size)
{
    yaml_parser_t parser;
    yaml_document_t document;
    yaml_document_t tail;
    json_t *value = NULL;
    if (!yaml_parser_initialize(&parser)) return NULL;
    yaml_parser_set_input_string(&parser, text, length);
    if (!yaml_parser_load(&parser, &document)) {
        (void)snag_errorf(error, size, "YAML line %zu: %s",
            parser.problem_mark.line + 1u, parser.problem ? parser.problem : "parse failure");
        goto done;
    }
    size_t nodes = (size_t)(document.nodes.top - document.nodes.start);
    struct yaml_data data = {.document = &document, .error = error, .size = size};
    data.visiting = calloc(nodes + 1u, 1u);
    if (data.visiting && yaml_document_get_root_node(&document)) {
        value = yaml_value(&data, 1, 0u);
    }
    free(data.visiting);
    yaml_document_delete(&document);
    if (value) {
        if (!yaml_parser_load(&parser, &tail)) {
            (void)snag_errorf(error, size, "invalid YAML trailing document");
            json_decref(value);
            value = NULL;
        } else {
            if (yaml_document_get_root_node(&tail)) {
                (void)snag_errorf(error, size, "multiple YAML documents");
                json_decref(value);
                value = NULL;
            }
            yaml_document_delete(&tail);
        }
    }
done:
    yaml_parser_delete(&parser);
    return value;
}

static int
read_descriptor(int fd, const char *path, struct snag_buf *buffer, char *error, size_t size)
{
    if (fd < 0) return snag_errorf(error, size, "%s: %s", path, strerror(errno));
    snag_file_info before;
    snag_file_info after;
    int rc = snag_fstat(fd, &before);
    if (rc == 0 && !S_ISREG(before.st_mode)) rc = snag_errno(EINVAL);
    if (rc == 0) rc = snag_buf_read(buffer, fd);
    if (rc == 0) rc = snag_fstat(fd, &after);
    if (rc == 0 && !snag_file_unchanged(&before, &after)) rc = snag_errno(ESTALE);
    int saved = errno;
    close(fd);
    errno = saved;
    if (rc < 0 || snag_buf_terminate(buffer) < 0) {
        return snag_errorf(error, size, "%s: %s", path, strerror(errno));
    }
    if (memchr(buffer->data, 0, buffer->len)) {
        return snag_errorf(error, size, "%s: NUL in text", path);
    }
    return 0;
}

static int
read_text(const char *path, struct snag_buf *buffer, char *error, size_t size)
{
    return read_descriptor(snag_open_inspect_path(NULL, path), path, buffer, error, size);
}

static size_t
text_characters(const char *text)
{
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xc0u) != 0x80u) ++count;
    }
    return count;
}

static bool
path_below(const char *path, const char *root)
{
    size_t length = strlen(root);
    return !strncmp(path, root, length) &&
        (!path[length] || root[length - 1u] == '/' || path[length] == '/');
}

static int
instruction_origin(char **path, const char *root, char *error, size_t size)
{
    char *canonical = snag_realpath(*path);
    if (!canonical || !path_below(canonical, root)) {
        free(canonical);
        return snag_errorf(error, size, "instruction origin must remain inside its skill package");
    }
    free(*path);
    *path = canonical;
    return 0;
}

static int
boolean_field(const json_t *object, const char *key, char *error, size_t size)
{
    json_t *value = json_object_get(object, key);
    const char *text = json_string_value(value);
    return !value || json_is_boolean(value) ||
        (text && (!strcmp(text, "true") || !strcmp(text, "false"))) ? 0 :
        snag_errorf(error, size, "%s must be true or false", key);
}

bool
snag_skill_name_valid(const char *name)
{
    size_t length = strlen(name);
    if (!length || length > 64u || name[0] == '-' || name[length - 1u] == '-') return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') && ch != '-') {
            return false;
        }
        if (ch == '-' && i && name[i - 1u] == '-') return false;
    }
    return true;
}

static bool
boolean_value(const json_t *object, const char *key, bool fallback)
{
    json_t *value = json_object_get(object, key);
    if (json_is_boolean(value)) return json_is_true(value);
    const char *text = json_string_value(value);
    if (text && !strcmp(text, "true")) return true;
    if (text && !strcmp(text, "false")) return false;
    return fallback;
}

static bool array_has_text(const json_t *, const char *);
static int string_compare(const void *, const void *);

static int
manifest_walk(const char *path, const char *root, const char *relative, json_t *ancestors,
    json_t *entries, char *error, size_t size)
{
    char *canonical = snag_realpath(path);
    if (!canonical || !path_below(canonical, root) || array_has_text(ancestors, canonical)) {
        free(canonical);
        return snag_errorf(error, size, "package resource escapes, cycles or is absent");
    }
    snag_file_info kind;
    if (snag_lstat(canonical, &kind) < 0 ||
        (!S_ISDIR(kind.st_mode) && !S_ISREG(kind.st_mode))) {
        free(canonical);
        return snag_errorf(error, size, "special files are unsupported in skill packages");
    }
    int fd = snag_open_inspect_path(NULL, canonical);
    snag_file_info before;
    snag_file_info after;
    int rc = -1;
    if (fd < 0 || snag_fstat(fd, &before) < 0) goto done;
    if (S_ISREG(before.st_mode)) {
        struct snag_sha256 hash;
        char digest[SNAG_SHA256_HEX_LEN + 1u];
        unsigned char buffer[65536];
        ssize_t amount;
        snag_sha256_init(&hash);
        while ((amount = read(fd, buffer, sizeof(buffer))) != 0) {
            if (amount < 0 && errno == EINTR) continue;
            if (amount < 0) goto done;
            snag_sha256_update(&hash, buffer, (size_t)amount);
        }
        snag_sha256_final_hex(&hash, digest);
        if (snag_fstat(fd, &after) < 0 || !snag_file_unchanged(&before, &after)) goto done;
        rc = json_array_append_new(entries, json_pack("{s:s,s:s,s:b,s:i,s:I,s:s}",
            "path", relative, "kind", "file", "executable", (before.st_mode & 0111) != 0,
            "executable_bits", (int)(before.st_mode & 0111),
            "size", (json_int_t)before.st_size, "sha256", digest));
    } else if (S_ISDIR(before.st_mode)) {
        if (json_array_append_new(entries, json_pack("{s:s,s:s}",
            "path", relative, "kind", "directory")) < 0 ||
            json_array_append_new(ancestors, json_string(canonical)) < 0) goto done;
        struct snag_directory *directory = snag_directory_open(fd);
        if (!directory) goto done;
        json_t *names = json_array();
        const char *name;
        rc = names ? 0 : -1;
        while (rc == 0 && (name = snag_directory_next(directory))) {
            if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git")) continue;
            rc = json_array_append_new(names, json_string(name));
        }
        if (rc == 0 && errno) rc = -1;
        size_t count = json_array_size(names);
        const char **sorted = calloc(count ? count : 1u, sizeof(*sorted));
        if (!sorted) rc = -1;
        for (size_t i = 0; sorted && i < count; ++i) {
            sorted[i] = json_string_value(json_array_get(names, i));
        }
        if (sorted) qsort(sorted, count, sizeof(*sorted), string_compare);
        for (size_t i = 0; rc == 0 && i < count; ++i) {
            char *next = snag_path_join(canonical, sorted[i]);
            char *child = *relative ? snag_path_join(relative, sorted[i]) : strdup(sorted[i]);
            rc = next && child ? manifest_walk(next, root, child, ancestors,
                entries, error, size) : -1;
            free(next);
            free(child);
        }
        free(sorted);
        json_decref(names);
        (void)json_array_remove(ancestors, json_array_size(ancestors) - 1u);
        if (rc == 0 && (snag_fstat(fd, &after) < 0 ||
            !snag_file_unchanged(&before, &after))) rc = -1;
        if (snag_directory_close(directory) < 0) rc = -1;
        fd = -1;
    }
done:
    if (fd >= 0) close(fd);
    free(canonical);
    if (rc < 0 && !*error) {
        (void)snag_errorf(error, size,
            "package contains special files or changed during manifest read");
    }
    return rc;
}

json_t *
snag_skill_manifest(const char *directory, char *error, size_t size)
{
    char *root = snag_realpath(directory);
    json_t *entries = json_array();
    json_t *ancestors = json_array();
    json_t *manifest = NULL;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    if (root && entries && ancestors &&
        manifest_walk(root, root, "", ancestors, entries, error, size) == 0 &&
        snag_json_digest_bounded(entries, SNAG_MAX_EVENT_LINE, digest, NULL) == 0) {
        manifest = json_pack("{s:i,s:s,s:O}", "version", 1, "digest", digest, "entries", entries);
    }
    free(root);
    json_decref(entries);
    json_decref(ancestors);
    return manifest;
}

static int
resource_reference(const char *root, const char *start, size_t length, bool local,
    char *error, size_t size)
{
    char *reference = malloc(length + 1u);
    if (!reference) return -1;
    memcpy(reference, start, length);
    reference[length] = '\0';
    char *suffix = strpbrk(reference, "#?");
    if (suffix) *suffix = 0;
    if (!*reference || (!local && strchr(reference, ':'))) { free(reference); return 0; }
    char *path = !snag_path_root_len(reference) ? snag_path_join(root, reference) : NULL;
    char *canonical = path ? snag_realpath(path) : NULL;
    int rc = canonical && path_below(canonical, root) ? 0 :
        snag_errorf(error, size, "missing or escaping package resource: %s", reference);
    free(canonical);
    free(path);
    free(reference);
    return rc;
}

/* Validate inline Markdown destinations outside fenced/inline code. Examples in
 * code blocks remain examples; external links are never fetched by the validator. */
static int
resource_references(const char *root, const char *body, const json_t *vendor,
    char *error, size_t size)
{
    const char *icons[] = {"icon_small", "icon_large"};
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
        const char *path = snag_json_string(json_object_get(vendor, "interface"), icons[i]);
        if (path && resource_reference(root, path, strlen(path), true, error, size) < 0) return -1;
    }
    char fence = 0;
    size_t fence_width = 0;
    size_t code = 0;
    const char *cursor = body;
    while (*cursor) {
        if (cursor == body || cursor[-1] == '\n') {
            const char *line = cursor;
            while (*line == ' ' && line - cursor < 3) ++line;
            size_t width = 0;
            if (*line == '`' || *line == '~') {
                while (line[width] == *line) ++width;
            }
            if (width >= 3u && (!fence || (fence == *line && width >= fence_width))) {
                fence = fence ? 0 : *line;
                fence_width = width;
                code = 0;
                const char *end = strchr(line, '\n');
                cursor = end ? end + 1u : line + strlen(line);
                continue;
            }
        }
        if (fence) { ++cursor; continue; }
        if (*cursor == '\\' && cursor[1]) { cursor += 2u; continue; }
        if (*cursor == '`') {
            size_t width = 1u;
            while (cursor[width] == '`') ++width;
            if (!code) code = width;
            else if (code == width) code = 0;
            cursor += width;
            continue;
        }
        if (!code && cursor[0] == ']' && cursor[1] == '(') {
            const char *start = cursor + 2u;
            while (*start == ' ' || *start == '\t') ++start;
            bool angle = *start == '<';
            if (angle) ++start;
            const char *end = start;
            size_t depth = 0;
            while (*end && *end != '\n') {
                if (angle ? *end == '>' : (!depth && (*end == ')' || isspace((unsigned char)*end))))
                    break;
                if (!angle && *end == '(') ++depth;
                else if (!angle && *end == ')' && depth) --depth;
                ++end;
            }
            if (resource_reference(root, start, (size_t)(end - start), false, error, size) < 0)
                return -1;
            cursor = end;
        } else ++cursor;
    }
    return 0;
}

json_t *
snag_skill_load(const char *directory, bool strict, char *error, size_t size)
{
    struct snag_buf text = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf auxiliary = {.max = SNAG_MAX_EVENT_LINE};
    char *root = snag_realpath(directory);
    char *path = root ? snag_path_join(root, "SKILL.md") : NULL;
    char *metadata_path = root ? snag_path_join(root, "agents/openai.yaml") : NULL;
    json_t *result = NULL;
    json_t *metadata = NULL;
    json_t *vendor = NULL;
    if (!root || !path || !metadata_path) goto done;
    if (instruction_origin(&path, root, error, size) < 0 ||
        read_text(path, &text, error, size) < 0) goto done;
    const char *source = (const char *)text.data;
    const char *yaml_start = NULL;
    if (!strncmp(source, "---\n", 4u)) yaml_start = source + 4u;
    else if (!strncmp(source, "---\r\n", 5u)) yaml_start = source + 5u;
    if (!yaml_start) {
        (void)snag_errorf(error, size, "SKILL.md requires YAML frontmatter");
        goto done;
    }
    const char *end = yaml_start;
    const char *body = NULL;
    while (*end) {
        const char *next = strchr(end, '\n');
        size_t width = next ? (size_t)(next - end) : strlen(end);
        if (width && end[width - 1u] == '\r') --width;
        if (width == 3u && !memcmp(end, "---", 3u)) {
            body = next ? next + 1u : end + width;
            break;
        }
        if (!next) break;
        end = next + 1u;
    }
    if (!body) {
        (void)snag_errorf(error, size, "unterminated YAML frontmatter");
        goto done;
    }
    metadata = parse_yaml((const unsigned char *)yaml_start,
        (size_t)(end - yaml_start), error, size);
    if (!json_is_object(metadata)) goto done;
    const char *name = snag_json_string(metadata, "name");
    const char *description = snag_json_string(metadata, "description");
    if (!name || !*name || !description || !*description) {
        (void)snag_errorf(error, size, "name and description must be nonempty strings");
        goto done;
    }
    const char *base = strrchr(root, '/');
    base = base ? base + 1u : root;
    bool naming = snag_skill_name_valid(name) && !strcmp(name, base);
    if (strict && (!naming || text_characters(description) > 1024u)) {
        (void)snag_errorf(error, size,
            "invalid standard skill name/directory or description length");
        goto done;
    }
    const char *optional[] = {"license", "compatibility", "argument-hint", "allowed-tools"};
    for (size_t i = 0; i < sizeof(optional) / sizeof(optional[0]); ++i) {
        json_t *field = json_object_get(metadata, optional[i]);
        if (field && !json_is_string(field)) {
            (void)snag_errorf(error, size, "%s must be text", optional[i]);
            goto done;
        }
    }
    json_t *extra = json_object_get(metadata, "metadata");
    if (extra && !json_is_object(extra)) {
        (void)snag_errorf(error, size, "metadata must be a mapping");
        goto done;
    }
    const char *extra_key;
    json_t *extra_value;
    json_object_foreach(extra, extra_key, extra_value) {
        if (!json_is_string(extra_value)) {
            (void)snag_errorf(error, size, "metadata.%s must be text", extra_key);
            goto done;
        }
    }
    const char *compatibility = snag_json_string(metadata, "compatibility");
    if (strict && compatibility && text_characters(compatibility) > 500u) {
        (void)snag_errorf(error, size, "compatibility exceeds 500 characters");
        goto done;
    }
    if (boolean_field(metadata, "disable-model-invocation", error, size) < 0 ||
        boolean_field(metadata, "user-invocable", error, size) < 0) goto done;
    snag_file_info auxiliary_info;
    if (snag_lstat(metadata_path, &auxiliary_info) == 0) {
        if (instruction_origin(&metadata_path, root, error, size) < 0 ||
            read_text(metadata_path, &auxiliary, error, size) < 0) goto done;
        vendor = parse_yaml(auxiliary.data, auxiliary.len, error, size);
        if (!json_is_object(vendor)) goto done;
        const char *mappings[] = {"interface", "policy", "dependencies"};
        for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); ++i) {
            json_t *value = json_object_get(vendor, mappings[i]);
            if (value && !json_is_object(value)) {
                (void)snag_errorf(error, size, "openai.yaml %s must be a mapping", mappings[i]);
                goto done;
            }
        }
        json_t *interface = json_object_get(vendor, "interface");
        json_object_foreach(interface, extra_key, extra_value) {
            if (!json_is_string(extra_value)) {
                (void)snag_errorf(error, size, "openai.yaml interface.%s must be text", extra_key);
                goto done;
            }
        }
        if (boolean_field(json_object_get(vendor, "policy"),
            "allow_implicit_invocation", error, size) < 0) goto done;
        json_t *tools = json_object_get(json_object_get(vendor, "dependencies"), "tools");
        if (tools && !json_is_array(tools)) {
            (void)snag_errorf(error, size, "openai.yaml dependencies.tools must be a sequence");
            goto done;
        }
        for (size_t i = 0; i < json_array_size(tools); ++i) {
            json_t *tool = json_array_get(tools, i);
            if (!json_is_object(tool) || !snag_json_string(tool, "type") ||
                !snag_json_string(tool, "value")) {
                (void)snag_errorf(error, size, "dependency requires text type and value");
                goto done;
            }
            json_object_foreach(tool, extra_key, extra_value) {
                if (!json_is_string(extra_value)) {
                    (void)snag_errorf(error, size, "dependency.%s must be text", extra_key);
                    goto done;
                }
            }
        }
    } else if (errno != ENOENT) {
        (void)snag_errorf(error, size, "%s: %s", metadata_path, strerror(errno));
        goto done;
    }
    if (strict) {
        if (resource_references(root, body, vendor, error, size) < 0) goto done;
        json_t *manifest = snag_skill_manifest(root, error, size);
        if (!manifest) goto done;
        json_decref(manifest);
    }
    bool implicit = !boolean_value(metadata, "disable-model-invocation", false) &&
        boolean_value(json_object_get(vendor, "policy"), "allow_implicit_invocation", true);
    bool invocable = boolean_value(metadata, "user-invocable", true);
    bool unsupported = json_object_get(metadata, "hooks") ||
        json_object_get(metadata, "model") || json_object_get(metadata, "context") ||
        json_object_get(metadata, "agent") || strstr(body, "!`");
    struct snag_sha256 hash;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, text.data, text.len);
    snag_sha256_update(&hash, "\0openai.yaml\0", 13u);
    snag_sha256_update(&hash, auxiliary.data, auxiliary.len);
    snag_sha256_final_hex(&hash, digest);
    result = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:O,s:O,s:b,s:b,s:b,s:b}",
        "name", name, "description", description, "root", root, "path", path,
        "digest", digest, "body", body, "metadata", metadata,
        "openai", vendor ? vendor : json_null(), "implicit", implicit,
        "user_invocable", invocable, "unsupported_execution", unsupported,
        "naming_warning", !naming);

done:
    json_decref(metadata);
    json_decref(vendor);
    free(root);
    free(path);
    free(metadata_path);
    snag_buf_free(&text);
    snag_buf_free(&auxiliary);
    return result;
}

json_t *
snag_skill_arguments(const char *text, char *error, size_t size)
{
    json_t *words = json_array();
    struct snag_buf word = {.max = SNAG_MAX_EVENT_LINE};
    while (words && *text) {
        while (isspace((unsigned char)*text)) ++text;
        if (!*text) break;
        char quote = 0;
        word.len = 0;
        while (*text && (quote || !isspace((unsigned char)*text))) {
            char ch = *text++;
            if (ch == '\\' && quote != '\'' && *text) ch = *text++;
            else if (ch == quote) { quote = 0; continue; }
            else if (!quote && (ch == '\'' || ch == '"')) { quote = ch; continue; }
            if (snag_buf_append(&word, &ch, 1u) < 0) goto fail;
        }
        if (quote) {
            (void)snag_errorf(error, size, "unterminated quote in skill arguments");
            goto fail;
        }
        if (json_array_append_new(words,
            json_stringn((const char *)word.data, word.len)) < 0) goto fail;
    }
    snag_buf_free(&word);
    return words;
fail:
    snag_buf_free(&word);
    json_decref(words);
    return NULL;
}

json_t *
snag_skill_invocation(const json_t *skill, const char *arguments, char *error, size_t size)
{
    struct snag_buf rendered = {.max = SNAG_MAX_EVENT_LINE};
    const char *body = snag_json_string(skill, "body");
    const char *root = snag_json_string(skill, "root");
    json_t *words = NULL;
    json_t *result = NULL;
    if (!body || !root) return NULL;
    if (!arguments) arguments = "";
    words = snag_skill_arguments(arguments, error, size);
    if (!words) goto done;
    for (size_t i = 0; body[i];) {
        const char *replacement = NULL;
        size_t consumed = 0;
        if (!strncmp(body + i, "${CLAUDE_SKILL_DIR}", 19u)) {
            replacement = root;
            consumed = 19u;
        } else if (!strncmp(body + i, "$ARGUMENTS", 10u)) {
            replacement = arguments;
            consumed = 10u;
        }
        size_t number_start = i + 1u;
        bool bracket = consumed == 10u && body[i + consumed] == '[';
        if (bracket) number_start = i + consumed + 1u;
        if (body[i] == '$' && (!consumed || bracket) &&
            isdigit((unsigned char)body[number_start])) {
            char *end;
            errno = 0;
            unsigned long number = strtoul(body + number_start, &end, 10);
            if (!errno && (!bracket || *end == ']')) {
                replacement = json_string_value(json_array_get(words, (size_t)number));
                if (!replacement) replacement = "";
                consumed = (size_t)(end - body) - i + (bracket ? 1u : 0u);
            }
        }
        if (replacement) {
            if (snag_buf_append(&rendered, replacement, strlen(replacement)) < 0) goto done;
            i += consumed;
        } else {
            if (snag_buf_append(&rendered, body + i, 1u) < 0) goto done;
            ++i;
        }
    }
    if (snag_buf_terminate(&rendered) < 0) goto done;
    /* Invocation edits only top-level fields; package metadata stays immutable. */
    result = json_copy((json_t *)skill);
    if (result && (json_object_set_new(result, "arguments", json_string(arguments)) < 0 ||
        json_object_set_new(result, "instructions", json_string((char *)rendered.data)) < 0)) {
        json_decref(result);
        result = NULL;
    }
done:
    json_decref(words);
    snag_buf_free(&rendered);
    return result;
}

static bool
array_has_text(const json_t *array, const char *text)
{
    for (size_t i = 0; i < json_array_size(array); ++i) {
        const char *item = json_string_value(json_array_get(array, i));
        if (item && !strcmp(item, text)) return true;
    }
    return false;
}

static int
catalog_diagnostic(json_t *catalog, const char *path, const char *message)
{
    return json_array_append_new(json_object_get(catalog, "diagnostics"),
        json_pack("{s:s,s:s}", "path", path, "message", message));
}

static int
string_compare(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

struct skill_scan {
    json_t *catalog;
    json_t *visited;
    const json_t *settings;
    const char *origin;
    size_t priority;
};

static int
scan_directory(struct skill_scan *scan, const char *path, char *error, size_t size)
{
    char *canonical = snag_realpath(path);
    if (!canonical) {
        if (errno == ENOENT || errno == ENOTDIR) return 0;
        return catalog_diagnostic(scan->catalog, path, strerror(errno));
    }
    if (!path_below(canonical, scan->origin) &&
        json_array_append_new(json_object_get(scan->catalog, "linked_origins"),
            json_pack("{s:s,s:s,s:s,s:b}", "path", path, "canonical", canonical,
                "origin", scan->origin, "present", true)) < 0) {
        free(canonical);
        return -1;
    }
    if (array_has_text(scan->visited, canonical)) { free(canonical); return 0; }
    int rc = json_array_append_new(scan->visited, json_string(canonical));
    char *skill_path = snag_path_join(canonical, "SKILL.md");
    if (rc < 0 || !skill_path) { free(canonical); free(skill_path); return -1; }
    snag_file_info info;
    if (snag_stat(skill_path, &info) == 0) {
        json_t *skill = snag_skill_load(canonical, false, error, size);
        if (!skill) {
            rc = catalog_diagnostic(scan->catalog, canonical, error);
        } else {
            char digest[SNAG_SHA256_HEX_LEN + 1u];
            snag_sha256_hex(canonical, strlen(canonical), digest);
            struct snag_buf id = {.max = SNAG_PATH_MAX_BYTES};
            const char *name = snag_json_string(skill, "name");
            const char *managed_id = NULL;
            const char *managed_name;
            json_t *receipt;
            json_t *installed = json_object_get(scan->settings, "installed");
            json_object_foreach(installed, managed_name, receipt) {
                (void)managed_name;
                const char *installed_root = snag_json_string(receipt, "root");
                if (installed_root && !strcmp(installed_root, canonical)) {
                    managed_id = snag_json_string(receipt, "id");
                    if (json_object_set(skill, "provenance", receipt) < 0) rc = -1;
                    break;
                }
            }
            if ((managed_id ? snag_buf_append(&id, managed_id, strlen(managed_id)) :
                snag_buf_printf(&id, "%s@%.16s", name, digest)) < 0 ||
                snag_buf_terminate(&id) < 0 ||
                json_object_set_new(skill, "id", json_string((char *)id.data)) < 0 ||
                json_object_set_new(skill, "origin", json_string(scan->origin)) < 0 ||
                json_object_set_new(skill, "priority",
                    json_integer((json_int_t)scan->priority)) < 0 ||
                json_object_set_new(skill, "state", json_string("available")) < 0) {
                rc = -1;
            } else {
                json_object_del(skill, "body");
                json_object_del(skill, "metadata");
                rc = json_array_append(json_object_get(scan->catalog, "entries"), skill);
            }
            snag_buf_free(&id);
            json_decref(skill);
        }
        free(canonical);
        free(skill_path);
        return rc;
    }
    free(skill_path);
    int fd = snag_open_inspect_path(NULL, canonical);
    struct snag_directory *directory = fd >= 0 ? snag_directory_open(fd) : NULL;
    if (!directory) {
        if (fd >= 0) close(fd);
        rc = catalog_diagnostic(scan->catalog, canonical, strerror(errno));
        free(canonical);
        return rc;
    }
    char **names = NULL;
    size_t count = 0;
    const char *name;
    while ((name = snag_directory_next(directory))) {
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git") ||
            !strcmp(name, ".store")) continue;
        char **grown = realloc(names, (count + 1u) * sizeof(*names));
        if (!grown) { rc = -1; break; }
        names = grown;
        names[count] = strdup(name);
        if (!names[count]) { rc = -1; break; }
        ++count;
    }
    int saved = errno;
    if (snag_directory_close(directory) < 0 && rc == 0) rc = -1;
    if (saved && rc == 0) rc = catalog_diagnostic(scan->catalog, canonical, strerror(saved));
    qsort(names, count, sizeof(*names), string_compare);
    for (size_t i = 0; i < count; ++i) {
        char *child = snag_path_join(canonical, names[i]);
        if (rc == 0 && child && snag_stat(child, &info) == 0 && S_ISDIR(info.st_mode)) {
            rc = scan_directory(scan, child, error, size);
        }
        free(child);
        free(names[i]);
    }
    free(names);
    free(canonical);
    return rc;
}

static int
scan_root(struct skill_scan *scan, const char *path, const char *configured, bool defaults,
    char *error, size_t size)
{
    char *canonical = snag_realpath(path);
    int code = canonical ? 0 : errno;
    if (!canonical && !configured) return code == ENOENT || code == ENOTDIR ? 0 :
        catalog_diagnostic(scan->catalog, path, strerror(code));
    json_t *item = json_pack("{s:s,s:s,s:b}", "path", path,
        "canonical", canonical ? canonical : "", "present", canonical != NULL);
    if (item && configured &&
        (json_object_set_new(item, "configured", json_string(configured)) < 0 ||
            json_object_set_new(item, "default", json_boolean(defaults)) < 0)) {
        json_decref(item);
        item = NULL;
    }
    if (item && code && json_object_set_new(item, "diagnostic",
        json_string(strerror(code))) < 0) {
        json_decref(item);
        item = NULL;
    }
    int rc = item ? json_array_append_new(json_object_get(scan->catalog, "roots"), item) : -1;
    if (rc == 0 && canonical) {
        scan->origin = canonical;
        rc = scan_directory(scan, canonical, error, size);
    }
    ++scan->priority;
    free(canonical);
    return rc;
}

static int
project_skills(struct skill_scan *scan, const char *cwd, char *error, size_t size)
{
    char *current = cwd ? snag_realpath(cwd) : NULL;
    char *root = NULL;
    if (!current) return 0; /* A disappeared cwd retains the global catalog/recovery tools. */
    snag_file_info info;
    if (snag_stat(current, &info) < 0) {
        int saved = errno;
        free(current);
        if (saved == ENOENT || saved == ENOTDIR) return 0;
        return snag_fail(error, size, saved, "cannot inspect project skill directory");
    }
    if (!S_ISDIR(info.st_mode)) {
        free(current);
        return 0;
    }
    int rc = snag_instructions_project_root(current, &root, error, size);
    while (rc == 0) {
        const char *names[] = {".snajpagent/skills", ".agents/skills", ".claude/skills"};
        for (size_t i = 0; rc == 0 && i < sizeof(names) / sizeof(names[0]); ++i) {
            char *path = snag_path_join(current, names[i]);
            rc = path ? scan_root(scan, path, NULL, false, error, size) : -1;
            free(path);
        }
        if (rc < 0 || !strcmp(current, root)) break;
        char *slash = strrchr(current, '/');
        if (!slash || slash == current) current[1] = '\0';
        else *slash = '\0';
    }
    free(root);
    free(current);
    return rc;
}

json_t *
snag_skills_discover(const char *cwd, const char *dotdir, const json_t *settings,
    char *error, size_t size)
{
    json_t *catalog = json_pack("{s:[],s:[],s:[],s:[]}",
        "entries", "diagnostics", "linked_origins", "roots");
    struct skill_scan scan = {.catalog = catalog, .settings = settings,
        .visited = json_array()};
    if (!catalog || !scan.visited) goto fail;
    const char *visit_root = snag_json_string(settings, "visit_root");
    if (visit_root && (!cwd || strcmp(cwd, visit_root)) &&
        project_skills(&scan, visit_root, error, size) < 0) goto fail;
    if (project_skills(&scan, cwd, error, size) < 0) goto fail;
    json_t *roots = json_object_get(settings, "roots");
    bool defaults = !roots;
    for (size_t i = 0; i < (defaults ? 1u : json_array_size(roots)); ++i) {
        const char *configured = defaults ? "skills" : json_string_value(json_array_get(roots, i));
        char *path = configured && snag_path_root_len(configured) ? strdup(configured) :
            configured ? snag_path_join(dotdir, configured) : NULL;
        char *canonical = path ? snag_realpath(path) : NULL;
        if (!path) { free(canonical); goto fail; }
        int rc = scan_root(&scan, path, configured, defaults, error, size);
        const char *name;
        json_t *receipt;
        json_object_foreach(json_object_get(settings, "installed"), name, receipt) {
            (void)name;
            const char *root = snag_json_string(receipt, "root");
            if (rc == 0 && canonical && root && path_below(root, canonical)) {
                rc = scan_root(&scan, root, NULL, false, error, size);
            }
        }
        free(canonical);
        free(path);
        if (rc < 0) goto fail;
    }
    json_t *entries = json_object_get(catalog, "entries");
    for (size_t i = 0; i < json_array_size(entries); ++i) {
        json_t *entry = json_array_get(entries, i);
        const char *name = snag_json_string(entry, "name");
        bool selected = true;
        for (size_t j = 0; j < i; ++j) {
            json_t *previous = json_array_get(entries, j);
            if (!strcmp(name, snag_json_string(previous, "name")) &&
                json_integer_value(json_object_get(previous, "priority")) <
                    json_integer_value(json_object_get(entry, "priority"))) {
                selected = false;
                break;
            }
        }
        if (json_object_set_new(entry, "selected", json_boolean(selected)) < 0) goto fail;
    }
    char generation[SNAG_SHA256_HEX_LEN + 1u];
    if (snag_json_digest_bounded(catalog, SNAG_MAX_EVENT_LINE, generation, NULL) < 0 ||
        json_object_set_new(catalog, "generation", json_string(generation)) < 0) goto fail;
    json_decref(scan.visited);
    return catalog;
fail:
    json_decref(scan.visited);
    json_decref(catalog);
    return NULL;
}

const json_t *
snag_skills_resolve(const json_t *catalog, const char *id, char *error, size_t size)
{
    const json_t *match = NULL;
    json_t *entries = json_object_get(catalog, "entries");
    for (size_t i = 0; i < json_array_size(entries); ++i) {
        json_t *entry = json_array_get(entries, i);
        if (!strcmp(id, snag_json_string(entry, "id"))) return entry;
        if (!strcmp(id, snag_json_string(entry, "name")) &&
            json_is_true(json_object_get(entry, "selected"))) {
            if (match) {
                (void)snag_errorf(error, size, "ambiguous skill; select an origin-qualified id");
                return NULL;
            }
            match = entry;
        }
    }
    if (!match) (void)snag_errorf(error, size, "skill unavailable");
    return match;
}

json_t *
snag_skills_list(const json_t *catalog, const char *query, size_t offset, size_t limit)
{
    json_t *result = json_pack("{s:[],s:O,s:O}", "entries", "diagnostics",
        json_object_get(catalog, "diagnostics"), "generation",
        json_object_get(catalog, "generation") ?
            json_object_get(catalog, "generation") : json_null());
    if (!result) return NULL;
    size_t total = 0;
    size_t returned = 0;
    json_t *entries = json_object_get(catalog, "entries");
    for (size_t i = 0; i < json_array_size(entries); ++i) {
        const json_t *entry = json_array_get(entries, i);
        if (query && *query && !strstr(snag_json_string(entry, "name"), query) &&
            !strstr(snag_json_string(entry, "description"), query)) continue;
        if (total++ < offset || returned >= limit) continue;
        if (json_array_append(json_object_get(result, "entries"), (json_t *)entry) < 0) {
            json_decref(result);
            return NULL;
        }
        ++returned;
    }
    if (json_object_set_new(result, "total", json_integer((json_int_t)total)) < 0 ||
        json_object_set_new(result, "offset", json_integer((json_int_t)offset)) < 0 ||
        json_object_set_new(result, "remaining", json_integer((json_int_t)(
            total > offset + returned ? total - offset - returned : 0u))) < 0) {
        json_decref(result);
        return NULL;
    }
    return result;
}

int
snag_skills_report(struct snag_buf *text, const char *action, const json_t *result)
{
    int rc = 0;
    if (!strcmp(action, "list") || !strcmp(action, "refresh")) {
        json_t *entries = json_object_get(result, "entries");
        size_t count = json_array_size(entries);
        json_int_t total = json_integer_value(json_object_get(result, "total"));
        rc = count ? snag_buf_printf(text, "Available skills (%zu of %lld):", count,
            (long long)total) : snag_buf_printf(text, "No matching skills.");
        for (size_t i = 0; rc == 0 && i < count; ++i) {
            json_t *entry = json_array_get(entries, i);
            const char *description = snag_json_string(entry, "description");
            rc = snag_buf_printf(text, "\n  %s (%s) — %.*s", snag_json_string(entry, "name"),
                snag_json_string(entry, "id"), (int)strcspn(description, "\r\n"), description);
        }
        json_int_t remaining = json_integer_value(json_object_get(result, "remaining"));
        if (rc == 0 && remaining) rc = snag_buf_printf(text,
            "\n%lld more; narrow the search to see other matches.", (long long)remaining);
    } else if (!strcmp(action, "paths")) {
        rc = snag_buf_printf(text, "Skill paths:");
        const char *groups[] = {"roots", "linked_origins", "managed"};
        for (size_t i = 0; rc == 0 && i < sizeof(groups) / sizeof(groups[0]); ++i) {
            json_t *entries = json_object_get(result, groups[i]);
            for (size_t j = 0; rc == 0 && j < json_array_size(entries); ++j) {
                json_t *entry = json_array_get(entries, j);
                const char *path = snag_json_string(entry, "path");
                if (!path) path = snag_json_string(entry, "root");
                rc = snag_buf_printf(text, "\n  %s: %s", groups[i], path ? path : "unavailable");
            }
        }
        const char *retained = snag_json_string(result, "excluded_retained_storage");
        if (rc == 0 && retained) rc = snag_buf_printf(text,
            "\n  Retained storage (excluded): %s", retained);
    } else {
        const char *title = !strcmp(action, "show") ? "Skill" :
            !strcmp(action, "validate") ? "Valid skill" :
            !strcmp(action, "create") ? "Created skill" :
            !strcmp(action, "install") ? "Installed skill" :
            !strcmp(action, "update") ? "Updated skill" : "Removed skill";
        const char *name = snag_json_string(result, "name");
        rc = snag_buf_printf(text, "%s: %s", title, name ? name : "unknown");
        if (rc == 0 && !strcmp(action, "show")) {
            const char *description = snag_json_string(result, "description");
            const char *root = snag_json_string(result, "root");
            const char *body = snag_json_string(result, "body");
            rc = snag_buf_printf(text, "\n%s\nPath: %s\n\n%s",
                description ? description : "", root ? root : "", body ? body : "");
        } else if (rc == 0 && strcmp(action, "remove")) {
            const char *root = snag_json_string(result, "root");
            if (root) rc = snag_buf_printf(text, "\nPath: %s", root);
        }
    }
    json_t *diagnostics = json_object_get(result, "diagnostics");
    for (size_t i = 0; rc == 0 && i < json_array_size(diagnostics); ++i) {
        json_t *entry = json_array_get(diagnostics, i);
        rc = snag_buf_printf(text, "\nWarning: %s: %s", snag_json_string(entry, "path"),
            snag_json_string(entry, "message"));
    }
    return rc;
}

json_t *
snag_skill_snapshot(const json_t *entry, char *error, size_t size)
{
    const char *root = snag_json_string(entry, "root");
    const char *expected = snag_json_string(entry, "digest");
    if (!root || !expected) {
        (void)snag_errorf(error, size, "skill unavailable");
        return NULL;
    }
    json_t *skill = snag_skill_load(root, false, error, size);
    if (!skill) return NULL;
    const char *actual = snag_json_string(skill, "digest");
    if (!actual || strcmp(expected, actual)) {
        (void)snag_errorf(error, size,
            "skill instructions changed since discovery; refresh before use");
        json_decref(skill);
        return NULL;
    }
    return skill;
}

json_t *
snag_skills_activate(const json_t *catalog, const char *id, const char *arguments,
    bool explicit_selection, char *error, size_t size)
{
    const json_t *entry = snag_skills_resolve(catalog, id, error, size);
    if (!entry) return NULL;
    (void)explicit_selection; /* Vendor invocation preferences remain metadata. */
    json_t *skill = snag_skill_snapshot(entry, error, size);
    if (!skill) return NULL;
    json_t *result = snag_skill_invocation(skill, arguments, error, size);
    if (result && (json_object_set(result, "id", json_object_get(entry, "id")) < 0 ||
        json_object_set(result, "origin", json_object_get(entry, "origin")) < 0)) {
        json_decref(result);
        result = NULL;
    }
    json_decref(skill);
    return result;
}

json_t *
snag_skills_settings_load(const char *dotdir, char *error, size_t size)
{
    int parent = snag_open_inspect_path(NULL, dotdir);
    if (parent >= 0) {
        struct snag_file_privacy privacy;
        bool private = snag_fd_privacy(parent, &privacy) == 0 &&
            privacy.effective_owner && privacy.private_access;
        close(parent);
        if (!private) {
            (void)snag_errorf(error, size,
                "skills settings require a private application directory");
            return NULL;
        }
    } else if (errno != ENOENT) {
        (void)snag_errorf(error, size,
            "cannot inspect skills settings directory: %s", strerror(errno));
        return NULL;
    }
    char *path = snag_path_join(dotdir, "skills.json");
    struct snag_buf text = {.max = SNAG_MAX_EVENT_LINE};
    json_t *settings = NULL;
    if (!path) return NULL;
    if (access(path, F_OK) < 0 && errno == ENOENT) {
        free(path);
        return json_pack("{s:i}", "version", 1);
    }
    int fd = snag_open_inspect_path(NULL, path);
    struct snag_file_privacy privacy;
    if (fd >= 0 && (snag_fd_privacy(fd, &privacy) < 0 ||
        !privacy.effective_owner || !privacy.private_access)) {
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        (void)snag_errorf(error, size, "skills settings require an owned private file: %s", path);
    } else if (read_descriptor(fd, path, &text, error, size) == 0) {
        json_error_t parse;
        settings = json_loadb((const char *)text.data, text.len, JSON_REJECT_DUPLICATES, &parse);
        if (!json_is_object(settings)) {
            json_decref(settings);
            settings = NULL;
            (void)snag_errorf(error, size, "invalid local skills settings: %s", path);
        }
    }
    if (settings) {
        json_t *installed = json_object_get(settings, "installed");
        if (installed && !json_is_object(installed)) {
            (void)snag_errorf(error, size, "installed skills settings must be a mapping");
            json_decref(settings);
            settings = NULL;
        }
    }
    if (settings) {
        json_t *receipt;
        const char *name;
        json_object_foreach(json_object_get(settings, "installed"), name, receipt) {
            bool valid = snag_skill_name_valid(name) && json_is_object(receipt) &&
                json_is_boolean(json_object_get(receipt, "git"));
            const char *fields[] = {
                "id", "root", "source", "ref", "subdir", "digest", "generation"};
            for (size_t i = 0; valid && i < sizeof(fields) / sizeof(fields[0]); ++i) {
                valid = snag_json_string(receipt, fields[i]) != NULL;
            }
            if (!valid || !snag_path_root_len(snag_json_string(receipt, "root"))) {
                (void)snag_errorf(error, size, "invalid managed skill receipt: %s", name);
                json_decref(settings);
                settings = NULL;
                break;
            }
        }
    }
    if (settings) {
        json_t *version = json_object_get(settings, "version");
        if (version && (!json_is_integer(version) || json_integer_value(version) != 1)) {
            (void)snag_errorf(error, size, "invalid skills settings version");
            json_decref(settings);
            settings = NULL;
        } else {
            /* Old curation records are inert; retain files, never gate discovery. */
            json_object_del(settings, "approvals");
            json_object_del(settings, "reviews");
            json_object_del(settings, "disabled");
            json_object_del(settings, "banned");
            json_object_del(settings, "roots");
            json_object_del(settings, "trusted_workspaces");
            json_object_del(settings, "admitted_origins");
        }
    }
    free(path);
    snag_buf_free(&text);
    return settings;
}

json_t *
snag_skill_resources(const char *root, size_t budget, char *error, size_t size)
{
    int fd = snag_open_inspect_path(NULL, root);
    struct snag_directory *directory = fd >= 0 ? snag_directory_open(fd) : NULL;
    if (!directory) {
        if (fd >= 0) close(fd);
        (void)snag_errorf(error, size, "skill resources unavailable: %s", strerror(errno));
        return NULL;
    }
    json_t *entries = json_array();
    size_t total = 0;
    size_t used = 0;
    const char *name;
    int rc = entries ? 0 : -1;
    while (rc == 0 && (name = snag_directory_next(directory))) {
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git")) continue;
        ++total;
        snag_file_info info;
        if (snag_lstat_at(fd, name, &info) < 0) { rc = -1; break; }
        const char *kind = S_ISDIR(info.st_mode) ? "directory" :
            S_ISREG(info.st_mode) ? "file" : S_ISLNK(info.st_mode) ? "link" : "special";
        json_t *entry = json_pack("{s:s,s:s}", "name", name, "kind", kind);
        char *encoded = entry ? json_dumps(entry, JSON_COMPACT) : NULL;
        size_t length = encoded ? strlen(encoded) + 1u : 0u;
        if (!entry || !encoded) rc = -1;
        else if (used <= budget && length <= budget - used) {
            rc = json_array_append(entries, entry);
            used += length;
        }
        json_decref(entry);
        free(encoded);
    }
    if (rc == 0 && errno) rc = -1;
    if (snag_directory_close(directory) < 0) rc = -1;
    json_t *result = rc == 0 ? json_pack("{s:O,s:I,s:I,s:s}", "entries", entries,
        "total", (json_int_t)total, "remaining", (json_int_t)(total - json_array_size(entries)),
        "more", "Use ordinary list_files/read_file tools at the absolute skill root") : NULL;
    json_decref(entries);
    return result;
}

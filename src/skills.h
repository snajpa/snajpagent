/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SKILLS_H
#define SNAJPAGENT_SKILLS_H

#include "snag_jansson.h"

#include <stdbool.h>
#include <stddef.h>

/* Returned JSON values are owned. Catalog entries retain metadata, never bodies. */
bool snag_skill_name_valid(const char *name);
json_t *snag_skill_manifest(const char *directory, char *error, size_t size);
json_t *snag_skill_load(const char *directory, bool strict, char *error, size_t size);
json_t *snag_skill_snapshot(const json_t *entry, char *error, size_t size);
json_t *snag_skill_invocation(const json_t *skill, const char *arguments,
    char *error, size_t size);
json_t *snag_skills_discover(const char *cwd, const char *dotdir,
    const json_t *settings, char *error, size_t size);
const json_t *snag_skills_resolve(const json_t *catalog, const char *id,
    char *error, size_t size);
json_t *snag_skills_list(const json_t *catalog, const char *query, size_t offset,
    size_t limit);
json_t *snag_skills_activate(const json_t *catalog, const char *id,
    const char *arguments, bool explicit_selection, char *error, size_t size);
json_t *snag_skills_settings_load(const char *dotdir, char *error, size_t size);
int snag_skills_settings_save(const char *, const json_t *, char *, size_t);
json_t *snag_skills_manage(const char *dotdir, const char *action, const char *target,
    const char *git, const char *ref, const char *subdir, char *error, size_t size);
json_t *snag_skill_arguments(const char *, char *, size_t);
json_t *snag_skill_resources(const char *root, size_t budget, char *, size_t);
bool snag_skill_bindings_valid(const json_t *active);
struct snag_session;
bool snag_skill_binding_active(const struct snag_session *, const json_t *entry);
int snag_skills_main(int argc, char **argv);

#endif

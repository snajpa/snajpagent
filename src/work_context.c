/* SPDX-License-Identifier: GPL-2.0-only */
#include "work_context.h"
#include "base.h"
#include "json.h"

#include <string.h>

bool
snag_work_context_valid(const json_t *visit)
{
    if (!visit || json_is_null(visit)) return true;
    if (!json_is_object(visit) || !snag_json_exact_keys(visit,
        "id root task return_cwd return_turn_id")) return false;
    const char *id = snag_json_string(visit, "id");
    const char *turn = snag_json_string(visit, "return_turn_id");
    const char *root = snag_json_string(visit, "root");
    const char *back = snag_json_string(visit, "return_cwd");
    const char *task = snag_json_string(visit, "task");
    return id && strlen(id) == SNAG_ID_HEX_LEN && snag_hex_is_lower(id, SNAG_ID_HEX_LEN) &&
        turn && (!*turn || (strlen(turn) == SNAG_ID_HEX_LEN &&
            snag_hex_is_lower(turn, SNAG_ID_HEX_LEN))) &&
        snag_text_valid(root, 1u, SNAG_PATH_MAX_BYTES) && snag_path_root_len(root) &&
        snag_text_valid(back, 1u, SNAG_PATH_MAX_BYTES) && snag_path_root_len(back) &&
        snag_text_valid(task, 1u, SNAG_MAX_EVENT_LINE);
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "json.h"

#include <string.h>

static bool
managed_continuation_call_matches(const struct snj_response_item *item,
                                  const char *handle)
{
    const char *arg_handle;

    if (!item || item->kind != SNJ_ITEM_TOOL_CALL ||
        !item->name || strcmp(item->name, "write_stdin") != 0)
        return false;
    arg_handle = snj_json_string(item->arguments, "handle");
    return arg_handle && strcmp(arg_handle, handle) == 0;
}

bool
snj_app_managed_continuation_graph_matches(const struct app_state *app,
                                           const struct snj_response_graph *graph,
                                           const struct snj_graph_decision *decision)
{
    const char *handle = app->session.active_process_handle;

    if (!handle[0])
        return true;
    if (decision->outcome != SNJ_GRAPH_CALLS || decision->call_count != 1u)
        return false;
    for (size_t i = 0; i < graph->count; ++i)
        if (graph->items[i].kind == SNJ_ITEM_TOOL_CALL)
            return managed_continuation_call_matches(&graph->items[i], handle);
    return false;
}

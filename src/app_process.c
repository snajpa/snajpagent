/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "json.h"

#include <string.h>

static enum snj_managed_continuation
managed_continuation_call_classify(const struct snj_response_item *item,
                                   const char *handle)
{
    const char *arg_handle;

    if (!item || item->kind != SNJ_ITEM_TOOL_CALL ||
        !item->name || strcmp(item->name, "write_stdin") != 0)
        return SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION;
    arg_handle = snj_json_string(item->arguments, "handle");
    return arg_handle && strcmp(arg_handle, handle) == 0 ?
           SNJ_MANAGED_CONTINUATION_MATCHED :
           SNJ_MANAGED_CONTINUATION_HANDLE_MISMATCH;
}

enum snj_managed_continuation
snj_app_managed_continuation_classify(const struct app_state *app,
                                      const struct snj_response_graph *graph,
                                      const struct snj_graph_decision *decision)
{
    const char *handle = app->session.active_process_handle;

    if (!handle[0])
        return SNJ_MANAGED_CONTINUATION_NONE;
    if (decision->outcome != SNJ_GRAPH_CALLS || decision->call_count != 1u)
        return SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION;
    for (size_t i = 0; i < graph->count; ++i)
        if (graph->items[i].kind == SNJ_ITEM_TOOL_CALL)
            return managed_continuation_call_classify(&graph->items[i], handle);
    return SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION;
}

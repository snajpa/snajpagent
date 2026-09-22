/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_OFFICE_SHEET_H
#define SNAJPAGENT_OFFICE_SHEET_H
#include "json.h"
struct snag_sheet_range { uint32_t sheet, row, column, rows, columns; };
bool snag_sheet_range_valid(const struct snag_sheet_range *);
/* Returns rendered PNG and bounded cell text; Office frees its own strings. */
struct LibreOfficeKitDocumentStruct;
struct LibreOfficeKitStruct;
int snag_office_sheet(struct LibreOfficeKitStruct *, struct LibreOfficeKitDocumentStruct *,
                       const struct snag_sheet_range *, struct snag_buf *, json_t **, char *, size_t);
int snag_office_sheet_html(const char *, const struct snag_sheet_range *, struct snag_buf *);
#endif

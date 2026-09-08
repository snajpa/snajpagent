/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PDF_H
#define SNAJPAGENT_PDF_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct snag_buf;
struct snag_pdf;
int snag_pdf_open(const char *, int (*)(void *, unsigned int), void *,
                   struct snag_pdf **, unsigned int *, char *, size_t);
int snag_pdf_page(struct snag_pdf *, unsigned int, struct snag_buf *, struct snag_buf *, char *, size_t);
void snag_pdf_close(struct snag_pdf *);
#ifdef __cplusplus
}
#endif
#endif

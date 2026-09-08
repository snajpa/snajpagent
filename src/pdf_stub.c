/* SPDX-License-Identifier: GPL-2.0-only */
#include "pdf.h"
#include "base.h"
int snag_pdf_open(const char *path, int (*pump)(void *, unsigned int), void *opaque,
                   struct snag_pdf **out, unsigned int *pages, char *error, size_t size)
{
    (void)path; (void)pump; (void)opaque; (void)pages; *out = NULL;
    snag_errorf(error, size, "This custom build excludes linked PDF support (WITH_PDF=0)");
    return -1;
}
int snag_pdf_page(struct snag_pdf *pdf, unsigned int page, struct snag_buf *text,
                   struct snag_buf *image, char *error, size_t size)
{
    (void)pdf; (void)page; (void)text; (void)image; (void)error; (void)size;
    return -1;
}
void snag_pdf_close(struct snag_pdf *pdf) { (void)pdf; }

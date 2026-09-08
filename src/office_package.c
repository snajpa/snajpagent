/* SPDX-License-Identifier: GPL-2.0-only */
#include "office.h"
#include "fs.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if SNAJPAGENT_OFFICE
#include <archive.h>
#include <archive_entry.h>
#include <libxml/xmlreader.h>

static bool
local_reference(const char *value)
{
    if (!value || !*value || *value == '#') return true;
    if (*value == '/' || *value == '\\' || strstr(value, "..")) return false;
    /* URI encoded separators/schemes must not escape package interpretation. */
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p == ':' || *p == '%' || *p == '\\' || *p < 0x20u) return false;
    return true;
}

static int
xml_safe(const void *bytes, size_t length, const char *entry)
{
    xmlTextReaderPtr reader = xmlReaderForMemory(bytes, (int)length, NULL, NULL,
        XML_PARSE_NONET | XML_PARSE_NO_XXE | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_COMPACT);
    if (!reader) return -1;
    int rc = 0, step;
    while ((step = xmlTextReaderRead(reader)) == 1) {
        int type = xmlTextReaderNodeType(reader);
        if (type == XML_READER_TYPE_DOCUMENT_TYPE || type == XML_READER_TYPE_ENTITY_REFERENCE ||
            xmlTextReaderDepth(reader) > 128) { rc = -1; break; }
        if (type != XML_READER_TYPE_ELEMENT) continue;
        const char *name = (const char *)xmlTextReaderConstLocalName(reader);
        const char *ns = (const char *)xmlTextReaderConstNamespaceUri(reader);
        if (!name || (ns && strstr(ns, "w3.org/2001/XInclude")) ||
            !strcmp(name, "script") || !strcmp(name, "scripts") || !strcmp(name, "event-listener") ||
            !strcmp(name, "dde-connection") || !strcmp(name, "dde-link") ||
            !strcmp(name, "connection") || !strcmp(name, "externalLink") || !strcmp(name, "altChunk")) {
            rc = -1; break;
        }
        if (!strcmp(name, "Relationship")) {
            xmlChar *mode = xmlTextReaderGetAttribute(reader, BAD_CAST "TargetMode");
            xmlChar *target = xmlTextReaderGetAttribute(reader, BAD_CAST "Target");
            xmlChar *kind = xmlTextReaderGetAttribute(reader, BAD_CAST "Type");
            /* Ordinary hyperlinks are inert navigation, never followed. Other
             * external relations (templates, images, OLE, data) are rejected. */
            bool link = kind && strstr((const char *)kind, "/hyperlink") &&
                !strcmp(strstr((const char *)kind, "/hyperlink"), "/hyperlink");
            if (!link && target) {
                const char *relative = (const char *)target;
                int depth = -1; /* relationships live in an extra _rels directory */
                for (const char *p = entry; *p; ++p) if (*p == '/') ++depth;
                if (depth < 0) depth = 0;
                while (!strncmp(relative,"../",3u)) { --depth; relative += 3u; }
                if (depth < 0 || !local_reference(relative)) rc = -1;
            }
            if (!link && mode && !strcmp((const char *)mode, "External")) rc = -1;
            xmlFree(mode); xmlFree(target); xmlFree(kind);
            if (rc) break;
        }
        if (xmlTextReaderMoveToFirstAttribute(reader) == 1) {
            do {
                const char *attr = (const char *)xmlTextReaderConstLocalName(reader);
                const char *value = (const char *)xmlTextReaderConstValue(reader);
                const char *attr_ns = (const char *)xmlTextReaderConstNamespaceUri(reader);
                if (!attr || !value) { rc = -1; break; }
                if ((!strcmp(attr, "href") && strcmp(name, "a") && !local_reference(value)) ||
                    (!strcmp(attr, "base") && attr_ns && !strcmp(attr_ns, "http://www.w3.org/XML/1998/namespace")) ||
                    (!strcmp(attr, "src") && !local_reference(value)) ||
                    (!strncmp(attr, "on", 2u) && ns && !strcmp(ns, "http://www.w3.org/2000/svg"))) {
                    rc = -1; break;
                }
            } while (xmlTextReaderMoveToNextAttribute(reader) == 1);
            (void)xmlTextReaderMoveToElement(reader);
        }
        if (rc) break;
    }
    if (step < 0) rc = -1;
    xmlFreeTextReader(reader); return rc;
}

int
snag_office_package(const char *path, char *error, size_t size)
{
    int fd = snag_open_inspect_path("/", path), rc = -1;
    struct archive *archive = archive_read_new();
    struct archive_entry *entry;
    struct snag_buf bytes;
    uint64_t total = 0;
    char *names[4096] = {0}; size_t count = 0;
    bool document = false;
    snag_buf_init(&bytes, 16u * 1024u * 1024u);
    if (fd < 0 || !archive) goto done;
    archive_read_support_format_zip(archive);
    if (archive_read_open_fd(archive, fd, 32768u) != ARCHIVE_OK) goto done;
    int status;
    while ((status = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!name || !*name || strlen(name) > 512u || *name == '/' || strchr(name, '\\') ||
            strchr(name, ':') || strstr(name, "..") || count == 4096u || archive_entry_is_encrypted(entry) ||
            (archive_entry_filetype(entry) != AE_IFREG && archive_entry_filetype(entry) != AE_IFDIR) ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry)) goto done;
        for (size_t i = 0; i < count; ++i) if (!strcmp(name, names[i])) goto done;
        names[count] = snag_strdup_checked(name, 512u);
        if (!names[count++]) goto done;
        char lower[513];
        for (size_t i = 0; i <= strlen(name); ++i) lower[i] = (char)tolower((unsigned char)name[i]);
        if (strstr(lower, "vba") || strstr(lower, "macros/") || strstr(lower, "basic/") ||
            strstr(lower, "scripts/") || strstr(lower, "externallinks/") || strstr(lower, "embeddings/") ||
            strstr(lower, "connections.xml")) goto done;
        if (!strcmp(name, "word/document.xml") || !strcmp(name, "ppt/presentation.xml") ||
            !strcmp(name, "xl/workbook.xml") || !strcmp(name, "content.xml")) document = true;
        if (archive_entry_filetype(entry) == AE_IFDIR) continue;
        const char *ext = strrchr(lower, '.');
        bool xml = ext && (!strcmp(ext, ".xml") || !strcmp(ext, ".rels") || !strcmp(ext, ".svg"));
        snag_buf_reset(&bytes);
        unsigned char block[32768]; la_ssize_t got;
        while ((got = archive_read_data(archive, block, sizeof(block))) > 0) {
            if ((uint64_t)got > (256u << 20) - total) goto done;
            total += (uint64_t)got;
            if (xml && snag_buf_append(&bytes, block, (size_t)got) < 0) goto done;
        }
        if (got < 0 || (xml && xml_safe(bytes.data, bytes.len, name) < 0)) goto done;
    }
    if (status == ARCHIVE_EOF && document && archive_read_has_encrypted_entries(archive) <= 0) rc = 0;
done:
    for (size_t i = 0; i < count; ++i) free(names[i]);
    if (archive) archive_read_free(archive);
    if (fd >= 0) close(fd);
    snag_buf_free(&bytes);
    if (rc) snag_errorf(error, size, "Office package invalid, encrypted, active/external, or exceeds 4096 entries / 16 MiB XML / 256 MiB expanded");
    return rc;
}
#else
int snag_office_package(const char *path, char *error, size_t size)
{
    (void)path; snag_errorf(error, size, "This custom build excludes Office import"); return -1;
}
#endif

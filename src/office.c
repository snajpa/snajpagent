/* SPDX-License-Identifier: GPL-2.0-only */
#include "office.h"
#include "store.h"
#include "convert.h"
#include "fs.h"
#include "pdf.h"
#include "media.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#endif

static char executable[SNAG_PATH_MAX_BYTES + 1u];
void snag_office_program(const char *program)
{
    char *path = snag_program_path(program);
    if (path) (void)snag_strcpy(executable, sizeof(executable), path);
    free(path);
}

char *snag_office_runtime(const char *program,const char *root)
{
    if(!root || !*root)return NULL;
    if(snag_path_root_len(root))return snag_strdup_checked(root,SNAG_PATH_MAX_BYTES);
    if(!program || !snag_path_root_len(program))return NULL;
    char *dir=snag_strdup_checked(program,SNAG_PATH_MAX_BYTES);if(!dir)return NULL;
    snag_path_slashes(dir);
    char *last=strrchr(dir,'/');
    if(!last) {free(dir);return NULL;}
    size_t root_len=snag_path_root_len(dir);
    if((size_t)(last-dir)<root_len)dir[root_len]=0;else *last=0;
    char *path=snag_path_join(dir,root);free(dir);return path;
}

char *snag_office_file_url(const char *path)
{
    if(!path || !*path)return NULL;
    bool drive=((path[0]>='A' && path[0]<='Z') || (path[0]>='a' && path[0]<='z')) &&
        path[1]==':' && (path[2]=='/' || path[2]=='\\');
    bool unc=(path[0]=='/' && path[1]=='/') || (path[0]=='\\' && path[1]=='\\');
    if(!drive && !unc && path[0]!='/')return NULL;
    struct snag_buf out;
    snag_buf_init(&out, 3u * SNAG_PATH_MAX_BYTES + 16u);
    if (snag_buf_append(&out, "file://", 7u) < 0) goto failed;
    if(drive && snag_buf_putc(&out,'/')<0)goto failed;
    if(unc)path+=2;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c=*p;
        if((drive || unc) && c=='\\')c='/';
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("/-_.~:", c)) {
            if (snag_buf_putc(&out, c) < 0) goto failed;
        } else if (snag_buf_printf(&out, "%%%02X", c) < 0) goto failed;
    }
    if (snag_buf_terminate(&out) == 0) return (char *)out.data;
failed: snag_buf_free(&out); return NULL;
}

#if SNAJPAGENT_OFFICE
#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.h>
extern LibreOfficeKit *libreofficekit_hook_2(const char *, const char *);

static bool office_type(const char *mime)
{
    return mime && (!strcmp(mime,"application/vnd.openxmlformats-officedocument.wordprocessingml.document") ||
        !strcmp(mime,"application/vnd.openxmlformats-officedocument.presentationml.presentation") ||
        !strcmp(mime,"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") ||
        !strcmp(mime,"application/vnd.oasis.opendocument.text") ||
        !strcmp(mime,"application/vnd.oasis.opendocument.presentation") ||
        !strcmp(mime,"application/vnd.oasis.opendocument.spreadsheet"));
}

struct office_progress {
    int work_fd;
    int (*pump)(void *,unsigned int);
    void *opaque;
    uint64_t next_check;
    bool resource_failed;
};

static int
office_progress(void *opaque,unsigned int timeout)
{
    struct office_progress *state=opaque;
    int rc=state->pump?state->pump(state->opaque,timeout):0;
    if(rc)return rc;
    uint64_t now=snag_monotonic_ms();
    if(now>=state->next_check) {
        state->next_check=now+50u;
        if(snag_media_work_check(state->work_fd)<0) {
            state->resource_failed=true;return 1;
        }
    }
    return 0;
}

int
snag_office_export(struct snag_session *session, const char *path, const char *mime,
                 unsigned int first, unsigned int last, const struct snag_sheet_range *range, int (*pump)(void *, unsigned int),
                 void *opaque, snag_wake_fd wake, struct snag_buf *out, json_t **metadata, char *error, size_t size)
{
    *metadata=NULL;
    if ((range && !snag_sheet_range_valid(range)) || !office_type(mime) || !*executable || !first || last < first || last-first > 3u || last > 100000u) {
        snag_errorf(error,size,"Invalid Office page selection or unavailable internal executable"); return -1;
    }
    char start[16], end[16];
    int work=snag_media_work_open(session->dir_fd,error,size);
    if(work<0)return -1;
    char *dir = snag_path_join(session->dir_path,SNAG_MEDIA_WORK_NAME);
    snprintf(start,sizeof(start),"%u",first); snprintf(end,sizeof(end),"%u",last);
    char selectors[5][16];
    const char *args[12] = {executable,"--internal-office-pdf",path,mime,start,end,NULL};
    if(range) {
        const uint32_t values[]={range->sheet,range->row,range->column,range->rows,range->columns};
        for(size_t i=0;i<5u;++i){snprintf(selectors[i],sizeof(selectors[i]),"%u",values[i]);args[6u+i]=selectors[i];}
    }
    size_t original = out->len;
    struct office_progress progress={.work_fd=work,.pump=pump,.opaque=opaque};
    int rc = dir ? snag_convert_internal(args,dir,out,office_progress,&progress,wake,error,size) : -1;
    if(progress.resource_failed || (!rc && snag_media_work_check(work)<0)) {
        snag_errorf(error,size,"Office temporary files exceeded 256 MiB / 4096 entries / depth 32, or became unsafe");
        rc=-1;
    }
    if (!rc) {
        unsigned char *lf = memchr(out->data + original,'\n',out->len - original);
        size_t header = lf ? (size_t)(lf - out->data) - original : 2097153u;
        json_t *meta = header <= 2097152u ? json_loadb((char *)out->data+original,header,JSON_REJECT_DUPLICATES,NULL) : NULL;
        const char *format=snag_json_string(meta,"format");
        if (!meta || !snag_json_string(meta,"coverage") || !format ||
            out->len-original < header+9u || strcmp(format,range?"png":"pdf") ||
            memcmp(lf+1u,range?"\211PNG\r\n\032\n":"%PDF-",range?8u:5u)) {
            snag_errorf(error,size,"Invalid linked Office result envelope"); rc=-1;
        } else {
            memmove(out->data+original,lf+1u,out->len-original-header-1u);
            out->len -= header+1u;
            *metadata=json_incref(meta);
        }
        json_decref(meta);
    }
    close(work);
    char cleanup_error[256];
    if(snag_media_work_remove(session->dir_fd,cleanup_error,sizeof(cleanup_error))!=0 && !rc) {
        snag_errorf(error,size,"%s",cleanup_error);rc=-1;
    }
    if(rc) {out->len=original;json_decref(*metadata);*metadata=NULL;}
    free(dir); return rc;
}

int
snag_office_worker(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1],"--internal-office-pdf")) return -1;
    int rc = 1;
    char error[256] = "Invalid internal Office request", *dir = NULL;
    char *source_url = NULL, *output_url = NULL, *profile_url = NULL;
    char *runtime=NULL,*program_dir=NULL;
    struct snag_sheet_range range={0},*selection=NULL;
    if ((argc != 6 && argc != 11) || !snag_path_root_len(argv[2]) || !office_type(argv[3])) goto done;
    char *tail; unsigned long first = strtoul(argv[4],&tail,10);
    if (*tail || !first) goto done;
    unsigned long last = strtoul(argv[5],&tail,10);
    if (*tail || last < first || last-first > 3u || last > 100000u) goto done;
    if(argc==11) {
        uint32_t values[5];
        for(size_t i=0;i<5u;++i) {
            unsigned long value=strtoul(argv[6u+i],&tail,10);
            if(*tail || !value || value>1048576u)goto done;
            values[i]=(uint32_t)value;
        }
        range=(struct snag_sheet_range){values[0],values[1],values[2],values[3],values[4]};
        if(!snag_sheet_range_valid(&range))goto done;
        selection=&range;
    }
    dir = snag_realpath(".");
    runtime=snag_office_runtime(executable,SNAJPAGENT_OFFICE_ROOT);
    program_dir=runtime?snag_path_join(runtime,"program"):NULL;
    if(!program_dir)goto done;
    if (!dir || snag_office_worker_limits(dir,error,sizeof(error))<0) goto done;
    char *profile = snag_path_join(dir,"profile");
    char *output = snag_path_join(dir,"pages.pdf");
    source_url = snag_office_file_url(argv[2]); profile_url = profile ? snag_office_file_url(profile) : NULL;
    output_url = output ? snag_office_file_url(output) : NULL;
    free(profile); free(output);
    if (!source_url || !profile_url || !output_url) goto done;
    int confinement = snag_office_confine(dir,runtime,argv[2],error,sizeof(error));
    if (confinement < 0) goto done;
    char confinement_note[sizeof(error)];
    (void)snag_strcpy(confinement_note,sizeof(confinement_note),error);
    if (confinement > 0) (void)fprintf(stderr,"%s\n",error);
    if (snag_office_package(argv[2],error,sizeof(error)) < 0) goto done;
    /* Suppress library stdout. Only verified PDF bytes reach the parent. */
#ifdef _WIN32
    if(_setmode(STDOUT_FILENO,_O_BINARY)<0)goto done;
    int channel = dup(STDOUT_FILENO), null = open("NUL",O_WRONLY|O_BINARY);
#else
    int channel = dup(STDOUT_FILENO), null = open("/dev/null",O_WRONLY);
#endif
    if (channel < 0 || null < 0 || dup2(null,STDOUT_FILENO) < 0) goto done;
    close(null);
    LibreOfficeKit *office = libreofficekit_hook_2(program_dir,profile_url);
    if (!office) { strcpy(error,"Linked Office initialization failed"); goto done; }
    LibreOfficeKitDocument *doc = office->pClass->documentLoadWithOptions(office,source_url,
        "Language=en-US,Timezone=UTC,Batch=true,MacroSecurityLevel=3,EnableMacrosExecution=false");
    if (!doc) { strcpy(error,"Linked Office import failed (unsupported or protected document)"); goto done; }
    /* LibreOfficeKitDocumentType: text=0, spreadsheet=1, presentation=2. */
    int type=doc->pClass->getDocumentType(doc);
    int expected=strstr(argv[3],"spreadsheet")?1:strstr(argv[3],"presentation")?2:0;
    if(type!=expected) {strcpy(error,"Office package does not match its declared document type");goto done;}
    int parts=doc->pClass->getParts(doc);
    if(!selection && type==2 && (parts<1 || last>(unsigned int)parts)) {
        snprintf(error,sizeof(error),"Presentation has %d slides; select an existing slide range",parts);goto done;
    }
    struct snag_buf png;snag_buf_init(&png,12u*1024u*1024u);
    json_t *meta=NULL;
    int fd=-1;
    if(selection) {
        if(snag_office_sheet(doc,selection,&png,&meta,error,sizeof(error))<0)goto done;
    } else {
        char options[768];
        snprintf(options,sizeof(options),"{\"PageRange\":{\"type\":\"string\",\"value\":\"%lu-%lu\"},"
            "\"ExportHiddenSlides\":{\"type\":\"boolean\",\"value\":\"true\"},"
            "\"ExportNotesPages\":{\"type\":\"boolean\",\"value\":\"false\"},"
            "\"ExportOnlyNotesPages\":{\"type\":\"boolean\",\"value\":\"false\"},"
            "\"ExportNotes\":{\"type\":\"boolean\",\"value\":\"false\"},"
            "\"ExportBookmarks\":{\"type\":\"boolean\",\"value\":\"false\"},"
            "\"ExportFormFields\":{\"type\":\"boolean\",\"value\":\"false\"},"
            "\"IsAddStream\":{\"type\":\"boolean\",\"value\":\"false\"}}",first,last);
        if (!doc->pClass->saveAs(doc,output_url,"pdf",options)) { strcpy(error,"Linked Office page export failed"); goto done; }
        fd=snag_open_inspect_path(dir,"pages.pdf");
        snag_file_info st;
        if (fd<0 || snag_fstat(fd,&st)<0 || st.st_size<5 || st.st_size>32u*1024u*1024u-4096u) {
            strcpy(error,"Office PDF is absent or exceeds 32 MiB"); goto done;
        }
        /* Validate the whole derivative before sending or retaining it. */
        struct snag_pdf *pdf=NULL;unsigned int pages=0;
        char *pdf_path=snag_path_join(dir,"pages.pdf");
        int opened=pdf_path?snag_pdf_open(pdf_path,NULL,NULL,&pdf,&pages,error,sizeof(error)):-1;
        free(pdf_path);snag_pdf_close(pdf);
        if(opened)goto done;
        if(pages!=last-first+1u) {
            snprintf(error,sizeof(error),"Office export returned %u pages for requested range %lu-%lu",pages,first,last);goto done;
        }
        meta=json_pack("{s:s,s:s}","format","pdf","page_kind",
            type==2?"Source slide":type==1?"Imported workbook print page":"Imported document page");
    }
    char coverage[1536],selected[192];
    if(selection)snprintf(selected,sizeof(selected),"Sheet %u, row %u column %u, %u rows by %u columns only",
        range.sheet,range.row,range.column,range.rows,range.columns);
    else snprintf(selected,sizeof(selected),"%s %lu-%lu only%s",snag_json_string(meta,"page_kind"),first,last,
        type==2?" (source slide order, including selected hidden slides)":" (importer pagination)");
    snprintf(coverage,sizeof(coverage),
        "LibreOffice: %s; document type %d, parts %d. "
        "Layout/fonts and computed values may differ from the originating application. "
        "Macros, scripts, embedded OLE and external resource relationships rejected; "
        "macro execution disabled. %s. "
        "Other pages, slides, sheets, cells and speaker notes uninspected.",
        selected,type,parts,
        confinement_note);
    struct snag_buf header;snag_buf_init(&header,2u*1024u*1024u);
    if(!meta || json_object_set_new(meta,"coverage",json_string(coverage))<0 ||
        snag_json_canonical(meta,&header)<0 || snag_buf_putc(&header,'\n')<0 ||
        snag_write_full(channel,header.data,header.len)<0)goto done;
    json_decref(meta);snag_buf_free(&header);
    if(selection) {
        if(snag_write_full(channel,png.data,png.len)<0)goto done;
        snag_buf_free(&png);
    } else {
        unsigned char block[32768];ssize_t got;
        while((got=read(fd,block,sizeof(block)))>0)
            if(snag_write_full(channel,block,(size_t)got)<0)goto done;
        if(got<0)goto done;
        close(fd);
    }
    close(channel);rc=0;
    /* Disposable child exits after synchronous export. Do not run global LO
     * destructors (the upstream runtime may retain cross-thread globals). */
done:
    if (rc) (void)fprintf(stderr,"%s\n",error);
    free(dir); free(source_url); free(output_url); free(profile_url);free(runtime);free(program_dir);
    _Exit(rc);
}
#else
int snag_office_worker(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1],"--internal-office-pdf")) return -1;
    fputs("This custom build excludes Office import\n",stderr); _Exit(1);
}
int snag_office_export(struct snag_session *s, const char *p, const char *m, unsigned int f, unsigned int l,
                        const struct snag_sheet_range *range, int (*pump)(void *, unsigned int), void *opaque, snag_wake_fd wake,
                        struct snag_buf *out, json_t **metadata, char *error, size_t size)
{
    (void)range;(void)s;(void)p;(void)m;(void)f;(void)l;(void)pump;(void)opaque;(void)wake;(void)out;*metadata=NULL;
    snag_errorf(error,size,"This custom build excludes Office import (WITH_OFFICE=0)"); return -1;
}
#endif

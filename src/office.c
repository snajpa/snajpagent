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
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef _WIN32
#include <dirent.h>
#endif
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

/* Installed runtimes keep the engine and the LibreOfficeKit library under
 * <root>/program; a root already naming that directory is accepted too.
 *
 * The names differ per platform, and the audited installs expose the
 * LibreOfficeKit hooks from the merged library on Windows and macOS rather than
 * from a libsofficeapp file, so both attested names are tried with the canonical
 * one first. The engine carries the platform's usual executable name. Detection
 * stays by component presence: never PATH, never a conversion command's exit
 * status. */
#if defined(__APPLE__)
static const char *const office_kit_names[]={"libsofficeapp.dylib","libmergedlo.dylib",NULL};
static const char *const office_engine_names[]={"soffice",NULL};
#elif defined(_WIN32)
static const char *const office_kit_names[]={"libsofficeapp.dll","mergedlo.dll",NULL};
static const char *const office_engine_names[]={"soffice.exe","soffice",NULL};
#else
static const char *const office_kit_names[]={"libsofficeapp.so",NULL};
/* `soffice` is the supported entry point even when it is a shell wrapper: it sets
 * up what the bare soffice.bin expects, and the conversion child inherits PATH. */
static const char *const office_engine_names[]={"soffice","soffice.bin",NULL};
#endif

static bool office_present(const char *path)
{
    snag_file_info st;
    return path && *path && snag_stat(path,&st)==0 && S_ISREG(st.st_mode);
}

/* True when any attested name exists under dir; the matching path is handed back
 * through found so the caller owns and releases it. */
static bool office_component_present(const char *dir,const char *const *names,char **found)
{
    if(!dir)return false;
    for(size_t i=0u;names[i];++i) {
        char *path=snag_path_join(dir,names[i]);
        if(path && office_present(path)) {
            if(found)*found=path;
            else free(path);
            return true;
        }
        free(path);
    }
    return false;
}

int snag_office_verify_runtime(const char *root,char *error,size_t size)
{
    if(!root || !*root) {
        snag_errorf(error,size,"LibreOffice runtime root is not configured");
        return -1;
    }
    char *program=snag_path_join(root,"program");
    char *kit=NULL,*engine=NULL;
    bool usable=office_component_present(program,office_kit_names,&kit) ||
        office_component_present(root,office_kit_names,&kit) ||
        office_component_present(program,office_engine_names,&engine);
    if(!usable) {
        char *expect_kit=program?snag_path_join(program,office_kit_names[0]):NULL;
        char *expect_engine=program?snag_path_join(program,office_engine_names[0]):NULL;
        snag_errorf(error,size,
            "LibreOffice runtime is missing or incomplete at %s: expected %s or %s "
            "(install LibreOffice, or point OFFICE_ROOT at its lib/libreoffice directory)",
            root,expect_kit?expect_kit:office_kit_names[0],
            expect_engine?expect_engine:"the engine executable");
        free(expect_kit);free(expect_engine);
    }
    free(program);free(kit);free(engine);
    return usable?0:-1;
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

int snag_office_profile(const char *path,char *error,size_t size)
{
    static const char settings[]=
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<oor:items xmlns:oor=\"http://openoffice.org/2001/registry\">"
        "<item oor:path=\"/org.openoffice.Office.Common/Security/Scripting\">"
        "<prop oor:name=\"MacroSecurityLevel\" oor:op=\"fuse\"><value>3</value></prop>"
        "<prop oor:name=\"DisableMacrosExecution\" oor:op=\"fuse\"><value>true</value></prop>"
        "<prop oor:name=\"SecureURL\" oor:op=\"fuse\"><value/></prop>"
        "<prop oor:name=\"BlockUntrustedRefererLinks\" oor:op=\"fuse\"><value>true</value></prop>"
        "</item><item oor:path=\"/org.openoffice.Office.Writer/Content/Update\">"
        "<prop oor:name=\"Link\" oor:op=\"fuse\"><value>2</value></prop>"
        "</item><item oor:path=\"/org.openoffice.Office.Calc/Content/Update\">"
        "<prop oor:name=\"Link\" oor:op=\"fuse\"><value>1</value></prop>"
        "</item></oor:items>";
    int root=-1,user=-1,fd=-1,rc=-1;
    if(snag_mkdir_private(path)<0 || (root=snag_open_read(path,true))<0 ||
        snag_mkdir_private_at(root,"user")<0 || (user=snag_open_read_at(root,"user",true))<0 ||
        (fd=snag_create_private_at(user,"registrymodifications.xcu",true))<0 ||
        snag_write_full(fd,settings,sizeof(settings)-1u)<0)goto out;
    rc=0;
out:
    if(rc)snag_errorf(error,size,"Cannot prepare private Office profile: %s",strerror(errno));
    if(fd>=0)close(fd);
    if(user>=0)close(user);
    if(root>=0)close(root);
    return rc;
}

/* Engine discovery for the commands backend: the installed runtime root first, then
 * the target's PATH, then the usual and versioned installation roots, so a
 * distribution or vendor install is found without configuration. Caller owns it. */
char *snag_office_command(const char *program,const char *root)
{
    char *runtime=snag_office_runtime(program,root);
    char *dir=runtime?snag_path_join(runtime,"program"):NULL;
    char *found=NULL;
    if(dir && office_component_present(dir,office_engine_names,&found)) {
        free(dir);free(runtime);return found;
    }
    free(dir);free(runtime);
    static const char *const names[]={"soffice","libreoffice",NULL};
    for(size_t i=0u;names[i];++i) {
        char *path=snag_program_path(names[i]);
        if(path && strchr(path,'/') && snag_file_executable(path)==0) return path;
        free(path);
    }
#ifndef _WIN32
    static const char *const roots[]={"/usr/lib/libreoffice","/usr/lib64/libreoffice",
        "/usr/local/lib/libreoffice","/opt/libreoffice",NULL};
    for(size_t i=0u;roots[i];++i) {
        char *program_dir=snag_path_join(roots[i],"program");
        if(program_dir && office_component_present(program_dir,office_engine_names,&found)) {
            free(program_dir);return found;
        }
        free(program_dir);
    }
    /* Versioned installs: /usr/lib/libreoffice-7.6, /opt/libreoffice25.2, ... */
    static const char *const parents[]={"/usr/lib","/usr/lib64","/opt",NULL};
    for(size_t i=0u;parents[i];++i) {
        DIR *parent=opendir(parents[i]);
        if(!parent)continue;
        struct dirent *entry;
        for(unsigned scanned=0u;(entry=readdir(parent)) && scanned<256u;++scanned) {
            if(strncmp(entry->d_name,"libreoffice",11u))continue;
            char *base=snag_path_join(parents[i],entry->d_name);
            char *program_dir=base?snag_path_join(base,"program"):NULL;
            if(program_dir && office_component_present(program_dir,office_engine_names,&found)) {
                free(program_dir);free(base);closedir(parent);return found;
            }
            free(program_dir);free(base);
        }
        closedir(parent);
    }
#endif
    return NULL;
}

/* Shared by both Office backends: the document types the modality claims. */
#if SNAJPAGENT_OFFICE || SNAJPAGENT_OFFICE_COMMANDS
static bool office_type(const char *mime)
{
    return mime && (!strcmp(mime,"application/vnd.openxmlformats-officedocument.wordprocessingml.document") ||
        !strcmp(mime,"application/vnd.openxmlformats-officedocument.presentationml.presentation") ||
        !strcmp(mime,"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") ||
        !strcmp(mime,"application/vnd.oasis.opendocument.text") ||
        !strcmp(mime,"application/vnd.oasis.opendocument.presentation") ||
        !strcmp(mime,"application/vnd.oasis.opendocument.spreadsheet"));
}
#endif

#if SNAJPAGENT_OFFICE
#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.h>
extern LibreOfficeKit *libreofficekit_hook_2(const char *, const char *);

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
    if(snag_office_verify_runtime(runtime,error,sizeof(error))<0)goto done;
    if (!dir || snag_office_worker_limits(dir,error,sizeof(error))<0) goto done;
    char *profile = snag_path_join(dir,"profile");
    char *output = snag_path_join(dir,"pages.pdf");
    source_url = snag_office_file_url(argv[2]);
    profile_url = profile && !snag_office_profile(profile,error,sizeof(error)) ? snag_office_file_url(profile) : NULL;
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
        if(snag_office_sheet(office,doc,selection,&png,&meta,error,sizeof(error))<0)goto done;
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
        "macro execution and Writer/Calc link updates disabled; untrusted links blocked. %s. "
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
    _exit(rc);
    return rc; /* Old libc headers may omit the noreturn annotation. */
}
#elif SNAJPAGENT_OFFICE_COMMANDS
/* Command backend: drive the installed LibreOffice through its own executable.
 * The linked worker's confinement denies execve, so this path runs in the parent
 * and never loads the runtime itself. Page-range export only: sheet-area
 * selection has no command-line equivalent and is refused rather than guessed. */
int snag_office_worker(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1],"--internal-office-pdf")) return -1;
    fputs("This build drives the installed LibreOffice command line, not the linked worker\n",stderr);
    _exit(1);
    return 1;
}

/* The CLI takes one PDF export filter per document family. */
static const char *office_filter(const char *mime)
{
    if (strstr(mime,"spreadsheet")) return "calc_pdf_Export";
    if (strstr(mime,"presentation")) return "impress_pdf_Export";
    return "writer_pdf_Export";
}

int snag_office_export(struct snag_session *session, const char *path, const char *mime,
        unsigned int first, unsigned int last, const struct snag_sheet_range *range,
        int (*pump)(void *, unsigned int), void *opaque, snag_wake_fd wake,
        struct snag_buf *out, json_t **metadata, char *error, size_t size)
{
    *metadata=NULL;
    if ((range && !snag_sheet_range_valid(range)) || !office_type(mime) || !*executable ||
        !first || last < first || last-first > 3u || last > 100000u) {
        snag_errorf(error,size,"Invalid Office page selection or unavailable internal executable");
        return -1;
    }
    if (range) {
        snag_errorf(error,size,"Sheet-area Office selection requires the linked Office import; "
            "this build uses the installed command line (WITH_OFFICE_COMMANDS=1)");
        return -1;
    }
    char *engine=snag_office_command(executable,SNAJPAGENT_OFFICE_ROOT);
    if (!engine) {
        snag_errorf(error,size,"No LibreOffice engine found: tried %s, the PATH, and the usual installation roots",
            SNAJPAGENT_OFFICE_ROOT);
        return -1;
    }
    int work=snag_media_work_open(session->dir_fd,error,size);
    if (work<0) { free(engine); return -1; }
    char *dir=snag_path_join(session->dir_path,SNAG_MEDIA_WORK_NAME);
    char *profile=dir?snag_path_join(dir,"profile"):NULL;
    char *profile_url=profile?snag_office_file_url(profile):NULL;
    char *source_url=snag_office_file_url(path);
    char profile_arg[SNAG_PATH_MAX_BYTES+32u], convert_arg[96], pages[24];
    if (first==last) snprintf(pages,sizeof(pages),"%u",first);
    else snprintf(pages,sizeof(pages),"%u-%u",first,last);
    snprintf(profile_arg,sizeof(profile_arg),"-env:UserInstallation=%s",profile_url?profile_url:"");
    /* The format token comes first: `pdf:<filter>:<json options>`. Without the
     * leading `pdf:` the engine treats the filter name as the target format and
     * writes a differently named file, which the check below then rejects. */
    snprintf(convert_arg,sizeof(convert_arg),"pdf:%s:{\"PageRange\":{\"type\":\"string\",\"value\":\"%s\"}}",
        office_filter(mime),pages);
    const char *args[]={engine,"--headless","--invisible","--nodefault","--norestore","--nolockcheck",
        "--nofirststartwizard",profile_arg,"--convert-to",convert_arg,"--outdir",dir,
        path,NULL};
    int rc=-1;
    size_t original=out->len;
    struct snag_buf trace;
    snag_buf_init(&trace,4096u);
    if (!dir || !profile || !profile_url || !source_url) {
        snag_errorf(error,size,"Office conversion workspace is unavailable");
        goto out;
    }
    if (snag_office_profile(profile,error,size)!=0) goto out;
    if (snag_convert_internal(args,dir,&trace,pump,opaque,wake,error,size)!=0) goto out;
    if (snag_media_work_check(work)<0) {
        snag_errorf(error,size,"Office temporary files exceeded 256 MiB / 4096 entries / depth 32, or became unsafe");
        goto out;
    }
    /* The command line writes <basename>.pdf into the work directory. */
    const char *base=strrchr(path,'/');
    base=base?base+1:path;
    char stem[SNAG_PATH_MAX_BYTES], pdf_name[SNAG_PATH_MAX_BYTES];
    if (snprintf(stem,sizeof(stem),"%s",base)>=(int)sizeof(stem)) {
        snag_errorf(error,size,"Office source name is too long");
        goto out;
    }
    char *dot=strrchr(stem,'.');
    if (dot) *dot=0;
    if (snprintf(pdf_name,sizeof(pdf_name),"%s.pdf",stem)>=(int)sizeof(pdf_name)) {
        snag_errorf(error,size,"Office output name is too long");
        goto out;
    }
    int fd=snag_open_inspect_path(dir,pdf_name);
    snag_file_info st;
    if (fd<0 || snag_fstat(fd,&st)<0 || st.st_size<5 || st.st_size>32u*1024u*1024u-4096u) {
        snag_errorf(error,size,"Office PDF is absent or exceeds 32 MiB: %.200s",
            trace.data?(const char *)trace.data:"");
        if (fd>=0) close(fd);
        goto out;
    }
    char *pdf_path=snag_path_join(dir,pdf_name);
    struct snag_pdf *pdf=NULL; unsigned int produced=0;
    int opened=pdf_path?snag_pdf_open(pdf_path,NULL,NULL,&pdf,&produced,error,size):-1;
    free(pdf_path); snag_pdf_close(pdf);
    if (opened || produced!=last-first+1u) {
        if (!opened) snag_errorf(error,size,"Office export returned %u pages for requested range %u-%u",
            produced,first,last);
        close(fd);
        goto out;
    }
    char block[65536]; ssize_t got;
    while ((got=read(fd,block,sizeof(block)))>0)
        if (snag_buf_append(out,block,(size_t)got)<0) {
            snag_errorf(error,size,"Converted Office PDF exceeds the caller's output budget");
            close(fd);
            goto out;
        }
    close(fd);
    if (got<0) {
        snag_errorf(error,size,"Cannot read the converted Office PDF: %s",strerror(errno));
        goto out;
    }
    char coverage[1536];
    snprintf(coverage,sizeof(coverage),
        "LibreOffice command line (%s): imported document page %u-%u only. "
        "Layout, fonts and computed values may differ from the originating application. "
        "Macros, scripts and link updates disabled by a private profile; the runtime is the user's own installation. "
        "Other pages, slides, sheets, cells and speaker notes uninspected.",
        office_filter(mime),first,last);
    *metadata=json_pack("{s:s,s:s,s:s}","format","pdf","page_kind","Imported document page",
        "coverage",coverage);
    rc=*metadata?0:-1;
out:
    if (rc) {
        out->len=original; json_decref(*metadata); *metadata=NULL;
        /* Never return an unexplained failure: name the status and whatever the
         * engine printed, so a host-level problem is diagnosable from the error. */
        if (error && size && !error[0])
            snag_errorf(error,size,"Office command export failed (rc=%d): %.200s",rc,
                trace.data?(const char *)trace.data:"no engine output");
    }
    snag_buf_free(&trace);
    close(work);
    char cleanup_error[256];
    if (snag_media_work_remove(session->dir_fd,cleanup_error,sizeof(cleanup_error))!=0 && !rc) {
        snag_errorf(error,size,"%s",cleanup_error); rc=-1;
    }
    free(dir);free(profile);free(profile_url);free(source_url);free(engine);
    return rc;
}
#else
int snag_office_worker(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1],"--internal-office-pdf")) return -1;
    fputs("This custom build excludes Office import\n",stderr); _exit(1);
    return 1;
}
int snag_office_export(struct snag_session *s, const char *p, const char *m, unsigned int f, unsigned int l,
                        const struct snag_sheet_range *range, int (*pump)(void *, unsigned int), void *opaque, snag_wake_fd wake,
                        struct snag_buf *out, json_t **metadata, char *error, size_t size)
{
    (void)range;(void)s;(void)p;(void)m;(void)f;(void)l;(void)pump;(void)opaque;(void)wake;(void)out;*metadata=NULL;
    snag_errorf(error,size,"This custom build excludes Office import (WITH_OFFICE=0)"); return -1;
}
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#include "office_sheet.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
bool snag_sheet_range_valid(const struct snag_sheet_range *r)
{
    return r && r->sheet && r->sheet <= 10000u && r->row && r->row <= 1048576u &&
        r->column && r->column <= 16384u && r->rows && r->rows <= 200u &&
        r->columns && r->columns <= 32u && r->rows <= 1048577u-r->row && r->columns <= 16385u-r->column;
}
#if SNAJPAGENT_OFFICE
#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <png.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

static void column_name(uint32_t col, char name[8])
{
    char reverse[8]; unsigned int n=0;
    do { --col; reverse[n++]=(char)('A'+col%26u); col/=26u; } while(col);
    for (unsigned int i=0;i<n;++i) name[i]=reverse[n-i-1u];
    name[n]='\0';
}

static bool element(const xmlNode *n,const char *name)
{
    return n && n->type==XML_ELEMENT_NODE && !xmlStrcmp(n->name,BAD_CAST name);
}

static int cell_text(xmlNode *node, struct snag_buf *out, unsigned int depth)
{
    if(depth>32u)return -1;
    for(;node;node=node->next) {
        if(node->type==XML_TEXT_NODE) {
            if(node->content && snag_buf_append(out,node->content,xmlStrlen(node->content))<0)return -1;
        } else if(element(node,"br")) { if(snag_buf_putc(out,'\n')<0)return -1; }
        else if(node->type==XML_ELEMENT_NODE && !element(node,"comment") && !element(node,"style")) {
            if(cell_text(node->children,out,depth+1u)<0)return -1;
        }
    }
    return 0;
}
static unsigned int span(xmlNode *node,const char *name)
{
    xmlChar *value=xmlGetProp(node,BAD_CAST name);
    if(!value)return 1;
    char *end; unsigned long n=strtoul((char *)value,&end,10);
    bool valid=n>0 && n<=200u && !*end; xmlFree(value);
    return valid?(unsigned int)n:0u;
}
struct table_read {
    const struct snag_sheet_range *range;
    struct snag_buf *out;
    uint32_t row;
    uint32_t owner[6400]; /* merged-cell anchor plus one, zero means unoccupied */
};
static int table_rows(xmlNode *node,struct table_read *t,unsigned int depth)
{
    if(depth>8u)return -1;
    const struct snag_sheet_range *r=t->range;
    for(;node;node=node->next) {
        if(element(node,"tbody") || element(node,"thead") || element(node,"tfoot")) {
            if(table_rows(node->children,t,depth+1u)<0)return -1;
            continue;
        }
        if(!element(node,"tr"))continue;
        if(t->row>=r->rows)return -1;
        uint32_t col=0;
        for(xmlNode *cell=node->children;cell;cell=cell->next) {
            if(!element(cell,"td") && !element(cell,"th"))continue;
            while(col<r->columns && t->owner[t->row*r->columns+col])++col;
            if(col>=r->columns)return -1;
            unsigned int wide=span(cell,"colspan"), high=span(cell,"rowspan");
            if(!wide || !high || wide>r->columns-col || high>r->rows-t->row)return -1;
            uint32_t anchor=t->row*r->columns+col+1u;
            for(uint32_t y=t->row;y<t->row+high;++y) for(uint32_t x=col;x<col+wide;++x) {
                if(t->owner[y*r->columns+x])return -1;
                t->owner[y*r->columns+x]=anchor;
            }
            struct snag_buf value; snag_buf_init(&value,65536u);
            int rc=0;
            /* LOK HTML's data-sheets-value preserves quoted strings/newlines
             * without conflating an empty-cell <br> with a literal newline. */
            xmlChar *attr=xmlGetProp(cell,BAD_CAST "data-sheets-value");
            json_t *encoded=attr?json_loadb((char *)attr,strlen((char *)attr),JSON_REJECT_DUPLICATES,NULL):NULL;
            const char *literal=snag_json_string(encoded,"2");
            if(literal) rc=snag_buf_append(&value,literal,strlen(literal));
            else rc=cell_text(cell->children,&value,0u);
            if(!literal && value.len==1u && value.data[0]=='\n')value.len=0;
            xmlFree(attr); json_decref(encoded);
            if(!rc)rc=snag_buf_terminate(&value);
            char column[8]; column_name(r->column+col,column);
            if(!rc)rc=snag_buf_printf(t->out,"%s%u",column,r->row+t->row);
            if(!rc && (wide>1u || high>1u)) {
                char last[8];column_name(r->column+col+wide-1u,last);
                rc=snag_buf_printf(t->out," (merged through %s%u)",last,r->row+t->row+high-1u);
            }
            json_t *text=!rc?json_string((char *)value.data):NULL;
            struct snag_buf quoted;snag_buf_init(&quoted,256u*1024u);
            if(!text || snag_json_canonical(text,&quoted)<0 || snag_buf_append(t->out,": ",2u)<0 ||
                snag_buf_append(t->out,quoted.data,quoted.len)<0 || snag_buf_putc(t->out,'\n')<0)rc=-1;
            snag_buf_free(&quoted);json_decref(text);snag_buf_free(&value);if(rc)return -1;
            col+=wide;
        }
        for(uint32_t x=0;x<r->columns;++x) {
            uint32_t at=t->row*r->columns+x, anchor=t->owner[at];
            char column[8];column_name(r->column+x,column);
            if(!anchor) {
                if(snag_buf_printf(t->out,"%s%u: \"\"\n",column,r->row+t->row)<0)return -1;
            } else if(anchor!=at+1u) {
                char owner[8];column_name(r->column+(anchor-1u)%r->columns,owner);
                if(snag_buf_printf(t->out,"%s%u: covered by merged %s%u\n",column,r->row+t->row,
                    owner,r->row+(anchor-1u)/r->columns)<0)return -1;
            }
        }
        ++t->row;
    }
    return 0;
}
int
snag_office_sheet_html(const char *html,const struct snag_sheet_range *range,struct snag_buf *out)
{
    size_t length=strnlen(html,1048577u), original=out->len;
    if(length>1024u*1024u || !snag_sheet_range_valid(range))return -1;
    htmlDocPtr doc=htmlReadMemory(html,(int)length,NULL,"UTF-8",HTML_PARSE_NONET|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING);
    if(!doc)return -1;
    xmlNode *root=xmlDocGetRootElement(doc), *body=NULL,*table=NULL;
    for(xmlNode *n=root?root->children:NULL;n;n=n->next) if(element(n,"body"))body=n;
    for(xmlNode *n=body?body->children:NULL;n;n=n->next) if(element(n,"table")) {
        if(table){xmlFreeDoc(doc);return -1;}table=n;
    }
    struct table_read t={.range=range,.out=out};
    int rc=table?table_rows(table->children,&t,0u):-1;
    if(t.row!=range->rows)rc=-1;
    xmlFreeDoc(doc);if(rc)out->len=original;return rc;
}

/* Only pinned LOK callbacks are used; no executable commands from the model. */
struct selection {
    pthread_mutex_t mutex;
    bool done, success, rectangle;
    int x,y,width,height;
    char request[80];
};
static void selected(int type,const char *payload,void *opaque)
{
    struct selection *s=opaque;
    if((type!=16 && type!=42) || !payload || strnlen(payload,65537u)>65536u)return;
    pthread_mutex_lock(&s->mutex);
    if(type==42) {
        char extra;
        s->rectangle=sscanf(payload,"%d, %d, %d, %d %c",&s->x,&s->y,&s->width,&s->height,&extra)==4;
    } else {
        json_t *json=json_loadb(payload,strlen(payload),JSON_REJECT_DUPLICATES,NULL);
        const char *command=snag_json_string(json,"commandName");
        if(command && !strcmp(command,".uno:GoToCell")) {
            const char *value=snag_json_string(json_object_get(json,"result"),"value");
            s->success=json_is_true(json_object_get(json,"success")) && value && !strcmp(value,s->request);
            s->done=true;
        }
        json_decref(json);
    }
    pthread_mutex_unlock(&s->mutex);
}

int
snag_office_sheet(LibreOfficeKit *office,LibreOfficeKitDocument *doc,const struct snag_sheet_range *range,
                   struct snag_buf *png,json_t **metadata,char *error,size_t error_size)
{
    *metadata=NULL;
    if(!snag_sheet_range_valid(range) || doc->pClass->getDocumentType(doc)!=1 ||
        range->sheet>(uint32_t)doc->pClass->getParts(doc))goto invalid;
    static struct selection state={.mutex=PTHREAD_MUTEX_INITIALIZER}; /* one import per child */
    char first[8],last[8],args[160];
    column_name(range->column,first);column_name(range->column+range->columns-1u,last);
    snprintf(state.request,sizeof(state.request),"$%s$%u:$%s$%u",first,range->row,last,range->row+range->rows-1u);
    snprintf(args,sizeof(args),"{\"ToPoint\":{\"type\":\"string\",\"value\":\"%s\"}}",state.request);
    doc->pClass->initializeForRendering(doc,"{}");
    doc->pClass->setPart(doc,(int)range->sheet-1);
    doc->pClass->registerCallback(doc,selected,&state);
    doc->pClass->postUnoCommand(doc,".uno:GoToCell",args,true);
    bool ready=false,valid=false; int x=0,y=0,width=0,height=0;
    uint64_t deadline=snag_monotonic_ms()+3000u;
    do {
        pthread_mutex_lock(&state.mutex);
        ready=state.done;valid=state.success && state.rectangle;
        x=state.x;y=state.y;width=state.width;height=state.height;
        pthread_mutex_unlock(&state.mutex);
        if(ready)break;
        nanosleep(&(struct timespec){.tv_nsec=1000000},NULL);
    } while(snag_monotonic_ms()<deadline);
    doc->pClass->registerCallback(doc,NULL,NULL);

    if(!ready || !valid || x<0 || y<0 || width<=0 || height<=0 ||
        width>1000000 || height>1000000 || x>INT32_MAX-width || y>INT32_MAX-height)goto invalid;
    /* A merged cell intersecting the selection may change its origin; do not
     * silently relabel the expanded selection as the requested cell grid. */
    char *cursor=doc->pClass->getCommandValues(doc,".uno:CellCursor");
    json_t *value=cursor?json_loadb(cursor,strlen(cursor),JSON_REJECT_DUPLICATES,NULL):NULL;
    const char *coords=snag_json_string(value,"commandValues");
    int cx,cy,cw,ch,col,row;char extra;
    valid=coords && sscanf(coords,"%d, %d, %d, %d, %d, %d %c",&cx,&cy,&cw,&ch,&col,&row,&extra)==6 &&
        col==(int)range->column-1 && row==(int)range->row-1;
    json_decref(value);
    if(cursor)office->pClass->freeError(cursor);
    if(!valid)goto invalid;
    char *html=doc->pClass->getTextSelection(doc,"text/html",NULL);
    struct snag_buf cells;snag_buf_init(&cells,256u*1024u);
    int rc=html?snag_office_sheet_html(html,range,&cells):-1;
    if(html)office->pClass->freeError(html);
    if(!rc)rc=snag_buf_terminate(&cells);
    if(rc){snag_buf_free(&cells);goto invalid;}
    char *name=doc->pClass->getPartName(doc,(int)range->sheet-1);
    char *info=doc->pClass->getPartInfo(doc,(int)range->sheet-1);
    json_t *partinfo=info?json_loadb(info,strlen(info),JSON_REJECT_DUPLICATES,NULL):NULL;
    if(info)office->pClass->freeError(info);
    if(partinfo)(void)json_object_del(partinfo,"hash");
    *metadata=json_pack("{s:s,s:s,s:i,s:i,s:s,s:O}","format","png","sheet_name",name?name:"",
        "sheet",(int)range->sheet,"sheet_count",doc->pClass->getParts(doc),"cells",(char *)cells.data,
        "sheet_info",partinfo?partinfo:json_null());
    if(name)office->pClass->freeError(name);
    json_decref(partinfo);snag_buf_free(&cells);
    if(!*metadata)goto invalid;
    double factor=fmin(1.0/15.0,fmin(1600.0/width,1600.0/height));
    int w=(int)ceil(width*factor),h=(int)ceil(height*factor);
    if(w<1)w=1;
    if(h<1)h=1;
    unsigned char *rgba=calloc((size_t)w*h,4u);
    if(!rgba)goto failed;
    doc->pClass->paintPartTile(doc,rgba,(int)range->sheet-1,0,w,h,x,y,width,height);
    int mode=doc->pClass->getTileMode(doc);
    if(mode!=0 && mode!=1){free(rgba);goto failed;}
    /* LOK supplies premultiplied pixels. Composite on white, not dark alpha. */
    for(size_t i=0;i<(size_t)w*h;++i) {
        unsigned int alpha=rgba[i*4u+3u];
        if(mode==1){unsigned char b=rgba[i*4u];rgba[i*4u]=rgba[i*4u+2u];rgba[i*4u+2u]=b;}
        for(size_t c=0;c<3u;++c) {unsigned int v=rgba[i*4u+c]+255u-alpha;rgba[i*4u+c]=(unsigned char)(v>255u?255u:v);}
        rgba[i*4u+3u]=255u;
    }
    png_image image={.version=PNG_IMAGE_VERSION,.width=(png_uint_32)w,.height=(png_uint_32)h,.format=PNG_FORMAT_RGBA};
    png_alloc_size_t bytes=0;
    bool encoded=png_image_write_to_memory(&image,NULL,&bytes,0,rgba,0,NULL) && bytes<=png->max-png->len &&
        snag_buf_reserve(png,(size_t)bytes)==0 && png_image_write_to_memory(&image,png->data+png->len,&bytes,0,rgba,0,NULL);
    free(rgba);png_image_free(&image);
    if(!encoded)goto failed;
    png->len+=(size_t)bytes;return 0;
failed:
    json_decref(*metadata);*metadata=NULL;
invalid:
    snag_errorf(error,error_size,"Cannot map/render selected sheet range safely; select an existing, narrower unambiguous range");return -1;
}
#else
int snag_office_sheet_html(const char *h,const struct snag_sheet_range *r,struct snag_buf *b)
{(void)h;(void)r;(void)b;return -1;}
int snag_office_sheet(struct LibreOfficeKitStruct *o,struct LibreOfficeKitDocumentStruct *d,
                       const struct snag_sheet_range *r,
                       struct snag_buf *b,json_t **m,char *e,size_t n)
{(void)o;(void)d;(void)r;(void)b;*m=NULL;snag_errorf(e,n,"This custom build excludes Office sheets");return -1;}
#endif

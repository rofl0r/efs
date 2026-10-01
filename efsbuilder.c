/*
 * efsbuilder.c - Build an EFS v1 image from a directory tree.
 *
 * Strategy:
 *  - Recursively walk source directory.
 *  - For each directory: collect entry names + is_dir flag + file sizes
 *    (no file contents in RAM). Generate MPH from names only.
 *  - Write efs_dir header, hashtab, name_offset[], leave hole for
 *    entry_offset[], then names blob.
 *  - Stream each entry's data (file bytes or child dir blob) to output
 *    in 16K chunks from stack buffer. No whole-file buffering.
 *  - Back-patch entry_offset[] after sizes known.
 *  - Only per-directory metadata + MPH data kept in RAM; tree never fully
 *    resident.
 */

#define EFS_IMPL
#define EFS_BUILDER
#include "efs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#define u32 uint32_t
#define u8 uint8_t

#define COPY_CHUNK 16384

#ifdef EFS_BENCH
uint64_t efs_mph_build_ns;

static uint64_t monotonic_ns(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

/* ---------- directory building ---------- */

struct ent {
    char *name;
    int is_dir;
    char *src;
    uint32_t size; /* for files only */
};

static void broken(const char* name) {
    fprintf(stderr, "warning: skipping %s: %s\n", name, strerror(errno));
}

static int copy_stream(FILE *out, uint64_t *pos, const char *src, uint32_t *outlen){
    FILE *f=fopen(src,"rb");
    if(!f){
        broken(src);
        return 0;   /* caller leaves size as-is (0) */
    }
    uint8_t buf[COPY_CHUNK];
    uint64_t start=*pos;
    fseek(out,(long)*pos,SEEK_SET);
    for(;;){
        size_t r=fread(buf,1,sizeof buf,f);
        if(!r) break;
        fwrite(buf,1,r,out);
        *pos+=r;
    }
    fclose(f);
    if(outlen) *outlen=(uint32_t)(*pos-start);
    return 1;
}

static uint32_t build_dir(FILE *out, uint64_t *pos, const char *path){
    DIR *d=opendir(path);
    struct ent *es=0; uint32_t n=0, cap=0;
    struct dirent *de;
    while((de=readdir(d))){
        char *nm=de->d_name;
        if(nm[0]=='.' && (nm[1]==0 || (nm[1]=='.' && nm[2]==0))) continue;
        char full[4096]; snprintf(full,sizeof full,"%s/%s",path,nm);
        if(n==cap){ cap=cap?cap*2:16; es=realloc(es,cap*sizeof *es); }
        es[n].src=strdup(full);
        struct stat st;
        if(stat(full,&st)!=0) {
            broken(full);
            es[n].name=strdup(nm);
            es[n].is_dir = 0;
            es[n].size = 0;
            n++;
            continue;
        }
        if(S_ISDIR(st.st_mode)){
            es[n].is_dir=1; es[n].size=0;
            char *dn=malloc(strlen(nm)+2);
            sprintf(dn,"/%s",nm);
            es[n].name = dn;
        }
        else {
            es[n].name=strdup(nm);
            es[n].is_dir=0; es[n].size=st.st_size;
        }
        n++;
    }
    closedir(d);

    /* build MPH.
     * Names are per-directory components, bounded by the filename limit
     * (<= 4 KB even for PATH_MAX); the unified MPH API takes 32-bit key
     * lengths. */
    char **keys=malloc(n*sizeof(char*));
    uint32_t *kl=malloc(n*sizeof(uint32_t));
    for(uint32_t i=0;i<n;i++){ keys[i]=es[i].name; kl[i]=(uint32_t)strlen(es[i].name); }
    struct efs_mph_in mi={n,(const char*const*)keys,kl};
    struct efs_mph_out mo;
#ifdef EFS_BENCH
    uint64_t mph_start = monotonic_ns();
#endif
    if(!efs_mph_build(&mi,&mo)){ fprintf(stderr,"mph fail in %s\n",path); exit(1); }
#ifdef EFS_BENCH
    efs_mph_build_ns += monotonic_ns() - mph_start;
#endif

    /* local MPH index (mirrors efs.h reader). The key length is taken from
     * the kl[] array we already built, so we don't re-scan the string here
     * (efs_mph_index only needs the length). */
    uint32_t *order = malloc(n * sizeof(uint32_t));
    for(uint32_t i=0;i<n;i++) order[i] = i;
    for(uint32_t i=1;i<n;i++){
        uint32_t key = order[i];
        uint32_t j = i;
        while(j>0 && efs_mph_index(mo.data, mo.blen, mo.shift, mo.salt, mo.w, es[order[j-1]].name, kl[order[j-1]])
                     > efs_mph_index(mo.data, mo.blen, mo.shift, mo.salt, mo.w, es[key].name, kl[key])){
            order[j] = order[j-1]; j--;
        }
        order[j] = key;
    }

    uint32_t names_len=0;
    for(uint32_t i=0;i<n;i++) names_len += (uint32_t)kl[order[i]] + 1;
    while(names_len & 3) names_len++;
    uint8_t *names = malloc(names_len);
    uint32_t *name_off = malloc(n * sizeof(uint32_t));
    uint32_t off=0;
    for(uint32_t i=0;i<n;i++){
        const char *nm = es[order[i]].name;
        uint32_t l = kl[order[i]];
        name_off[i] = off;
        memcpy(names+off, nm, l+1);
        off += (uint32_t)l + 1;
    }

    uint64_t dir_start=*pos;
    struct efs_dir hdr={0};
    hdr.count=n; hdr.blen=mo.blen; hdr.shift=mo.shift; hdr.salt=mo.salt; hdr.w=mo.w; hdr.names_len=names_len;
    fseek(out,(long)dir_start,SEEK_SET);
    fwrite(&hdr,1,sizeof hdr,out);
    fwrite(mo.data,1,mo.len,out);
    fwrite(name_off,1,4*n,out);
    uint64_t eoff_pos=ftell(out);
    uint32_t *entry_off=malloc((n+1)*sizeof(uint32_t));
    fseek(out,4*(n+1),SEEK_CUR);
    fwrite(names,1,names_len,out);

    uint64_t data_pos=ftell(out);
    for(uint32_t i=0;i<n;i++){
        entry_off[i]=(uint32_t)(data_pos-dir_start);
        uint32_t src = order[i];
        if(es[src].is_dir){
            uint32_t clen=build_dir(out,&data_pos,es[src].src);
            (void)clen;
        } else {
            int ok=copy_stream(out,&data_pos,es[src].src,0);
            (void)ok;  /* if !ok, data_pos unchanged -> zero-length entry */
        }
    }
    entry_off[n]=(uint32_t)(data_pos-dir_start);
    fseek(out,(long)eoff_pos,SEEK_SET);
    fwrite(entry_off,1,4*(n+1),out);
    *pos=data_pos;

    /* cleanup */
    for(uint32_t i=0;i<n;i++){ free(es[i].name); free(es[i].src); }
    free(es); free(keys); free(kl); free(mo.data); free(names); free(name_off); free(entry_off); free(order);
    return (uint32_t)(data_pos-dir_start);
}

/* Library entry point: build an EFS image of the directory tree at srcpath
 * and write it to outpath (with the EFS magic prepended). Returns 0 on
 * success, -1 on failure. Used by the CLI below and by efstest. */
EFS_BUILD_API int efs_build_path(const char *srcpath, const char *outpath){
#ifdef EFS_BENCH
    efs_mph_build_ns = 0;
#endif
    FILE *out=fopen(outpath,"wb");
    if(!out){ perror("fopen"); return -1; }
    fwrite(EFS_MAGIC,1,4,out);
    uint64_t pos=4;
    build_dir(out,&pos,srcpath);
    fclose(out);
    return 0;
}

#ifndef EFS_NO_MAIN
int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: %s out.efs dir\n",argv[0]); return 1; }
    return efs_build_path(argv[2], argv[1]) == 0 ? 0 : 1;
}
#endif

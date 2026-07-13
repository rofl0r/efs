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
#include "efs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#define u32 uint32_t
#define u8 uint8_t

#define COPY_CHUNK 16384

/* ---------- MPH generator (minimal port) ---------- */

#define USE_SCRAMBLE 4096

#define mix(a,b,c) \
{ a-=b;a-=c;a^=c>>13;b-=c;b-=a;b^=a<<8;c-=a;c-=b;c^=b>>13; \
  a-=b;a-=c;a^=c>>12;b-=c;b-=a;b^=a<<16;c-=a;c-=b;c^=b>>5; \
  a-=b;a-=c;a^=c>>3;b-=c;b-=a;b^=a<<10;c-=a;c-=b;c^=b>>15; }

typedef struct { u32 val_b; u32 len; u32 water; u32 off; } Bstuff;

static uint32_t mylog2(uint32_t v){ uint32_t i; for(i=0;((uint32_t)1<<i)<v;i++); return i; }

static uint32_t lookup_k(const uint8_t *k, uint32_t len, uint32_t level){
    uint32_t a,b,c,o=len;
    a=b=0x9e3779b9; c=level;
    while(len>=12){
        a+=(k[0]|k[1]<<8|k[2]<<16|k[3]<<24);
        b+=(k[4]|k[5]<<8|k[6]<<16|k[7]<<24);
        c+=(k[8]|k[9]<<8|k[10]<<16|k[11]<<24);
        mix(a,b,c); k+=12; len-=12;
    }
    c+=o;
    switch(len){
    case 11:c+=k[10]<<24; case 10:c+=k[9]<<16; case 9:c+=k[8]<<8;
    case 8:b+=k[7]<<24; case 7:b+=k[6]<<16; case 6:b+=k[5]<<8; case 5:b+=k[4];
    case 4:a+=k[3]<<24; case 3:a+=k[2]<<16; case 2:a+=k[1]<<8; case 1:a+=k[0];
    }
    mix(a,b,c);
    return c;
}

static uint32_t permute(uint32_t x, uint32_t nbits){
    uint32_t mask=((uint32_t)1<<nbits)-1;
    int c2=1+nbits/2,c3=1+nbits/3,c4=1+nbits/4,c5=1+nbits/5;
    for(int i=0;i<20;i++){
        x=(x+(x<<c2))&mask; x^=x>>c3;
        x=(x+(x<<c4))&mask; x^=x>>c5;
    }
    return x;
}

struct mph_in { uint32_t n; char **keys; unsigned short *kl; };
struct mph_out { uint32_t blen, shift, salt, w; uint8_t *data; };

static int solve(u32 b, u32 *ka, u32 *kb, u32 *scramble, u32 limit, u32 n,
                 Bstuff *tabb, u32 *flat_list, u32 *tabh, u32 tag){
    tabb[b].water = tag;
    for(u32 i=0; i<limit; i++){
        int conflict=0;
        u32 child=0xFFFFFFFF;
        for(u32 j=0;j<tabb[b].len;j++){
            u32 ki=flat_list[tabb[b].off+j];
            u32 h=ka[ki]^scramble[i];
            if(h>=n){conflict=1;break;}
            if(tabh[h]!=0xFFFFFFFF){
                u32 ck=tabh[h];
                u32 cb=kb[ck];
                if(tabb[cb].water==tag){conflict=1;break;}
                if(child==0xFFFFFFFF) child=cb;
                else if(child!=cb){conflict=1;break;}
            }
        }
        if(conflict) continue;
        if(child==0xFFFFFFFF){
            tabb[b].val_b=i;
            for(u32 j=0;j<tabb[b].len;j++){
                u32 ki=flat_list[tabb[b].off+j];
                tabh[ka[ki]^scramble[i]]=ki;
            }
            return 1;
        } else {
            u32 old_i=tabb[child].val_b;
            for(u32 j=0;j<tabb[child].len;j++){
                u32 ki=flat_list[tabb[child].off+j];
                tabh[ka[ki]^scramble[old_i]]=0xFFFFFFFF;
            }
            tabb[b].val_b=i;
            for(u32 j=0;j<tabb[b].len;j++){
                u32 ki=flat_list[tabb[b].off+j];
                tabh[ka[ki]^scramble[i]]=ki;
            }
            if(solve(child,ka,kb,scramble,limit,n,tabb,flat_list,tabh,tag)) return 1;
            for(u32 j=0;j<tabb[b].len;j++){
                u32 ki=flat_list[tabb[b].off+j];
                tabh[ka[ki]^scramble[i]]=0xFFFFFFFF;
            }
            for(u32 j=0;j<tabb[child].len;j++){
                u32 ki=flat_list[tabb[child].off+j];
                tabh[ka[ki]^scramble[old_i]]=ki;
            }
            tabb[child].val_b = old_i;
        }
    }
    return 0;
}

static void heap_sift(Bstuff *tabb, u32 *order, u32 n, u32 start){
    u32 root = start;
    for(;;){
        u32 child = 2*root + 1;
        if(child >= n) break;
        if(child+1 < n && tabb[order[child]].len > tabb[order[child+1]].len)
            child++;
        if(tabb[order[root]].len <= tabb[order[child]].len)
            break;
        u32 t = order[root]; order[root] = order[child]; order[child] = t;
        root = child;
    }
}

static void heap_sort_buckets(Bstuff *tabb, u32 *order, u32 n){
    if(n == 0) return;
    for(u32 i = n/2; i-- > 0; )
        heap_sift(tabb, order, n, i);
    for(u32 end = n-1; end > 0; end--){
        u32 t = order[0]; order[0] = order[end]; order[end] = t;
        heap_sift(tabb, order, end, 0);
    }
}

/* (solve/heap omitted for space; identical to earlier finalized code) */
static int gen_mph(const struct mph_in *in, struct mph_out *out){
    const u32 n = in->n;
    if(!n){
        out->blen = 1;
        out->shift = 31;  /* or 32 - mylog2(1) */
        out->salt = 0;
        out->data = calloc(1, mph_bytes(1));
        return 1;
    }
    u32 smax=1; while(smax<n) smax<<=1;
    if(smax < 2) smax = 2;
    u32 alen, blen;
    u32 sl = mylog2(smax);
    if(sl <= 8){ alen=smax/2; blen=smax/2; }
    else if(sl <= 17){
        alen = (n <= smax*0.52) ? smax/8 : smax/4;
        blen = (n <= smax*0.52) ? smax/8 : smax/4;
        if(blen >= USE_SCRAMBLE) blen = smax/4;
    } else if(sl == 18){
        alen = smax/8;
        blen = (n <= smax*5/8) ? smax/4 : smax/2;
    } else {
        alen = (n <= smax*5/8) ? smax/8 : smax/2;
        blen = (n <= smax*5/8) ? smax/4 : smax/2;
    }
    if(blen<2) blen=2;
    if(alen<2) alen=2;

    u32 shift = (alen > 1) ? 32 - mylog2(alen) : 0;
    u32 mask = blen-1;
    int use_scramble = (blen >= USE_SCRAMBLE);

    u32 *scramble = malloc(smax * sizeof(u32));
    u32 *vbuf=malloc(n*sizeof(u32));
    u32 *ka=malloc(n*sizeof(u32));
    u32 *kb=malloc(n*sizeof(u32));
    Bstuff *tabb = calloc(smax, sizeof(Bstuff));
    u32 *flat_list = malloc(n*sizeof(u32));
    u32 *counts = calloc(smax, sizeof(u32));
    u32 *order = malloc(smax * sizeof(u32));
    u32 *tabh = malloc(n*sizeof(u32));
    if(!scramble||!vbuf||!ka||!kb||!tabb||!flat_list||!counts||!order||!tabh){
        perror("malloc"); goto fail;
    }

    for(u32 i=0;i<smax;i++) scramble[i]=permute(i, sl);

    int ok=0;
    u32 salt=0;
    for(u32 attempt=1; attempt<1000000 && !ok; attempt++){
        salt=attempt;
        u32 seed=salt*0x9e3779b9;
        for(u32 i=0;i<n;i++) vbuf[i]=lookup_k((u8*)in->keys[i],in->kl[i],seed);
        for(u32 i=0;i<n;i++){
            ka[i] = (alen>1)? vbuf[i]>>shift : 0;
            kb[i] = (blen>1)? vbuf[i]&mask : 0;
        }

        memset(tabb, 0, blen*sizeof(Bstuff));
        memset(counts, 0, blen*sizeof(u32));

        for(u32 i=0;i<n;i++){ u32 b = kb[i]; counts[b]++; }

        u32 total_entries = 0;
        for(u32 i=0;i<blen;i++){ tabb[i].off = total_entries; total_entries += counts[i]; }

        int bad = 0;
        for(u32 i=0;i<n;i++){
            u32 b = kb[i];
            u32 base = tabb[b].off;
            /* only check early collisions for bigger N, there it's an
               efficient filter, for small ones it can actually cause
               failure to find a bucket. */
            if(n > 256) {
                for(u32 j=0;j<tabb[b].len;j++){
                    u32 other=flat_list[base+j];
                    if(ka[other]==ka[i]) { bad=1; break; }
                }
                if(bad) break;
            }
            flat_list[base + tabb[b].len++] = i;
        }
        if(bad) continue;

        for(u32 i=0;i<n;i++) tabh[i] = 0xFFFFFFFF;
        ok=1;

        u32 na=0;
        for(u32 i=0;i<blen;i++) if(tabb[i].len) order[na++]=i;
        heap_sort_buckets(tabb, order, na);
        for(u32 i=0;i<blen;i++) if(!tabb[i].len) order[na++]=i;

        u32 limit = use_scramble ? 256 : smax;
        u32 tag=1;
        for(u32 x=0;x<blen;x++){
            u32 b=order[x];
            if(!tabb[b].len) continue;
            u32 ki=flat_list[tabb[b].off];
            if(tabh[ka[ki]^scramble[tabb[b].val_b]] == ki) continue;
            if(!solve(b,ka,kb,scramble,limit,n,tabb,flat_list,tabh,tag++)){ ok=0; break; }
        }

        if(ok){
            u32 placed = 0;
            for(u32 i=0;i<n;i++) tabh[i]=0xFFFFFFFF;
            for(u32 x=0;x<blen;x++){
                u32 b=order[x];
                if(!tabb[b].len) continue;
                for(u32 j=0;j<tabb[b].len;j++){
                    u32 ki=flat_list[tabb[b].off+j];
                    u32 h=ka[ki]^scramble[tabb[b].val_b];
                    if(h>=n || tabh[h]!=0xFFFFFFFF){ ok=0; break; }
                    tabh[h]=ki;
                    placed++;
                }
                if(!ok) break;
            }
            if(ok && placed != n) ok=0;
        }
        if(ok) break;
        if(!ok){
            if(attempt % 32 == 0){
                blen *= 2;
                if(blen > smax) blen = smax;
                mask = blen - 1;
            }
        }
    }

    if(!ok) goto fail;

    u32 total_bytes = mph_bytes(blen);
    out->data = malloc(total_bytes);
    if(!out->data) goto fail;
    memset(out->data, 0, total_bytes);

    if(use_scramble){
        for(u32 b=0;b<blen;b++) out->data[b]=(u8)tabb[b].val_b;
        u32 o=blen;
        u32 w=(mylog2(blen)+7)/8;
        for(u32 i=0;i<256;i++) for(u32 j=0;j<w;j++)
            out->data[o+ i*w +j] = (scramble[i]>>(8*(w-1-j)))&0xFF;
    } else {
        u32 w=(mylog2(blen)+7)/8;
        for(u32 b=0;b<blen;b++){
            u32 disp = scramble[tabb[b].val_b];
            for(u32 j=0;j<w;j++) out->data[b*w+j]=(disp>>(8*(w-1-j)))&0xFF;
        }
    }
    out->blen=blen;
    out->shift=shift;
    out->salt=salt;

    free(scramble);
    free(vbuf); free(ka); free(kb); free(tabb); free(flat_list);
    free(counts); free(order); free(tabh);
    return 1;

fail:
    free(scramble);
    free(vbuf); free(ka); free(kb); free(tabb); free(flat_list);
    free(counts); free(order); free(tabh);
    if(out->data) free(out->data);
    out->data=0;
    return 0;
}

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

static uint32_t idx_of(const char *nm, struct mph_out *mo){
    uint32_t v = lookup_k((const uint8_t*)nm, (uint32_t)strlen(nm), mo->salt * 0x9e3779b9);
    uint32_t a = v >> mo->shift;
    uint32_t b = v & (mo->blen - 1);
    uint32_t w = mph_w(mo->blen);
    uint32_t disp = (mo->blen >= 4096)
        ? tab_load(mo->data + mo->blen + mo->data[b]*w, w)
        : tab_load(mo->data + b*w, w);
    return a ^ disp;
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

    /* build MPH */
    char **keys=malloc(n*sizeof(char*));
    unsigned short *kl=malloc(n*sizeof(unsigned short));
    for(uint32_t i=0;i<n;i++){ keys[i]=es[i].name; kl[i]=strlen(es[i].name); }
    struct mph_in mi={n,keys,kl};
    struct mph_out mo;
    if(!gen_mph(&mi,&mo)){ fprintf(stderr,"mph fail in %s\n",path); exit(1); }

    /* local MPH index (mirrors efs.h reader) */
    uint32_t *order = malloc(n * sizeof(uint32_t));
    for(uint32_t i=0;i<n;i++) order[i] = i;
    for(uint32_t i=1;i<n;i++){
        uint32_t key = order[i];
        uint32_t j = i;
        while(j>0 && idx_of(es[order[j-1]].name, &mo) > idx_of(es[key].name, &mo)){
            order[j] = order[j-1]; j--;
        }
        order[j] = key;
    }

    uint32_t names_len=0;
    for(uint32_t i=0;i<n;i++) names_len += (uint32_t)strlen(es[order[i]].name)+1;
    while(names_len & 3) names_len++;
    uint8_t *names = malloc(names_len);
    uint32_t *name_off = malloc(n * sizeof(uint32_t));
    uint32_t off=0;
    for(uint32_t i=0;i<n;i++){
        const char *nm = es[order[i]].name;
        name_off[i] = off;
        memcpy(names+off, nm, strlen(nm)+1);
        off += (uint32_t)strlen(nm)+1;
    }

    uint64_t dir_start=*pos;
    struct efs_dir hdr;
    hdr.count=n; hdr.blen=mo.blen; hdr.shift=mo.shift; hdr.salt=mo.salt; hdr.names_len=names_len;
    fseek(out,(long)dir_start,SEEK_SET);
    fwrite(&hdr,1,sizeof hdr,out);
    fwrite(mo.data,1,mph_bytes(mo.blen),out);
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

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: %s out.efs dir\n",argv[0]); return 1; }
    FILE *out=fopen(argv[1],"wb");
    if(!out){ perror("fopen"); return 1; }
    fwrite(EFS_MAGIC,1,4,out);
    uint64_t pos=4;
    build_dir(out,&pos,argv[2]);
    fclose(out);
    return 0;
}


#define EFS_IMPL
#include "efs.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t entry_len(const struct efs_dir *d, uint32_t idx){
    const uint32_t *eo = dir_entry_off(d);
    return eo[idx+1] - eo[idx];
}

int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr, "usage: %s image.efs /path/in/fs\n", argv[0]);
        return 1;
    }
    int fd = open(argv[1], O_RDONLY);
    if(fd < 0){ perror("open"); return 1; }
    struct stat st; fstat(fd, &st);
    uint8_t *base = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(base == MAP_FAILED){ perror("mmap"); return 1; }

    if(memcmp(base, EFS_MAGIC, 4) != 0){
        fprintf(stderr, "not an EFS image\n"); return 1;
    }
    const struct efs_dir *root = (const struct efs_dir*)(base + 4);

    uint32_t len;
    int isdir = 0;
    const uint8_t *p = efs_lookup(root, argv[2], &len, &isdir);
    if(!p){
        fprintf(stderr, "not found: %s\n", argv[2]);
        return 1;
    }

    if(isdir){
        const struct efs_dir *sub = (const struct efs_dir*)p;
        uint32_t cur = 0;
        const char *nm;
        while((nm = efs_readdir(sub, &cur))){
            uint32_t idx = cur - 1;   /* cursor was incremented */
            printf("%s\t%u\n", nm, entry_len(sub, idx));
        }
    } else {
        fwrite(p, 1, len, stdout);
    }

    munmap(base, st.st_size);
    close(fd);
    return 0;
}


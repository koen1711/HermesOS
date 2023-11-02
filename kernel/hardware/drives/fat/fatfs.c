#include "../fs_internal.h"
#include "fatfs.h"
#include "lib/ff.h"
#include "../../../cfunctions/string/string.h"
#include "../../../cfunctions/memory/kmalloc.h"
#include "lib/diskio.h"
#include "../../../cfunctions/page/page.h"


static struct fs_dirent *fatfs_volume_root(struct fs_volume *v)
{
    DIR* dir = kmalloc(sizeof(DIR));
    struct fs_dirent *d = kmalloc(sizeof(struct fs_dirent));
    FRESULT result = f_opendir(dir, "0:/");
    d->volume = v;
    d->size = 0;
    d->inumber = 0;
    d->refcount = 1;
    d->isdir = 1;
    d->fatfs_dir = *dir;

    return d;
}

int fatfs_volume_format( struct device *device )
{
    return 0;
}

int fatfs_volume_close( struct fs_volume *v )
{
    f_mount(0, "", 0);
    return 0;
}

extern struct fs fat_fs;


struct fs_volume * fatfs_volume_open( struct device *d )
{
    add_device(d, 0);
    struct fs_volume *v = kmalloc(sizeof(struct fs_volume));
    v->fs = &fat_fs;
    v->device = d;
    v->block_size = device_block_size(d);
    v->refcount = 1;

    FATFS* fatfs = page_alloc(0);
    FRESULT result = f_mount(fatfs, "", 1);
    printf("f_mount result: %d\n", result);
    v->fatfs = *fatfs;
    printf("Cluster size: %d, root dir sector: %d\n", fatfs->csize, fatfs->dirbase);

    char volume_label[12];
    result = f_getlabel("", volume_label, 0);
    if (result == FR_OK)
        printf("Volume label: %s\n", volume_label);
    else {
        // print the return value
        printf("Volume label failed: %d\n", result);
    }

    return v;
}

struct fs_dirent * fatfs_dirent_lookup( struct fs_dirent *d, const char *name ) {
    printf("fatfs_dirent_lookup\n");
    struct fs_dirent *d2 = kmalloc(sizeof(struct fs_dirent));
    d2->volume = d->volume;
    d2->size = 0;
    d2->inumber = 0;
    d2->refcount = 1;
    d2->fatfs_dir = d->fatfs_dir;
    // check if it is a file or directory
    FILINFO fno;
    FRESULT result = f_stat(name, &fno);
    if (result != FR_OK) {
        printf("f_stat result: %d\n", result);
        return 0;
    }
    if (fno.fattrib & AM_DIR) {
        d2->isdir = 1;
        f_opendir(&d2->fatfs_dir, name);
    }
    else {
        d2->isdir = 0;
        result = f_stat(name, &fno);
        if (result != FR_OK) {
            printf("f_stat result: %d\n", result);
            return 0;
        }
        f_open(&d2->fatfs_file, name, FA_READ);
    }
    d2->size = fno.fsize;
    return d2;
}

int fatfs_dirent_list( struct fs_dirent *d, char *buffer, int length )
{
    FILINFO* fno = kmalloc(sizeof(FILINFO));
    int i = 0;
    while (f_readdir(&d->fatfs_dir, fno) == FR_OK) {
        printf("name: %s\n", fno->fname);
        if (fno->fname[0] == 0) break;
        if (i >= length) break;
        strcpy(buffer + i, fno->fname);
        i += strlen(fno->fname);
        strcpy(buffer + i, "\n");
        i++;
    }
    kfree(fno);
    return i;
}

int fatfs_dirent_close( struct fs_dirent *d )
{
    if (d->isdir) {
        printf("Closing directory\n");
        f_closedir(&d->fatfs_dir);
    } else
        f_close(&d->fatfs_file);
    return 0;
}

int fatfs_dirent_read_block( struct fs_dirent *d, char *data, uint32_t blockno )
{
    UINT br;
    f_lseek(&d->fatfs_file, blockno * d->volume->block_size);
    int re = f_read(&d->fatfs_file, data, d->volume->block_size, &br);
    return d->volume->block_size;
}


struct fs_ops fatfs_ops = {
        .volume_open = fatfs_volume_open,
        .volume_close = fatfs_volume_close,
        .volume_format = fatfs_volume_format,
        .volume_root = fatfs_volume_root,

        .lookup = fatfs_dirent_lookup,
//        .mkdir = fatfs_dirent_create_dir,
//        .mkfile = fatfs_dirent_create_file,
        .read_block = fatfs_dirent_read_block,
//        .write_block = fatfs_dirent_write_block,
        .list = fatfs_dirent_list,
//        .remove = fatfs_dirent_remove,
//        .resize = fatfs_dirent_resize,
        .close = fatfs_dirent_close
};

struct fs fat_fs = {
        "fatfs",
        &fatfs_ops,
        0
};

int fatfs_init(void)
{
    fs_register(&fat_fs);
    return 0;
}

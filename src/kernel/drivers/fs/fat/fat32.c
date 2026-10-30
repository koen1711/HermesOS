#include "fat32.h"

#include <hardware/memory/alloc.h>
#include "drivers/terminal/terminal.h"
#include <drivers/timer/clock.h>
#include <utils/str/str.h>

#include <drivers/fs/vfs/vfs.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <os/errno.h>

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

#define FAT_NT_LOWER_BASE 0x08
#define FAT_NT_LOWER_EXT  0x10

#define FAT_ENTRY_MASK    0x0FFFFFFF
#define FAT_EOC           0x0FFFFFFF
#define FAT_FREE          0
#define FSINFO_UNKNOWN    0xFFFFFFFF
#define FSINFO_LEAD_SIG   0x41615252
#define DIRENT_SIZE       32
#define DIRENT_DELETED    0xE5
#define LFN_CHARS         13
#define LFN_LAST          0x40
#define LFN_MAX_ENTRIES   20
#define FAT_NAME_MAX      255
#define WRITE_RUN_MAX     (16u << 20)
#define WRITE_ZERO_CHUNK  (64u << 10)

static inode_operations_t fat32_dir_iops;
static inode_operations_t fat32_file_iops;
static file_operations_t  fat32_dir_fops;
static file_operations_t  fat32_file_fops;

static int bdev_read(const fat32_sb_info *sbi, const uint64_t off, const uint32_t len, void *buf)
{
    return sbi->bdev->ops->read_bytes(sbi->bdev, off, len, buf);
}

static int bdev_write(const fat32_sb_info *sbi, const uint64_t off, const uint32_t len, const void *buf)
{
    if (!sbi->bdev->ops->write_bytes)
        return -EROFS;
    return sbi->bdev->ops->write_bytes(sbi->bdev, off, len, buf) == 0 ? 0 : -EIO;
}

static uint64_t cluster_to_byte(const fat32_sb_info *sbi, const uint32_t cluster)
{
    return (uint64_t)(sbi->data_begin_lba +
           (cluster - 2) * sbi->sectors_per_cluster) * sbi->bytes_per_sector;
}

static uint32_t cluster_byte_size(const fat32_sb_info *sbi)
{
    return sbi->sectors_per_cluster * sbi->bytes_per_sector;
}

static inline int is_eof_cluster(const uint32_t c) { return c >= 0x0FFFFFF8; }

static bool valid_cluster(const fat32_sb_info *sbi, const uint32_t c)
{
    return c >= 2 && c < sbi->total_clusters + 2;
}

static uint64_t fat_entry_byte(const fat32_sb_info *sbi, const int copy, const uint32_t cluster)
{
    return ((uint64_t)sbi->fat_begin_lba + (uint64_t)copy * sbi->sectors_per_fat) * sbi->bytes_per_sector
           + (uint64_t)cluster * 4;
}

static int fat_flush(fat32_sb_info *sbi)
{
    if (!sbi->fat_cache_dirty)
        return 0;
    const uint64_t copy_size = (uint64_t)sbi->sectors_per_fat * sbi->bytes_per_sector;
    for (int copy = 0; copy < (sbi->bs.number_of_fats ? sbi->bs.number_of_fats : 1); copy++) {
        if (bdev_write(sbi, (uint64_t)sbi->fat_cache_sector * 512 + copy * copy_size, 512, sbi->fat_cache) < 0)
            return -EIO;
    }
    sbi->fat_cache_dirty = false;
    return 0;
}

static int fat_get(fat32_sb_info *sbi, const uint32_t cluster, uint32_t *out)
{
    const uint64_t off = fat_entry_byte(sbi, 0, cluster);
    const uint32_t sector = (uint32_t)(off / 512);
    if (!sbi->fat_cache_valid || sbi->fat_cache_sector != sector) {
        if (fat_flush(sbi) < 0)
            return -EIO;
        if (bdev_read(sbi, (uint64_t)sector * 512, 512, sbi->fat_cache) != 0) {
            sbi->fat_cache_valid = false;
            return -EIO;
        }
        sbi->fat_cache_sector = sector;
        sbi->fat_cache_valid = true;
    }
    uint32_t v;
    memcpy(&v, sbi->fat_cache + off % 512, 4);
    *out = v & FAT_ENTRY_MASK;
    return 0;
}

static int fat_set(fat32_sb_info *sbi, const uint32_t cluster, const uint32_t value)
{
    uint32_t old;
    if (fat_get(sbi, cluster, &old) < 0)
        return -EIO;
    uint32_t raw;
    const uint64_t off0 = fat_entry_byte(sbi, 0, cluster);
    memcpy(&raw, sbi->fat_cache + off0 % 512, 4);
    raw = (raw & ~FAT_ENTRY_MASK) | (value & FAT_ENTRY_MASK);
    memcpy(sbi->fat_cache + off0 % 512, &raw, 4);
    sbi->fat_cache_dirty = true;
    return 0;
}

static uint32_t fat32_next_cluster(fat32_sb_info *sbi, const uint32_t cluster)
{
    uint32_t next;
    if (!valid_cluster(sbi, cluster) || fat_get(sbi, cluster, &next) < 0)
        return FAT_EOC;
    return next;
}

static void fsinfo_adjust(fat32_sb_info *sbi, const int delta, const uint32_t hint)
{
    if (sbi->fsinfo.free_cluster_count != FSINFO_UNKNOWN)
        sbi->fsinfo.free_cluster_count += delta;
    if (hint)
        sbi->fsinfo.next_free_cluster = hint;
    sbi->fsinfo_dirty = true;
}

static void fsinfo_sync(fat32_sb_info *sbi)
{
    fat_flush(sbi);
    if (!sbi->fsinfo_dirty || sbi->fsinfo.lead_signature != FSINFO_LEAD_SIG || sbi->bs.fsinfo_sector == 0)
        return;
    bdev_write(sbi, (uint64_t)sbi->bs.fsinfo_sector * sbi->bytes_per_sector, sizeof(sbi->fsinfo), &sbi->fsinfo);
    sbi->fsinfo_dirty = false;
}

static uint32_t alloc_cluster(fat32_sb_info *sbi, const uint32_t prev, const bool zero_fill)
{
    const uint32_t total = sbi->total_clusters;
    const uint32_t hint = sbi->fsinfo.next_free_cluster;
    const uint32_t start = valid_cluster(sbi, hint) ? hint : 2;

    for (uint32_t n = 0; n < total; n++) {
        const uint32_t c = 2 + (start - 2 + n) % total;
        uint32_t v;
        if (fat_get(sbi, c, &v) < 0)
            return 0;
        if (v != FAT_FREE)
            continue;

        if (zero_fill) {
            uint8_t *zero = malloc(cluster_byte_size(sbi));
            if (!zero)
                return 0;
            memset(zero, 0, cluster_byte_size(sbi));
            const int err = bdev_write(sbi, cluster_to_byte(sbi, c), cluster_byte_size(sbi), zero);
            free(zero);
            if (err < 0)
                return 0;
        }
        if (fat_set(sbi, c, FAT_EOC) < 0)
            return 0;
        if (prev && fat_set(sbi, prev, c) < 0)
            return 0;
        fsinfo_adjust(sbi, -1, valid_cluster(sbi, c + 1) ? c + 1 : 2);
        return c;
    }
    return 0;
}

static void free_chain(fat32_sb_info *sbi, uint32_t c)
{
    while (valid_cluster(sbi, c)) {
        const uint32_t next = fat32_next_cluster(sbi, c);
        if (fat_set(sbi, c, FAT_FREE) < 0)
            return;
        fsinfo_adjust(sbi, +1, 0);
        c = next;
    }
}

static int chain_seek(fat32_inode_info *fi, const uint64_t pos, const bool grow, uint32_t *out)
{
    fat32_sb_info *sbi = fi->sbi;
    const uint32_t index = (uint32_t)(pos / cluster_byte_size(sbi));

    if (fi->first_cluster == 0) {
        if (!grow)
            return -EIO;
        const uint32_t c = alloc_cluster(sbi, 0, false);
        if (!c)
            return -ENOSPC;
        fi->first_cluster = c;
        fi->inode->id = c;
    }

    uint32_t c = fi->first_cluster;
    uint32_t i = 0;
    if (fi->cache_cluster && fi->cache_index <= index) {
        c = fi->cache_cluster;
        i = fi->cache_index;
    }
    for (; i < index; i++) {
        uint32_t next = fat32_next_cluster(sbi, c);
        if (!valid_cluster(sbi, next)) {
            if (!grow)
                return -EIO;
            next = alloc_cluster(sbi, c, false);
            if (!next)
                return -ENOSPC;
        }
        c = next;
    }
    fi->cache_index = index;
    fi->cache_cluster = c;
    *out = c;
    return 0;
}

static uint64_t fat_now_secs()
{
    uint64_t ns = 0;
    clock_get_ns(CLOCK_REALTIME_K, &ns);
    return ns / 1000000000ULL;
}

static uint16_t fat_date()
{
    const int64_t z = (int64_t)(fat_now_secs() / 86400) + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = (int64_t)yoe + era * 400 + (month <= 2);
    return (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}

static uint16_t fat_time()
{
    const uint64_t s = fat_now_secs() % 86400;
    return (uint16_t)(((s / 3600) << 11) | (((s / 60) % 60) << 5) | ((s % 60) / 2));
}

static uint64_t fat_to_unix(const uint16_t date, const uint16_t time)
{
    const int64_t year = 1980 + (date >> 9);
    const unsigned month = (date >> 5) & 0xF;
    const unsigned day = date & 0x1F;
    if (month < 1 || month > 12 || day < 1)
        return 0;
    const int64_t y = year - (month <= 2);
    const int64_t era = y / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + (int64_t)doe - 719468;
    return (uint64_t)days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 0x3F) * 60 + (time & 0x1F) * 2;
}

static int dir_entry_io(fat32_sb_info *sbi, const uint32_t dir_first, const uint32_t pos,
                        fat32_directory_entry *de, const bool write)
{
    uint32_t c = dir_first;
    for (uint32_t i = pos / cluster_byte_size(sbi); i > 0; i--) {
        c = fat32_next_cluster(sbi, c);
        if (!valid_cluster(sbi, c))
            return -EIO;
    }
    const uint64_t off = cluster_to_byte(sbi, c) + pos % cluster_byte_size(sbi);

    if (write && fat_flush(sbi) < 0)
        return -EIO;
    if (write)
        return bdev_write(sbi, off, DIRENT_SIZE, de);
    return bdev_read(sbi, off, DIRENT_SIZE, de) == 0 ? 0 : -EIO;
}

static uint8_t short_checksum(const uint8_t raw[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + raw[i]);
    return sum;
}

static char fat32_lower(const char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static char fat32_upper(const char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

static void fat32_short_name(const fat32_directory_entry *de,
                             char *out, uint32_t out_size)
{
    int len = 0;

    for (int i = 0; i < 8 && len < (int)out_size - 1; i++) {
        if (de->filename[i] == ' ') break;
        out[len++] = de->reserved & FAT_NT_LOWER_BASE ? fat32_lower(de->filename[i]) : de->filename[i];
    }

    int ext_len = 0;
    for (int i = 0; i < 3; i++) {
        if (de->extension[i] != ' ') ext_len = i + 1;
    }
    if (ext_len > 0 && len < (int)out_size - 1) {
        out[len++] = '.';
        for (int i = 0; i < ext_len && len < (int)out_size - 1; i++)
            out[len++] = de->reserved & FAT_NT_LOWER_EXT ? fat32_lower(de->extension[i]) : de->extension[i];
    }

    out[len] = '\0';
}

struct dir_iter {
    fat32_sb_info *sbi;
    uint32_t cluster;
    uint32_t last_cluster;
    uint32_t pos;
    uint32_t end_pos;
    bool loaded;
    uint8_t *buf;
};

struct dir_found {
    fat32_directory_entry de;
    uint32_t pos;
    uint32_t lfn_pos;
    char name[FAT_NAME_MAX + 1];
};

static int iter_init(struct dir_iter *it, fat32_sb_info *sbi, const uint32_t dir_first, const uint32_t pos)
{
    memset(it, 0, sizeof(*it));
    it->sbi = sbi;
    it->buf = malloc(cluster_byte_size(sbi));
    if (!it->buf)
        return -ENOMEM;
    it->cluster = dir_first;
    it->last_cluster = dir_first;
    for (uint32_t i = pos / cluster_byte_size(sbi); i > 0 && valid_cluster(sbi, it->cluster); i--) {
        it->last_cluster = it->cluster;
        it->cluster = fat32_next_cluster(sbi, it->cluster);
    }
    it->pos = pos;
    return 0;
}

static void iter_done(struct dir_iter *it)
{
    free(it->buf);
    it->buf = nullptr;
}

static int iter_read(struct dir_iter *it, fat32_directory_entry *raw)
{
    const uint32_t cs = cluster_byte_size(it->sbi);
    if (!it->loaded) {
        if (!valid_cluster(it->sbi, it->cluster))
            return 0;
        if (bdev_read(it->sbi, cluster_to_byte(it->sbi, it->cluster), cs, it->buf) != 0)
            return -EIO;
        it->loaded = true;
        it->last_cluster = it->cluster;
    }
    memcpy(raw, it->buf + it->pos % cs, DIRENT_SIZE);
    it->pos += DIRENT_SIZE;
    if (it->pos % cs == 0) {
        it->cluster = fat32_next_cluster(it->sbi, it->cluster);
        it->loaded = false;
    }
    return 1;
}

static int iter_next(struct dir_iter *it, struct dir_found *f)
{
    bool lfn_valid = false;
    uint8_t lfn_sum = 0;
    for (;;) {
        const uint32_t entry_pos = it->pos;
        fat32_directory_entry raw;
        const int r = iter_read(it, &raw);
        if (r <= 0) {
            it->end_pos = entry_pos;
            return r;
        }
        if (raw.filename[0] == 0x00) {
            it->end_pos = entry_pos;
            return 0;
        }
        if ((uint8_t)raw.filename[0] == DIRENT_DELETED) {
            lfn_valid = false;
            continue;
        }
        if (raw.attributes == FAT_ATTR_LFN) {
            const fat32_long_filename_entry *l = (const fat32_long_filename_entry *)&raw;
            const int seq = l->sequence_number & 0x1F;
            if (l->sequence_number & LFN_LAST) {
                memset(f->name, 0, sizeof(f->name));
                f->lfn_pos = entry_pos;
                lfn_sum = l->checksum;
                lfn_valid = true;
            }
            if (!lfn_valid || seq < 1 || seq > LFN_MAX_ENTRIES || l->checksum != lfn_sum) {
                lfn_valid = false;
                continue;
            }
            const uint16_t *parts[3] = { l->name1, l->name2, l->name3 };
            const int counts[3] = { 5, 6, 2 };
            int at = (seq - 1) * LFN_CHARS;
            for (int p = 0; p < 3; p++) {
                for (int k = 0; k < counts[p]; k++, at++) {
                    uint16_t ch;
                    memcpy(&ch, &parts[p][k], sizeof(ch));
                    if (ch != 0 && ch != 0xFFFF && at < FAT_NAME_MAX)
                        f->name[at] = (char)ch;
                }
            }
            continue;
        }
        if (raw.attributes & FAT_ATTR_VOLUME_ID) {
            lfn_valid = false;
            continue;
        }

        f->de = raw;
        f->pos = entry_pos;
        if (!lfn_valid || short_checksum((const uint8_t *)raw.filename) != lfn_sum || f->name[0] == '\0') {
            fat32_short_name(&raw, f->name, sizeof(f->name));
            f->lfn_pos = entry_pos;
        }
        return 1;
    }
}

static bool name_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (fat32_lower(*a) != fat32_lower(*b))
            return false;
    }
    return *a == *b;
}

static int find_entry(fat32_sb_info *sbi, const uint32_t dir_first, const char *name, struct dir_found *f)
{
    struct dir_iter it;
    int err = iter_init(&it, sbi, dir_first, 0);
    if (err)
        return err;
    for (;;) {
        const int r = iter_next(&it, f);
        if (r <= 0) {
            err = r < 0 ? r : -ENOENT;
            break;
        }
        if (name_equal(f->name, name)) {
            err = 0;
            break;
        }
    }
    iter_done(&it);
    return err;
}

static bool short_name_taken(fat32_sb_info *sbi, const uint32_t dir_first, const uint8_t raw[11])
{
    struct dir_iter it;
    if (iter_init(&it, sbi, dir_first, 0) < 0)
        return true;
    struct dir_found *f = malloc(sizeof(struct dir_found));
    bool taken = f == nullptr;
    while (!taken && iter_next(&it, f) > 0)
        taken = memcmp(f->de.filename, raw, 11) == 0;
    free(f);
    iter_done(&it);
    return taken;
}

static bool short_char_ok(const char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (uint8_t)c >= 0x80)
        return true;
    return strchr("$%'-_@~`!(){}^#&", c) != nullptr;
}

static int part_case(const char *s, const size_t n)
{
    bool lower = false, upper = false;
    for (size_t i = 0; i < n; i++) {
        lower |= s[i] >= 'a' && s[i] <= 'z';
        upper |= s[i] >= 'A' && s[i] <= 'Z';
    }
    return lower && upper ? 0 : upper ? 2 : 1;
}

static int make_short_name(fat32_sb_info *sbi, const uint32_t dir_first, const char *name, uint8_t raw[11],
                           uint8_t *nt, bool *lfn)
{
    const size_t len = strlen(name);
    const char *dot = nullptr;
    for (const char *p = name; *p; p++) {
        if (*p == '.')
            dot = p;
    }
    if (dot == name)
        dot = nullptr;
    const size_t base_len = dot ? (size_t)(dot - name) : len;
    const size_t ext_len = dot ? len - base_len - 1 : 0;

    memset(raw, ' ', 11);
    *nt = 0;

    bool exact = base_len >= 1 && base_len <= 8 && ext_len <= 3 && (!dot || ext_len > 0);
    for (size_t i = 0; exact && i < len; i++) {
        if (name + i != dot && !short_char_ok(name[i]))
            exact = false;
    }
    const int bc = exact ? part_case(name, base_len) : 0;
    const int ec = exact && dot ? part_case(dot + 1, ext_len) : 1;
    if (exact && bc && ec) {
        for (size_t i = 0; i < base_len; i++)
            raw[i] = (uint8_t)fat32_upper(name[i]);
        for (size_t i = 0; i < ext_len; i++)
            raw[8 + i] = (uint8_t)fat32_upper(dot[1 + i]);
        if ((uint8_t)raw[0] == DIRENT_DELETED)
            raw[0] = 0x05;
        *nt = (bc == 1 ? FAT_NT_LOWER_BASE : 0) | (ec == 1 ? FAT_NT_LOWER_EXT : 0);
        *lfn = false;
        if (!short_name_taken(sbi, dir_first, raw))
            return 0;
    }

    *lfn = true;
    *nt = 0;
    char basis[8];
    size_t bn = 0;
    for (size_t i = 0; i < base_len && bn < 8; i++) {
        const char c = name[i];
        if (c == ' ' || c == '.')
            continue;
        basis[bn++] = short_char_ok(c) ? fat32_upper(c) : '_';
    }
    if (bn == 0)
        basis[bn++] = '_';
    uint8_t ext[3] = { ' ', ' ', ' ' };
    for (size_t i = 0, en = 0; dot && i < ext_len && en < 3; i++) {
        const char c = dot[1 + i];
        if (c != ' ')
            ext[en++] = (uint8_t)(short_char_ok(c) ? fat32_upper(c) : '_');
    }

    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[8];
        int tl = 0;
        char digits[7];
        int dn = 0;
        for (uint32_t v = n; v; v /= 10)
            digits[dn++] = (char)('0' + v % 10);
        tail[tl++] = '~';
        while (dn)
            tail[tl++] = digits[--dn];
        const size_t keep = bn < (size_t)(8 - tl) ? bn : (size_t)(8 - tl);
        memset(raw, ' ', 11);
        memcpy(raw, basis, keep);
        memcpy(raw + keep, tail, tl);
        memcpy(raw + 8, ext, 3);
        if (!short_name_taken(sbi, dir_first, raw))
            return 0;
    }
    return -EEXIST;
}

static int dir_alloc_slots(fat32_sb_info *sbi, const uint32_t dir_first, const uint32_t count, uint32_t *pos_out)
{
    struct dir_iter it;
    int err = iter_init(&it, sbi, dir_first, 0);
    if (err)
        return err;
    uint32_t run_start = 0, run = 0;
    for (;;) {
        const uint32_t pos = it.pos;
        fat32_directory_entry raw;
        const int r = iter_read(&it, &raw);
        if (r < 0) {
            err = r;
            break;
        }
        if (r == 0) {
            const uint32_t c = alloc_cluster(sbi, it.last_cluster, true);
            if (!c) {
                err = -ENOSPC;
                break;
            }
            if (run == 0)
                run_start = pos;
            run += cluster_byte_size(sbi) / DIRENT_SIZE;
            if (run >= count)
                break;
            it.cluster = fat32_next_cluster(sbi, c);
            it.last_cluster = c;
            it.pos = pos + cluster_byte_size(sbi);
            continue;
        }
        if (raw.filename[0] == 0x00 || (uint8_t)raw.filename[0] == DIRENT_DELETED) {
            if (run == 0)
                run_start = pos;
            if (++run == count)
                break;
        } else {
            run = 0;
        }
    }
    iter_done(&it);
    if (!err)
        *pos_out = run_start;
    return err;
}

static int write_name_entries(fat32_sb_info *sbi, const uint32_t dir_first, const uint32_t pos, const char *name,
                              const bool lfn, fat32_directory_entry *de, uint32_t *short_pos)
{
    const size_t len = strlen(name);
    const uint32_t lfn_count = lfn ? (uint32_t)((len + LFN_CHARS - 1) / LFN_CHARS) : 0;
    const uint8_t sum = short_checksum((const uint8_t *)de->filename);

    for (uint32_t e = 0; e < lfn_count; e++) {
        const uint32_t seq = lfn_count - e;
        fat32_long_filename_entry l;
        memset(&l, 0, sizeof(l));
        l.sequence_number = (uint8_t)(seq | (e == 0 ? LFN_LAST : 0));
        l.attributes = FAT_ATTR_LFN;
        l.checksum = sum;
        uint16_t *parts[3] = { l.name1, l.name2, l.name3 };
        const int counts[3] = { 5, 6, 2 };
        size_t at = (seq - 1) * LFN_CHARS;
        for (int p = 0; p < 3; p++) {
            for (int k = 0; k < counts[p]; k++, at++) {
                const uint16_t ch = at < len ? (uint8_t)name[at] : at == len ? 0x0000 : 0xFFFF;
                memcpy(&parts[p][k], &ch, sizeof(ch));
            }
        }
        const int err = dir_entry_io(sbi, dir_first, pos + e * DIRENT_SIZE, (fat32_directory_entry *)&l, true);
        if (err)
            return err;
    }
    *short_pos = pos + lfn_count * DIRENT_SIZE;
    return dir_entry_io(sbi, dir_first, *short_pos, de, true);
}

static int delete_slots(fat32_sb_info *sbi, const uint32_t dir_first, const uint32_t from, const uint32_t to)
{
    for (uint32_t p = from; p <= to; p += DIRENT_SIZE) {
        fat32_directory_entry de;
        int err = dir_entry_io(sbi, dir_first, p, &de, false);
        if (err)
            return err;
        de.filename[0] = (char)DIRENT_DELETED;
        if ((err = dir_entry_io(sbi, dir_first, p, &de, true)) != 0)
            return err;
    }
    return 0;
}

static bool valid_name(const char *name)
{
    const size_t len = strlen(name);
    if (len == 0 || len > FAT_NAME_MAX || strcmp(name, ".") == nullptr || strcmp(name, "..") == nullptr)
        return false;
    for (size_t i = 0; i < len; i++) {
        const char c = name[i];
        if ((uint8_t)c < 0x20 || strchr("\"*/:<>?\\|", c))
            return false;
    }
    return true;
}

static uint32_t entry_cluster(const fat32_directory_entry *de)
{
    return ((uint32_t)de->cluster_high << 16) | de->cluster_low;
}

static void set_entry_cluster(fat32_directory_entry *de, const uint32_t c)
{
    de->cluster_high = (uint16_t)(c >> 16);
    de->cluster_low = (uint16_t)c;
}

static fat32_inode_info *registry_find(const fat32_sb_info *sbi, const uint32_t dir_cluster, const uint32_t pos)
{
    for (fat32_inode_info *fi = sbi->open_nodes; fi; fi = fi->next) {
        if (!fi->orphan && fi->dir_cluster == dir_cluster && fi->dirent_pos == pos)
            return fi;
    }
    return nullptr;
}

static uint32_t dir_first_cluster(const fat32_inode_info *dir)
{
    return dir->first_cluster ? dir->first_cluster : dir->sbi->root_cluster;
}

static inode_t *fat32_make_inode(fat32_sb_info *sbi, const char *name, const fat32_directory_entry *de,
                                 const uint32_t dir_cluster, const uint32_t pos, const uint32_t lfn_pos)
{
    inode_t *inode = malloc(sizeof(inode_t));
    fat32_inode_info *fi = malloc(sizeof(fat32_inode_info));
    if (!inode || !fi) {
        free(inode);
        free(fi);
        return nullptr;
    }
    memset(inode, 0, sizeof(*inode));
    memset(fi, 0, sizeof(*fi));

    fi->sbi           = sbi;
    fi->inode         = inode;
    fi->refs          = 1;
    fi->first_cluster = entry_cluster(de);
    fi->size          = de->file_size;
    fi->attr          = de->attributes;
    fi->dir_cluster   = dir_cluster;
    fi->dirent_pos    = pos;
    fi->lfn_pos       = lfn_pos;

    inode->id           = fi->first_cluster ? fi->first_cluster
                                            : ((uint64_t)1 << 40) | ((uint64_t)dir_cluster << 20) | pos;
    inode->size         = fi->attr & FAT_ATTR_DIRECTORY ? 0 : fi->size;
    inode->mtime        = fat_to_unix(de->modification_date, de->modification_time);
    inode->private_data = fi;
    strncpy(inode->path, name, MAX_FILE_PATH - 1);

    if (fi->attr & FAT_ATTR_DIRECTORY) {
        inode->type = INODE_DIR;
        inode->iops = &fat32_dir_iops;
        inode->fops = &fat32_dir_fops;
    } else {
        inode->type = INODE_FILE;
        inode->iops = &fat32_file_iops;
        inode->fops = &fat32_file_fops;
    }

    fi->next = sbi->open_nodes;
    sbi->open_nodes = fi;
    return inode;
}

static void registry_remove(fat32_sb_info *sbi, const fat32_inode_info *fi)
{
    for (fat32_inode_info **p = &sbi->open_nodes; *p; p = &(*p)->next) {
        if (*p == fi) {
            *p = fi->next;
            return;
        }
    }
}

static int dirent_update(fat32_inode_info *fi)
{
    if (fi->is_root || fi->orphan)
        return 0;
    fat32_directory_entry de;
    int err = dir_entry_io(fi->sbi, fi->dir_cluster, fi->dirent_pos, &de, false);
    if (err)
        return err;
    set_entry_cluster(&de, fi->first_cluster);
    if (!(fi->attr & FAT_ATTR_DIRECTORY)) {
        de.file_size = fi->size;
        de.attributes |= FAT_ATTR_ARCHIVE;
    }
    de.modification_date = fat_date();
    de.modification_time = fat_time();
    de.access_date = de.modification_date;
    fi->inode->mtime = fat_to_unix(de.modification_date, de.modification_time);
    err = dir_entry_io(fi->sbi, fi->dir_cluster, fi->dirent_pos, &de, true);
    fsinfo_sync(fi->sbi);
    return err;
}

static inode_t *fat32_lookup(const inode_t *dir, const char *target)
{
    const fat32_inode_info *di = dir->private_data;
    fat32_sb_info *sbi = di->sbi;
    const uint32_t dir_first = dir_first_cluster(di);

    struct dir_found *f = malloc(sizeof(struct dir_found));
    if (!f)
        return nullptr;
    inode_t *inode = nullptr;
    if (find_entry(sbi, dir_first, target, f) == 0) {
        fat32_inode_info *live = registry_find(sbi, dir_first, f->pos);
        if (live) {
            live->refs++;
            inode = live->inode;
        } else {
            inode = fat32_make_inode(sbi, f->name, &f->de, dir_first, f->pos, f->lfn_pos);
        }
    }
    free(f);
    return inode;
}

static void fat32_release(inode_t *inode)
{
    fat32_inode_info *fi = inode->private_data;
    if (fi->is_root || --fi->refs > 0)
        return;
    registry_remove(fi->sbi, fi);
    pcache_destroy(&fi->pages);
    if (fi->orphan) {
        free_chain(fi->sbi, fi->first_cluster);
        fsinfo_sync(fi->sbi);
    }
    free(fi);
    free(inode);
}

static ssize_t read_at(fat32_inode_info *fi, const uint64_t pos, void *user_buf, const size_t len)
{
    fat32_sb_info *sbi = fi->sbi;
    const uint32_t cs = cluster_byte_size(sbi);
    uint32_t clus;
    if (chain_seek(fi, pos, false, &clus) < 0)
        return -EIO;

    size_t copied = 0;
    uint32_t in = (uint32_t)(pos % cs);
    while (copied < len && valid_cluster(sbi, clus)) {
        const uint32_t run_start = clus;
        uint32_t n = len - copied < cs - in ? (uint32_t)(len - copied) : cs - in;
        uint32_t next = 0;
        while (copied + n < len) {
            next = fat32_next_cluster(sbi, clus);
            if (next != clus + 1 || !valid_cluster(sbi, next))
                break;
            fi->cache_index++;
            fi->cache_cluster = next;
            clus = next;
            n += len - copied - n < cs ? (uint32_t)(len - copied - n) : cs;
        }
        if (bdev_read(sbi, cluster_to_byte(sbi, run_start) + in, n, (uint8_t *)user_buf + copied) != 0)
            break;
        copied += n;
        in = 0;
        if (copied < len) {
            fi->cache_index++;
            fi->cache_cluster = next;
            clus = next;
        }
    }
    if (!valid_cluster(sbi, fi->cache_cluster))
        fi->cache_cluster = 0;
    return copied ? (ssize_t)copied : -EIO;
}

static ssize_t fat32_file_read(file_t *file, void *buf, size_t len)
{
    fat32_inode_info *fi = file->node->private_data;
    const size_t pos = file->file_position;
    if (pos >= fi->size)
        return 0;
    if (pos + len > fi->size)
        len = fi->size - pos;

    size_t done = 0;
    ssize_t err = 0;
    while (done < len) {
        const uint64_t at = pos + done;
        uint8_t *page = pcache_get(&fi->pages, at / PAGE_SIZE);
        size_t n;
        if (page) {
            const size_t in = at % PAGE_SIZE;
            n = len - done < PAGE_SIZE - in ? len - done : PAGE_SIZE - in;
            memcpy((uint8_t *)buf + done, page + in, n);
            free_kernel_page(page);
        } else {
            uint64_t stop = (at / PAGE_SIZE + 1) * PAGE_SIZE;
            while (stop < pos + len && !pcache_present(&fi->pages, stop / PAGE_SIZE))
                stop += PAGE_SIZE;
            n = (stop < pos + len ? stop : pos + len) - at;
            const ssize_t r = read_at(fi, at, (uint8_t *)buf + done, n);
            if (r < 0) {
                err = r;
                break;
            }
            if ((size_t)r < n) {
                done += (size_t)r;
                break;
            }
        }
        done += n;
    }
    file->file_position += done;
    return done ? (ssize_t)done : err;
}

static ssize_t write_at(fat32_inode_info *fi, const uint64_t pos, const void *src, const size_t len)
{
    fat32_sb_info *sbi = fi->sbi;
    const uint32_t cs = cluster_byte_size(sbi);
    if (len == 0)
        return 0;
    if (pos + len > 0xFFFFFFFFULL)
        return -EFBIG;

    uint8_t *zero = nullptr;
    const uint32_t zero_len = cs > WRITE_ZERO_CHUNK ? cs : WRITE_ZERO_CHUNK;
    if (!src) {
        zero = malloc(zero_len);
        if (!zero)
            return -ENOMEM;
        memset(zero, 0, zero_len);
    }

    const uint32_t run_max = src ? WRITE_RUN_MAX : zero_len;
    size_t done = 0;
    int err = 0;
    while (done < len) {
        uint32_t clus;
        if ((err = chain_seek(fi, pos + done, true, &clus)) < 0)
            break;
        const uint32_t in = (uint32_t)((pos + done) % cs);
        const uint32_t run_start = clus;
        uint32_t n = len - done < cs - in ? (uint32_t)(len - done) : cs - in;
        while (done + n < len && n + cs <= run_max) {
            uint32_t next = fat32_next_cluster(sbi, clus);
            if (!valid_cluster(sbi, next))
                next = alloc_cluster(sbi, clus, false);
            if (next != clus + 1 || !valid_cluster(sbi, next))
                break;
            fi->cache_index++;
            fi->cache_cluster = next;
            clus = next;
            n += len - done - n < cs ? (uint32_t)(len - done - n) : cs;
        }
        if ((err = bdev_write(sbi, cluster_to_byte(sbi, run_start) + in, n,
                              src ? (const uint8_t *)src + done : zero)) < 0)
            break;
        pcache_update(&fi->pages, pos + done, src ? (const uint8_t *)src + done : nullptr, n);
        done += n;
    }
    free(zero);

    if (pos + done > fi->size) {
        fi->size = (uint32_t)(pos + done);
        fi->inode->size = fi->size;
    }
    const int uerr = dirent_update(fi);
    if (done)
        return (ssize_t)done;
    return err ? err : uerr;
}

static ssize_t fat32_file_write(file_t *file, const void *buf, const size_t len)
{
    fat32_inode_info *fi = file->node->private_data;
    const size_t pos = file->file_position;
    if (pos > fi->size) {
        const ssize_t z = write_at(fi, fi->size, nullptr, pos - fi->size);
        if (z < 0)
            return z;
    }
    const ssize_t n = write_at(fi, pos, buf, len);
    if (n > 0)
        file->file_position += (size_t)n;
    return n;
}

static int fat32_truncate(file_t *file, const size_t size)
{
    fat32_inode_info *fi = file->node->private_data;
    fat32_sb_info *sbi = fi->sbi;
    if (size > 0xFFFFFFFFULL)
        return -EFBIG;
    if (size > fi->size) {
        const ssize_t z = write_at(fi, fi->size, nullptr, size - fi->size);
        return z < 0 ? (int)z : 0;
    }

    const uint32_t cs = cluster_byte_size(sbi);
    const uint32_t keep = (uint32_t)((size + cs - 1) / cs);
    if (keep == 0) {
        free_chain(sbi, fi->first_cluster);
        fi->first_cluster = 0;
    } else if (fi->first_cluster) {
        uint32_t last;
        if (chain_seek(fi, (uint64_t)(keep - 1) * cs, false, &last) == 0) {
            const uint32_t rest = fat32_next_cluster(sbi, last);
            if (valid_cluster(sbi, rest)) {
                fat_set(sbi, last, FAT_EOC);
                free_chain(sbi, rest);
            }
        }
    }
    fi->cache_cluster = 0;
    fi->cache_index = 0;
    fi->size = (uint32_t)size;
    fi->inode->size = size;
    pcache_truncate(&fi->pages, size);
    return dirent_update(fi);
}

static int fat32_mmap_page(file_t *file, const uint64_t offset, uint64_t *frame)
{
    fat32_inode_info *fi = file->node->private_data;
    if (offset >= 0xFFFFFFFFULL)
        return -EFBIG;
    const size_t index = offset / PAGE_SIZE;
    uint8_t *page = pcache_get(&fi->pages, index);
    if (page == nullptr) {
        page = pcache_insert(&fi->pages, index);
        if (page == nullptr)
            return -ENOMEM;
        const uint64_t start = (uint64_t)index * PAGE_SIZE;
        if (start < fi->size) {
            const size_t want = fi->size - start < PAGE_SIZE ? fi->size - start : PAGE_SIZE;
            const ssize_t r = read_at(fi, start, page, want);
            if (r < 0 || (size_t)r < want) {
                pcache_remove(&fi->pages, index);
                free_kernel_page(page);
                return r < 0 ? (int)r : -EIO;
            }
        }
    }
    *frame = virt_to_phys(page);
    return 0;
}

static int fat32_fallocate(file_t *file, const int mode, const int64_t offset, const int64_t len)
{
    if (mode != 0)
        return -EOPNOTSUPP;
    const fat32_inode_info *fi = file->node->private_data;
    const uint64_t end = (uint64_t)offset + (uint64_t)len;
    if (end > 0xFFFFFFFFULL)
        return -EFBIG;
    return end > fi->size ? fat32_truncate(file, end) : 0;
}

static int fat32_readdir(file_t *file, struct vfs_dirent *out)
{
    const fat32_inode_info *fi = file->node->private_data;
    struct dir_iter it;
    int r = iter_init(&it, fi->sbi, dir_first_cluster(fi), (uint32_t)file->file_position);
    if (r)
        return r;
    struct dir_found *f = malloc(sizeof(struct dir_found));
    if (!f) {
        iter_done(&it);
        return -ENOMEM;
    }
    r = iter_next(&it, f);
    if (r > 0) {
        strncpy(out->name, f->name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
        out->ino  = entry_cluster(&f->de);
        out->type = (f->de.attributes & FAT_ATTR_DIRECTORY) ? INODE_DIR : INODE_FILE;
        file->file_position = it.pos;
    } else if (r == 0) {
        file->file_position = it.end_pos;
    }
    free(f);
    iter_done(&it);
    return r;
}

static bool dir_empty(fat32_sb_info *sbi, const uint32_t dir_first)
{
    struct dir_iter it;
    if (iter_init(&it, sbi, dir_first, 0) < 0)
        return false;
    struct dir_found *f = malloc(sizeof(struct dir_found));
    bool empty = f != nullptr;
    while (empty && iter_next(&it, f) > 0) {
        if (strcmp(f->name, ".") != nullptr && strcmp(f->name, "..") != nullptr)
            empty = false;
    }
    free(f);
    iter_done(&it);
    return empty;
}

static void dot_entry(fat32_directory_entry *de, const char *name, const uint32_t cluster)
{
    memset(de, 0, sizeof(*de));
    memset(de->filename, ' ', 8);
    memset(de->extension, ' ', 3);
    memcpy(de->filename, name, strlen(name));
    de->attributes = FAT_ATTR_DIRECTORY;
    set_entry_cluster(de, cluster);
    de->creation_date = de->modification_date = de->access_date = fat_date();
    de->creation_time = de->modification_time = fat_time();
}

static int add_entry(fat32_sb_info *sbi, const uint32_t dir_first, const char *name, fat32_directory_entry *de,
                     uint32_t *pos_out, uint32_t *lfn_pos_out)
{
    uint8_t raw[11];
    uint8_t nt;
    bool lfn;
    int err = make_short_name(sbi, dir_first, name, raw, &nt, &lfn);
    if (err)
        return err;
    memcpy(de->filename, raw, 8);
    memcpy(de->extension, raw + 8, 3);
    de->reserved = nt;

    const uint32_t slots = 1 + (lfn ? (uint32_t)((strlen(name) + LFN_CHARS - 1) / LFN_CHARS) : 0);
    uint32_t pos;
    if ((err = dir_alloc_slots(sbi, dir_first, slots, &pos)) != 0)
        return err;
    if (lfn_pos_out)
        *lfn_pos_out = pos;
    return write_name_entries(sbi, dir_first, pos, name, lfn, de, pos_out);
}

static int fat32_create(inode_t *dir, const char *name, const bool directory)
{
    fat32_inode_info *di = dir->private_data;
    fat32_sb_info *sbi = di->sbi;
    const uint32_t dir_first = dir_first_cluster(di);
    if (!valid_name(name))
        return -EINVAL;

    struct dir_found *f = malloc(sizeof(struct dir_found));
    if (!f)
        return -ENOMEM;
    int err = find_entry(sbi, dir_first, name, f);
    free(f);
    if (err == 0)
        return -EEXIST;
    if (err != -ENOENT)
        return err;

    fat32_directory_entry de;
    memset(&de, 0, sizeof(de));
    de.attributes = directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    de.creation_date = de.modification_date = de.access_date = fat_date();
    de.creation_time = de.modification_time = fat_time();

    uint32_t cluster = 0;
    if (directory) {
        cluster = alloc_cluster(sbi, 0, true);
        if (!cluster)
            return -ENOSPC;
        fat32_directory_entry dot;
        dot_entry(&dot, ".", cluster);
        err = dir_entry_io(sbi, cluster, 0, &dot, true);
        dot_entry(&dot, "..", di->is_root ? 0 : dir_first);
        if (!err)
            err = dir_entry_io(sbi, cluster, DIRENT_SIZE, &dot, true);
        if (err) {
            free_chain(sbi, cluster);
            fsinfo_sync(sbi);
            return err;
        }
        set_entry_cluster(&de, cluster);
    }

    uint32_t pos;
    err = add_entry(sbi, dir_first, name, &de, &pos, nullptr);
    if (err && cluster)
        free_chain(sbi, cluster);
    fsinfo_sync(sbi);
    return err;
}

static int remove_entry(fat32_sb_info *sbi, const uint32_t dir_first, const struct dir_found *f)
{
    const int err = delete_slots(sbi, dir_first, f->lfn_pos, f->pos);
    if (err)
        return err;
    fat32_inode_info *live = registry_find(sbi, dir_first, f->pos);
    if (live)
        live->orphan = true;
    else
        free_chain(sbi, entry_cluster(&f->de));
    fsinfo_sync(sbi);
    return 0;
}

static int fat32_unlink(inode_t *dir, const char *name, const bool directory)
{
    const fat32_inode_info *di = dir->private_data;
    fat32_sb_info *sbi = di->sbi;
    const uint32_t dir_first = dir_first_cluster(di);

    struct dir_found *f = malloc(sizeof(struct dir_found));
    if (!f)
        return -ENOMEM;
    int err = find_entry(sbi, dir_first, name, f);
    if (err == 0) {
        const bool is_dir = f->de.attributes & FAT_ATTR_DIRECTORY;
        if (directory && !is_dir)
            err = -ENOTDIR;
        else if (!directory && is_dir)
            err = -EISDIR;
        else if (is_dir && !dir_empty(sbi, entry_cluster(&f->de)))
            err = -ENOTEMPTY;
        else
            err = remove_entry(sbi, dir_first, f);
    }
    free(f);
    return err;
}

static int fat32_rename(inode_t *old_dir, const char *old_name, inode_t *new_dir, const char *new_name,
                        const bool no_replace)
{
    fat32_inode_info *odi = old_dir->private_data;
    fat32_inode_info *ndi = new_dir->private_data;
    fat32_sb_info *sbi = odi->sbi;
    const uint32_t old_first = dir_first_cluster(odi);
    const uint32_t new_first = dir_first_cluster(ndi);
    if (!valid_name(new_name))
        return -EINVAL;

    struct dir_found *src = malloc(sizeof(struct dir_found));
    struct dir_found *dst = malloc(sizeof(struct dir_found));
    int err = src && dst ? find_entry(sbi, old_first, old_name, src) : -ENOMEM;
    if (err)
        goto out;
    const bool src_dir = src->de.attributes & FAT_ATTR_DIRECTORY;

    err = find_entry(sbi, new_first, new_name, dst);
    if (err == 0) {
        const bool same = old_first == new_first && dst->pos == src->pos;
        if (same && strcmp(src->name, new_name) == nullptr)
            goto out;
        if (!same) {
            const bool dst_dir = dst->de.attributes & FAT_ATTR_DIRECTORY;
            if (no_replace)
                err = -EEXIST;
            else if (src_dir && !dst_dir)
                err = -ENOTDIR;
            else if (!src_dir && dst_dir)
                err = -EISDIR;
            else if (dst_dir && !dir_empty(sbi, entry_cluster(&dst->de)))
                err = -ENOTEMPTY;
            else
                err = remove_entry(sbi, new_first, dst);
            if (err)
                goto out;
        }
    } else if (err != -ENOENT) {
        goto out;
    }

    fat32_directory_entry de = src->de;
    uint32_t pos, lfn_pos;
    if ((err = add_entry(sbi, new_first, new_name, &de, &pos, &lfn_pos)) != 0)
        goto out;
    if ((err = delete_slots(sbi, old_first, src->lfn_pos, src->pos)) != 0)
        goto out;

    if (src_dir && old_first != new_first) {
        fat32_directory_entry dotdot;
        const uint32_t moved = entry_cluster(&src->de);
        if (dir_entry_io(sbi, moved, DIRENT_SIZE, &dotdot, false) == 0) {
            set_entry_cluster(&dotdot, ndi->is_root ? 0 : new_first);
            dir_entry_io(sbi, moved, DIRENT_SIZE, &dotdot, true);
        }
    }
    fat32_inode_info *live = registry_find(sbi, old_first, src->pos);
    if (live) {
        live->dir_cluster = new_first;
        live->dirent_pos = pos;
        live->lfn_pos = lfn_pos;
        strncpy(live->inode->path, new_name, MAX_FILE_PATH - 1);
    }
    err = 0;
out:
    fsinfo_sync(sbi);
    free(src);
    free(dst);
    return err;
}

static int fat32_file_open(inode_t *inode, file_t *file)
{
    return 0;
}

static int fat32_file_close(file_t *file)
{
    return 0;
}

static file_operations_t fat32_dir_fops = {
    .open    = fat32_file_open,
    .readdir = fat32_readdir,
    .close = fat32_file_close,
};

static file_operations_t fat32_file_fops = {
    .read     = fat32_file_read,
    .write    = fat32_file_write,
    .truncate = fat32_truncate,
    .fallocate = fat32_fallocate,
    .mmap_page = fat32_mmap_page,
    .open     = fat32_file_open,
    .close    = fat32_file_close,
};

static inode_operations_t fat32_dir_iops = {
    .lookup  = fat32_lookup,
    .release = fat32_release,
    .create  = fat32_create,
    .unlink  = fat32_unlink,
    .rename  = fat32_rename,
};

static inode_operations_t fat32_file_iops = {
    .release = fat32_release,
};

static inode_t *fat32_mount(void *blob)
{
    const auto bdev = (struct block_device *)blob;
    if (!bdev) {
        terminal_printf("fat32: no block device\n");
        return nullptr;
    }

    fat32_boot_sector bs;
    if (bdev->ops->read_bytes(bdev, 0, sizeof(bs), &bs) != 0) {
        terminal_printf("fat32: failed to read boot sector\n");
        return nullptr;
    }

    if (bs.bytes_per_sector == 0 ||
        (bs.bytes_per_sector & (bs.bytes_per_sector - 1)) != 0 || bs.sectors_per_cluster == 0) {
        terminal_printf("fat32: invalid bytes_per_sector\n");
        return nullptr;
    }

    fat32_fsinfo fsi = {0};
    bdev->ops->read_bytes(bdev, (uint64_t)bs.fsinfo_sector * bs.bytes_per_sector, sizeof(fsi), &fsi);

    fat32_sb_info *sbi = malloc(sizeof(fat32_sb_info));
    if (!sbi) return nullptr;
    memset(sbi, 0, sizeof(*sbi));

    sbi->bdev                = bdev;
    sbi->bs                  = bs;
    sbi->fsinfo              = fsi;
    sbi->bytes_per_sector    = bs.bytes_per_sector;
    sbi->sectors_per_cluster = bs.sectors_per_cluster;
    sbi->sectors_per_fat     = bs.sectors_per_fat32;
    sbi->root_cluster        = bs.root_cluster;
    sbi->fat_begin_lba       = bs.reserved_sectors;
    sbi->data_begin_lba      = bs.reserved_sectors +
                               bs.number_of_fats * bs.sectors_per_fat32;

    const uint32_t total_sectors = bs.total_sectors ? bs.total_sectors : bs.total_sectors_large;
    sbi->total_clusters = (total_sectors - sbi->data_begin_lba) / bs.sectors_per_cluster;
    const uint32_t fat_entries = sbi->sectors_per_fat * sbi->bytes_per_sector / 4;
    if (sbi->total_clusters + 2 > fat_entries)
        sbi->total_clusters = fat_entries - 2;
    if (fsi.lead_signature == FSINFO_LEAD_SIG && fsi.free_cluster_count > sbi->total_clusters)
        sbi->fsinfo.free_cluster_count = FSINFO_UNKNOWN;

    inode_t *root = malloc(sizeof(inode_t));
    fat32_inode_info *fi = malloc(sizeof(fat32_inode_info));
    if (!root || !fi) {
        free(root);
        free(fi);
        free(sbi);
        return nullptr;
    }
    memset(root, 0, sizeof(*root));
    memset(fi, 0, sizeof(*fi));
    fi->sbi = sbi;
    fi->inode = root;
    fi->refs = 1;
    fi->is_root = true;
    fi->first_cluster = sbi->root_cluster;
    fi->attr = FAT_ATTR_DIRECTORY;
    root->id = sbi->root_cluster;
    root->type = INODE_DIR;
    root->iops = &fat32_dir_iops;
    root->fops = &fat32_dir_fops;
    root->private_data = fi;
    strncpy(root->path, "/", MAX_FILE_PATH - 1);
    return root;
}

static int fat32_sync(inode_t *root)
{
    const fat32_inode_info *fi = root->private_data;
    fsinfo_sync(fi->sbi);
    const struct block_device *bdev = fi->sbi->bdev;
    if (!bdev->ops->flush)
        return 0;
    return bdev->ops->flush((struct block_device *)bdev) == 0 ? 0 : -EIO;
}

static int fat32_statfs(const inode_t *root, struct vfs_statfs *out)
{
    fat32_sb_info *sbi = ((fat32_inode_info *)root->private_data)->sbi;
    if (sbi->fsinfo.free_cluster_count == FSINFO_UNKNOWN) {
        uint32_t free = 0;
        for (uint32_t c = 2; c < sbi->total_clusters + 2; c++) {
            uint32_t v;
            if (fat_get(sbi, c, &v) < 0)
                return -EIO;
            if (v == FAT_FREE)
                free++;
        }
        sbi->fsinfo.free_cluster_count = free;
        sbi->fsinfo_dirty = true;
    }

    out->magic        = 0x4d44;
    out->block_size   = cluster_byte_size(sbi);
    out->total_blocks = sbi->total_clusters;
    out->free_blocks  = sbi->fsinfo.free_cluster_count;
    out->total_files  = 0;
    out->free_files   = 0;
    out->name_max     = FAT_NAME_MAX;
    return 0;
}

static void fat32_unmount(inode_t *root)
{
    if (!root) return;
    fat32_inode_info *fi = root->private_data;
    fat32_sb_info *sbi = fi->sbi;

    if (sbi) {
        fsinfo_sync(sbi);
        if (sbi->bdev->ops->release)
            sbi->bdev->ops->release(sbi->bdev);
        else
            free(sbi->bdev);
        free(sbi);
    }
    free(fi);
    free(root);
}

file_system_driver_t fat32_driver = {
    .mount   = fat32_mount,
    .unmount = fat32_unmount,
    .sync    = fat32_sync,
    .statfs  = fat32_statfs,
};

struct block_device *fat32_create_bdev(const uint32_t unique_id,
                                       const uint64_t part_offset_bytes,
                                       const uint64_t part_length_bytes,
                                       const struct block_device_ops *ops,
                                       void *private)
{
    struct block_device *bdev = malloc(sizeof(struct block_device));
    if (!bdev)
        return nullptr;
    bdev->unique_id   = unique_id;
    bdev->part_offset = part_offset_bytes;
    bdev->part_length = part_length_bytes;
    bdev->ops         = ops;
    bdev->private     = private;
    return bdev;
}

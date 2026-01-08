/* readahead.c - read in advance a list of files, adding them to the page cache
 *
 * Copyright (C) 2005,2008  Behdad Esfahbod
 *
 * This file is part of preload.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */

#include "readahead.h"

#include <sys/ioctl.h>
#include <sys/wait.h>

#include "common.h"
#include "conf.h"
#include "log.h"
#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif

typedef struct {
    /* Cached stat result so we do not repeat syscalls for the same path. */
    struct stat st;
    gboolean valid;
} CachedStat;

static GHashTable* stat_cache = NULL;

/*
 * Return a cached stat result for a path. This avoids repeating expensive
 * syscalls when multiple segments of the same file are processed. The "ok"
 * flag is set only when a valid cached result exists.
 */
static const struct stat* get_cached_stat(const char* path, gboolean* ok) {
    CachedStat* cached = NULL;

    if (!stat_cache)
        stat_cache =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    cached = g_hash_table_lookup(stat_cache, path);
    if (!cached) {
        cached = g_new0(CachedStat, 1);
        if (0 == stat(path, &cached->st)) {
            cached->valid = TRUE;
            g_hash_table_insert(stat_cache, g_strdup(path), cached);
        } else {
            g_free(cached);
            cached = NULL;
        }
    }

    if (ok)
        *ok = cached && cached->valid;

    return cached ? &cached->st : NULL;
}

static void clear_stat_cache(void) {
    if (!stat_cache)
        return;

    g_hash_table_destroy(stat_cache);
    stat_cache = NULL;
}

/* Helper to fetch a cached block size if available. */
static size_t get_block_size(const char* path) {
    gboolean ok = FALSE;
    const struct stat* st = get_cached_stat(path, &ok);

    /* Return zero when the stat call fails so callers can fall back to the
     * system page size without adding extra branching here. */
    return ok ? (size_t)st->st_blksize : 0;
}

/*
 * Populate file->block for block/inode sorting. We seed the value from a
 * cached inode (when available) so we can fall back gracefully if FIBMAP or
 * other lookups fail.
 */
static void set_block(preload_map_t* file, gboolean G_GNUC_UNUSED use_inode) {
    int fd = -1;
    int block = 0;
    struct stat buf;
    const struct stat* cached = NULL;
    gboolean cached_ok = FALSE;
    int inode_fallback = 0;

    cached = get_cached_stat(file->path, &cached_ok);

    /* Default to inode when known, otherwise leave unset. */
    file->block = cached_ok ? (int)cached->st_ino : -1;

    if (cached_ok && file->length < (size_t)cached->st_blksize) {
        /* Avoid extra syscalls for tiny segments; fall back to inode sort. */
        file->block = cached->st_ino;
        return;
    }

    fd = open(file->path, O_RDONLY);
    if (fd < 0) {
        return;
    }

    if (0 > fstat(fd, &buf)) {
        close(fd);
        if (cached_ok)
            file->block = (int)cached->st_ino;
        return;
    }

    inode_fallback = cached_ok ? (int)cached->st_ino : (int)buf.st_ino;

    if (use_inode) {
        file->block = inode_fallback;
        close(fd);
        return;
    }

#ifdef FIBMAP
    block = file->offset / buf.st_blksize;
    if (0 > ioctl(fd, FIBMAP, &block))
        block = 0;
#endif

    /* fall back to inode number when FIBMAP cannot supply a block */
    if (block == 0)
        block = inode_fallback;

    file->block = block;

    close(fd);
}

/* Compare files by path */
static int map_path_compare(const preload_map_t** pa,
                            const preload_map_t** pb) {
    const preload_map_t *a = *pa, *b = *pb;
    int i;

    i = strcmp(a->path, b->path);
    if (!i) /* same file */
        i = a->offset - b->offset;
    if (!i) /* same offset?! */
        i = b->length - a->length;

    return i;
}

/* Compare files by block */
static int map_block_compare(const preload_map_t** pa,
                             const preload_map_t** pb) {
    const preload_map_t *a = *pa, *b = *pb;
    int i;

    i = a->block - b->block;
    if (!i) /* no block? */
        i = strcmp(a->path, b->path);
    if (!i) /* same file */
        i = a->offset - b->offset;
    if (!i) /* same offset?! */
        i = b->length - a->length;

    return i;
}

static int procs = 0;

typedef struct {
    const char* path;
    size_t offset;
    size_t length;
} PreloadRange;

static void wait_for_slot(int maxprocs) {
    int status;

    if (maxprocs <= 0)
        return;

    while (procs >= maxprocs) {
        if (waitpid(-1, &status, 0) > 0)
            procs--;
    }
}

static void wait_for_children(void) {
    int status;

    /* wait for child processes to terminate */
    while (procs > 0) {
        if (waitpid(-1, &status, 0) > 0)
            procs--;
    }
}

static void readahead_range(const char* path, size_t offset, size_t length) {
    int fd = -1;

    fd = open(path, O_RDONLY | O_NOCTTY
#ifdef O_NOATIME
                        | O_NOATIME
#endif
    );
    if (fd >= 0) {
        readahead(fd, offset, length);

        close(fd);
    }
}

static void process_range(const char* path, size_t offset, size_t length) {
    int maxprocs = conf->system.maxprocs;

    if (maxprocs > 0 && procs >= maxprocs)
        wait_for_slot(maxprocs);

    if (maxprocs > 0) {
        /* parallel reading */
        int status = fork();

        if (status == -1) {
            /* ignore error, return */
            return;
        }

        /* return immediately in the parent */
        if (status > 0) {
            procs++;
            return;
        }
    }

    readahead_range(path, offset, length);

    if (maxprocs > 0) {
        /* we're in a child process, exit */
        exit(0);
    }
}

static void append_range(GArray* ranges,
                         const char* path,
                         size_t offset,
                         size_t length) {
    PreloadRange range;

    range.path = path;
    range.offset = offset;
    range.length = length;
    g_array_append_val(ranges, range);
}

static void build_ranges(preload_map_t** files,
                         int start,
                         int end,
                         GArray* ranges) {
    int i;
    const char* path = NULL;
    size_t offset = 0;
    size_t length = 0;
    size_t merge_gap = 0;

    for (i = start; i < end; i++) {
        if (path && 0 == strcmp(path, files[i]->path)) {
            size_t curr_end = offset + length;
            size_t gap =
                files[i]->offset > curr_end ? files[i]->offset - curr_end : 0;

            if (gap <= merge_gap) {
                size_t new_end = files[i]->offset + files[i]->length;
                if (new_end > curr_end)
                    length = new_end - offset;
                continue;
            }
        }

        if (path) {
            append_range(ranges, path, offset, length);
            path = NULL;
        }

        path = files[i]->path;
        offset = files[i]->offset;
        length = files[i]->length;
        merge_gap = get_block_size(path);
        if (merge_gap == 0)
            merge_gap = (size_t)sysconf(_SC_PAGESIZE);
    }

    if (path)
        append_range(ranges, path, offset, length);
}

static void process_ranges(GArray* ranges) {
    guint i;
    int maxprocs = conf->system.maxprocs;

    if (ranges->len == 0)
        return;

    if (maxprocs > 0 && procs >= maxprocs)
        wait_for_slot(maxprocs);

    if (maxprocs > 0) {
        /* parallel reading */
        int status = fork();

        if (status == -1) {
            /* ignore error, return */
            return;
        }

        /* return immediately in the parent */
        if (status > 0) {
            procs++;
            return;
        }
    }

    for (i = 0; i < ranges->len; i++) {
        PreloadRange* range = &g_array_index(ranges, PreloadRange, i);
        readahead_range(range->path, range->offset, range->length);
    }

    if (maxprocs > 0) {
        /* we're in a child process, exit */
        exit(0);
    }
}

/*
 * Sort using block/inode keys. We first sort by path to reuse cached stat
 * results when filling missing block values, then sort by block for the final
 * order.
 */
static void sort_by_block_or_inode(preload_map_t** files, int file_count) {
    int i;
    gboolean need_block = FALSE;

    /* first see if any file doesn't have block/inode info */
    for (i = 0; i < file_count; i++)
        if (files[i]->block == -1) {
            need_block = TRUE;
            break;
        }

    if (need_block) {
        /* Sorting by path, to make stat fast. */
        qsort(files, file_count, sizeof(*files),
              (GCompareFunc)map_path_compare);

        for (i = 0; i < file_count; i++)
            if (files[i]->block == -1)
                set_block(files[i], conf->system.sortstrategy == SORT_INODE);
    }

    /* Sorting by block. */
    qsort(files, file_count, sizeof(*files), (GCompareFunc)map_block_compare);
}

static void sort_files(preload_map_t** files, int file_count) {
    switch (conf->system.sortstrategy) {
        case SORT_NONE:
            break;

        case SORT_PATH:
            qsort(files, file_count, sizeof(*files),
                  (GCompareFunc)map_path_compare);
            break;

        case SORT_INODE:
        case SORT_BLOCK:
            sort_by_block_or_inode(files, file_count);
            break;

        default:
            g_warning("Invalid value for config key system.sortstrategy: %d",
                      conf->system.sortstrategy);
            /* avoid warning every time */
            conf->system.sortstrategy = SORT_BLOCK;
            break;
    }
}

int preload_readahead(preload_map_t** files, int file_count) {
    int i;
    int processed = 0;
    int group_start = 0;
    GArray* ranges = NULL;

    sort_files(files, file_count);
    ranges = g_array_new(FALSE, FALSE, sizeof(PreloadRange));
    if (conf->system.sortstrategy == SORT_BLOCK) {
        /* Batch ranges per block to reduce fork/syscall overhead on fragmented
         * layouts while keeping block order. */
        for (i = 0; i < file_count; i++) {
            if (i > group_start &&
                files[i]->block != files[group_start]->block) {
                build_ranges(files, group_start, i, ranges);
                processed += ranges->len;
                process_ranges(ranges);
                g_array_set_size(ranges, 0);
                group_start = i;
            }
        }

        if (file_count > 0) {
            build_ranges(files, group_start, file_count, ranges);
            processed += ranges->len;
            process_ranges(ranges);
        }
    } else {
        build_ranges(files, 0, file_count, ranges);
        processed += ranges->len;
        for (i = 0; i < (int)ranges->len; i++) {
            PreloadRange* range = &g_array_index(ranges, PreloadRange, i);
            process_range(range->path, range->offset, range->length);
        }
    }

    g_array_free(ranges, TRUE);

    wait_for_children();
    clear_stat_cache();

    return processed;
}

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

/* Helper to fetch a cached block size if available. */
static size_t get_block_size(const char* path) {
    gboolean ok = FALSE;
    const struct stat* st = get_cached_stat(path, &ok);

    /* Return zero when the stat call fails so callers can fall back to the
     * system page size without adding extra branching here. */
    return ok ? (size_t)st->st_blksize : 0;
}

/* Determine the merge gap for a path, preferring the filesystem block size. */
static size_t get_merge_gap(const char* path) {
    size_t merge_gap = get_block_size(path);

    if (merge_gap == 0)
        merge_gap = (size_t)sysconf(_SC_PAGESIZE);

    return merge_gap;
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

static void process_file(const char* path, size_t offset, size_t length) {
    int fd = -1;
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

    fd = open(path, O_RDONLY | O_NOCTTY
#ifdef O_NOATIME
                        | O_NOATIME
#endif
    );
    if (fd >= 0) {
        readahead(fd, offset, length);

        close(fd);
    }

    if (maxprocs > 0) {
        /* we're in a child process, exit */
        exit(0);
    }
}

typedef struct {
    size_t offset;
    size_t length;
    size_t merge_gap;
} PendingRange;

static PendingRange* pending_range_new(size_t offset,
                                       size_t length,
                                       size_t merge_gap) {
    PendingRange* range = g_new0(PendingRange, 1);

    range->offset = offset;
    range->length = length;
    range->merge_gap = merge_gap;

    return range;
}

/*
 * Extend the last range when it overlaps or is close enough to merge, otherwise
 * append a new range for the same path.
 */
static void pending_ranges_add(GPtrArray* ranges,
                               size_t offset,
                               size_t length,
                               size_t merge_gap) {
    if (ranges->len > 0) {
        PendingRange* last = g_ptr_array_index(ranges, ranges->len - 1);
        size_t last_end = last->offset + last->length;
        size_t gap = offset > last_end ? offset - last_end : 0;

        if (gap <= merge_gap) {
            size_t new_end = offset + length;
            if (new_end > last_end)
                last->length = new_end - last->offset;
            return;
        }
    }

    g_ptr_array_add(ranges, pending_range_new(offset, length, merge_gap));
}

static int flush_pending_ranges(GHashTable* pending) {
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    int processed = 0;

    g_hash_table_iter_init(&iter, pending);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char* path = key;
        GPtrArray* ranges = value;
        guint idx = 0;

        for (idx = 0; idx < ranges->len; idx++) {
            PendingRange* range = g_ptr_array_index(ranges, idx);

            process_file(path, range->offset, range->length);
            processed++;
        }
    }

    g_hash_table_remove_all(pending);

    return processed;
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

/*
 * Group entries with the same block so we can coalesce multiple ranges per file
 * even when sorted by block. This reduces open/readahead syscalls in fragmented
 * environments where the same file appears interleaved with others.
 */
static int preload_readahead_by_block(preload_map_t** files, int file_count) {
    GHashTable* pending = NULL;
    int processed = 0;
    int current_block = 0;
    int i = 0;

    if (file_count <= 0)
        return 0;

    pending = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                    (GDestroyNotify)g_ptr_array_unref);

    while (i < file_count) {
        current_block = files[i]->block;
        for (; i < file_count && files[i]->block == current_block; i++) {
            GPtrArray* ranges = g_hash_table_lookup(pending, files[i]->path);
            size_t merge_gap = get_merge_gap(files[i]->path);

            if (!ranges) {
                ranges = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(pending, files[i]->path, ranges);
            }

            pending_ranges_add(ranges, files[i]->offset, files[i]->length,
                               merge_gap);
        }

        processed += flush_pending_ranges(pending);
    }
    g_hash_table_destroy(pending);

    wait_for_children();

    return processed;
}

int preload_readahead(preload_map_t** files, int file_count) {
    int i;
    const char* path = NULL;
    size_t offset = 0, length = 0;
    size_t merge_gap = 0;
    int processed = 0;

    sort_files(files, file_count);
    if (conf->system.sortstrategy == SORT_BLOCK ||
        conf->system.sortstrategy == SORT_INODE)
        return preload_readahead_by_block(files, file_count);

    for (i = 0; i < file_count; i++) {
        if (path && 0 == strcmp(path, files[i]->path)) {
            size_t end = offset + length;
            size_t gap = files[i]->offset > end ? files[i]->offset - end : 0;

            if (gap <= merge_gap) {
                /* merge requests across small gaps or overlaps */
                size_t new_end = files[i]->offset + files[i]->length;
                if (new_end > end)
                    length = new_end - offset;
                continue;
            }
        }

        if (path) {
            process_file(path, offset, length);
            processed++;
            path = NULL;
        }

        path = files[i]->path;
        offset = files[i]->offset;
        length = files[i]->length;
        /* Use the filesystem block size (or page size) to decide how aggressively
         * we coalesce adjacent ranges for a path. Aligning the gap threshold to
         * the block size avoids issuing redundant readahead calls on segments
         * that would map to the same set of pages anyway. */
        merge_gap = get_merge_gap(path);
    }

    if (path) {
        process_file(path, offset, length);
        processed++;
        path = NULL;
    }

    wait_for_children();

    return processed;
}

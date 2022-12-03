#define FUSE_USE_VERSION 26

#include <errno.h>
#include <fuse.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "sfs.h"
#include "diskio.h"

#define BASE_PERMS 0755

static const char default_img[] = "test.img";

/* Options passed from commandline arguments */
struct options {
    const char *img;
    int background;
    int verbose;
    int show_help;
    int show_fuse_help;
} options;


#define log(fmt, ...) \
    do { \
        if (options.verbose) \
            printf(" # " fmt, ##__VA_ARGS__); \
    } while (0)


/* libfuse2 leaks, so let's shush LeakSanitizer if we are using Asan. */
const char* __asan_default_options() { return "detect_leaks=0"; }


struct sfs_entry find_entry(struct sfs_entry* entries, unsigned entries_n, char* entry_fname, int* pos)
{
    for(unsigned i = 0; i < entries_n; i++)
    {
        if(strcmp(entries[i].filename, entry_fname) == 0)
        {
            *pos = i;
           
            return entries[i];
        }
    }
    *pos = -1;

    struct sfs_entry ret;
    ret.first_block = SFS_BLOCKIDX_END;
    return ret;
}

static int get_entry(const char *path, struct sfs_entry *ret_entry,
                     unsigned *ret_dir_off, unsigned* ret_entry_off)
{
    int res = 0;
    int pos = -1;

    char* entry_fname = strtok(strdup(path), "/");
    unsigned dir_off = SFS_ROOTDIR_OFF;
    unsigned entry_off = dir_off;

    if(entry_fname != NULL)
    {

        struct sfs_entry* entries = malloc(SFS_ROOTDIR_SIZE);
        disk_read(entries, SFS_ROOTDIR_SIZE, dir_off);
        *ret_entry = find_entry(entries, SFS_ROOTDIR_NENTRIES, entry_fname, &pos);

        if(pos == -1) res = -ENOENT;
        else
        {
            entry_off = dir_off + pos * sizeof(struct sfs_entry);
            dir_off = SFS_DATA_OFF + ret_entry->first_block * SFS_BLOCK_SIZE;
        
            while((entry_fname = strtok(NULL, "/")))
            {
                disk_read(entries, SFS_DIR_SIZE, dir_off);
                *ret_entry = find_entry(entries, SFS_DIR_NENTRIES, entry_fname, &pos);

                if(pos == -1) 
                {
                    res = -ENOENT;
                    break;
                }
                entry_off = dir_off + pos * sizeof(struct sfs_entry);
                dir_off = SFS_DATA_OFF + ret_entry->first_block * SFS_BLOCK_SIZE;
            }
        }
    }
    else
    {
        ret_entry->first_block = 0;
        ret_entry->size = SFS_DIRECTORY;
        strcpy(ret_entry->filename, "/");
    }

    if(ret_dir_off != NULL) *ret_dir_off = dir_off;
    if(ret_entry_off != NULL) *ret_entry_off = entry_off;
    
    return res;
}


/*
 * Retrieve information about a file or directory.
 * You should populate fields of `stbuf` with appropriate information if the
 * file exists and is accessible, or return an error otherwise.
 *
 * For directories, you should at least set st_mode (with S_IFDIR) and st_nlink.
 * For files, you should at least set st_mode (with S_IFREG), st_nlink and
 * st_size.
 *
 * Return 0 on success, < 0 on error.
 */
static int sfs_getattr(const char *path,
                       struct stat *st)
{
    int res = 0;

    log("getattr %s\n", path);

    memset(st, 0, sizeof(struct stat));
    /* Set owner to user/group who mounted the image */
    st->st_uid = getuid();
    st->st_gid = getgid();
    /* Last accessed/modified just now */
    st->st_atime = time(NULL);
    st->st_mtime = time(NULL);

    struct sfs_entry entry;

    res = get_entry(path, &entry, NULL, NULL);

    if (res == 0) 
    {
        // check if its a directory or a file;
        int is_dir = entry.size & SFS_DIRECTORY;
        if(is_dir)
        {
            st->st_mode = S_IFDIR | BASE_PERMS;
            st->st_nlink = 2;
        }
        else
        {
            st->st_mode = S_IFREG | BASE_PERMS;
            st->st_size = entry.size & SFS_SIZEMASK;
            st->st_nlink = 1;
        }
    } 

    return res;
}


/*
 * Return directory contents for `path`. This function should simply fill the
 * filenames - any additional information (e.g., whether something is a file or
 * directory) is later retrieved through getattr calls.
 * Use the function `filler` to add an entry to the directory. Use it like:
 *  filler(buf, <dirname>, NULL, 0);
 * Return 0 on success, < 0 on error.
 */
static int sfs_readdir(const char *path,
                       void *buf,
                       fuse_fill_dir_t filler,
                       off_t offset,
                       struct fuse_file_info *fi)
{
    log("readdir %s\n", path);

    (void) offset; (void) fi;

    struct sfs_entry dir;
    int res = get_entry(path, &dir, NULL, NULL);

    struct sfs_entry* dir_entries;
    unsigned dir_length;
    if(res == 0)
    {
        if(strcmp(dir.filename, "/") == 0)
        {
            dir_entries = malloc(SFS_ROOTDIR_SIZE);
            dir_length = SFS_ROOTDIR_NENTRIES;
            disk_read(dir_entries, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);
        }
        else
        {
            dir_entries = malloc(SFS_DIR_SIZE);
            dir_length = SFS_DIR_NENTRIES;
            disk_read(dir_entries, SFS_DIR_SIZE, SFS_DATA_OFF + dir.first_block * SFS_BLOCK_SIZE);
        }

        for (unsigned i = 0; i < dir_length; i++)
        {
            if(strlen(dir_entries[i].filename) > 0)
            {
                filler(buf, dir_entries[i].filename, NULL, 0);
            }
        }
    
    }

    return 0;

}


/*
 * Read contents of `path` into `buf` for  up to `size` bytes.
 * Note that `size` may be bigger than the file actually is.
 * Reading should start at offset `offset`; the OS will generally read your file
 * in chunks of 4K byte.
 * Returns the number of bytes read (writting into `buf`), or < 0 on error.
 */
static int sfs_read(const char *path,
                    char *buf,
                    size_t size,
                    off_t offset,
                    struct fuse_file_info *fi)
{
    (void)fi;
    log("read %s size=%zu offset=%ld\n", path, size, offset);

    struct sfs_entry *file = malloc(SFS_ROOTDIR_SIZE);
    
    int res = get_entry(path, file, NULL, NULL);
    if(res == 0)
    {
        blockidx_t curr_block = file->first_block;
        blockidx_t block_offset = offset / SFS_BLOCK_SIZE;
        off_t block_relative_offset = offset % SFS_BLOCK_SIZE;

        for(blockidx_t i = 0; i < block_offset; i++)
        {
            disk_read(&curr_block, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + curr_block * sizeof(blockidx_t));
        }

        size_t read = 0;
        size_t prev_size = size;
        size_t curr_read_size = 0;

        while(curr_block != SFS_BLOCKIDX_END)
        {

            prev_size = size;
            if(block_relative_offset > 0)
            {
                size -= SFS_BLOCK_SIZE - block_relative_offset;
                block_relative_offset = 0;
            }
            else size -= SFS_BLOCK_SIZE;

            if(prev_size < size)
            {
                disk_read(buf, prev_size, SFS_DATA_OFF + curr_block * SFS_BLOCK_SIZE);
                read += prev_size;
                break;
            }
            
            curr_read_size = prev_size - size;
            disk_read(buf, curr_read_size, SFS_DATA_OFF + curr_block * SFS_BLOCK_SIZE);
            buf += curr_read_size;
            read += curr_read_size;

            disk_read(&curr_block, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + curr_block * sizeof(blockidx_t));
        }

        return read;
    }

    return res;
}

static unsigned find_free_entry(unsigned offset)
{
    unsigned entries_n = offset == SFS_ROOTDIR_NENTRIES ? SFS_ROOTDIR_NENTRIES : SFS_DIR_NENTRIES;
    unsigned entries_size = entries_n * sizeof(struct sfs_entry);

    struct sfs_entry entries[entries_n];
    
    disk_read(entries, entries_size, offset);

    unsigned ret_offset = 0;
    for(unsigned i = 0; i < entries_n; i++)
    {
        if(strlen(entries[i].filename) == 0)
        {
            ret_offset = offset +  i * sizeof(struct sfs_entry);
            break;
        }
    }
    return ret_offset;
}

static void read_block_tbl(blockidx_t* block_tbl)
{
    disk_read(block_tbl, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);
}

static blockidx_t find_free_block_pointer(blockidx_t from, blockidx_t* block_tbl)
{
    if(block_tbl == NULL) {
        block_tbl = malloc(SFS_BLOCKTBL_SIZE);
        read_block_tbl(block_tbl);
    }
    
    for(blockidx_t i = from; i < SFS_BLOCKTBL_NENTRIES; i++)
    {
        if(block_tbl[i] == SFS_BLOCKIDX_EMPTY)
        {
            return i;
        }
    }
    return SFS_BLOCKIDX_END;
}

static int is_name_too_long(char* fname)
{
    return strlen(fname) > SFS_FILENAME_MAX;
}

static void read_dir_path_and_fname(const char *path, char** ret_dir_path, char** ret_fname)
{
    size_t dir_path_len = strlen(path) + 1;
    char* dir_path = malloc(dir_path_len);
    bzero(dir_path, dir_path_len);
    strcpy(dir_path, "/");

    char* part = strtok(strdup(path), "/");
    *ret_fname = part;

    while((part = strtok(NULL, "/"))) {
        strcat(dir_path, *ret_fname);
        strcat(dir_path, "/");
        *ret_fname = part;

        log("fname len: %s\n", *ret_fname);
    }
    
    *ret_dir_path = dir_path;
}

struct sfs_entry empty_entry()
{
    struct sfs_entry empty_entry;
    empty_entry.filename[0] = '\0';
    empty_entry.first_block = SFS_BLOCKIDX_EMPTY;
    empty_entry.size = 0;
    return empty_entry;
}

/*
 * Create directory at `path`.
 * The `mode` argument describes the permissions, which you may ignore for this
 * assignment.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_mkdir(const char *path,
                     mode_t mode)
{
    log("mkdir %s mode=%o\n", path, mode);

    char* part;
    char* dir_path;

    read_dir_path_and_fname(path, &dir_path, &part);
    if(is_name_too_long(part)) return -ENAMETOOLONG;
    
    struct sfs_entry dir;
    unsigned offset;
    int res = get_entry(path, &dir, &offset, NULL);

    if(res == 0) res = -EEXIST;
    else
    {
        unsigned part_offset = 0;
        blockidx_t* blck_tbl = malloc(SFS_BLOCKTBL_SIZE);
        read_block_tbl(blck_tbl);
        
        part_offset = find_free_entry(offset);
        if(part_offset == 0)
        {
            res = -ENOBUFS;
        }
        else
        {
            blockidx_t bl_first_half = 0;
        
            bl_first_half = find_free_block_pointer(bl_first_half, blck_tbl);
            if(bl_first_half == SFS_BLOCKIDX_END || bl_first_half + 1 >= SFS_BLOCKTBL_NENTRIES) return -ENOBUFS;
            
            
            strcpy(dir.filename, part);
            dir.first_block = bl_first_half;
            dir.size = SFS_DIRECTORY;

            disk_write(&dir, sizeof(struct sfs_entry), part_offset);

            struct sfs_entry empty = empty_entry();

            unsigned new_dir_offset = SFS_DATA_OFF + bl_first_half * SFS_BLOCK_SIZE;
            for(unsigned i = 0; i < SFS_DIR_NENTRIES; i++) 
            {
                disk_write(&empty, sizeof(struct sfs_entry), new_dir_offset + i * sizeof(struct sfs_entry));
            }

            blockidx_t entry_blocks[2] = {bl_first_half + 1, SFS_BLOCKIDX_END};
            disk_write(&entry_blocks, sizeof(blockidx_t) * 2, SFS_BLOCKTBL_OFF + bl_first_half * sizeof(blockidx_t));
        }
        res = 0;
    }
    return res;
}


/*
 * Remove directory at `path`.
 * Directories may only be removed if they are empty, otherwise this function
 * should return -ENOTEMPTY.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_rmdir(const char *path)
{
    log("rmdir %s\n", path);

    struct sfs_entry dir;
    unsigned dir_offset = 0;
    unsigned entry_offset = 0;

    int res = get_entry(path, &dir, &dir_offset, &entry_offset);

    int is_empty = 1;
    struct sfs_entry* dir_entries = malloc(SFS_DIR_SIZE);
    disk_read(dir_entries, SFS_DIR_SIZE, dir_offset);

    for(unsigned i = 0; i < SFS_DIR_NENTRIES; i++)
    {
        if(strlen(dir_entries[i].filename) != 0)
        {
            is_empty = 0;
            break;
        }
    }

    if(res == 0 && is_empty)
    {
        blockidx_t empty_dir_blocks[2] = {SFS_BLOCKIDX_EMPTY, SFS_BLOCKIDX_EMPTY};
        disk_write(&empty_dir_blocks, sizeof(blockidx_t) * 2, SFS_BLOCKTBL_OFF + dir.first_block * sizeof(blockidx_t));

        struct sfs_entry empty_en = empty_entry();
        log("entry offset %u\n", entry_offset);
        disk_write(&empty_en, sizeof(struct sfs_entry), entry_offset);
    }
    else if(res == 0) res = -ENOTEMPTY;

    return res;
}

/*
 * Remove file at `path`.
 * Can not be used to remove directories.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_unlink(const char *path)
{
    log("unlink %s\n", path);

    return -ENOSYS;
}


/*
 * Create an empty file at `path`.
 * The `mode` argument describes the permissions, which you may ignore for this
 * assignment.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_create(const char *path,
                      mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)fi;
    log("create %s mode=%o\n", path, mode);

    struct sfs_entry dir_file;
    unsigned offset;
    char* fname = NULL;
    char* dir_path = NULL;

    read_dir_path_and_fname(path, &dir_path, &fname);
    if(is_name_too_long(fname)) return -ENAMETOOLONG;

    int res = get_entry(dir_path, &dir_file, &offset, NULL);
    if(res == 0)
    {
        unsigned file_offset = find_free_entry(offset);

        if(file_offset == 0) res = -1;
        else 
        {
            blockidx_t bl = 0;
            bl = find_free_block_pointer(bl, NULL);

            if(bl == SFS_BLOCKIDX_END) res = -1;
            else
            {
                struct sfs_entry entry;
                entry.first_block = SFS_BLOCKIDX_END;
                strcpy(entry.filename, fname);
                entry.size = 0;

                disk_write(&entry, sizeof(struct sfs_entry), file_offset);
            }
        }


    }

    
    return res;
}


/*
 * Shrink or grow the file at `path` to `size` bytes.
 * Excess bytes are thrown away, whereas any bytes added in the process should
 * be nil (\0).
 * Returns 0 on success, < 0 on error.
 */
static int sfs_truncate(const char *path, off_t size)
{
    log("truncate %s size=%ld\n", path, size);

    return -ENOSYS;
}


/*
 * Write contents of `buf` (of `size` bytes) to the file at `path`.
 * The file is grown if nessecary, and any bytes already present are overwritten
 * (whereas any other data is left intact). The `offset` argument specifies how
 * many bytes should be skipped in the file, after which `size` bytes from
 * buffer are written.
 * This means that the new file size will be max(old_size, offset + size).
 * Returns the number of bytes written, or < 0 on error.
 */
static int sfs_write(const char *path,
                     const char *buf,
                     size_t size,
                     off_t offset,
                     struct fuse_file_info *fi)
{
    (void)fi;
    log("write %s data='%.*s' size=%zu offset=%ld\n", path, (int)size, buf,
        size, offset);

    return -ENOSYS;
}


/*
 * Move/rename the file at `path` to `newpath`.
 * Returns 0 on succes, < 0 on error.
 */
static int sfs_rename(const char *path,
                      const char *newpath)
{
    /* Implementing this function is optional, and not worth any points. */
    log("rename %s %s\n", path, newpath);

    return -ENOSYS;
}


static const struct fuse_operations sfs_oper = {
    .getattr    = sfs_getattr,
    .readdir    = sfs_readdir,
    .read       = sfs_read,
    .mkdir      = sfs_mkdir,
    .rmdir      = sfs_rmdir,
    .unlink     = sfs_unlink,
    .create     = sfs_create,
    .truncate   = sfs_truncate,
    .write      = sfs_write,
    .rename     = sfs_rename,
};


#define OPTION(t, p)                            \
    { t, offsetof(struct options, p), 1 }
#define LOPTION(s, l, p)                        \
    OPTION(s, p),                               \
    OPTION(l, p)
static const struct fuse_opt option_spec[] = {
    LOPTION("-i %s",    "--img=%s",     img),
    LOPTION("-b",       "--background", background),
    LOPTION("-v",       "--verbose",    verbose),
    LOPTION("-h",       "--help",       show_help),
    OPTION(             "--fuse-help",  show_fuse_help),
    FUSE_OPT_END
};

static void show_help(const char *progname)
{
    printf("usage: %s mountpoint [options]\n\n", progname);
    printf("By default this FUSE runs in the foreground, and will unmount on\n"
           "exit. If something goes wrong and FUSE does not exit cleanly, use\n"
           "the following command to unmount your mountpoint:\n"
           "  $ fusermount -u <mountpoint>\n\n");
    printf("common options (use --fuse-help for all options):\n"
           "    -i, --img=FILE      filename of SFS image to mount\n"
           "                        (default: \"%s\")\n"
           "    -b, --background    run fuse in background\n"
           "    -v, --verbose       print debug information\n"
           "    -h, --help          show this summarized help\n"
           "        --fuse-help     show full FUSE help\n"
           "\n", default_img);
}

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.img = strdup(default_img);

    fuse_opt_parse(&args, &options, option_spec, NULL);

    if (options.show_help) {
        show_help(argv[0]);
        return 0;
    }

    if (options.show_fuse_help) {
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0][0] = '\0';
    }

    if (!options.background)
        assert(fuse_opt_add_arg(&args, "-f") == 0);

    disk_open_image(options.img);

    return fuse_main(args.argc, args.argv, &sfs_oper, NULL);
}
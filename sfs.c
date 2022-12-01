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
                     unsigned *ret_entry_off)
{
    
    (void) *ret_entry_off;
    char* entry_fname = strtok(strdup(path), "/");
    unsigned off = 0;

    if(entry_fname != NULL)
    {
        struct sfs_entry* entries = malloc(SFS_ROOTDIR_SIZE);
        off = SFS_ROOTDIR_OFF;
        disk_read(entries, SFS_ROOTDIR_SIZE, off);

        int pos = 0;

        *ret_entry = find_entry(entries, SFS_ROOTDIR_NENTRIES, entry_fname, &pos);

        if(pos == -1) return -ENOENT;
        else if(ret_entry->first_block != SFS_BLOCKIDX_END)
        {
            while((entry_fname = strtok(NULL, "/")) && ret_entry->first_block != SFS_BLOCKIDX_END)
            {
                off = SFS_DATA_OFF + ret_entry->first_block * SFS_BLOCK_SIZE;
                disk_read(entries, SFS_DIR_SIZE, off);
                *ret_entry = find_entry(entries, SFS_DIR_NENTRIES, entry_fname, &pos);

                if(pos == -1) return -ENOENT;
                else if(ret_entry->first_block == SFS_BLOCKIDX_END) break;
            }
        }
    }
    else
    {
        ret_entry->first_block = 0;
        ret_entry->size = SFS_DIRECTORY;
        strcpy(ret_entry->filename, "/");
    }

    if(ret_entry_off != NULL) *ret_entry_off = off;
    
    return 0;
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

    res = get_entry(path, &entry, NULL);

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
    int res = get_entry(path, &dir, NULL);

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
    }
    else return -ENOSYS;

    for (unsigned i = 0; i < dir_length; i++)
    {
        if(strlen(dir_entries[i].filename) > 0)
        {
            filler(buf, dir_entries[i].filename, NULL, 0);
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
    
    int res = get_entry(path, file, NULL);
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

static struct sfs_entry* find_free_entry(unsigned offset)
{
    unsigned entries_n = offset == SFS_ROOTDIR_NENTRIES ? SFS_ROOTDIR_NENTRIES : SFS_DIR_NENTRIES;
    unsigned entries_size = entries_n * sizeof(struct sfs_entry);

    struct sfs_entry* entries = malloc(entries_size);
    
    disk_read(entries, entries_size, offset);

    for(unsigned i = 0; i < entries_n; i++)
    {
        if(strlen(entries[i].filename) == 0)
        {
            return &entries[i];
        }
    }
    return NULL;
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

    // check if entry exists already

    struct sfs_entry dir;
    int res = get_entry(path, &dir, NULL);

    if(res == 0) res = -EEXIST;
    else
    {
        //create directory
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

    return -ENOSYS;
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

    return -ENOSYS;
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
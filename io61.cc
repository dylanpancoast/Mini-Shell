#include "io61.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>
#include <iostream>
#include <sys/mman.h>


// io61_file
//    Data structure for io61 file wrappers.

struct io61_file {
    // file descriptor
    int fd = -1; 
    
    // Mode that file is currently in
    int current_mode; 
    bool mmapped = false;

    // Cache block size
    static constexpr off_t bufsize = 4096;
    // Cached data is stored in `cbuf`
    unsigned char cbuf[bufsize];

    // The following “tags” are addresses—file offsets—that describe the cache’s contents.
    // `tag`: File offset of first byte of cached data (0 when file is opened).
    off_t tag;
    // `end_tag`: File offset one past the last byte of cached data (0 when file is opened).
    off_t end_tag;
    // `pos_tag`: Cache position: file offset of the cache.
    // In read caches, this is the file offset of the next character to be read.
    off_t pos_tag;

    // Addr value that tell us where at in mmapping user is reading from or writing to
    uintptr_t addr;
    // Memory block where mapping was made
    char* file_in_vm;
    // File data
    struct stat sb;
};


// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file or O_WRONLY for a write-only file.
//    You need not support read/write files.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    // Initialize file data
    io61_file* f = new io61_file;
    f->fd = fd;
    f->current_mode = mode;
    // If we can get stats on file, then we prefer to mmap; otherwise use single-slot cache
    if (fstat(f->fd, &f->sb) == 0 && f->current_mode == O_RDONLY) {
        // Try to mmap file into contiguous virtual memory
        f->file_in_vm = (char*) mmap(NULL, f->sb.st_size, PROT_READ, MAP_PRIVATE, f->fd, 0);
        if (f->file_in_vm != (void*) -1) {
            // User will engage with file via a mmap
            f->mmapped = true;
            f->addr = (uintptr_t) f->file_in_vm;
        }
    }
    return f;
}


// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    if (f->mmapped) {
        munmap(f->file_in_vm, f->sb.st_size);
    } else {
        io61_flush(f);
    }
    int r = close(f->fd);
    delete f;
    return r;
}


// io61_fill(f)
// Fill the read cache with new data, starting from file offset `end_tag`
// Only called for read caches

void io61_fill(io61_file* f) {
    // Reset the cache buffer to empty
    f->tag = f->pos_tag = f->end_tag;

    // Read data from file on disk to cache buffer
    ssize_t n = read(f->fd, f->cbuf, f->bufsize);
    if (n >= 0) {
        f->end_tag = f->tag + n;
    }
}

// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

int io61_readc(io61_file* f) {
    if (f->mmapped) {
        if (f->addr == (uintptr_t) f->file_in_vm + f->sb.st_size) {
            errno = 0;      // clear `errno` to indicate EOF
            return -1;
        }
        unsigned char* ch_ptr = (unsigned char*) f->addr;
        ++f->addr;
        return *ch_ptr;
    }
    if (f->pos_tag == f->end_tag) {
        // If cache buffer is empty, fill it
        io61_fill(f);
        if (f->pos_tag == f->end_tag) {
            errno = 0;      // clear `errno` to indicate EOF
            return -1;
        }
    }
    unsigned char ch = f->cbuf[f->pos_tag - f->tag];
    ++f->pos_tag;
    return ch;
}

// io61_read(f, buf, sz)
//    Reads up to `sz` bytes from `f` into `buf`. Returns the number of
//    bytes read on success. Returns 0 if end-of-file is encountered before
//    any bytes are read, and -1 if an error is encountered before any
//    bytes are read.
//
//    Note that the return value might be positive, but less than `sz`,
//    if end-of-file or error is encountered before all `sz` bytes are read.
//    This is called a “short read.”

ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    if (f->mmapped) {
        size_t cpy = std::min(sz, (size_t) (f->sb.st_size - (f->addr - (uintptr_t) f->file_in_vm)));
        memcpy(buf, (void*) f->addr, cpy);
        f->addr += cpy;
        return cpy;
    }
    // Initialize position into user buffer
    size_t pos = 0;
    while (pos < sz) {
        if (f->pos_tag == f->end_tag) {
            // If cache buffer is empty, fill it
            io61_fill(f);
            if (f->pos_tag == f->end_tag) {
                break;
            }
        }

        // Determine how much data to copy into user buffer
        size_t cbuf_rmng = f->end_tag - f->pos_tag;   
        size_t sz_rmng = sz - pos;
        size_t cpy = std::min(cbuf_rmng, sz_rmng);
        
        // Copy data from cache buffer to user buffer
        memcpy(&buf[pos], &f->cbuf[f->pos_tag - f->tag], cpy);

        // Update positions in cache/user buffers
        f->pos_tag += cpy;
        pos += cpy;
    }
    return pos;
}


// io61_flush(f)
//    Forces a write of any cached data written to `f`. Returns 0 on
//    success. Returns -1 if an error is encountered before all cached
//    data was written.
//
//    If `f` was opened read-only, `io61_flush(f)` returns 0. If may also
//    drop any data cached for reading.

int io61_flush(io61_file* f) {
    if (f->current_mode == O_RDONLY) {
        // f was opened read only
        return 0;
    }
    // Write data from cache buffer to file on disk
    off_t pos = 0;
    while (pos < f->pos_tag - f->tag) {
        ssize_t n = write(f->fd, f->cbuf, f->pos_tag - f->tag);
        if ((errno == EINTR || errno == EAGAIN) && n == -1) {
            // Restartable error encountered
            continue;
        }
        pos += n;
        if (n != f->pos_tag - f->tag) {
            return -1;
        }
    }

    // Update cache buffer's relative starting position
    f->tag = f->pos_tag;
    return 0;
}


// io61_writec(f)
//    Write a single character `ch` to `f`. Returns 0 on success and
//    -1 on error.

int io61_writec(io61_file* f, int ch) {
    if (f->end_tag == f->tag + f->bufsize) {
        // If cache buffer is full, flush it
        if (io61_flush(f) == -1) {
            return -1;
        }
    }
    f->cbuf[f->pos_tag - f->tag] = ch;
    ++f->pos_tag;
    ++f->end_tag;
    return 0;
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {   
    // Initialize position into user buffer
    size_t pos = 0;
    while (pos < sz) {
        if (f->end_tag == f->tag + f->bufsize) {
            // If cache buffer is full, flush it
            if (io61_flush(f) == -1) {
                return pos;
            }
        }

        // Determine how much data to write into cache buffer
        off_t sz_rmng = sz - pos;
        off_t cpy = std::min(f->bufsize - (f->pos_tag - f->tag), sz_rmng);
        
        // Copy data from cache buffer to user buffer
        memcpy(&f->cbuf[f->pos_tag - f->tag], (void*) &buf[pos], cpy);

        // Update positions in cache/user buffers and cache buffer's relative starting position
        f->pos_tag += cpy;
        f->end_tag += cpy;
        pos += cpy;
    }
    return pos;
}


// io61_seek(f, pos)
//    Changes the file pointer for file `f` to `pos` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t pos) {
    if (f->mmapped) {
        if (pos < f->sb.st_size && pos >= 0) {
            f->addr = (uintptr_t) f->file_in_vm + pos;
            return 0;
        } else {
            return -1;
        }
    }
    
    if (pos < f->end_tag && pos >= f->tag) {
        // pos byte found in the current cach buffer
        f->pos_tag = pos;
        return 0;
    }

    // File is using a cache buffer
    if (f->current_mode == O_RDONLY) {
        // Align the cache buffer to a block which has pos byte
        off_t pos_aligned = pos - (pos % f->bufsize);
        // Search for the pos byte
        ssize_t n = lseek(f->fd, pos_aligned, SEEK_SET);
        if (pos_aligned == n || 0 == n) {
            // Update the cache buffer
            f->tag = f->pos_tag = f->end_tag = pos_aligned;
            io61_fill(f);
            f->pos_tag = pos;
            return 0;
        }
    } 
    else if (f->current_mode == O_WRONLY) {
        // Flush out the dirty cache
        if (io61_flush(f) == -1) {
            return -1;
        }
        // Search for the pos byte
        if (lseek(f->fd, pos, SEEK_SET) == -1) {
            return -1;
        }
        // Update the cache buffer
        f->tag = f->pos_tag = f->end_tag = pos;
        return 0;
    }
    return -1;
}


// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Opens the file corresponding to `filename` and returns its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_fileno(f)
//    Returns the file descriptor associated with `f`.

int io61_fileno(io61_file* f) {
    return f->fd;
}


// io61_filesize(f)
//    Returns the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}
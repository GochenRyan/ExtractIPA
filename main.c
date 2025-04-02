#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>

#include <zlib.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(dir) _mkdir(dir)
#define STAT _stat
#else
#include <unistd.h>
#define MKDIR(dir) mkdir(dir, 0755)
#define STAT stat
#endif

// ZIP format constants
#define LOCAL_HEADER_SIGNATURE 0x04034b50
#define CENTRAL_HEADER_SIGNATURE 0x02014b50
#define END_OF_CENTRAL_DIRECTORY_SIGNATURE 0x06054b50
#define CENTRAL_HEADER_DIGITAL_SIGNATURE 0x05054b50
#define ARCHIVE_EXTRA_DATA_SIGNATURE 0x07064b50
#define ZIP64_CENTRAL_FILE_HEADER_SIGNATURE 0x06064b50
#define BUFFER_SIZE 4096

#define COMPRESSION_STORE 0       // No compression
#define COMPRESSION_DEFLATE 8     // DEFLATE compression
#define FLAG_DATA_DESCRIPTOR 0x08 // Bit flag for data descriptor

// Pack structs to avoid padding
#pragma pack(push, 1)
typedef struct {
    uint32_t signature;       // Local file header signature
    uint16_t version;         // Version needed to extract
    uint16_t flags;          // General purpose bit flag
    uint16_t compression;     // Compression method
    uint16_t mod_time;       // Last mod file time
    uint16_t mod_date;       // Last mod file date
    uint32_t crc32;          // CRC-32
    uint32_t compressed_size; // Compressed size
    uint32_t uncompressed_size; // Uncompressed size
    uint16_t name_length;     // Filename length
    uint16_t extra_length;    // Extra field length
} LocalFileHeader;
#pragma pack(pop)

// Parser state structure
typedef struct {
    FILE* fp;            // File pointer to ZIP archive
    char filename[256];  // Current entry filename
    uint64_t comp_size;  // Actual compressed size
    uint64_t uncomp_size;// Actual uncompressed size
    uint16_t compression;// Compression method
    uint16_t name_length;     // Filename length
    uint16_t extra_length;    // Extra field length
    uint16_t flags;      // Bit flags
    int64_t data_start;     // Start position of file data
    int64_t header_start;
    bool consumed;
} ZipParser;

/* Open ZIP file and initialize parser */
ZipParser* zip_open(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;

    ZipParser* zp = calloc(1, sizeof(ZipParser));
    zp->fp = fp;
    zp->header_start = -1;
    zp->data_start = -1;
    return zp;
}

int zip_skip_until_next_entry(ZipParser* zp) {
    uint32_t signature;
    size_t read_size;
    uint8_t buffer[BUFFER_SIZE];

    // Read the file in chunks (BUFFER_SIZE)
    while (1) {
        uint64_t start = _ftelli64(zp->fp);

        // Read a chunk of data into the buffer
        read_size = fread(buffer, 1, BUFFER_SIZE, zp->fp);
        if (read_size == 0) {
            return 0; // No more data to read
        }

        // Process the buffer one byte at a time
        for (size_t i = 0; i < read_size - 3; ++i) {
            // Read 4-byte signature safely
            memcpy(&signature, buffer + i, sizeof(uint32_t));

            // Check if the signature matches the Local Header
            if (signature == LOCAL_HEADER_SIGNATURE) {
                zp->header_start = start + i;
                return 1; // Found Local File Header
            }

            // Check for Central Header or End of Central Directory
            if (signature == CENTRAL_HEADER_SIGNATURE ||
                signature == END_OF_CENTRAL_DIRECTORY_SIGNATURE ||
                signature == CENTRAL_HEADER_DIGITAL_SIGNATURE ||
                signature == ARCHIVE_EXTRA_DATA_SIGNATURE ||
                signature == ZIP64_CENTRAL_FILE_HEADER_SIGNATURE) {
                return 0;
            }
        }

        // Move file pointer to continue searching
        _fseeki64(zp->fp, start + read_size - 3, SEEK_SET);
    }
}

void reset_entry(ZipParser* zp) {
    memset(zp->filename, 0, sizeof(zp->filename));
    zp->comp_size = 0;
    zp->uncomp_size = 0;
    zp->compression = 0;
    zp->name_length = 0;
    zp->extra_length = 0;
    zp->flags = 0;
    zp->data_start = 0;
    zp->header_start = 0;
    zp->consumed = false;
}


void close_entry(ZipParser* zp)
{
    if (zp->compression == COMPRESSION_DEFLATE)
    {
        // Buffers for reading compressed data and writing decompressed data
        unsigned char in[BUFFER_SIZE];  // Input buffer for compressed data
        unsigned char out[BUFFER_SIZE]; // Output buffer for decompressed data
        size_t read_size;
        z_stream strm;
        int ret = 0;

        // Initialize zlib decompression stream
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        inflateInit2(&strm, -MAX_WBITS); // Negative for raw DEFLATE

        _fseeki64(zp->fp, zp->data_start, SEEK_SET);

        uint64_t total_in_size = 0;
        uint64_t read_int_size = 0;

        do {
            strm.avail_in = fread(in, 1, sizeof(in), zp->fp);
            read_int_size = strm.avail_in;
            if (ferror(zp->fp)) break;
            strm.next_in = in;

            do {
                strm.avail_out = sizeof(out);
                strm.next_out = out;
                ret = inflate(&strm, Z_NO_FLUSH);

                if (ret == Z_STREAM_ERROR) break;

            } while (strm.avail_out == 0);

            total_in_size += (read_int_size - strm.avail_in);

        } while (ret != Z_STREAM_END);


        // NOTICE: The type of total_in is 'unsigned long',  which is only 4 bytes on WIN64
        _fseeki64(zp->fp, zp->data_start + total_in_size, SEEK_SET);

        inflateEnd(&strm);
    }
    else if (zp->compression == COMPRESSION_STORE) 
    {
        _fseeki64(zp->fp, zp->data_start + zp->comp_size, SEEK_SET);
    }

    reset_entry(zp);
}



/* Get next entry in ZIP file */
int zip_get_next_entry(ZipParser* zp) {
    if (zp->consumed == false && zp->header_start != -1)
    {
        close_entry(zp);
    }
    // Seek to current scanning position
    if (!zip_skip_until_next_entry(zp))
        return 0;

    _fseeki64(zp->fp, zp->header_start, SEEK_SET);

    // Read local file header
    LocalFileHeader lfh;
    if (fread(&lfh, sizeof(lfh), 1, zp->fp) != 1)
        return 0;

    // Verify signature
    if (lfh.signature != LOCAL_HEADER_SIGNATURE)
        return 0;

    // Read filename
    fread(zp->filename, lfh.name_length, 1, zp->fp);
    zp->filename[lfh.name_length] = '\0';

    // Skip extra field
    _fseeki64(zp->fp, lfh.extra_length, SEEK_CUR);

    // Store compression info
    zp->compression = lfh.compression;
    zp->flags = lfh.flags;
    zp->name_length = lfh.name_length;
    zp->extra_length = lfh.extra_length;
    zp->data_start = _ftelli64(zp->fp);  // Data starts here

    // Handle case where sizes are in data descriptor
    if ((zp->flags & FLAG_DATA_DESCRIPTOR) && lfh.compressed_size == 0) {
        zp->comp_size = 0;
    }
    else {
        zp->comp_size = lfh.compressed_size;
        zp->uncomp_size = lfh.uncompressed_size;
    }

    if (zp->compression == COMPRESSION_STORE)
    {
        assert(zp->flags != FLAG_DATA_DESCRIPTOR && "Store method, but exists data descriptor");
    }

    return 1;
}

/* Extract current entry to output path */
int extract_current(ZipParser* zp, const char* output_path) {
    FILE* out = fopen(output_path, "wb");
    if (!out) return 0;

    int result = 0;
    _fseeki64(zp->fp, zp->data_start, SEEK_SET);

    // Handle different compression methods
    if (zp->compression == COMPRESSION_STORE) {
        // Simple store - just copy data
        char* buffer = malloc(zp->comp_size);
        _fseeki64(zp->fp, zp->data_start, SEEK_SET);
        fread(buffer, zp->comp_size, 1, zp->fp);
        fwrite(buffer, zp->comp_size, 1, out);
        free(buffer);
        result = 1;
        zp->consumed = true;
    }
    else if (zp->compression == COMPRESSION_DEFLATE) {
        // Use zlib for DEFLATE decompression
        _fseeki64(zp->fp, zp->data_start, SEEK_SET);
        z_stream strm;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        inflateInit2(&strm, -MAX_WBITS); // Negative for raw DEFLATE

        uint8_t in[4096];
        uint8_t out_buf[4096];
        int ret;

        uint64_t total_in_size = 0;
        uint64_t read_int_size = 0;

        do {
            strm.avail_in = fread(in, 1, sizeof(in), zp->fp);
            read_int_size = strm.avail_in;
            if (ferror(zp->fp)) break;
            strm.next_in = in;

            do {
                strm.avail_out = sizeof(out_buf);
                strm.next_out = out_buf;
                ret = inflate(&strm, Z_NO_FLUSH);

                if (ret == Z_STREAM_ERROR) break;

                size_t have = sizeof(out_buf) - strm.avail_out;
                fwrite(out_buf, 1, have, out);
            } while (strm.avail_out == 0);

            total_in_size += (read_int_size - strm.avail_in);
        } while (ret != Z_STREAM_END);

        // NOTICE: The type of total_in is 'unsigned long',  which is only 4 bytes on WIN64
        _fseeki64(zp->fp, zp->data_start + total_in_size, SEEK_SET);

        inflateEnd(&strm);
        result = (ret == Z_STREAM_END) ? 1 : 0;
        
        zp->consumed = true;
    }

    fclose(out);
    return result;
}

/* Close ZIP file and cleanup */
void zip_close(ZipParser* zp) {
    if (zp) {
        fclose(zp->fp);
        free(zp);
    }
}

// Check if the path exists (either file or directory)
int exists(const char* path) {
    struct STAT st;
    return (STAT(path, &st) == 0);
}

// Recursively create directories from the drive letter.
// It checks the path step by step and creates directories that do not exist.
int create_directories(const char* path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        fprintf(stderr, "Path is too long.\n");
        return -1;
    }
    strcpy(tmp, path);

    // On Windows, replace backslashes with forward slashes for easier processing
    for (size_t i = 0; i < len; i++) {
        if (tmp[i] == '\\')
            tmp[i] = '/';
    }

    // If the path ends with '/', remove the trailing '/'
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    // For Windows paths starting with a drive letter, e.g., "C:/"
    char partial[1024] = { 0 };
    size_t pos = 0;
    if (len >= 2 && (tmp[1] == ':' && (tmp[2] == '/' || tmp[2] == '\\'))) {
        // Copy the drive letter part into partial (e.g., "C:/")
        strncpy(partial, tmp, 3);
        partial[3] = '\0';
        pos = 3;
    }

    // Traverse the rest of the path one character at a time
    for (; pos < strlen(tmp); pos++) {
        if (tmp[pos] == '/') {
            // Temporarily terminate the string to form the current directory path
            partial[pos] = '\0';
            if (!exists(partial)) {
                if (MKDIR(partial) != 0) {
                    fprintf(stderr, "Failed to create directory %s: %s\n", partial, strerror(errno));
                    return -1;
                }
            }
            // Restore the separator
            partial[pos] = '/';
        }
        else {
            // Append the current character to partial
            partial[pos] = tmp[pos];
        }
    }
    // Check the last directory
    if (!exists(partial)) {
        if (MKDIR(partial) != 0) {
            fprintf(stderr, "Failed to create directory %s: %s\n", partial, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int vasprintf(char** PTR, const char* TEMPLATE, va_list AP)
{
    int res;
    char buf[16];
    res = vsnprintf(buf, 16, TEMPLATE, AP);
    if (res > 0) {
        *PTR = (char*)malloc(res + 1);
        res = vsnprintf(*PTR, res + 1, TEMPLATE, AP);
    }
    return res;
}

static int asprintf(char** PTR, const char* TEMPLATE, ...)
{
    int res;
    va_list AP;
    va_start(AP, TEMPLATE);
    res = vasprintf(PTR, TEMPLATE, AP);
    va_end(AP);
    return res;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <source_file> <destination_directory>\n", argv[0]);
        return 1;
    }

    const char *src_file = argv[1];
    const char* dst_dir = argv[2];

    ZipParser* zp = zip_open(src_file);
    if (!zp) return 1;

    while (zip_get_next_entry(zp)) {
        const char* z_name = zp->filename;
        char* dstpath = NULL;

        if ((asprintf(&dstpath, "%s/%s", dst_dir, z_name) > 0) && dstpath) {
            if (z_name[strlen(z_name) - 1] == '/') {
                create_directories(dstpath);
            } 
            else {
                printf("Extracting: %s\n", z_name);
                if (!extract_current(zp, dstpath)) {
                    printf("Failed to extract: %s\n", z_name);
                }
            }
        }
    }

    zip_close(zp);
    return 0;
}
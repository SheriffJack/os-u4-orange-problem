// index.c — Staging area implementation
//
// The index is a text file (.pes/index) where each line has the format:
//   <mode> <hash-hex> <mtime_sec> <size> <path>
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Forward declarations (from object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry *index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            memmove(&index->entries[i], &index->entries[i + 1],
                    (size_t)(index->count - i - 1) * sizeof(IndexEntry));
            index->count--;
            return 0;
        }
    }
    return -1;
}

// Compare two IndexEntry pointers by path (for qsort)
static int cmp_index_entry(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged++;
    }
    if (!staged) printf("  (nothing to show)\n");

    printf("\nUnstaged changes:\n");
    printf("  (nothing to show)\n");

    printf("\nUntracked files:\n");
    printf("  (nothing to show)\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index into memory.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // File doesn't exist yet — empty index, not an error
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(f);
            return -1;
        }

        IndexEntry *e = &index->entries[index->count];

        char hash_hex[HASH_HEX_SIZE + 1];
        unsigned int mode;
        unsigned long long mtime_sec;
        unsigned int size;
        char path[512];

        int parsed = sscanf(line, "%o %64s %llu %u %511s",
                            &mode, hash_hex, &mtime_sec, &size, path);
        if (parsed != 5) continue;

        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime_sec;
        e->size      = (uint32_t)size;
        snprintf(e->path, sizeof(e->path), "%s", path);

        if (hex_to_hash(hash_hex, &e->hash) != 0) continue;

        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index using atomic write (temp file + rename).
int index_save(const Index *index) {
    // Sort entries by path before writing
    Index sorted = *index;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(IndexEntry), cmp_index_entry);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }

    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hash_hex);
        fprintf(f, "%o %s %llu %u %s\n",
                e->mode,
                hash_hex,
                (unsigned long long)e->mtime_sec,
                e->size,
                e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, INDEX_FILE);
}

// Stage a file: read its contents, write as a blob, update/add index entry.
int index_add(Index *index, const char *path) {
    // 1. Open and read the file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }

    uint8_t *contents = malloc((size_t)file_size + 1);
    if (!contents) { fclose(f); return -1; }

    size_t read_bytes = fread(contents, 1, (size_t)file_size, f);
    fclose(f);
    if (read_bytes != (size_t)file_size) { free(contents); return -1; }

    // 2. Write as a blob object and get the hash
    ObjectID blob_id;
    int rc = object_write(OBJ_BLOB, contents, (size_t)file_size, &blob_id);
    free(contents);
    if (rc != 0) return -1;

    // 3. Stat the file to get mtime and size
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    // 4. Determine file mode
    uint32_t mode;
    if (st.st_mode & S_IXUSR)
        mode = 0100755;
    else
        mode = 0100644;

    // 5. Find existing entry or create a new one
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    // 6. Update the entry
    e->mode      = mode;
    e->hash      = blob_id;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size      = (uint32_t)st.st_size;

    // 7. Persist to disk
    return index_save(index);
}

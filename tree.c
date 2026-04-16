// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Forward declaration (from object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Recursive helper: builds a tree object for the slice of index entries
// that share the given path prefix (at the given depth level).
//
// entries  – pointer into the index entries array
// count    – number of entries in this slice
// prefix   – the directory prefix for this level (empty string for root)
// id_out   – receives the ObjectID of the written tree object
//
// All entry paths at this level are relative to 'prefix'. For example,
// at the root level "src/main.c" and "src/util.c" are grouped under "src/".
static int write_tree_level(const IndexEntry *entries, int count,
                            const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;

        // Strip the prefix to get the relative name at this level
        size_t prefix_len = strlen(prefix);
        const char *rel = path + prefix_len; // relative name within this dir

        // Is there a '/' in the remaining path? If so, this entry lives
        // inside a subdirectory at this level.
        const char *slash = strchr(rel, '/');

        if (slash == NULL) {
            // ── Leaf file ────────────────────────────────────────────────────
            // rel is just the filename, no subdirectory.
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = entries[i].mode;
            snprintf(e->name, sizeof(e->name), "%s", rel);
            e->hash = entries[i].hash;
            i++;
        } else {
            // ── Subdirectory ─────────────────────────────────────────────────
            // Extract the subdirectory name (the part before the first '/')
            size_t dir_name_len = (size_t)(slash - rel);
            char dir_name[512];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build the full prefix for the subdirectory
            char sub_prefix[1024];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);
            size_t sub_prefix_len = strlen(sub_prefix);

            // Collect all consecutive entries that share this subdirectory prefix
            int start = i;
            while (i < count && strncmp(entries[i].path, sub_prefix, sub_prefix_len) == 0) {
                i++;
            }

            // Recursively build the subtree for this subdirectory
            ObjectID sub_id;
            if (write_tree_level(entries + start, i - start, sub_prefix, &sub_id) != 0)
                return -1;

            // Add a directory entry pointing to the subtree
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = MODE_DIR;
            snprintf(e->name, sizeof(e->name), "%s", dir_name);
            e->hash = sub_id;
        }
    }

    // Serialize and write this tree level to the object store
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree objects.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;

    if (index.count == 0) {
        // Empty tree — still need to write a valid (empty) tree object
        Tree empty;
        empty.count = 0;
        void *data;
        size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }

    // The index entries must be sorted by path for the grouping logic to work.
    // index_save() sorts them, but let's sort a local copy to be safe.
    // (A simple insertion sort is fine for up to MAX_INDEX_ENTRIES.)
    IndexEntry *sorted = malloc((size_t)index.count * sizeof(IndexEntry));
    if (!sorted) return -1;
    memcpy(sorted, index.entries, (size_t)index.count * sizeof(IndexEntry));

    // qsort by path
    int cmp_path(const void *a, const void *b) {
        return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
    }
    qsort(sorted, (size_t)index.count, sizeof(IndexEntry), cmp_path);

    int rc = write_tree_level(sorted, index.count, "", id_out);
    free(sorted);
    return rc;
}

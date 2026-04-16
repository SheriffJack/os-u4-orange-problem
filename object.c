// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Write an object to the store.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Determine type string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // 2. Build full object: "<type> <size>\0<data>"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t full_len = (size_t)header_len + 1 + len;
    uint8_t *full = malloc(full_len);
    if (!full) return -1;
    memcpy(full, header, header_len);
    full[header_len] = '\0';
    memcpy(full + header_len + 1, data, len);

    // 3. Compute SHA-256 of the full object
    ObjectID id;
    compute_hash(full, full_len, &id);

    // 4. Deduplication — already stored?
    if (object_exists(&id)) {
        *id_out = id;
        free(full);
        return 0;
    }

    // 5. Build paths
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", shard_dir, hex + 2);

    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    // 6. Create shard directory if needed
    mkdir(shard_dir, 0755);

    // 7. Write to temp file
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0444);
    if (fd < 0) { free(full); return -1; }

    size_t written = 0;
    while (written < full_len) {
        ssize_t n = write(fd, (uint8_t *)full + written, full_len - written);
        if (n <= 0) { close(fd); free(full); return -1; }
        written += (size_t)n;
    }

    // 8. fsync temp file
    fsync(fd);
    close(fd);
    free(full);

    // 9. Atomic rename
    if (rename(tmp_path, final_path) != 0) return -1;

    // 10. fsync the shard directory
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    *id_out = id;
    return 0;
}

// Read an object from the store.
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Get the file path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { fclose(f); return -1; }

    uint8_t *raw = malloc((size_t)file_size);
    if (!raw) { fclose(f); return -1; }

    if (fread(raw, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f); free(raw); return -1;
    }
    fclose(f);

    // 3. Verify integrity — recompute hash
    ObjectID computed;
    compute_hash(raw, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(raw); return -1;
    }

    // 4. Parse header: find the '\0' separating header from data
    uint8_t *null_pos = memchr(raw, '\0', (size_t)file_size);
    if (!null_pos) { free(raw); return -1; }

    // 5. Parse type string from header
    if (strncmp((char *)raw, "blob ", 5) == 0)        *type_out = OBJ_BLOB;
    else if (strncmp((char *)raw, "tree ", 5) == 0)   *type_out = OBJ_TREE;
    else if (strncmp((char *)raw, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(raw); return -1; }

    // 6. Extract data length from header and copy data portion
    size_t header_len = (size_t)(null_pos - raw);
    size_t data_len   = (size_t)file_size - header_len - 1;

    char *space = memchr(raw, ' ', header_len);
    if (!space) { free(raw); return -1; }
    size_t declared_size = (size_t)strtoul(space + 1, NULL, 10);
    if (declared_size != data_len) { free(raw); return -1; }

    uint8_t *data_copy = malloc(data_len + 1);
    if (!data_copy) { free(raw); return -1; }
    memcpy(data_copy, null_pos + 1, data_len);
    data_copy[data_len] = '\0';

    free(raw);
    *data_out = data_copy;
    *len_out  = data_len;
    return 0;
}

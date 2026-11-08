// MSR-nx: fs:USER (the game's own RomFS and its save data in a host folder) and cfg:u.
//
// Layouts follow fearkov/zakuro (MIT) fs.rs and cfg.rs and 3dbrew "Filesystem services". The
// SelfNCCH RomFS file is romfs.bin from its IVFC level 3 (offset 0x1000) to the end.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "hle.h"
#include "msr_log.h"

#define ARCHIVE_SELF_NCCH 0x3u
#define ARCHIVE_SAVEDATA  0x4u
#define ERR_NOT_FOUND     0xC8804478u
#define ERR_FILE_NOT_FOUND 0xC8804470u
#define ERR_PATH_NOT_FOUND 0xC8804471u
#define ERR_NOT_FORMATTED 0xC8A04554u
#define ERR_FILE_EXISTS   0xC82044B4u
#define ERR_DIR_EXISTS    0xC82044B9u
#define ERR_DIR_NOT_EMPTY 0xC92044F0u
#define ERR_INVALID_PATH  0xE0E046BEu
#define ERR_NOT_MOUNTED   0xC8A0445Au
#define ERR_UNSUPPORTED   0xE0C046BEu
#define OPEN_WRITE  2u
#define OPEN_CREATE 4u

extern char msr_extract_dir[512];
char msr_save_dir[512];   // host folder that holds the save data archive

typedef struct {
    FILE* fp;
    u64 base, size;
    int writable;
} HostFile;

#define MAX_FILES 64
static HostFile g_files[MAX_FILES];

typedef struct {
    int used;
    char names[64][64];
    u8 is_dir[64];
    u64 sizes[64];
    int count, cursor;
} HostDir;

#define MAX_DIRS 8
static HostDir g_dirs[MAX_DIRS];

static int seek64(FILE* fp, u64 off) {
#ifdef _WIN32
    return _fseeki64(fp, (long long)off, SEEK_SET);
#else
    return fseeko(fp, (off_t)off, SEEK_SET);
#endif
}

static u64 size_of(FILE* fp) {
#ifdef _WIN32
    _fseeki64(fp, 0, SEEK_END);
    return (u64)_ftelli64(fp);
#else
    fseeko(fp, 0, SEEK_END);
    return (u64)ftello(fp);
#endif
}

static int add_file(FILE* fp, u64 base, int writable) {
    for (int k = 1; k < MAX_FILES; k++)
        if (!g_files[k].fp) {
            g_files[k].fp = fp;
            g_files[k].base = base;
            g_files[k].size = size_of(fp) - base;
            g_files[k].writable = writable;
            return k;
        }
    fclose(fp);
    return -1;
}

static u32 file_session(int f) {
    u32 h = k_new(OBJ_SESSION, "fs:file");
    k_get(h)->addr = (u32)f;
    return h;
}

// ---- save data: a host folder, "formatted" once it holds a .format file -----------------------

// Reads an fs path (empty, ASCII or UTF-16) into a relative host path. 0 if it is not allowed.
static int read_path(u32 type, u32 ptr, u32 size, char* out, int n) {
    int j = 0;
    out[0] = 0;
    if (type == 1) return 1;                       // empty: the archive root
    if (type != 3 && type != 4) return 0;
    for (u32 k = 0; k < size && j < n - 1; k += type == 4 ? 2 : 1) {
        u32 ch = type == 4 ? rd16(ptr + k) : rd8(ptr + k);
        if (!ch) break;
        if (ch > 0x7E) ch = '_';
        out[j++] = ch == '\\' ? '/' : (char)ch;
    }
    out[j] = 0;
    if (strstr(out, "..")) return 0;
    return 1;
}

static void host_path(const char* rel, char* out, int n) {
    snprintf(out, n, "%s/%s", msr_save_dir, rel[0] == '/' ? rel + 1 : rel);
    int len = (int)strlen(out);
    while (len > 0 && out[len - 1] == '/') out[--len] = 0;
}

static int is_formatted(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/.format", msr_save_dir);
    FILE* f = fopen(p, "rb");
    if (f) fclose(f);
    return f != 0;
}

static int path_kind(const char* host) {   // 0 missing, 1 file, 2 directory
    struct stat st;
    if (stat(host, &st)) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

static int parent_exists(const char* host) {
    char p[600];
    snprintf(p, sizeof p, "%s", host);
    char* slash = strrchr(p, '/');
    if (!slash) return 1;
    *slash = 0;
    return path_kind(p) == 2;
}

static int make_dir(const char* p) {
#ifdef _WIN32
    return _mkdir(p);
#else
    return mkdir(p, 0777);
#endif
}

static u32 save_format(void) {
    make_dir(msr_save_dir);
    char p[600];
    snprintf(p, sizeof p, "%s/.format", msr_save_dir);
    FILE* f = fopen(p, "wb");
    if (!f) return ERR_INVALID_PATH;
    fclose(f);
    msr_receipt("fs: save data formatted (%s)", msr_save_dir);
    return 0;
}

static u32 save_open_file(const char* rel, u32 flags, int* fidx) {
    char hp[600];
    host_path(rel, hp, sizeof hp);
    int kind = path_kind(hp);
    if (kind == 2) return 0xE0C04702u;
    if (kind == 0) {
        if (!(flags & OPEN_CREATE)) return ERR_FILE_NOT_FOUND;
        if (!parent_exists(hp)) return ERR_PATH_NOT_FOUND;
        FILE* f = fopen(hp, "wb");
        if (!f) return ERR_INVALID_PATH;
        fclose(f);
    }
    FILE* fp = fopen(hp, (flags & OPEN_WRITE) ? "r+b" : "rb");
    if (!fp) return ERR_FILE_NOT_FOUND;
    *fidx = add_file(fp, 0, (flags & OPEN_WRITE) != 0);
    return *fidx < 0 ? ERR_UNSUPPORTED : 0;
}

static u32 save_open_dir(const char* rel, int* didx) {
    char hp[600];
    host_path(rel, hp, sizeof hp);
    if (path_kind(hp) != 2) return ERR_PATH_NOT_FOUND;
    DIR* d = opendir(hp);
    if (!d) return ERR_PATH_NOT_FOUND;
    int k;
    for (k = 0; k < MAX_DIRS && g_dirs[k].used; k++) {}
    if (k == MAX_DIRS) { closedir(d); return ERR_UNSUPPORTED; }
    HostDir* hd = &g_dirs[k];
    memset(hd, 0, sizeof *hd);
    hd->used = 1;
    struct dirent* de;
    while ((de = readdir(d)) && hd->count < 64) {
        if (de->d_name[0] == '.') continue;   // ".", ".." and our .format marker
        char ep[900];
        snprintf(ep, sizeof ep, "%s/%s", hp, de->d_name);
        struct stat st;
        if (stat(ep, &st)) continue;
        snprintf(hd->names[hd->count], 64, "%s", de->d_name);
        hd->is_dir[hd->count] = S_ISDIR(st.st_mode) != 0;
        hd->sizes[hd->count] = S_ISDIR(st.st_mode) ? 0 : (u64)st.st_size;
        hd->count++;
    }
    closedir(d);
    *didx = k;
    return 0;
}

// One FSDir:Read entry (0x228 bytes): UTF-16 name, 8.3 name and extension, flags, size.
static void write_dir_entry(u32 dst, HostDir* hd, int i) {
    for (u32 k = 0; k < 0x228; k++) wr8(dst + k, 0);
    const char* name = hd->names[i];
    for (int k = 0; name[k] && k < 0x105; k++) wr16(dst + 2 * k, (u8)name[k]);
    const char* dot = hd->is_dir[i] ? 0 : strrchr(name, '.');
    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    for (int k = 0; k < stem && k < 8; k++) wr8(dst + 0x20C + k, (u8)toupper((u8)name[k]));
    if (dot) for (int k = 0; dot[1 + k] && k < 3; k++) wr8(dst + 0x216 + k, (u8)toupper((u8)dot[1 + k]));
    wr8(dst + 0x21A, 1);
    wr8(dst + 0x21C, hd->is_dir[i]);
    wr8(dst + 0x21E, !hd->is_dir[i]);
    wr32(dst + 0x220, (u32)hd->sizes[i]);
    wr32(dst + 0x224, (u32)(hd->sizes[i] >> 32));
}

static int open_romfs(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/romfs.bin", msr_extract_dir);
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        msr_receipt("fs: cannot open %s", path);
        return -1;
    }
    return add_file(fp, 0x1000, 0);
}

// Opens a file of an archive. Returns a file session handle or 0 with *err set.
static u32 open_file(u32 archive_id, u32 path_type, u32 path_ptr, u32 path_size, u32 flags, u32* err) {
    if (archive_id == ARCHIVE_SELF_NCCH && path_type == 2 && path_size >= 4 && rd32(path_ptr) == 0) {
        int f = open_romfs();
        if (f < 0) { *err = ERR_NOT_FOUND; return 0; }
        u32 h = file_session(f);
        msr_receipt("fs: opened the RomFS (%llu bytes) -> %X", (unsigned long long)g_files[f].size, h);
        return h;
    }
    char rel[256];
    if (archive_id == ARCHIVE_SAVEDATA) {
        if (!is_formatted()) { *err = ERR_NOT_FORMATTED; return 0; }
        if (!read_path(path_type, path_ptr, path_size, rel, sizeof rel)) { *err = ERR_INVALID_PATH; return 0; }
        int f = -1;
        *err = save_open_file(rel, flags, &f);
        msr_receipt("fs: save data open \"%s\" flags %X -> %08X", rel, flags, *err);
        return *err ? 0 : file_session(f);
    }
    read_path(path_type, path_ptr, path_size, rel, sizeof rel);
    msr_receipt("fs: open archive %X path type %u \"%s\" (not supported yet)", archive_id, path_type, rel);
    *err = ERR_NOT_FOUND;
    return 0;
}

static int fs_file(Ipc* q, KObject* o) {
    HostFile* f = &g_files[o->addr];
    switch (q->cmd) {
    case 0x0802: {  // Read(offset u64, size, buffer descriptor, pointer) -> bytes read
        u64 off = (u64)ipc_get(q, 1) | ((u64)ipc_get(q, 2) << 32);
        u32 size = ipc_get(q, 3), dst = ipc_get(q, 5);
        u32 got = 0;
        if (f->fp && off < f->size) {
            u64 left = f->size - off;
            u32 want = left < size ? (u32)left : size;
            seek64(f->fp, f->base + off);
            got = (u32)fread(g_mem + dst, 1, want, f->fp);
        }
        msr_log("fs: read %X bytes at %llX -> %08X (%X read)", size, (unsigned long long)off, dst, got);
        ipc_set(q, 0, IPC_HDR(0x0802, 2, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, got);
        ipc_set(q, 3, (size << 4) | 0xC);
        ipc_set(q, 4, dst);
        return 1;
    }
    case 0x0803: {  // Write(offset u64, size, flags, descriptor, pointer) -> bytes written
        u64 off = (u64)ipc_get(q, 1) | ((u64)ipc_get(q, 2) << 32);
        u32 size = ipc_get(q, 3), src = ipc_get(q, 6), put = 0;
        if (f->fp && f->writable) {
            seek64(f->fp, f->base + off);
            put = (u32)fwrite(g_mem + src, 1, size, f->fp);
            fflush(f->fp);
            if (off + put > f->size) f->size = off + put;
        }
        msr_log("fs: write %X bytes at %llX (%X written)", size, (unsigned long long)off, put);
        ipc_set(q, 0, IPC_HDR(0x0803, 2, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, put);
        ipc_set(q, 3, (size << 4) | 0xA);
        ipc_set(q, 4, src);
        return 1;
    }
    case 0x0804: {  // GetSize
        u32 w[2] = {(u32)f->size, (u32)(f->size >> 32)};
        ipc_reply(q, w, 2);
        return 1;
    }
    case 0x0805: {  // SetSize(size u64)
        u64 size = (u64)ipc_get(q, 1) | ((u64)ipc_get(q, 2) << 32);
        if (f->fp && f->writable && size > f->size) {   // growing: zero fill
            seek64(f->fp, f->base + size - 1);
            fputc(0, f->fp);
            fflush(f->fp);
        }
        f->size = size;   // shrinking is kept logically; the host file keeps its tail
        ipc_reply(q, 0, 0);
        return 1;
    }
    case 0x0806: {  // GetAttributes
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0808:  // Close
        if (f->fp) fclose(f->fp);
        f->fp = 0;   // the game closes the session handle itself
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0807: case 0x0809: case 0x080A:  // SetAttributes, Flush, SetPriority
        ipc_reply(q, 0, 0);
        return 1;
    case 0x080B: {  // GetPriority
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    }
    return 0;
}

static int fs_dir(Ipc* q, KObject* o) {
    HostDir* d = &g_dirs[o->addr];
    switch (q->cmd) {
    case 0x0801: {  // Read(count, descriptor, buffer) -> entries read
        u32 count = ipc_get(q, 1), desc = ipc_get(q, 2), dst = ipc_get(q, 3), n = 0;
        while (n < count && d->cursor < d->count) write_dir_entry(dst + 0x228 * n++, d, d->cursor++);
        ipc_set(q, 0, IPC_HDR(0x0801, 2, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, n);
        ipc_set(q, 3, desc);
        ipc_set(q, 4, dst);
        return 1;
    }
    case 0x0802:  // Close
        d->used = 0;
        ipc_reply(q, 0, 0);
        return 1;
    }
    return 0;
}

// Archive handles are u64: low word 0x10000 | archive id.
static u32 archive_of(Ipc* q, int word) { return ipc_get(q, word) & 0xFFFF; }

static u32 save_path_arg(Ipc* q, int type_w, int size_w, int ptr_w, char* rel, int n) {
    if (!is_formatted()) return ERR_NOT_FORMATTED;
    if (!read_path(ipc_get(q, type_w), ipc_get(q, ptr_w), ipc_get(q, size_w), rel, n)) return ERR_INVALID_PATH;
    return 0;
}

static void reply_result(Ipc* q, u32 r) {
    if (r) ipc_error(q, r); else ipc_reply(q, 0, 0);
}

int svc_fs(Ipc* q) {
    KObject* o = k_get(q->handle);
    if (o && !strcmp(o->name, "fs:file")) return fs_file(q, o);
    if (o && !strcmp(o->name, "fs:dir")) return fs_dir(q, o);
    char rel[256], hp[600];
    switch (q->cmd) {
    case 0x0801: case 0x0861: case 0x0862:  // Initialize, InitializeWithSdkVersion, SetPriority
    case 0x0840: case 0x085A: case 0x085D: case 0x086E:
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0863: case 0x0813: {  // GetPriority, GetCardType
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0817: case 0x0818: case 0x0821: case 0x083D: {  // SD detected/writable, card inserted, ext data access
        u32 w[1] = {1};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0812: {  // GetFreeBytes
        u32 w[2] = {0x20000000u, 0};
        ipc_reply(q, w, 2);
        return 1;
    }
    case 0x0814: case 0x0815: case 0x0849: {  // archive resource: sector, cluster, total, free
        u32 w[4] = {0x200, 0x4000, 0x80000, 0x80000};
        ipc_reply(q, w, 4);
        return 1;
    }
    case 0x086F: {  // GetThisSaveDataSecureValue: none
        u32 w[4] = {0, 0, 0, 0};
        ipc_reply(q, w, 4);
        return 1;
    }
    case 0x0803: {  // OpenFileDirectly(transaction, archive id, archive path type/size, file path
                    // type/size, flags, attributes, archive path, file path)
        u32 err = 0;
        u32 h = open_file(ipc_get(q, 2), ipc_get(q, 5), ipc_get(q, 12), ipc_get(q, 6), ipc_get(q, 7), &err);
        if (!h) { ipc_error(q, err); return 1; }
        ipc_set(q, 0, IPC_HDR(0x0803, 1, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x10);
        ipc_set(q, 3, h);
        return 1;
    }
    case 0x080C: {  // OpenArchive(archive id, path type, path size, path) -> archive handle (u64)
        u32 id = ipc_get(q, 1);
        if (id == ARCHIVE_SAVEDATA && !is_formatted()) {
            msr_receipt("fs: OpenArchive save data: not formatted yet");
            ipc_error(q, ERR_NOT_FORMATTED);
            return 1;
        }
        msr_receipt("fs: OpenArchive %X", id);
        u32 w[2] = {0x10000u | id, 0};
        ipc_reply(q, w, 2);
        return 1;
    }
    case 0x0802: {  // OpenFile(transaction, archive handle u64, path type/size, flags, attributes, path)
        u32 err = 0;
        u32 h = open_file(archive_of(q, 2), ipc_get(q, 4), ipc_get(q, 9), ipc_get(q, 5), ipc_get(q, 6), &err);
        if (!h) { ipc_error(q, err); return 1; }
        ipc_set(q, 0, IPC_HDR(0x0802, 1, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x10);
        ipc_set(q, 3, h);
        return 1;
    }
    case 0x0804: case 0x0806: case 0x0807: {  // DeleteFile / DeleteDirectory(Recursively)
        u32 r = archive_of(q, 2) != ARCHIVE_SAVEDATA ? ERR_UNSUPPORTED : save_path_arg(q, 4, 5, 7, rel, sizeof rel);
        if (!r) {
            host_path(rel, hp, sizeof hp);
            int kind = path_kind(hp);
            if (q->cmd == 0x0804) r = kind == 1 ? (remove(hp) ? ERR_INVALID_PATH : 0) : ERR_FILE_NOT_FOUND;
            else r = kind == 2 ? (rmdir(hp) ? ERR_DIR_NOT_EMPTY : 0) : ERR_PATH_NOT_FOUND;
        }
        msr_receipt("fs: save data delete \"%s\" -> %08X", rel, r);
        reply_result(q, r);
        return 1;
    }
    case 0x0805: case 0x080A: {  // RenameFile / RenameDirectory
        char rel2[256], hp2[600];
        u32 r = archive_of(q, 2) != ARCHIVE_SAVEDATA ? ERR_UNSUPPORTED : save_path_arg(q, 4, 5, 11, rel, sizeof rel);
        if (!r) r = save_path_arg(q, 8, 9, 13, rel2, sizeof rel2);
        if (!r) {
            host_path(rel, hp, sizeof hp);
            host_path(rel2, hp2, sizeof hp2);
            if (path_kind(hp2)) r = q->cmd == 0x0805 ? ERR_FILE_EXISTS : ERR_DIR_EXISTS;
            else if (rename(hp, hp2)) r = ERR_NOT_FOUND;
        }
        msr_receipt("fs: save data rename \"%s\" -> \"%s\" -> %08X", rel, rel2, r);
        reply_result(q, r);
        return 1;
    }
    case 0x0808: {  // CreateFile(transaction, archive u64, path type, path size, attributes, size u64, path)
        u32 r = archive_of(q, 2) != ARCHIVE_SAVEDATA ? ERR_UNSUPPORTED : save_path_arg(q, 4, 5, 10, rel, sizeof rel);
        u64 size = (u64)ipc_get(q, 7) | ((u64)ipc_get(q, 8) << 32);
        if (!r) {
            host_path(rel, hp, sizeof hp);
            if (path_kind(hp)) r = ERR_FILE_EXISTS;
            else if (!parent_exists(hp)) r = ERR_PATH_NOT_FOUND;
            else {
                FILE* f = fopen(hp, "wb");
                if (!f) r = ERR_INVALID_PATH;
                else {
                    if (size) { seek64(f, size - 1); fputc(0, f); }
                    fclose(f);
                }
            }
        }
        msr_receipt("fs: save data create file \"%s\" (%llX bytes) -> %08X", rel, (unsigned long long)size, r);
        reply_result(q, r);
        return 1;
    }
    case 0x0809: {  // CreateDirectory(transaction, archive u64, path type, path size, attributes, path)
        u32 r = archive_of(q, 2) != ARCHIVE_SAVEDATA ? ERR_UNSUPPORTED : save_path_arg(q, 4, 5, 8, rel, sizeof rel);
        if (!r) {
            host_path(rel, hp, sizeof hp);
            if (path_kind(hp)) r = ERR_DIR_EXISTS;
            else if (!parent_exists(hp)) r = ERR_PATH_NOT_FOUND;
            else if (make_dir(hp)) r = ERR_INVALID_PATH;
        }
        msr_receipt("fs: save data create directory \"%s\" -> %08X", rel, r);
        reply_result(q, r);
        return 1;
    }
    case 0x080B: {  // OpenDirectory(archive u64, path type, path size, path)
        u32 r = archive_of(q, 1) != ARCHIVE_SAVEDATA ? ERR_UNSUPPORTED : save_path_arg(q, 3, 4, 6, rel, sizeof rel);
        int d = -1;
        if (!r) r = save_open_dir(rel, &d);
        msr_receipt("fs: save data open directory \"%s\" -> %08X", rel, r);
        if (r) { ipc_error(q, r); return 1; }
        u32 h = k_new(OBJ_SESSION, "fs:dir");
        k_get(h)->addr = (u32)d;
        ipc_set(q, 0, IPC_HDR(0x080B, 1, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x10);
        ipc_set(q, 3, h);
        return 1;
    }
    case 0x080D: {  // ControlArchive(archive u64, action, input size, output size, descriptors): commit
        u32 d[4];
        for (int k = 0; k < 4; k++) d[k] = ipc_get(q, 6 + k);
        ipc_set(q, 0, IPC_HDR(0x080D, 1, 4));
        ipc_set(q, 1, 0);
        for (int k = 0; k < 4; k++) ipc_set(q, 2 + k, d[k]);
        return 1;
    }
    case 0x080E:  // CloseArchive
        ipc_reply(q, 0, 0);
        return 1;
    case 0x080F:  // FormatThisUserSaveData
        reply_result(q, save_format());
        return 1;
    case 0x084C:  // FormatSaveData(archive id, ...)
        reply_result(q, ipc_get(q, 1) == ARCHIVE_SAVEDATA ? save_format() : ERR_INVALID_PATH);
        return 1;
    case 0x0845:  // GetFormatInfo(archive id, path type, path size, path)
        if (ipc_get(q, 1) == ARCHIVE_SAVEDATA && is_formatted()) {
            u32 w[4] = {0x80000, 16, 16, 0};
            ipc_reply(q, w, 4);
        } else ipc_error(q, ERR_NOT_FORMATTED);
        return 1;
    }
    return 0;
}

int svc_cfg(Ipc* q) {
    switch (q->cmd) {
    case 0x0001: {  // GetConfigInfoBlk2(size, block id, descriptor, pointer)
        u32 size = ipc_get(q, 1), id = ipc_get(q, 2), out = ipc_get(q, 4);
        for (u32 k = 0; k < size; k++) wr8(out + k, 0);
        if (id == 0x000A0002 && size) wr8(out, 1);                   // language: English
        if ((id == 0x000B0000 || id == 0x000B0001) && size >= 4) wr8(out + 3, 49);  // USA
        if (id == 0x000A0001 && size >= 2) { wr8(out, 1); wr8(out + 1, 1); }       // birthday
        if (id == 0x000D0000 && size >= 2) { wr8(out, 0x7F); wr8(out + 1, 0x7F); } // EULA version accepted
        if (id == 0x00070001 && size) wr8(out, 1);                                  // sound output: stereo
        if (id == 0x00050005) {   // stereo camera settings: the values a retail console ships with
            static const float k[8] = {62.0f, 289.0f, 76.80000305f, 46.08000183f, 10.0f, 5.0f, 55.58000183f, 21.56999969f};
            for (u32 k2 = 0; k2 < 8 && 4 * k2 + 4 <= size; k2++) { u32 v; memcpy(&v, &k[k2], 4); wr32(out + 4 * k2, v); }
        }
        if (id == 0x000A0000) {   // user name, UTF-16
            const char* n = "Samus";
            for (u32 k2 = 0; n[k2] && 2 * k2 + 2 <= size; k2++) wr16(out + 2 * k2, (u8)n[k2]);
        }
        msr_log("cfg: block %08X (%u bytes)", id, size);
        ipc_reply(q, 0, 0);
        return 1;
    }
    case 0x0002: {  // SecureInfoGetRegion: USA
        u32 w[1] = {1};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0003: {  // GenHashConsoleUnique
        u32 w[2] = {0x335A4155u, 0x00005255u};
        ipc_reply(q, w, 2);
        return 1;
    }
    case 0x0004: case 0x0006: {  // GetRegionCanadaUSA: yes; GetModelNintendo2DS: not a 2DS
        u32 w[1] = {1};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0005: {  // GetSystemModel: Old 3DS
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    }
    return 0;
}

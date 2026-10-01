/* The CD's InstallShield cabinets as the game folder (iscab.c): every file the cabinet layer exposes is
   byte-compared with the installed game (the unshield extraction, the ground truth), and every
   installed file must be exposed.

     OPENBALLANCE_DATA=cd|Ballance.iso ./build/cab_test [installed folder, default ./data]

   Skips unless OPENBALLANCE_DATA mounts cabinets. */
#include "iscab.h"
#include "vfs_host.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *ref;
static int files, failures;
static uint64_t bytes;
static double read_s;   /* in vfs_read_all */

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint8_t *read_host(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

typedef struct { char names[512][256]; bool dir[512]; int n; } Listing;

static void collect(const char *name, bool is_dir, void *user)
{
    Listing *l = user;
    if (l->n < 512) snprintf(l->names[l->n], 256, "%s", name), l->dir[l->n++] = is_dir;
}

/* Every exposed file under dir equals <ref>/<path>. */
static void walk_vfs(const char *dir)
{
    Listing *l = calloc(1, sizeof *l);
    if (!vfs_list(dir, collect, l)) { printf("FAIL: can't list '%s'\n", dir); failures++; }
    for (int i = 0; i < l->n; i++) {
        char rel[1024], host[2048];
        snprintf(rel, sizeof rel, "%s%s%s", dir, *dir ? "/" : "", l->names[i]);
        if (l->dir[i]) { walk_vfs(rel); continue; }
        snprintf(host, sizeof host, "%s/%s", ref, rel);
        size_t n = 0, m = 0;
        double t = now();
        uint8_t *a = vfs_read_all(rel, &n);
        read_s += now() - t;
        uint8_t *b = read_host(host, &m);
        files++;
        bytes += n;
        if (!a) printf("FAIL: %s: can't be read from the cabinets\n", rel), failures++;
        else if (!b) printf("FAIL: %s: not in %s\n", rel, ref), failures++;
        else if (n != m || memcmp(a, b, n)) printf("FAIL: %s: differs (%zu vs %zu bytes)\n", rel, n, m), failures++;
        free(a);
        free(b);
    }
    free(l);
}

/* Every installed file under <ref>/dir is exposed. */
static void walk_ref(const char *dir)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", ref, dir);
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char rel[1024], full[3072];
        snprintf(rel, sizeof rel, "%s%s%s", dir, *dir ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) walk_ref(rel);
        else if (!vfs_exists(rel)) printf("FAIL: %s: installed but not in the cabinets\n", rel), failures++;
    }
    if (d) closedir(d);
}

int main(int argc, char **argv)
{
    ref = argc > 1 ? argv[1] : "data";
    double t0 = now();
    if (!vfs_mount_default() || !strstr(vfs_describe(), "InstallShield")) {
        printf("SKIP: OPENBALLANCE_DATA is not the Ballance CD (folder or image)\n");
        return 0;
    }
    double t1 = now();
    printf("mounted %s in %.1f ms\n", vfs_describe(), (t1 - t0) * 1e3);
    walk_vfs("");
    double t2 = now();
    walk_ref("");
    IsCabStats st = {0};
    vfs_cab_stats(&st);
    printf("%d files, %.1f MB expanded in %.2f s (%.1f MB/s; %u extracts, %u cache hits; %.2f s with the comparison)\n",
           files, bytes / 1e6, read_s, bytes / 1e6 / read_s, st.extracts, st.cache_hits, t2 - t1);
    if (failures) printf("%d failure(s)\n", failures);
    else printf("PASS\n");
    return failures != 0;
}

#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// ------------------------ Config & Globals ------------------------

static const char *DEFAULT_EXTS =
    ".c,.h,.cpp,.hpp,.cc,.hh,.cxx,.hxx,"
    ".rs,.go,.py,.js,.ts,.tsx,.jsx,.java,.kt,.swift,.m,.mm,.cs,"
    ".php,.rb,.sh,.bash,.zsh,.fish,.ps1,.psm1,.r,.jl,.sql,"
    ".yaml,.yml,.toml,.ini,.cfg,.conf,.md,.txt,.cmake,.make,.mk,"
    ".gradle,.sbt,.pl,.pm,.scala,.dart,.lua,.zig,.hs,.erl,.ex,.exs,.elm,.hx";

static const char *DEFAULT_EXCLUDES =
    ".git,node_modules,.cache,.idea,.vscode,target,build,dist,.venv,venv";

typedef struct {
    char root[PATH_MAX];
    char out_path[PATH_MAX];
    bool out_specified;
    bool follow_symlinks;
    bool include_hidden;
    char *exts;           // comma-separated, leading dots required
    char *exclude_dirs;   // comma-separated names
} Options;

static FILE *g_out = NULL;
static Options g_opts;
static uint64_t g_bytes_written = 0;
static uint64_t g_files_written = 0;

// ------------------------ Utility ------------------------

static void usage(const char *prog) {
    fprintf(stderr,
        "codecat — concatenate code files with headers\n\n"
        "Usage:\n"
        "  %s [options] [root]\n\n"
        "If [root] is provided as a positional argument (e.g. `codecat src`), it\n"
        "sets the root directory to scan. If -o/--out is not given, output defaults\n"
        "to a timestamped file: codecat_YYYYMMDDHHMMSS.txt\n\n"
        "Options:\n"
        "  -r, --root <path>           Root directory to scan (default: current dir)\n"
        "  -o, --out  <file>           Output text file (overrides timestamped default)\n"
        "      --exts <list>           Comma-separated extensions to include (default: sensible set)\n"
        "      --exclude-dirs <list>   Comma-separated directory names to skip (default: common junk)\n"
        "      --follow-links          Follow symlinks (default: off)\n"
        "      --include-hidden        Include hidden files/dirs starting with '.' (default: off)\n"
        "  -h, --help                  Show this help\n\n"
        "Examples:\n"
        "  %s src                      # scan ./src, write to codecat_YYYYMMDDHHMMSS.txt\n"
        "  %s -r . -o dump.txt         # explicit root and output file\n"
        "  %s --exts .c,.h --exclude-dirs .git,node_modules\n",
        prog, prog, prog, prog
    );
}

static bool str_list_contains(const char *csv, const char *needle, bool case_insensitive) {
    if (!csv || !needle) return false;
    size_t nlen = strlen(needle);

    const char *p = csv;
    while (*p) {
        while (*p == ',') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - start);
        if (len == 0) continue;

        if (nlen == len) {
            int cmp = 0;
            if (case_insensitive) {
                for (size_t i = 0; i < len; i++) {
                    char a = start[i];
                    char b = needle[i];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                    if (a != b) { cmp = (a < b) ? -1 : 1; break; }
                }
            } else {
                cmp = strncmp(start, needle, len);
            }
            if (cmp == 0) return true;
        }
    }
    return false;
}

static const char *path_basename(const char *path) {
    const char *b = strrchr(path, '/');
#ifdef _WIN32
    const char *b2 = strrchr(path, '\\');
    if (!b || (b2 && b2 > b)) b = b2;
#endif
    return b ? b + 1 : path;
}

static const char *path_extension(const char *path) {
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot : "";
}

static bool is_hidden(const char *path) {
    const char *base = path_basename(path);
    return base[0] == '.';
}

static bool should_skip_dir(const char *path) {
    const char *base = path_basename(path);
    if (!g_opts.include_hidden && base[0] == '.') return true;
    return str_list_contains(g_opts.exclude_dirs, base, /*case_insensitive*/false);
}

static bool has_allowed_ext(const char *path) {
    const char *ext = path_extension(path);
    if (ext[0] == '\0') return false;
    char buf[64];
    size_t elen = strlen(ext);
    if (elen >= sizeof(buf)) return false;
    for (size_t i = 0; i < elen; i++) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[elen] = '\0';
    return str_list_contains(g_opts.exts, buf, /*ci*/true);
}

static void write_header(const char *path) {
    const char *prefix = "==================== BEGIN FILE: ";
    const char *suffix = " ====================\n";
    fwrite(prefix, 1, strlen(prefix), g_out); g_bytes_written += strlen(prefix);
    fwrite(path, 1, strlen(path), g_out);     g_bytes_written += strlen(path);
    fwrite(suffix, 1, strlen(suffix), g_out); g_bytes_written += strlen(suffix);
}

static void write_footer(void) {
    const char *line = "\n===================== END FILE =====================\n\n";
    fwrite(line, 1, strlen(line), g_out); g_bytes_written += strlen(line);
}

static void copy_stream(FILE *in) {
    char buf[1 << 15]; // 32KB
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t w = fwrite(buf, 1, n, g_out);
        g_bytes_written += w;
        if (w != n) {
            fprintf(stderr, "Write error: %s\n", strerror(errno));
            exit(1);
        }
    }
}

static int visitor(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;

    if (typeflag == FTW_D || typeflag == FTW_DNR) {
        if (should_skip_dir(fpath)) return FTW_SKIP_SUBTREE;
        return 0;
    }

    if (typeflag != FTW_F) return 0; // only regular files

    if (!g_opts.include_hidden && is_hidden(fpath)) return 0;
    if (!has_allowed_ext(fpath)) return 0;

    FILE *in = fopen(fpath, "rb");
    if (!in) {
        fprintf(stderr, "Warn: cannot open %s: %s\n", fpath, strerror(errno));
        return 0; // continue
    }

    write_header(fpath);
    copy_stream(in);
    fclose(in);
    write_footer();

    g_files_written++;
    return 0;
}

static char *dup_cstr(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(p, s, n + 1);
    return p;
}

static void make_timestamped_filename(char *buf, size_t buflen) {
    // codecat_YYYYMMDDHHMMSS.txt in current dir
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    if (strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", &tmv) == 0) {
        // fallback if something weird happens
        snprintf(ts, sizeof(ts), "unknown");
    }
    snprintf(buf, buflen, "codecat_%s.txt", ts);
    buf[buflen - 1] = '\0';
}

static void parse_args(int argc, char **argv, Options *o) {
    // defaults
    if (!getcwd(o->root, sizeof(o->root))) {
        perror("getcwd");
        exit(1);
    }
    o->out_path[0] = '\0';
    o->out_specified = false;
    o->follow_symlinks = false;
    o->include_hidden  = false;
    o->exts = dup_cstr(DEFAULT_EXTS);
    o->exclude_dirs = dup_cstr(DEFAULT_EXCLUDES);

    // parse options + optional positional root
    const char *positional_root = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-') {
            if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
                usage(argv[0]);
                exit(0);
            } else if (!strcmp(a, "-r") || !strcmp(a, "--root")) {
                if (i + 1 >= argc) { usage(argv[0]); exit(1); }
                strncpy(o->root, argv[++i], sizeof(o->root)-1);
                o->root[sizeof(o->root)-1] = '\0';
            } else if (!strcmp(a, "-o") || !strcmp(a, "--out")) {
                if (i + 1 >= argc) { usage(argv[0]); exit(1); }
                strncpy(o->out_path, argv[++i], sizeof(o->out_path)-1);
                o->out_path[sizeof(o->out_path)-1] = '\0';
                o->out_specified = true;
            } else if (!strcmp(a, "--exts")) {
                if (i + 1 >= argc) { usage(argv[0]); exit(1); }
                free(o->exts);
                o->exts = dup_cstr(argv[++i]);
            } else if (!strcmp(a, "--exclude-dirs")) {
                if (i + 1 >= argc) { usage(argv[0]); exit(1); }
                free(o->exclude_dirs);
                o->exclude_dirs = dup_cstr(argv[++i]);
            } else if (!strcmp(a, "--follow-links")) {
                o->follow_symlinks = true;
            } else if (!strcmp(a, "--include-hidden")) {
                o->include_hidden = true;
            } else {
                fprintf(stderr, "Unknown option: %s\n", a);
                usage(argv[0]);
                exit(1);
            }
        } else {
            // first non-option is positional root
            if (!positional_root) positional_root = a;
            else {
                fprintf(stderr, "Unexpected extra argument: %s\n", a);
                usage(argv[0]);
                exit(1);
            }
        }
    }

    if (positional_root) {
        strncpy(o->root, positional_root, sizeof(o->root)-1);
        o->root[sizeof(o->root)-1] = '\0';
    }

    // If no explicit -o/--out given, generate timestamped default
    if (!o->out_specified) {
        make_timestamped_filename(o->out_path, sizeof(o->out_path));
    }
}

int main(int argc, char **argv) {
    parse_args(argc, argv, &g_opts);

    // Open output file
    g_out = fopen(g_opts.out_path, "wb");
    if (!g_out) {
        fprintf(stderr, "Error: cannot open output %s: %s\n", g_opts.out_path, strerror(errno));
        return 1;
    }

    int flags = g_opts.follow_symlinks ? FTW_CHDIR : FTW_PHYS; // FTW_PHYS = don't follow symlinks
    int res = nftw(g_opts.root, visitor, /*fdlimit*/ 64, flags);
    if (res == -1) {
        fprintf(stderr, "nftw error: %s\n", strerror(errno));
        fclose(g_out);
        return 1;
    }

    if (fflush(g_out) != 0) {
        fprintf(stderr, "fflush error: %s\n", strerror(errno));
    }
    if (fclose(g_out) != 0) {
        fprintf(stderr, "fclose error: %s\n", strerror(errno));
    }

    struct stat st;
    if (stat(g_opts.out_path, &st) == 0) {
        printf("Done.\n");
        printf(" Files written : %llu\n", (unsigned long long)g_files_written);
        printf(" Output path   : %s\n", g_opts.out_path);
        printf(" Output size   : %llu bytes\n", (unsigned long long)st.st_size);
    } else {
        printf("Done (stat failed: %s).\n", strerror(errno));
        printf(" Files written : %llu\n", (unsigned long long)g_files_written);
        printf(" Output path   : %s\n", g_opts.out_path);
        printf(" Bytes tracked : %llu bytes\n", (unsigned long long)g_bytes_written);
    }

    free(g_opts.exts);
    free(g_opts.exclude_dirs);
    return 0;
}

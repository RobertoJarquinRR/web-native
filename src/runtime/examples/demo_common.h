/*
 * demo_common.h - tiny helpers shared by the C ABI demos.
 * Not part of the engine or the public ABI.
 */
#ifndef WN_DEMO_COMMON_H
#define WN_DEMO_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double demo_now_ms(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER tick;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static char *demo_read_file(const char *path, long *out_length)
{
    FILE *file;
    long length;
    char *data;
    size_t got;
    if (out_length != NULL) {
        *out_length = 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || length > 8 * 1024 * 1024) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    data = (char *)malloc((size_t)length + 1);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    got = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(data);
        return NULL;
    }
    data[length] = '\0';
    if (out_length != NULL) {
        *out_length = length;
    }
    return data;
}

static int demo_file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

/* Locate page/index.html next to the executable or in the usual build-dir
 * relative locations. Returns 1 on success. */
static int demo_resolve_page(char *html_path, size_t html_cap,
                             char *css_path, size_t css_cap)
{
    char exe[MAX_PATH];
    char dir[MAX_PATH];
    char *slash;
    char candidates[6][MAX_PATH];
    int i;

    html_path[0] = '\0';
    css_path[0] = '\0';
    if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe)) == 0) {
        exe[0] = '\0';
    }
    snprintf(dir, sizeof(dir), "%s", exe);
    slash = strrchr(dir, '\\');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        dir[0] = '\0';
    }

    snprintf(candidates[0], sizeof(candidates[0]), "%s\\..\\page\\index.html", dir);
    snprintf(candidates[1], sizeof(candidates[1]), "%s\\page\\index.html", dir);
    snprintf(candidates[2], sizeof(candidates[2]), "page\\index.html");
    snprintf(candidates[3], sizeof(candidates[3]), "..\\page\\index.html");
    snprintf(candidates[4], sizeof(candidates[4]), "..\\..\\src\\runtime\\c-r2\\page\\index.html");
    candidates[5][0] = '\0';

    for (i = 0; candidates[i][0] != '\0'; ++i) {
        if (demo_file_exists(candidates[i])) {
            snprintf(html_path, html_cap, "%s", candidates[i]);
            {
                char css_candidate[MAX_PATH];
                snprintf(css_candidate, sizeof(css_candidate), "%s", candidates[i]);
                slash = strrchr(css_candidate, '\\');
                if (slash != NULL) {
                    snprintf(slash + 1, sizeof(css_candidate) - (size_t)(slash + 1 - css_candidate),
                             "style.css");
                }
                snprintf(css_path, css_cap, "%s", css_candidate);
            }
            return 1;
        }
    }
    return 0;
}

static void demo_base_dir(const char *path, char *out, size_t cap)
{
    const char *slash;
    size_t length;
    snprintf(out, cap, "%s", path != NULL ? path : "");
    slash = strrchr(out, '\\');
    if (slash == NULL) {
        slash = strrchr(out, '/');
    }
    if (slash != NULL) {
        length = (size_t)(slash - out);
        out[length] = '\0';
    } else {
        out[0] = '\0';
    }
}

#define DEMO_INLINE_HTML \
    "<!doctype html><html><head><meta charset=\"utf-8\">" \
    "<title>webnative R2 core fixture</title><link rel=\"stylesheet\" href=\"style.css\">" \
    "</head><body class=\"root\"><div class=\"sidebar\"><div class=\"brand\">webnative</div>" \
    "<div class=\"nav\" data-hover=\"#2a3140\" data-press=\"#3a4a5c\" " \
    "data-selected=\"#3498db\" data-action=\"select\">Bloque</div>" \
    "<div class=\"nav\" data-action=\"select\">Flex</div>" \
    "<div class=\"nav\" data-action=\"select\">Texto</div>" \
    "<div class=\"nav\" data-action=\"select\">IR</div></div>" \
    "<div class=\"content\"><div class=\"header\"><div class=\"title\">Runtime R2</div>" \
    "<div class=\"pill\"></div></div><div class=\"cards\">" \
    "<div class=\"card\"><div class=\"accent accent-red\"></div></div>" \
    "<div class=\"card\"><div class=\"accent accent-green\"></div></div>" \
    "<div class=\"card\"><div class=\"accent accent-blue\"></div></div></div>" \
    "<div class=\"panel\"><div class=\"line line-1\"></div>" \
    "<div class=\"line line-2\"></div><div class=\"line line-3\"></div>" \
    "</div></div></body></html>"

#define DEMO_INLINE_CSS \
    "html{height:100%}body{margin:0}.root{width:100%;height:100%;display:flex;" \
    "flex-direction:row;background:#0b0e14}.sidebar{width:30%;display:flex;" \
    "flex-direction:column;gap:12px;padding:16px;background:#121824}" \
    ".brand{font-size:16px;color:#fff}.nav{width:120px;height:32px;color:#fff}" \
    ".content{width:70%;display:flex;flex-direction:column;gap:16px;padding:24px;" \
    "background:#0b0e14}.header{display:flex;flex-direction:row;justify-content:space-between;" \
    "align-items:center;height:40px}.title{font-size:24px;color:#fff}.pill{width:80px;" \
    "height:12px;background:#2a3140}.cards{display:flex;flex-direction:row;flex-wrap:wrap;" \
    "gap:16px;height:auto}.card{width:180px;height:120px;padding:12px;background:#1a1f2a}" \
    ".accent{width:96px;height:6px}.accent-red{background:#e74c3c}" \
    ".accent-green{background:#2ecc71}.accent-blue{background:#3498db}" \
    ".panel{display:block;height:100px;padding:16px;background:#10141b}" \
    ".line{height:6px;background:#2a3140}.line-1{width:140px}.line-2{width:100px}" \
    ".line-3{width:60px}"

#endif /* WN_DEMO_COMMON_H */

/*
 * marcepan v0.9 - Interactive Mandelbrot/Julia ASCII Viewer
 * ==========================================================
 * 
 * A multi-threaded fractal visualizer for Linux terminals.
 * Renders Mandelbrot and Julia sets as ASCII art with ANSI 256-color support.
 * Optional half-block mode doubles vertical resolution using Unicode ▀▄ chars.
 * 
 * ARCHITECTURE
 * ------------
 * The program separates calculation from presentation:
 * 
 *   1. CALCULATION: Worker threads compute raw iteration counts into a buffer.
 *      This is the expensive part - complex number math for each pixel.
 * 
 *   2. PRESENTATION: Map iteration values to ASCII chars and colors.
 *      This is cheap - just array lookups. Allows instant palette switching!
 * 
 *   3. EXPORT: Save current view to file using the same mapping logic.
 * 
 * COMPILATION
 * -----------
 *   gcc -O3 -std=c11 -pthread -o marcepan marcepan.c -lm
 * 
 * KEYBOARD CONTROLS (NumLock OFF for numpad)
 * ------------------------------------------
 *   Navigation:     Numpad 8/2/4/6 = pan, 7/9/1/3 = diagonal pan
 *   Zoom:           Numpad 0 (Ins) = in, Enter = out
 *   Axis zoom:      Shift + Arrow keys
 *   Iterations:     +/- keys
 *   ASCII palette:  / and * keys
 *   Color palette:  1 and 2 keys
 *   Toggles:        c = color, m = modulo/linear, j = Julia/Mandelbrot
 *                   h = half-block mode (2x vertical resolution)
 *   Save:           p = plain .txt, P = colored .ansi
 *   Other:          ESC = reset, q = quit
 * 
 * The header shows a copy-pasteable command to recreate the current view.
 * 
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/select.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <math.h>

/* ========================================================================== */
/*                              CONSTANTS                                     */
/* ========================================================================== */

#define MAX_THREADS      256
#define MAX_TERM_WIDTH   1000
#define MAX_TERM_HEIGHT  2000     /* Doubled for half-block mode */
#define MIN_TERM_SIZE    4
#define MAX_ITERATIONS   10000
#define MAX_CUSTOM_PAL   256
#define OUTBUF_PER_CELL  32       /* Increased for half-block ANSI codes */
#define MAX_STATUS_LEN   128

/* Virtual key codes for special keys */
enum {
    KEY_NONE = 0,
    KEY_UP = 128, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_SHIFT_UP, KEY_SHIFT_DOWN, KEY_SHIFT_LEFT, KEY_SHIFT_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INS, KEY_DEL,
    KEY_ESC, KEY_ENTER, KEY_SLASH, KEY_STAR, KEY_PLUS, KEY_MINUS
};

/* ========================================================================== */
/*                            GLOBAL STATE                                    */
/* ========================================================================== */

/* Terminal */
static struct termios orig_tio;
static int term_w = 80, term_h = 24;
static int batch_mode = 0;

/* Viewport in complex plane */
static double view_xmin = -2.0, view_xmax = 1.0;
static double view_ymin = -1.0, view_ymax = 1.0;
static int max_iter = 30;

/* Julia mode: when enabled, julia_cr/ci define the constant c */
static int julia_mode = 0;
static double julia_cr = -0.7, julia_ci = 0.27015;  /* Classic Julia point */

/* Navigation */
static const double PAN_FRACTION  = 0.1;
static const double ZOOM_FRACTION = 0.3;

/* Display options */
static int use_color = 1;
static int use_modulo = 1;
static int use_halfblock = 0;    /* Half-block rendering for 2x vertical res */
static const char FILL_CHAR = ' ';

/* Status message (shown instead of command line until next redraw) */
static char status_message[MAX_STATUS_LEN] = {0};

/* Custom palette */
static char custom_palette[MAX_CUSTOM_PAL + 1];
static int has_custom_palette = 0;

/* Threading */
static int num_threads = 0;

/* ========================================================================== */
/*                           ASCII PALETTES                                   */
/* ========================================================================== */

static const char *builtin_palettes[] = {
    " #",
    ".,:;!?%$#@",
    " .,:;i1tfLCG08@",
    " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$",
    " .:-=+*#%@",
    "@%#*+=-:. ",
    " .:-=+*#",
    " .oO@*",
    " .:+*#%@",
    " ~-=oO0@",
    " .'\"*+oO#",
    " .<>^v*#@",
    " .-~=o*O@#",
    " ._-~:;!*",
    " .,;:!|I#",
    " ░▒▓█",
};
#define BUILTIN_PALETTE_COUNT (sizeof(builtin_palettes) / sizeof(builtin_palettes[0]))

static const char *palettes[BUILTIN_PALETTE_COUNT + 1];
static int palette_count;
static int current_palette = 1;

/* ========================================================================== */
/*                           COLOR PALETTES                                   */
/* ========================================================================== */

static const uint8_t color_schemes[][16] = {
    {0x11,0x12,0x13,0x14,0x15,0x1B,0x21,0x27,0x2D,0x33,0x32,0x31,0x30,0x2F,0x2E,0x2D},
    {0x10,0x34,0x58,0x7C,0xA0,0xC4,0xCA,0xD0,0xD6,0xDC,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7},
    {0x16,0x1C,0x22,0x28,0x2E,0x2F,0x30,0x31,0x32,0x33,0x2D,0x27,0x21,0x1B,0x15,0x39},
    {0x16,0x1C,0x22,0x40,0x46,0x6A,0x8E,0xB2,0xB3,0x8F,0x6B,0x47,0x23,0x1D,0x17,0x16},
    {0x35,0x36,0x37,0x38,0x39,0x5D,0x81,0xA5,0xC9,0xC8,0xC7,0xB2,0xD6,0xDC,0xDD,0xDE},
    {0xFF,0xFE,0xFD,0xFC,0xFB,0xC3,0xBD,0x99,0x75,0x51,0x2D,0x27,0x21,0x1B,0x15,0x14},
    {0xC9,0xC8,0xC7,0xC6,0xC5,0xC4,0xCA,0xD0,0xD6,0xDC,0xE2,0xBE,0x9A,0x76,0x52,0x2E},
    {0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7},
    {0xD8,0xD9,0xDA,0xDB,0xB7,0x93,0x6F,0x4B,0x45,0x3F,0x39,0x5D,0x81,0xA5,0xC9,0xCF},
    {0x10,0x16,0x1C,0x22,0x28,0x2E,0x52,0x76,0x9A,0xBE,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7},
    {0xDA,0xDB,0xB7,0x93,0x99,0xBD,0xE1,0xE0,0xDF,0xDE,0xDD,0xD7,0xD1,0xCB,0xCC,0xD2},
    {0x5E,0x82,0xA6,0xAC,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xDF,0xE7},
    {0x10,0x11,0x12,0x13,0x14,0x15,0x39,0x5D,0x81,0xA5,0xC9,0xCF,0xD5,0xDB,0xE1,0xE7},
    {0xC4,0xCA,0xD0,0xD6,0xDC,0xE2,0xBE,0x9A,0x76,0x52,0x2E,0x2F,0x30,0x31,0x32,0x33},
    {0x34,0x58,0x7C,0x7D,0x7E,0x7F,0xA3,0xC7,0xC6,0xC5,0xC4,0xA0,0x7C,0x58,0x34,0x35},
    {0x11,0x12,0x13,0x14,0x15,0x1B,0x21,0x27,0x2D,0x33,0x57,0x7B,0x9F,0xC3,0xE7,0xFF},
};
#define COLOR_SCHEME_COUNT (sizeof(color_schemes) / sizeof(color_schemes[0]))

static int current_color_scheme = 0;

/* ========================================================================== */
/*                         TERMINAL MANAGEMENT                                */
/* ========================================================================== */

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_tio) == -1) return;
    struct termios raw = orig_tio;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
}

static void safe_write(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return; }
        p += n; len -= n;
    }
}

static void cursor_hide(void) { safe_write(STDOUT_FILENO, "\x1b[?25l", 6); }
static void cursor_show(void) { safe_write(STDOUT_FILENO, "\x1b[?25h", 6); }
static void screen_clear(void) { safe_write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); }

static void update_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_w = ws.ws_col;
        term_h = ws.ws_row - 2;  /* Reserve for header */
        if (term_w < MIN_TERM_SIZE) term_w = MIN_TERM_SIZE;
        if (term_h < MIN_TERM_SIZE) term_h = MIN_TERM_SIZE;
        if (term_w > MAX_TERM_WIDTH) term_w = MAX_TERM_WIDTH;
        if (term_h > MAX_TERM_HEIGHT) term_h = MAX_TERM_HEIGHT;
    }
}

/* ========================================================================== */
/*                          ITERATION MAPPING                                 */
/* ========================================================================== */

static inline int iteration_to_index(int n, int max_n, int pal_len) {
    if (n >= max_n) return -1;
    return use_modulo ? (n % pal_len) : ((n * pal_len) / max_n);
}

static inline char iteration_to_char(int n, int max_n, const char *pal, int pal_len) {
    int idx = iteration_to_index(n, max_n, pal_len);
    return (idx < 0) ? FILL_CHAR : pal[idx];
}

static inline uint8_t iteration_to_color(int n, int max_n, const uint8_t *colors) {
    int idx = iteration_to_index(n, max_n, 16);
    return (idx < 0) ? 0 : colors[idx];
}

/* ========================================================================== */
/*                       FRACTAL CALCULATION                                  */
/* ========================================================================== */

typedef struct {
    int row_start, row_end;
    int width, height;
    int max_iter;
    double xmin, xmax, ymin, ymax;
    int julia_mode;
    double julia_cr, julia_ci;
    int *output;
} WorkerTask;

/*
 * Worker thread - calculates either Mandelbrot or Julia set.
 * 
 * Mandelbrot: z₀ = 0, c = pixel position, iterate z = z² + c
 * Julia:     z₀ = pixel position, c = fixed constant, iterate z = z² + c
 */
static void *calculate_rows(void *arg) {
    WorkerTask *task = arg;
    
    double dx = (task->xmax - task->xmin) / task->width;
    double dy = (task->ymax - task->ymin) / task->height;
    
    for (int row = task->row_start; row < task->row_end; row++) {
        int *out_row = task->output + row * task->width;
        double py = task->ymax - row * dy;
        
        for (int col = 0; col < task->width; col++) {
            double px = task->xmin + col * dx;
            
            double zr, zi, cr, ci;
            
            if (task->julia_mode) {
                /* Julia: z starts at pixel, c is constant */
                zr = px; zi = py;
                cr = task->julia_cr;
                ci = task->julia_ci;
            } else {
                /* Mandelbrot: z starts at 0, c is pixel */
                zr = 0; zi = 0;
                cr = px; ci = py;
            }
            
            int iter = 0;
            while (iter < task->max_iter) {
                double zr2 = zr * zr;
                double zi2 = zi * zi;
                if (zr2 + zi2 > 4.0) break;
                zi = 2 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                iter++;
            }
            
            out_row[col] = iter;
        }
    }
    return NULL;
}

static void snap_viewport_to_grid(int calc_height) {
    double px = (view_xmax - view_xmin) / term_w;
    double py = (view_ymax - view_ymin) / calc_height;
    
    double snapped_xmin = floor(view_xmin / px) * px;
    double snapped_ymin = floor(view_ymin / py) * py;
    
    view_xmax += snapped_xmin - view_xmin;
    view_ymax += snapped_ymin - view_ymin;
    view_xmin = snapped_xmin;
    view_ymin = snapped_ymin;
}

typedef struct { pthread_t handle; int created; } ThreadInfo;

/*
 * Compute fractal. In half-block mode, we calculate 2x the rows.
 */
static int compute_fractal(int **out_buffer, int *out_w, int *out_h) {
    update_term_size();
    
    int w = term_w;
    int h = use_halfblock ? term_h * 2 : term_h;  /* Double rows for half-blocks */
    
    snap_viewport_to_grid(h);
    
    int *buffer = malloc((size_t)w * h * sizeof(int));
    if (!buffer) return -1;
    
    int workers = num_threads ? num_threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (workers < 1) workers = 1;
    if (workers > h) workers = h;
    
    ThreadInfo *threads = malloc(sizeof(ThreadInfo) * workers);
    WorkerTask *tasks = malloc(sizeof(WorkerTask) * workers);
    if (!threads || !tasks) {
        free(buffer); free(threads); free(tasks);
        return -1;
    }
    
    int rows_each = h / workers;
    int extra_rows = h % workers;
    int current_row = 0;
    
    for (int i = 0; i < workers; i++) {
        int row_count = rows_each + (i < extra_rows ? 1 : 0);
        
        tasks[i] = (WorkerTask){
            .row_start = current_row,
            .row_end = current_row + row_count,
            .width = w, .height = h,
            .max_iter = max_iter,
            .xmin = view_xmin, .xmax = view_xmax,
            .ymin = view_ymin, .ymax = view_ymax,
            .julia_mode = julia_mode,
            .julia_cr = julia_cr, .julia_ci = julia_ci,
            .output = buffer
        };
        current_row += row_count;
        
        threads[i].created = (pthread_create(&threads[i].handle, NULL,
                                             calculate_rows, &tasks[i]) == 0);
        if (!threads[i].created) calculate_rows(&tasks[i]);
    }
    
    for (int i = 0; i < workers; i++)
        if (threads[i].created) pthread_join(threads[i].handle, NULL);
    
    free(threads);
    free(tasks);
    
    *out_buffer = buffer;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* ========================================================================== */
/*                            RENDERING                                       */
/* ========================================================================== */

/* Build command line that recreates current view */
static int build_cmdline(char *buf, size_t size) {
    const char *pal = palettes[current_palette];
    char *p = buf;
    char *end = buf + size - 1;
    
    p += snprintf(p, end - p, "marcepan -x %.9g %.9g -y %.9g %.9g -i %d",
                  view_xmin, view_xmax, view_ymin, view_ymax, max_iter);
    
    if (!use_color && p < end) p += snprintf(p, end - p, " -nc");
    if (!use_modulo && p < end) p += snprintf(p, end - p, " -m lin");
    if (use_halfblock && p < end) p += snprintf(p, end - p, " -hb");
    
    if (julia_mode && p < end)
        p += snprintf(p, end - p, " -j %.9g %.9g", julia_cr, julia_ci);
    
    if (p < end) p += snprintf(p, end - p, " -col %d", current_color_scheme + 1);
    
    /* Palette: use -pal N for builtins, --symbols '...' for custom */
    if (current_palette < (int)BUILTIN_PALETTE_COUNT && p < end) {
        p += snprintf(p, end - p, " -pal %d", current_palette + 1);
        /* Also show the actual palette string for reference (not copy-paste safe) */
        p += snprintf(p, end - p, " | \"%s\"", pal);
    } else if (p < end) {
        /* Custom palette - use single quotes with escaping */
        p += snprintf(p, end - p, " --symbols '");
        for (const char *s = pal; *s && p < end - 10; s++) {
            if (*s == '\'') {
                memcpy(p, "'\\''", 4);
                p += 4;
            } else {
                *p++ = *s;
            }
        }
        if (p < end) *p++ = '\'';
    }
    
    return (int)(p - buf);
}

/*
 * Render with half-blocks: each output row shows 2 calculation rows.
 * Uses '▀' (upper half) with FG=top, BG=bottom.
 * 
 * Logic for each cell:
 *   Both in set     → space (no color)
 *   Top in set      → '▄' with FG=bottom color, BG=black
 *   Bottom in set   → '▀' with FG=top color, BG=black  
 *   Neither in set  → '▀' with FG=top color, BG=bottom color
 * 
 * In monochrome mode: use grayscale based on iteration count.
 */
static char *render_halfblock(char *p, const int *iterations, int w, int h,
                              const uint8_t *colors) {
    for (int y = 0; y < h; y += 2) {
        const int *row_top = iterations + y * w;
        const int *row_bot = (y + 1 < h) ? iterations + (y + 1) * w : row_top;
        
        for (int x = 0; x < w; x++) {
            int n_top = row_top[x];
            int n_bot = row_bot[x];
            
            int in_set_top = (n_top >= max_iter);
            int in_set_bot = (n_bot >= max_iter);
            
            if (in_set_top && in_set_bot) {
                /* Both in set - empty space, reset any color */
                memcpy(p, "\x1b[0m ", 5);
                p += 5;
            } else if (in_set_top) {
                /* Top in set (black), bottom has color - lower half block */
                int c_bot = use_color ? colors[n_bot % 16] : (232 + (n_bot % 24));
                p += sprintf(p, "\x1b[38;5;%d;49m▄", c_bot);
            } else if (in_set_bot) {
                /* Bottom in set (black), top has color - upper half block */
                int c_top = use_color ? colors[n_top % 16] : (232 + (n_top % 24));
                p += sprintf(p, "\x1b[38;5;%d;49m▀", c_top);
            } else {
                /* Neither in set - upper half with FG=top, BG=bottom */
                int c_top = use_color ? colors[n_top % 16] : (232 + (n_top % 24));
                int c_bot = use_color ? colors[n_bot % 16] : (232 + (n_bot % 24));
                p += sprintf(p, "\x1b[38;5;%d;48;5;%dm▀", c_top, c_bot);
            }
        }
        /* Reset and newline */
        memcpy(p, "\x1b[0m\n", 5);
        p += 5;
    }
    return p;
}

/*
 * Standard ASCII rendering (original method)
 * Returns pointer to end of written data.
 */
static char *render_ascii(char *p, const int *iterations, int w, int h,
                          const char *pal, int pal_len, const uint8_t *colors) {
    int last_color = -1;
    
    for (int row = 0; row < h; row++) {
        const int *iter_row = iterations + row * w;
        
        for (int col = 0; col < w; col++) {
            int n = iter_row[col];
            char ch = iteration_to_char(n, max_iter, pal, pal_len);
            
            if (use_color && ch != FILL_CHAR) {
                int color = iteration_to_color(n, max_iter, colors);
                if (color != last_color) {
                    p += sprintf(p, "\x1b[38;5;%dm", color);
                    last_color = color;
                }
            } else if (last_color != -1) {
                memcpy(p, "\x1b[0m", 4);
                p += 4;
                last_color = -1;
            }
            *p++ = ch;
        }
        
        if (last_color != -1) {
            memcpy(p, "\x1b[0m", 4);
            p += 4;
            last_color = -1;
        }
        *p++ = '\n';
    }
    return p;
}

static void render_frame(const int *iterations, int w, int h) {
    const char *pal = palettes[current_palette];
    int pal_len = (int)strlen(pal);
    const uint8_t *colors = color_schemes[current_color_scheme];
    
    size_t buf_size = (size_t)w * h * OUTBUF_PER_CELL + 1024;
    char *buffer = malloc(buf_size);
    if (!buffer) return;
    
    char *p = buffer;
    
    /* Header */
    if (!batch_mode) {
        memcpy(p, "\x1b[2J\x1b[H", 7);
        p += 7;
        
        if (status_message[0]) {
            p += sprintf(p, "%s\n", status_message);
        } else {
            p += build_cmdline(p, 512);
            *p++ = '\n';
        }
    }
    
    /* Render fractal */
    if (use_halfblock) {
        p = render_halfblock(p, iterations, w, h, colors);
    } else {
        p = render_ascii(p, iterations, w, h, pal, pal_len, colors);
    }
    
    safe_write(STDOUT_FILENO, buffer, (size_t)(p - buffer));
    free(buffer);
}

/* ========================================================================== */
/*                           FILE EXPORT                                      */
/* ========================================================================== */

static void save_to_file(const int *iterations, int w, int h) {
    if (!iterations) return;
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char filename[64];
    snprintf(filename, sizeof(filename), "marcepan_%04d%02d%02d_%02d%02d%02d.txt",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    
    FILE *f = fopen(filename, "w");
    if (!f) return;
    
    const char *pal = palettes[current_palette];
    int pal_len = (int)strlen(pal);
    
    /* Write command line as comment */
    char cmdline[512];
    build_cmdline(cmdline, sizeof(cmdline));
    fprintf(f, "# %s\n", cmdline);
    
    /* Handle half-block mode: render 2 rows into 1 using simple chars */
    if (use_halfblock) {
        for (int y = 0; y < h; y += 2) {
            const int *row_top = iterations + y * w;
            const int *row_bot = (y + 1 < h) ? iterations + (y + 1) * w : row_top;
            for (int x = 0; x < w; x++) {
                /* Average the two iterations for ASCII representation */
                int avg = (row_top[x] + row_bot[x]) / 2;
                fputc(iteration_to_char(avg, max_iter, pal, pal_len), f);
            }
            fputc('\n', f);
        }
    } else {
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++)
                fputc(iteration_to_char(iterations[row * w + col], max_iter, pal, pal_len), f);
            fputc('\n', f);
        }
    }
    
    fclose(f);
    snprintf(status_message, MAX_STATUS_LEN, "Saved: %s", filename);
}

static void save_to_file_colored(const int *iterations, int w, int h) {
    if (!iterations) return;
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char filename[64];
    snprintf(filename, sizeof(filename), "marcepan_%04d%02d%02d_%02d%02d%02d.ansi",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    
    FILE *f = fopen(filename, "w");
    if (!f) return;
    
    const char *pal = palettes[current_palette];
    int pal_len = (int)strlen(pal);
    const uint8_t *colors = color_schemes[current_color_scheme];
    
    /* Write command line as comment */
    char cmdline[512];
    build_cmdline(cmdline, sizeof(cmdline));
    fprintf(f, "# %s\n", cmdline);
    
    if (use_halfblock) {
        /* Half-block colored output */
        for (int y = 0; y < h; y += 2) {
            const int *row_top = iterations + y * w;
            const int *row_bot = (y + 1 < h) ? iterations + (y + 1) * w : row_top;
            
            for (int x = 0; x < w; x++) {
                int n_top = row_top[x];
                int n_bot = row_bot[x];
                int c_top = iteration_to_color(n_top, max_iter, colors);
                int c_bot = iteration_to_color(n_bot, max_iter, colors);
                
                if (n_top >= max_iter && n_bot >= max_iter) {
                    fputc(' ', f);
                } else if (c_top == c_bot) {
                    if (n_top >= max_iter) {
                        fprintf(f, "\x1b[38;5;%dm▄", c_bot);
                    } else if (n_bot >= max_iter) {
                        fprintf(f, "\x1b[38;5;%dm▀", c_top);
                    } else {
                        fprintf(f, "\x1b[38;5;%dm█", c_top);
                    }
                } else {
                    fprintf(f, "\x1b[38;5;%d;48;5;%dm▀", c_top, c_bot);
                }
            }
            fprintf(f, "\x1b[0m\n");
        }
    } else {
        /* Standard colored output */
        int last_color = -1;
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                int n = iterations[row * w + col];
                char ch = iteration_to_char(n, max_iter, pal, pal_len);
                
                if (ch != FILL_CHAR) {
                    int color = iteration_to_color(n, max_iter, colors);
                    if (color != last_color) {
                        fprintf(f, "\x1b[38;5;%dm", color);
                        last_color = color;
                    }
                } else if (last_color != -1) {
                    fprintf(f, "\x1b[0m");
                    last_color = -1;
                }
                fputc(ch, f);
            }
            if (last_color != -1) {
                fprintf(f, "\x1b[0m");
                last_color = -1;
            }
            fputc('\n', f);
        }
    }
    
    fclose(f);
    snprintf(status_message, MAX_STATUS_LEN, "Saved: %s", filename);
}

/* ========================================================================== */
/*                         VIEW MANIPULATION                                  */
/* ========================================================================== */

static void pan_view(double dx_frac, double dy_frac) {
    double dx = (view_xmax - view_xmin) * dx_frac;
    double dy = (view_ymax - view_ymin) * dy_frac;
    view_xmin += dx; view_xmax += dx;
    view_ymin += dy; view_ymax += dy;
}

static void zoom_view(double factor) {
    double cx = (view_xmin + view_xmax) / 2;
    double cy = (view_ymin + view_ymax) / 2;
    double hw = (view_xmax - view_xmin) * factor / 2;
    double hh = (view_ymax - view_ymin) * factor / 2;
    view_xmin = cx - hw; view_xmax = cx + hw;
    view_ymin = cy - hh; view_ymax = cy + hh;
}

static void zoom_x_axis(double factor) {
    double cx = (view_xmin + view_xmax) / 2;
    double hw = (view_xmax - view_xmin) * factor / 2;
    view_xmin = cx - hw; view_xmax = cx + hw;
}

static void zoom_y_axis(double factor) {
    double cy = (view_ymin + view_ymax) / 2;
    double hh = (view_ymax - view_ymin) * factor / 2;
    view_ymin = cy - hh; view_ymax = cy + hh;
}

static void reset_view(void) {
    view_xmin = -2.0; view_xmax = 1.0;
    view_ymin = -1.0; view_ymax = 1.0;
    max_iter = 30;
    julia_mode = 0;
}

static void toggle_julia(void) {
    if (!julia_mode) {
        /* Switch to Julia: use current center as the constant c */
        julia_cr = (view_xmin + view_xmax) / 2;
        julia_ci = (view_ymin + view_ymax) / 2;
        julia_mode = 1;
        /* Reset view for Julia (they look best centered at origin) */
        view_xmin = -2.0; view_xmax = 2.0;
        view_ymin = -1.5; view_ymax = 1.5;
    } else {
        /* Switch back to Mandelbrot: jump to where julia_c came from */
        double cx = julia_cr, cy = julia_ci;
        julia_mode = 0;
        /* Center view on where we were */
        double hw = 1.5, hh = 1.0;
        view_xmin = cx - hw; view_xmax = cx + hw;
        view_ymin = cy - hh; view_ymax = cy + hh;
    }
}

static void cycle_value(int *value, int count, int delta) {
    *value = (*value + delta + count) % count;
}

/* ========================================================================== */
/*                          INPUT HANDLING                                    */
/* ========================================================================== */

static int read_key(void) {
    fd_set fds;
    struct timeval tv = {0, 10000};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return KEY_NONE;
    
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;
    
    switch (c) {
        case 'q': case 'Q': return 'q';
        case 'c': case 'C': return 'c';
        case 'm': case 'M': return 'm';
        case 'j': case 'J': return 'j';
        case 'h': case 'H': return 'h';
        case 'p': return 'p';
        case 'P': return 'P';
        case '1': return '1';
        case '2': return '2';
        case '+': return KEY_PLUS;
        case '-': return KEY_MINUS;
        case '/': return KEY_SLASH;
        case '*': return KEY_STAR;
        case '\r': case '\n': return KEY_ENTER;
    }
    
    if (c != '\x1b') return KEY_NONE;
    
    tv = (struct timeval){0, 2000};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return KEY_ESC;
    
    char seq[8] = {0};
    int len = 0;
    
    while (len < 7) {
        tv = (struct timeval){0, 1000};
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) break;
        if (read(STDIN_FILENO, &seq[len], 1) != 1) break;
        len++;
        if (len >= 2) {
            if (seq[0] == '[' && seq[len-1] >= 0x40 && seq[len-1] <= 0x7E) break;
            if (seq[0] == 'O' && len == 2) break;
        }
    }
    
    if (len == 0) return KEY_ESC;
    
    if (seq[0] == '[') {
        if (len == 2) {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
        if (len == 3 && seq[2] == '~') {
            switch (seq[1]) {
                case '2': return KEY_INS;
                case '3': return KEY_DEL;
                case '5': return KEY_PGUP;
                case '6': return KEY_PGDN;
            }
        }
        if (len == 5 && seq[1] == '1' && seq[2] == ';' && seq[3] == '2') {
            switch (seq[4]) {
                case 'A': return KEY_SHIFT_UP;
                case 'B': return KEY_SHIFT_DOWN;
                case 'C': return KEY_SHIFT_RIGHT;
                case 'D': return KEY_SHIFT_LEFT;
            }
        }
    }
    
    if (seq[0] == 'O' && len == 2) {
        switch (seq[1]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            case 'M': return KEY_ENTER;
            case 'P': return KEY_SLASH;
            case 'Q': return KEY_STAR;
            case 'R': return KEY_MINUS;
            case 'S': return KEY_PLUS;
            case 'o': return KEY_SLASH;
            case 'j': return KEY_STAR;
            case 'k': return KEY_PLUS;
            case 'm': return KEY_MINUS;
        }
    }
    
    return KEY_NONE;
}

/* ========================================================================== */
/*                              HELP                                          */
/* ========================================================================== */

static void print_help(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Interactive Mandelbrot/Julia fractal viewer\n\n");
    
    printf("OPTIONS:\n");
    printf("  -t N            Worker threads (default: auto-detect)\n");
    printf("  -nc             Disable color output\n");
    printf("  -x MIN MAX      X-axis range (default: -2.0 1.0)\n");
    printf("  -y MIN MAX      Y-axis range (default: -1.0 1.0)\n");
    printf("  -i N            Max iterations (default: 30, max %d)\n", MAX_ITERATIONS);
    printf("  -pal N          ASCII palette 1-%d (default: 2)\n", (int)BUILTIN_PALETTE_COUNT);
    printf("  -col N          Color scheme 1-%d (default: 1)\n", (int)COLOR_SCHEME_COUNT);
    printf("  -m, --mode M    Mapping mode: mod (default) or lin\n");
    printf("  -j CR CI        Julia mode with constant c = CR + CI*i\n");
    printf("  -hb             Enable half-block mode (2x vertical resolution)\n");
    printf("  --symbols \"S\"   Custom ASCII palette (2-%d chars)\n", MAX_CUSTOM_PAL);
    printf("  -b, --batch     Render once and exit\n");
    printf("  -h, --help      Show this help\n\n");
    
    printf("CONTROLS (NumLock OFF for numpad):\n");
    printf("  Numpad 8/2/4/6       Pan up/down/left/right\n");
    printf("  Numpad 7/9/1/3       Pan diagonally\n");
    printf("  Numpad 0 (Ins)       Zoom in\n");
    printf("  Numpad Enter         Zoom out\n");
    printf("  +/-                  Adjust iteration depth\n");
    printf("  Shift + Arrows       Stretch/shrink axis\n");
    printf("  ESC                  Reset to default view\n");
    printf("  / *                  Cycle ASCII palettes\n");
    printf("  1 2                  Cycle color schemes\n");
    printf("  c                    Toggle color on/off\n");
    printf("  m                    Toggle modulo/linear mode\n");
    printf("  j                    Toggle Julia/Mandelbrot mode\n");
    printf("  h                    Toggle half-block rendering\n");
    printf("  p                    Save to .txt (plain ASCII)\n");
    printf("  P (Shift+p)          Save to .ansi (with colors)\n");
    printf("  q                    Quit\n\n");
    
    printf("The header shows a command to recreate the current view.\n");
    printf("In Julia mode, the constant c is taken from the Mandelbrot center.\n");
}

/* ========================================================================== */
/*                              MAIN                                          */
/* ========================================================================== */

int main(int argc, char **argv) {
    /* Initialize palettes */
    for (int i = 0; i < (int)BUILTIN_PALETTE_COUNT; i++)
        palettes[i] = builtin_palettes[i];
    palette_count = BUILTIN_PALETTE_COUNT;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
            if (num_threads < 0 || num_threads > MAX_THREADS) num_threads = 0;
        }
        else if (!strcmp(argv[i], "-nc")) {
            use_color = 0;
        }
        else if (!strcmp(argv[i], "-hb")) {
            use_halfblock = 1;
        }
        else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--batch")) {
            batch_mode = 1;
        }
        else if (!strcmp(argv[i], "-x") && i + 2 < argc) {
            view_xmin = atof(argv[++i]);
            view_xmax = atof(argv[++i]);
            if (view_xmin >= view_xmax) {
                fprintf(stderr, "Error: xmin must be less than xmax\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "-y") && i + 2 < argc) {
            view_ymin = atof(argv[++i]);
            view_ymax = atof(argv[++i]);
            if (view_ymin >= view_ymax) {
                fprintf(stderr, "Error: ymin must be less than ymax\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            max_iter = atoi(argv[++i]);
            if (max_iter < 1 || max_iter > MAX_ITERATIONS) {
                fprintf(stderr, "Error: iterations must be 1-%d\n", MAX_ITERATIONS);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "-pal") && i + 1 < argc) {
            int v = atoi(argv[++i]) - 1;
            if (v < 0 || v >= (int)BUILTIN_PALETTE_COUNT) {
                fprintf(stderr, "Error: palette must be 1-%d\n", (int)BUILTIN_PALETTE_COUNT);
                return 1;
            }
            current_palette = v;
        }
        else if (!strcmp(argv[i], "-col") && i + 1 < argc) {
            int v = atoi(argv[++i]) - 1;
            if (v < 0 || v >= (int)COLOR_SCHEME_COUNT) {
                fprintf(stderr, "Error: color must be 1-%d\n", (int)COLOR_SCHEME_COUNT);
                return 1;
            }
            current_color_scheme = v;
        }
        else if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--mode")) && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "mod") || !strcmp(argv[i], "modulo")) {
                use_modulo = 1;
            } else if (!strcmp(argv[i], "lin") || !strcmp(argv[i], "linear")) {
                use_modulo = 0;
            } else {
                fprintf(stderr, "Error: mode must be 'mod' or 'lin'\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "-j") && i + 2 < argc) {
            julia_mode = 1;
            julia_cr = atof(argv[++i]);
            julia_ci = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--symbols") && i + 1 < argc) {
            const char *s = argv[++i];
            size_t len = strlen(s);
            if (len < 2 || len > MAX_CUSTOM_PAL) {
                fprintf(stderr, "Error: --symbols requires 2-%d characters\n", MAX_CUSTOM_PAL);
                return 1;
            }
            strncpy(custom_palette, s, MAX_CUSTOM_PAL);
            custom_palette[MAX_CUSTOM_PAL] = '\0';
            has_custom_palette = 1;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_help(argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }
    
    if (has_custom_palette) {
        palettes[palette_count] = custom_palette;
        current_palette = palette_count++;
    }
    
    if (num_threads == 0) {
        num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (num_threads < 1) num_threads = 4;
    }
    
    /* Setup terminal */
    atexit(disable_raw_mode);
    atexit(cursor_show);
    signal(SIGINT, exit);
    signal(SIGTERM, exit);
    
    enable_raw_mode();
    cursor_hide();
    
    /* Initial render */
    int *iterations = NULL;
    int img_w = 0, img_h = 0;
    
    if (compute_fractal(&iterations, &img_w, &img_h) == 0)
        render_frame(iterations, img_w, img_h);
    
    /* Main loop */
    int need_recalc = 0;
    int need_redraw = 0;
    
    while (!batch_mode) {
        int key = read_key();
        if (key == KEY_NONE) continue;
        
        /* Clear status message on any key (will be replaced by cmdline) */
        status_message[0] = '\0';
        
        switch (key) {
            case 'q': goto cleanup;
            
            /* Panning */
            case KEY_UP:    pan_view(0, PAN_FRACTION); need_recalc = 1; break;
            case KEY_DOWN:  pan_view(0, -PAN_FRACTION); need_recalc = 1; break;
            case KEY_LEFT:  pan_view(-PAN_FRACTION, 0); need_recalc = 1; break;
            case KEY_RIGHT: pan_view(PAN_FRACTION, 0); need_recalc = 1; break;
            
            /* Diagonal */
            case KEY_HOME:  pan_view(-PAN_FRACTION, PAN_FRACTION); need_recalc = 1; break;
            case KEY_PGUP:  pan_view(PAN_FRACTION, PAN_FRACTION); need_recalc = 1; break;
            case KEY_END:   pan_view(-PAN_FRACTION, -PAN_FRACTION); need_recalc = 1; break;
            case KEY_PGDN:  pan_view(PAN_FRACTION, -PAN_FRACTION); need_recalc = 1; break;
            
            /* Zoom */
            case KEY_INS:   zoom_view(1 - ZOOM_FRACTION); need_recalc = 1; break;
            case KEY_ENTER: zoom_view(1 + ZOOM_FRACTION); need_recalc = 1; break;
            
            /* Axis zoom */
            case KEY_SHIFT_UP:    zoom_y_axis(1 - ZOOM_FRACTION); need_recalc = 1; break;
            case KEY_SHIFT_DOWN:  zoom_y_axis(1 + ZOOM_FRACTION); need_recalc = 1; break;
            case KEY_SHIFT_LEFT:  zoom_x_axis(1 - ZOOM_FRACTION); need_recalc = 1; break;
            case KEY_SHIFT_RIGHT: zoom_x_axis(1 + ZOOM_FRACTION); need_recalc = 1; break;
            
            /* Iterations */
            case KEY_PLUS:
                if (max_iter < MAX_ITERATIONS - 5) { max_iter += 5; need_recalc = 1; }
                break;
            case KEY_MINUS:
                if (max_iter > 5) { max_iter -= 5; need_recalc = 1; }
                break;
            
            /* Reset */
            case KEY_ESC: reset_view(); need_recalc = 1; break;
            
            /* Palettes */
            case KEY_SLASH: cycle_value(&current_palette, palette_count, -1); need_redraw = 1; break;
            case KEY_STAR:  cycle_value(&current_palette, palette_count, 1); need_redraw = 1; break;
            case '1': cycle_value(&current_color_scheme, COLOR_SCHEME_COUNT, -1); need_redraw = 1; break;
            case '2': cycle_value(&current_color_scheme, COLOR_SCHEME_COUNT, 1); need_redraw = 1; break;
            
            /* Toggles */
            case 'c': use_color = !use_color; need_redraw = 1; break;
            case 'm': use_modulo = !use_modulo; need_redraw = 1; break;
            case 'j': toggle_julia(); need_recalc = 1; break;
            case 'h': use_halfblock = !use_halfblock; need_recalc = 1; break;
            
            /* Save */
            case 'p': save_to_file(iterations, img_w, img_h); need_redraw = 1; break;
            case 'P': save_to_file_colored(iterations, img_w, img_h); need_redraw = 1; break;
        }
        
        if (need_recalc) {
            free(iterations);
            iterations = NULL;
            if (compute_fractal(&iterations, &img_w, &img_h) == 0)
                render_frame(iterations, img_w, img_h);
            need_recalc = 0;
        } else if (need_redraw) {
            if (iterations) render_frame(iterations, img_w, img_h);
            need_redraw = 0;
        }
    }
    
cleanup:
    free(iterations);
    if (!batch_mode) screen_clear();
    return 0;
}
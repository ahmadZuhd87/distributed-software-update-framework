#include "config.h"

#include <GL/glut.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---- State parsed from the log ----------------------------------------- */

static char  g_logpath[256] = "logs/server.log";

static long  g_total     = 0;
static long  g_active     = 0;
static long  g_updates    = 0;
static long  g_uptodate   = 0;
static long  g_errors     = 0;

#define MAX_RECENT 12
static char  g_recent[MAX_RECENT][160];
static int   g_recent_count = 0;

static long  g_file_pos = 0;        /* how far we've read the log           */
static float g_pulse    = 0.0f;     /* animation phase for active lamps     */

#define WIN_W 760
#define WIN_H 520

/* ---- Text helper -------------------------------------------------------- */

static void draw_text(float x, float y, void *font, const char *s)
{
    glRasterPos2f(x, y);
    for (const char *p = s; *p; p++)
        glutBitmapCharacter(font, *p);
}

/* ---- Log polling -------------------------------------------------------- */
/* Read any new lines appended to the log and update the counters + recent
 * list. We re-derive "active" from the connection/disconnection deltas the
 * server prints, and we keep running totals from the keywords it logs. */
static void poll_log(void)
{
    FILE *fp = fopen(g_logpath, "rb");
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long end = ftell(fp);
    if (end < g_file_pos) g_file_pos = 0;   /* log rotated/truncated */
    fseek(fp, g_file_pos, SEEK_SET);

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        /* Update counters by matching the phrases server.c logs. */
        if (strstr(line, "connection accepted"))      g_total++;
        if (strstr(line, "update decision: UPDATE"))  g_updates++;
        if (strstr(line, "update decision: UP-TO-DATE")) g_uptodate++;
        if (strstr(line, "[ERROR]"))                   g_errors++;

        /* Track "active clients: N" if present to drive the lamps. */
        char *ac = strstr(line, "active clients: ");
        if (ac) g_active = atol(ac + strlen("active clients: "));

        /* Push into the recent ring (most-recent-last). */
        size_t llen = strlen(line);
        if (llen > 159) llen = 159;
        if (g_recent_count < MAX_RECENT) {
            memcpy(g_recent[g_recent_count], line, llen);
            g_recent[g_recent_count][llen] = '\0';
            g_recent_count++;
        } else {
            for (int i = 1; i < MAX_RECENT; i++)
                memcpy(g_recent[i-1], g_recent[i], sizeof(g_recent[0]));
            memcpy(g_recent[MAX_RECENT-1], line, llen);
            g_recent[MAX_RECENT-1][llen] = '\0';
        }
    }

    g_file_pos = ftell(fp);
    fclose(fp);
}

/* ---- Drawing ------------------------------------------------------------ */

static void draw_panel(float x, float y, float w, float h,
                       float r, float g, float b)
{
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
        glVertex2f(x,     y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x,     y + h);
    glEnd();
}

static void draw_counter(float x, float y, const char *label, long val,
                         float r, float g, float b)
{
    char buf[64];
    draw_panel(x, y, 130, 60, r * 0.18f, g * 0.18f, b * 0.18f);
    glColor3f(r, g, b);
    snprintf(buf, sizeof(buf), "%ld", val);
    draw_text(x + 12, y + 36, GLUT_BITMAP_TIMES_ROMAN_24, buf);
    glColor3f(0.8f, 0.8f, 0.85f);
    draw_text(x + 12, y + 14, GLUT_BITMAP_HELVETICA_10, label);
}

static void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT);

    /* Title. */
    glColor3f(0.95f, 0.95f, 1.0f);
    draw_text(20, WIN_H - 30, GLUT_BITMAP_HELVETICA_18,
              "Distributed Software Update Framework  -  Live Monitor");
    glColor3f(0.5f, 0.55f, 0.65f);
    draw_text(20, WIN_H - 48, GLUT_BITMAP_HELVETICA_12,
              "ENCS4330  Project #3   (reading server log)");

    /* Counter cards. */
    float cy = WIN_H - 130;
    draw_counter( 20, cy, "TOTAL CONNECTIONS", g_total,    0.4f, 0.7f, 1.0f);
    draw_counter(165, cy, "ACTIVE CLIENTS",    g_active,   0.3f, 0.9f, 0.4f);
    draw_counter(310, cy, "UPDATES SENT",      g_updates,  1.0f, 0.7f, 0.2f);
    draw_counter(455, cy, "UP TO DATE",        g_uptodate, 0.6f, 0.6f, 0.9f);
    draw_counter(600, cy, "ERRORS",            g_errors,   1.0f, 0.35f, 0.35f);

    /* Active-client lamps row. */
    glColor3f(0.8f, 0.8f, 0.85f);
    draw_text(20, cy - 28, GLUT_BITMAP_HELVETICA_12, "Concurrent clients:");
    float lx = 150, ly = cy - 24;
    int lamps = (int)g_active;
    if (lamps > 16) lamps = 16;
    float glow = 0.5f + 0.5f * sinf(g_pulse);
    for (int i = 0; i < 16; i++) {
        float cx = lx + i * 32.0f;
        int on = (i < lamps);
        if (on) glColor3f(0.15f, 0.4f + 0.5f * glow, 0.2f);
        else    glColor3f(0.18f, 0.18f, 0.2f);
        glBegin(GL_POLYGON);
        for (int a = 0; a < 16; a++) {
            float t = (float)a / 16.0f * 6.2832f;
            glVertex2f(cx + 9 * cosf(t), ly + 9 * sinf(t));
        }
        glEnd();
    }

    /* Recent events panel. */
    float py = 40, ph = cy - 70;
    draw_panel(20, py, WIN_W - 40, ph, 0.10f, 0.11f, 0.14f);
    glColor3f(0.7f, 0.75f, 0.85f);
    draw_text(30, py + ph - 18, GLUT_BITMAP_HELVETICA_12, "Recent events:");

    float ty = py + ph - 38;
    for (int i = 0; i < g_recent_count; i++) {
        const char *ln = g_recent[i];
        if (strstr(ln, "[ERROR]"))      glColor3f(1.0f, 0.45f, 0.45f);
        else if (strstr(ln, "[WARN"))   glColor3f(1.0f, 0.8f,  0.4f);
        else                            glColor3f(0.75f, 0.8f, 0.85f);
        /* Truncate long lines to fit. */
        char tmp[110];
        strncpy(tmp, ln, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        draw_text(30, ty, GLUT_BITMAP_8_BY_13, tmp);
        ty -= 16;
        if (ty < py + 8) break;
    }

    glutSwapBuffers();
}

static void reshape(int w, int h)
{
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, WIN_W, 0, WIN_H);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void timer(int v)
{
    (void)v;
    poll_log();
    g_pulse += 0.15f;
    glutPostRedisplay();
    glutTimerFunc(200, timer, 0);   /* ~5 fps poll; plenty for a monitor */
}

static void keyboard(unsigned char key, int x, int y)
{
    (void)x; (void)y;
    if (key == 27 || key == 'q') {   /* ESC or q to quit */
        exit(0);
    }
}

int main(int argc, char **argv)
{
    /* Resolve the log path from the config file if provided. */
    if (argc >= 2) {
        config_t cfg;
        if (config_load(argv[1], &cfg) == 0) {
            const char *lp = config_get_str(&cfg, "log_file", "logs/server.log");
            strncpy(g_logpath, lp, sizeof(g_logpath) - 1);
        }
    }
    printf("monitor_gl: visualising '%s'  (press q or ESC to quit)\n", g_logpath);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(WIN_W, WIN_H);
    glutCreateWindow("SW Update Framework - Monitor");

    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutTimerFunc(200, timer, 0);

    glutMainLoop();
    return 0;
}

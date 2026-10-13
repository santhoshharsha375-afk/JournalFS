/*
 * jfs_plot.c -- Generate labelled SVG graphs from benchmark CSV (pure C).
 *
 * Reads results/benchmark_results.csv and writes:
 *   results/throughput.svg  -- operations per second by mode
 *   results/latency.svg     -- avg / median / p95 latency by mode
 *   results/total_time.svg  -- total execution time by mode
 *   results/journal.svg     -- peak journal utilisation by mode
 *
 * SVG needs no libraries, so `make graphs` works on any machine with a
 * C compiler.  Open the .svg files in any browser or convert with e.g.
 * `rsvg-convert -o png file.svg` if PNGs are wanted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 16
#define W 800
#define H 500
#define ML 100    /* margin left   */
#define MR 40     /* margin right  */
#define MT 70     /* margin top    */
#define MB 100    /* margin bottom */

struct row {
    char mode[32];
    int ops;
    double total_s, ops_per_sec;
    double avg_ms, median_ms, p95_ms;
    int journal_used, journal_cap;
};

static const char *colors[] = {
    "#c0392b", "#2980b9", "#27ae60", "#8e44ad", "#e67e22"
};
static const int ncolors = 5;

/* Read "a,b,c" style CSV with a fixed header.  Returns row count. */
static int read_csv(const char *path, struct row *rows, int max)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; } /* header */
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        struct row *r = &rows[n];
        if (sscanf(line, "%31[^,],%d,%lf,%lf,%lf,%lf,%lf,%d,%d",
                   r->mode, &r->ops, &r->total_s, &r->ops_per_sec,
                   &r->avg_ms, &r->median_ms, &r->p95_ms,
                   &r->journal_used, &r->journal_cap) == 9)
            n++;
    }
    fclose(f);
    return n;
}

static double nice_max(double v)
{
    if (v <= 0)
        return 1.0;
    double exp = 1.0, x = v;
    while (x >= 10) { x /= 10; exp *= 10; }
    double m = 1;
    if (x > 5) m = 10;
    else if (x > 2.5) m = 5;
    else if (x > 2) m = 2.5;
    else if (x > 1) m = 2;
    return m * exp;
}

/*
 * Draw a grouped bar chart.
 * series: one entry per bar group member; values[i] is the value for row i.
 */
static int write_bar_svg(const char *path, const char *title,
                         const char *ylabel, struct row *rows, int n,
                         const char **series_names,
                         double (*values)[MAX_ROWS], int nseries)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    double vmax = 0;
    for (int s = 0; s < nseries; s++)
        for (int i = 0; i < n; i++)
            if (values[s][i] > vmax)
                vmax = values[s][i];
    vmax = nice_max(vmax * 1.15);

    int plot_w = W - ML - MR;
    int plot_h = H - MT - MB;
    double slot = (double)plot_w / n;
    double bw = slot * 0.7 / nseries;

    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" "
               "height=\"%d\" font-family=\"sans-serif\">\n", W, H);
    fprintf(f, "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", W, H);
    fprintf(f, "<text x=\"%d\" y=\"30\" text-anchor=\"middle\" font-size=\"18\" "
               "font-weight=\"bold\">%s</text>\n", W / 2, title);

    /* Axes. */
    fprintf(f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
               "stroke=\"black\"/>\n", ML, MT, ML, H - MB);
    fprintf(f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
               "stroke=\"black\"/>\n", ML, H - MB, W - MR, H - MB);

    /* Y ticks + gridlines. */
    for (int t = 0; t <= 5; t++) {
        double frac = t / 5.0;
        int y = (int)((H - MB) - frac * plot_h);
        fprintf(f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
                   "stroke=\"#dddddd\"/>\n", ML, y, W - MR, y);
        fprintf(f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
                   "stroke=\"black\"/>\n", ML - 5, y, ML, y);
        fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"end\" "
                   "font-size=\"11\">%.4g</text>\n",
                ML - 10, y + 4, frac * vmax);
    }
    fprintf(f, "<text x=\"24\" y=\"%d\" text-anchor=\"middle\" font-size=\"12\" "
               "transform=\"rotate(-90 24 %d)\">%s</text>\n",
            MT + plot_h / 2, MT + plot_h / 2, ylabel);

    /* Bars. */
    for (int i = 0; i < n; i++) {
        for (int s = 0; s < nseries; s++) {
            double v = values[s][i];
            int bh = (int)(v / vmax * plot_h);
            int x = (int)(ML + i * slot + slot * 0.15 + s * bw);
            int y = H - MB - bh;
            fprintf(f, "<rect x=\"%d\" y=\"%d\" width=\"%.0f\" height=\"%d\" "
                       "fill=\"%s\"/>\n",
                    x, y, bw - 2, bh, colors[s % ncolors]);
            fprintf(f, "<text x=\"%.0f\" y=\"%d\" text-anchor=\"middle\" "
                       "font-size=\"10\">%.2f</text>\n",
                    x + bw / 2, y - 4, v);
        }
        fprintf(f, "<text x=\"%.0f\" y=\"%d\" text-anchor=\"middle\" "
                   "font-size=\"12\">%s</text>\n",
                ML + i * slot + slot / 2, H - MB + 20, rows[i].mode);
    }

    /* Legend. */
    int lx = ML, ly = H - 30;
    for (int s = 0; s < nseries; s++) {
        fprintf(f, "<rect x=\"%d\" y=\"%d\" width=\"14\" height=\"14\" "
                   "fill=\"%s\"/>\n", lx, ly - 11, colors[s % ncolors]);
        fprintf(f, "<text x=\"%d\" y=\"%d\" font-size=\"12\">%s</text>\n",
                lx + 20, ly, series_names[s]);
        lx += 30 + (int)(10 * strlen(series_names[s]));
    }

    fprintf(f, "</svg>\n");
    fclose(f);
    return 0;
}

int main(void)
{
    struct row rows[MAX_ROWS];
    int n = read_csv("results/benchmark_results.csv", rows, MAX_ROWS);
    if (n < 0) {
        fprintf(stderr, "cannot read results/benchmark_results.csv -- "
                        "run 'make benchmark' first\n");
        return 2;
    }
    if (n == 0) {
        fprintf(stderr, "no data rows in CSV\n");
        return 2;
    }

    double thr[MAX_ROWS] = {0}, tot[MAX_ROWS] = {0}, jur[MAX_ROWS] = {0};
    double lat[3][MAX_ROWS] = {{0}};
    for (int i = 0; i < n; i++) {
        thr[i] = rows[i].ops_per_sec;
        tot[i] = rows[i].total_s;
        jur[i] = rows[i].journal_cap ? rows[i].journal_used : 0;
        lat[0][i] = rows[i].avg_ms;
        lat[1][i] = rows[i].median_ms;
        lat[2][i] = rows[i].p95_ms;
    }

    const char *one[] = { "value" };
    const char *lat_names[] = { "average", "median", "p95" };
    double (*thr_v)[MAX_ROWS] = (double (*)[MAX_ROWS])thr;
    double (*tot_v)[MAX_ROWS] = (double (*)[MAX_ROWS])tot;
    double (*jur_v)[MAX_ROWS] = (double (*)[MAX_ROWS])jur;

    int errs = 0;
    errs += write_bar_svg("results/throughput.svg",
                          "JournalFS: Throughput by mode",
                          "Operations per second", rows, n,
                          one, thr_v, 1) != 0;
    errs += write_bar_svg("results/latency.svg",
                          "JournalFS: Operation latency by mode",
                          "Latency (ms)", rows, n,
                          lat_names, lat, 3) != 0;
    errs += write_bar_svg("results/total_time.svg",
                          "JournalFS: Total execution time by mode",
                          "Total time (s)", rows, n,
                          one, tot_v, 1) != 0;
    errs += write_bar_svg("results/journal.svg",
                          "JournalFS: Peak journal utilisation by mode",
                          "Journal slots used", rows, n,
                          one, jur_v, 1) != 0;

    if (errs) {
        fprintf(stderr, "one or more graphs failed to write\n");
        return 1;
    }
    printf("Graphs written to results/\n");
    printf("  throughput.svg\n  latency.svg\n  total_time.svg\n  journal.svg\n");
    return 0;
}

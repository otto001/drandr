/* See LICENSE file for copyright and license details. */
/* Default settings; can be overriden by command line. */

static const char *fonts[] = {
        "monospace:size=11"
};
static const char *colors[SchemeLast][3] = {
        /*     fg         bg       */
        [SchemeSel] = { "#eeeeee", "#0d4b82" },
        [SchemeNorm] = { "#eeeeee", "#222222" },
        [SchemeMon] = { "#eeeeee", "#555555" },
};

static int width = 1400;
static int height = 800;
static int side_area = 400;

static int interval = 16;

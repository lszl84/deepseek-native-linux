#pragma once
#include "textlayout.h"

typedef enum { MD_PARA, MD_HEADING, MD_CODE, MD_LIST, MD_QUOTE, MD_TABLE, MD_HR, MD_IMAGE } MDKind;

typedef struct MDBlock MDBlock;

typedef struct {
    int task; /* -1 not a task, 0 open, 1 done */
    GPtrArray *blocks; /* MDBlock* */
} MDItem;

struct MDBlock {
    MDKind kind;
    RichText *rt;      /* paragraph / heading */
    int level;
    char *lang, *code; /* code */
    gboolean ordered;
    int start;
    GPtrArray *items;    /* MDItem* */
    GPtrArray *children; /* quote: MDBlock* */
    GPtrArray *header;   /* table: RichText* */
    GPtrArray *rows;     /* table: GPtrArray* of RichText* */
    int *aligns;         /* PangoAlignment per column */
    int ncols;
    char *alt, *src;
};

typedef struct {
    FontId font;
    int flags;
    ColorId color;
    int spacing;
    const char *base_dir;
} MDStyle;

MDStyle md_style_body(const char *base_dir);
MDStyle md_style_thinking(void);
GPtrArray *md_parse(const char *text, const MDStyle *st); /* MDBlock* */
RichText *md_inline(const char *text, const MDStyle *st);
void md_block_free(gpointer b);

/* Syntax highlighting into a monospaced RichText. */
RichText *syntax_highlight(const char *code, const char *lang, FontId font);

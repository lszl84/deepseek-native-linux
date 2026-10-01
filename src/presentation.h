#pragma once
#include "util.h"

typedef enum { CAT_COMMAND, CAT_READ, CAT_EDIT, CAT_WRITE, CAT_SEARCH, CAT_PLAN, CAT_WEB, CAT_DELEGATE, CAT_ASK, CAT_OTHER } ToolCategory;

ToolCategory tool_category(const char *name);
const char *tool_display_name(const char *name);
const char *tool_icon(const char *name);
/* One-line summary shown after the tool's name. */
char *tool_summary(const char *name, JsonObject *input, const char *cwd);
/* "Ran commands", "Wrote files and ran commands", ... */
char *tool_group_title(const ToolCategory *cats, int n, gboolean running);
const char *tool_group_icon(const ToolCategory *cats, int n);

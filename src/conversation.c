#include "conversation.h"
#include <string.h>

void usage_add(Usage *a, const Usage *b) {
    a->input += b->input;
    a->output += b->output;
    a->cache_read += b->cache_read;
    a->cache_write += b->cache_write;
}

ImageData *image_data_new(const char *media_type, const char *base64, const char *path) {
    ImageData *d = g_new0(ImageData, 1);
    d->media_type = g_strdup(media_type);
    d->base64 = g_strdup(base64);
    d->path = g_strdup(path);
    return d;
}

ImageData *image_data_copy(const ImageData *d) { return image_data_new(d->media_type, d->base64, d->path); }

void image_data_free(gpointer p) {
    ImageData *d = p;
    if (!d) return;
    g_free(d->media_type);
    g_free(d->base64);
    g_free(d->path);
    g_free(d);
}

static Block *block_new(BlockType t) {
    Block *b = g_new0(Block, 1);
    b->type = t;
    return b;
}

Block *block_text(const char *t) {
    Block *b = block_new(BLK_TEXT);
    b->text = g_strdup(t ? t : "");
    return b;
}

Block *block_thinking(const char *t, const char *sig) {
    Block *b = block_new(BLK_THINKING);
    b->text = g_strdup(t ? t : "");
    b->signature = sig && *sig ? g_strdup(sig) : NULL;
    return b;
}

Block *block_tool_use(const char *id, const char *name, const char *input) {
    Block *b = block_new(BLK_TOOL_USE);
    b->id = g_strdup(id);
    b->name = g_strdup(name);
    b->input = g_strdup(input && *input ? input : "{}");
    return b;
}

Block *block_tool_result(const char *id, const char *content, gboolean is_error, GPtrArray *images) {
    Block *b = block_new(BLK_TOOL_RESULT);
    b->id = g_strdup(id);
    b->text = g_strdup(content ? content : "");
    b->is_error = is_error;
    b->images = images;
    return b;
}

Block *block_image(ImageData *img) {
    Block *b = block_new(BLK_IMAGE);
    b->images = g_ptr_array_new_with_free_func(image_data_free);
    g_ptr_array_add(b->images, img);
    return b;
}

Block *block_copy(const Block *s) {
    Block *b = block_new(s->type);
    b->text = g_strdup(s->text);
    b->signature = g_strdup(s->signature);
    b->id = g_strdup(s->id);
    b->name = g_strdup(s->name);
    b->input = g_strdup(s->input);
    b->is_error = s->is_error;
    if (s->images) {
        b->images = g_ptr_array_new_with_free_func(image_data_free);
        for (guint i = 0; i < s->images->len; i++) g_ptr_array_add(b->images, image_data_copy(g_ptr_array_index(s->images, i)));
    }
    return b;
}

void block_free(gpointer p) {
    Block *b = p;
    if (!b) return;
    g_free(b->text);
    g_free(b->signature);
    g_free(b->id);
    g_free(b->name);
    g_free(b->input);
    if (b->images) g_ptr_array_unref(b->images);
    g_free(b);
}

Message *message_new(Role role) {
    Message *m = g_new0(Message, 1);
    m->id = uuid_new();
    m->role = role;
    m->blocks = g_ptr_array_new_with_free_func(block_free);
    m->time = now_ts();
    return m;
}

Message *message_copy(const Message *s) {
    Message *m = g_new0(Message, 1);
    *m = *s;
    m->id = g_strdup(s->id);
    m->model = g_strdup(s->model);
    m->blocks = g_ptr_array_new_with_free_func(block_free);
    for (guint i = 0; i < s->blocks->len; i++) g_ptr_array_add(m->blocks, block_copy(g_ptr_array_index(s->blocks, i)));
    return m;
}

void message_free(gpointer p) {
    Message *m = p;
    if (!m) return;
    g_free(m->id);
    g_free(m->model);
    g_ptr_array_unref(m->blocks);
    g_free(m);
}

void message_add(Message *m, Block *b) { g_ptr_array_add(m->blocks, b); }

char *message_text(const Message *m) {
    GString *s = g_string_new("");
    gboolean first = TRUE;
    for (guint i = 0; i < m->blocks->len; i++) {
        Block *b = g_ptr_array_index(m->blocks, i);
        if (b->type != BLK_TEXT) continue;
        if (!first) g_string_append(s, "\n\n");
        g_string_append(s, b->text);
        first = FALSE;
    }
    return g_string_free(s, FALSE);
}

/* ---------- storage ---------- */

static JsonObject *image_to_json(const ImageData *d) {
    JsonObject *o = jo_new();
    jo_str(o, "mediaType", d->media_type);
    jo_str(o, "base64", d->base64);
    if (d->path) jo_str(o, "path", d->path);
    return o;
}

static ImageData *image_from_json(JsonObject *o) {
    if (!o) return NULL;
    return image_data_new(jstr(o, "mediaType") ? jstr(o, "mediaType") : "image/png", jstr(o, "base64") ? jstr(o, "base64") : "", jstr(o, "path"));
}

static JsonObject *block_to_json(const Block *b) {
    JsonObject *o = jo_new();
    switch (b->type) {
    case BLK_TEXT:
        jo_str(o, "type", "text");
        jo_str(o, "text", b->text);
        break;
    case BLK_THINKING:
        jo_str(o, "type", "thinking");
        jo_str(o, "text", b->text);
        if (b->signature) jo_str(o, "signature", b->signature);
        break;
    case BLK_TOOL_USE:
        jo_str(o, "type", "tool_use");
        jo_str(o, "id", b->id);
        jo_str(o, "name", b->name);
        jo_str(o, "input", b->input);
        break;
    case BLK_TOOL_RESULT:
        jo_str(o, "type", "tool_result");
        jo_str(o, "id", b->id);
        jo_str(o, "content", b->text);
        if (b->is_error) jo_bool(o, "isError", TRUE);
        if (b->images && b->images->len) {
            JsonArray *a = json_array_new();
            for (guint i = 0; i < b->images->len; i++) json_array_add_object_element(a, image_to_json(g_ptr_array_index(b->images, i)));
            jo_arr(o, "images", a);
        }
        break;
    case BLK_IMAGE:
        jo_str(o, "type", "image");
        if (b->images && b->images->len) jo_obj(o, "image", image_to_json(g_ptr_array_index(b->images, 0)));
        break;
    }
    return o;
}

static Block *block_from_json(JsonObject *o) {
    const char *t = jstr(o, "type");
    if (!t) return block_text("");
    if (!strcmp(t, "text")) return block_text(jstr(o, "text"));
    if (!strcmp(t, "thinking")) return block_thinking(jstr(o, "text"), jstr(o, "signature"));
    if (!strcmp(t, "tool_use")) return block_tool_use(jstr(o, "id"), jstr(o, "name"), jstr(o, "input"));
    if (!strcmp(t, "tool_result")) {
        GPtrArray *imgs = NULL;
        JsonArray *a = jarr(o, "images");
        if (a && json_array_get_length(a)) {
            imgs = g_ptr_array_new_with_free_func(image_data_free);
            for (guint i = 0; i < json_array_get_length(a); i++) {
                ImageData *d = image_from_json(json_array_get_object_element(a, i));
                if (d) g_ptr_array_add(imgs, d);
            }
        }
        return block_tool_result(jstr(o, "id"), jstr(o, "content"), jbool(o, "isError", FALSE), imgs);
    }
    if (!strcmp(t, "image")) {
        ImageData *d = image_from_json(jobj(o, "image"));
        if (d) return block_image(d);
    }
    return block_text("");
}

JsonObject *message_to_json(const Message *m) {
    JsonObject *o = jo_new();
    jo_str(o, "id", m->id);
    jo_str(o, "role", m->role == ROLE_USER ? "user" : "assistant");
    JsonArray *a = json_array_new();
    for (guint i = 0; i < m->blocks->len; i++) json_array_add_object_element(a, block_to_json(g_ptr_array_index(m->blocks, i)));
    jo_arr(o, "blocks", a);
    jo_dbl(o, "time", m->time);
    if (m->has_usage) {
        JsonObject *u = jo_new();
        jo_int(u, "input", m->usage.input);
        jo_int(u, "output", m->usage.output);
        jo_int(u, "cacheRead", m->usage.cache_read);
        jo_int(u, "cacheWrite", m->usage.cache_write);
        jo_obj(o, "usage", u);
    }
    if (m->model) jo_str(o, "model", m->model);
    if (m->duration > 0) jo_dbl(o, "duration", m->duration);
    if (m->is_tool_results) jo_bool(o, "isToolResults", TRUE);
    if (m->steered) jo_bool(o, "steered", TRUE);
    if (m->notice) jo_bool(o, "notice", TRUE);
    return o;
}

Message *message_from_json(JsonObject *o) {
    if (!o) return NULL;
    Message *m = message_new(g_strcmp0(jstr(o, "role"), "assistant") == 0 ? ROLE_ASSISTANT : ROLE_USER);
    if (jstr(o, "id")) { g_free(m->id); m->id = g_strdup(jstr(o, "id")); }
    JsonArray *a = jarr(o, "blocks");
    for (guint i = 0; a && i < json_array_get_length(a); i++) {
        JsonNode *n = json_array_get_element(a, i);
        if (JSON_NODE_HOLDS_OBJECT(n)) message_add(m, block_from_json(json_node_get_object(n)));
    }
    m->time = jdbl(o, "time", now_ts());
    JsonObject *u = jobj(o, "usage");
    if (u) {
        m->has_usage = TRUE;
        m->usage.input = jint(u, "input", 0);
        m->usage.output = jint(u, "output", 0);
        m->usage.cache_read = jint(u, "cacheRead", 0);
        m->usage.cache_write = jint(u, "cacheWrite", 0);
    }
    if (jstr(o, "model")) m->model = g_strdup(jstr(o, "model"));
    m->duration = jdbl(o, "duration", 0);
    m->is_tool_results = jbool(o, "isToolResults", FALSE);
    m->steered = jbool(o, "steered", FALSE);
    m->notice = jbool(o, "notice", FALSE);
    return m;
}

/* ---------- wire ---------- */

static JsonObject *image_wire(const ImageData *d) {
    JsonObject *o = jo_new();
    jo_str(o, "type", "image");
    JsonObject *src = jo_new();
    jo_str(src, "type", "base64");
    jo_str(src, "media_type", d->media_type);
    jo_str(src, "data", d->base64);
    jo_obj(o, "source", src);
    return o;
}

JsonObject *block_wire(const Block *b) {
    JsonObject *o = jo_new();
    switch (b->type) {
    case BLK_TEXT:
        jo_str(o, "type", "text");
        jo_str(o, "text", b->text);
        break;
    case BLK_THINKING:
        jo_str(o, "type", "thinking");
        jo_str(o, "thinking", b->text);
        if (b->signature && *b->signature) jo_str(o, "signature", b->signature);
        break;
    case BLK_TOOL_USE: {
        jo_str(o, "type", "tool_use");
        jo_str(o, "id", b->id);
        jo_str(o, "name", b->name);
        JsonNode *n = json_parse_str(b->input, -1);
        if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
            if (n) json_node_unref(n);
            n = jnode_obj(jo_new());
            json_object_unref(json_node_get_object(n));
        }
        json_object_set_member(o, "input", n);
        break;
    }
    case BLK_TOOL_RESULT: {
        jo_str(o, "type", "tool_result");
        jo_str(o, "tool_use_id", b->id);
        JsonArray *parts = json_array_new();
        if (b->text && *b->text) {
            JsonObject *t = jo_new();
            jo_str(t, "type", "text");
            jo_str(t, "text", b->text);
            json_array_add_object_element(parts, t);
        }
        if (b->images)
            for (guint i = 0; i < b->images->len; i++) json_array_add_object_element(parts, image_wire(g_ptr_array_index(b->images, i)));
        jo_arr(o, "content", parts);
        if (b->is_error) jo_bool(o, "is_error", TRUE);
        break;
    }
    case BLK_IMAGE:
        json_object_unref(o);
        if (b->images && b->images->len) return image_wire(g_ptr_array_index(b->images, 0));
        o = jo_new();
        jo_str(o, "type", "text");
        jo_str(o, "text", "(image)");
        break;
    }
    return o;
}

JsonObject *message_wire(const Message *m) {
    JsonObject *o = jo_new();
    jo_str(o, "role", m->role == ROLE_USER ? "user" : "assistant");
    JsonArray *a = json_array_new();
    for (guint i = 0; i < m->blocks->len; i++) json_array_add_object_element(a, block_wire(g_ptr_array_index(m->blocks, i)));
    jo_arr(o, "content", a);
    return o;
}

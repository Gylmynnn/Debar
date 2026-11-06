#ifndef DEBAR_MODULE_H
#define DEBAR_MODULE_H

#include <stdbool.h>
#include <cairo.h>

/* Common interface for status modules (clock, battery, volume, workspace). */
typedef struct module module_t;

struct module {
    const char *name;
    void (*update)(module_t *self);
    void (*draw)(module_t *self, cairo_t *cr);
    void (*destroy)(module_t *self);
    char text[128];
    void *userdata;
    bool dirty;
};

#endif

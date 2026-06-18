/**
 * @file    ui_input.c
 * @brief   최상위 입력 분배기 구현
 */

#include "ui_input.h"
#include "bsp_ws_1_47.h"
#include <string.h>

#define MAX_GLOBAL    4
#define MAX_TABS      4
#define MAX_PER_TAB   4

typedef struct {
    ui_input_fn_t fn;
    int           priority;
} handler_t;

static handler_t s_global[MAX_GLOBAL];
static int       s_global_n = 0;

static handler_t s_tab[MAX_TABS][MAX_PER_TAB];
static int       s_tab_n[MAX_TABS];

static int s_active_tab = 0;

static void insert(handler_t *list, int *count, int max,
                   ui_input_fn_t fn, int priority)
{
    if (*count >= max) return;
    int i = *count;
    while (i > 0 && list[i - 1].priority > priority) {
        list[i] = list[i - 1];
        i--;
    }
    list[i].fn       = fn;
    list[i].priority = priority;
    (*count)++;
}

static bool dispatch(bool pressed, lv_point_t pt)
{
    for (int i = 0; i < s_global_n; i++) {
        if (s_global[i].fn(pressed, pt)) return true;
    }

    int tab = s_active_tab;
    if ((unsigned)tab < MAX_TABS) {
        for (int i = 0; i < s_tab_n[tab]; i++) {
            if (s_tab[tab][i].fn(pressed, pt)) return true;
        }
    }
    return false;
}

void ui_input_init(void)
{
    memset(s_global, 0, sizeof(s_global));
    memset(s_tab,    0, sizeof(s_tab));
    memset(s_tab_n,  0, sizeof(s_tab_n));
    bsp_indev_set_hook(dispatch);
}

void ui_input_add_global(ui_input_fn_t fn, int priority)
{
    insert(s_global, &s_global_n, MAX_GLOBAL, fn, priority);
}

void ui_input_add_tab(int tab_idx, ui_input_fn_t fn, int priority)
{
    if ((unsigned)tab_idx >= MAX_TABS) return;
    insert(s_tab[tab_idx], &s_tab_n[tab_idx], MAX_PER_TAB, fn, priority);
}

void ui_input_set_active_tab(int tab_idx)
{
    s_active_tab = tab_idx;
}

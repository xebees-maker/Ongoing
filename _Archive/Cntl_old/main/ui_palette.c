/**
 * @file    ui_palette.c
 * @brief   16색 표준 팔레트 배열 정의
 */

#include "ui_palette.h"

const ColorItem COLOR_PALETTE[] = {
    { "White",      PALETTE_WHITE      },
    { "Black",      PALETTE_BLACK      },
    { "Light Gray", PALETTE_LIGHT_GRAY },
    { "Gray",       PALETTE_GRAY       },
    { "Dark Gray",  PALETTE_DARK_GRAY  },
    { "Red",        PALETTE_RED        },
    { "Green",      PALETTE_GREEN      },
    { "Yellow",     PALETTE_YELLOW     },
    { "Blue",       PALETTE_BLUE       },
    { "Magenta",    PALETTE_MAGENTA    },
    { "Purple",     PALETTE_PURPLE     },
    { "Orange",     PALETTE_ORANGE     },
    { "Cyan",       PALETTE_CYAN       },
    { "Teal",       PALETTE_TEAL       },
    { "Olive",      PALETTE_OLIVE      },
    { "Lime",       PALETTE_LIME       },
};

const int COLOR_PALETTE_COUNT = sizeof(COLOR_PALETTE) / sizeof(COLOR_PALETTE[0]);

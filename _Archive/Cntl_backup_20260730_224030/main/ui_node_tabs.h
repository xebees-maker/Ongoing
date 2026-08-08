#ifndef UI_NODE_TABS_H
#define UI_NODE_TABS_H

#include "lvgl.h"

/* "센서 요약" 탭을 추가하고, 이후 1초마다 페어링된 노드를 확인해 노드별 탭을 추가한다.
 * LVGL 이 버전은 탭 삭제 API가 없어 한 번 추가된 탭은 계속 남아있다(언페어링 시 제거 안 함). */
void ui_node_tabs_init(lv_obj_t *tabview);

#endif /* UI_NODE_TABS_H */

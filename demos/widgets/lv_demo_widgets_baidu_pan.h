/**
 * @file lv_demo_widgets_baidu_pan.h
 *
 * Baidu Netdisk login tab for the LVGL widgets demo.
 */
#ifndef LV_DEMO_WIDGETS_BAIDU_PAN_H
#define LV_DEMO_WIDGETS_BAIDU_PAN_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lv_demo_widgets.h"
#if LV_USE_DEMO_WIDGETS

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Create the BaiduPan tab content inside @p parent.
 * @param parent  tab page object returned by lv_tabview_get_content()
 */
void lv_demo_widgets_baidu_pan_create(lv_obj_t *parent);

#endif /* LV_USE_DEMO_WIDGETS */

#ifdef __cplusplus
}
#endif

#endif /* LV_DEMO_WIDGETS_BAIDU_PAN_H */

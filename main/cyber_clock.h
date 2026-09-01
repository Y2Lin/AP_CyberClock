#pragma once
#include "bsp_button.h"

// 赛博朋克时钟演示页入口（与 demo_entry_t 接口兼容）。
void demo_cyber_clock_enter(void);
void demo_cyber_clock_exit(void);
void demo_cyber_clock_key(bsp_btn_t btn, bsp_btn_ev_t ev);

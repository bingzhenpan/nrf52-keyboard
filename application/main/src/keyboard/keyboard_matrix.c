/*
Copyright (C) 2018,2019 Jim Jiang <jim@lotlab.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
/**
 * @file matrix.c
 * @author Jim Jiang
 * @date 2018-05-13
 */
#include <stdbool.h>
#include <stdint.h>

#include "nrf.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_log.h"

#include "../config/keyboard_config.h"
#include "ble_keyboard.h"
#include "debug.h"
#include "keyboard_matrix.h"
#include "keyboard_evt.h"
#include "events.h"
#include "matrix.h"
#include "print.h"
#include "util.h"
#include "wait.h"

#ifndef DEBOUNCE
#define DEBOUNCE 1
#endif
// 实际的消抖次数
#define DEBOUNCE_RELOAD ((DEBOUNCE + KEYBOARD_SCAN_INTERVAL - 1) / KEYBOARD_SCAN_INTERVAL)

static uint8_t debouncing = 0; //DEBOUNCE_RELOAD;

static bool scan_for_wakeup = false;
matrix_row_t row_for_wakeup = -1;
matrix_row_t cols_for_wakeup = -1;

/* matrix state(1:on, 0:off) */
static matrix_row_t matrix[MATRIX_ROWS];
static matrix_row_t matrix_debouncing[MATRIX_COLS];

static matrix_row_t read_cols(void);
static void select_row(uint8_t row);
static void unselect_rows(void);

#define READ_COL(pin) (nrf_gpio_pin_read(pin))

// 只在键盘链接后才发生唤醒按键
static bool keyboard_connected = false;

/**
 * @brief 键盘唤醒后, 快速扫描一次按键, 获取唤醒键盘的按键.
 *        解决按一个建唤醒键盘后, 再按一次才发送该键的问题.
 */
void matrix_init_and_scan_once_for_wakeup(void)
{
    scan_for_wakeup = true;
    row_for_wakeup = 0;
    cols_for_wakeup = 0;
    matrix_init();
    matrix_scan();

    if (cols_for_wakeup == 0)
        scan_for_wakeup = false;
}

/**
 * @brief 初始化键盘阵列
 * 
 */
void matrix_init(void) 
{
    for (uint_fast8_t i = MATRIX_COLS; i--;) {
        nrf_gpio_cfg_input((uint32_t)col_pin_array[i], NRF_GPIO_PIN_PULLDOWN);
    }

    for (uint_fast8_t i = MATRIX_ROWS; i--;) {
        nrf_gpio_cfg(
            (uint32_t)row_pin_array[i],
            NRF_GPIO_PIN_DIR_OUTPUT,
            NRF_GPIO_PIN_INPUT_DISCONNECT,
            NRF_GPIO_PIN_PULLDOWN,
            NRF_GPIO_PIN_D0S1,
            NRF_GPIO_PIN_NOSENSE);

        //Set pin to low
        nrf_gpio_pin_clear((uint32_t)row_pin_array[i]);
    }
}

/** read all cols */
static matrix_row_t read_cols(void)
{
    matrix_row_t result = 0;

    for (uint_fast8_t c = 0; c < MATRIX_COLS; c++) {
        if (READ_COL((uint32_t)col_pin_array[c]))
            result |= 1 << c;
    }

    return result;
}

static void select_row(uint8_t row)
{
    nrf_gpio_pin_write((uint32_t)row_pin_array[row], 1);
}

static void unselect_rows(void)
{
    for (uint_fast8_t i = 0; i < MATRIX_ROWS; i++) {
        nrf_gpio_pin_write((uint32_t)row_pin_array[i], 0);
    }
}

static inline void delay_us(void)
{
#ifdef __GNUC__
#define __nop() __asm("NOP")
#endif

#ifndef MATRIX_SCAN_DELAY_CYCLE
#define MATRIX_SCAN_DELAY_CYCLE 36
#endif
    for (int i = 0; i < MATRIX_SCAN_DELAY_CYCLE; i++) {
        __nop(); //64mhz, 64cycle = 1us
    }
}

uint8_t matrix_scan(void)
{
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
#ifdef NRF_LOG_ENABLED
        NRF_LOG_INFO("matrix_scan row=%d", i);
#endif
        select_row(i);
// #ifdef HYBRID_MATRIX
        // init_rows();
// #endif
        delay_us(); // wait stable
        matrix_row_t cols = read_cols();
#ifdef NRF_LOG_ENABLED
        if (cols != 0) {
            NRF_LOG_INFO("read_rows row=%d, cols=%d", i, cols);
        }
#endif

        if (matrix_debouncing[i] != cols) {
            matrix_debouncing[i] = cols;
            if (debouncing) {
                // dprint("bounce!: ");
                // debug_hex(debouncing);
                // dprint("\n");
            }
            // 扫描唤醒按键时只消抖1次
            debouncing = scan_for_wakeup ? 1 : DEBOUNCE_RELOAD;
        }
        unselect_rows();
    }

    if (debouncing) {
        if (--debouncing) {
            // no need to delay here manually, because we use the clock.
            keyboard_debounce();
        } else {
            for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
                matrix[i] = matrix_debouncing[i];
            }

            if (scan_for_wakeup) {
                for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
                    if (matrix[i]) {
                        row_for_wakeup = i;
                        cols_for_wakeup = matrix[i];
                        break;
                    }
                }
            }
        }
    }

    return 1;
}

bool matrix_is_modified(void)
{
    if (debouncing)
        return false;
    return true;
}

// 外部按键
#ifdef MATRIX_FORIGN_KEY
// static bool matrix_oneshot_send[MATRIX_ROWS];
// static matrix_row_t matrix_forign_oneshot[MATRIX_ROWS];
// static matrix_row_t matrix_forign[MATRIX_ROWS];

// /**
//  * @brief 为matrix添加一个按键按下事件（会自动清空）
//  * 
//  * @param row 行号
//  * @param col 列号
//  */
// void matrix_forign_add_oneshot(uint8_t row, uint8_t col)
// {
//     if (row >= MATRIX_ROWS)
//         return;
//     if (col >= sizeof(matrix_row_t) * 8)
//         return;

//     matrix_forign_oneshot[row] |= (1 << col);
// }

// /**
//  * @brief 为matrix添加一个按键事件
//  * 
//  * @param row 行号
//  * @param col 列号
//  * @param press 按下
//  */
// void matrix_forign_set(uint8_t row, uint8_t col, bool press)
// {
//     if (row >= MATRIX_ROWS)
//         return;
//     if (col >= sizeof(matrix_row_t) * 8)
//         return;
//     if (press)
//         matrix_forign[row] |= (1 << col);
//     else
//         matrix_forign[row] &= ~(1 << col);
// }

// inline matrix_row_t matrix_get_row(uint8_t row)
// {
//     matrix_row_t val = matrix[row] | matrix_forign[row];

//     // 发送一次后在下次发送空包，防止多次连击
//     if (matrix_oneshot_send[row]) {
//         val |= matrix_forign_oneshot[row];
//         matrix_forign_oneshot[row] = 0; // 清空单个按键
//     }
//     matrix_oneshot_send[row] = !matrix_oneshot_send[row];
//     return val;
// }
#else
inline matrix_row_t matrix_get_row(uint8_t row)
{
    static uint16_t n = 0;
    if (row == row_for_wakeup && scan_for_wakeup && keyboard_connected && (++n % 70) == 0)
    {
        // 唤醒按键只发送一次
        scan_for_wakeup = false;

#ifdef NRF_LOG_ENABLED
        NRF_LOG_INFO("matrix_get_row for wakeup row=%d col=%05x", row, cols_for_wakeup);
#endif

        return cols_for_wakeup;
    }

#ifdef NRF_LOG_ENABLED
    if (matrix[row]) {
        uint32_t tt = matrix[row];
        uint8_t nn = -1;
        while(tt) {
            nn++;
            tt >>= 1;
        }

        NRF_LOG_INFO("matrix_get_row row=%d col=%d", row, nn);
    }
#endif

    return matrix[row];
}
#endif

uint8_t matrix_key_count(void)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        count += bitpop16(matrix[i]);
    }
    return count;
}

/**
 * @brief 禁用所有阵列针脚
 * 
 */
void matrix_deinit(void)
{
    for (uint8_t i = 0; i < MATRIX_COLS; i++) {
        nrf_gpio_cfg_default(col_pin_array[i]);
    }
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        nrf_gpio_cfg_default(row_pin_array[i]);
    }
}

/**
 * @brief 阵列准备睡眠
 * 
 */
void matrix_wakeup_prepare(void)
{
// 这里监听所有按键作为唤醒按键，所以真正的唤醒判断应该在main的初始化过程中
    for (uint8_t i = 0; i < MATRIX_COLS; i++) {
        nrf_gpio_cfg_output(col_pin_array[i]);
        nrf_gpio_pin_set(col_pin_array[i]);
    }

    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        nrf_gpio_cfg_sense_input(row_pin_array[i], NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
    }
}

static void keyboard_matrix_evt_handler(enum user_event event, void* arg)
{
    uint8_t arg2 = (uint32_t)arg;
    switch (event) {
    case USER_EVT_USB: // USB事件
        keyboard_connected |= (arg2 == USB_WORKING);
        break;
    case USER_EVT_BLE_STATE_CHANGE: // 蓝牙状态事件
        keyboard_connected |= (arg2 == BLE_STATE_CONNECTED);
        break;
    default:
        break;
    }
}

EVENT_HANDLER(keyboard_matrix_evt_handler);
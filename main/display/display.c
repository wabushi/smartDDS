#include "display.h"
#include "display_ui.h"
#include "display_hw.h"
#include "splash_bitmap.h"
#include "config/board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>

static SemaphoreHandle_t s_lock; static display_output_t s_output; static bool s_rotate=true; static uint32_t s_interval=10; static TaskHandle_t s_task;
static TickType_t s_test_until;
#define WAVEFORM_ANIMATION_STEP_MS 10U
#define DISPLAY_NOTIFY_REDRAW      (1UL << 0)

static void request_render(void)
{
    if (s_task != NULL) {
        /* Use a notification bit instead of a counting semaphore.  This
         * makes a state change an explicit redraw event and guarantees that
         * it wakes the display task even while the task is waiting for the
         * next animation frame. */
        (void)xTaskNotify(s_task, DISPLAY_NOTIFY_REDRAW, eSetBits);
    }
}

static void render_current_state(display_output_t *rendered_output,
                                 uint32_t *animation_offset,
                                 bool *have_rendered_output,
                                 TickType_t *next_animation)
{
    *animation_offset = 0;
    *rendered_output = display_get_output();
    display_ui_render(*rendered_output);
    *have_rendered_output = true;
    *next_animation = xTaskGetTickCount() + pdMS_TO_TICKS(WAVEFORM_ANIMATION_STEP_MS);
    ESP_LOGI("DISPLAY", "Immediate redraw: output=%d", (int)*rendered_output + 1);
}

static void task(void *arg)
{
    (void)arg;
    const TickType_t animation_period = pdMS_TO_TICKS(WAVEFORM_ANIMATION_STEP_MS);
    TickType_t now = xTaskGetTickCount();
    TickType_t next_animation = now + animation_period;
    TickType_t last_rotation = now;
    uint32_t animation_offset = 0;
    display_output_t rendered_output = DISPLAY_SINE1;
    bool have_rendered_output = false;
    while (1) {
        now = xTaskGetTickCount();
        if (s_test_until != 0 && now < s_test_until) {
            /* Do not block through the splash period: retain redraw events
             * and render the latest state as soon as the splash expires. */
            uint32_t splash_events = 0;
            (void)xTaskNotifyWait(0, UINT32_MAX, &splash_events, s_test_until - now);
            continue;
        }
        if (s_test_until != 0) {
            s_test_until = 0;
            last_rotation = xTaskGetTickCount();
            render_current_state(&rendered_output, &animation_offset,
                                 &have_rendered_output, &next_animation);
        }

        uint32_t events = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &events, 0) == pdTRUE &&
            (events & DISPLAY_NOTIFY_REDRAW) != 0) {
            render_current_state(&rendered_output, &animation_offset,
                                 &have_rendered_output, &next_animation);
            continue;
        }

        /* A rotation or an explicit output selection changes the text and
         * attributes as well as the waveform.  A partial animation frame
         * cannot update those fields, so force a complete frame whenever
         * the selected physical output changes. */
        display_output_t current_output = display_get_output();
        if (!have_rendered_output || current_output != rendered_output) {
            rendered_output = current_output;
            animation_offset = 0;
            display_ui_render(rendered_output);
            have_rendered_output = true;
            now = xTaskGetTickCount();
            next_animation = now + animation_period;
        }
        if ((int32_t)(now - next_animation) >= 0) {
            /* Wrap at the active bitmap's actual width in draw_waveform().
             * This keeps both the 32-column sine/square and 42-column
             * triangle animations periodic, without a forced short wrap. */
            animation_offset++;
            display_ui_render_animated(rendered_output, animation_offset);
            /* The OLED transfer itself may consume part of the animation
             * period. Schedule from the completed frame and never catch up
             * by advancing multiple columns in one iteration. */
            now = xTaskGetTickCount();
            next_animation = now + animation_period;
        }
        if (s_rotate && (now - last_rotation) >= pdMS_TO_TICKS(s_interval * 1000U)) {
            last_rotation = now;
            display_next();
            animation_offset = 0;
        }
        TickType_t wait = animation_period;
        if ((int32_t)(next_animation - now) > 0) {
            wait = next_animation - now;
        }
        uint32_t events_after_wait = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &events_after_wait, wait) == pdTRUE &&
            (events_after_wait & DISPLAY_NOTIFY_REDRAW) != 0) {
            render_current_state(&rendered_output, &animation_offset,
                                 &have_rendered_output, &next_animation);
        }
    }
}
esp_err_t display_init(void)
{
    s_lock=xSemaphoreCreateMutex(); if(!s_lock) return ESP_ERR_NO_MEM;
    esp_err_t hw_err = display_hw_init();
    if (hw_err == ESP_OK) {
        s_test_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        esp_err_t splash_err = display_hw_show_bitmap(splash_bitmap);
        if (splash_err != ESP_OK) {
            ESP_LOGW("DISPLAY", "Splash bitmap failed: %s", esp_err_to_name(splash_err));
        } else {
            ESP_LOGI("DISPLAY", "Displayed SPLASH.png boot bitmap; normal UI starts in 3 seconds");
        }
    } else {
        ESP_LOGW("DISPLAY", "OLED unavailable: %s; continuing with log-backed UI", esp_err_to_name(hw_err));
    }
    return xTaskCreate(task,"display",8192,NULL,2,&s_task)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
void display_next(void){xSemaphoreTake(s_lock,portMAX_DELAY);s_output=(display_output_t)((s_output+1)%3);xSemaphoreGive(s_lock);request_render();}
void display_select(display_output_t o){if(o>DISPLAY_SQUARE)return;xSemaphoreTake(s_lock,portMAX_DELAY);s_output=o;xSemaphoreGive(s_lock);request_render();}
void display_set_rotation(bool e){xSemaphoreTake(s_lock,portMAX_DELAY);s_rotate=e;xSemaphoreGive(s_lock);}
bool display_rotation_enabled(void){bool e;xSemaphoreTake(s_lock,portMAX_DELAY);e=s_rotate;xSemaphoreGive(s_lock);return e;}
esp_err_t display_set_interval(uint32_t sec){if(sec<1||sec>3600)return ESP_ERR_INVALID_ARG;xSemaphoreTake(s_lock,portMAX_DELAY);s_interval=sec;xSemaphoreGive(s_lock);return ESP_OK;}
uint32_t display_get_interval(void){uint32_t x;xSemaphoreTake(s_lock,portMAX_DELAY);x=s_interval;xSemaphoreGive(s_lock);return x;}
display_output_t display_get_output(void){display_output_t x;xSemaphoreTake(s_lock,portMAX_DELAY);x=s_output;xSemaphoreGive(s_lock);return x;}
int display_status(char *b,size_t n){return snprintf(b,n,"Display hardware: %s\nCurrent output: %s\nRotation: %s\nInterval: %lu s\n",display_hw_present()?"detected (SSD1306-compatible 72x40)":"not detected",display_get_output()==DISPLAY_SINE1?"sine1":display_get_output()==DISPLAY_SINE2?"sine2":"square",display_rotation_enabled()?"on":"off",(unsigned long)display_get_interval());}
void display_notify_state_change(void){request_render();}

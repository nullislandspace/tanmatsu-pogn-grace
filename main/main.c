#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bsp/audio.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "bounce_sounds.h"
#include "hershey_font.h"

// External BSP audio function (not in public header)
extern void bsp_audio_initialize(uint32_t rate);

// ── Constants ────────────────────────────────────────────────────────────────

static char const TAG[] = "pogn";

#define PADDLE_WIDTH    10
#define PADDLE_HEIGHT   80
#define PADDLE_MARGIN   30
#define BALL_SIZE       10
#define BALL_INITIAL_SPEED 3.0f
#define PADDLE_SPEED    6.0f
#define AI_SPEED        4.0f
#define WIN_SCORE       11
#define GAME_TICK_MS    16

// Audio
#define MAX_ACTIVE_SOUNDS 4
#define FRAMES_PER_WRITE  64
#define SAMPLE_RATE       44100

// ── Types ────────────────────────────────────────────────────────────────────

typedef enum {
    STATE_TITLE,
    STATE_PLAYING,
    STATE_SCORED,
    STATE_GAME_OVER,
} game_state_e;

typedef struct {
    float ball_x, ball_y;
    float ball_vx, ball_vy;
    float player_y, ai_y;
    int player_score, ai_score;
    bool key_up_held, key_down_held;
    game_state_e state;
    int state_timer;
    bool player_scored_last;
} game_state_t;

typedef struct {
    const int16_t* sample_data;
    uint32_t sample_length;
    uint32_t playback_position;
    bool active;
    float volume;
} active_sound_t;

// ── Globals ──────────────────────────────────────────────────────────────────

static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};
static QueueHandle_t                input_event_queue    = NULL;

static i2s_chan_handle_t i2s_handle = NULL;
static active_sound_t    active_sounds[MAX_ACTIVE_SOUNDS];
static volatile bool     sound_trigger[NUM_SOUNDS] = {false};

#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define GRAY  0xFF808080

// ── Display ──────────────────────────────────────────────────────────────────

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

// ── Audio ────────────────────────────────────────────────────────────────────

static void play_sound(int sound_id) {
    if (sound_id >= 0 && sound_id < NUM_SOUNDS) {
        sound_trigger[sound_id] = true;
    }
}

static void audio_task(void* arg) {
    int16_t output_buffer[FRAMES_PER_WRITE * 2];

    while (1) {
        // Check for triggered sounds
        for (int i = 0; i < NUM_SOUNDS; i++) {
            if (sound_trigger[i]) {
                sound_trigger[i] = false;
                // Find a free slot (or reuse one playing the same sound)
                int slot = -1;
                for (int j = 0; j < MAX_ACTIVE_SOUNDS; j++) {
                    if (!active_sounds[j].active) {
                        slot = j;
                        break;
                    }
                }
                if (slot < 0) slot = 0; // Steal oldest slot
                active_sounds[slot].sample_data = bounce_samples[i];
                active_sounds[slot].sample_length = bounce_lengths[i];
                active_sounds[slot].playback_position = 0;
                active_sounds[slot].volume = 0.3f;
                active_sounds[slot].active = true;
            }
        }

        // Mix active sounds
        for (int f = 0; f < FRAMES_PER_WRITE; f++) {
            float mix = 0.0f;

            for (int i = 0; i < MAX_ACTIVE_SOUNDS; i++) {
                if (!active_sounds[i].active) continue;
                int16_t sample = active_sounds[i].sample_data[active_sounds[i].playback_position];
                mix += (sample / 32768.0f) * active_sounds[i].volume;
                active_sounds[i].playback_position++;
                if (active_sounds[i].playback_position >= active_sounds[i].sample_length) {
                    active_sounds[i].active = false;
                }
            }

            // Soft clip
            mix = fminf(1.0f, fmaxf(-1.0f, mix));
            int16_t out = (int16_t)(mix * 32767.0f);
            output_buffer[f * 2] = out;
            output_buffer[f * 2 + 1] = out;
        }

        if (i2s_handle != NULL) {
            size_t bytes_written;
            i2s_channel_write(i2s_handle, output_buffer, sizeof(output_buffer), &bytes_written, portMAX_DELAY);
        }
    }
}

// ── Game Logic ───────────────────────────────────────────────────────────────

static int screen_w, screen_h;

static void reset_ball(game_state_t* g, int direction) {
    g->ball_x = screen_w / 2.0f - BALL_SIZE / 2.0f;
    g->ball_y = screen_h / 2.0f - BALL_SIZE / 2.0f;
    g->ball_vx = BALL_INITIAL_SPEED * direction;
    g->ball_vy = ((float)(rand() % 100) / 100.0f - 0.5f) * BALL_INITIAL_SPEED;
}

static void reset_game(game_state_t* g) {
    g->player_score = 0;
    g->ai_score = 0;
    g->player_y = screen_h / 2.0f - PADDLE_HEIGHT / 2.0f;
    g->ai_y = screen_h / 2.0f - PADDLE_HEIGHT / 2.0f;
    g->key_up_held = false;
    g->key_down_held = false;
    reset_ball(g, 1);
}

static void update_game(game_state_t* g) {
    if (g->state == STATE_TITLE || g->state == STATE_GAME_OVER) {
        return;
    }

    if (g->state == STATE_SCORED) {
        g->state_timer--;
        if (g->state_timer <= 0) {
            if (g->player_score >= WIN_SCORE || g->ai_score >= WIN_SCORE) {
                g->state = STATE_GAME_OVER;
                g->state_timer = 0;
            } else {
                g->state = STATE_PLAYING;
            }
        }
        return;
    }

    // Player paddle movement
    if (g->key_up_held) {
        g->player_y -= PADDLE_SPEED;
    }
    if (g->key_down_held) {
        g->player_y += PADDLE_SPEED;
    }
    // Clamp player paddle
    if (g->player_y < 0) g->player_y = 0;
    if (g->player_y > screen_h - PADDLE_HEIGHT) g->player_y = screen_h - PADDLE_HEIGHT;

    // AI paddle movement (left side) - only track when ball moves toward AI
    if (g->ball_vx < 0) {
        float ball_center = g->ball_y + BALL_SIZE / 2.0f;
        float ai_center = g->ai_y + PADDLE_HEIGHT / 2.0f;
        float diff = ball_center - ai_center;

        // Only react when ball is past center
        if (g->ball_x < screen_w / 2.0f) {
            if (diff > AI_SPEED) {
                g->ai_y += AI_SPEED;
            } else if (diff < -AI_SPEED) {
                g->ai_y -= AI_SPEED;
            }
        }
    }
    // Clamp AI paddle
    if (g->ai_y < 0) g->ai_y = 0;
    if (g->ai_y > screen_h - PADDLE_HEIGHT) g->ai_y = screen_h - PADDLE_HEIGHT;

    // Move ball
    g->ball_x += g->ball_vx;
    g->ball_y += g->ball_vy;

    // Wall bounce (top/bottom)
    if (g->ball_y < 0) {
        g->ball_y = -g->ball_y;
        g->ball_vy = -g->ball_vy;
        play_sound(SOUND_WALL_BOUNCE);
    }
    if (g->ball_y > screen_h - BALL_SIZE) {
        g->ball_y = 2 * (screen_h - BALL_SIZE) - g->ball_y;
        g->ball_vy = -g->ball_vy;
        play_sound(SOUND_WALL_BOUNCE);
    }

    // AI paddle collision (left side)
    float ai_paddle_right = PADDLE_MARGIN + PADDLE_WIDTH;
    if (g->ball_vx < 0 &&
        g->ball_x <= ai_paddle_right &&
        g->ball_x + BALL_SIZE >= PADDLE_MARGIN &&
        g->ball_y + BALL_SIZE >= g->ai_y &&
        g->ball_y <= g->ai_y + PADDLE_HEIGHT) {

        g->ball_x = ai_paddle_right;
        float paddle_center = g->ai_y + PADDLE_HEIGHT / 2.0f;
        float ball_center = g->ball_y + BALL_SIZE / 2.0f;
        float offset = (ball_center - paddle_center) / (PADDLE_HEIGHT / 2.0f);
        float speed = fabsf(g->ball_vx) * 1.05f;
        g->ball_vx = speed;
        g->ball_vy = offset * BALL_INITIAL_SPEED * 2.0f;
        play_sound(SOUND_PADDLE_HIT);
    }

    // Player paddle collision (right side)
    float player_paddle_left = screen_w - PADDLE_MARGIN - PADDLE_WIDTH;
    if (g->ball_vx > 0 &&
        g->ball_x + BALL_SIZE >= player_paddle_left &&
        g->ball_x <= screen_w - PADDLE_MARGIN &&
        g->ball_y + BALL_SIZE >= g->player_y &&
        g->ball_y <= g->player_y + PADDLE_HEIGHT) {

        g->ball_x = player_paddle_left - BALL_SIZE;
        float paddle_center = g->player_y + PADDLE_HEIGHT / 2.0f;
        float ball_center = g->ball_y + BALL_SIZE / 2.0f;
        float offset = (ball_center - paddle_center) / (PADDLE_HEIGHT / 2.0f);
        float speed = fabsf(g->ball_vx) * 1.05f;
        g->ball_vx = -speed;
        g->ball_vy = offset * BALL_INITIAL_SPEED * 2.0f;
        play_sound(SOUND_PADDLE_HIT);
    }

    // Scoring: ball exits left → player scores
    if (g->ball_x + BALL_SIZE < 0) {
        g->player_score++;
        g->player_scored_last = true;
        play_sound(SOUND_SCORE);
        reset_ball(g, 1);
        g->state = STATE_SCORED;
        g->state_timer = 120; // ~2 seconds
    }

    // Scoring: ball exits right → AI scores
    if (g->ball_x > screen_w) {
        g->ai_score++;
        g->player_scored_last = false;
        play_sound(SOUND_SCORE);
        reset_ball(g, -1);
        g->state = STATE_SCORED;
        g->state_timer = 120;
    }
}

// ── Rendering ────────────────────────────────────────────────────────────────

static void draw_centered_text(const char* text, float font_height, int y) {
    uint8_t* pixels = (uint8_t*)pax_buf_get_pixels(&fb);
    int raw_w = display_h_res;  // Raw buffer dimensions (before rotation)
    int raw_h = display_v_res;
    int w = hershey_string_width(text, font_height);
    int x = (screen_w - w) / 2;
    hershey_draw_string(pixels, raw_w, raw_h, x, y, text, font_height, 255, 255, 255);
}

static void render_game(game_state_t* g) {
    pax_background(&fb, BLACK);

    // Draw center dashed line
    for (int y = 0; y < screen_h; y += 20) {
        pax_simple_rect(&fb, GRAY, screen_w / 2 - 1, y, 2, 10);
    }

    if (g->state == STATE_TITLE) {
        draw_centered_text("POGN", 80, 120);
        draw_centered_text("UP/DOWN to move paddle", 24, 260);
        draw_centered_text("First to 11 wins", 24, 300);
        draw_centered_text("Press any key to start", 24, 360);
        blit();
        return;
    }

    // Draw paddles (AI on left, player on right)
    pax_simple_rect(&fb, WHITE, PADDLE_MARGIN, g->ai_y, PADDLE_WIDTH, PADDLE_HEIGHT);
    pax_simple_rect(&fb, WHITE, screen_w - PADDLE_MARGIN - PADDLE_WIDTH, g->player_y, PADDLE_WIDTH, PADDLE_HEIGHT);

    // Draw ball
    pax_simple_rect(&fb, WHITE, g->ball_x, g->ball_y, BALL_SIZE, BALL_SIZE);

    // Draw scores using Hershey font
    char score_text[8];
    uint8_t* pixels = (uint8_t*)pax_buf_get_pixels(&fb);
    int raw_w = display_h_res;
    int raw_h = display_v_res;

    snprintf(score_text, sizeof(score_text), "%d", g->ai_score);
    int sw = hershey_string_width(score_text, 60);
    hershey_draw_string(pixels, raw_w, raw_h, screen_w / 4 - sw / 2, 20, score_text, 60, 255, 255, 255);

    snprintf(score_text, sizeof(score_text), "%d", g->player_score);
    sw = hershey_string_width(score_text, 60);
    hershey_draw_string(pixels, raw_w, raw_h, 3 * screen_w / 4 - sw / 2, 20, score_text, 60, 255, 255, 255);

    // State-specific overlays
    if (g->state == STATE_SCORED) {
        if (g->player_scored_last) {
            draw_centered_text("PLAYER SCORES!", 40, 200);
        } else {
            draw_centered_text("AI SCORES!", 40, 200);
        }
    }

    if (g->state == STATE_GAME_OVER) {
        if (g->player_score >= WIN_SCORE) {
            draw_centered_text("YOU WIN!", 60, 160);
        } else {
            draw_centered_text("YOU LOSE!", 60, 160);
        }
        draw_centered_text("Press any key to restart", 24, 280);
    }

    blit();
}

// ── Input Handling ───────────────────────────────────────────────────────────

static void handle_input(bsp_input_event_t* event, game_state_t* g) {
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION) {
        // ESC or F1 → back to launcher
        if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC ||
            event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
            if (event->args_navigation.state) {
                bsp_device_restart_to_launcher();
            }
            return;
        }

        // Up/Down tracking
        if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_UP) {
            g->key_up_held = event->args_navigation.state;
        }
        if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
            g->key_down_held = event->args_navigation.state;
        }

        // State transitions on press
        if (event->args_navigation.state) {
            if (g->state == STATE_TITLE) {
                g->state = STATE_PLAYING;
                reset_game(g);
            } else if (g->state == STATE_GAME_OVER) {
                g->state = STATE_TITLE;
            }
        }
    }

    // Also allow keyboard to trigger state transitions
    if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        if (g->state == STATE_TITLE) {
            g->state = STATE_PLAYING;
            reset_game(g);
        } else if (g->state == STATE_GAME_OVER) {
            g->state = STATE_TITLE;
        }
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────

void app_main(void) {
    gpio_install_isr_service(0);

    // Initialize NVS
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        return;
    }

    // Initialize BSP
    const bsp_configuration_t bsp_configuration = {
        .display = {
            .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
            .num_fbs = 1,
        },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }

    // Get display parameters
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Display params failed: %d", res);
        return;
    }

    // Setup PAX buffer
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565: format = PAX_BUF_16_565RGB; break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888: format = PAX_BUF_24_888RGB; break;
        default: break;
    }

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CCW;  break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW;   break;
        default: break;
    }

    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    screen_w = pax_buf_get_width(&fb);
    screen_h = pax_buf_get_height(&fb);

    // Get input queue
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Initialize audio
    bsp_audio_initialize(SAMPLE_RATE);
    bsp_audio_get_i2s_handle(&i2s_handle);
    bsp_audio_set_amplifier(true);
    bsp_audio_set_volume(100);

    memset(active_sounds, 0, sizeof(active_sounds));
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);

    // Initialize game
    game_state_t game = {0};
    game.state = STATE_TITLE;
    game.player_y = screen_h / 2.0f - PADDLE_HEIGHT / 2.0f;
    game.ai_y = screen_h / 2.0f - PADDLE_HEIGHT / 2.0f;
    reset_ball(&game, 1);

    // Show title
    render_game(&game);

    // Main loop
    while (1) {
        bsp_input_event_t event;
        bool got_event = xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(GAME_TICK_MS));

        if (got_event) {
            handle_input(&event, &game);
        }

        update_game(&game);
        render_game(&game);
    }
}

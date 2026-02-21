#include <libndls.h>
#include <SDL/SDL.h>

#include "gfx_window_manager_api.h"
#include "gfx_backend.h"
#include "macros.h"
#include "nspireio.h"

#include "pc/configfile.h"
#include "pc/timer.h"
#include "pc/profiling.h"

#include <string.h>

#define HALF_WIDTH SCREEN_WIDTH / 2
#define HALF_HEIGHT SCREEN_HEIGHT / 2

static uint32_t frames_now = 0;
static uint32_t frames_prev = 0;

static bool skip_frame = false;

void nsp_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {
    if (configOverclock) {
        set_cpu_speed(CPU_SPEED_150MHZ);
    }
    lcd_init(SCR_320x240_565);
}

void nsp_main_loop(void (*run_one_game_iter)(void)) {
    nio_console *console;
    if (!nio_init(console, NIO_MAX_COLS, NIO_MAX_ROWS, 0, 0, NIO_COLOR_WHITE, NIO_COLOR_BLACK, true))
        abort();
    nio_set_default(console);

    tmr_init();

    // timer sanity check
    uint32_t t0 = tmr_ms();
    nio_printf("ms0 (should be zero): %lu\n", t0);
    msleep(1000);
    uint64_t t1 = tmr_ms();
    nio_printf("ms delta (should be ~1000): %llu\n\n", t1 - t0);

    // info
    nio_puts("DEFAULT CONTROLS:\n"
             "Start buttong: Enter\n"
             "Analog stick: Touchpad\n"
             "A button: menu\n"
             "B button: del\n"
             "Z trigger: doc\n"
             "C buttons (up, right down, left): 8, 6, 2, 4, respectively\n\n");

    nio_puts("Press ESC to exit, and CTRL for profiling\n");

    nio_printf("Frameskip config: %u\nPress any key to continue...\n", configFrameskip);

    wait_key_pressed();
    nio_free(console);

    tmr_reset();
    tmr_start();
    while (true) {
        frames_now = tmr_ms() / 33; // one frame every 33 milliseconds -> 30 fps
        uint32_t new_frames = frames_now - frames_prev;

        if (new_frames) {
            // printf("ideal: %lu\ncurrent: %lu\n", frames_now, frames_prev); // PRINTING TOO MUCH ON
            // HARDWARE CAUSES EXTREME TEARING

            int to_skip = (new_frames > configFrameskip)
                              ? configFrameskip
                              : (new_frames - 1); // catch up by skipping up to configFrameskip frames

            uint32_t t0 = tmr_ms();

            // printf("Rendering: %lu\nSkipping: %lu\n", new_frames, to_skip);
            for (int i = 0; i < to_skip + 1; ++i) { // render only one frame out of this chunk
                // printf("skipping: %ld\n", to_skip);
                skip_frame = i < to_skip;
                run_one_game_iter();

                if (isKeyPressed(KEY_NSPIRE_ESC)) {
                    return;
                }
            }
            // printf("tFRAME: %lu\n", tmr_ms() - t0);

            // Status screen / debug
            if (isKeyPressed(KEY_NSPIRE_CTRL)) {
                tmr_stop();
                if (!nio_init(console, NIO_MAX_COLS, NIO_MAX_ROWS, 0, 0, NIO_COLOR_WHITE,
                              NIO_COLOR_BLACK, true))
                    abort();

                uint32_t tDelta = tmr_ms() - t0;
                float fps = 1000.f / tDelta;

                nio_printf("Total elapsed (ms): %lu\n"
                           "Backend Gfx time: %d\n"
                           "Front + Backend Gfx time: %d\n"
                           "Total frame time (ms): %lu\n"
                           "FPS, physical: %f\n"
                           "FPS, virtual: %f\n"
                           "^ This includes frames skipped\n"
                           "Tris this frame: %d\n"
                           "Frames skipped: %d\n",
                           tmr_ms(), tFlushing, tFullRender, tDelta, fps, fps * (to_skip + 1), numTris,
                           to_skip);

                wait_key_pressed();
                nio_free(console);
                tmr_start();
            }

        } // mostly borrowed from dos implementation

        frames_prev = frames_now;
    }
}

void nsp_get_dimensions(uint32_t *width, uint32_t *height) {
    if (config80pMode) { // 1/16th the pixel resolution (80x60)
        *width = SCREEN_WIDTH / 4;
        *height = SCREEN_HEIGHT / 4;
    } else if (config120pMode) { // quarter the pixel resolution (160x120)
        *width = HALF_WIDTH;
        *height = HALF_HEIGHT;
    } else {
        *width = SCREEN_WIDTH;
        *height = SCREEN_HEIGHT;
    }
}

bool nsp_start_frame(void) {
    return !skip_frame; // (current_frame % 4) == 0; //
}

static inline uint16_t
c4444_to_c565(uint32_t c) { // aaaaaaaa bbbbbbbb gggggggg rrrrrrrr -> rrrrr gggggg bbbbb
    return ((c & 0b11111000) << 8) | ((c & 0b1111110000000000) >> 5) | ((c >> 19) & 0b11111);
}

void nsp_swap_buffers_begin(void) {
}

void nsp_swap_buffers_end(void) {
    static uint16_t buffer[SCREEN_WIDTH * SCREEN_HEIGHT];

    if (config80pMode) {
        // 80x60 -> 320x240: each pixel becomes a 4x4 block
        const int src_w = SCREEN_WIDTH / 4; // 80
        int img_pix = 0;
        for (int row = 0; row < SCREEN_HEIGHT / 4; ++row) {
            int base = row * 4 * SCREEN_WIDTH;
            for (int col = 0; col < src_w; ++col, ++img_pix) {
                uint16_t c16 = c4444_to_c565(gfx_output[img_pix]);
                // Pack two identical pixels into a 32-bit word
                uint32_t c32 = ((uint32_t) c16 << 16) | c16;
                int x = col * 4;
                uint32_t *row0 = (uint32_t *) &buffer[base + x];
                uint32_t *row1 = (uint32_t *) &buffer[base + SCREEN_WIDTH + x];
                row0[0] = c32;
                row0[1] = c32;
                row1[0] = c32;
                row1[1] = c32;
            }
            // Copy row pair to the next two rows
            memcpy(&buffer[base + 2 * SCREEN_WIDTH], &buffer[base], SCREEN_WIDTH * 2);
            memcpy(&buffer[base + 3 * SCREEN_WIDTH], &buffer[base], SCREEN_WIDTH * 2);
        }
    } else if (config120pMode) {
        // 160x120 -> 320x240: optimized with 32-bit writes
        // Process two source pixels at a time, writing four 16-bit pixels as two 32-bit values
        const int src_w = HALF_WIDTH; // 160
        int img_pix = 0;
        for (int row = 0; row < SCREEN_HEIGHT / 2; ++row) {
            int base = row * 2 * SCREEN_WIDTH;
            for (int col = 0; col < src_w; col += 2, img_pix += 2) {
                uint16_t c16a = c4444_to_c565(gfx_output[img_pix]);
                uint16_t c16b = c4444_to_c565(gfx_output[img_pix + 1]);
                // Pack: [c16a, c16a] and [c16b, c16b] as 32-bit words
                uint32_t pair_a = ((uint32_t) c16a << 16) | c16a;
                uint32_t pair_b = ((uint32_t) c16b << 16) | c16b;
                int x = col * 2;
                uint32_t *dst = (uint32_t *) &buffer[base + x];
                dst[0] = pair_a;
                dst[1] = pair_b;
            }
            // Duplicate row: copy first expanded row to second
            memcpy(&buffer[base + SCREEN_WIDTH], &buffer[base], SCREEN_WIDTH * 2);
        }
    } else {
        for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
            buffer[i] = c4444_to_c565(gfx_output[i]);
        }
    }

    lcd_blit(buffer, SCR_320x240_565);
}

// unimplemented windowing features
void nsp_set_keyboard_callbacks(UNUSED bool (*on_key_down)(int scancode),
                                UNUSED bool (*on_key_up)(int scancode),
                                UNUSED void (*on_all_keys_up)(void)) {
}
void nsp_set_fullscreen_changed_callback(UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}
void nsp_set_fullscreen(UNUSED bool enable) {
}
void nsp_handle_events(void) {
}
double nsp_get_time(void) {
    return 0.0;
}

void nsp_shutdown(void) {
    lcd_init(SCR_TYPE_INVALID);
    // tmr_shutdown(); // BANDAID FIX: attempting to restore old timer soft locks calc. not required but
    // could be problematic?
}

struct GfxWindowManagerAPI gfx_nsp_api = { nsp_init,
                                           nsp_set_keyboard_callbacks,
                                           nsp_set_fullscreen_changed_callback,
                                           nsp_set_fullscreen,
                                           nsp_main_loop,
                                           nsp_get_dimensions,
                                           nsp_handle_events,
                                           nsp_start_frame,
                                           nsp_swap_buffers_begin,
                                           nsp_swap_buffers_end,
                                           nsp_get_time,
                                           nsp_shutdown };

// #endif
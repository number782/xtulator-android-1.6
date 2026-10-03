#ifndef _ANDROID_FRONTEND_H_
#define _ANDROID_FRONTEND_H_

#include <stdint.h>

void android_fb_get_dims(int *w, int *h);

int android_fb_copy(int32_t *out, int count);

int android_fb_get_dims_and_copy(int *w, int *h, int32_t *out, int count);

uint8_t android_keycode_to_scancode(int keycode);

int sdlconsole_init(char *title);
int sdlconsole_setWindow(int w, int h);
void sdlconsole_blit(uint32_t* pixels, int w, int h, int stride);
void sdlconsole_destroy(void);

#ifdef __cplusplus
extern "C" {
#endif
int android_copy_asset(void *env, void *assetManager, const char *assetName, const char *outPath);
#ifdef __cplusplus
}
#endif

#endif

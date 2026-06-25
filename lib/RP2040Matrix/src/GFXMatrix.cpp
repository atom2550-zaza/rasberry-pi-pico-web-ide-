#include "GFXMatrix.h"
#include <hub75.h>
extern "C" void log_to_web_c(const char* msg);

GFXMatrix::GFXMatrix(uint16_t w, uint16_t h): Adafruit_GFX(w,h) {
    buffer = static_cast<uint32_t*>(malloc(WIDTH*HEIGHT*sizeof(uint32_t)));
    overlay_buffer = static_cast<uint8_t*>(malloc(WIDTH*HEIGHT*sizeof(uint8_t)));
    memset(overlay_buffer, 0, WIDTH*HEIGHT*sizeof(uint8_t));
    memset(buffer, 0, WIDTH*HEIGHT*sizeof(uint32_t));
}

GFXMatrix::~GFXMatrix() {
    hub75_stop();
    if(buffer) free(buffer);
    if(overlay_buffer) free(overlay_buffer);
    buffer = nullptr;
    overlay_buffer = nullptr;
}

void GFXMatrix::begin() {
    hub75_config(8);
    hub75_set_masterbrightness(60);
}

void GFXMatrix::clear() {
    memset(buffer, 0, WIDTH*HEIGHT*sizeof(uint32_t));
}

void GFXMatrix::drawPixel(int16_t x, int16_t y, uint16_t color) {
    buffer[x + y*WIDTH] = LEDmx_565toRGB(color);
}

void GFXMatrix::display() {
    hub75_update((rgb_t*)buffer, overlay_buffer);
}
#ifndef GFXMATRIX_H
#define GFXMATRIX_H

#include <Adafruit_GFX.h>

///  An Adafruit GFX implementation for the Matrix
class GFXMatrix : public Adafruit_GFX {
public:
  GFXMatrix(uint16_t w, uint16_t h);
  ~GFXMatrix();
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void display();
  void begin();
  void clear();
private:
  static uint32_t LEDmx_565toRGB(uint16_t pix) {

    uint8_t r = (pix >> 11) << 3;
    uint8_t g = ((pix >> 5) & 0x3F) << 2;
    uint8_t b = (pix & 0x1F) << 3;
    // ลองสลับ r กับ g
    return ((uint32_t)g) | ((uint32_t)r << 8) | ((uint32_t)b << 16);
  }



  uint32_t *buffer = nullptr;
  uint8_t *overlay_buffer = nullptr;

};

#endif // GFXMATRIX_H
#pragma once
#include <cstdint>
#include <vector>
struct ImageRGBA { int width=0,height=0; std::vector<uint8_t> pixels; bool empty() const{return width<=0||height<=0||pixels.size()!=size_t(width)*height*4;} };
struct Mask { int width=0,height=0; std::vector<uint8_t> pixels; bool empty() const{return width<=0||height<=0||pixels.size()!=size_t(width)*height;} };

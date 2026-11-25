#pragma once

#include "Metal.hpp"
#include <cstdint>
#include <cstring>
#include <map>
#include <d3d9.h>

namespace dxmt {

// Fixed-function vertex shader key — state that changes VS code
struct FFVSKey {
  uint8_t has_position_t;       // POSITIONT vs POSITION
  uint8_t has_normal;
  uint8_t has_color0, has_color1;
  uint8_t tex_coord_count;      // 0-8
  uint8_t fog_mode;             // 0=NONE, 1=EXP, 2=EXP2, 3=LINEAR
  uint8_t lighting_enabled;
  uint8_t num_active_lights;
  uint8_t normalize_normals;
  uint8_t light_types[8];       // D3DLIGHTTYPE per active light
  uint8_t diffuse_source;       // D3DMATERIALCOLORSOURCE
  uint8_t ambient_source;
  uint8_t specular_source;
  uint8_t emissive_source;
  uint8_t color_vertex;
  uint8_t tci_modes[8] = {};  // TCI generation mode per texcoord output (0=passthru, 1=normal, 2=pos, 3=reflect)
  uint8_t tci_coord_indices[8] = {}; // low bits of TEXCOORDINDEX: which coord set to use for passthru
  uint8_t ttf_modes[8] = {}; // D3DTSS_TEXTURETRANSFORMFLAGS per texcoord (0=disable, 2=count2, 3=count3, etc.)

  bool operator<(const FFVSKey &rhs) const {
    return memcmp(this, &rhs, sizeof(FFVSKey)) < 0;
  }
  bool operator==(const FFVSKey &rhs) const {
    return memcmp(this, &rhs, sizeof(FFVSKey)) == 0;
  }
};

// Fixed-function pixel shader key — state that changes PS code
struct FFPSKey {
  struct Stage {
    uint8_t color_op, color_arg1, color_arg2;
    uint8_t alpha_op, alpha_arg1, alpha_arg2;
    uint8_t has_texture;
    uint8_t texcoord_index = 0;
    uint8_t color_arg0 = 1, alpha_arg0 = 1; // D3DTA_CURRENT
    uint8_t result_arg = 1;
    uint8_t pad = 0;
  };
  Stage stages[8] = {};
  uint8_t tex_coord_count = 0;
  uint8_t specular_enable = 0;
  uint8_t alpha_test_enable = 0, alpha_test_func = 0;
  uint8_t fog_enable = 0;
  // 0: FOG0 is the fog factor (FF VS). 1=EXP, 2=EXP2, 3=LINEAR: FOG0 is a fog *coordinate* written by a
  // shader-model < 3 vertex shader (oFog) with vertex fog enabled; D3D9 applies this formula to it.
  uint8_t fog_coord_mode = 0;

  bool operator<(const FFPSKey &rhs) const {
    return memcmp(this, &rhs, sizeof(FFPSKey)) < 0;
  }
  bool operator==(const FFPSKey &rhs) const {
    return memcmp(this, &rhs, sizeof(FFPSKey)) == 0;
  }
};

WMT::Reference<WMT::Function> GenerateFFVertexShader(
    WMT::Device device, const FFVSKey &key,
    const D3DVERTEXELEMENT9 *elements, uint32_t numElements, uint32_t slotMask,
    WMTMetalVersion metalVersion);

WMT::Reference<WMT::Function> GenerateFFPixelShader(
    WMT::Device device, const FFPSKey &key,
    uint8_t texcoord_count,
    WMTMetalVersion metalVersion);

} // namespace dxmt

#include "stddef.h"
#include "stdint.h"
#include "stdbool.h"

#ifndef __AIRCONV_H
#define __AIRCONV_H

#ifdef __cplusplus
#include <string>
enum class ShaderType {
  Vertex,
  /* Metal: fragment function */
  Pixel,
  /* Metal: kernel function */
  Compute,
  /* Not present in Metal */
  Hull,
  /* Metal: post-vertex function */
  Domain,
  /* Not present in Metal */
  Geometry,
  Mesh,
  /* Metal: object function */
  Amplification,
};

enum class SM50BindingType : uint32_t {
  ConstantBuffer,
  Sampler,
  SRV,
  UAV,
};
#else
typedef uint32_t ShaderType;
typedef uint32_t SM50BindingType;
#endif

enum MTL_SM50_SHADER_ARGUMENT_FLAG : uint32_t {
  MTL_SM50_SHADER_ARGUMENT_BUFFER = 1 << 0,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE = 1 << 1,
  MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH = 1 << 2,
  MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER = 1 << 3,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP = 1 << 4,
  MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET = 1 << 5,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY = 1 << 6,
  MTL_SM50_SHADER_ARGUMENT_READ_ACCESS = 1 << 10,
  MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS = 1 << 11,
};

struct MTL_SM50_SHADER_ARGUMENT {
  SM50BindingType Type;
  /**
  bind point of it's corresponding resource space
  constant buffer:    cb1 -> 1
  srv:                t10 -> 10
  uav:                u0  -> 0
  sampler:            s2  -> 2
  */
  uint32_t SM50BindingSlot;
  enum MTL_SM50_SHADER_ARGUMENT_FLAG Flags;
  uint32_t StructurePtrOffset;
};

enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE {
  MTL_TESSELLATOR_OUTPUT_POINT = 1,
  MTL_TESSELLATOR_OUTPUT_LINE = 2,
  MTL_TESSELLATOR_OUTPUT_TRIANGLE_CW = 3,
  MTL_TESSELLATOR_TRIANGLE_CCW = 4
};

struct MTL_TESSELLATOR_REFLECTION {
  uint32_t Partition;
  float MaxFactor;
  enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE OutputPrimitive;
};

struct MTL_GEOMETRY_SHADER_PASS_THROUGH {
  uint8_t RenderTargetArrayIndexReg;
  uint8_t RenderTargetArrayIndexComponent;
  uint8_t ViewportArrayIndexReg;
  uint8_t ViewportArrayIndexComponent;
};

struct MTL_GEOMETRY_SHADER_REFLECTION {
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t GSPassThrough;
  };
  uint32_t Primitive;
};

struct MTL_POST_TESSELLATOR_REFLECTION {
  uint32_t MaxPotentialTessFactor;
};

struct MTL_SHADER_REFLECTION {
  uint32_t ConstanttBufferTableBindIndex;
  uint32_t ArgumentBufferBindIndex;
  uint32_t NumConstantBuffers;
  uint32_t NumArguments;
  union {
    uint32_t ThreadgroupSize[3];
    struct MTL_TESSELLATOR_REFLECTION Tessellator;
    struct MTL_GEOMETRY_SHADER_REFLECTION GeometryShader;
    struct MTL_POST_TESSELLATOR_REFLECTION PostTessellator;
    uint32_t PSValidRenderTargets;
  };
  uint16_t ConstantBufferSlotMask;
  uint16_t SamplerSlotMask;
  uint64_t UAVSlotMask;
  uint64_t SRVSlotMaskHi;
  uint64_t SRVSlotMaskLo;
  uint32_t NumOutputElement;
  uint32_t ThreadsPerPatch;
  uint32_t ArgumentTableQwords;
};

#if defined(__LP64__) || defined(_WIN64)
typedef void *sm50_ptr64_t;
#else
typedef struct sm50_ptr64_t {
  uint64_t impl;

#ifdef __cplusplus
  sm50_ptr64_t() {
    impl = 0;
  }

  sm50_ptr64_t(void * ptr) {
    impl = (uint64_t)ptr;
  }

  sm50_ptr64_t(uint64_t v) {
    impl = v;
  }

  operator uint64_t () const {
    return impl;
  }
#endif

} sm50_ptr64_t;
#endif

typedef sm50_ptr64_t sm50_shader_t;
typedef sm50_ptr64_t sm50_bitcode_t;
typedef sm50_ptr64_t sm50_error_t;

#ifdef _WIN32
#ifdef WIN_EXPORT
#define AIRCONV_API __declspec(dllexport)
#else
#define AIRCONV_API __declspec(dllimport)
#endif
#else
#define AIRCONV_API __attribute__((sysv_abi))
#endif

struct SM50_COMPILED_BITCODE {
  sm50_ptr64_t Data;
  uint64_t Size;
};

#ifdef __cplusplus

inline uint32_t
GetArgumentIndex(SM50BindingType Type, uint32_t SM50BindingSlot) {
  switch (Type) {
  case SM50BindingType::ConstantBuffer:
    return SM50BindingSlot;
  case SM50BindingType::Sampler:
    return SM50BindingSlot + 32;
  case SM50BindingType::SRV:
    return SM50BindingSlot * 3 + 128;
  case SM50BindingType::UAV:
    return SM50BindingSlot * 3 + 512;
  }
};

inline uint32_t GetArgumentIndex(struct MTL_SM50_SHADER_ARGUMENT &Argument) {
  return GetArgumentIndex(Argument.Type, Argument.SM50BindingSlot);
};

extern "C" {
#endif

enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE {
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT = 1,
  SM50_SHADER_COMMON = 2,
  SM50_SHADER_PSO_PIXEL_SHADER = 3,
  SM50_SHADER_IA_INPUT_LAYOUT = 4,
  SM50_SHADER_GS_PASS_THROUGH = 5,
  SM50_SHADER_PSO_GEOMETRY_SHADER = 6,
  SM50_SHADER_PSO_TESSELLATOR = 7,
  SM50_SHADER_DXSO_ALPHA_TEST = 8,
  SM50_SHADER_DXSO_FOG = 9,
  SM50_SHADER_ARGUMENT_TYPE_MAX = 0xffffffff,
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_STREAM_OUTPUT_ELEMENT {
  uint32_t reg_id;
  uint32_t component;
  uint32_t output_slot;
  uint32_t offset;
};

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  struct SM50_STREAM_OUTPUT_ELEMENT *elements;
};

enum SM50_SHADER_METAL_VERSION {
  SM50_SHADER_METAL_310 = 310,
  SM50_SHADER_METAL_320 = 320,
  SM50_SHADER_METAL_MAX = 0xffffffff,
};

struct SM50_SHADER_COMMON_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
};

struct SM50_IA_INPUT_ELEMENT {
  uint32_t reg;
  uint32_t slot;
  uint32_t aligned_byte_offset;
  /** MTLAttributeFormat */
  uint32_t format;
  uint32_t step_function: 1;
  uint32_t step_rate: 31;
};

enum SM50_INDEX_BUFFER_FORAMT {
  SM50_INDEX_BUFFER_FORMAT_NONE = 0,
  SM50_INDEX_BUFFER_FORMAT_UINT16 = 1,
  SM50_INDEX_BUFFER_FORMAT_UINT32 = 2,
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORAMT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  struct SM50_IA_INPUT_ELEMENT *elements;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

struct SM50_SHADER_DXSO_ALPHA_TEST_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint8_t alpha_test_func; // 0=disabled, 1-8 = D3DCMP_*
};

struct SM50_SHADER_DXSO_FOG_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint8_t fog_mode; // 0=disabled, 1=vertex (FOG0), 2=table EXP, 3=table EXP2, 4=table LINEAR
};

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
);
AIRCONV_API void SM50Destroy(sm50_shader_t pShader);
AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData
);
AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode);
AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize);
AIRCONV_API void SM50FreeError(sm50_error_t pError);

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
);

/* DXSO (SM1-3) shader API */
typedef sm50_ptr64_t dxso_shader_t;

AIRCONV_API int DXSOInitialize(
  const void *pBytecode, size_t BytecodeSize,
  dxso_shader_t *ppShader, sm50_error_t *ppError
);
AIRCONV_API void DXSODestroy(dxso_shader_t pShader);

struct DXSO_INPUT_DECL {
  uint32_t reg;
  uint32_t usage;       /* DxsoUsage */
  uint32_t usageIndex;
};
AIRCONV_API uint32_t DXSOGetInputDeclCount(dxso_shader_t pShader);
AIRCONV_API void DXSOGetInputDecls(dxso_shader_t pShader, struct DXSO_INPUT_DECL *pDecls);
AIRCONV_API uint32_t DXSOGetPSMaxTexcoordCount(dxso_shader_t pShader);
AIRCONV_API uint32_t DXSOGetVSHasFogOutput(dxso_shader_t pShader);

struct DXSO_SAMPLER_DECL {
  uint32_t reg;
  uint32_t textureType; /* DxsoTextureType */
};
AIRCONV_API uint32_t DXSOGetSamplerDeclCount(dxso_shader_t pShader);
AIRCONV_API void DXSOGetSamplerDecls(dxso_shader_t pShader, struct DXSO_SAMPLER_DECL *pDecls);
AIRCONV_API uint32_t DXSOGetMaxConstantRegister(dxso_shader_t pShader);

AIRCONV_API int DXSOCompile(
  dxso_shader_t pShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName,
  sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

/* D3D9 Fixed-Function pipeline shader generation */

struct D3D9_FF_VS_KEY {
  uint8_t has_position_t;
  uint8_t has_normal;
  uint8_t has_color0, has_color1;
  uint8_t tex_coord_count;
  uint8_t fog_mode;
  uint8_t lighting_enabled;
  uint8_t num_active_lights;
  uint8_t normalize_normals;
  uint8_t light_types[8];
  uint8_t diffuse_source;
  uint8_t ambient_source;
  uint8_t specular_source;
  uint8_t emissive_source;
  uint8_t color_vertex;
  uint8_t tci_modes[8];  /* TCI generation mode per texcoord output (0=passthru, 1=cameraspacenormal, 2=cameraspaceposition, 3=cameraspacereflection) */
  uint8_t tci_coord_indices[8]; /* low bits of TEXCOORDINDEX: which coord set to use for passthru */
  uint8_t ttf_modes[8]; /* D3DTSS_TEXTURETRANSFORMFLAGS per texcoord (0=disable, 2=count2, 3=count3) */
};

struct D3D9_FF_VS_ELEMENT {
  uint8_t usage;        /* D3DDECLUSAGE */
  uint8_t usage_index;
  uint8_t type;         /* D3DDECLTYPE */
  uint8_t stream;
  uint16_t offset;
  uint16_t padding;
};

struct D3D9_FF_PS_STAGE {
  uint8_t color_op, color_arg1, color_arg2;
  uint8_t alpha_op, alpha_arg1, alpha_arg2;
  uint8_t has_texture;
  uint8_t texcoord_index;
  uint8_t color_arg0, alpha_arg0; /* third argument of MULTIPLYADD / LERP */
  uint8_t result_arg;             /* D3DTA_CURRENT or D3DTA_TEMP */
  uint8_t pad;
};

struct D3D9_FF_PS_KEY {
  struct D3D9_FF_PS_STAGE stages[8];
  uint8_t tex_coord_count;
  uint8_t specular_enable;
  uint8_t alpha_test_enable, alpha_test_func;
  uint8_t fog_enable;
  uint8_t fog_coord_mode; /* 0 = FOG0 is the factor; 1=EXP 2=EXP2 3=LINEAR applied to FOG0 as a coordinate */
};

AIRCONV_API int D3D9FFCompileVS(
  const struct D3D9_FF_VS_KEY *pKey,
  const struct D3D9_FF_VS_ELEMENT *pElements,
  uint32_t numElements,
  uint32_t slotMask,
  const char *FunctionName,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API int D3D9FFCompilePS(
  const struct D3D9_FF_PS_KEY *pKey,
  uint8_t texcoordCount,
  const char *FunctionName,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

#ifdef __cplusplus
};

inline std::string SM50GetErrorMessageString(sm50_error_t pError) {
  std::string str;
  str.resize(256);
  auto size = SM50GetErrorMessage(pError, str.data(), str.size());
  str.resize(size);
  return str;
};

#endif

#endif

#include <cstdlib>
#include "airconv_public.h"
#include "air_signature.hpp"
#include "air_type.hpp"
#include "airconv_context.hpp"
#include "dxbc_converter.hpp"
#include "metallib_writer.hpp"
#include "nt/air_builder.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace dxmt::air;

// Reuse from dxso_compiler.cpp
enum {
  FFAttrFormatFloat  = 28,
  FFAttrFormatFloat2 = 29,
  FFAttrFormatFloat3 = 30,
  FFAttrFormatFloat4 = 31,
};

struct FFCompiledBitcodeInternal {
  SmallVector<char, 0> vec;
};

// D3DDECLUSAGE values (from d3d9types.h)
enum {
  FF_DECLUSAGE_POSITION = 0,
  FF_DECLUSAGE_BLENDWEIGHT = 1,
  FF_DECLUSAGE_BLENDINDICES = 2,
  FF_DECLUSAGE_NORMAL = 3,
  FF_DECLUSAGE_PSIZE = 4,
  FF_DECLUSAGE_TEXCOORD = 5,
  FF_DECLUSAGE_TANGENT = 6,
  FF_DECLUSAGE_BINORMAL = 7,
  FF_DECLUSAGE_TESSFACTOR = 8,
  FF_DECLUSAGE_POSITIONT = 9,
  FF_DECLUSAGE_COLOR = 10,
  FF_DECLUSAGE_FOG = 11,
  FF_DECLUSAGE_DEPTH = 12,
  FF_DECLUSAGE_SAMPLE = 13,
};

// D3DDECLTYPE values
enum {
  FF_DECLTYPE_FLOAT1 = 0,
  FF_DECLTYPE_FLOAT2 = 1,
  FF_DECLTYPE_FLOAT3 = 2,
  FF_DECLTYPE_FLOAT4 = 3,
  FF_DECLTYPE_D3DCOLOR = 4,
};

// D3DTOP values
enum {
  FF_TOP_DISABLE = 1,
  FF_TOP_SELECTARG1 = 2,
  FF_TOP_SELECTARG2 = 3,
  FF_TOP_MODULATE = 4,
  FF_TOP_MODULATE2X = 5,
  FF_TOP_MODULATE4X = 6,
  FF_TOP_ADD = 7,
  FF_TOP_ADDSIGNED = 8,
  FF_TOP_ADDSIGNED2X = 9,
  FF_TOP_SUBTRACT = 10,
  FF_TOP_ADDSMOOTH = 11,
  FF_TOP_BLENDDIFFUSEALPHA = 12,
  FF_TOP_BLENDTEXTUREALPHA = 13,
  FF_TOP_BLENDFACTORALPHA = 14,
  FF_TOP_BLENDCURRENTALPHA = 15,
  FF_TOP_BLENDTEXTUREALPHAPM = 16,
  FF_TOP_PREMODULATE = 17,
  FF_TOP_MODULATEALPHA_ADDCOLOR = 18,
  FF_TOP_MODULATECOLOR_ADDALPHA = 19,
  FF_TOP_MODULATEINVALPHA_ADDCOLOR = 20,
  FF_TOP_MODULATEINVCOLOR_ADDALPHA = 21,
  FF_TOP_BUMPENVMAP = 22,
  FF_TOP_BUMPENVMAPLUMINANCE = 23,
  FF_TOP_DOTPRODUCT3 = 24,
  FF_TOP_MULTIPLYADD = 25,
  FF_TOP_LERP = 26,
};

// D3DTA values
enum {
  FF_TA_DIFFUSE = 0,
  FF_TA_CURRENT = 1,
  FF_TA_TEXTURE = 2,
  FF_TA_TFACTOR = 3,
  FF_TA_SPECULAR = 4,
  FF_TA_TEMP = 5,
};

// D3DCMP values
enum {
  FF_CMP_NEVER = 1,
  FF_CMP_LESS = 2,
  FF_CMP_EQUAL = 3,
  FF_CMP_LESSEQUAL = 4,
  FF_CMP_GREATER = 5,
  FF_CMP_NOTEQUAL = 6,
  FF_CMP_GREATEREQUAL = 7,
  FF_CMP_ALWAYS = 8,
};

static uint32_t decltypeToAttrFormat(uint8_t decltype_) {
  switch (decltype_) {
  case FF_DECLTYPE_FLOAT1: return FFAttrFormatFloat;
  case FF_DECLTYPE_FLOAT2: return FFAttrFormatFloat2;
  case FF_DECLTYPE_FLOAT3: return FFAttrFormatFloat3;
  case FF_DECLTYPE_FLOAT4: return FFAttrFormatFloat4;
  default: return FFAttrFormatFloat4;
  }
}

// Helper: create a saturate (clamp 0..1)
static Value *saturate(IRBuilder<> &builder, Value *val) {
  auto *floatTy = val->getType();
  if (floatTy->isVectorTy()) {
    auto *elemTy = cast<FixedVectorType>(floatTy)->getElementType();
    unsigned numElems = cast<FixedVectorType>(floatTy)->getNumElements();
    auto *zero = ConstantVector::getSplat(
      ElementCount::getFixed(numElems), ConstantFP::get(elemTy, 0.0));
    auto *one = ConstantVector::getSplat(
      ElementCount::getFixed(numElems), ConstantFP::get(elemTy, 1.0));
    val = builder.CreateSelect(
      builder.CreateFCmpOLT(val, zero), zero, val);
    val = builder.CreateSelect(
      builder.CreateFCmpOGT(val, one), one, val);
    return val;
  }
  auto *zero = ConstantFP::get(floatTy, 0.0);
  auto *one = ConstantFP::get(floatTy, 1.0);
  val = builder.CreateSelect(builder.CreateFCmpOLT(val, zero), zero, val);
  val = builder.CreateSelect(builder.CreateFCmpOGT(val, one), one, val);
  return val;
}

static uint32_t decltypeComponentCount(uint8_t decltype_) {
  switch (decltype_) {
  case FF_DECLTYPE_FLOAT1: return 1;
  case FF_DECLTYPE_FLOAT2: return 2;
  case FF_DECLTYPE_FLOAT3: return 3;
  case FF_DECLTYPE_FLOAT4: return 4;
  case FF_DECLTYPE_D3DCOLOR: return 4;
  default: return 4;
  }
}

// Load a vertex attribute from the VB table using vertex pulling
static Value *pullVertexAttribute(
    IRBuilder<> &builder, AirType &types, Value *vbuf_table, Value *vertex_id,
    uint32_t vb_entry_idx, uint32_t byte_offset, uint8_t decltype_, uint32_t format) {

  auto *float4Ty = types._float4;
  auto *vbEntryTy = types._dxmt_vertex_buffer_entry;

  auto *vb_entry = builder.CreateLoad(
    vbEntryTy,
    builder.CreateConstGEP1_32(vbEntryTy, vbuf_table, vb_entry_idx));
  auto *base_addr = builder.CreateExtractValue(vb_entry, {0});
  auto *stride = builder.CreateExtractValue(vb_entry, {1});
  auto *byte_off = builder.CreateAdd(
    builder.CreateMul(stride, vertex_id),
    builder.getInt32(byte_offset));

  auto *byte_ptr = builder.CreateIntToPtr(
    builder.CreateAdd(
      builder.CreatePtrToInt(base_addr, builder.getInt64Ty()),
      builder.CreateZExt(byte_off, builder.getInt64Ty())),
    types._float->getPointerTo((uint32_t)AddressSpace::device));

  if (decltype_ == FF_DECLTYPE_D3DCOLOR) {
    // D3DCOLOR: load as uint32, extract BGRA bytes, swizzle to RGBA, normalize
    auto *int_ptr = builder.CreateBitCast(
      byte_ptr, types._int->getPointerTo((uint32_t)AddressSpace::device));
    auto *packed = builder.CreateLoad(types._int, int_ptr);

    // Extract bytes: packed = 0xAARRGGBB in memory
    auto *b = builder.CreateAnd(packed, builder.getInt32(0xFF));
    auto *g = builder.CreateAnd(builder.CreateLShr(packed, builder.getInt32(8)), builder.getInt32(0xFF));
    auto *r = builder.CreateAnd(builder.CreateLShr(packed, builder.getInt32(16)), builder.getInt32(0xFF));
    auto *a = builder.CreateAnd(builder.CreateLShr(packed, builder.getInt32(24)), builder.getInt32(0xFF));

    auto *inv255 = ConstantFP::get(types._float, 1.0 / 255.0);
    auto *rf = builder.CreateFMul(builder.CreateUIToFP(r, types._float), inv255);
    auto *gf = builder.CreateFMul(builder.CreateUIToFP(g, types._float), inv255);
    auto *bf = builder.CreateFMul(builder.CreateUIToFP(b, types._float), inv255);
    auto *af = builder.CreateFMul(builder.CreateUIToFP(a, types._float), inv255);

    Value *result = UndefValue::get(float4Ty);
    result = builder.CreateInsertElement(result, rf, builder.getInt32(0));
    result = builder.CreateInsertElement(result, gf, builder.getInt32(1));
    result = builder.CreateInsertElement(result, bf, builder.getInt32(2));
    result = builder.CreateInsertElement(result, af, builder.getInt32(3));
    return result;
  }

  // Float formats
  uint32_t numComponents = decltypeComponentCount(decltype_);
  Value *result = ConstantVector::get({
    ConstantFP::get(types._float, 0.0),
    ConstantFP::get(types._float, 0.0),
    ConstantFP::get(types._float, 0.0),
    ConstantFP::get(types._float, 1.0)});

  for (uint32_t i = 0; i < numComponents; i++) {
    auto *f = builder.CreateLoad(types._float,
      builder.CreateConstGEP1_32(types._float, byte_ptr, i));
    result = builder.CreateInsertElement(result, f, builder.getInt32(i));
  }
  return result;
}

// Number of components a D3DDECLTYPE supplies
static uint32_t declTypeComponents(uint8_t t) {
  switch (t) {
  case 0: return 1;                 // FLOAT1
  case 1: case 6: case 9: case 11: case 15: return 2; // FLOAT2, SHORT2, SHORT2N, USHORT2N, FLOAT16_2
  case 2: case 13: case 14: return 3; // FLOAT3, UDEC3, DEC3N
  default: return 4;
  }
}

// Helper: v × M (D3D9 row-vector convention, matrix stored row-major as 4 consecutive float4s in CB)
// result[j] = sum_i(v[i] * M[i][j])
static Value *matMulVec4(IRBuilder<> &builder, Type *float4Ty, Type *floatTy,
                         Value *cbPtr, uint32_t matStartRow, Value *vec) {
  // Load all 4 matrix rows
  Value *rows[4];
  for (uint32_t i = 0; i < 4; i++) {
    rows[i] = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, cbPtr, matStartRow + i));
  }

  // result = v.x * row0 + v.y * row1 + v.z * row2 + v.w * row3
  Value *result = UndefValue::get(float4Ty);
  result = ConstantAggregateZero::get(float4Ty);
  for (uint32_t i = 0; i < 4; i++) {
    auto *vi = builder.CreateExtractElement(vec, builder.getInt32(i));
    auto *viSplat = builder.CreateVectorSplat(4, vi);
    result = builder.CreateFAdd(result, builder.CreateFMul(viSplat, rows[i]));
  }
  return result;
}

static void compileFFVertexShader(
  const D3D9_FF_VS_KEY *key,
  const D3D9_FF_VS_ELEMENT *elements, uint32_t numElements, uint32_t slotMask,
  const char *functionName,
  SM50_SHADER_METAL_VERSION metal_version,
  LLVMContext &context, Module &module
) {
  AirType types(context);
  FunctionSignatureBuilder func_sig;

  // Define outputs
  auto pos_out_idx = func_sig.DefineOutput(OutputPosition{.type = msl_float4});

  std::vector<uint32_t> texcoord_out_indices;

  // Output texcoords first, then colors — keeps texture/sampler argument
  // indices stable to work around a Metal shader compiler crash.
  for (uint8_t i = 0; i < key->tex_coord_count; i++) {
    auto idx = func_sig.DefineOutput(OutputVertex{
      .user = "TEXCOORD" + std::to_string(i), .type = msl_float4,
    });
    texcoord_out_indices.push_back(idx);
  }
  uint32_t color0_out_idx = func_sig.DefineOutput(OutputVertex{
    .user = "COLOR0", .type = msl_float4,
  });
  uint32_t color1_out_idx = func_sig.DefineOutput(OutputVertex{
    .user = "COLOR1", .type = msl_float4,
  });

  // Fog output
  uint32_t fog_out_idx = ~0u;
  if (key->fog_mode != 0) {
    fog_out_idx = func_sig.DefineOutput(OutputVertex{
      .user = "FOG0", .type = msl_float4,
    });
  }

  // Define inputs
  uint32_t vbuf_table_idx = func_sig.DefineInput(ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = 16,
    .array_size = 0,
    .memory_access = MemoryAccess::read,
    .address_space = AddressSpace::constant,
    .type = msl_uint,
    .arg_name = "vertex_buffers",
    .raster_order_group = {},
  });

  uint32_t const_buf_idx = func_sig.DefineInput(ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = 17,
    .array_size = 0,
    .memory_access = MemoryAccess::read,
    .address_space = AddressSpace::constant,
    .type = msl_float4,
    .arg_name = "vs_constants",
    .raster_order_group = {},
  });

  uint32_t vertex_id_idx = func_sig.DefineInput(InputVertexID{});
  (void)func_sig.DefineInput(InputBaseVertex{});

  auto [function, function_md] = func_sig.CreateFunction(
    functionName, context, module, 0, false
  );

  auto *entry_bb = BasicBlock::Create(context, "entry", function);
  IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nullOS;
  llvm::air::AIRBuilder air(builder, nullOS);

  dxmt::dxbc::setup_metal_version(module, metal_version);

  auto *float4Ty = types._float4;
  auto *floatTy = types._float;

  auto *vertex_id = function->getArg(vertex_id_idx);
  auto *vbuf_table_raw = function->getArg(vbuf_table_idx);
  auto *vbEntryTy = types._dxmt_vertex_buffer_entry;
  auto *vbuf_table = builder.CreateBitCast(
    vbuf_table_raw,
    vbEntryTy->getPointerTo((uint32_t)AddressSpace::constant));

  auto *cbFloat4 = function->getArg(const_buf_idx);

  // Pull vertex attributes
  Value *position = nullptr;
  Value *normal = nullptr;
  Value *color0 = nullptr;
  Value *color1 = nullptr;
  Value *texcoords[8] = {};
  uint8_t texcoordTypes[8] = {};

  for (uint32_t i = 0; i < numElements; i++) {
    auto &elem = elements[i];
    unsigned shift = 32u - elem.stream;
    unsigned vb_entry_idx =
      elem.stream ? __builtin_popcount((slotMask << shift) >> shift) : 0;

    uint32_t format = (elem.type == FF_DECLTYPE_D3DCOLOR)
      ? FFAttrFormatFloat4 : decltypeToAttrFormat(elem.type);

    Value *val = pullVertexAttribute(
      builder, types, vbuf_table, vertex_id,
      vb_entry_idx, elem.offset, elem.type, format);

    switch (elem.usage) {
    case FF_DECLUSAGE_POSITION:
    case FF_DECLUSAGE_POSITIONT:
      position = val;
      break;
    case FF_DECLUSAGE_NORMAL:
      normal = val;
      break;
    case FF_DECLUSAGE_COLOR:
      if (elem.usage_index == 0) color0 = val;
      else if (elem.usage_index == 1) color1 = val;
      break;
    case FF_DECLUSAGE_TEXCOORD:
      if (elem.usage_index < 8) {
        texcoords[elem.usage_index] = val;
        texcoordTypes[elem.usage_index] = elem.type;
      }
      break;
    default:
      break;
    }
  }

  // Default values for missing attributes
  auto *white = ConstantVector::get({
    ConstantFP::get(floatTy, 1.0), ConstantFP::get(floatTy, 1.0),
    ConstantFP::get(floatTy, 1.0), ConstantFP::get(floatTy, 1.0)});
  auto *zero4 = ConstantAggregateZero::get(float4Ty);

  if (!position) position = zero4;
  if (!color0) color0 = white;
  if (!color1) color1 = zero4;

  // Extend position to float4 with w=1
  auto *posW1 = builder.CreateInsertElement(position, ConstantFP::get(floatTy, 1.0), builder.getInt32(3));

  // Check if any TCI mode needs eye-space vectors
  bool needEyePos = false, needEyeNormal = false;
  for (int i = 0; i < 8; i++) {
    if (key->tci_modes[i] == 2 || key->tci_modes[i] == 3) needEyePos = true;
    if (key->tci_modes[i] == 1 || key->tci_modes[i] == 3) needEyeNormal = true;
  }
  // Compute eye-space position (for lighting/fog/TCI): posEye = pos * WorldView (c[12..15])
  Value *posEye = nullptr;
  if (!key->has_position_t && (key->fog_mode != 0 || key->lighting_enabled || needEyePos)) {
    posEye = matMulVec4(builder, float4Ty, floatTy, cbFloat4, 12, posW1);
  }

  // Transform position
  Value *outPos;
  if (key->has_position_t) {
    auto *vpInfo = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, cbFloat4, 20));
    auto *vpW = builder.CreateExtractElement(vpInfo, builder.getInt32(0));
    auto *vpH = builder.CreateExtractElement(vpInfo, builder.getInt32(1));

    auto *px = builder.CreateExtractElement(position, builder.getInt32(0));
    auto *py = builder.CreateExtractElement(position, builder.getInt32(1));
    auto *pz = builder.CreateExtractElement(position, builder.getInt32(2));
    auto *pw = builder.CreateExtractElement(position, builder.getInt32(3));

    auto *two = ConstantFP::get(floatTy, 2.0);
    auto *one = ConstantFP::get(floatTy, 1.0);
    auto *ndcX = builder.CreateFSub(builder.CreateFMul(builder.CreateFDiv(px, vpW), two), one);
    auto *ndcY = builder.CreateFSub(one, builder.CreateFMul(builder.CreateFDiv(py, vpH), two));
    auto *rhw = pw;
    auto *wOut = builder.CreateFDiv(one, rhw);

    outPos = UndefValue::get(float4Ty);
    outPos = builder.CreateInsertElement(outPos, builder.CreateFMul(ndcX, wOut), builder.getInt32(0));
    outPos = builder.CreateInsertElement(outPos, builder.CreateFMul(ndcY, wOut), builder.getInt32(1));
    outPos = builder.CreateInsertElement(outPos, builder.CreateFMul(pz, wOut), builder.getInt32(2));
    outPos = builder.CreateInsertElement(outPos, wOut, builder.getInt32(3));
  } else {
    outPos = matMulVec4(builder, float4Ty, floatTy, cbFloat4, 16, posW1);
  }

  // --- Lighting ---
  Value *outColor0 = color0;
  Value *outColor1 = color1;

  if (key->lighting_enabled && !key->has_position_t) {
    // Transform normal to eye space using WorldView 3x3 (c[12..15])
    Value *normalEye;
    if (normal) {
      auto *nW0 = builder.CreateInsertElement(normal, ConstantFP::get(floatTy, 0.0), builder.getInt32(3));
      normalEye = matMulVec4(builder, float4Ty, floatTy, cbFloat4, 12, nW0);
    } else {
      normalEye = ConstantVector::get({
        ConstantFP::get(floatTy, 0.0), ConstantFP::get(floatTy, 0.0),
        ConstantFP::get(floatTy, 1.0), ConstantFP::get(floatTy, 0.0)});
    }
    // Normalize
    if (key->normalize_normals || true) { // always normalize after transform
      auto *nx = builder.CreateExtractElement(normalEye, builder.getInt32(0));
      auto *ny = builder.CreateExtractElement(normalEye, builder.getInt32(1));
      auto *nz = builder.CreateExtractElement(normalEye, builder.getInt32(2));
      auto *dot = builder.CreateFAdd(builder.CreateFAdd(
        builder.CreateFMul(nx, nx), builder.CreateFMul(ny, ny)),
        builder.CreateFMul(nz, nz));
      auto *invLen = air.CreateFPUnOp(air.rsqrt, dot);
      normalEye = builder.CreateInsertElement(
        builder.CreateInsertElement(
          builder.CreateInsertElement(zero4,
            builder.CreateFMul(nx, invLen), builder.getInt32(0)),
          builder.CreateFMul(ny, invLen), builder.getInt32(1)),
        builder.CreateFMul(nz, invLen), builder.getInt32(2));
    }

    // Resolve material sources: 0=MATERIAL, 1=COLOR1, 2=COLOR2
    auto resolveMat = [&](uint8_t source, uint32_t matSlot) -> Value * {
      if (key->color_vertex) {
        if (source == 1 && key->has_color0) return color0;
        if (source == 2 && key->has_color1) return color1;
      }
      return builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, cbFloat4, matSlot));
    };

    auto *matDiffuse  = resolveMat(key->diffuse_source,  23);
    auto *matAmbient  = resolveMat(key->ambient_source,  24);
    auto *matSpecular = resolveMat(key->specular_source, 25);
    auto *matEmissive = resolveMat(key->emissive_source, 26);

    auto *powerVec = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, cbFloat4, 27));
    auto *power = builder.CreateExtractElement(powerVec, builder.getInt32(0));

    auto *globalAmbient = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, cbFloat4, 22));

    // Start accumulation: emissive + globalAmbient * matAmbient
    Value *diffuseAccum = builder.CreateFAdd(matEmissive,
      builder.CreateFMul(globalAmbient, matAmbient));
    Value *specAccum = zero4;

    // Per-light loop (unrolled)
    for (uint8_t li = 0; li < key->num_active_lights; li++) {
      uint32_t base = 28 + li * 5;
      auto *lightPosType = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, cbFloat4, base + 0));
      auto *lightDirFalloff = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, cbFloat4, base + 1));
      auto *lightDiffuse = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, cbFloat4, base + 2));
      auto *lightAmbient = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, cbFloat4, base + 3));
      auto *lightAtten = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, cbFloat4, base + 4));
      auto *lightSpecular = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, cbFloat4, 100 + li));

      Value *L; // light direction (towards light)
      Value *atten;

      if (key->light_types[li] == 1) {
        // D3DLIGHT_POINT
        auto *lpx = builder.CreateExtractElement(lightPosType, builder.getInt32(0));
        auto *lpy = builder.CreateExtractElement(lightPosType, builder.getInt32(1));
        auto *lpz = builder.CreateExtractElement(lightPosType, builder.getInt32(2));
        auto *vx = builder.CreateExtractElement(posEye, builder.getInt32(0));
        auto *vy = builder.CreateExtractElement(posEye, builder.getInt32(1));
        auto *vz = builder.CreateExtractElement(posEye, builder.getInt32(2));
        auto *dx = builder.CreateFSub(lpx, vx);
        auto *dy = builder.CreateFSub(lpy, vy);
        auto *dz = builder.CreateFSub(lpz, vz);
        auto *dist2 = builder.CreateFAdd(builder.CreateFAdd(
          builder.CreateFMul(dx, dx), builder.CreateFMul(dy, dy)),
          builder.CreateFMul(dz, dz));
        auto *dist = air.CreateFPUnOp(air.sqrt, dist2);
        auto *invDist = air.CreateFPUnOp(air.rsqrt, dist2);
        L = builder.CreateInsertElement(
          builder.CreateInsertElement(
            builder.CreateInsertElement(zero4,
              builder.CreateFMul(dx, invDist), builder.getInt32(0)),
            builder.CreateFMul(dy, invDist), builder.getInt32(1)),
          builder.CreateFMul(dz, invDist), builder.getInt32(2));

        auto *a0 = builder.CreateExtractElement(lightAtten, builder.getInt32(0));
        auto *a1 = builder.CreateExtractElement(lightAtten, builder.getInt32(1));
        auto *a2 = builder.CreateExtractElement(lightAtten, builder.getInt32(2));
        auto *attenDenom = builder.CreateFAdd(a0,
          builder.CreateFAdd(builder.CreateFMul(a1, dist),
            builder.CreateFMul(a2, dist2)));
        atten = builder.CreateFDiv(ConstantFP::get(floatTy, 1.0), attenDenom);
      } else {
        // D3DLIGHT_DIRECTIONAL (type == 3 or default)
        // Direction in eye space is pre-computed and stored negated
        auto *ldx = builder.CreateExtractElement(lightDirFalloff, builder.getInt32(0));
        auto *ldy = builder.CreateExtractElement(lightDirFalloff, builder.getInt32(1));
        auto *ldz = builder.CreateExtractElement(lightDirFalloff, builder.getInt32(2));
        L = builder.CreateInsertElement(
          builder.CreateInsertElement(
            builder.CreateInsertElement(zero4,
              builder.CreateFNeg(ldx), builder.getInt32(0)),
            builder.CreateFNeg(ldy), builder.getInt32(1)),
          builder.CreateFNeg(ldz), builder.getInt32(2));
        atten = ConstantFP::get(floatTy, 1.0);
      }

      // N·L
      auto *nlx = builder.CreateFMul(
        builder.CreateExtractElement(normalEye, builder.getInt32(0)),
        builder.CreateExtractElement(L, builder.getInt32(0)));
      auto *nly = builder.CreateFMul(
        builder.CreateExtractElement(normalEye, builder.getInt32(1)),
        builder.CreateExtractElement(L, builder.getInt32(1)));
      auto *nlz = builder.CreateFMul(
        builder.CreateExtractElement(normalEye, builder.getInt32(2)),
        builder.CreateExtractElement(L, builder.getInt32(2)));
      auto *NdotL = air.CreateFPBinOp(air.fmax,
        builder.CreateFAdd(builder.CreateFAdd(nlx, nly), nlz),
        ConstantFP::get(floatTy, 0.0));

      auto *attenSplat = builder.CreateVectorSplat(4, atten);
      auto *NdotLSplat = builder.CreateVectorSplat(4, NdotL);

      // Diffuse contribution: atten * NdotL * lightDiffuse * matDiffuse
      diffuseAccum = builder.CreateFAdd(diffuseAccum,
        builder.CreateFMul(attenSplat,
          builder.CreateFMul(NdotLSplat,
            builder.CreateFMul(lightDiffuse, matDiffuse))));

      // Ambient contribution: atten * lightAmbient * matAmbient
      diffuseAccum = builder.CreateFAdd(diffuseAccum,
        builder.CreateFMul(attenSplat,
          builder.CreateFMul(lightAmbient, matAmbient)));

      // Specular: H = normalize(L + V), V = normalize(-posEye)
      // Only if power > 0 and NdotL > 0
      auto *evx = builder.CreateFNeg(builder.CreateExtractElement(posEye, builder.getInt32(0)));
      auto *evy = builder.CreateFNeg(builder.CreateExtractElement(posEye, builder.getInt32(1)));
      auto *evz = builder.CreateFNeg(builder.CreateExtractElement(posEye, builder.getInt32(2)));
      auto *vDot = builder.CreateFAdd(builder.CreateFAdd(
        builder.CreateFMul(evx, evx), builder.CreateFMul(evy, evy)),
        builder.CreateFMul(evz, evz));
      auto *vInvLen = air.CreateFPUnOp(air.rsqrt, vDot);
      auto *Vx = builder.CreateFMul(evx, vInvLen);
      auto *Vy = builder.CreateFMul(evy, vInvLen);
      auto *Vz = builder.CreateFMul(evz, vInvLen);

      auto *Hx = builder.CreateFAdd(builder.CreateExtractElement(L, builder.getInt32(0)), Vx);
      auto *Hy = builder.CreateFAdd(builder.CreateExtractElement(L, builder.getInt32(1)), Vy);
      auto *Hz = builder.CreateFAdd(builder.CreateExtractElement(L, builder.getInt32(2)), Vz);
      auto *hDot = builder.CreateFAdd(builder.CreateFAdd(
        builder.CreateFMul(Hx, Hx), builder.CreateFMul(Hy, Hy)),
        builder.CreateFMul(Hz, Hz));
      auto *hInvLen = air.CreateFPUnOp(air.rsqrt, hDot);
      auto *NHx = builder.CreateFMul(builder.CreateExtractElement(normalEye, builder.getInt32(0)),
        builder.CreateFMul(Hx, hInvLen));
      auto *NHy = builder.CreateFMul(builder.CreateExtractElement(normalEye, builder.getInt32(1)),
        builder.CreateFMul(Hy, hInvLen));
      auto *NHz = builder.CreateFMul(builder.CreateExtractElement(normalEye, builder.getInt32(2)),
        builder.CreateFMul(Hz, hInvLen));
      auto *NdotH = air.CreateFPBinOp(air.fmax,
        builder.CreateFAdd(builder.CreateFAdd(NHx, NHy), NHz),
        ConstantFP::get(floatTy, 0.0));
      Value *specFactor = air.CreateFPUnOp(air.exp2, builder.CreateFMul(power, air.CreateFPUnOp(air.log2, NdotH)));
      // Zero specular when NdotL <= 0 or NdotH <= 0 (also avoids NaN from power * log2(0) when power == 0)
      specFactor = builder.CreateSelect(
        builder.CreateAnd(builder.CreateFCmpOGT(NdotL, ConstantFP::get(floatTy, 0.0)),
                          builder.CreateFCmpOGT(NdotH, ConstantFP::get(floatTy, 0.0))),
        specFactor, ConstantFP::get(floatTy, 0.0));
      auto *specSplat = builder.CreateVectorSplat(4, specFactor);
      specAccum = builder.CreateFAdd(specAccum,
        builder.CreateFMul(attenSplat,
          builder.CreateFMul(specSplat,
            builder.CreateFMul(lightSpecular, matSpecular)))); // D3DLIGHT9.Specular, not Diffuse
    }

    // Saturate lighting results
    outColor0 = saturate(builder, diffuseAccum);
    outColor1 = saturate(builder, specAccum);
    // Preserve original alpha from material diffuse
    auto *diffAlpha = builder.CreateExtractElement(matDiffuse, builder.getInt32(3));
    outColor0 = builder.CreateInsertElement(outColor0, diffAlpha, builder.getInt32(3));
  }

  // --- Eye-space normal for TCI (when not already computed by lighting) ---
  Value *normalEyeForTCI = nullptr;
  if (needEyeNormal && !key->has_position_t) {
    if (normal) {
      auto *nW0 = builder.CreateInsertElement(normal, ConstantFP::get(floatTy, 0.0), builder.getInt32(3));
      normalEyeForTCI = matMulVec4(builder, float4Ty, floatTy, cbFloat4, 12, nW0);
      // Normalize
      auto *nx = builder.CreateExtractElement(normalEyeForTCI, builder.getInt32(0));
      auto *ny = builder.CreateExtractElement(normalEyeForTCI, builder.getInt32(1));
      auto *nz = builder.CreateExtractElement(normalEyeForTCI, builder.getInt32(2));
      auto *dot = builder.CreateFAdd(builder.CreateFAdd(
        builder.CreateFMul(nx, nx), builder.CreateFMul(ny, ny)),
        builder.CreateFMul(nz, nz));
      auto *invLen = air.CreateFPUnOp(air.rsqrt, dot);
      normalEyeForTCI = builder.CreateInsertElement(
        builder.CreateInsertElement(
          builder.CreateInsertElement(zero4,
            builder.CreateFMul(nx, invLen), builder.getInt32(0)),
          builder.CreateFMul(ny, invLen), builder.getInt32(1)),
        builder.CreateFMul(nz, invLen), builder.getInt32(2));
    } else {
      normalEyeForTCI = ConstantVector::get({
        ConstantFP::get(floatTy, 0.0), ConstantFP::get(floatTy, 0.0),
        ConstantFP::get(floatTy, 1.0), ConstantFP::get(floatTy, 0.0)});
    }
  }

  // --- Fog ---
  Value *fogFactor = nullptr;
  if (key->fog_mode != 0 && !key->has_position_t) {
    auto *fogParams = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, cbFloat4, 21));
    auto *fogStart = builder.CreateExtractElement(fogParams, builder.getInt32(0));
    auto *fogEnd = builder.CreateExtractElement(fogParams, builder.getInt32(1));
    auto *fogDensity = builder.CreateExtractElement(fogParams, builder.getInt32(2));
    auto *eyeZ = air.CreateFPUnOp(air.fabs,
      builder.CreateExtractElement(posEye, builder.getInt32(2)));

    switch (key->fog_mode) {
    case 1: // EXP
      fogFactor = air.CreateFPUnOp(air.exp2,
        builder.CreateFMul(ConstantFP::get(floatTy, -1.442695f), // -1/ln(2)
          builder.CreateFMul(fogDensity, eyeZ)));
      break;
    case 2: { // EXP2
      auto *dz = builder.CreateFMul(fogDensity, eyeZ);
      fogFactor = air.CreateFPUnOp(air.exp2,
        builder.CreateFMul(ConstantFP::get(floatTy, -1.442695f),
          builder.CreateFMul(dz, dz)));
      break;
    }
    case 3: // LINEAR
      fogFactor = builder.CreateFDiv(
        builder.CreateFSub(fogEnd, eyeZ),
        builder.CreateFSub(fogEnd, fogStart));
      break;
    }
    fogFactor = saturate(builder, fogFactor);
  }

  // Build return value
  auto retTy = function->getReturnType();
  Value *retVal = UndefValue::get(retTy);

  retVal = builder.CreateInsertValue(retVal, outPos, {pos_out_idx});
  retVal = builder.CreateInsertValue(retVal, outColor0, {color0_out_idx});
  retVal = builder.CreateInsertValue(retVal, outColor1, {color1_out_idx});
  for (uint8_t i = 0; i < key->tex_coord_count; i++) {
    Value *tc;
    switch (key->tci_modes[i]) {
    case 1: // TCI_CAMERASPACENORMAL
      tc = normalEyeForTCI ? normalEyeForTCI : zero4;
      break;
    case 2: // TCI_CAMERASPACEPOSITION
      tc = posEye ? posEye : zero4;
      break;
    case 3: { // TCI_CAMERASPACEREFLECTIONVECTOR
      // R = 2*N*dot(N,E) - E where E = normalize(posEye), N = normalEye
      if (posEye && normalEyeForTCI) {
        auto *ex = builder.CreateExtractElement(posEye, builder.getInt32(0));
        auto *ey = builder.CreateExtractElement(posEye, builder.getInt32(1));
        auto *ez = builder.CreateExtractElement(posEye, builder.getInt32(2));
        auto *eDot = builder.CreateFAdd(builder.CreateFAdd(
          builder.CreateFMul(ex, ex), builder.CreateFMul(ey, ey)),
          builder.CreateFMul(ez, ez));
        auto *eInvLen = air.CreateFPUnOp(air.rsqrt, eDot);
        auto *Enx = builder.CreateFMul(ex, eInvLen);
        auto *Eny = builder.CreateFMul(ey, eInvLen);
        auto *Enz = builder.CreateFMul(ez, eInvLen);
        auto *nx = builder.CreateExtractElement(normalEyeForTCI, builder.getInt32(0));
        auto *ny = builder.CreateExtractElement(normalEyeForTCI, builder.getInt32(1));
        auto *nz = builder.CreateExtractElement(normalEyeForTCI, builder.getInt32(2));
        // dot(N, E)
        auto *NdotE = builder.CreateFAdd(builder.CreateFAdd(
          builder.CreateFMul(nx, Enx), builder.CreateFMul(ny, Eny)),
          builder.CreateFMul(nz, Enz));
        // 2 * dot(N, E)
        auto *twoNdotE = builder.CreateFMul(ConstantFP::get(floatTy, 2.0), NdotE);
        // R = 2*N*dot(N,E) - E
        auto *rx = builder.CreateFSub(builder.CreateFMul(twoNdotE, nx), Enx);
        auto *ry = builder.CreateFSub(builder.CreateFMul(twoNdotE, ny), Eny);
        auto *rz = builder.CreateFSub(builder.CreateFMul(twoNdotE, nz), Enz);
        tc = builder.CreateInsertElement(
          builder.CreateInsertElement(
            builder.CreateInsertElement(zero4, rx, builder.getInt32(0)),
            ry, builder.getInt32(1)),
          rz, builder.getInt32(2));
      } else {
        tc = zero4;
      }
      break;
    }
    default: { // TCI_PASSTHRU (0)
      // Low bits of D3DTSS_TEXCOORDINDEX select which coord set to use
      uint8_t coordIdx = key->tci_coord_indices[i];
      tc = (coordIdx < 8 && texcoords[coordIdx]) ? texcoords[coordIdx] : zero4;
      break;
    }
    }
    // Apply texture matrix transform if TTF is enabled
    if (key->ttf_modes[i] != 0) {
      // Texture matrix at c[68 + i*4]
      uint32_t texMatBase = 68 + i * 4;
      // D3D9 expands the input to 4 components with the default 1.0 in the component right after the
      // last one supplied (u -> (u,1,0,0), uv -> (u,v,1,0), uvw -> (u,v,w,1)), so for 2D coordinates the
      // matrix's THIRD row is the translation (RenderWare UV animation, GTA SA vehicle env maps).
      // Generated coordinates are 3-component.
      uint32_t comps = 3;
      if (key->tci_modes[i] == 0) {
        uint8_t coordIdx = key->tci_coord_indices[i];
        comps = (coordIdx < 8 && texcoords[coordIdx]) ? declTypeComponents(texcoordTypes[coordIdx]) : 4;
      }
      Value *tcIn = tc;
      for (uint32_t c = comps; c < 4; c++)
        tcIn = builder.CreateInsertElement(tcIn, ConstantFP::get(floatTy, c == comps ? 1.0 : 0.0),
                                           builder.getInt32(c));
      tc = matMulVec4(builder, float4Ty, floatTy, cbFloat4, texMatBase, tcIn);
    }
    retVal = builder.CreateInsertValue(retVal, tc, {texcoord_out_indices[i]});
  }
  if (fog_out_idx != ~0u && fogFactor) {
    auto *fogVec = builder.CreateInsertElement(zero4, fogFactor, builder.getInt32(0));
    retVal = builder.CreateInsertValue(retVal, fogVec, {fog_out_idx});
  }

  builder.CreateRet(retVal);

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_md);
}

static void compileFFPixelShader(
  const D3D9_FF_PS_KEY *key,
  uint8_t texcoordCount,
  const char *functionName,
  SM50_SHADER_METAL_VERSION metal_version,
  LLVMContext &context, Module &module
) {
  AirType types(context);
  FunctionSignatureBuilder func_sig;

  // Define stage-in inputs: texcoords first, then colors — keeps texture/sampler
  // argument indices stable to work around Metal shader compiler crash.
  std::vector<uint32_t> texcoord_in_indices;
  for (uint8_t i = 0; i < texcoordCount; i++) {
    auto idx = func_sig.DefineInput(InputFragmentStageIn{
      .user = "TEXCOORD" + std::to_string(i), .type = msl_float4,
      .interpolation = Interpolation::center_perspective, .pull_mode = false,
    });
    texcoord_in_indices.push_back(idx);
  }
  uint32_t color0_in_idx = func_sig.DefineInput(InputFragmentStageIn{
    .user = "COLOR0", .type = msl_float4,
    .interpolation = Interpolation::center_perspective, .pull_mode = false,
  });
  uint32_t color1_in_idx = func_sig.DefineInput(InputFragmentStageIn{
    .user = "COLOR1", .type = msl_float4,
    .interpolation = Interpolation::center_perspective, .pull_mode = false,
  });

  // Fog input from VS
  uint32_t fog_in_idx = ~0u;
  if (key->fog_enable) {
    fog_in_idx = func_sig.DefineInput(InputFragmentStageIn{
      .user = "FOG0", .type = msl_float4,
      .interpolation = Interpolation::center_perspective, .pull_mode = false,
    });
  }

  // PS constants at buffer(18)
  uint32_t const_buf_idx = func_sig.DefineInput(ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = 18,
    .array_size = 0,
    .memory_access = MemoryAccess::read,
    .address_space = AddressSpace::constant,
    .type = msl_float4,
    .arg_name = "ps_constants",
    .raster_order_group = {},
  });

  // Texture and sampler inputs for stages that have textures
  struct TexBinding {
    uint32_t texArgIdx;
    uint32_t sampArgIdx;
    llvm::air::Texture texDesc;
  };
  std::vector<std::pair<uint32_t, TexBinding>> texBindings; // stage → binding
  for (uint32_t stage = 0; stage < 8; stage++) {
    if (!key->stages[stage].has_texture) continue;
    if (key->stages[stage].color_op == FF_TOP_DISABLE) break;

    llvm::air::Texture texDesc{
      .kind = llvm::air::Texture::texture_2d,
      .sample_type = llvm::air::Texture::sample_float,
      .memory_access = llvm::air::Texture::access_sample,
    };
    uint32_t texIdx = func_sig.DefineInput(ArgumentBindingTexture{
      .location_index = 19 + stage * 2,
      .array_size = 1,
      .memory_access = MemoryAccess::sample,
      .type = MSLTexture{
        .component_type = msl_float,
        .memory_access = MemoryAccess::sample,
        .resource_kind = TextureKind::texture_2d,
        .resource_kind_logical = TextureKind::texture_2d,
      },
      .arg_name = "tex" + std::to_string(stage),
      .raster_order_group = {},
    });
    uint32_t sampIdx = func_sig.DefineInput(ArgumentBindingSampler{
      .location_index = stage, // Metal sampler slots are 0..15 (separate namespace)
      .array_size = 1,
      .arg_name = "samp" + std::to_string(stage),
    });
    texBindings.push_back({stage, {texIdx, sampIdx, texDesc}});
  }

  // Output: render target 0
  uint32_t rt0_idx = func_sig.DefineOutput(OutputRenderTarget{
    .dual_source_blending = false,
    .index = 0,
    .type = msl_float4,
  });

  auto [function, function_md] = func_sig.CreateFunction(
    functionName, context, module, 0, false
  );

  auto *entry_bb = BasicBlock::Create(context, "entry", function);
  IRBuilder<> builder(entry_bb);

  dxmt::dxbc::setup_metal_version(module, metal_version);

  auto *float4Ty = types._float4;
  auto *floatTy = types._float;

  // Load stage-in
  auto *diffuse = function->getArg(color0_in_idx);
  auto *specular = function->getArg(color1_in_idx);

  Value *texcoordVals[8] = {};
  for (uint8_t i = 0; i < texcoordCount; i++) {
    texcoordVals[i] = function->getArg(texcoord_in_indices[i]);
  }

  // Constant buffer
  auto *constBufPtr = function->getArg(const_buf_idx);
  auto *cbFloat4 = builder.CreateBitCast(
    constBufPtr, float4Ty->getPointerTo((uint32_t)AddressSpace::constant));

  // c[0] = texture factor, c[1] = fog color RGB + alpha ref in .w
  auto *texFactor = builder.CreateLoad(float4Ty,
    builder.CreateConstGEP1_32(float4Ty, cbFloat4, 0));

  // Sample textures
  llvm::raw_null_ostream nullOS;
  llvm::air::AIRBuilder air(builder, nullOS);
  int32_t offsets[3] = {0, 0, 0};

  Value *texSamples[8] = {};
  for (auto &[stage, binding] : texBindings) {
    // Use texcoord_index for routing (Step 11)
    uint8_t tcIdx = key->stages[stage].texcoord_index;
    Value *coords = (tcIdx < texcoordCount) ? texcoordVals[tcIdx] : nullptr;
    if (!coords) continue;

    auto *u = builder.CreateExtractElement(coords, builder.getInt32(0));
    auto *v = builder.CreateExtractElement(coords, builder.getInt32(1));

    auto *float2Ty = FixedVectorType::get(floatTy, 2);
    Value *coord2d = UndefValue::get(float2Ty);
    coord2d = builder.CreateInsertElement(coord2d, u, builder.getInt32(0));
    coord2d = builder.CreateInsertElement(coord2d, v, builder.getInt32(1));

    auto *texHandle = function->getArg(binding.texArgIdx);
    auto *sampHandle = function->getArg(binding.sampArgIdx);

    auto [sampled, residency] = air.CreateSample(
      binding.texDesc, texHandle, sampHandle, coord2d, nullptr, offsets,
      llvm::air::sample_bias{air.getFloat(0)} // implicit LOD (mipmaps), like D3D9; level 0 aliased badly
    );
    texSamples[stage] = sampled;
  }

  // Process texture stages
  Value *current = diffuse;
  Value *temp = ConstantAggregateZero::get(float4Ty); // D3DTA_TEMP register, starts at 0

  auto *white = ConstantVector::get({
    ConstantFP::get(floatTy, 1.0), ConstantFP::get(floatTy, 1.0),
    ConstantFP::get(floatTy, 1.0), ConstantFP::get(floatTy, 1.0)});

  auto resolveArg = [&](uint8_t arg, uint32_t stage) -> Value * {
    Value *val;
    switch (arg & 0x0F) { // mask off modifier bits
    case FF_TA_DIFFUSE:  val = diffuse; break;
    case FF_TA_CURRENT:  val = current; break;
    case FF_TA_TEXTURE:  val = texSamples[stage] ? texSamples[stage] : white; break;
    case FF_TA_TFACTOR:  val = texFactor; break;
    case FF_TA_SPECULAR: val = specular; break;
    case FF_TA_TEMP:     val = temp; break;
    default: val = current; break;
    }
    if (arg & 0x10) { // D3DTA_COMPLEMENT
      val = builder.CreateFSub(white, val);
    }
    if (arg & 0x20) { // D3DTA_ALPHAREPLICATE
      val = builder.CreateShuffleVector(val, {3, 3, 3, 3});
    }
    return val;
  };

  auto applyOp = [&](uint8_t op, Value *arg0, Value *arg1, Value *arg2, uint32_t stage) -> Value * {
    auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 1.0));
    auto alphaSplat = [&](Value *v) { return builder.CreateShuffleVector(v, {3, 3, 3, 3}); };
    // rgb from `rgb`, alpha from `a`
    auto withAlpha = [&](Value *rgb, Value *a) {
      return builder.CreateInsertElement(rgb, builder.CreateExtractElement(a, builder.getInt32(3)),
                                         builder.getInt32(3));
    };
    switch (op) {
    case FF_TOP_BLENDTEXTUREALPHAPM: { // arg1 + arg2 * (1 - texture.a)
      auto *tex = texSamples[stage] ? texSamples[stage] : white;
      return saturate(builder, builder.CreateFAdd(arg1,
        builder.CreateFMul(arg2, builder.CreateFSub(one, alphaSplat(tex)))));
    }
    case FF_TOP_MODULATEALPHA_ADDCOLOR: // arg1.rgb + arg1.a * arg2.rgb
      return withAlpha(saturate(builder, builder.CreateFAdd(arg1,
        builder.CreateFMul(alphaSplat(arg1), arg2))), arg1);
    case FF_TOP_MODULATECOLOR_ADDALPHA: // arg1.rgb * arg2.rgb + arg1.a
      return withAlpha(saturate(builder, builder.CreateFAdd(builder.CreateFMul(arg1, arg2),
        alphaSplat(arg1))), arg1);
    case FF_TOP_MODULATEINVALPHA_ADDCOLOR: // (1 - arg1.a) * arg2.rgb + arg1.rgb
      return withAlpha(saturate(builder, builder.CreateFAdd(arg1,
        builder.CreateFMul(builder.CreateFSub(one, alphaSplat(arg1)), arg2))), arg1);
    case FF_TOP_MODULATEINVCOLOR_ADDALPHA: // (1 - arg1.rgb) * arg2.rgb + arg1.a
      return withAlpha(saturate(builder, builder.CreateFAdd(
        builder.CreateFMul(builder.CreateFSub(one, arg1), arg2), alphaSplat(arg1))), arg1);
    case FF_TOP_DOTPRODUCT3: { // 4 * dot3(arg1 - 0.5, arg2 - 0.5), replicated to all channels
      auto *half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 0.5));
      auto *a = builder.CreateFSub(arg1, half);
      auto *b = builder.CreateFSub(arg2, half);
      Value *d = nullptr;
      for (int c = 0; c < 3; c++) {
        auto *m = builder.CreateFMul(builder.CreateExtractElement(a, builder.getInt32(c)),
                                     builder.CreateExtractElement(b, builder.getInt32(c)));
        d = d ? builder.CreateFAdd(d, m) : m;
      }
      d = builder.CreateFMul(d, ConstantFP::get(floatTy, 4.0));
      return saturate(builder, builder.CreateVectorSplat(4, d));
    }
    case FF_TOP_MULTIPLYADD: // arg0 + arg1 * arg2
      return saturate(builder, builder.CreateFAdd(arg0, builder.CreateFMul(arg1, arg2)));
    case FF_TOP_LERP: // arg0 * arg1 + (1 - arg0) * arg2
      return saturate(builder, builder.CreateFAdd(builder.CreateFMul(arg0, arg1),
        builder.CreateFMul(builder.CreateFSub(one, arg0), arg2)));
    case FF_TOP_PREMODULATE:        // needs next stage's texture; approximate
    case FF_TOP_BUMPENVMAP:         // not supported: pass through current
    case FF_TOP_BUMPENVMAPLUMINANCE:
      return current;
    case FF_TOP_SELECTARG1:
      return arg1;
    case FF_TOP_SELECTARG2:
      return arg2;
    case FF_TOP_MODULATE:
      return builder.CreateFMul(arg1, arg2);
    case FF_TOP_MODULATE2X:
      return saturate(builder, builder.CreateFMul(
        builder.CreateFMul(arg1, arg2),
        ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 2.0))));
    case FF_TOP_MODULATE4X:
      return saturate(builder, builder.CreateFMul(
        builder.CreateFMul(arg1, arg2),
        ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 4.0))));
    case FF_TOP_ADD:
      return saturate(builder, builder.CreateFAdd(arg1, arg2));
    case FF_TOP_ADDSIGNED: {
      auto *half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 0.5));
      return saturate(builder, builder.CreateFSub(builder.CreateFAdd(arg1, arg2), half));
    }
    case FF_TOP_ADDSIGNED2X: {
      auto *half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 0.5));
      auto *two = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 2.0));
      return saturate(builder, builder.CreateFMul(
        builder.CreateFSub(builder.CreateFAdd(arg1, arg2), half), two));
    }
    case FF_TOP_SUBTRACT:
      return saturate(builder, builder.CreateFSub(arg1, arg2));
    case FF_TOP_ADDSMOOTH: {
      // arg1 + arg2 * (1 - arg1)
      auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 1.0));
      return saturate(builder, builder.CreateFAdd(arg1,
        builder.CreateFMul(arg2, builder.CreateFSub(one, arg1))));
    }
    case FF_TOP_BLENDDIFFUSEALPHA: {
      auto *da = builder.CreateShuffleVector(diffuse, {3, 3, 3, 3});
      auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 1.0));
      return builder.CreateFAdd(
        builder.CreateFMul(arg1, da),
        builder.CreateFMul(arg2, builder.CreateFSub(one, da)));
    }
    case FF_TOP_BLENDTEXTUREALPHA: {
      auto *tex = texSamples[stage] ? texSamples[stage] : current;
      auto *ta = builder.CreateShuffleVector(tex, {3, 3, 3, 3});
      auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 1.0));
      return builder.CreateFAdd(
        builder.CreateFMul(arg1, ta),
        builder.CreateFMul(arg2, builder.CreateFSub(one, ta)));
    }
    case FF_TOP_BLENDFACTORALPHA: {
      auto *fa = builder.CreateShuffleVector(texFactor, {3, 3, 3, 3});
      auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 1.0));
      return builder.CreateFAdd(
        builder.CreateFMul(arg1, fa),
        builder.CreateFMul(arg2, builder.CreateFSub(one, fa)));
    }
    case FF_TOP_BLENDCURRENTALPHA: {
      auto *ca = builder.CreateShuffleVector(current, {3, 3, 3, 3});
      auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(floatTy, 1.0));
      return builder.CreateFAdd(
        builder.CreateFMul(arg1, ca),
        builder.CreateFMul(arg2, builder.CreateFSub(one, ca)));
    }
    default:
      return arg1;
    }
  };

  for (uint32_t stage = 0; stage < 8; stage++) {
    auto &stg = key->stages[stage];
    if (stg.color_op == FF_TOP_DISABLE) break;

    // Color operation
    auto *carg0 = resolveArg(stg.color_arg0, stage);
    auto *carg1 = resolveArg(stg.color_arg1, stage);
    auto *carg2 = resolveArg(stg.color_arg2, stage);
    auto *colorResult = applyOp(stg.color_op, carg0, carg1, carg2, stage);

    // Alpha operation (DISABLE with colour enabled keeps the current alpha)
    Value *alphaResult = current;
    if (stg.alpha_op != FF_TOP_DISABLE) {
      auto *aarg0 = resolveArg(stg.alpha_arg0, stage);
      auto *aarg1 = resolveArg(stg.alpha_arg1, stage);
      auto *aarg2 = resolveArg(stg.alpha_arg2, stage);
      alphaResult = applyOp(stg.alpha_op, aarg0, aarg1, aarg2, stage);
    }

    // Combine: use color result RGB + alpha result A
    auto *rgb = colorResult;
    auto *a = builder.CreateExtractElement(alphaResult, builder.getInt32(3));
    auto *result = builder.CreateInsertElement(rgb, a, builder.getInt32(3));
    if ((stg.result_arg & 0x0F) == FF_TA_TEMP)
      temp = result;
    else
      current = result;
  }

  // Specular add
  if (key->specular_enable) {
    auto *sr = builder.CreateExtractElement(specular, builder.getInt32(0));
    auto *sg = builder.CreateExtractElement(specular, builder.getInt32(1));
    auto *sb = builder.CreateExtractElement(specular, builder.getInt32(2));
    auto *cr = builder.CreateFAdd(builder.CreateExtractElement(current, builder.getInt32(0)), sr);
    auto *cg = builder.CreateFAdd(builder.CreateExtractElement(current, builder.getInt32(1)), sg);
    auto *cb = builder.CreateFAdd(builder.CreateExtractElement(current, builder.getInt32(2)), sb);
    current = builder.CreateInsertElement(current, cr, builder.getInt32(0));
    current = builder.CreateInsertElement(current, cg, builder.getInt32(1));
    current = builder.CreateInsertElement(current, cb, builder.getInt32(2));
  }

  // Fog blending
  if (key->fog_enable && fog_in_idx != ~0u) {
    auto *fogVec = function->getArg(fog_in_idx);
    // D3D9 clamps the fog factor to [0,1]. The FF VS saturates its own fog, but a game vertex shader's
    // oFog arrives raw; unclamped values extrapolated past the fog colour (washed-out pale peds in GTA SA).
    Value *fogIn = builder.CreateExtractElement(fogVec, builder.getInt32(0));
    if (key->fog_coord_mode >= 1 && key->fog_coord_mode <= 3) {
      // FOG0 is a fog coordinate from an SM<3 VS: factor per FOGVERTEXMODE, params in c[2] = (start, end, density)
      auto *fogParams = builder.CreateLoad(float4Ty, builder.CreateConstGEP1_32(float4Ty, cbFloat4, 2));
      auto *fStart = builder.CreateExtractElement(fogParams, builder.getInt32(0));
      auto *fEnd = builder.CreateExtractElement(fogParams, builder.getInt32(1));
      auto *fDensity = builder.CreateExtractElement(fogParams, builder.getInt32(2));
      auto *log2e = ConstantFP::get(floatTy, 1.4426950408889634);
      llvm::raw_null_ostream fogNull;
      llvm::air::AIRBuilder fogAir(builder, fogNull);
      if (key->fog_coord_mode == 3) { // LINEAR
        fogIn = builder.CreateFDiv(builder.CreateFSub(fEnd, fogIn), builder.CreateFSub(fEnd, fStart));
      } else if (key->fog_coord_mode == 1) { // EXP: e^-(d*z)
        fogIn = fogAir.CreateFPUnOp(fogAir.exp2,
          builder.CreateFNeg(builder.CreateFMul(builder.CreateFMul(fDensity, fogIn), log2e)));
      } else { // EXP2: e^-((d*z)^2)
        auto *dz = builder.CreateFMul(fDensity, fogIn);
        fogIn = fogAir.CreateFPUnOp(fogAir.exp2,
          builder.CreateFNeg(builder.CreateFMul(builder.CreateFMul(dz, dz), log2e)));
      }
    }
    auto *fogFactorPS = saturate(builder, fogIn);
    auto *fogInfo = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, cbFloat4, 1));
    // fogColor = c[1].rgb
    auto *fcR = builder.CreateExtractElement(fogInfo, builder.getInt32(0));
    auto *fcG = builder.CreateExtractElement(fogInfo, builder.getInt32(1));
    auto *fcB = builder.CreateExtractElement(fogInfo, builder.getInt32(2));
    // current.rgb = mix(fogColor, current.rgb, fogFactor)
    auto *curR = builder.CreateExtractElement(current, builder.getInt32(0));
    auto *curG = builder.CreateExtractElement(current, builder.getInt32(1));
    auto *curB = builder.CreateExtractElement(current, builder.getInt32(2));
    auto *oneMinusFog = builder.CreateFSub(ConstantFP::get(floatTy, 1.0), fogFactorPS);
    curR = builder.CreateFAdd(builder.CreateFMul(curR, fogFactorPS), builder.CreateFMul(fcR, oneMinusFog));
    curG = builder.CreateFAdd(builder.CreateFMul(curG, fogFactorPS), builder.CreateFMul(fcG, oneMinusFog));
    curB = builder.CreateFAdd(builder.CreateFMul(curB, fogFactorPS), builder.CreateFMul(fcB, oneMinusFog));
    current = builder.CreateInsertElement(current, curR, builder.getInt32(0));
    current = builder.CreateInsertElement(current, curG, builder.getInt32(1));
    current = builder.CreateInsertElement(current, curB, builder.getInt32(2));
  }

  // Alpha test
  if (key->alpha_test_enable && key->alpha_test_func != FF_CMP_ALWAYS) {
    auto *fogInfo = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, cbFloat4, 1));
    auto *alphaRef = builder.CreateExtractElement(fogInfo, builder.getInt32(3));
    auto *alpha = builder.CreateExtractElement(current, builder.getInt32(3));

    Value *discard_cond = nullptr;
    switch (key->alpha_test_func) {
    case FF_CMP_NEVER:
      discard_cond = ConstantInt::getTrue(context);
      break;
    case FF_CMP_LESS:
      discard_cond = builder.CreateFCmpOGE(alpha, alphaRef); // discard if NOT less
      break;
    case FF_CMP_EQUAL:
      discard_cond = builder.CreateFCmpONE(alpha, alphaRef);
      break;
    case FF_CMP_LESSEQUAL:
      discard_cond = builder.CreateFCmpOGT(alpha, alphaRef);
      break;
    case FF_CMP_GREATER:
      discard_cond = builder.CreateFCmpOLE(alpha, alphaRef);
      break;
    case FF_CMP_NOTEQUAL:
      discard_cond = builder.CreateFCmpOEQ(alpha, alphaRef);
      break;
    case FF_CMP_GREATEREQUAL:
      discard_cond = builder.CreateFCmpOLT(alpha, alphaRef);
      break;
    default:
      break;
    }

    if (discard_cond) {
      auto *discard_bb = BasicBlock::Create(context, "discard", function);
      auto *continue_bb = BasicBlock::Create(context, "continue", function);
      builder.CreateCondBr(discard_cond, discard_bb, continue_bb);

      builder.SetInsertPoint(discard_bb);
      // AIR uses @air.discard_fragment
      auto discardFnCallee = module.getOrInsertFunction(
        "air.discard_fragment", FunctionType::get(Type::getVoidTy(context), false));
      builder.CreateCall(discardFnCallee);
      builder.CreateBr(continue_bb);

      builder.SetInsertPoint(continue_bb);
    }
  }

  // Return
  auto retTy = function->getReturnType();
  Value *retVal = UndefValue::get(retTy);
  retVal = builder.CreateInsertValue(retVal, current, {rt0_idx});
  builder.CreateRet(retVal);

  module.getOrInsertNamedMetadata("air.fragment")->addOperand(function_md);
}

extern "C" {

AIRCONV_API int D3D9FFCompileVS(
  const struct D3D9_FF_VS_KEY *pKey,
  const struct D3D9_FF_VS_ELEMENT *pElements,
  uint32_t numElements,
  uint32_t slotMask,
  const char *FunctionName,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  if (ppError) *ppError = nullptr;
  if (!pKey || !ppBitcode) return 1;

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  {
    auto *arg = pArgs;
    while (arg) {
      if (arg->type == SM50_SHADER_COMMON)
        metal_version = ((SM50_SHADER_COMMON_DATA *)arg)->metal_version;
      arg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next;
    }
  }

  LLVMContext context;
  context.setOpaquePointers(false);

  auto pModule = std::make_unique<Module>("ff_vs.air", context);
  dxmt::initializeModule(*pModule);

  compileFFVertexShader(pKey, pElements, numElements, slotMask,
                        FunctionName, metal_version, context, *pModule);

  dxmt::runOptimizationPasses(*pModule);

  auto *compiled = new FFCompiledBitcodeInternal();
  raw_svector_ostream OS(compiled->vec);
  dxmt::metallib::MetallibWriter writer;
  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int D3D9FFCompilePS(
  const struct D3D9_FF_PS_KEY *pKey,
  uint8_t texcoordCount,
  const char *FunctionName,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  if (ppError) *ppError = nullptr;
  if (!pKey || !ppBitcode) return 1;

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  {
    auto *arg = pArgs;
    while (arg) {
      if (arg->type == SM50_SHADER_COMMON)
        metal_version = ((SM50_SHADER_COMMON_DATA *)arg)->metal_version;
      arg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next;
    }
  }

  LLVMContext context;
  context.setOpaquePointers(false);

  auto pModule = std::make_unique<Module>("ff_ps.air", context);
  dxmt::initializeModule(*pModule);

  compileFFPixelShader(pKey, texcoordCount, FunctionName, metal_version,
                       context, *pModule);

  dxmt::runOptimizationPasses(*pModule);

  auto *compiled = new FFCompiledBitcodeInternal();
  raw_svector_ostream OS(compiled->vec);
  dxmt::metallib::MetallibWriter writer;
  writer.Write(*pModule, OS);
  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

} // extern "C"

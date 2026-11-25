#include "dxso_compiler.hpp"
#include "air_signature.hpp"
#include "air_type.hpp"
#include "airconv_context.hpp"
#include "dxbc_converter.hpp"
#include "metallib_writer.hpp"
#include "nt/air_builder.hpp"
#include <set>

// MTLAttributeFormat values (from Metal headers)
enum {
  DxsoAttrFormatUChar4                = 3,
  DxsoAttrFormatUShort2               = 13,
  DxsoAttrFormatUShort4               = 15,
  DxsoAttrFormatShort2                = 16,
  DxsoAttrFormatShort4                = 18,
  DxsoAttrFormatUShort2Normalized     = 19,
  DxsoAttrFormatUShort4Normalized     = 21,
  DxsoAttrFormatShort2Normalized      = 22,
  DxsoAttrFormatShort4Normalized      = 24,
  DxsoAttrFormatHalf2                 = 25,
  DxsoAttrFormatHalf4                 = 27,
  DxsoAttrFormatUChar4Normalized      = 9,
  DxsoAttrFormatFloat                 = 28,
  DxsoAttrFormatFloat2                = 29,
  DxsoAttrFormatFloat3                = 30,
  DxsoAttrFormatFloat4                = 31,
  DxsoAttrFormatUChar4Normalized_BGRA = 42,
};

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace dxmt::air;
using namespace dxmt::dxso;

namespace dxmt::dxso {

// SM50ErrorInternal and SM50CompiledBitcodeInternal are defined in dxbc_converter.cpp
// We reuse them here
struct DxsoErrorInternal {
  SmallVector<char, 256> buf;
};
struct DxsoCompiledBitcodeInternal {
  SmallVector<char, 0> vec;
};

static void compileVertexShader(
  DxsoShaderInternal *shader,
  const char *functionName,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  LLVMContext &context,
  Module &module
) {
  AirType types(context);
  FunctionSignatureBuilder func_sig;

  // Parse compilation arguments for input layout
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  SM50_SHADER_COMMON_DATA *common = nullptr;
  {
    auto *arg = pArgs;
    while (arg) {
      if (arg->type == SM50_SHADER_IA_INPUT_LAYOUT)
        ia_layout = (SM50_SHADER_IA_INPUT_LAYOUT_DATA *)arg;
      if (arg->type == SM50_SHADER_COMMON)
        common = (SM50_SHADER_COMMON_DATA *)arg;
      arg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next;
    }
  }

  // SM2 VS uses separate register types (RasterizerOut, AttributeOut, TexcoordOut)
  // that can share the same register number. Map them to non-overlapping output slots.
  // SM3 uses only Output (==TexcoordOut) with unique register numbers, so no remapping needed.
  bool sm2VS = shader->info.majorVersion < 3;
  auto mapOutputSlot = [sm2VS](DxsoRegisterType type, uint32_t num) -> uint32_t {
    if (!sm2VS) return num;
    switch (type) {
    case DxsoRegisterType::RasterizerOut: return num;       // 0-1 (oPos, oFog)
    case DxsoRegisterType::AttributeOut: return 2 + num;    // 2-3 (oD0, oD1)
    case DxsoRegisterType::Output:       return 4 + num;    // 4-11 (oT0-oT7)
    default: return num;
    }
  };

  // For each input DCL, set up vertex pulling via the IA input layout
  // The input layout elements tell us which register maps to which vertex buffer slot
  uint32_t max_input_reg = 0;
  for (auto &dcl : shader->inputDecls) {
    max_input_reg = std::max(max_input_reg, dcl.reg + 1);
  }
  uint32_t max_output_reg = 0;
  for (auto &dcl : shader->outputDecls) {
    max_output_reg = std::max(max_output_reg, dcl.reg + 1);
  }
  // Ensure at least some registers for temp usage
  if (max_output_reg < 2) max_output_reg = 2;

  // Define vertex shader outputs based on DCL semantics
  // Position output
  uint32_t position_out_idx = ~0u;
  std::vector<std::pair<uint32_t, uint32_t>> user_out_indices; // (reg, output_idx)
  auto defineVsOutput = [&](const DxsoShaderInternal::OutputDecl &dcl) {
    std::string semantic;
    switch (dcl.usage) {
    case DxsoUsage::Color: semantic = "COLOR" + std::to_string(dcl.usageIndex); break;
    case DxsoUsage::Texcoord: semantic = "TEXCOORD" + std::to_string(dcl.usageIndex); break;
    case DxsoUsage::Fog: semantic = "FOG" + std::to_string(dcl.usageIndex); break;
    case DxsoUsage::PointSize: semantic = "PSIZE" + std::to_string(dcl.usageIndex); break;
    default: semantic = "USER" + std::to_string(dcl.reg); break;
    }
    auto idx = func_sig.DefineOutput(OutputVertex{
      .user = semantic, .type = msl_float4,
    });
    user_out_indices.push_back({dcl.reg, idx});
  };
  // Position first
  for (auto &dcl : shader->outputDecls) {
    if (dcl.usage == DxsoUsage::Position && dcl.usageIndex == 0)
      position_out_idx = func_sig.DefineOutput(OutputPosition{.type = msl_float4});
  }
  // Non-color outputs, then color outputs (keeps color at end to match PS order)
  for (auto &dcl : shader->outputDecls) {
    if (dcl.usage == DxsoUsage::Position && dcl.usageIndex == 0) continue;
    if (dcl.usage == DxsoUsage::Color) continue;
    defineVsOutput(dcl);
  }
  for (auto &dcl : shader->outputDecls) {
    if (dcl.usage != DxsoUsage::Color) continue;
    defineVsOutput(dcl);
  }
  if (position_out_idx == ~0u) {
    // If no explicit position DCL, assume o0 = position (SM2 and below)
    position_out_idx = func_sig.DefineOutput(OutputPosition{.type = msl_float4});
    max_output_reg = std::max(max_output_reg, 1u);
  }

  // Define vertex buffer table input (same as DXBC pull_vertex_input)
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

  // Constants buffer at index 17
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

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  if (common) metal_version = common->metal_version;

  // Create function
  auto [function, function_md] = func_sig.CreateFunction(
    functionName, context, module, 0, false
  );

  auto entry_bb = BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = BasicBlock::Create(context, "epilogue", function);
  IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nullOS;
  llvm::air::AIRBuilder air(builder, nullOS);

  dxbc::setup_metal_version(module, metal_version);

  // Alloca register files
  auto *float4Ty = types._float4;
  auto *int4Ty = FixedVectorType::get(builder.getInt32Ty(), 4);
  auto *inputArray = builder.CreateAlloca(ArrayType::get(float4Ty, max_input_reg ? max_input_reg : 1));
  auto *outputArray = builder.CreateAlloca(ArrayType::get(float4Ty, max_output_reg));
  auto *tempArray = builder.CreateAlloca(ArrayType::get(float4Ty, 32));
  auto *addrReg = builder.CreateAlloca(int4Ty, nullptr, "addr_reg");
  builder.CreateStore(ConstantAggregateZero::get(int4Ty), addrReg);

  // Zero-initialize output registers
  for (uint32_t i = 0; i < max_output_reg; i++) {
    auto *ptr = builder.CreateGEP(
      ArrayType::get(float4Ty, max_output_reg), outputArray,
      {builder.getInt32(0), builder.getInt32(i)});
    builder.CreateStore(ConstantAggregateZero::get(float4Ty), ptr);
  }

  // Pull vertex inputs using the IA layout
  auto *vertex_id_with_base = function->getArg(vertex_id_idx);

  if (ia_layout && max_input_reg > 0) {
    auto *vbuf_table_raw = function->getArg(vbuf_table_idx);
    auto *vbEntryTy = types._dxmt_vertex_buffer_entry;
    auto *vbuf_table = builder.CreateBitCast(
      vbuf_table_raw,
      vbEntryTy->getPointerTo((uint32_t)AddressSpace::constant));

    for (auto &inputDcl : shader->inputDecls) {
      // Find matching IA element
      for (uint32_t i = 0; i < ia_layout->num_elements; i++) {
        if (ia_layout->elements[i].reg == inputDcl.reg) {
          auto &elem = ia_layout->elements[i];
          // Calculate vertex buffer entry index from slot and slot_mask
          unsigned shift = 32u - elem.slot;
          unsigned vb_entry_idx =
            elem.slot ? __builtin_popcount((ia_layout->slot_mask << shift) >> shift) : 0;

          auto *vb_entry = builder.CreateLoad(
            vbEntryTy,
            builder.CreateConstGEP1_32(vbEntryTy, vbuf_table, vb_entry_idx));
          auto *base_addr = builder.CreateExtractValue(vb_entry, {0});
          auto *stride = builder.CreateExtractValue(vb_entry, {1});
          auto *byte_offset = builder.CreateAdd(
            builder.CreateMul(stride, vertex_id_with_base),
            builder.getInt32(elem.aligned_byte_offset));

          // Pull vec4 based on format
          // For simplicity, use direct byte-address load for float formats
          auto *byte_ptr = builder.CreateIntToPtr(
            builder.CreateAdd(
              builder.CreatePtrToInt(base_addr, builder.getInt64Ty()),
              builder.CreateZExt(byte_offset, builder.getInt64Ty())),
            types._float->getPointerTo((uint32_t)AddressSpace::device));

          Value *result;
          switch (elem.format) {
          case DxsoAttrFormatUChar4Normalized:
          case DxsoAttrFormatUChar4Normalized_BGRA: {
            // Load 4 bytes, normalize to [0,1], and swizzle BGRA→RGBA if needed
            auto *byte_ptr_i8 = builder.CreateBitCast(byte_ptr,
              builder.getInt8Ty()->getPointerTo((uint32_t)AddressSpace::device));
            Value *channels[4];
            for (int c = 0; c < 4; c++) {
              auto *byte_val = builder.CreateLoad(builder.getInt8Ty(),
                builder.CreateConstGEP1_32(builder.getInt8Ty(), byte_ptr_i8, c));
              channels[c] = builder.CreateFDiv(
                builder.CreateUIToFP(byte_val, types._float),
                ConstantFP::get(types._float, 255.0));
            }
            if (elem.format == DxsoAttrFormatUChar4Normalized_BGRA) {
              // BGRA → RGBA: swap R and B
              result = UndefValue::get(float4Ty);
              result = builder.CreateInsertElement(result, channels[2], builder.getInt32(0)); // B→R
              result = builder.CreateInsertElement(result, channels[1], builder.getInt32(1)); // G→G
              result = builder.CreateInsertElement(result, channels[0], builder.getInt32(2)); // R→B
              result = builder.CreateInsertElement(result, channels[3], builder.getInt32(3)); // A→A
            } else {
              result = UndefValue::get(float4Ty);
              result = builder.CreateInsertElement(result, channels[0], builder.getInt32(0));
              result = builder.CreateInsertElement(result, channels[1], builder.getInt32(1));
              result = builder.CreateInsertElement(result, channels[2], builder.getInt32(2));
              result = builder.CreateInsertElement(result, channels[3], builder.getInt32(3));
            }
            break;
          }
          case DxsoAttrFormatFloat: {
            auto *f0 = builder.CreateLoad(types._float, byte_ptr);
            result = ConstantVector::get({
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 1.0)});
            result = builder.CreateInsertElement(result, f0, builder.getInt32(0));
            break;
          }
          case DxsoAttrFormatFloat2: {
            auto *f0 = builder.CreateLoad(types._float, byte_ptr);
            auto *f1 = builder.CreateLoad(types._float,
              builder.CreateConstGEP1_32(types._float, byte_ptr, 1));
            result = ConstantVector::get({
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 1.0)});
            result = builder.CreateInsertElement(result, f0, builder.getInt32(0));
            result = builder.CreateInsertElement(result, f1, builder.getInt32(1));
            break;
          }
          case DxsoAttrFormatFloat3: {
            auto *f0 = builder.CreateLoad(types._float, byte_ptr);
            auto *f1 = builder.CreateLoad(types._float,
              builder.CreateConstGEP1_32(types._float, byte_ptr, 1));
            auto *f2 = builder.CreateLoad(types._float,
              builder.CreateConstGEP1_32(types._float, byte_ptr, 2));
            result = ConstantVector::get({
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 1.0)});
            result = builder.CreateInsertElement(result, f0, builder.getInt32(0));
            result = builder.CreateInsertElement(result, f1, builder.getInt32(1));
            result = builder.CreateInsertElement(result, f2, builder.getInt32(2));
            break;
          }
          case DxsoAttrFormatFloat4: {
            auto *vec_ptr = builder.CreateBitCast(byte_ptr, float4Ty->getPointerTo((uint32_t)AddressSpace::device));
            result = builder.CreateLoad(float4Ty, vec_ptr);
            break;
          }
          case DxsoAttrFormatUChar4:
          case DxsoAttrFormatUShort2:
          case DxsoAttrFormatUShort4:
          case DxsoAttrFormatShort2:
          case DxsoAttrFormatShort4:
          case DxsoAttrFormatUShort2Normalized:
          case DxsoAttrFormatUShort4Normalized:
          case DxsoAttrFormatShort2Normalized:
          case DxsoAttrFormatShort4Normalized:
          case DxsoAttrFormatHalf2:
          case DxsoAttrFormatHalf4: {
            // Integer / normalized / half formats: convert per component, missing components = (0,0,0,1)
            // like D3D9. (Previously these fell into the float4 fallback below: e.g. UBYTE4 blend indices
            // were read as garbage floats, so skinned peds all used bone 0 = T-pose.)
            unsigned count = 4;
            Type *compTy = builder.getInt16Ty();
            bool isSigned = false, normalize = false, isHalf = false;
            switch (elem.format) {
            case DxsoAttrFormatUChar4: compTy = builder.getInt8Ty(); break;
            case DxsoAttrFormatUShort2: count = 2; break;
            case DxsoAttrFormatUShort4: break;
            case DxsoAttrFormatShort2: count = 2; isSigned = true; break;
            case DxsoAttrFormatShort4: isSigned = true; break;
            case DxsoAttrFormatUShort2Normalized: count = 2; normalize = true; break;
            case DxsoAttrFormatUShort4Normalized: normalize = true; break;
            case DxsoAttrFormatShort2Normalized: count = 2; isSigned = true; normalize = true; break;
            case DxsoAttrFormatShort4Normalized: isSigned = true; normalize = true; break;
            case DxsoAttrFormatHalf2: count = 2; isHalf = true; break;
            case DxsoAttrFormatHalf4: isHalf = true; break;
            default: break;
            }
            auto *comp_ptr = builder.CreateBitCast(byte_ptr, compTy->getPointerTo((uint32_t)AddressSpace::device));
            result = ConstantVector::get({
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 0.0),
              ConstantFP::get(types._float, 1.0)});
            for (unsigned c = 0; c < count; c++) {
              Value *raw = builder.CreateLoad(compTy, builder.CreateConstGEP1_32(compTy, comp_ptr, c));
              Value *f;
              if (isHalf) {
                f = builder.CreateFPExt(builder.CreateBitCast(raw, builder.getHalfTy()), types._float);
              } else if (isSigned) {
                f = builder.CreateSIToFP(raw, types._float);
                if (normalize) { // D3D9 SHORT*N: x / 32767, clamped to -1
                  f = builder.CreateFDiv(f, ConstantFP::get(types._float, 32767.0));
                  auto *minus1 = ConstantFP::get(types._float, -1.0);
                  f = builder.CreateSelect(builder.CreateFCmpOLT(f, minus1), minus1, f);
                }
              } else {
                f = builder.CreateUIToFP(raw, types._float);
                if (normalize)
                  f = builder.CreateFDiv(f, ConstantFP::get(types._float, 65535.0));
              }
              result = builder.CreateInsertElement(result, f, builder.getInt32(c));
            }
            break;
          }
          default: {
            // Fallback: load as float4 (works for Float, Float2, Float3, Float4)
            auto *vec_ptr = builder.CreateBitCast(byte_ptr, float4Ty->getPointerTo((uint32_t)AddressSpace::device));
            result = builder.CreateLoad(float4Ty, vec_ptr);
            break;
          }
          }

          auto *dst_ptr = builder.CreateGEP(
            ArrayType::get(float4Ty, max_input_reg), inputArray,
            {builder.getInt32(0), builder.getInt32(inputDcl.reg)});
          builder.CreateStore(result, dst_ptr);
          break;
        }
      }
    }
  }

  // Constant buffer pointer
  auto *constBufPtr = function->getArg(const_buf_idx);
  auto *constBufFloat4 = builder.CreateBitCast(
    constBufPtr, float4Ty->getPointerTo((uint32_t)AddressSpace::constant));

  // Pre-scan for Def instructions to collect inline constants
  std::unordered_map<uint32_t, std::array<float, 4>> defConstants;
  {
    DxsoDecoder defScan(shader->fullTokens.data());
    DxsoInstructionContext defCtx;
    while (defScan.decodeInstruction(defCtx)) {
      if (defCtx.instruction.opcode == DxsoOpcode::Def) {
        defConstants[defCtx.dst.id.num] = {
          defCtx.def.float32[0], defCtx.def.float32[1],
          defCtx.def.float32[2], defCtx.def.float32[3]
        };
      }
    }
  }

  // Now decode and compile instructions
  DxsoDecoder decoder(shader->fullTokens.data());
  DxsoInstructionContext inst;

  // Lambda to load a source register as float4
  auto loadSrc = [&](const DxsoRegister &reg) -> Value * {
    Value *val = nullptr;
    switch (reg.id.type) {
    case DxsoRegisterType::Temp:
      val = builder.CreateLoad(float4Ty, builder.CreateGEP(
        ArrayType::get(float4Ty, 32), tempArray,
        {builder.getInt32(0), builder.getInt32(reg.id.num)}));
      break;
    case DxsoRegisterType::Input:
      val = builder.CreateLoad(float4Ty, builder.CreateGEP(
        ArrayType::get(float4Ty, max_input_reg ? max_input_reg : 1), inputArray,
        {builder.getInt32(0), builder.getInt32(reg.id.num)}));
      break;
    case DxsoRegisterType::Const: {
      if (reg.hasRelative) {
        // Dynamic indexing: c[a0.comp + N]
        auto *addr = builder.CreateLoad(int4Ty, addrReg);
        auto *offset = builder.CreateExtractElement(addr, builder.getInt32(reg.relativeSwizzle[0]));
        auto *index = builder.CreateAdd(offset, builder.getInt32(reg.id.num));
        // Clamp to valid constant register range
        index = builder.CreateBinaryIntrinsic(Intrinsic::smax, index, builder.getInt32(0));
        index = builder.CreateBinaryIntrinsic(Intrinsic::smin, index, builder.getInt32(255));
        val = builder.CreateLoad(float4Ty,
          builder.CreateGEP(float4Ty, constBufFloat4, index));
      } else {
        // Check if this constant was defined inline via Def instruction
        auto defIt = defConstants.find(reg.id.num);
        if (defIt != defConstants.end()) {
          auto &v = defIt->second;
          val = ConstantVector::get({
            ConstantFP::get(types._float, v[0]),
            ConstantFP::get(types._float, v[1]),
            ConstantFP::get(types._float, v[2]),
            ConstantFP::get(types._float, v[3])
          });
        } else {
          val = builder.CreateLoad(float4Ty,
            builder.CreateConstGEP1_32(float4Ty, constBufFloat4, reg.id.num));
        }
      }
      break;
    }
    case DxsoRegisterType::Output:
    case DxsoRegisterType::RasterizerOut:
    case DxsoRegisterType::AttributeOut:
      val = builder.CreateLoad(float4Ty, builder.CreateGEP(
        ArrayType::get(float4Ty, max_output_reg), outputArray,
        {builder.getInt32(0), builder.getInt32(mapOutputSlot(reg.id.type, reg.id.num))}));
      break;
    default:
      val = ConstantAggregateZero::get(float4Ty);
      break;
    }

    // Apply swizzle
    auto swz = reg.swizzle;
    if (swz.data != 0xe4) { // not identity
      Value *swizzled = UndefValue::get(float4Ty);
      for (uint32_t i = 0; i < 4; i++) {
        swizzled = builder.CreateInsertElement(swizzled,
          builder.CreateExtractElement(val, builder.getInt32(swz[i])),
          builder.getInt32(i));
      }
      val = swizzled;
    }

    // Apply modifier
    switch (reg.modifier) {
    case DxsoRegModifier::None:
      break;
    case DxsoRegModifier::Neg:
      val = builder.CreateFNeg(val);
      break;
    case DxsoRegModifier::Abs:
      val = air.CreateFPUnOp(air.fabs, val);
      break;
    case DxsoRegModifier::AbsNeg:
      val = builder.CreateFNeg(air.CreateFPUnOp(air.fabs, val));
      break;
    case DxsoRegModifier::Comp: { // 1 - x
      auto one = ConstantFP::get(types._float, 1.0);
      auto ones = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      val = builder.CreateFSub(ones, val);
      break;
    }
    case DxsoRegModifier::X2: // 2 * x
      val = builder.CreateFMul(val, ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0)));
      break;
    case DxsoRegModifier::X2Neg: // -(2 * x)
      val = builder.CreateFNeg(builder.CreateFMul(val, ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0))));
      break;
    case DxsoRegModifier::Bias: { // x - 0.5
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      val = builder.CreateFSub(val, half);
      break;
    }
    case DxsoRegModifier::BiasNeg: { // -(x - 0.5)
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      val = builder.CreateFNeg(builder.CreateFSub(val, half));
      break;
    }
    case DxsoRegModifier::Sign: { // 2 * (x - 0.5)
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      auto two = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0));
      val = builder.CreateFMul(builder.CreateFSub(val, half), two);
      break;
    }
    case DxsoRegModifier::SignNeg: { // -(2 * (x - 0.5))
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      auto two = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0));
      val = builder.CreateFNeg(builder.CreateFMul(builder.CreateFSub(val, half), two));
      break;
    }
    default:
      fprintf(stderr, "DXSO VS: unhandled source modifier %u\n", (unsigned)reg.modifier);
      break;
    }
    return val;
  };

  // Lambda to store to destination register with write mask
  auto storeDst = [&](const DxsoRegister &reg, Value *val) {
    Value *dst_ptr = nullptr;
    Value *old_val = nullptr;
    auto arrayTy32 = ArrayType::get(float4Ty, 32);
    auto arrayTyOut = ArrayType::get(float4Ty, max_output_reg);

    switch (reg.id.type) {
    case DxsoRegisterType::Temp:
      dst_ptr = builder.CreateGEP(arrayTy32, tempArray,
        {builder.getInt32(0), builder.getInt32(reg.id.num)});
      old_val = builder.CreateLoad(float4Ty, dst_ptr);
      break;
    case DxsoRegisterType::Output: // same as TexcoordOut = 6
    case DxsoRegisterType::AttributeOut:
    case DxsoRegisterType::ColorOut:
    case DxsoRegisterType::RasterizerOut:
      dst_ptr = builder.CreateGEP(arrayTyOut, outputArray,
        {builder.getInt32(0), builder.getInt32(mapOutputSlot(reg.id.type, reg.id.num))});
      old_val = builder.CreateLoad(float4Ty, dst_ptr);
      break;
    default:
      return; // can't write
    }

    // Apply write mask
    if (reg.mask.mask != 0xf) {
      for (uint32_t i = 0; i < 4; i++) {
        if (reg.mask[i]) {
          old_val = builder.CreateInsertElement(old_val,
            builder.CreateExtractElement(val, builder.getInt32(i)),
            builder.getInt32(i));
        }
      }
      val = old_val;
    }

    // Saturate
    if (reg.saturate) {
      auto *zero = ConstantFP::get(types._float, 0.0);
      auto *one = ConstantFP::get(types._float, 1.0);
      auto *zeroVec = ConstantVector::getSplat(ElementCount::getFixed(4), zero);
      auto *oneVec = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      val = air.CreateFPBinOp(air.fmax, val, zeroVec);
      val = air.CreateFPBinOp(air.fmin, val, oneVec);
    }

    builder.CreateStore(val, dst_ptr);
  };

  // Compile instructions
  while (decoder.decodeInstruction(inst)) {
    switch (inst.instruction.opcode) {
    case DxsoOpcode::Dcl:
    case DxsoOpcode::Comment:
      break; // already handled during initialize

    case DxsoOpcode::Def: {
      // Store constant definition into temp const register
      // Actually, def stores into the constant register file — we handle this by
      // loading the defined value when the constant is referenced
      // For simplicity, store to a local alloca (const reg is c0-c255)
      // We'll handle def by pre-loading into constant buffer conceptually
      // Actually: def cN, x, y, z, w just means "c[N] = {x,y,z,w}" locally
      // We need a separate mechanism... For now, skip and handle in loadSrc
      break;
    }

    case DxsoOpcode::Mov: {
      auto *src = loadSrc(inst.src[0]);
      storeDst(inst.dst, src);
      break;
    }

    case DxsoOpcode::Mova: {
      auto *src = loadSrc(inst.src[0]);
      // Round to nearest integer (D3D9 mova semantics)
      auto *rounded = air.CreateFPUnOp(air.rint, src);
      auto *asInt = builder.CreateFPToSI(rounded, int4Ty);
      builder.CreateStore(asInt, addrReg);
      break;
    }

    case DxsoOpcode::Add: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      storeDst(inst.dst, builder.CreateFAdd(a, b));
      break;
    }

    case DxsoOpcode::Sub: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      storeDst(inst.dst, builder.CreateFSub(a, b));
      break;
    }

    case DxsoOpcode::Mul: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      storeDst(inst.dst, builder.CreateFMul(a, b));
      break;
    }

    case DxsoOpcode::Mad: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      auto *c = loadSrc(inst.src[2]);
      storeDst(inst.dst, builder.CreateFAdd(builder.CreateFMul(a, b), c));
      break;
    }

    case DxsoOpcode::Dp3: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      auto *mul = builder.CreateFMul(a, b);
      auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
      auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
      auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
      auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), z);
      auto *result = builder.CreateVectorSplat(4, dot);
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::Dp4: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      auto *mul = builder.CreateFMul(a, b);
      auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
      auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
      auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
      auto *w = builder.CreateExtractElement(mul, builder.getInt32(3));
      auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), builder.CreateFAdd(z, w));
      auto *result = builder.CreateVectorSplat(4, dot);
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::Rcp: {
      auto *src = loadSrc(inst.src[0]);
      auto *one = ConstantFP::get(types._float, 1.0);
      auto *oneVec = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      storeDst(inst.dst, builder.CreateFDiv(oneVec, src));
      break;
    }

    case DxsoOpcode::Rsq: {
      // D3D9: rsq operates on abs(src)
      auto *src = loadSrc(inst.src[0]);
      storeDst(inst.dst, air.CreateFPUnOp(air.rsqrt, air.CreateFPUnOp(air.fabs, src)));
      break;
    }

    case DxsoOpcode::Min: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      storeDst(inst.dst, air.CreateFPBinOp(air.fmin, a, b));
      break;
    }

    case DxsoOpcode::Max: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      storeDst(inst.dst, air.CreateFPBinOp(air.fmax, a, b));
      break;
    }

    case DxsoOpcode::Slt: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      auto *cmp = builder.CreateFCmpOLT(a, b);
      auto *one = ConstantFP::get(types._float, 1.0);
      auto *zero = ConstantFP::get(types._float, 0.0);
      auto *oneVec = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      auto *zeroVec = ConstantVector::getSplat(ElementCount::getFixed(4), zero);
      storeDst(inst.dst, builder.CreateSelect(cmp, oneVec, zeroVec));
      break;
    }

    case DxsoOpcode::Sge: {
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      auto *cmp = builder.CreateFCmpOGE(a, b);
      auto *one = ConstantFP::get(types._float, 1.0);
      auto *zero = ConstantFP::get(types._float, 0.0);
      auto *oneVec = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      auto *zeroVec = ConstantVector::getSplat(ElementCount::getFixed(4), zero);
      storeDst(inst.dst, builder.CreateSelect(cmp, oneVec, zeroVec));
      break;
    }

    case DxsoOpcode::Pow: {
      // D3D9: pow(base, exp) where base <= 0 returns 0
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      auto *zeroVec = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.0));
      auto *result = air.CreateFPUnOp(air.exp2, builder.CreateFMul(b, air.CreateFPUnOp(air.log2, a)));
      auto *basePositive = builder.CreateFCmpOGT(a, zeroVec);
      storeDst(inst.dst, builder.CreateSelect(basePositive, result, zeroVec));
      break;
    }

    case DxsoOpcode::Log: {
      // D3D9: log operates on abs(src)
      auto *src = loadSrc(inst.src[0]);
      storeDst(inst.dst, air.CreateFPUnOp(air.log2, air.CreateFPUnOp(air.fabs, src)));
      break;
    }

    case DxsoOpcode::Exp: {
      auto *src = loadSrc(inst.src[0]);
      storeDst(inst.dst, air.CreateFPUnOp(air.exp2, src));
      break;
    }

    case DxsoOpcode::Lrp: {
      // lrp dst, s0, s1, s2 => dst = s2 + s0 * (s1 - s2)
      auto *s0 = loadSrc(inst.src[0]);
      auto *s1 = loadSrc(inst.src[1]);
      auto *s2 = loadSrc(inst.src[2]);
      storeDst(inst.dst, builder.CreateFAdd(s2, builder.CreateFMul(s0, builder.CreateFSub(s1, s2))));
      break;
    }

    case DxsoOpcode::Frc: {
      auto *src = loadSrc(inst.src[0]);
      auto *floor = air.CreateFPUnOp(air.floor, src);
      storeDst(inst.dst, builder.CreateFSub(src, floor));
      break;
    }

    case DxsoOpcode::Nrm: {
      auto *src = loadSrc(inst.src[0]);
      auto *mul = builder.CreateFMul(src, src);
      auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
      auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
      auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
      auto *dot3 = builder.CreateFAdd(builder.CreateFAdd(x, y), z);
      auto *invLen = air.CreateFPUnOp(air.rsqrt, dot3);
      auto *invLenVec = builder.CreateVectorSplat(4, invLen);
      storeDst(inst.dst, builder.CreateFMul(src, invLenVec));
      break;
    }

    case DxsoOpcode::Crs: {
      // cross product: dst = s0 × s1
      auto *a = loadSrc(inst.src[0]);
      auto *b = loadSrc(inst.src[1]);
      auto *ax = builder.CreateExtractElement(a, builder.getInt32(0));
      auto *ay = builder.CreateExtractElement(a, builder.getInt32(1));
      auto *az = builder.CreateExtractElement(a, builder.getInt32(2));
      auto *bx = builder.CreateExtractElement(b, builder.getInt32(0));
      auto *by = builder.CreateExtractElement(b, builder.getInt32(1));
      auto *bz = builder.CreateExtractElement(b, builder.getInt32(2));
      auto *rx = builder.CreateFSub(builder.CreateFMul(ay, bz), builder.CreateFMul(az, by));
      auto *ry = builder.CreateFSub(builder.CreateFMul(az, bx), builder.CreateFMul(ax, bz));
      auto *rz = builder.CreateFSub(builder.CreateFMul(ax, by), builder.CreateFMul(ay, bx));
      Value *result = UndefValue::get(float4Ty);
      result = builder.CreateInsertElement(result, rx, builder.getInt32(0));
      result = builder.CreateInsertElement(result, ry, builder.getInt32(1));
      result = builder.CreateInsertElement(result, rz, builder.getInt32(2));
      result = builder.CreateInsertElement(result, ConstantFP::get(types._float, 0.0), builder.getInt32(3));
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::Abs: {
      auto *src = loadSrc(inst.src[0]);
      storeDst(inst.dst, air.CreateFPUnOp(air.fabs, src));
      break;
    }

    case DxsoOpcode::SinCos: {
      // sincos dst, src — dst.x = cos(src.x), dst.y = sin(src.x)
      auto *src = loadSrc(inst.src[0]);
      auto *angle = builder.CreateExtractElement(src, builder.getInt32(0));
      auto *cosVal = air.CreateFPUnOp(air.cos, angle);
      auto *sinVal = air.CreateFPUnOp(air.sin, angle);
      Value *result = ConstantAggregateZero::get(float4Ty);
      result = builder.CreateInsertElement(result, cosVal, builder.getInt32(0));
      result = builder.CreateInsertElement(result, sinVal, builder.getInt32(1));
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::Cmp: {
      // cmp dst, s0, s1, s2 => dst = (s0 >= 0) ? s1 : s2
      auto *s0 = loadSrc(inst.src[0]);
      auto *s1 = loadSrc(inst.src[1]);
      auto *s2 = loadSrc(inst.src[2]);
      auto *zero = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.0));
      auto *cmp = builder.CreateFCmpOGE(s0, zero);
      storeDst(inst.dst, builder.CreateSelect(cmp, s1, s2));
      break;
    }

    case DxsoOpcode::M4x4: {
      // M4x4 dst, src, matrix_start
      // dst.x = dp4(src, c[n+0])
      // dst.y = dp4(src, c[n+1])
      // dst.z = dp4(src, c[n+2])
      // dst.w = dp4(src, c[n+3])
      auto *src = loadSrc(inst.src[0]);
      Value *result = UndefValue::get(float4Ty);
      for (uint32_t row = 0; row < 4; row++) {
        DxsoRegister rowReg = inst.src[1];
        rowReg.id.num += row;
        auto *matRow = loadSrc(rowReg);
        auto *mul = builder.CreateFMul(src, matRow);
        auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
        auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
        auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
        auto *w = builder.CreateExtractElement(mul, builder.getInt32(3));
        auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), builder.CreateFAdd(z, w));
        result = builder.CreateInsertElement(result, dot, builder.getInt32(row));
      }
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::M4x3: {
      auto *src = loadSrc(inst.src[0]);
      Value *result = ConstantAggregateZero::get(float4Ty);
      for (uint32_t row = 0; row < 3; row++) {
        DxsoRegister rowReg = inst.src[1];
        rowReg.id.num += row;
        auto *matRow = loadSrc(rowReg);
        auto *mul = builder.CreateFMul(src, matRow);
        auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
        auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
        auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
        auto *w = builder.CreateExtractElement(mul, builder.getInt32(3));
        auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), builder.CreateFAdd(z, w));
        result = builder.CreateInsertElement(result, dot, builder.getInt32(row));
      }
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::M3x3: {
      auto *src = loadSrc(inst.src[0]);
      Value *result = ConstantAggregateZero::get(float4Ty);
      for (uint32_t row = 0; row < 3; row++) {
        DxsoRegister rowReg = inst.src[1];
        rowReg.id.num += row;
        auto *matRow = loadSrc(rowReg);
        auto *mul = builder.CreateFMul(src, matRow);
        auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
        auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
        auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
        auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), z);
        result = builder.CreateInsertElement(result, dot, builder.getInt32(row));
      }
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::M3x2: {
      auto *src = loadSrc(inst.src[0]);
      Value *result = ConstantAggregateZero::get(float4Ty);
      for (uint32_t row = 0; row < 2; row++) {
        DxsoRegister rowReg = inst.src[1];
        rowReg.id.num += row;
        auto *matRow = loadSrc(rowReg);
        auto *mul = builder.CreateFMul(src, matRow);
        auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
        auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
        auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
        auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), z);
        result = builder.CreateInsertElement(result, dot, builder.getInt32(row));
      }
      storeDst(inst.dst, result);
      break;
    }

    case DxsoOpcode::End:
      break;

    default:
      fprintf(stderr, "DXSO VS: unhandled opcode %u\n", (unsigned)inst.instruction.opcode);
      break;
    }
  }

  // Epilogue: build return value from output registers
  builder.CreateBr(epilogue_bb);
  builder.SetInsertPoint(epilogue_bb);

  auto *retTy = function->getReturnType();
  Value *retVal = ConstantAggregateZero::get(retTy);

  // Position from output register
  if (position_out_idx != ~0u) {
    // Find which output register has the position
    uint32_t posReg = 0;
    for (auto &dcl : shader->outputDecls) {
      if (dcl.usage == DxsoUsage::Position && dcl.usageIndex == 0) {
        posReg = dcl.reg;
        break;
      }
    }
    auto *posVal = builder.CreateLoad(float4Ty, builder.CreateGEP(
      ArrayType::get(float4Ty, max_output_reg), outputArray,
      {builder.getInt32(0), builder.getInt32(posReg)}));
    // Sanitize position: ensure w != 0 for proper perspective division
    retVal = builder.CreateInsertValue(retVal, posVal, {position_out_idx});
  }

  // User outputs (colors, texcoords, etc.) — sanitize NaN to prevent undefined GPU interpolation
  for (auto &[reg, idx] : user_out_indices) {
    Value *val = builder.CreateLoad(float4Ty, builder.CreateGEP(
      ArrayType::get(float4Ty, max_output_reg), outputArray,
      {builder.getInt32(0), builder.getInt32(reg)}));
    // Replace NaN components with 0 (NaN in vertex varyings causes undefined rasterizer behavior on Metal)
    auto *isNotNaN = builder.CreateFCmpORD(val, val);
    auto *zeroVec = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.0));
    val = builder.CreateSelect(isNotNaN, val, zeroVec);
    // SM < 3: D3D9 clamps oD0/oD1 (slots 2-3) and oFog (slot 1) to [0,1] before interpolation. Unclamped,
    // accumulated lighting > 1 made textured RW-skinned peds overbright/pale in GTA SA.
    if (sm2VS && (reg == 1 || reg == 2 || reg == 3)) {
      auto *oneVec = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 1.0));
      val = builder.CreateSelect(builder.CreateFCmpOLT(val, zeroVec), zeroVec, val);
      val = builder.CreateSelect(builder.CreateFCmpOGT(val, oneVec), oneVec, val);
    }
    retVal = builder.CreateInsertValue(retVal, val, {idx});
  }

  builder.CreateRet(retVal);

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_md);
}

static void compilePixelShader(
  DxsoShaderInternal *shader,
  const char *functionName,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  LLVMContext &context,
  Module &module
) {
  AirType types(context);
  FunctionSignatureBuilder func_sig;

  SM50_SHADER_COMMON_DATA *common = nullptr;
  SM50_SHADER_DXSO_ALPHA_TEST_DATA *alphaTestArg = nullptr;
  SM50_SHADER_DXSO_FOG_DATA *fogArg = nullptr;
  {
    auto *arg = pArgs;
    while (arg) {
      if (arg->type == SM50_SHADER_COMMON)
        common = (SM50_SHADER_COMMON_DATA *)arg;
      if (arg->type == SM50_SHADER_DXSO_ALPHA_TEST)
        alphaTestArg = (SM50_SHADER_DXSO_ALPHA_TEST_DATA *)arg;
      if (arg->type == SM50_SHADER_DXSO_FOG)
        fogArg = (SM50_SHADER_DXSO_FOG_DATA *)arg;
      arg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next;
    }
  }

  uint8_t fogMode = fogArg ? fogArg->fog_mode : 0;
  bool vertexFog = fogMode == 1 && shader->info.majorVersion < 3;
  bool tableFog = fogMode >= 2 && fogMode <= 4 && shader->info.majorVersion < 3;
  bool injectFog = vertexFog || tableFog;

  // PS inputs: stage-in from VS outputs, matched by semantic
  // Skip Position/PositionT — these are consumed by the rasterizer, not PS inputs
  // Define TEXCOORD inputs first, then COLOR inputs — this keeps texture/sampler
  // argument indices at the same positions as without color inputs, avoiding a
  // Metal shader compiler crash with high argument indices.
  uint32_t max_texcoord_reg = 0;
  uint32_t max_input_reg = 0; // max across ALL v# inputs (texcoord + color)
  uint32_t max_color_idx = 0;
  std::vector<DxsoShaderInternal::PSInputDecl> filteredPsInputDecls;

  auto defineStageIn = [&](const DxsoShaderInternal::PSInputDecl &dcl) {
    std::string semantic;
    switch (dcl.usage) {
    case DxsoUsage::Color: semantic = "COLOR" + std::to_string(dcl.usageIndex); break;
    case DxsoUsage::Texcoord: semantic = "TEXCOORD" + std::to_string(dcl.usageIndex); break;
    default: semantic = "USER" + std::to_string(dcl.reg); break;
    }
    func_sig.DefineInput(InputFragmentStageIn{
      .user = semantic,
      .type = msl_float4,
      .interpolation = Interpolation::center_perspective,
      .pull_mode = false,
    });
    filteredPsInputDecls.push_back(dcl);
    max_input_reg = std::max(max_input_reg, dcl.reg + 1);
    if (dcl.usage == DxsoUsage::Color) {
      max_color_idx = std::max(max_color_idx, dcl.usageIndex + 1);
    } else {
      max_texcoord_reg = std::max(max_texcoord_reg, dcl.reg + 1);
    }
  };

  // Pass 1: non-color inputs (texcoords etc.)
  for (auto &dcl : shader->psInputDecls) {
    if (dcl.usage == DxsoUsage::Position || dcl.usage == DxsoUsage::PositionT)
      continue;
    if (dcl.usage == DxsoUsage::Color)
      continue;
    defineStageIn(dcl);
  }
  // Pass 2: color inputs (after texcoords, so texture/sampler args stay at same indices)
  for (auto &dcl : shader->psInputDecls) {
    if (dcl.usage != DxsoUsage::Color)
      continue;
    defineStageIn(dcl);
  }

  // FOG0 stage-in input for vertex fog (SM < 3 only)
  uint32_t fog_in_idx = ~0u;
  if (vertexFog) {
    fog_in_idx = func_sig.DefineInput(InputFragmentStageIn{
      .user = "FOG0",
      .type = msl_float4,
      .interpolation = Interpolation::center_perspective,
      .pull_mode = false,
    });
  }

  // [[position]] input for table fog (to get fragment depth)
  uint32_t frag_pos_idx = ~0u;
  if (tableFog) {
    frag_pos_idx = func_sig.DefineInput(InputPosition{
      .interpolation = Interpolation::center_perspective,
    });
  }

  // Constants buffer at index 18
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

  // Texture and sampler inputs for each sampler declaration
  struct SamplerBinding {
    uint32_t texArgIdx;
    uint32_t sampArgIdx;
    llvm::air::Texture texDesc;
  };
  std::unordered_map<uint32_t, SamplerBinding> samplerBindings;
  for (auto &sdcl : shader->samplerDecls) {
    llvm::air::Texture texDesc{
      .kind = llvm::air::Texture::texture_2d,
      .sample_type = llvm::air::Texture::sample_float,
      .memory_access = llvm::air::Texture::access_sample,
    };
    uint32_t texIdx = func_sig.DefineInput(ArgumentBindingTexture{
      .location_index = 19 + sdcl.reg * 2,
      .array_size = 1,
      .memory_access = MemoryAccess::sample,
      .type = MSLTexture{
        .component_type = msl_float,
        .memory_access = MemoryAccess::sample,
        .resource_kind = TextureKind::texture_2d,
        .resource_kind_logical = TextureKind::texture_2d,
      },
      .arg_name = "tex" + std::to_string(sdcl.reg),
      .raster_order_group = {},
    });
    uint32_t sampIdx = func_sig.DefineInput(ArgumentBindingSampler{
      .location_index = sdcl.reg, // Metal sampler slots are 0..15 (separate namespace)
      .array_size = 1,
      .arg_name = "samp" + std::to_string(sdcl.reg),
    });
    samplerBindings[sdcl.reg] = {texIdx, sampIdx, texDesc};
  }

  // Output: render target 0
  uint32_t rt0_idx = func_sig.DefineOutput(OutputRenderTarget{
    .dual_source_blending = false,
    .index = 0,
    .type = msl_float4,
  });

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  if (common) metal_version = common->metal_version;

  auto [function, function_md] = func_sig.CreateFunction(
    functionName, context, module, 0, false
  );

  auto entry_bb = BasicBlock::Create(context, "entry", function);
  IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nullOS;
  llvm::air::AIRBuilder air(builder, nullOS);

  dxbc::setup_metal_version(module, metal_version);

  auto *float4Ty = types._float4;

  auto epilogue_bb = BasicBlock::Create(context, "epilogue", function);

  // Alloca register files
  // Texcoord inputs use inputArray, indexed by dcl.reg (t# register number).
  // Color inputs (v0/v1) are kept as direct SSA values to match the pattern
  // used by the FF PS compiler — avoids a Metal shader compiler crash that
  // occurs when color inputs go through stack allocas alongside texture args.
  uint32_t inputArraySize = std::max({max_texcoord_reg, max_input_reg, 1u});
  auto *inputArray = builder.CreateAlloca(ArrayType::get(float4Ty, inputArraySize));
  auto *tempArray = builder.CreateAlloca(ArrayType::get(float4Ty, 32));
  auto *colorOut = builder.CreateAlloca(float4Ty);
  builder.CreateStore(ConstantAggregateZero::get(float4Ty), colorOut);

  // Color inputs stored as direct SSA values (no alloca)
  Value *colorArgs[2] = {
    ConstantAggregateZero::get(float4Ty),
    ConstantAggregateZero::get(float4Ty)
  };

  // Copy stage-in values to input registers
  bool isSM1PS = shader->info.majorVersion < 2;
  uint32_t argIdx = 0;
  for (auto &dcl : filteredPsInputDecls) {
    auto *inputVal = function->getArg(argIdx++);

    if (dcl.usage == DxsoUsage::Color && dcl.usageIndex < 2) {
      colorArgs[dcl.usageIndex] = inputVal;
    } else {
      auto *dst = builder.CreateGEP(
        ArrayType::get(float4Ty, inputArraySize), inputArray,
        {builder.getInt32(0), builder.getInt32(dcl.reg)});
      builder.CreateStore(inputVal, dst);
    }

    // For SM1 PS: also copy texcoord inputs to tempArray (t# register file)
    if (isSM1PS && dcl.usage == DxsoUsage::Texcoord) {
      auto *tmpDst = builder.CreateGEP(
        ArrayType::get(float4Ty, 32), tempArray,
        {builder.getInt32(0), builder.getInt32(dcl.usageIndex)});
      builder.CreateStore(inputVal, tmpDst);
    }
  }

  // Constant buffer pointer
  auto *constBufPtr = function->getArg(const_buf_idx);
  auto *constBufFloat4 = builder.CreateBitCast(
    constBufPtr, float4Ty->getPointerTo((uint32_t)AddressSpace::constant));


  // Pre-scan for Def instructions
  std::unordered_map<uint32_t, std::array<float, 4>> psDefConstants;
  {
    DxsoDecoder defScan(shader->fullTokens.data());
    DxsoInstructionContext defCtx;
    while (defScan.decodeInstruction(defCtx)) {
      if (defCtx.instruction.opcode == DxsoOpcode::Def) {
        psDefConstants[defCtx.dst.id.num] = {
          defCtx.def.float32[0], defCtx.def.float32[1],
          defCtx.def.float32[2], defCtx.def.float32[3]
        };
      }
    }
  }

  // Lambda to load source register
  auto loadSrc = [&](const DxsoRegister &reg) -> Value * {
    Value *val = nullptr;
    switch (reg.id.type) {
    case DxsoRegisterType::Temp:
      val = builder.CreateLoad(float4Ty, builder.CreateGEP(
        ArrayType::get(float4Ty, 32), tempArray,
        {builder.getInt32(0), builder.getInt32(reg.id.num)}));
      break;
    case DxsoRegisterType::Input:
    case DxsoRegisterType::AttributeOut:
      // v# registers are Color in PS < 3.0 (max 2: v0, v1)
      // PS 3.0+ could have more, but for now handle the common case
      val = (reg.id.num < 2) ? colorArgs[reg.id.num]
                              : ConstantAggregateZero::get(float4Ty);
      break;
    case DxsoRegisterType::Texture: {
      if (isSM1PS) {
        // SM1 PS: t# registers live in tempArray (pre-loaded with texcoords,
        // overwritten by tex instruction with sampled data)
        val = builder.CreateLoad(float4Ty, builder.CreateGEP(
          ArrayType::get(float4Ty, 32), tempArray,
          {builder.getInt32(0), builder.getInt32(reg.id.num)}));
      } else {
        // SM2+ PS: t# registers = texture coordinates
        uint32_t inputReg = reg.id.num;
        for (auto &dcl : shader->psInputDecls) {
          if (dcl.usage == DxsoUsage::Texcoord && dcl.usageIndex == reg.id.num) {
            inputReg = dcl.reg;
            break;
          }
        }
        val = builder.CreateLoad(float4Ty, builder.CreateGEP(
          ArrayType::get(float4Ty, inputArraySize), inputArray,
          {builder.getInt32(0), builder.getInt32(inputReg)}));
      }
      break;
    }
    case DxsoRegisterType::Const: {
      auto defIt = psDefConstants.find(reg.id.num);
      if (defIt != psDefConstants.end()) {
        auto &v = defIt->second;
        val = ConstantVector::get({
          ConstantFP::get(types._float, v[0]),
          ConstantFP::get(types._float, v[1]),
          ConstantFP::get(types._float, v[2]),
          ConstantFP::get(types._float, v[3])
        });
      } else {
        val = builder.CreateLoad(float4Ty,
          builder.CreateConstGEP1_32(float4Ty, constBufFloat4, reg.id.num));
      }
      break;
    }
    default:
      val = ConstantAggregateZero::get(float4Ty);
      break;
    }

    // Apply swizzle
    auto swz = reg.swizzle;
    if (swz.data != 0xe4) {
      Value *swizzled = UndefValue::get(float4Ty);
      for (uint32_t i = 0; i < 4; i++) {
        swizzled = builder.CreateInsertElement(swizzled,
          builder.CreateExtractElement(val, builder.getInt32(swz[i])),
          builder.getInt32(i));
      }
      val = swizzled;
    }

    switch (reg.modifier) {
    case DxsoRegModifier::None:
      break;
    case DxsoRegModifier::Neg:
      val = builder.CreateFNeg(val);
      break;
    case DxsoRegModifier::Abs:
      val = air.CreateFPUnOp(air.fabs, val);
      break;
    case DxsoRegModifier::AbsNeg:
      val = builder.CreateFNeg(air.CreateFPUnOp(air.fabs, val));
      break;
    case DxsoRegModifier::Comp: { // 1 - x
      auto one = ConstantFP::get(types._float, 1.0);
      auto ones = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      val = builder.CreateFSub(ones, val);
      break;
    }
    case DxsoRegModifier::X2: // 2 * x
      val = builder.CreateFMul(val, ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0)));
      break;
    case DxsoRegModifier::X2Neg: // -(2 * x)
      val = builder.CreateFNeg(builder.CreateFMul(val, ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0))));
      break;
    case DxsoRegModifier::Bias: { // x - 0.5
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      val = builder.CreateFSub(val, half);
      break;
    }
    case DxsoRegModifier::BiasNeg: { // -(x - 0.5)
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      val = builder.CreateFNeg(builder.CreateFSub(val, half));
      break;
    }
    case DxsoRegModifier::Sign: { // 2 * (x - 0.5)
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      auto two = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0));
      val = builder.CreateFMul(builder.CreateFSub(val, half), two);
      break;
    }
    case DxsoRegModifier::SignNeg: { // -(2 * (x - 0.5))
      auto half = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.5));
      auto two = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 2.0));
      val = builder.CreateFNeg(builder.CreateFMul(builder.CreateFSub(val, half), two));
      break;
    }
    default:
      fprintf(stderr, "DXSO PS: unhandled source modifier %u\n", (unsigned)reg.modifier);
      break;
    }
    return val;
  };

  auto storeDst = [&](const DxsoRegister &reg, Value *val) {
    Value *dst_ptr = nullptr;
    Value *old_val = nullptr;

    switch (reg.id.type) {
    case DxsoRegisterType::Temp:
      dst_ptr = builder.CreateGEP(ArrayType::get(float4Ty, 32), tempArray,
        {builder.getInt32(0), builder.getInt32(reg.id.num)});
      old_val = builder.CreateLoad(float4Ty, dst_ptr);
      break;
    case DxsoRegisterType::ColorOut:
      dst_ptr = colorOut;
      old_val = builder.CreateLoad(float4Ty, dst_ptr);
      break;
    default:
      return;
    }

    if (reg.mask.mask != 0xf) {
      for (uint32_t i = 0; i < 4; i++) {
        if (reg.mask[i]) {
          old_val = builder.CreateInsertElement(old_val,
            builder.CreateExtractElement(val, builder.getInt32(i)),
            builder.getInt32(i));
        }
      }
      val = old_val;
    }

    if (reg.saturate) {
      auto *zero = ConstantFP::get(types._float, 0.0);
      auto *one = ConstantFP::get(types._float, 1.0);
      auto *zeroVec = ConstantVector::getSplat(ElementCount::getFixed(4), zero);
      auto *oneVec = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      val = air.CreateFPBinOp(air.fmax, val, zeroVec);
      val = air.CreateFPBinOp(air.fmin, val, oneVec);
    }

    builder.CreateStore(val, dst_ptr);
  };

  // Decode and compile instructions
  DxsoDecoder psDecoder(shader->fullTokens.data());
  DxsoInstructionContext psInst;

  while (psDecoder.decodeInstruction(psInst)) {
    switch (psInst.instruction.opcode) {
    case DxsoOpcode::Dcl:
    case DxsoOpcode::Comment:
    case DxsoOpcode::Def:
      break;

    case DxsoOpcode::Mov:
      storeDst(psInst.dst, loadSrc(psInst.src[0]));
      break;

    case DxsoOpcode::Add:
      storeDst(psInst.dst, builder.CreateFAdd(loadSrc(psInst.src[0]), loadSrc(psInst.src[1])));
      break;

    case DxsoOpcode::Sub:
      storeDst(psInst.dst, builder.CreateFSub(loadSrc(psInst.src[0]), loadSrc(psInst.src[1])));
      break;

    case DxsoOpcode::Mul:
      storeDst(psInst.dst, builder.CreateFMul(loadSrc(psInst.src[0]), loadSrc(psInst.src[1])));
      break;

    case DxsoOpcode::Mad: {
      auto *a = loadSrc(psInst.src[0]);
      auto *b = loadSrc(psInst.src[1]);
      auto *c = loadSrc(psInst.src[2]);
      storeDst(psInst.dst, builder.CreateFAdd(builder.CreateFMul(a, b), c));
      break;
    }

    case DxsoOpcode::Dp3: {
      auto *a = loadSrc(psInst.src[0]);
      auto *b = loadSrc(psInst.src[1]);
      auto *mul = builder.CreateFMul(a, b);
      auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
      auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
      auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
      auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), z);
      storeDst(psInst.dst, builder.CreateVectorSplat(4, dot));
      break;
    }

    case DxsoOpcode::Dp4: {
      auto *a = loadSrc(psInst.src[0]);
      auto *b = loadSrc(psInst.src[1]);
      auto *mul = builder.CreateFMul(a, b);
      auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
      auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
      auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
      auto *w = builder.CreateExtractElement(mul, builder.getInt32(3));
      auto *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), builder.CreateFAdd(z, w));
      storeDst(psInst.dst, builder.CreateVectorSplat(4, dot));
      break;
    }

    case DxsoOpcode::Rcp: {
      auto *src = loadSrc(psInst.src[0]);
      auto *one = ConstantFP::get(types._float, 1.0);
      auto *oneVec = ConstantVector::getSplat(ElementCount::getFixed(4), one);
      storeDst(psInst.dst, builder.CreateFDiv(oneVec, src));
      break;
    }

    case DxsoOpcode::Rsq: {
      // D3D9: rsq operates on abs(src)
      auto *src = loadSrc(psInst.src[0]);
      storeDst(psInst.dst, air.CreateFPUnOp(air.rsqrt, air.CreateFPUnOp(air.fabs, src)));
      break;
    }

    case DxsoOpcode::Min:
      storeDst(psInst.dst, air.CreateFPBinOp(air.fmin,
        loadSrc(psInst.src[0]), loadSrc(psInst.src[1])));
      break;

    case DxsoOpcode::Max:
      storeDst(psInst.dst, air.CreateFPBinOp(air.fmax,
        loadSrc(psInst.src[0]), loadSrc(psInst.src[1])));
      break;

    case DxsoOpcode::Slt: {
      auto *a = loadSrc(psInst.src[0]);
      auto *b = loadSrc(psInst.src[1]);
      auto *cmp = builder.CreateFCmpOLT(a, b);
      auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 1.0));
      auto *zero = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.0));
      storeDst(psInst.dst, builder.CreateSelect(cmp, one, zero));
      break;
    }

    case DxsoOpcode::Sge: {
      auto *a = loadSrc(psInst.src[0]);
      auto *b = loadSrc(psInst.src[1]);
      auto *cmp = builder.CreateFCmpOGE(a, b);
      auto *one = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 1.0));
      auto *zero = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.0));
      storeDst(psInst.dst, builder.CreateSelect(cmp, one, zero));
      break;
    }

    case DxsoOpcode::Pow: {
      // D3D9: pow(base, exp) where base <= 0 returns 0
      auto *a = loadSrc(psInst.src[0]);
      auto *b = loadSrc(psInst.src[1]);
      auto *zeroVec = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.0));
      auto *result = air.CreateFPUnOp(air.exp2, builder.CreateFMul(b, air.CreateFPUnOp(air.log2, a)));
      auto *basePositive = builder.CreateFCmpOGT(a, zeroVec);
      storeDst(psInst.dst, builder.CreateSelect(basePositive, result, zeroVec));
      break;
    }

    case DxsoOpcode::Log: {
      // D3D9: log operates on abs(src)
      auto *src = loadSrc(psInst.src[0]);
      storeDst(psInst.dst, air.CreateFPUnOp(air.log2, air.CreateFPUnOp(air.fabs, src)));
      break;
    }

    case DxsoOpcode::Exp:
      storeDst(psInst.dst, air.CreateFPUnOp(air.exp2, loadSrc(psInst.src[0])));
      break;

    case DxsoOpcode::Lrp: {
      auto *s0 = loadSrc(psInst.src[0]);
      auto *s1 = loadSrc(psInst.src[1]);
      auto *s2 = loadSrc(psInst.src[2]);
      storeDst(psInst.dst, builder.CreateFAdd(s2, builder.CreateFMul(s0, builder.CreateFSub(s1, s2))));
      break;
    }

    case DxsoOpcode::Frc: {
      auto *src = loadSrc(psInst.src[0]);
      storeDst(psInst.dst, builder.CreateFSub(src, air.CreateFPUnOp(air.floor, src)));
      break;
    }

    case DxsoOpcode::Nrm: {
      auto *src = loadSrc(psInst.src[0]);
      auto *mul = builder.CreateFMul(src, src);
      auto *x = builder.CreateExtractElement(mul, builder.getInt32(0));
      auto *y = builder.CreateExtractElement(mul, builder.getInt32(1));
      auto *z = builder.CreateExtractElement(mul, builder.getInt32(2));
      auto *dot3 = builder.CreateFAdd(builder.CreateFAdd(x, y), z);
      auto *invLen = air.CreateFPUnOp(air.rsqrt, dot3);
      storeDst(psInst.dst, builder.CreateFMul(src, builder.CreateVectorSplat(4, invLen)));
      break;
    }

    case DxsoOpcode::Abs:
      storeDst(psInst.dst, air.CreateFPUnOp(air.fabs, loadSrc(psInst.src[0])));
      break;

    case DxsoOpcode::SinCos: {
      auto *src = loadSrc(psInst.src[0]);
      auto *angle = builder.CreateExtractElement(src, builder.getInt32(0));
      Value *result = ConstantAggregateZero::get(float4Ty);
      result = builder.CreateInsertElement(result, air.CreateFPUnOp(air.cos, angle), builder.getInt32(0));
      result = builder.CreateInsertElement(result, air.CreateFPUnOp(air.sin, angle), builder.getInt32(1));
      storeDst(psInst.dst, result);
      break;
    }

    case DxsoOpcode::Cmp: {
      auto *s0 = loadSrc(psInst.src[0]);
      auto *s1 = loadSrc(psInst.src[1]);
      auto *s2 = loadSrc(psInst.src[2]);
      auto *zero = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(types._float, 0.0));
      storeDst(psInst.dst, builder.CreateSelect(builder.CreateFCmpOGE(s0, zero), s1, s2));
      break;
    }

    case DxsoOpcode::Tex: {
      bool isSM1 = shader->info.majorVersion < 2;
      if (isSM1) {
        // SM1: tex tN — sample texture N using tN (texcoord pre-loaded into tempArray)
        uint32_t regNum = psInst.dst.id.num;
        auto it = samplerBindings.find(regNum);
        if (it != samplerBindings.end()) {
          auto &binding = it->second;
          // Load texcoord from tempArray (pre-loaded from stage-in during init)
          auto *coords = builder.CreateLoad(float4Ty, builder.CreateGEP(
            ArrayType::get(float4Ty, 32), tempArray,
            {builder.getInt32(0), builder.getInt32(regNum)}));
          auto *u = builder.CreateExtractElement(coords, builder.getInt32(0));
          auto *v = builder.CreateExtractElement(coords, builder.getInt32(1));
          auto *float2Ty = FixedVectorType::get(types._float, 2);
          Value *coord2d = UndefValue::get(float2Ty);
          coord2d = builder.CreateInsertElement(coord2d, u, builder.getInt32(0));
          coord2d = builder.CreateInsertElement(coord2d, v, builder.getInt32(1));

          auto *texHandle = function->getArg(binding.texArgIdx);
          auto *sampHandle = function->getArg(binding.sampArgIdx);
          int32_t offsets[3] = {0, 0, 0};
          auto [sampled, residency] = air.CreateSample(
            binding.texDesc, texHandle, sampHandle, coord2d, nullptr, offsets,
            llvm::air::sample_bias{air.getFloat(0)}); // implicit LOD (mipmaps), like D3D9

          // Store sampled result to temp register (t# registers are temps after tex)
          builder.CreateStore(sampled, builder.CreateGEP(
            ArrayType::get(float4Ty, 32), tempArray,
            {builder.getInt32(0), builder.getInt32(regNum)}));
        }
      } else {
        // SM2+: texld dst, coords, sampler
        uint32_t sampReg = psInst.src[1].id.num;
        auto it = samplerBindings.find(sampReg);
        if (it != samplerBindings.end()) {
          auto &binding = it->second;
          auto *coords = loadSrc(psInst.src[0]);
          auto *u = builder.CreateExtractElement(coords, builder.getInt32(0));
          auto *v = builder.CreateExtractElement(coords, builder.getInt32(1));
          auto *float2Ty = FixedVectorType::get(types._float, 2);
          Value *coord2d = UndefValue::get(float2Ty);
          coord2d = builder.CreateInsertElement(coord2d, u, builder.getInt32(0));
          coord2d = builder.CreateInsertElement(coord2d, v, builder.getInt32(1));

          auto *texHandle = function->getArg(binding.texArgIdx);
          auto *sampHandle = function->getArg(binding.sampArgIdx);
          int32_t offsets[3] = {0, 0, 0};
          auto [sampled, residency] = air.CreateSample(
            binding.texDesc, texHandle, sampHandle, coord2d, nullptr, offsets,
            llvm::air::sample_bias{air.getFloat(0)}); // implicit LOD (mipmaps), like D3D9
          storeDst(psInst.dst, sampled);
        }
      }
      break;
    }

    case DxsoOpcode::TexCoord: {
      // SM1: texcoord tN — load texcoord N into temp register N (no-op if already there)
      // Copy from input register to temp register for consistency
      uint32_t regNum = psInst.dst.id.num;
      if (regNum < inputArraySize) {
        auto *val = builder.CreateLoad(float4Ty, builder.CreateGEP(
          ArrayType::get(float4Ty, inputArraySize), inputArray,
          {builder.getInt32(0), builder.getInt32(regNum)}));
        builder.CreateStore(val, builder.CreateGEP(
          ArrayType::get(float4Ty, 32), tempArray,
          {builder.getInt32(0), builder.getInt32(regNum)}));
      }
      break;
    }

    case DxsoOpcode::TexKill: {
      // texkill tN — discard if any xyz component of tN < 0
      Value *val;
      if (psInst.dst.id.type == DxsoRegisterType::Texture) {
        val = builder.CreateLoad(float4Ty, builder.CreateGEP(
          ArrayType::get(float4Ty, 32), tempArray,
          {builder.getInt32(0), builder.getInt32(psInst.dst.id.num)}));
      } else {
        val = loadSrc(psInst.src[0]);
      }
      auto *x = builder.CreateExtractElement(val, builder.getInt32(0));
      auto *y = builder.CreateExtractElement(val, builder.getInt32(1));
      auto *z = builder.CreateExtractElement(val, builder.getInt32(2));
      auto *zero = ConstantFP::get(types._float, 0.0);
      auto *killCond = builder.CreateOr(
        builder.CreateOr(builder.CreateFCmpOLT(x, zero), builder.CreateFCmpOLT(y, zero)),
        builder.CreateFCmpOLT(z, zero));

      auto *discard_bb = BasicBlock::Create(context, "texkill_discard", function);
      auto *continue_bb = BasicBlock::Create(context, "texkill_continue", function);
      builder.CreateCondBr(killCond, discard_bb, continue_bb);
      builder.SetInsertPoint(discard_bb);
      auto discardFn = module.getOrInsertFunction(
        "air.discard_fragment", FunctionType::get(Type::getVoidTy(context), false));
      builder.CreateCall(discardFn);
      builder.CreateBr(continue_bb);
      builder.SetInsertPoint(continue_bb);
      break;
    }

    default:
      fprintf(stderr, "DXSO PS: unhandled opcode %u\n", (unsigned)psInst.instruction.opcode);
      break;
    }
  }

  // Epilogue: return output color
  builder.CreateBr(epilogue_bb);
  builder.SetInsertPoint(epilogue_bb);

  auto *retTy = function->getReturnType();
  Value *retVal = ConstantAggregateZero::get(retTy);

  Value *outputColor;
  if (shader->info.majorVersion < 2) {
    // SM1 PS: r0 is implicitly the output
    outputColor = builder.CreateLoad(float4Ty, builder.CreateGEP(
      ArrayType::get(float4Ty, 32), tempArray,
      {builder.getInt32(0), builder.getInt32(0)}));
  } else {
    outputColor = builder.CreateLoad(float4Ty, colorOut);
  }


  // Fog blending (injected for SM < 3 PS when fog is enabled)
  if (injectFog) {
    Value *fogFactor = nullptr;

    if (vertexFog && fog_in_idx != ~0u) {
      // Vertex fog: read fog factor from VS output
      auto *fogVec = function->getArg(fog_in_idx);
      fogFactor = builder.CreateExtractElement(fogVec, builder.getInt32(0));
    } else if (tableFog && frag_pos_idx != ~0u) {
      // Table fog: compute fog factor from fragment depth
      // [[position]].w = 1/clip_w, and clip_w ≈ eye_z for perspective projection
      auto *fragPos = function->getArg(frag_pos_idx);
      auto *invW = builder.CreateExtractElement(fragPos, builder.getInt32(3));
      auto *eyeZ = air.CreateFPUnOp(air.fabs,
        builder.CreateFDiv(ConstantFP::get(types._float, 1.0), invW));
      // Fog params from c254: x=start, y=end, z=density
      auto *fogParams = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, constBufFloat4, 254));
      auto *fogStart = builder.CreateExtractElement(fogParams, builder.getInt32(0));
      auto *fogEnd = builder.CreateExtractElement(fogParams, builder.getInt32(1));
      auto *fogDensity = builder.CreateExtractElement(fogParams, builder.getInt32(2));
      switch (fogMode) {
      case 2: // EXP
        fogFactor = air.CreateFPUnOp(air.exp2,
          builder.CreateFMul(ConstantFP::get(types._float, -1.442695f),
            builder.CreateFMul(fogDensity, eyeZ)));
        break;
      case 3: { // EXP2
        auto *dz = builder.CreateFMul(fogDensity, eyeZ);
        fogFactor = air.CreateFPUnOp(air.exp2,
          builder.CreateFMul(ConstantFP::get(types._float, -1.442695f),
            builder.CreateFMul(dz, dz)));
        break;
      }
      case 4: // LINEAR
        fogFactor = builder.CreateFDiv(
          builder.CreateFSub(fogEnd, eyeZ),
          builder.CreateFSub(fogEnd, fogStart));
        break;
      }
      // Saturate fog factor to [0,1]
      if (fogFactor) {
        fogFactor = air.CreateFPBinOp(air.fmax,
          fogFactor, ConstantFP::get(types._float, 0.0));
        fogFactor = air.CreateFPBinOp(air.fmin,
          fogFactor, ConstantFP::get(types._float, 1.0));
      }
    }

    if (fogFactor) {
      // Fog color is packed into c255.yzw
      auto *sysConst = builder.CreateLoad(float4Ty,
        builder.CreateConstGEP1_32(float4Ty, constBufFloat4, 255));
      auto *fcR = builder.CreateExtractElement(sysConst, builder.getInt32(1));
      auto *fcG = builder.CreateExtractElement(sysConst, builder.getInt32(2));
      auto *fcB = builder.CreateExtractElement(sysConst, builder.getInt32(3));
      auto *curR = builder.CreateExtractElement(outputColor, builder.getInt32(0));
      auto *curG = builder.CreateExtractElement(outputColor, builder.getInt32(1));
      auto *curB = builder.CreateExtractElement(outputColor, builder.getInt32(2));
      auto *oneMinusFog = builder.CreateFSub(ConstantFP::get(types._float, 1.0), fogFactor);
      curR = builder.CreateFAdd(builder.CreateFMul(curR, fogFactor), builder.CreateFMul(fcR, oneMinusFog));
      curG = builder.CreateFAdd(builder.CreateFMul(curG, fogFactor), builder.CreateFMul(fcG, oneMinusFog));
      curB = builder.CreateFAdd(builder.CreateFMul(curB, fogFactor), builder.CreateFMul(fcB, oneMinusFog));
      outputColor = builder.CreateInsertElement(outputColor, curR, builder.getInt32(0));
      outputColor = builder.CreateInsertElement(outputColor, curG, builder.getInt32(1));
      outputColor = builder.CreateInsertElement(outputColor, curB, builder.getInt32(2));
    }
  }

  // Alpha test (injected when alpha_test_func is set via compilation argument)
  if (alphaTestArg && alphaTestArg->alpha_test_func != 0 && alphaTestArg->alpha_test_func != 8) {
    // Load alpha ref from constant buffer slot 255 (c255.x)
    auto *alphaRefVec = builder.CreateLoad(float4Ty,
      builder.CreateConstGEP1_32(float4Ty, constBufFloat4, 255));
    auto *alphaRef = builder.CreateExtractElement(alphaRefVec, builder.getInt32(0));
    auto *alpha = builder.CreateExtractElement(outputColor, builder.getInt32(3));

    Value *discard_cond = nullptr;
    switch (alphaTestArg->alpha_test_func) {
    case 1: // D3DCMP_NEVER
      discard_cond = ConstantInt::getTrue(context);
      break;
    case 2: // D3DCMP_LESS
      discard_cond = builder.CreateFCmpOGE(alpha, alphaRef);
      break;
    case 3: // D3DCMP_EQUAL
      discard_cond = builder.CreateFCmpONE(alpha, alphaRef);
      break;
    case 4: // D3DCMP_LESSEQUAL
      discard_cond = builder.CreateFCmpOGT(alpha, alphaRef);
      break;
    case 5: // D3DCMP_GREATER
      discard_cond = builder.CreateFCmpOLE(alpha, alphaRef);
      break;
    case 6: // D3DCMP_NOTEQUAL
      discard_cond = builder.CreateFCmpOEQ(alpha, alphaRef);
      break;
    case 7: // D3DCMP_GREATEREQUAL
      discard_cond = builder.CreateFCmpOLT(alpha, alphaRef);
      break;
    default:
      break;
    }

    if (discard_cond) {
      auto *discard_bb = BasicBlock::Create(context, "alpha_discard", function);
      auto *continue_bb = BasicBlock::Create(context, "alpha_continue", function);
      builder.CreateCondBr(discard_cond, discard_bb, continue_bb);
      builder.SetInsertPoint(discard_bb);
      auto discardFn = module.getOrInsertFunction(
        "air.discard_fragment", FunctionType::get(Type::getVoidTy(context), false));
      builder.CreateCall(discardFn);
      builder.CreateBr(continue_bb);
      builder.SetInsertPoint(continue_bb);
    }
  }

  retVal = builder.CreateInsertValue(retVal, outputColor, {rt0_idx});
  builder.CreateRet(retVal);

  module.getOrInsertNamedMetadata("air.fragment")->addOperand(function_md);
}

} // namespace dxmt::dxso

// C API implementation

extern "C" {

AIRCONV_API int DXSOInitialize(
  const void *pBytecode, size_t BytecodeSize,
  sm50_ptr64_t *ppShader, sm50_error_t *ppError
) {
  using namespace dxmt::dxso;

  if (ppError) *ppError = nullptr;
  if (!ppShader) return 1;

  auto *shader = new DxsoShaderInternal();

  const uint32_t *tokens = (const uint32_t *)pBytecode;
  size_t tokenCount = BytecodeSize / sizeof(uint32_t);

  // Copy full tokens
  shader->fullTokens.assign(tokens, tokens + tokenCount);

  // Parse header and collect declarations
  DxsoDecoder decoder(tokens);
  shader->info = decoder.programInfo();

  DxsoInstructionContext ctx;
  while (decoder.decodeInstruction(ctx)) {
    if (ctx.instruction.opcode == DxsoOpcode::Dcl) {
      auto regType = ctx.dst.id.type;
      auto regNum = ctx.dst.id.num;

      if (shader->info.isVertexShader) {
        if (regType == DxsoRegisterType::Input) {
          shader->inputDecls.push_back({
            .reg = regNum,
            .usage = ctx.dcl.semantic.usage,
            .usageIndex = ctx.dcl.semantic.usageIndex,
          });
        } else if (regType == DxsoRegisterType::Output ||
                   regType == DxsoRegisterType::TexcoordOut ||
                   regType == DxsoRegisterType::AttributeOut ||
                   regType == DxsoRegisterType::RasterizerOut) {
          shader->outputDecls.push_back({
            .reg = regNum,
            .usage = ctx.dcl.semantic.usage,
            .usageIndex = ctx.dcl.semantic.usageIndex,
          });
        }
      } else {
        // Pixel shader
        if (shader->info.majorVersion < 3) {
          // PS < 3.0: semantic is determined by register type, not DCL token
          // v# (Input) = Color, t# (Texture/Addr) = Texcoord
          if (regType == DxsoRegisterType::Input) {
            shader->psInputDecls.push_back({
              .reg = regNum,
              .usage = DxsoUsage::Color,
              .usageIndex = regNum,
            });
          } else if (regType == DxsoRegisterType::Texture) {
            shader->psInputDecls.push_back({
              .reg = regNum,
              .usage = DxsoUsage::Texcoord,
              .usageIndex = regNum,
            });
          }
        } else {
          // PS 3.0+: use actual DCL semantic
          if (regType == DxsoRegisterType::Input) {
            shader->psInputDecls.push_back({
              .reg = regNum,
              .usage = ctx.dcl.semantic.usage,
              .usageIndex = ctx.dcl.semantic.usageIndex,
            });
          }
        }
        if (regType == DxsoRegisterType::Sampler) {
          shader->samplerDecls.push_back({
            .reg = regNum,
            .textureType = ctx.dcl.textureType,
          });
        }
      }
    }
  }

  // Scan for maximum float constant register used (for sized constant copies)
  {
    DxsoDecoder constScan(tokens);
    DxsoInstructionContext constCtx;
    while (constScan.decodeInstruction(constCtx)) {
      auto op = constCtx.instruction.opcode;
      if (op == DxsoOpcode::Def) {
        uint32_t reg = constCtx.dst.id.num + 1;
        if (reg > shader->maxConstantReg) shader->maxConstantReg = reg;
        continue;
      }
      if (op == DxsoOpcode::Dcl || op == DxsoOpcode::DefI ||
          op == DxsoOpcode::DefB || op == DxsoOpcode::Comment || op == DxsoOpcode::End)
        continue;
      uint32_t numSrcs = (constCtx.instruction.tokenLength > 1) ? constCtx.instruction.tokenLength - 1 : 0;
      for (uint32_t s = 0; s < numSrcs && s < DxsoMaxSrcRegs; s++) {
        if (constCtx.src[s].id.type == DxsoRegisterType::Const) {
          if (constCtx.src[s].hasRelative) {
            shader->hasRelativeConst = true;
          } else {
            uint32_t reg = constCtx.src[s].id.num + 1;
            if (reg > shader->maxConstantReg) shader->maxConstantReg = reg;
          }
        }
      }
    }
    // If dynamic indexing is used, we can't statically bound — use full 256
    if (shader->hasRelativeConst)
      shader->maxConstantReg = 256;
  }

  // For VS with no input DCLs (vs_1_1): infer inputs from register reads using
  // the standard D3D9 register-to-usage mapping
  if (shader->info.isVertexShader && shader->inputDecls.empty()) {
    std::set<uint32_t> usedInputRegs;
    DxsoDecoder inputScanDecoder(tokens);
    DxsoInstructionContext inputScanCtx;
    while (inputScanDecoder.decodeInstruction(inputScanCtx)) {
      auto op = inputScanCtx.instruction.opcode;
      if (op == DxsoOpcode::Dcl || op == DxsoOpcode::Def ||
          op == DxsoOpcode::DefI || op == DxsoOpcode::DefB ||
          op == DxsoOpcode::Comment || op == DxsoOpcode::End)
        continue;
      // Scan source operands for v# (input) register usage
      uint32_t instLen = inputScanCtx.instruction.tokenLength;
      uint32_t numSrcs = (instLen > 1) ? instLen - 1 : 0;
      for (uint32_t s = 0; s < numSrcs && s < DxsoMaxSrcRegs; s++) {
        if (inputScanCtx.src[s].id.type == DxsoRegisterType::Input) {
          usedInputRegs.insert(inputScanCtx.src[s].id.num);
        }
      }
    }
    // Standard D3D9 vs_1_1 register mapping
    for (uint32_t reg : usedInputRegs) {
      DxsoUsage usage;
      uint32_t usageIdx;
      switch (reg) {
      case 0:  usage = DxsoUsage::Position;     usageIdx = 0; break;
      case 1:  usage = DxsoUsage::BlendWeight;   usageIdx = 0; break;
      case 2:  usage = DxsoUsage::BlendIndices;  usageIdx = 0; break;
      case 3:  usage = DxsoUsage::Normal;        usageIdx = 0; break;
      case 4:  usage = DxsoUsage::PointSize;     usageIdx = 0; break;
      case 5:  usage = DxsoUsage::Color;         usageIdx = 0; break;
      case 6:  usage = DxsoUsage::Color;         usageIdx = 1; break;
      default: usage = DxsoUsage::Texcoord;      usageIdx = reg - 7; break;
      }
      shader->inputDecls.push_back({
        .reg = reg,
        .usage = usage,
        .usageIndex = usageIdx,
      });
    }
  }

  // For SM2 VS: infer output semantics from instruction writes if no output DCLs exist
  // Register numbers are remapped to avoid collisions between register types:
  //   RasterizerOut[n] → slot n, AttributeOut[n] → slot 2+n, TexcoordOut[n] → slot 4+n
  if (shader->info.isVertexShader && shader->outputDecls.empty()) {
    std::set<uint32_t> writtenTexcoords;
    std::set<uint32_t> writtenColors;
    bool writtenFog = false;
    DxsoDecoder scanDecoder(tokens);
    DxsoInstructionContext scanCtx;
    while (scanDecoder.decodeInstruction(scanCtx)) {
      if (scanCtx.instruction.opcode == DxsoOpcode::Dcl ||
          scanCtx.instruction.opcode == DxsoOpcode::Def ||
          scanCtx.instruction.opcode == DxsoOpcode::DefI ||
          scanCtx.instruction.opcode == DxsoOpcode::DefB ||
          scanCtx.instruction.opcode == DxsoOpcode::Comment)
        continue;
      auto dstType = scanCtx.dst.id.type;
      auto dstNum = scanCtx.dst.id.num;
      if (dstType == DxsoRegisterType::Output ||
          dstType == DxsoRegisterType::TexcoordOut) {
        writtenTexcoords.insert(dstNum);
      } else if (dstType == DxsoRegisterType::AttributeOut) {
        writtenColors.insert(dstNum);
      } else if (dstType == DxsoRegisterType::RasterizerOut && dstNum == 0) {
        // oPos — position, handled by default
      } else if (dstType == DxsoRegisterType::RasterizerOut && dstNum == 1) {
        writtenFog = true;
      }
    }
    for (uint32_t reg : writtenTexcoords) {
      shader->outputDecls.push_back({
        .reg = 4 + reg,  // remapped: TexcoordOut[n] → slot 4+n
        .usage = DxsoUsage::Texcoord,
        .usageIndex = reg,
      });
    }
    for (uint32_t reg : writtenColors) {
      shader->outputDecls.push_back({
        .reg = 2 + reg,  // remapped: AttributeOut[n] → slot 2+n
        .usage = DxsoUsage::Color,
        .usageIndex = reg,
      });
    }
    if (writtenFog) {
      shader->outputDecls.push_back({
        .reg = 1,  // RasterizerOut[1] = oFog
        .usage = DxsoUsage::Fog,
        .usageIndex = 0,
      });
    }
  }

  // For SM2 PS: infer texture coordinate inputs from dcl t# registers
  if (!shader->info.isVertexShader && shader->info.majorVersion == 2) {
    DxsoDecoder scanDecoder2(tokens);
    DxsoInstructionContext scanCtx2;
    while (scanDecoder2.decodeInstruction(scanCtx2)) {
      if (scanCtx2.instruction.opcode == DxsoOpcode::Dcl) {
        if (scanCtx2.dst.id.type == DxsoRegisterType::Texture) {
          uint32_t reg = scanCtx2.dst.id.num;
          bool found = false;
          for (auto &dcl : shader->psInputDecls) {
            if (dcl.usage == DxsoUsage::Texcoord && dcl.usageIndex == reg) {
              found = true;
              break;
            }
          }
          if (!found) {
            shader->psInputDecls.push_back({
              .reg = reg,
              .usage = DxsoUsage::Texcoord,
              .usageIndex = reg,
            });
          }
        }
      }
    }
  }

  // For PS 1.x (majorVersion < 2): no DCLs at all — infer everything from instruction usage
  if (!shader->info.isVertexShader && shader->info.majorVersion < 2) {
    std::set<uint32_t> usedTexRegs;
    std::set<uint32_t> usedColorRegs;

    DxsoDecoder scanDecoder3(tokens);
    DxsoInstructionContext scanCtx3;
    while (scanDecoder3.decodeInstruction(scanCtx3)) {
      auto op = scanCtx3.instruction.opcode;
      if (op == DxsoOpcode::Dcl || op == DxsoOpcode::Def ||
          op == DxsoOpcode::DefI || op == DxsoOpcode::DefB ||
          op == DxsoOpcode::Comment || op == DxsoOpcode::End)
        continue;

      // tex tN or texcoord tN → implies sampler N and TEXCOORD[N] input
      if (op == DxsoOpcode::Tex || op == DxsoOpcode::TexCoord) {
        usedTexRegs.insert(scanCtx3.dst.id.num);
      }
      // texkill tN
      if (op == DxsoOpcode::TexKill) {
        usedTexRegs.insert(scanCtx3.dst.id.num);
      }

      // Scan source operands for v# (color) register usage
      // Only check operands that the instruction actually decoded
      uint32_t instLen = scanCtx3.instruction.tokenLength;
      uint32_t numSrcs = (instLen > 1) ? instLen - 1 : 0;
      for (uint32_t s = 0; s < numSrcs && s < DxsoMaxSrcRegs; s++) {
        if (scanCtx3.src[s].id.type == DxsoRegisterType::Input ||
            scanCtx3.src[s].id.type == DxsoRegisterType::AttributeOut) {
          usedColorRegs.insert(scanCtx3.src[s].id.num);
        }
      }
    }

    // Add inferred TEXCOORD inputs and sampler declarations
    for (uint32_t reg : usedTexRegs) {
      shader->psInputDecls.push_back({
        .reg = reg,
        .usage = DxsoUsage::Texcoord,
        .usageIndex = reg,
      });
      shader->samplerDecls.push_back({
        .reg = reg,
        .textureType = DxsoTextureType::Texture2D,
      });
    }
    // Add inferred COLOR inputs
    for (uint32_t reg : usedColorRegs) {
      shader->psInputDecls.push_back({
        .reg = reg,
        .usage = DxsoUsage::Color,
        .usageIndex = reg,
      });
    }
  }

  *ppShader = (sm50_ptr64_t)shader;
  return 0;
}

AIRCONV_API void DXSODestroy(sm50_ptr64_t pShader) {
  delete (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
}

AIRCONV_API int DXSOCompile(
  sm50_ptr64_t pShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName,
  sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;
  using namespace dxmt::dxso;

  if (ppError) *ppError = nullptr;

  auto *shader = (DxsoShaderInternal *)(void *)pShader;
  if (!shader || !ppBitcode) return 1;

  LLVMContext context;
  context.setOpaquePointers(false);

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (shader->info.isVertexShader) {
    compileVertexShader(shader, FunctionName, pArgs, context, *pModule);
  } else {
    compilePixelShader(shader, FunctionName, pArgs, context, *pModule);
  }

  runOptimizationPasses(*pModule);

  // Serialize to metallib
  auto *compiled = new DxsoCompiledBitcodeInternal();
  raw_svector_ostream OS(compiled->vec);
  metallib::MetallibWriter writer;
  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API uint32_t DXSOGetInputDeclCount(sm50_ptr64_t pShader) {
  auto *shader = (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
  if (!shader) return 0;
  return (uint32_t)shader->inputDecls.size();
}

AIRCONV_API void DXSOGetInputDecls(sm50_ptr64_t pShader, struct DXSO_INPUT_DECL *pDecls) {
  auto *shader = (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
  if (!shader || !pDecls) return;
  for (size_t i = 0; i < shader->inputDecls.size(); i++) {
    pDecls[i].reg = shader->inputDecls[i].reg;
    pDecls[i].usage = (uint32_t)shader->inputDecls[i].usage;
    pDecls[i].usageIndex = shader->inputDecls[i].usageIndex;
  }
}

AIRCONV_API uint32_t DXSOGetPSMaxTexcoordCount(sm50_ptr64_t pShader) {
  auto *shader = (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
  if (!shader) return 0;
  uint32_t maxTC = 0;
  for (auto &dcl : shader->psInputDecls) {
    if (dcl.usage == dxmt::dxso::DxsoUsage::Texcoord && dcl.usageIndex + 1 > maxTC)
      maxTC = dcl.usageIndex + 1;
  }
  return maxTC;
}

AIRCONV_API uint32_t DXSOGetSamplerDeclCount(sm50_ptr64_t pShader) {
  auto *shader = (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
  if (!shader) return 0;
  return (uint32_t)shader->samplerDecls.size();
}

AIRCONV_API void DXSOGetSamplerDecls(sm50_ptr64_t pShader, struct DXSO_SAMPLER_DECL *pDecls) {
  auto *shader = (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
  if (!shader || !pDecls) return;
  for (size_t i = 0; i < shader->samplerDecls.size(); i++) {
    pDecls[i].reg = shader->samplerDecls[i].reg;
    pDecls[i].textureType = (uint32_t)shader->samplerDecls[i].textureType;
  }
}

AIRCONV_API uint32_t DXSOGetVSHasFogOutput(sm50_ptr64_t pShader) {
  auto *shader = (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
  if (!shader) return 0;
  for (auto &dcl : shader->outputDecls) {
    if (dcl.usage == dxmt::dxso::DxsoUsage::Fog)
      return 1;
  }
  return 0;
}

AIRCONV_API uint32_t DXSOGetMaxConstantRegister(sm50_ptr64_t pShader) {
  auto *shader = (dxmt::dxso::DxsoShaderInternal *)(void *)pShader;
  if (!shader) return 256;
  return shader->maxConstantReg;
}

} // extern "C"

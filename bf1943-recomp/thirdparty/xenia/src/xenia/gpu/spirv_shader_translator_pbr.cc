// bf1943: metal/roughness lighting for the game's lit pixel shaders (--pbr).
// See runtime/xenia_shims/pbr_shaders.h for the shader table.

#include "xenia/gpu/spirv_shader_translator.h"

#include "pbr_shaders.h"

#include <vector>
#include "third_party/glslang/SPIRV/GLSL.std.450.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/spirv_compatibility.h"

namespace xe {
namespace gpu {

uint32_t SpirvShaderTranslator::GetExtraRegisterCount() {
  pbr_ = nullptr;
  pbr_started_ = false;
  pbr_env_blurred_ = false;
  pbr_raw_fetch_ = false;
  pbr_need_frag_coord_ = false;
  pbr_pom_var_ = spv::NoResult;
  pbr_pom_internal_ = false;
  for (spv::Id& var : pbr_tap_vars_) var = spv::NoResult;
  if (!is_pixel_shader() || !pbr::Enabled()) return 0;
  const pbr::ShaderConfig* config = pbr::Find(current_shader().ucode_data_hash());
  if (!config) return 0;
  const Shader::ConstantRegisterMap& map = current_shader().constant_register_map();
  std::vector<uint32_t> constants = {config->c_light, config->c_up, config->c_sky,
                                     config->c_ground, config->c_sun};
  for (const pbr::Scalar* s : {&config->env_scale, &config->pow}) {
    if (s->is_constant()) constants.push_back(s->constant);
  }
  for (uint32_t c : constants) {
    if (map.GetPackedFloatConstantIndex(c) == UINT32_MAX) {
      XELOGW("pbr: shader {:016X} does not use c{}, left as is",
             current_shader().ucode_data_hash(), c);
      return 0;
    }
  }
  pbr_ = config;
  pbr_need_frag_coord_ =
      config->show_fetch != UINT32_MAX || pbr::MaterialOutput();
  // The roughness/metallic fetch, the blurred cube map fetch, debug, and for
  // parallax shifted coordinates and a height fetch.
  return 5;
}

spv::Id SpirvShaderTranslator::LoadExtraRegister(uint32_t index) {
  id_vector_temp_util_.clear();
  id_vector_temp_util_.push_back(
      builder_->makeIntConstant(int(extra_register_base() + index)));
  return builder_->createLoad(
      builder_->createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                  id_vector_temp_util_),
      spv::NoPrecision);
}

namespace {
// Pack normal maps keep alpha at 254/255 or below; the raw view of every other
// texture returns 1.
constexpr float kPbrAlphaLimit = 0.998f;
// Pack textures keep alpha in 191-253 (the height); a game texture whose alpha
// is a mask or anything lower must not count as pack data (it was taken for
// smooth metal: dirt turned into tinted mirrors).
constexpr float kPbrAlphaMin = 189.5f / 255.0f;
}  // namespace

void SpirvShaderTranslator::ProcessInstructionBegin(uint32_t address) {
  if (!pbr_) return;
  if (!pbr_started_) {
    pbr_started_ = true;
    StorePbrTaps(pbr::kTapAtStart);
  }
  auto clone_fetch = [&](uint32_t extra_index) {
    ParsedTextureFetchInstruction fetch = last_texture_fetch();
    fetch.result.storage_target = InstructionStorageTarget::kRegister;
    fetch.result.storage_index = extra_register_base() + extra_index;
    fetch.result.storage_addressing_mode =
        InstructionStorageAddressingMode::kAbsolute;
    fetch.result.is_clamped = false;
    fetch.result.original_write_mask = 0b1111;
    for (uint32_t i = 0; i < 4; ++i) {
      fetch.result.components[i] = SwizzleSource(uint32_t(SwizzleSource::kX) + i);
    }
    return fetch;
  };
  if (address == pbr_->show_fetch && input_fragment_coordinates_ != spv::NoResult) {
    // Debug: the texture at the pixel's screen position.
    EnsureBuildPointAvailable();
    spv::Id frag = builder_->createLoad(input_fragment_coordinates_, spv::NoPrecision);
    spv::Id uv = builder_->createCompositeConstruct(
        type_float4_,
        {builder_->createBinOp(spv::OpFDiv, type_float_,
                               builder_->createBinOp(spv::OpFSub, type_float_,
                                                     builder_->createCompositeExtract(frag, type_float_, 0),
                                                     builder_->makeFloatConstant(pbr_->show_x0)),
                               builder_->makeFloatConstant(pbr_->show_width)),
         builder_->createBinOp(spv::OpFDiv, type_float_,
                               builder_->createBinOp(spv::OpFSub, type_float_,
                                                     builder_->createCompositeExtract(frag, type_float_, 1),
                                                     builder_->makeFloatConstant(pbr_->show_y0)),
                               builder_->makeFloatConstant(pbr_->show_height)),
         builder_->makeFloatConstant(0.0f), builder_->makeFloatConstant(0.0f)});
    id_vector_temp_util_.clear();
    id_vector_temp_util_.push_back(builder_->makeIntConstant(int(extra_register_base() + 2)));
    builder_->createStore(uv, builder_->createAccessChain(spv::StorageClassFunction,
                                                          var_main_registers_, id_vector_temp_util_));
    ParsedTextureFetchInstruction fetch = clone_fetch(2);
    fetch.operands[0].storage_source = InstructionStorageSource::kRegister;
    fetch.operands[0].storage_index = extra_register_base() + 2;
    fetch.operands[0].storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
    fetch.operands[0].is_negated = false;
    fetch.operands[0].is_absolute_value = false;
    fetch.operands[0].component_count = 2;
    fetch.operands[0].components[0] = SwizzleSource::kX;
    fetch.operands[0].components[1] = SwizzleSource::kY;
    fetch.attributes.offset_x = 0.0f;
    fetch.attributes.offset_y = 0.0f;
    ProcessTextureFetchInstruction(fetch);
  }
  if (address == pbr_->rm_fetch &&
      last_texture_fetch().opcode == ucode::FetchOpcode::kTextureFetch) {
    // Parallax first: this fetch and every later one then read shifted.
    if (pbr_->pom * pbr::PomScale() > 0.0f && last_texture_fetch().dimension == xenos::FetchOpDimension::k2D) {
      EmitPbrParallax(last_texture_fetch());
    }
    // The normal map again, before the game's fetch may overwrite its
    // coordinates: all four channels of the image, into the extra register.
    ParsedTextureFetchInstruction fetch = clone_fetch(0);
    pbr_raw_fetch_ = true;
    ProcessTextureFetchInstruction(fetch);
    pbr_raw_fetch_ = false;
  }
  if (pbr_->env.valid() && address == pbr_->env.address && pbr_->blur > 0.0f &&
      last_texture_fetch().opcode == ucode::FetchOpcode::kTextureFetch &&
      last_texture_fetch().dimension == xenos::FetchOpDimension::kCube) {
    // The cube map again with a LOD bias of roughness * blur.
    EnsureBuildPointAvailable();
    ParsedTextureFetchInstruction fetch = clone_fetch(1);
    fetch.attributes.use_register_lod = true;
    spv::Id old_lod = builder_->createLoad(var_main_tfetch_lod_, spv::NoPrecision);
    builder_->createStore(
        builder_->createBinOp(spv::OpFMul, type_float_, PbrRoughness(),
                              builder_->makeFloatConstant(pbr_->blur)),
        var_main_tfetch_lod_);
    ProcessTextureFetchInstruction(fetch);
    EnsureBuildPointAvailable();
    builder_->createStore(old_lod, var_main_tfetch_lod_);
    pbr_env_blurred_ = true;
  }
}

void SpirvShaderTranslator::ProcessInstructionEnd(uint32_t address) {
  if (!pbr_) return;
  StorePbrTaps(address);
  if (address == pbr_->out.address) EmitPbrLighting();
}

void SpirvShaderTranslator::StorePbrTaps(uint32_t address) {
  const pbr::Tap* taps[] = {&pbr_->n,   &pbr_->v,   &pbr_->albedo,
                            &pbr_->lit, &pbr_->env, &pbr_->so,
                            &pbr_->ao,  &pbr_->wp};
  for (size_t i = 0; i < std::size(taps); ++i) {
    if (!taps[i]->valid() || taps[i]->address != address) continue;
    EnsureBuildPointAvailable();
    if (pbr_tap_vars_[i] == spv::NoResult) {
      pbr_tap_vars_[i] = builder_->createVariable(
          spv::NoPrecision, spv::StorageClassFunction, type_float4_,
          fmt::format("xe_var_pbr_tap_{}", i).c_str(), const_float4_0_);
    }
    id_vector_temp_util_.clear();
    id_vector_temp_util_.push_back(builder_->makeIntConstant(int(taps[i]->reg)));
    builder_->createStore(
        builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassFunction,
                                        var_main_registers_, id_vector_temp_util_),
            spv::NoPrecision),
        pbr_tap_vars_[i]);
  }
}

spv::Id SpirvShaderTranslator::PbrScalar(const pbr::Scalar& s) {
  if (!s.is_constant()) return builder_->makeFloatConstant(s.value);
  InstructionOperand op;
  op.storage_source = InstructionStorageSource::kConstantFloat;
  op.storage_index = s.constant;
  return builder_->createCompositeExtract(LoadOperandStorage(op), type_float_,
                                          s.comp);
}

spv::Id SpirvShaderTranslator::PbrRoughness() {
  SpirvBuilder& b = *builder_;
  const spv::Id f1 = type_float_;
  spv::Id rm = LoadExtraRegister(0);
  spv::Id rm_alpha = b.createCompositeExtract(rm, f1, 3);
  spv::Id has_pbr = b.createBinOp(
      spv::OpLogicalAnd, b.makeBoolType(),
      b.createBinOp(spv::OpFOrdLessThan, b.makeBoolType(), rm_alpha, b.makeFloatConstant(kPbrAlphaLimit)),
      b.createBinOp(spv::OpFOrdGreaterThanEqual, b.makeBoolType(), rm_alpha, b.makeFloatConstant(kPbrAlphaMin)));
  // Without pack data: from the game's specular power, sqrt(sqrt(2/(n+2))).
  spv::Id fallback = b.makeFloatConstant(pbr_->rough);
  if (pbr_->pow.is_constant()) {
    spv::Id n = b.createBinBuiltinCall(f1, ext_inst_glsl_std_450_, GLSLstd450FMax,
                                       PbrScalar(pbr_->pow), b.makeFloatConstant(1.0f));
    fallback = b.createUnaryBuiltinCall(
        f1, ext_inst_glsl_std_450_, GLSLstd450Sqrt,
        b.createUnaryBuiltinCall(
            f1, ext_inst_glsl_std_450_, GLSLstd450Sqrt,
            b.createBinOp(spv::OpFDiv, f1, b.makeFloatConstant(2.0f),
                          b.createBinOp(spv::OpFAdd, f1, n, b.makeFloatConstant(2.0f)))));
  }
  // Blue: roughness in its upper half, in the lower half where metallic.
  spv::Id blue = b.createCompositeExtract(rm, f1, 2);
  spv::Id upper = b.createBinOp(spv::OpFOrdGreaterThanEqual, b.makeBoolType(), blue,
                                b.makeFloatConstant(0.5f));
  spv::Id pack_rough = b.createBinOp(
      spv::OpFMul, f1,
      b.createBinOp(spv::OpFSub, f1, blue,
                    b.createTriOp(spv::OpSelect, f1, upper, b.makeFloatConstant(128.0f / 255.0f),
                                  b.makeFloatConstant(0.0f))),
      b.makeFloatConstant(255.0f / 127.0f));
  return b.createTriBuiltinCall(
      f1, ext_inst_glsl_std_450_, GLSLstd450NClamp,
      b.createTriOp(spv::OpSelect, f1, has_pbr,
                    b.createBinOp(spv::OpFMul, f1, pack_rough,
                                  b.makeFloatConstant(pbr_->rough_scale)),
                    fallback),
      b.makeFloatConstant(0.045f), b.makeFloatConstant(1.0f));
}

void SpirvShaderTranslator::EmitPbrParallax(
    const ParsedTextureFetchInstruction& normal_fetch) {
  if (pbr_tap_vars_[7] == spv::NoResult || !pbr_->wp.valid()) return;
  if (current_shader().constant_register_map().GetPackedFloatConstantIndex(pbr_->c_cam) ==
      UINT32_MAX) {
    return;
  }
  EnsureBuildPointAvailable();
  SpirvBuilder& b = *builder_;
  const spv::Id f1 = type_float_, f2 = type_float2_, f3 = type_float3_;
  const spv::Id bt = b.makeBoolType();
  auto k = [&](float v) { return b.makeFloatConstant(v); };
  auto add = [&](spv::Id t, spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFAdd, t, x, y); };
  auto sub = [&](spv::Id t, spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFSub, t, x, y); };
  auto mul = [&](spv::Id t, spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFMul, t, x, y); };
  auto div = [&](spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFDiv, f1, x, y); };
  auto vs = [&](spv::Id t, spv::Id v, spv::Id s) { return b.createBinOp(spv::OpVectorTimesScalar, t, v, s); };
  auto dot = [&](spv::Id x, spv::Id y) { return b.createBinOp(spv::OpDot, f1, x, y); };
  auto ext1 = [&](int op, spv::Id t, spv::Id x) { return b.createUnaryBuiltinCall(t, ext_inst_glsl_std_450_, op, x); };
  auto ext2 = [&](int op, spv::Id t, spv::Id x, spv::Id y) { return b.createBinBuiltinCall(t, ext_inst_glsl_std_450_, op, x, y); };
  auto ext3 = [&](int op, spv::Id t, spv::Id x, spv::Id y, spv::Id z) { return b.createTriBuiltinCall(t, ext_inst_glsl_std_450_, op, x, y, z); };
  auto sat = [&](spv::Id x) { return ext3(GLSLstd450NClamp, f1, x, k(0.0f), k(1.0f)); };
  auto comp = [&](spv::Id v, uint32_t c) { return b.createCompositeExtract(v, f1, c); };
  auto sel = [&](spv::Id t, spv::Id c, spv::Id x, spv::Id y) { return b.createTriOp(spv::OpSelect, t, c, x, y); };

  // The texture coordinates of the normal map, the world position and the
  // view vector.
  const InstructionOperand& op = normal_fetch.operands[0];
  spv::Id uv_all = GetOperandComponents(LoadOperandStorage(op), op, 0b0011);
  spv::Id uv = uv_all;
  spv::Id wp4 = b.createLoad(pbr_tap_vars_[7], spv::NoPrecision);
  spv::Id p = b.createCompositeConstruct(
      f3, {comp(wp4, pbr_->wp.comps[0]), comp(wp4, pbr_->wp.comps[1]),
           comp(wp4, pbr_->wp.comps[std::min(2u, pbr_->wp.count - 1)])});
  InstructionOperand cam_op;
  cam_op.storage_source = InstructionStorageSource::kConstantFloat;
  cam_op.storage_index = pbr_->c_cam;
  spv::Id cam4 = LoadOperandStorage(cam_op);
  spv::Id cam = b.createCompositeConstruct(f3, {comp(cam4, 0), comp(cam4, 1), comp(cam4, 2)});
  spv::Id to_eye = sub(f3, cam, p);
  spv::Id dist = ext1(GLSLstd450Length, f1, to_eye);
  spv::Id v = vs(f3, to_eye, div(k(1.0f), ext2(GLSLstd450FMax, f1, dist, k(1e-4f))));

  // The surface frame from screen derivatives (no tangents needed): the face
  // normal, and the gradients of u and v over the world (texture units a
  // metre).
  spv::Id dpdx = b.createUnaryOp(spv::OpDPdx, f3, p);
  spv::Id dpdy = b.createUnaryOp(spv::OpDPdy, f3, p);
  spv::Id duvdx = b.createUnaryOp(spv::OpDPdx, f2, uv);
  spv::Id duvdy = b.createUnaryOp(spv::OpDPdy, f2, uv);
  spv::Id face = ext2(GLSLstd450Cross, f3, dpdx, dpdy);
  spv::Id face_len = ext1(GLSLstd450Length, f1, face);
  spv::Id n = vs(f3, face, div(k(1.0f), ext2(GLSLstd450FMax, f1, face_len, k(1e-20f))));
  n = vs(f3, n, ext1(GLSLstd450FSign, f1, dot(n, v)));
  spv::Id dp2perp = ext2(GLSLstd450Cross, f3, dpdy, n);
  spv::Id dp1perp = ext2(GLSLstd450Cross, f3, n, dpdx);
  spv::Id det = dot(dpdx, dp2perp);
  spv::Id frame_ok = b.createBinOp(
      spv::OpLogicalAnd, bt,
      b.createBinOp(spv::OpFOrdGreaterThan, bt, ext1(GLSLstd450FAbs, f1, det), k(1e-20f)),
      b.createBinOp(spv::OpFOrdGreaterThan, bt, face_len, k(0.0f)));
  spv::Id inv_det = div(k(1.0f), sel(f1, frame_ok, det, k(1.0f)));
  spv::Id grad_u = vs(f3, add(f3, vs(f3, dp2perp, comp(duvdx, 0)), vs(f3, dp1perp, comp(duvdy, 0))), inv_det);
  spv::Id grad_v = vs(f3, add(f3, vs(f3, dp2perp, comp(duvdx, 1)), vs(f3, dp1perp, comp(duvdy, 1))), inv_det);
  // Down into the surface along the view ray: texture units a metre of depth.
  spv::Id n_v = ext2(GLSLstd450FMax, f1, dot(n, v), k(0.25f));
  spv::Id dir = b.createCompositeConstruct(
      f2, {div(b.createUnaryOp(spv::OpFNegate, f1, dot(grad_u, v)), n_v),
           div(b.createUnaryOp(spv::OpFNegate, f1, dot(grad_v, v)), n_v)});
  // A texture's relief scales with how big it is in the world: 1.5 % of a
  // repeat (fine detail on a gun, deeper planks on a deck), at most pom
  // metres; full near, none from 45 m (it would only shimmer).
  spv::Id texels_per_m = ext2(GLSLstd450FMax, f1, ext1(GLSLstd450Length, f1, grad_u),
                              ext1(GLSLstd450Length, f1, grad_v));
  spv::Id depth = ext2(GLSLstd450FMin, f1, k(pbr_->pom * pbr::PomScale()),
                       div(k(0.015f), ext2(GLSLstd450FMax, f1, texels_per_m, k(1e-3f))));
  depth = mul(f1, depth, sat(sub(f1, k(1.5f), div(dist, k(30.0f)))));
  depth = sel(f1, frame_ok, depth, k(0.0f));

  // The height (alpha of the normal map without swizzle; 1, flat, for maps
  // without one) at uv + dir * d.
  auto height_at = [&](spv::Id d) {
    spv::Id at = add(f2, uv, vs(f2, dir, d));
    id_vector_temp_util_.clear();
    id_vector_temp_util_.push_back(b.makeIntConstant(int(extra_register_base() + 3)));
    b.createStore(b.createCompositeConstruct(type_float4_, {comp(at, 0), comp(at, 1), k(0.0f), k(0.0f)}),
                  b.createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                      id_vector_temp_util_));
    ParsedTextureFetchInstruction fetch = normal_fetch;
    fetch.result.storage_target = InstructionStorageTarget::kRegister;
    fetch.result.storage_index = extra_register_base() + 4;
    fetch.result.storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
    fetch.result.is_clamped = false;
    fetch.result.original_write_mask = 0b1111;
    for (uint32_t i = 0; i < 4; ++i) {
      fetch.result.components[i] = SwizzleSource(uint32_t(SwizzleSource::kX) + i);
    }
    fetch.operands[0].storage_source = InstructionStorageSource::kRegister;
    fetch.operands[0].storage_index = extra_register_base() + 3;
    fetch.operands[0].storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
    fetch.operands[0].is_negated = false;
    fetch.operands[0].is_absolute_value = false;
    fetch.operands[0].component_count = 2;
    fetch.operands[0].components[0] = SwizzleSource::kX;
    fetch.operands[0].components[1] = SwizzleSource::kY;
    fetch.is_predicated = false;
    pbr_raw_fetch_ = true;
    pbr_pom_internal_ = true;
    ProcessTextureFetchInstruction(fetch);
    pbr_pom_internal_ = false;
    pbr_raw_fetch_ = false;
    EnsureBuildPointAvailable();
    // Alpha 191-253 holds the height 0-1 (above 253/255, no pack data: 1).
    return sat(mul(f1, sub(f1, comp(LoadExtraRegister(4), 3), k(191.0f / 255.0f)),
                   k(255.0f / 62.0f)));
  };
  // March down in eight steps; where the ray first goes under the height
  // field, the crossing between the last two steps.
  constexpr int kSteps = 8;
  spv::Id d_prev = k(0.0f);
  spv::Id s_prev = mul(f1, sub(f1, k(1.0f), height_at(k(0.0f))), depth);
  spv::Id found = b.createBinOp(spv::OpFOrdLessThanEqual, bt, s_prev, k(1e-5f));
  spv::Id hit = sel(f1, found, k(0.0f), depth);
  for (int i = 1; i <= kSteps; ++i) {
    spv::Id d = mul(f1, depth, k(float(i) / float(kSteps)));
    spv::Id s = mul(f1, sub(f1, k(1.0f), height_at(d)), depth);
    spv::Id cross = b.createBinOp(spv::OpLogicalAnd, bt, b.createUnaryOp(spv::OpLogicalNot, bt, found),
                                  b.createBinOp(spv::OpFOrdGreaterThanEqual, bt, d, s));
    spv::Id gap_prev = sub(f1, s_prev, d_prev);
    spv::Id t = sat(div(gap_prev, ext2(GLSLstd450FMax, f1, sub(f1, gap_prev, sub(f1, s, d)), k(1e-6f))));
    hit = sel(f1, cross, add(f1, d_prev, mul(f1, sub(f1, d, d_prev), t)), hit);
    found = b.createBinOp(spv::OpLogicalOr, bt, found, cross);
    d_prev = d;
    s_prev = s;
  }
  spv::Id delta = vs(f2, dir, hit);
  // As a screen displacement: J (a, b) = delta, J the coordinates' derivatives.
  spv::Id jdet = sub(f1, mul(f1, comp(duvdx, 0), comp(duvdy, 1)), mul(f1, comp(duvdy, 0), comp(duvdx, 1)));
  spv::Id j_ok = b.createBinOp(spv::OpFOrdGreaterThan, bt, ext1(GLSLstd450FAbs, f1, jdet), k(1e-24f));
  spv::Id inv_j = div(k(1.0f), sel(f1, j_ok, jdet, k(1.0f)));
  spv::Id sa = mul(f1, sub(f1, mul(f1, comp(delta, 0), comp(duvdy, 1)), mul(f1, comp(duvdy, 0), comp(delta, 1))), inv_j);
  spv::Id sb = mul(f1, sub(f1, mul(f1, comp(duvdx, 0), comp(delta, 1)), mul(f1, comp(delta, 0), comp(duvdx, 1))), inv_j);
  spv::Id ab = sel(f2, j_ok, b.createCompositeConstruct(f2, {sa, sb}), const_float2_0_);
  pbr_pom_var_ = b.createVariable(spv::NoPrecision, spv::StorageClassFunction, f2,
                                  "xe_var_pbr_parallax", const_float2_0_);
  b.createStore(ab, pbr_pom_var_);
}

bool SpirvShaderTranslator::PbrShiftFetch(const ParsedTextureFetchInstruction& instr) {
  if (!pbr_ || pbr_pom_var_ == spv::NoResult || pbr_pom_internal_ ||
      instr.opcode != ucode::FetchOpcode::kTextureFetch ||
      instr.dimension != xenos::FetchOpDimension::k2D ||
      instr.attributes.mag_filter == xenos::TextureFilter::kPoint ||
      IsGameShadowFetch(instr) || instr.operands[0].component_count < 2) {
    return false;
  }
  EnsureBuildPointAvailable();
  SpirvBuilder& b = *builder_;
  const spv::Id f1 = type_float_, f2 = type_float2_;
  const InstructionOperand& op = instr.operands[0];
  uint32_t count = std::min(op.component_count, 4u);
  spv::Id c = GetOperandComponents(LoadOperandStorage(op), op, (1u << count) - 1);
  spv::Id xy = count == 2 ? c
                          : b.createCompositeConstruct(f2, {b.createCompositeExtract(c, f1, 0),
                                                            b.createCompositeExtract(c, f1, 1)});
  // This fetch's coordinates moved by the same screen displacement.
  spv::Id ab = b.createLoad(pbr_pom_var_, spv::NoPrecision);
  spv::Id dx = b.createUnaryOp(spv::OpDPdx, f2, xy);
  spv::Id dy = b.createUnaryOp(spv::OpDPdy, f2, xy);
  xy = b.createBinOp(
      spv::OpFAdd, f2, xy,
      b.createBinOp(spv::OpFAdd, f2,
                    b.createBinOp(spv::OpVectorTimesScalar, f2, dx, b.createCompositeExtract(ab, f1, 0)),
                    b.createBinOp(spv::OpVectorTimesScalar, f2, dy, b.createCompositeExtract(ab, f1, 1))));
  spv::Id z = count > 2 ? b.createCompositeExtract(c, f1, 2) : b.makeFloatConstant(0.0f);
  spv::Id w = count > 3 ? b.createCompositeExtract(c, f1, 3) : b.makeFloatConstant(0.0f);
  id_vector_temp_util_.clear();
  id_vector_temp_util_.push_back(b.makeIntConstant(int(extra_register_base() + 3)));
  b.createStore(b.createCompositeConstruct(type_float4_, {b.createCompositeExtract(xy, f1, 0),
                                                          b.createCompositeExtract(xy, f1, 1), z, w}),
                b.createAccessChain(spv::StorageClassFunction, var_main_registers_,
                                    id_vector_temp_util_));
  ParsedTextureFetchInstruction shifted = instr;
  shifted.operands[0].storage_source = InstructionStorageSource::kRegister;
  shifted.operands[0].storage_index = extra_register_base() + 3;
  shifted.operands[0].storage_addressing_mode = InstructionStorageAddressingMode::kAbsolute;
  shifted.operands[0].is_negated = false;
  shifted.operands[0].is_absolute_value = false;
  for (uint32_t i = 0; i < 4; ++i) {
    shifted.operands[0].components[i] = SwizzleSource(uint32_t(SwizzleSource::kX) + i);
  }
  pbr_pom_internal_ = true;
  ProcessTextureFetchInstruction(shifted);
  pbr_pom_internal_ = false;
  return true;
}

void SpirvShaderTranslator::EmitPbrLighting() {
  EnsureBuildPointAvailable();
  SpirvBuilder& b = *builder_;
  const spv::Id f1 = type_float_, f3 = type_float3_;
  auto k = [&](float v) { return b.makeFloatConstant(v); };
  auto k3 = [&](float v) { return b.smearFloatConstant(v, f3); };
  auto add = [&](spv::Id t, spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFAdd, t, x, y); };
  auto sub = [&](spv::Id t, spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFSub, t, x, y); };
  auto mul = [&](spv::Id t, spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFMul, t, x, y); };
  auto div = [&](spv::Id x, spv::Id y) { return b.createBinOp(spv::OpFDiv, f1, x, y); };
  auto scale = [&](spv::Id v3, spv::Id s) { return b.createBinOp(spv::OpVectorTimesScalar, f3, v3, s); };
  auto dot = [&](spv::Id x, spv::Id y) { return b.createBinOp(spv::OpDot, f1, x, y); };
  auto ext1 = [&](int op, spv::Id t, spv::Id x) { return b.createUnaryBuiltinCall(t, ext_inst_glsl_std_450_, op, x); };
  auto ext2 = [&](int op, spv::Id t, spv::Id x, spv::Id y) { return b.createBinBuiltinCall(t, ext_inst_glsl_std_450_, op, x, y); };
  auto ext3 = [&](int op, spv::Id t, spv::Id x, spv::Id y, spv::Id z) { return b.createTriBuiltinCall(t, ext_inst_glsl_std_450_, op, x, y, z); };
  auto sat = [&](spv::Id x) { return ext3(GLSLstd450NClamp, f1, x, k(0.0f), k(1.0f)); };
  auto normalize = [&](spv::Id v3) { return ext1(GLSLstd450Normalize, f3, v3); };
  auto mix3 = [&](spv::Id x, spv::Id y, spv::Id a) { return ext3(GLSLstd450FMix, f3, x, y, b.smearScalar(spv::NoPrecision, a, f3)); };

  auto component = [&](spv::Id v4, uint32_t c) { return b.createCompositeExtract(v4, f1, c); };
  auto tap = [&](size_t i, const pbr::Tap& t, uint32_t count, float fallback) -> spv::Id {
    spv::Id c[3];
    spv::Id v4 = pbr_tap_vars_[i] != spv::NoResult
                     ? b.createLoad(pbr_tap_vars_[i], spv::NoPrecision) : spv::NoResult;
    for (uint32_t j = 0; j < count; ++j) {
      c[j] = (v4 != spv::NoResult && t.valid())
                 ? component(v4, t.comps[std::min(j, t.count - 1)]) : k(fallback);
    }
    if (count == 1) return c[0];
    return b.createCompositeConstruct(f3, {c[0], c[1], c[2]});
  };
  auto constant = [&](uint32_t index) {
    InstructionOperand op;
    op.storage_source = InstructionStorageSource::kConstantFloat;
    op.storage_index = index;
    spv::Id v4 = LoadOperandStorage(op);
    return b.createCompositeConstruct(f3, {component(v4, 0), component(v4, 1), component(v4, 2)});
  };

  spv::Id n = normalize(tap(0, pbr_->n, 3, 0.0f));
  spv::Id v_raw = tap(1, pbr_->v, 3, 0.0f);
  spv::Id v = normalize(v_raw);
  spv::Id ao = sat(tap(6, pbr_->ao, 1, 1.0f));
  spv::Id albedo = scale(tap(2, pbr_->albedo, 3, 0.5f), ao);

  // Relief: the normal map detail exaggerated. The flat face normal comes
  // from screen derivatives of the position (the camera minus the view
  // vector), which are constant over a triangle, so this does not shimmer;
  // the game's mip-filtered normal minus the face normal is the detail, scaled
  // by bump. Only where the two are close (normal map detail, not the smooth
  // shading of curved meshes) and fading out by 60 m.
  if (pbr_->bump > 0.0f && pbr_->v.valid()) {
    spv::Id sx = b.createUnaryOp(spv::OpDPdx, f3, v_raw);
    spv::Id sy = b.createUnaryOp(spv::OpDPdy, f3, v_raw);
    spv::Id face = ext2(GLSLstd450Cross, f3, sx, sy);
    spv::Id face_len = ext1(GLSLstd450Length, f1, face);
    face = scale(face, div(k(1.0f), ext2(GLSLstd450FMax, f1, face_len, k(1e-20f))));
    face = scale(face, ext1(GLSLstd450FSign, f1, dot(face, n)));
    spv::Id n_f = dot(n, face);
    spv::Id dist = ext1(GLSLstd450Length, f1, v_raw);
    spv::Id weight = mul(f1, ext3(GLSLstd450SmoothStep, f1, k(0.80f), k(0.92f), n_f),
                         sat(sub(f1, k(1.5f), mul(f1, dist, k(1.0f / 40.0f)))));
    weight = b.createTriOp(spv::OpSelect, f1,
                           b.createBinOp(spv::OpFOrdGreaterThan, b.makeBoolType(), face_len, k(0.0f)),
                           weight, k(0.0f));
    spv::Id detail = sub(f3, n, scale(face, n_f));
    n = normalize(add(f3, n, scale(detail, mul(f1, weight, k(pbr_->bump)))));
  }
  spv::Id lit = sat(tap(3, pbr_->lit, 1, 1.0f));
  spv::Id so = mul(f1, sat(tap(5, pbr_->so, 1, 1.0f)), ao);

  spv::Id rm = LoadExtraRegister(0);
  spv::Id rm_a = component(rm, 3);
  spv::Id has_pbr = b.createBinOp(
      spv::OpLogicalAnd, b.makeBoolType(),
      b.createBinOp(spv::OpFOrdLessThan, b.makeBoolType(), rm_a, k(kPbrAlphaLimit)),
      b.createBinOp(spv::OpFOrdGreaterThanEqual, b.makeBoolType(), rm_a, k(kPbrAlphaMin)));
  spv::Id rough = PbrRoughness();
  spv::Id metal = b.createTriOp(
      spv::OpSelect, f1, has_pbr,
      b.createTriOp(spv::OpSelect, f1,
                    b.createBinOp(spv::OpFOrdGreaterThanEqual, b.makeBoolType(),
                                  component(rm, 2), k(0.5f)),
                    k(0.0f), k(0.9f)),
      k(pbr_->metal));

  spv::Id l = normalize(constant(pbr_->c_light));
  spv::Id up = constant(pbr_->c_up);
  spv::Id sky = constant(pbr_->c_sky), ground = constant(pbr_->c_ground);
  spv::Id sun = scale(constant(pbr_->c_sun), lit);

  spv::Id h = normalize(add(f3, v, l));
  spv::Id n_l = sat(dot(n, l));
  spv::Id n_v = ext3(GLSLstd450NClamp, f1, dot(n, v), k(1.0f / 256.0f), k(1.0f));
  spv::Id n_h = sat(dot(n, h));
  spv::Id v_h = sat(dot(v, h));

  // The game's hemisphere ambient.
  spv::Id hemi = add(f1, mul(f1, dot(n, up), k(0.5f)), k(0.5f));
  spv::Id ambient = add(f3, ground, scale(sub(f3, sky, ground), hemi));

  spv::Id f0 = mix3(k3(0.04f), albedo, metal);

  // Sun: GGX distribution, Schlick-Smith visibility, Schlick Fresnel. The game
  // lights diffuse as albedo * colour * N.L (no 1/pi), so the specular BRDF
  // gets the matching factor of pi.
  spv::Id a = mul(f1, rough, rough);
  spv::Id a2 = mul(f1, a, a);
  spv::Id d_den = add(f1, mul(f1, mul(f1, n_h, n_h), sub(f1, a2, k(1.0f))), k(1.0f));
  spv::Id d = div(a2, mul(f1, d_den, d_den));  // * 1/pi, cancelled
  spv::Id kk = mul(f1, mul(f1, add(f1, rough, k(1.0f)), add(f1, rough, k(1.0f))), k(0.125f));
  spv::Id g_l = add(f1, mul(f1, n_l, sub(f1, k(1.0f), kk)), kk);
  spv::Id g_v = add(f1, mul(f1, n_v, sub(f1, k(1.0f), kk)), kk);
  spv::Id vis = div(k(0.25f), mul(f1, g_l, g_v));  // G / (4 N.L N.V)
  spv::Id fw = ext2(GLSLstd450Pow, f1, sub(f1, k(1.0f), v_h), k(5.0f));
  spv::Id fresnel = add(f3, f0, scale(sub(f3, k3(1.0f), f0), fw));
  spv::Id spec_sun = mul(f3, fresnel, b.createBinOp(spv::OpVectorTimesScalar, f3, sun,
                                                    ext2(GLSLstd450FMin, f1,
                                                         mul(f1, mul(f1, d, vis), n_l),
                                                         k(pbr_->spec_max))));

  // Environment: rough surfaces reflect something close to the ambient light.
  // Karis' analytic approximation of the split-sum environment BRDF.
  spv::Id r0 = add(f1, mul(f1, rough, k(-1.0f)), k(1.0f));
  spv::Id r1 = add(f1, mul(f1, rough, k(-0.0275f)), k(0.0425f));
  spv::Id r2 = add(f1, mul(f1, rough, k(-0.572f)), k(1.04f));
  spv::Id r3 = add(f1, mul(f1, rough, k(0.022f)), k(-0.04f));
  spv::Id a004 = add(f1, mul(f1, ext2(GLSLstd450FMin, f1, mul(f1, r0, r0),
                                      ext1(GLSLstd450Exp2, f1, mul(f1, k(-9.28f), n_v))),
                             r0),
                     r1);
  spv::Id env_a = add(f1, mul(f1, a004, k(-1.04f)), r2);
  spv::Id env_b = add(f1, mul(f1, a004, k(1.04f)), r3);
  spv::Id env_brdf = add(f3, scale(f0, env_a), k3(0.0f));
  env_brdf = add(f3, env_brdf, b.smearScalar(spv::NoPrecision, env_b, f3));
  // Environment: the blurred cube map fetch, or the game's; rough surfaces
  // reflect something close to the ambient light. Without a cube map, the
  // ambient light.
  spv::Id env_light = ambient;
  if (pbr_env_blurred_ || pbr_->env.valid()) {
    spv::Id env;
    if (pbr_env_blurred_) {
      spv::Id blurred = LoadExtraRegister(1);
      env = b.createCompositeConstruct(
          f3, {component(blurred, 0), component(blurred, 1), component(blurred, 2)});
    } else {
      env = tap(4, pbr_->env, 3, 0.0f);
    }
    env = scale(env, PbrScalar(pbr_->env_scale));
    spv::Id to_ambient = pbr_env_blurred_ ? mul(f1, rough, rough)
                                          : sat(mul(f1, rough, k(1.25f)));
    env_light = mix3(env, ambient, to_ambient);
  }
  spv::Id spec_env = scale(mul(f3, env_light, env_brdf), so);

  // The material for the scene effects: the nearest pixel's (by its distance
  // from the camera) roughness and metallic.
  if (buffer_bf_material_ != spv::NoResult &&
      input_fragment_coordinates_ != spv::NoResult && pbr_->v.valid()) {
    const spv::Id u1 = type_uint_;
    auto ku = [&](uint32_t v) { return b.makeUintConstant(v); };
    spv::Id frag = b.createLoad(input_fragment_coordinates_, spv::NoPrecision);
    spv::Id x = b.createUnaryOp(spv::OpConvertFToU, u1, component(frag, 0));
    spv::Id y = b.createUnaryOp(spv::OpConvertFToU, u1, component(frag, 1));
    spv::Id inside = b.createBinOp(
        spv::OpLogicalAnd, type_bool_,
        b.createBinOp(spv::OpULessThan, type_bool_, x, ku(pbr::kMaterialPitch)),
        b.createBinOp(spv::OpULessThan, type_bool_, y, ku(pbr::kMaterialRows)));
    SpirvBuilder::IfBuilder if_inside(inside, spv::SelectionControlDontFlattenMask, b);
    spv::Id dist = ext1(GLSLstd450Length, f1, v_raw);
    auto bits4 = [&](spv::Id v) {
      return b.createUnaryOp(spv::OpConvertFToU, u1,
                             add(f1, mul(f1, sat(v), k(15.0f)), k(0.5f)));
    };
    spv::Id key = b.createBinOp(
        spv::OpBitwiseOr, u1,
        b.createBinOp(spv::OpBitwiseAnd, u1, b.createUnaryOp(spv::OpBitcast, u1, dist),
                      ku(0xFFFFFF00u)),
        b.createBinOp(spv::OpBitwiseOr, u1,
                      b.createBinOp(spv::OpShiftLeftLogical, u1, bits4(rough), ku(4)),
                      bits4(metal)));
    id_vector_temp_util_.clear();
    id_vector_temp_util_.push_back(const_int_0_);
    id_vector_temp_util_.push_back(b.createUnaryOp(
        spv::OpBitcast, type_int_,
        b.createBinOp(spv::OpIAdd, u1,
                      b.createBinOp(spv::OpIMul, u1, y, ku(pbr::kMaterialPitch)), x)));
    spv::Id pointer = b.createAccessChain(
        features_.spirv_version >= spv::Spv_1_3 ? spv::StorageClassStorageBuffer
                                                : spv::StorageClassUniform,
        buffer_bf_material_, id_vector_temp_util_);
    b.createQuadOp(spv::OpAtomicUMin, u1, pointer, ku(uint32_t(spv::ScopeDevice)), ku(0), key);
    if_inside.makeEndIf();
  }

  spv::Id diffuse = mul(f3, scale(albedo, sub(f1, k(1.0f), metal)),
                        add(f3, scale(sun, n_l), ambient));
  spv::Id color = add(f3, add(f3, diffuse, spec_sun), spec_env);
  if (pbr_->show_fetch != UINT32_MAX && input_fragment_coordinates_ != spv::NoResult) {
    spv::Id d = component(LoadExtraRegister(2), 0);
    color = b.createCompositeConstruct(f3, {sat(mul(f1, sub(f1, k(1.0f), d), k(30.0f))),
                                            sat(mul(f1, sub(f1, k(1.0f), d), k(3.0f))),
                                            b.createBinOp(spv::OpFOrdLessThan, b.makeBoolType(), d, k(0.0001f)) != 0
                                                ? b.createTriOp(spv::OpSelect, f1,
                                                                b.createBinOp(spv::OpFOrdLessThan, b.makeBoolType(), d, k(0.0001f)),
                                                                k(1.0f), k(0.0f))
                                                : k(0.0f)});
  }

  // Into the game's lit colour register.
  id_vector_temp_util_.clear();
  id_vector_temp_util_.push_back(b.makeIntConstant(int(pbr_->out.reg)));
  spv::Id out_pointer = b.createAccessChain(spv::StorageClassFunction,
                                            var_main_registers_, id_vector_temp_util_);
  spv::Id out = b.createLoad(out_pointer, spv::NoPrecision);
  for (uint32_t j = 0; j < std::min(pbr_->out.count, 3u); ++j) {
    out = b.createCompositeInsert(component(color, j), out, type_float4_,
                                  pbr_->out.comps[j]);
  }
  b.createStore(out, out_pointer);
}

}  // namespace gpu
}  // namespace xe

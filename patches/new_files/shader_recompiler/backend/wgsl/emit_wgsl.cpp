// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include "shader_recompiler/backend/wgsl/emit_wgsl.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/reg.h"

namespace Shader::Backend::WGSL {

namespace {

// The program is emitted as `fn shader_main()` over module-scope private variables for the
// stage's inputs/outputs; the stage entry point copies its parameters into those, calls
// shader_main, and returns the outputs. Every IR value is a function-scope `var` declared up
// front (IR values can be used outside the WGSL block that computes them, e.g. after a loop),
// and phis are resolved with copies on the incoming control-flow edges.

const char* TypeName(IR::Type t) {
    switch (t) {
    case IR::Type::U1:
        return "bool";
    case IR::Type::U8:
    case IR::Type::U16:
    case IR::Type::U32:
        return "u32";
    case IR::Type::F16:
    case IR::Type::F32:
    case IR::Type::F64:
        return "f32";
    case IR::Type::U64:
    case IR::Type::U32x2:
        return "vec2<u32>";
    case IR::Type::U32x3:
        return "vec3<u32>";
    case IR::Type::U32x4:
        return "vec4<u32>";
    case IR::Type::F16x2:
    case IR::Type::F32x2:
    case IR::Type::F64x2:
        return "vec2<f32>";
    case IR::Type::F16x3:
    case IR::Type::F32x3:
    case IR::Type::F64x3:
        return "vec3<f32>";
    case IR::Type::F16x4:
    case IR::Type::F32x4:
    case IR::Type::F64x4:
        return "vec4<f32>";
    default:
        return nullptr;
    }
}

std::string FloatLit(float f) {
    if (!std::isfinite(f)) {
        return fmt::format("bitcast<f32>({:#x}u)", std::bit_cast<u32>(f));
    }
    std::string s = fmt::format("{}", f);
    if (s.find_first_of(".en") == std::string::npos) {
        s += ".0";
    }
    // WGSL abstract float literals in f32 context: an explicit `f` keeps them f32 everywhere.
    return s + "f";
}

constexpr const char* kSwizzle = "xyzw";

struct Phi {
    const IR::Inst* phi;
    IR::Value value;
};

class Emitter {
public:
    Emitter(const Profile& profile_, const RuntimeInfo& rt_, const IR::Program& program_)
        : profile{profile_}, rt{rt_}, program{program_}, info{program_.info},
          group{program_.info.sw_stage == SwStage::Fragment ? 1u : 0u} {}

    Output Run();

private:
    // ---- helpers ----
    void Error(std::string msg) {
        if (std::find(out.errors.begin(), out.errors.end(), msg) == out.errors.end()) {
            out.errors.push_back(std::move(msg));
        }
    }
    void Line(std::string_view s) {
        body.append(indent * 4, ' ');
        body.append(s);
        body.push_back('\n');
    }
    template <typename... Args>
    void L(fmt::format_string<Args...> f, Args&&... args) {
        Line(fmt::format(f, std::forward<Args>(args)...));
    }
    void Need(std::string_view helper) {
        helpers_used.insert(std::string(helper));
    }

    std::string Name(const IR::Inst* inst) {
        const auto it = names.find(inst);
        if (it == names.end()) {
            Error(fmt::format("use of a value with no definition ({})", inst->GetOpcode()));
            return "0u";
        }
        return fmt::format("v{}", it->second);
    }

    std::string V(const IR::Value& v) {
        if (!v.IsImmediate()) {
            return Name(v.Inst());
        }
        switch (v.Type()) {
        case IR::Type::U1:
            return v.U1() ? "true" : "false";
        case IR::Type::U8:
            return fmt::format("{}u", v.U8());
        case IR::Type::U16:
            return fmt::format("{}u", v.U16());
        case IR::Type::U32:
            return fmt::format("{}u", v.U32());
        case IR::Type::F32:
        case IR::Type::F64: {
            const float f = v.Type() == IR::Type::F32 ? v.F32() : static_cast<float>(v.F64());
            if (!std::isfinite(f)) {
                Need("f32bits");
                return fmt::format("f32_bits({:#x}u)", std::bit_cast<u32>(f));
            }
            return FloatLit(f);
        }
        case IR::Type::U64:
            return fmt::format("vec2<u32>({}u, {}u)", static_cast<u32>(v.U64()),
                               static_cast<u32>(v.U64() >> 32));
        default:
            Error(fmt::format("immediate of type {}", v.Type()));
            return "0u";
        }
    }
    std::string A(const IR::Inst& inst, size_t i) {
        return V(inst.Arg(i));
    }
    std::string F(const IR::Inst& inst, size_t i) { // argument as f32 bits reinterpreted
        return fmt::format("bitcast<u32>({})", A(inst, i));
    }
    std::string S(const IR::Inst& inst, size_t i) { // argument as i32
        return fmt::format("bitcast<i32>({})", A(inst, i));
    }
    std::string ShiftAmount(const IR::Inst& inst) { // 64-bit shifts may carry a U64 amount
        const auto amount = A(inst, 1);
        return inst.Arg(1).Type() == IR::Type::U64 ? fmt::format("({}).x", amount) : amount;
    }

    void Prepass();
    void EmitDeclarations();
    void EmitBlock(const IR::Block* block);
    void EmitMoves(const IR::Block* from, const IR::Block* to);
    void EmitAllMoves(const IR::Block* from);
    void Traverse();
    void EmitInst(const IR::Inst& inst);
    std::string Assign(const IR::Inst& inst) {
        return Name(&inst);
    }
    void EmitEntryPoint();
    void EmitBarrier(const char* call);
    void CloseIfs(size_t first);
    void ReopenIfs(size_t first);
    size_t FirstIfInLoop() const;
    std::string HelperCode();

    // Resource access.
    std::string Buf(u32 handle) {
        return fmt::format("buf{}", handle);
    }
    bool BufAtomic(u32 handle) const {
        return handle < buffer_atomic.size() && buffer_atomic[handle];
    }
    std::string BufLoad(u32 handle, const std::string& index) {
        return BufAtomic(handle) ? fmt::format("atomicLoad(&{}[{}])", Buf(handle), index)
                                 : fmt::format("{}[{}]", Buf(handle), index);
    }
    void BufStore(u32 handle, const std::string& index, const std::string& value) {
        if (BufAtomic(handle)) {
            L("atomicStore(&{}[{}], {});", Buf(handle), index, value);
        } else {
            L("{}[{}] = {};", Buf(handle), index, value);
        }
    }
    std::string SharedLoad(const std::string& index) {
        return fmt::format("atomicLoad(&shmem[{}])", index);
    }

    struct ImageDesc {
        std::string type;  // WGSL texture type
        std::string scalar; // f32 / u32 / i32
        bool is_integer{};
        bool is_depth{};    // declared as texture_depth_*
        bool is_storage{};
        bool is_array{};
        bool is_3d{};
        bool is_cube{};
        bool is_msaa{};
        bool is_1d{};      // declared as 2D; the guest's coordinates have one spatial component
        int coord_dims{2}; // spatial dimensions
        // The T#'s component selection, applied to sampled results (WebGPU texture views have
        // no swizzle); empty for identity.
        std::string swizzle;
    };
    std::vector<ImageDesc> images;
    std::string ImageName(u32 handle) {
        return fmt::format("tex{}", handle & 0xFFFF);
    }
    std::string SamplerName(u32 handle) {
        return fmt::format("samp{}", handle >> 16);
    }
    std::string SampleCoords(const ImageDesc& img, const std::string& coords, IR::Type type);
    std::string ArrayLayer(const ImageDesc& img, const std::string& coords, IR::Type type);
    std::string ResultF32x4(const ImageDesc& img, const std::string& expr) {
        if (img.is_integer) {
            return fmt::format("bitcast<vec4<f32>>({})", expr);
        }
        return expr;
    }
    // `v` with the image's component selection applied, as a vec4<f32> expression.
    static std::string SwizzleOf(const AmdGpu::CompMapping& map, bool is_integer) {
        const auto comp = [&](AmdGpu::CompSwizzle c) -> std::string {
            switch (c) {
            case AmdGpu::CompSwizzle::Zero:
                return "0.0f";
            case AmdGpu::CompSwizzle::One:
                return is_integer ? "bitcast<f32>(1u)" : "1.0f";
            case AmdGpu::CompSwizzle::Red:
                return "{0}.x";
            case AmdGpu::CompSwizzle::Green:
                return "{0}.y";
            case AmdGpu::CompSwizzle::Blue:
                return "{0}.z";
            default:
                return "{0}.w";
            }
        };
        const std::string s = fmt::format("vec4<f32>({}, {}, {}, {})", comp(map.r), comp(map.g),
                                          comp(map.b), comp(map.a));
        return s == "vec4<f32>({0}.x, {0}.y, {0}.z, {0}.w)" ? std::string{} : s;
    }
    void ApplySwizzle(const ImageDesc& img, const std::string& dst) {
        if (!img.swizzle.empty()) {
            L("{} = {};", dst, fmt::format(fmt::runtime(img.swizzle), dst));
        }
    }
    void EmitImageOp(const IR::Inst& inst);
    void EmitAtomic(const IR::Inst& inst, bool shared, const char* fn, bool signed_op);

    const Profile& profile;
    const RuntimeInfo& rt;
    const IR::Program& program;
    const Info& info;
    const u32 group;
    Output out;

    std::string body;
    int indent = 1;
    std::unordered_map<const IR::Inst*, u32> names;
    std::vector<std::pair<u32, IR::Type>> var_decls;
    std::map<std::pair<const IR::Block*, const IR::Block*>, std::vector<Phi>> moves;
    std::vector<bool> buffer_atomic;
    // Open WGSL constructs, innermost last: an if's condition, or "" for a loop.
    std::vector<std::string> open_constructs;
    // Loops containing a barrier (syntax list indices of their Loop nodes); see Traverse.
    std::set<size_t> barrier_loops;
    struct OpenLoop {
        bool uniform_exit;
        u32 id;
        std::vector<std::string> enclosing_ifs; // closed around the loop, reopened after it
    };
    std::vector<OpenLoop> loops;
    u32 next_loop_id = 0;
    std::set<u32> comparison_samplers;
    std::set<u32> depth_compared_images;
    std::set<u32> storage_read_images; // written images the program also reads
    std::set<std::string> helpers_used;
    u32 flatbuf_index = ~0u;
    bool uses_shared = false;
    bool uses_subgroups = false;
    bool uses_f64 = false;
    std::string decls;
    std::string entry;
};

// ---- prepass ---------------------------------------------------------------------------------

bool IsBufferAtomic(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::BufferAtomicIAdd32:
    case IR::Opcode::BufferAtomicISub32:
    case IR::Opcode::BufferAtomicSMin32:
    case IR::Opcode::BufferAtomicUMin32:
    case IR::Opcode::BufferAtomicFMin32:
    case IR::Opcode::BufferAtomicSMax32:
    case IR::Opcode::BufferAtomicUMax32:
    case IR::Opcode::BufferAtomicFMax32:
    case IR::Opcode::BufferAtomicInc32:
    case IR::Opcode::BufferAtomicDec32:
    case IR::Opcode::BufferAtomicAnd32:
    case IR::Opcode::BufferAtomicOr32:
    case IR::Opcode::BufferAtomicXor32:
    case IR::Opcode::BufferAtomicSwap32:
    case IR::Opcode::BufferAtomicCmpSwap32:
    case IR::Opcode::BufferAtomicFCmpSwap32:
    case IR::Opcode::BufferAtomicIAdd64:
    case IR::Opcode::BufferAtomicSMin64:
    case IR::Opcode::BufferAtomicUMin64:
    case IR::Opcode::BufferAtomicSMax64:
    case IR::Opcode::BufferAtomicUMax64:
        return true;
    default:
        return false;
    }
}

void Emitter::Prepass() {
    buffer_atomic.assign(info.buffers.size(), false);
    u32 next = 0;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            const IR::Opcode op = inst.GetOpcode();
            const IR::Type type = inst.Type();
            if (op == IR::Opcode::Phi) {
                names[&inst] = next;
                var_decls.emplace_back(next++, type);
                for (size_t i = 0; i < inst.NumArgs(); i++) {
                    moves[{inst.PhiBlock(i), block}].push_back({&inst, inst.Arg(i)});
                }
                continue;
            }
            if (type != IR::Type::Void && TypeName(type) != nullptr) {
                names[&inst] = next;
                var_decls.emplace_back(next++, type);
            }
            if (IsBufferAtomic(op) || op == IR::Opcode::DataAppend ||
                op == IR::Opcode::DataConsume) {
                const u32 handle = inst.Arg(0).U32();
                if (handle < buffer_atomic.size()) {
                    buffer_atomic[handle] = true;
                }
            }
            if (op == IR::Opcode::ImageRead) {
                const u32 image = inst.Arg(0).U32() & 0xFFFF;
                if (image < info.images.size() && info.images[image].is_written) {
                    storage_read_images.insert(image);
                }
            }
            switch (op) {
            case IR::Opcode::ImageSampleDrefImplicitLod:
            case IR::Opcode::ImageSampleDrefExplicitLod:
            case IR::Opcode::ImageGatherDref: {
                const u32 handle = inst.Arg(0).U32();
                comparison_samplers.insert(handle >> 16);
                depth_compared_images.insert(handle & 0xFFFF);
                break;
            }
            default:
                break;
            }
        }
    }
    for (u32 i = 0; i < info.buffers.size(); i++) {
        if (info.buffers[i].buffer_type == BufferType::Flatbuf) {
            flatbuf_index = i;
        }
    }
    if (info.sw_stage == SwStage::Compute) {
        std::vector<size_t> loop_stack;
        const auto& list = program.syntax_list;
        for (size_t i = 0; i < list.size(); i++) {
            using T = IR::AbstractSyntaxNode::Type;
            if (list[i].type == T::Loop) {
                loop_stack.push_back(i);
            } else if (list[i].type == T::Repeat) {
                loop_stack.pop_back();
            } else if (list[i].type == T::Block) {
                for (const IR::Inst& inst : list[i].data.block->Instructions()) {
                    const auto op = inst.GetOpcode();
                    if (op == IR::Opcode::Barrier || op == IR::Opcode::WorkgroupMemoryBarrier ||
                        op == IR::Opcode::DeviceMemoryBarrier) {
                        barrier_loops.insert(loop_stack.begin(), loop_stack.end());
                    }
                }
            }
        }
    }
    for (const auto& desc : info.buffers) {
        if (desc.buffer_type == BufferType::SharedMemory) {
            Error("shared memory spilled to a storage buffer");
        }
    }
}

// ---- declarations ----------------------------------------------------------------------------

void Emitter::EmitDeclarations() {
    u32 binding = 0;
    const bool vertex = info.sw_stage == SwStage::Vertex;
    for (u32 i = 0; i < info.buffers.size(); i++) {
        const auto& desc = info.buffers[i];
        // WebGPU forbids writable storage buffers in vertex shaders.
        const bool writable = (desc.is_written || BufAtomic(i)) && !vertex;
        if ((desc.is_written || BufAtomic(i)) && vertex) {
            Error("vertex shader writes a buffer (not allowed in WebGPU)");
        }
        const std::string elem = BufAtomic(i) ? "atomic<u32>" : "u32";
        const std::string type = fmt::format("array<{}>", elem);
        decls += fmt::format("@group({}) @binding({}) var<storage, {}> buf{}: {};\n", group,
                             binding, writable ? "read_write" : "read", i, type);
        out.bindings.push_back({Binding::Kind::Buffer, binding++, i, writable, false, type});
    }
    for (u32 i = 0; i < info.images.size(); i++) {
        const auto& desc = info.images[i];
        const AmdGpu::Image sharp = desc.GetSharp(info);
        ImageDesc img;
        const auto nfmt = sharp.GetNumberFmt();
        img.is_integer = nfmt == AmdGpu::NumberFormat::Uint || nfmt == AmdGpu::NumberFormat::Sint;
        img.scalar = nfmt == AmdGpu::NumberFormat::Uint   ? "u32"
                     : nfmt == AmdGpu::NumberFormat::Sint ? "i32"
                                                          : "f32";
        const auto view = sharp.GetViewType(desc.is_array);
        std::string dim;
        switch (view) {
        case AmdGpu::ImageType::Color1D:
        case AmdGpu::ImageType::Color2D:
            dim = "2d";
            img.is_1d = view == AmdGpu::ImageType::Color1D;
            break;
        case AmdGpu::ImageType::Color1DArray:
        case AmdGpu::ImageType::Color2DArray:
            dim = "2d_array";
            img.is_array = true;
            img.is_1d = view == AmdGpu::ImageType::Color1DArray;
            break;
        case AmdGpu::ImageType::Color3D:
            dim = "3d";
            img.is_3d = true;
            img.coord_dims = 3;
            break;
        case AmdGpu::ImageType::Cube:
            dim = desc.is_array ? "cube_array" : "cube";
            img.is_cube = true;
            img.is_array = desc.is_array;
            img.coord_dims = 3;
            break;
        case AmdGpu::ImageType::Color2DMsaa:
            dim = "multisampled_2d";
            img.is_msaa = true;
            break;
        default:
            dim = "2d";
            Error(fmt::format("image view type {}", magic_enum::enum_name(view)));
            break;
        }
        std::string type;
        if (desc.is_written || desc.is_atomic) {
            img.is_storage = true;
            if (desc.is_atomic) {
                Error("image atomics");
            }
            const auto dfmt = sharp.GetDataFmt();
            std::string texel = StorageTexelFormat(dfmt, nfmt);
            if (texel.empty()) {
                texel = "rgba8unorm";
                Error(fmt::format("storage image format {}/{}", magic_enum::enum_name(dfmt),
                                  magic_enum::enum_name(nfmt)));
            }
            if (img.is_cube) {
                // WGSL storage textures can't be cubes; GCN addresses a cube's faces as layers
                // (x, y, face) anyway.
                img.is_cube = false;
                img.is_array = true;
                img.coord_dims = 2;
                dim = "2d_array";
            } else if (img.is_msaa) {
                Error("storage image of msaa type");
                dim = "2d";
            }
            // Read back as well as written: read_write (the formats tier2 allows).
            const char* access = storage_read_images.contains(i) ? "read_write" : "write";
            type = fmt::format("texture_storage_{}<{}, {}>", dim, texel, access);
            out.bindings.push_back(
                {Binding::Kind::StorageTexture, binding, i, true, false, type});
        } else if (depth_compared_images.contains(i)) {
            img.is_depth = true;
            img.is_integer = false;
            img.scalar = "f32";
            type = img.is_msaa ? "texture_depth_multisampled_2d"
                               : fmt::format("texture_depth_{}", dim);
            out.bindings.push_back({Binding::Kind::Texture, binding, i, false, false, type});
        } else {
            type = fmt::format("texture_{}<{}>", dim, img.scalar);
            out.bindings.push_back({Binding::Kind::Texture, binding, i, false, false, type});
        }
        img.type = type;
        if (!img.is_storage && !img.is_depth) {
            img.swizzle = SwizzleOf(sharp.DstSelect(), img.is_integer);
        }
        decls += fmt::format("@group({}) @binding({}) var tex{}: {};\n", group, binding++, i, type);
        images.push_back(img);
    }
    for (u32 i = 0; i < info.samplers.size(); i++) {
        const bool cmp = comparison_samplers.contains(i);
        const std::string type = cmp ? "sampler_comparison" : "sampler";
        decls += fmt::format("@group({}) @binding({}) var samp{}: {};\n", group, binding, i, type);
        out.bindings.push_back({Binding::Kind::Sampler, binding++, i, false, cmp, type});
    }
    if (info.sw_stage == SwStage::Compute) {
        const u32 words = std::max(1u, (rt.hw.cs.shared_memory_size + 3) / 4);
        decls += fmt::format("var<workgroup> shmem: array<atomic<u32>, {}>;\n", words);
        if (!barrier_loops.empty()) {
            decls += "var<workgroup> loop_live: atomic<u32>;\n"
                     "var<workgroup> loop_vote: u32;\n";
        }
    }
}

// ---- control flow ----------------------------------------------------------------------------

void Emitter::EmitMoves(const IR::Block* from, const IR::Block* to) {
    const auto it = moves.find({from, to});
    if (it == moves.end() || it->second.empty()) {
        return;
    }
    const auto& list = it->second;
    if (list.size() == 1) {
        L("{} = {};", Name(list[0].phi), V(list[0].value));
        return;
    }
    // Parallel copy: read every incoming value before writing any phi.
    L("{{");
    indent++;
    for (size_t i = 0; i < list.size(); i++) {
        L("let t{} = {};", i, V(list[i].value));
    }
    for (size_t i = 0; i < list.size(); i++) {
        L("{} = t{};", Name(list[i].phi), i);
    }
    indent--;
    L("}}");
}

void Emitter::EmitAllMoves(const IR::Block* from) {
    for (const IR::Block* succ : from->ImmSuccessors()) {
        EmitMoves(from, succ);
    }
}

void Emitter::EmitBlock(const IR::Block* block) {
    for (const IR::Inst& inst : block->Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::Phi) {
            continue;
        }
        EmitInst(inst);
    }
}

void Emitter::Traverse() {
    const auto& list = program.syntax_list;
    const IR::Block* current = nullptr;
    for (size_t i = 0; i < list.size(); i++) {
        const auto& node = list[i];
        using T = IR::AbstractSyntaxNode::Type;
        switch (node.type) {
        case T::Block: {
            current = node.data.block;
            EmitBlock(current);
            // A block whose terminator is conditional gets its edge copies at that node.
            const bool conditional = i + 1 < list.size() &&
                                     (list[i + 1].type == T::If || list[i + 1].type == T::Break ||
                                      list[i + 1].type == T::Repeat);
            if (!conditional) {
                EmitAllMoves(current);
            }
            break;
        }
        case T::If: {
            const std::string cond = V(node.data.if_node.cond);
            if (current && moves.contains({current, node.data.if_node.merge})) {
                L("if (!({})) {{", cond);
                indent++;
                EmitMoves(current, node.data.if_node.merge);
                indent--;
                L("}}");
            }
            L("if ({}) {{", cond);
            indent++;
            open_constructs.push_back(cond);
            if (current) {
                EmitMoves(current, node.data.if_node.body);
            }
            break;
        }
        case T::EndIf:
            indent--;
            L("}}");
            open_constructs.pop_back();
            break;
        case T::Loop: {
            // GCN runs a loop until no lane of the wave is left in it, and a barrier inside makes
            // every wave of the workgroup iterate in lockstep. WGSL wants barriers in uniform
            // control flow, so such a loop keeps all invocations iterating - finished ones with
            // their work predicated off by loop_doneN - and exits on a workgroup-wide vote.
            // Invocations outside the ifs around the loop join it already done.
            OpenLoop loop{barrier_loops.contains(i), next_loop_id++, {}};
            if (loop.uniform_exit) {
                const size_t first = FirstIfInLoop();
                loop.enclosing_ifs.assign(open_constructs.begin() + first, open_constructs.end());
                CloseIfs(first);
                open_constructs.resize(first);
                std::string active;
                for (const auto& cond : loop.enclosing_ifs) {
                    active += fmt::format("{}({})", active.empty() ? "" : " && ", cond);
                }
                L("var loop_done{}: bool = {};", loop.id,
                  active.empty() ? "false" : fmt::format("!({})", active));
            }
            L("loop {{");
            indent++;
            open_constructs.push_back("");
            if (loop.uniform_exit) {
                const std::string guard = fmt::format("!loop_done{}", loop.id);
                L("if ({}) {{", guard);
                indent++;
                open_constructs.push_back(guard);
            }
            loops.push_back(std::move(loop));
            break;
        }
        case T::Break: {
            const bool uniform_exit = !loops.empty() && loops.back().uniform_exit;
            L("if ({}) {{", V(node.data.break_node.cond));
            indent++;
            if (current) {
                EmitMoves(current, node.data.break_node.merge);
            }
            if (uniform_exit) {
                L("loop_done{} = true;", loops.back().id);
            } else {
                L("break;");
            }
            indent--;
            L("}}");
            if (uniform_exit) {
                // Re-test the done flag for the rest of the iteration.
                const size_t first = FirstIfInLoop();
                CloseIfs(first);
                ReopenIfs(first);
            }
            if (current) {
                EmitMoves(current, node.data.break_node.skip);
            }
            break;
        }
        case T::Repeat: {
            L("if ({}) {{", V(node.data.repeat.cond));
            indent++;
            if (current) {
                EmitMoves(current, node.data.repeat.loop_header);
            }
            indent--;
            L("}} else {{");
            indent++;
            if (current) {
                EmitMoves(current, node.data.repeat.merge);
            }
            OpenLoop loop = std::move(loops.back());
            loops.pop_back();
            if (loop.uniform_exit) {
                L("loop_done{} = true;", loop.id);
            } else {
                L("break;");
            }
            indent--;
            L("}}");
            if (loop.uniform_exit) {
                indent--;
                L("}}");
                open_constructs.pop_back();
                L("if (!loop_done{}) {{ atomicStore(&loop_live, 1u); }}", loop.id);
                L("workgroupBarrier();");
                L("if (in_local_invocation_index == 0u) {{ loop_vote = "
                  "atomicExchange(&loop_live, 0u); }}");
                L("if (workgroupUniformLoad(&loop_vote) == 0u) {{ break; }}");
            }
            indent--;
            L("}}");
            open_constructs.pop_back();
            for (const auto& cond : loop.enclosing_ifs) {
                L("if ({}) {{", cond);
                indent++;
                open_constructs.push_back(cond);
            }
            break;
        }
        case T::Return:
            if (std::ranges::any_of(loops, [](const OpenLoop& l) { return l.uniform_exit; })) {
                Error("return inside a loop containing a barrier");
            }
            L("return;");
            break;
        case T::Unreachable:
            L("return;");
            break;
        }
        if (node.type != T::Block) {
            current = nullptr;
        }
    }
}

size_t Emitter::FirstIfInLoop() const {
    size_t first = open_constructs.size();
    while (first > 0 && !open_constructs[first - 1].empty()) {
        first--;
    }
    return first;
}

void Emitter::CloseIfs(size_t first) {
    for (size_t i = open_constructs.size(); i > first; i--) {
        indent--;
        L("}}");
    }
}

void Emitter::ReopenIfs(size_t first) {
    for (size_t i = first; i < open_constructs.size(); i++) {
        L("if ({}) {{", open_constructs[i]);
        indent++;
    }
}

void Emitter::EmitBarrier(const char* call) {
    // GCN barriers are per wave, whatever the exec mask: close the ifs around the barrier (up to
    // the innermost loop, which Traverse made uniform) and reopen them after it.
    const size_t first = FirstIfInLoop();
    CloseIfs(first);
    Line(call);
    ReopenIfs(first);
}

// ---- instructions ----------------------------------------------------------------------------

static int NumComponents(IR::Type type) {
    switch (type) {
    case IR::Type::F32:
    case IR::Type::U32:
        return 1;
    case IR::Type::F32x2:
    case IR::Type::U32x2:
        return 2;
    case IR::Type::F32x3:
    case IR::Type::U32x3:
        return 3;
    default:
        return 4;
    }
}

std::string Emitter::SampleCoords(const ImageDesc& img, const std::string& c, IR::Type type) {
    // Coordinates arrive as F32 scalars/vectors: the spatial ones, then an array's layer. 1D
    // images are 2D textures one texel high here.
    const int n = NumComponents(type);
    if (n == 1 || img.is_1d) {
        return fmt::format("vec2<f32>({}, 0.0f)", n == 1 ? c : fmt::format("({}).x", c));
    }
    if (img.coord_dims == 3) {
        return n >= 3 ? fmt::format("({}).xyz", c) : fmt::format("vec3<f32>({}, 0.0f)", c);
    }
    return n == 2 ? c : fmt::format("({}).xy", c);
}

std::string Emitter::ArrayLayer(const ImageDesc& img, const std::string& c, IR::Type type) {
    // The component after the spatial ones; layer 0 if the coordinates don't have one.
    const int spatial = img.is_1d ? 1 : img.coord_dims;
    if (NumComponents(type) <= spatial) {
        return "0";
    }
    return fmt::format("i32(round(({}).{}))", c, kSwizzle[spatial]);
}

void Emitter::EmitImageOp(const IR::Inst& inst) {
    const IR::Opcode op = inst.GetOpcode();
    const u32 handle = inst.Arg(0).U32();
    const u32 index = handle & 0xFFFF;
    if (index >= images.size()) {
        Error(fmt::format("{} with image handle {} out of range", op, index));
        return;
    }
    const ImageDesc& img = images[index];
    const auto flags = inst.Flags<IR::TextureInstInfo>();
    const bool fragment = info.sw_stage == SwStage::Fragment;
    const std::string t = ImageName(handle);
    const std::string s = SamplerName(handle);
    const std::string dst = inst.Type() != IR::Type::Void ? Assign(inst) : std::string{};
    const auto coords_type = inst.NumArgs() > 1 ? inst.Arg(1).Type() : IR::Type::Void;
    const std::string c = inst.NumArgs() > 1 ? A(inst, 1) : "";

    if (flags.has_offset && op != IR::Opcode::ImageRead && op != IR::Opcode::ImageWrite) {
        // WGSL needs texel offsets as constant expressions; GCN supplies them in a register.
        Error("texture sample with texel offset");
    }
    if ((op == IR::Opcode::ImageSampleImplicitLod || op == IR::Opcode::ImageSampleExplicitLod ||
         op == IR::Opcode::ImageGradient || op == IR::Opcode::ImageGather) &&
        img.is_integer) {
        // Integer textures can't be filtered or sampled in WGSL: load the nearest texel instead
        // (GCN samples them with point filtering anyway).
        const std::string coords = SampleCoords(img, c, coords_type);
        std::string ic = img.coord_dims == 3
                             ? fmt::format("vec3<i32>(floor({} * vec3<f32>(textureDimensions({}))))",
                                           coords, t)
                             : fmt::format("vec2<i32>(floor({} * vec2<f32>(textureDimensions({}))))",
                                           coords, t);
        const std::string layer = img.is_array ? ", " + ArrayLayer(img, c, coords_type) : "";
        L("{} = {};", dst,
          ResultF32x4(img, fmt::format("textureLoad({}, {}{}, 0)", t, ic, layer)));
        ApplySwizzle(img, dst);
        return;
    }
    if (img.is_storage && op != IR::Opcode::ImageWrite && op != IR::Opcode::ImageQueryDimensions &&
        op != IR::Opcode::ImageRead) {
        Error(fmt::format("{} on a written image", op));
        return;
    }
    const std::string layer = img.is_array ? ", " + ArrayLayer(img, c, coords_type) : "";
    const std::string coords = c.empty() ? "" : SampleCoords(img, c, coords_type);

    switch (op) {
    case IR::Opcode::ImageSampleImplicitLod: {
        if (!fragment) {
            L("{} = textureSampleLevel({}, {}, {}{}, 0.0f);", dst, t, s, coords, layer);
        } else if (flags.has_bias) {
            L("{} = textureSampleBias({}, {}, {}{}, {});", dst, t, s, coords, layer, A(inst, 2));
        } else {
            L("{} = textureSample({}, {}, {}{});", dst, t, s, coords, layer);
        }
        ApplySwizzle(img, dst);
        return;
    }
    case IR::Opcode::ImageSampleExplicitLod:
        L("{} = textureSampleLevel({}, {}, {}{}, {});", dst, t, s, coords, layer, A(inst, 2));
        ApplySwizzle(img, dst);
        return;
    case IR::Opcode::ImageSampleDrefImplicitLod:
        if (fragment) {
            L("{} = vec4<f32>(textureSampleCompare({}, {}, {}{}, {}), 0.0f, 0.0f, 0.0f);", dst,
              t, s, coords, layer, A(inst, 2));
        } else {
            L("{} = vec4<f32>(textureSampleCompareLevel({}, {}, {}{}, {}), 0.0f, 0.0f, 0.0f);",
              dst, t, s, coords, layer, A(inst, 2));
        }
        return;
    case IR::Opcode::ImageSampleDrefExplicitLod:
        // WGSL only offers comparison sampling at level 0 outside implicit-LOD contexts.
        L("{} = vec4<f32>(textureSampleCompareLevel({}, {}, {}{}, {}), 0.0f, 0.0f, 0.0f);", dst,
          t, s, coords, layer, A(inst, 2));
        return;
    case IR::Opcode::ImageGather:
        if (img.is_depth) {
            L("{} = textureGather({}, {}, {}{});", dst, t, s, coords, layer);
        } else {
            // The component is picked after the T#'s selection: gather what it selects.
            u32 comp = flags.gather_comp.Value();
            std::string constant;
            if (!img.swizzle.empty()) {
                const std::string sel = fmt::format(fmt::runtime(img.swizzle), "v");
                // "vec4<f32>(a, b, c, d)" -> the comp'th argument
                std::vector<std::string> args;
                std::string arg;
                for (char ch : sel.substr(10, sel.size() - 11)) {
                    if (ch == ',') {
                        args.push_back(arg);
                        arg.clear();
                    } else if (ch != ' ') {
                        arg.push_back(ch);
                    }
                }
                args.push_back(arg);
                const std::string& pick = args[comp & 3];
                if (pick.size() == 3 && pick[0] == 'v') {
                    comp = std::string_view("xyzw").find(pick[2]);
                } else {
                    constant = pick;
                }
            }
            if (!constant.empty()) {
                L("{} = vec4<f32>({});", dst, constant);
            } else {
                L("{} = textureGather({}, {}, {}, {}{});", dst, comp, t, s, coords, layer);
            }
        }
        return;
    case IR::Opcode::ImageGatherDref:
        L("{} = textureGatherCompare({}, {}, {}{}, {});", dst, t, s, coords, layer, A(inst, 3));
        return;
    case IR::Opcode::ImageGradient:
        L("{} = textureSampleGrad({}, {}, {}{}, {}, {});", dst, t, s, coords, layer,
          SampleCoords(img, A(inst, 2), inst.Arg(2).Type()),
          SampleCoords(img, A(inst, 3), inst.Arg(3).Type()));
        ApplySwizzle(img, dst);
        return;
    case IR::Opcode::ImageQueryDimensions: {
        if (img.is_storage) {
            if (img.coord_dims == 3) {
                L("{} = vec4<u32>(textureDimensions({}), 1u);", dst, t);
            } else if (img.is_array) {
                L("{} = vec4<u32>(textureDimensions({}), textureNumLayers({}), 1u);", dst, t, t);
            } else {
                L("{} = vec4<u32>(textureDimensions({}), 1u, 1u);", dst, t);
            }
            return;
        }
        if (img.is_msaa) {
            L("{} = vec4<u32>(textureDimensions({}), 1u, 1u);", dst, t);
            return;
        }
        const std::string lod = A(inst, 1);
        if (img.coord_dims == 3 && !img.is_cube) {
            L("{} = vec4<u32>(textureDimensions({}, {}), textureNumLevels({}));", dst, t, lod, t);
        } else if (img.is_array) {
            L("{} = vec4<u32>(textureDimensions({}, {}).xy, textureNumLayers({}), "
              "textureNumLevels({}));",
              dst, t, lod, t, t);
        } else {
            L("{} = vec4<u32>(textureDimensions({}, {}).xy, 1u, textureNumLevels({}));", dst, t,
              lod, t);
        }
        return;
    }
    case IR::Opcode::ImageRead: {
        // Integer texel coordinates; array layer last.
        const auto ctype = inst.Arg(1).Type();
        const int n = ctype == IR::Type::U32 ? 1 : ctype == IR::Type::U32x2 ? 2
                                                 : ctype == IR::Type::U32x3 ? 3
                                                                            : 4;
        std::string ic;
        std::string lay;
        if (n == 1) {
            ic = fmt::format("vec2<i32>(i32({}), 0)", c);
        } else if (img.is_1d) {
            ic = fmt::format("vec2<i32>(i32(({}).x), 0)", c);
            lay = img.is_array ? fmt::format(", i32(({}).y)", c) : "";
        } else if (img.coord_dims == 3) {
            ic = fmt::format("vec3<i32>(({}).xyz)", c);
        } else {
            ic = fmt::format("vec2<i32>(({}).xy)", c);
            if (img.is_array && n >= 3) {
                lay = fmt::format(", i32(({}).z)", c);
            }
        }
        if (img.is_array && lay.empty()) {
            lay = ", 0";
        }
        if (img.is_storage) {
            // Storage textures have one mip level and no level argument.
            L("{} = {};", dst, ResultF32x4(img, fmt::format("textureLoad({}, {}{})", t, ic, lay)));
            return;
        }
        const std::string lod = img.is_msaa ? A(inst, 3) : A(inst, 2);
        if (img.is_depth) {
            L("{} = vec4<f32>(textureLoad({}, {}{}, i32({})), 0.0f, 0.0f, 1.0f);", dst, t, ic,
              lay, lod);
        } else {
            L("{} = {};", dst,
              ResultF32x4(img, fmt::format("textureLoad({}, {}{}, i32({}))", t, ic, lay, lod)));
            ApplySwizzle(img, dst);
        }
        return;
    }
    case IR::Opcode::ImageWrite: {
        const auto ctype = inst.Arg(1).Type();
        const int n = ctype == IR::Type::U32 ? 1 : ctype == IR::Type::U32x2 ? 2
                                                 : ctype == IR::Type::U32x3 ? 3
                                                                            : 4;
        std::string ic;
        std::string lay;
        if (n == 1) {
            ic = fmt::format("vec2<i32>(i32({}), 0)", c);
        } else if (img.is_1d) {
            ic = fmt::format("vec2<i32>(i32(({}).x), 0)", c);
            lay = img.is_array ? fmt::format(", i32(({}).y)", c) : "";
        } else if (img.coord_dims == 3) {
            ic = fmt::format("vec3<i32>(({}).xyz)", c);
        } else {
            ic = fmt::format("vec2<i32>(({}).xy)", c);
            if (img.is_array && n >= 3) {
                lay = fmt::format(", i32(({}).z)", c);
            }
        }
        if (img.is_array && lay.empty()) {
            lay = ", 0";
        }
        const std::string color = A(inst, 4);
        const std::string value = img.is_integer
                                      ? fmt::format("bitcast<vec4<{}>>({})", img.scalar, color)
                                      : color;
        L("textureStore({}, {}{}, {});", t, ic, lay, value);
        return;
    }
    case IR::Opcode::ImageQueryLod:
        Error("ImageQueryLod (no WGSL equivalent)");
        L("{} = vec4<f32>(0.0f);", dst);
        return;
    default:
        Error(fmt::format("image op {}", op));
        return;
    }
}

void Emitter::EmitAtomic(const IR::Inst& inst, bool shared, const char* fn, bool signed_op) {
    // Buffer atomics: (handle, index, value); shared: (byte offset, value).
    std::string ptr;
    size_t value_arg;
    if (shared) {
        ptr = fmt::format("&shmem[({}) >> 2u]", A(inst, 0));
        value_arg = 1;
    } else {
        const u32 handle = inst.Arg(0).U32();
        ptr = fmt::format("&{}[{}]", Buf(handle), A(inst, 1));
        value_arg = 2;
    }
    std::string value = A(inst, value_arg);
    if (signed_op) {
        Need("atomic_signed");
        L("{} = {}_signed{}({}, {});", Name(&inst), fn, shared ? "_w" : "", ptr, value);
        return;
    }
    L("{} = {}({}, {});", Name(&inst), fn, ptr, value);
}

void Emitter::EmitInst(const IR::Inst& inst) {
    using Op = IR::Opcode;
    const Op op = inst.GetOpcode();
    const auto set = [&](const std::string& expr) { L("{} = {};", Name(&inst), expr); };
    const auto un = [&](const char* fn) { set(fmt::format("{}({})", fn, A(inst, 0))); };
    const auto bin = [&](const char* o) { set(fmt::format("({} {} {})", A(inst, 0), o, A(inst, 1))); };
    const auto call2 = [&](const char* fn) {
        set(fmt::format("{}({}, {})", fn, A(inst, 0), A(inst, 1)));
    };
    const auto scmp = [&](const char* o) {
        set(fmt::format("({} {} {})", S(inst, 0), o, S(inst, 1)));
    };
    const auto ucmp = [&](const char* o) {
        set(fmt::format("({} {} {})", A(inst, 0), o, A(inst, 1)));
    };
    const auto fcmp_ord = [&](const char* o) {
        set(fmt::format("({} {} {})", A(inst, 0), o, A(inst, 1)));
    };
    const auto fcmp_unord = [&](const char* o) {
        Need("isnan");
        set(fmt::format("(({} {} {}) || is_nan_f({}) || is_nan_f({}))", A(inst, 0), o, A(inst, 1),
                        A(inst, 0), A(inst, 1)));
    };
    const auto composite = [&](const char* type) {
        std::string args;
        for (size_t i = 0; i < inst.NumArgs(); i++) {
            args += (i ? ", " : "") + A(inst, i);
        }
        set(fmt::format("{}({})", type, args));
    };

    switch (op) {
    // ---- structural ----
    case Op::Phi:
    case Op::Void:
    case Op::Prologue:
    case Op::Epilogue:
    case Op::DebugPrint:
        return;
    case Op::ConditionRef:
        set(A(inst, 0));
        return;
    case Op::Discard:
        out.uses_discard = true;
        L("discard;");
        return;
    case Op::DiscardCond:
        out.uses_discard = true;
        L("if ({}) {{ discard; }}", A(inst, 0));
        return;
    case Op::Barrier:
    case Op::WorkgroupMemoryBarrier:
    case Op::DeviceMemoryBarrier:
        if (info.sw_stage == SwStage::Compute) {
            EmitBarrier(op == Op::DeviceMemoryBarrier ? "storageBarrier();" : "workgroupBarrier();");
        }
        return;
    case Op::SubgroupBarrier:
        return;

    // ---- constants / user data ----
    case Op::GetUserData:
    case Op::ReadConst: {
        // User data registers and constants the frontend resolved through the SRT both live in
        // the flattened user-data buffer: ReadConst carries its dword offset in its flags.
        const u32 off = op == Op::GetUserData ? static_cast<u32>(inst.Arg(0).ScalarReg())
                                              : inst.Flags<u32>();
        if (op == Op::ReadConst && off == 0) {
            // Not resolved through the SRT: a read of arbitrary guest memory (DMA), which needs
            // the page-table buffer the SPIR-V backend uses.
            Error("dynamic ReadConst (guest memory read through a pointer)");
            set("0u");
        } else if (flatbuf_index == ~0u) {
            Error("user data read without a flatbuf buffer");
            set("0u");
        } else {
            set(BufLoad(flatbuf_index, fmt::format("{}u", off)));
        }
        return;
    }
    case Op::ReadConstBuffer:
        set(BufLoad(inst.Arg(0).U32(), A(inst, 1)));
        return;

    // ---- attributes ----
    case Op::GetAttribute: {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        const u32 comp = inst.Arg(1).U32();
        if (IR::IsParam(attr)) {
            const u32 idx = u32(attr) - u32(IR::Attribute::Param0);
            set(fmt::format("in_param{}.{}", idx, kSwizzle[comp & 3]));
            return;
        }
        switch (attr) {
        case IR::Attribute::FragCoord:
            set(fmt::format("in_frag_coord.{}", kSwizzle[comp & 3]));
            return;
        default:
            Error(fmt::format("read attribute {}", magic_enum::enum_name(attr)));
            set("0.0f");
            return;
        }
    }
    case Op::GetAttributeU1: {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        if (attr == IR::Attribute::IsFrontFace) {
            set("in_front_facing");
        } else {
            Error(fmt::format("read U1 attribute {}", magic_enum::enum_name(attr)));
            set("false");
        }
        return;
    }
    case Op::GetAttributeU32: {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        const u32 comp = inst.Arg(1).U32();
        switch (attr) {
        case IR::Attribute::VertexId:
            set("in_vertex_index");
            return;
        case IR::Attribute::InstanceId:
            set("in_instance_index");
            return;
        case IR::Attribute::BaseVertex:
        case IR::Attribute::BaseInstance:
            set("0u"); // WGSL has no base vertex/instance builtins; draws pass them pre-offset
            return;
        case IR::Attribute::WorkgroupId:
            set(fmt::format("in_workgroup_id.{}", kSwizzle[comp & 3]));
            return;
        case IR::Attribute::LocalInvocationId:
            set(fmt::format("in_local_invocation_id.{}", kSwizzle[comp & 3]));
            return;
        case IR::Attribute::LocalInvocationIndex:
            set("in_local_invocation_index");
            return;
        case IR::Attribute::WorkgroupIndex:
            set("(in_workgroup_id.x + in_num_workgroups.x * (in_workgroup_id.y + "
                "in_num_workgroups.y * in_workgroup_id.z))");
            return;
        case IR::Attribute::SampleIndex:
            set("in_sample_index");
            return;
        case IR::Attribute::PrimitiveId:
            set("0u");
            Error("PrimitiveId (no WGSL builtin)");
            return;
        default:
            Error(fmt::format("read U32 attribute {}", magic_enum::enum_name(attr)));
            set("0u");
            return;
        }
    }
    case Op::SetAttribute: {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        const std::string value = A(inst, 1);
        const u32 comp = inst.Arg(2).U32();
        if (IR::IsParam(attr)) {
            const u32 idx = u32(attr) - u32(IR::Attribute::Param0);
            L("out_param{}.{} = {};", idx, kSwizzle[comp & 3], value);
            return;
        }
        if (IR::IsMrt(attr)) {
            const u32 idx = u32(attr) - u32(IR::Attribute::RenderTarget0);
            const auto nfmt = rt.hw.fs.color_buffers[idx].num_format;
            std::string v = value;
            if (nfmt == AmdGpu::NumberFormat::Uint) {
                v = fmt::format("bitcast<u32>({})", value);
            } else if (nfmt == AmdGpu::NumberFormat::Sint) {
                v = fmt::format("bitcast<i32>({})", value);
            } else if (comp < 3 && rt.hw.fs.color_buffers[idx].blend_self_scale) {
                v = fmt::format("({} * {})", value, value);
            }
            L("out_color{}.{} = {};", idx, kSwizzle[comp & 3], v);
            return;
        }
        switch (attr) {
        case IR::Attribute::Position0:
            L("out_position.{} = {};", kSwizzle[comp & 3], value);
            return;
        case IR::Attribute::Depth:
            L("out_depth = {};", value);
            return;
        case IR::Attribute::PointSize:
        case IR::Attribute::ClipDistance:
        case IR::Attribute::CullDistance:
        case IR::Attribute::ViewportIndex:
        case IR::Attribute::RenderTargetIndex:
            // No WGSL outputs for these; the draw renders as if they were defaults.
            return;
        default:
            Error(fmt::format("write attribute {}", magic_enum::enum_name(attr)));
            return;
        }
    }

    // ---- undefined ----
    case Op::UndefU1:
        set("false");
        return;
    case Op::UndefU8:
    case Op::UndefU16:
    case Op::UndefU32:
        set("0u");
        return;
    case Op::UndefU32x2:
    case Op::UndefU64:
        set("vec2<u32>()");
        return;
    case Op::UndefU32x3:
        set("vec3<u32>()");
        return;
    case Op::UndefU32x4:
        set("vec4<u32>()");
        return;
    case Op::UndefF32:
        set("0.0f");
        return;

    // ---- buffers ----
    case Op::LoadBufferU8: {
        const u32 h = inst.Arg(0).U32();
        L("{{ let a = {}; {} = ({} >> ((a & 3u) * 8u)) & 0xffu; }}", A(inst, 1), Name(&inst),
          BufLoad(h, "a >> 2u"));
        return;
    }
    case Op::LoadBufferU16: {
        const u32 h = inst.Arg(0).U32();
        L("{{ let a = {}; {} = ({} >> ((a & 1u) * 16u)) & 0xffffu; }}", A(inst, 1), Name(&inst),
          BufLoad(h, "a >> 1u"));
        return;
    }
    case Op::LoadBufferU32:
        set(BufLoad(inst.Arg(0).U32(), A(inst, 1)));
        return;
    case Op::LoadBufferF32:
        set(fmt::format("bitcast<f32>({})", BufLoad(inst.Arg(0).U32(), A(inst, 1))));
        return;
    case Op::LoadBufferU32x2:
    case Op::LoadBufferU32x3:
    case Op::LoadBufferU32x4:
    case Op::LoadBufferF32x2:
    case Op::LoadBufferF32x3:
    case Op::LoadBufferF32x4:
    case Op::LoadBufferU64: {
        const u32 h = inst.Arg(0).U32();
        const bool f = op == Op::LoadBufferF32x2 || op == Op::LoadBufferF32x3 ||
                       op == Op::LoadBufferF32x4;
        const int n = (op == Op::LoadBufferU32x2 || op == Op::LoadBufferF32x2 ||
                       op == Op::LoadBufferU64)
                          ? 2
                      : (op == Op::LoadBufferU32x3 || op == Op::LoadBufferF32x3) ? 3
                                                                                 : 4;
        std::string parts;
        for (int i = 0; i < n; i++) {
            parts += (i ? ", " : "") + BufLoad(h, i ? fmt::format("a + {}u", i) : "a");
        }
        const std::string index = op == Op::LoadBufferU64 ? fmt::format("({}) * 2u", A(inst, 1))
                                                          : A(inst, 1);
        const std::string vec = fmt::format("vec{}<u32>({})", n, parts);
        L("{{ let a = {}; {} = {}; }}", index, Name(&inst),
          f ? fmt::format("bitcast<vec{}<f32>>({})", n, vec) : vec);
        return;
    }
    case Op::StoreBufferU8:
    case Op::StoreBufferU16: {
        const u32 h = inst.Arg(0).U32();
        const bool b8 = op == Op::StoreBufferU8;
        const std::string word = b8 ? "a >> 2u" : "a >> 1u";
        const std::string shift = b8 ? "(a & 3u) * 8u" : "(a & 1u) * 16u";
        const std::string mask = b8 ? "0xffu" : "0xffffu";
        if (BufAtomic(h)) {
            L("{{ let a = {}; let sh = {}; atomicAnd(&{}[{}], ~({} << sh)); atomicOr(&{}[{}], "
              "(({}) & {}) << sh); }}",
              A(inst, 1), shift, Buf(h), word, mask, Buf(h), word, A(inst, 2), mask);
        } else {
            L("{{ let a = {}; let sh = {}; {}[{}] = ({}[{}] & ~({} << sh)) | ((({}) & {}) << sh); "
              "}}",
              A(inst, 1), shift, Buf(h), word, Buf(h), word, mask, A(inst, 2), mask);
        }
        return;
    }
    case Op::StoreBufferU32:
        BufStore(inst.Arg(0).U32(), A(inst, 1), A(inst, 2));
        return;
    case Op::StoreBufferF32:
        BufStore(inst.Arg(0).U32(), A(inst, 1), fmt::format("bitcast<u32>({})", A(inst, 2)));
        return;
    case Op::StoreBufferU32x2:
    case Op::StoreBufferU32x3:
    case Op::StoreBufferU32x4:
    case Op::StoreBufferF32x2:
    case Op::StoreBufferF32x3:
    case Op::StoreBufferF32x4:
    case Op::StoreBufferU64: {
        const u32 h = inst.Arg(0).U32();
        const bool f = op == Op::StoreBufferF32x2 || op == Op::StoreBufferF32x3 ||
                       op == Op::StoreBufferF32x4;
        const int n = (op == Op::StoreBufferU32x2 || op == Op::StoreBufferF32x2 ||
                       op == Op::StoreBufferU64)
                          ? 2
                      : (op == Op::StoreBufferU32x3 || op == Op::StoreBufferF32x3) ? 3
                                                                                   : 4;
        const std::string index = op == Op::StoreBufferU64 ? fmt::format("({}) * 2u", A(inst, 1))
                                                           : A(inst, 1);
        L("{{");
        indent++;
        L("let a = {};", index);
        L("let v = {};", f ? fmt::format("bitcast<vec{}<u32>>({})", n, A(inst, 2)) : A(inst, 2));
        for (int i = 0; i < n; i++) {
            BufStore(h, i ? fmt::format("a + {}u", i) : "a", fmt::format("v.{}", kSwizzle[i]));
        }
        indent--;
        L("}}");
        return;
    }
    case Op::LoadBufferFormatF32:
    case Op::StoreBufferFormatF32:
        Error("formatted buffer access (should have been lowered)");
        return;

    // ---- buffer / shared atomics ----
    case Op::BufferAtomicIAdd32:
        EmitAtomic(inst, false, "atomicAdd", false);
        return;
    case Op::BufferAtomicISub32:
        EmitAtomic(inst, false, "atomicSub", false);
        return;
    case Op::BufferAtomicUMin32:
        EmitAtomic(inst, false, "atomicMin", false);
        return;
    case Op::BufferAtomicUMax32:
        EmitAtomic(inst, false, "atomicMax", false);
        return;
    case Op::BufferAtomicSMin32:
        EmitAtomic(inst, false, "atomicMin", true);
        return;
    case Op::BufferAtomicSMax32:
        EmitAtomic(inst, false, "atomicMax", true);
        return;
    case Op::BufferAtomicAnd32:
        EmitAtomic(inst, false, "atomicAnd", false);
        return;
    case Op::BufferAtomicOr32:
        EmitAtomic(inst, false, "atomicOr", false);
        return;
    case Op::BufferAtomicXor32:
        EmitAtomic(inst, false, "atomicXor", false);
        return;
    case Op::BufferAtomicSwap32:
        EmitAtomic(inst, false, "atomicExchange", false);
        return;
    case Op::BufferAtomicCmpSwap32:
        // (handle, index, value, cmp): returns the old value.
        L("{} = atomicCompareExchangeWeak(&{}[{}], {}, {}).old_value;", Name(&inst),
          Buf(inst.Arg(0).U32()), A(inst, 1), A(inst, 3), A(inst, 2));
        return;
    case Op::BufferAtomicInc32:
    case Op::BufferAtomicDec32: {
        Need("atomic_incdec");
        const bool inc = op == Op::BufferAtomicInc32;
        L("{} = atomic_{}(&{}[{}]);", Name(&inst), inc ? "inc_wrap" : "dec_wrap",
          Buf(inst.Arg(0).U32()), A(inst, 1));
        return;
    }
    case Op::SharedAtomicIAdd32:
        EmitAtomic(inst, true, "atomicAdd", false);
        return;
    case Op::SharedAtomicISub32:
        EmitAtomic(inst, true, "atomicSub", false);
        return;
    case Op::SharedAtomicUMin32:
        EmitAtomic(inst, true, "atomicMin", false);
        return;
    case Op::SharedAtomicUMax32:
        EmitAtomic(inst, true, "atomicMax", false);
        return;
    case Op::SharedAtomicSMin32:
        EmitAtomic(inst, true, "atomicMin", true);
        return;
    case Op::SharedAtomicSMax32:
        EmitAtomic(inst, true, "atomicMax", true);
        return;
    case Op::SharedAtomicAnd32:
        EmitAtomic(inst, true, "atomicAnd", false);
        return;
    case Op::SharedAtomicOr32:
        EmitAtomic(inst, true, "atomicOr", false);
        return;
    case Op::SharedAtomicXor32:
        EmitAtomic(inst, true, "atomicXor", false);
        return;
    case Op::SharedAtomicInc32:
    case Op::SharedAtomicDec32:
        Need("atomic_incdec_shared");
        L("{} = atomic_{}_shared(&shmem[({}) >> 2u]);", Name(&inst),
          op == Op::SharedAtomicInc32 ? "inc_wrap" : "dec_wrap", A(inst, 0));
        return;
    case Op::SharedAtomicCmpSwap32:
        L("{} = atomicCompareExchangeWeak(&shmem[({}) >> 2u], {}, {}).old_value;", Name(&inst),
          A(inst, 0), A(inst, 2), A(inst, 1));
        return;
    case Op::LoadSharedU32:
        set(SharedLoad(fmt::format("({}) >> 2u", A(inst, 0))));
        return;
    case Op::WriteSharedU32:
        L("atomicStore(&shmem[({}) >> 2u], {});", A(inst, 0), A(inst, 1));
        return;
    case Op::LoadSharedU16:
        L("{{ let a = {}; {} = ({} >> ((a & 2u) * 8u)) & 0xffffu; }}", A(inst, 0), Name(&inst),
          SharedLoad("a >> 2u"));
        return;
    case Op::WriteSharedU16:
        L("{{ let a = {}; let sh = (a & 2u) * 8u; atomicAnd(&shmem[a >> 2u], ~(0xffffu << sh)); "
          "atomicOr(&shmem[a >> 2u], (({}) & 0xffffu) << sh); }}",
          A(inst, 0), A(inst, 1));
        return;
    case Op::LoadSharedU64:
        L("{{ let a = ({}) >> 2u; {} = vec2<u32>({}, {}); }}", A(inst, 0), Name(&inst),
          SharedLoad("a"), SharedLoad("a + 1u"));
        return;
    case Op::WriteSharedU64:
        L("{{ let a = ({}) >> 2u; let v = {}; atomicStore(&shmem[a], v.x); "
          "atomicStore(&shmem[a + 1u], v.y); }}",
          A(inst, 0), A(inst, 1));
        return;

    // ---- composites ----
    case Op::CompositeConstructU32x2:
        composite("vec2<u32>");
        return;
    case Op::CompositeConstructU32x3:
        composite("vec3<u32>");
        return;
    case Op::CompositeConstructU32x4:
    case Op::CompositeConstructU32x2x2:
        composite("vec4<u32>");
        return;
    case Op::CompositeConstructF32x2:
        composite("vec2<f32>");
        return;
    case Op::CompositeConstructF32x3:
        composite("vec3<f32>");
        return;
    case Op::CompositeConstructF32x4:
    case Op::CompositeConstructF32x2x2:
        composite("vec4<f32>");
        return;
    case Op::CompositeExtractU32x2:
    case Op::CompositeExtractU32x3:
    case Op::CompositeExtractU32x4:
    case Op::CompositeExtractF32x2:
    case Op::CompositeExtractF32x3:
    case Op::CompositeExtractF32x4:
        set(fmt::format("({}).{}", A(inst, 0), kSwizzle[inst.Arg(1).U32() & 3]));
        return;
    case Op::CompositeInsertU32x2:
    case Op::CompositeInsertU32x3:
    case Op::CompositeInsertU32x4:
    case Op::CompositeInsertF32x2:
    case Op::CompositeInsertF32x3:
    case Op::CompositeInsertF32x4:
        L("{} = {};", Name(&inst), A(inst, 0));
        L("{}.{} = {};", Name(&inst), kSwizzle[inst.Arg(2).U32() & 3], A(inst, 1));
        return;
    case Op::CompositeShuffleU32x2:
    case Op::CompositeShuffleU32x3:
    case Op::CompositeShuffleU32x4:
    case Op::CompositeShuffleF32x2:
    case Op::CompositeShuffleF32x3:
    case Op::CompositeShuffleF32x4: {
        // Indices count across both inputs, like OpVectorShuffle.
        const bool f = op == Op::CompositeShuffleF32x2 || op == Op::CompositeShuffleF32x3 ||
                       op == Op::CompositeShuffleF32x4;
        const auto in_type = inst.Arg(0).Type();
        const u32 width = (in_type == IR::Type::U32x2 || in_type == IR::Type::F32x2) ? 2
                          : (in_type == IR::Type::U32x3 || in_type == IR::Type::F32x3) ? 3
                                                                                        : 4;
        std::string parts;
        const size_t n = inst.NumArgs() - 2;
        for (size_t i = 0; i < n; i++) {
            const u32 idx = inst.Arg(2 + i).U32();
            const std::string src = idx < width ? A(inst, 0) : A(inst, 1);
            parts += fmt::format("{}({}).{}", i ? ", " : "", src, kSwizzle[(idx % width) & 3]);
        }
        set(fmt::format("vec{}<{}>({})", n, f ? "f32" : "u32", parts));
        return;
    }

    // ---- select / casts / packing ----
    case Op::SelectU1:
    case Op::SelectU32:
    case Op::SelectF32:
    case Op::SelectU64:
        set(fmt::format("select({}, {}, {})", A(inst, 2), A(inst, 1), A(inst, 0)));
        return;
    case Op::BitCastU32F32:
        set(fmt::format("bitcast<u32>({})", A(inst, 0)));
        return;
    case Op::BitCastF32U32:
        set(fmt::format("bitcast<f32>({})", A(inst, 0)));
        return;
    case Op::BitCastU16F16:
        // F16 values are carried as f32 here.
        set(fmt::format("(pack2x16float(vec2<f32>({}, 0.0f)) & 0xffffu)", A(inst, 0)));
        return;
    case Op::BitCastF16U16:
        set(fmt::format("unpack2x16float({}).x", A(inst, 0)));
        return;
    case Op::PackUint2x32:
    case Op::UnpackUint2x32:
        set(A(inst, 0));
        return;
    case Op::PackDouble2x32:
        uses_f64 = true;
        Need("f64");
        set(fmt::format("f64_bits_to_f32({})", A(inst, 0)));
        return;
    case Op::UnpackDouble2x32:
        uses_f64 = true;
        Need("f64");
        set(fmt::format("f32_to_f64_bits({})", A(inst, 0)));
        return;
    case Op::PackUnorm2x16:
        un("pack2x16unorm");
        return;
    case Op::UnpackUnorm2x16:
        un("unpack2x16unorm");
        return;
    case Op::PackSnorm2x16:
        un("pack2x16snorm");
        return;
    case Op::UnpackSnorm2x16:
        un("unpack2x16snorm");
        return;
    case Op::PackHalf2x16:
        un("pack2x16float");
        return;
    case Op::UnpackHalf2x16:
        un("unpack2x16float");
        return;
    case Op::PackUnorm4x8:
        un("pack4x8unorm");
        return;
    case Op::UnpackUnorm4x8:
        un("unpack4x8unorm");
        return;
    case Op::PackSnorm4x8:
        un("pack4x8snorm");
        return;
    case Op::UnpackSnorm4x8:
        un("unpack4x8snorm");
        return;
    case Op::PackUint2x16:
    case Op::PackSint2x16:
        L("{{ let u = bitcast<vec2<u32>>({}); {} = (u.x & 0xffffu) | (u.y << 16u); }}",
          A(inst, 0), Name(&inst));
        return;
    case Op::UnpackUint2x16:
        set(fmt::format("bitcast<vec2<f32>>(vec2<u32>(extractBits({0}, 0u, 16u), "
                        "extractBits({0}, 16u, 16u)))",
                        A(inst, 0)));
        return;
    case Op::UnpackSint2x16:
        set(fmt::format("bitcast<vec2<f32>>(vec2<i32>(extractBits(bitcast<i32>({0}), 0u, 16u), "
                        "extractBits(bitcast<i32>({0}), 16u, 16u)))",
                        A(inst, 0)));
        return;
    case Op::PackUint4x8:
    case Op::PackSint4x8:
        L("{{ let u = bitcast<vec4<u32>>({}); {} = (u.x & 0xffu) | ((u.y & 0xffu) << 8u) | "
          "((u.z & 0xffu) << 16u) | (u.w << 24u); }}",
          A(inst, 0), Name(&inst));
        return;
    case Op::UnpackUint4x8:
        set(fmt::format("bitcast<vec4<f32>>(vec4<u32>(extractBits({0}, 0u, 8u), extractBits({0}, "
                        "8u, 8u), extractBits({0}, 16u, 8u), extractBits({0}, 24u, 8u)))",
                        A(inst, 0)));
        return;
    case Op::UnpackSint4x8:
        set(fmt::format("bitcast<vec4<f32>>(vec4<i32>(extractBits(bitcast<i32>({0}), 0u, 8u), "
                        "extractBits(bitcast<i32>({0}), 8u, 8u), extractBits(bitcast<i32>({0}), "
                        "16u, 8u), extractBits(bitcast<i32>({0}), 24u, 8u)))",
                        A(inst, 0)));
        return;
    case Op::PackUint2_10_10_10:
    case Op::PackSint2_10_10_10:
        L("{{ let u = bitcast<vec4<u32>>({}); {} = (u.x & 0x3ffu) | ((u.y & 0x3ffu) << 10u) | "
          "((u.z & 0x3ffu) << 20u) | (u.w << 30u); }}",
          A(inst, 0), Name(&inst));
        return;
    case Op::UnpackUint2_10_10_10:
        set(fmt::format("bitcast<vec4<f32>>(vec4<u32>(extractBits({0}, 0u, 10u), "
                        "extractBits({0}, 10u, 10u), extractBits({0}, 20u, 10u), "
                        "extractBits({0}, 30u, 2u)))",
                        A(inst, 0)));
        return;
    case Op::UnpackSint2_10_10_10:
        set(fmt::format("bitcast<vec4<f32>>(vec4<i32>(extractBits(bitcast<i32>({0}), 0u, 10u), "
                        "extractBits(bitcast<i32>({0}), 10u, 10u), extractBits(bitcast<i32>({0}), "
                        "20u, 10u), extractBits(bitcast<i32>({0}), 30u, 2u)))",
                        A(inst, 0)));
        return;
    case Op::PackUnorm2_10_10_10:
        L("{{ let u = vec4<u32>(round(clamp({}, vec4<f32>(0.0f), vec4<f32>(1.0f)) * "
          "vec4<f32>(1023.0f, 1023.0f, 1023.0f, 3.0f))); {} = u.x | (u.y << 10u) | (u.z << 20u) | "
          "(u.w << 30u); }}",
          A(inst, 0), Name(&inst));
        return;
    case Op::UnpackUnorm2_10_10_10:
        set(fmt::format("vec4<f32>(f32(extractBits({0}, 0u, 10u)), f32(extractBits({0}, 10u, "
                        "10u)), f32(extractBits({0}, 20u, 10u)), f32(extractBits({0}, 30u, 2u))) "
                        "/ vec4<f32>(1023.0f, 1023.0f, 1023.0f, 3.0f)",
                        A(inst, 0)));
        return;
    case Op::PackSnorm2_10_10_10:
        L("{{ let s = vec4<i32>(round(clamp({}, vec4<f32>(-1.0f), vec4<f32>(1.0f)) * "
          "vec4<f32>(511.0f, 511.0f, 511.0f, 1.0f))); let u = bitcast<vec4<u32>>(s); {} = (u.x & "
          "0x3ffu) | ((u.y & 0x3ffu) << 10u) | ((u.z & 0x3ffu) << 20u) | (u.w << 30u); }}",
          A(inst, 0), Name(&inst));
        return;
    case Op::UnpackSnorm2_10_10_10:
        set(fmt::format("vec4<f32>(f32(extractBits(bitcast<i32>({0}), 0u, 10u)), "
                        "f32(extractBits(bitcast<i32>({0}), 10u, 10u)), "
                        "f32(extractBits(bitcast<i32>({0}), 20u, 10u)), "
                        "f32(extractBits(bitcast<i32>({0}), 30u, 2u))) / vec4<f32>(511.0f, "
                        "511.0f, 511.0f, 1.0f)",
                        A(inst, 0)));
        return;
    case Op::PackUfloat10_11_11:
        Need("ufloat");
        L("{{ let f = {}; {} = f32_to_ufloat(f.x, 6u) | (f32_to_ufloat(f.y, 6u) << 11u) | "
          "(f32_to_ufloat(f.z, 5u) << 22u); }}",
          A(inst, 0), Name(&inst));
        return;
    case Op::UnpackUfloat10_11_11:
        Need("ufloat");
        set(fmt::format("vec3<f32>(ufloat_to_f32(extractBits({0}, 0u, 11u), 6u), "
                        "ufloat_to_f32(extractBits({0}, 11u, 11u), 6u), "
                        "ufloat_to_f32(extractBits({0}, 22u, 10u), 5u))",
                        A(inst, 0)));
        return;

    // ---- floating point (64-bit variants run in f32) ----
    case Op::FPAbs32:
    case Op::FPAbs64:
        un("abs");
        return;
    case Op::FPAdd32:
    case Op::FPAdd64:
        bin("+");
        return;
    case Op::FPSub32:
        bin("-");
        return;
    case Op::FPMul32:
    case Op::FPMul64:
        bin("*");
        return;
    case Op::FPDiv32:
    case Op::FPDiv64:
        bin("/");
        return;
    case Op::FPFma32:
    case Op::FPFma64:
        set(fmt::format("fma({}, {}, {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::FPMax32:
    case Op::FPMax64:
        call2("max");
        return;
    case Op::FPMin32:
    case Op::FPMin64:
        call2("min");
        return;
    case Op::FPMinTri32:
        set(fmt::format("min(min({}, {}), {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::FPMaxTri32:
        set(fmt::format("max(max({}, {}), {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::FPMedTri32:
        set(fmt::format("max(min({0}, {1}), min(max({0}, {1}), {2}))", A(inst, 0), A(inst, 1),
                        A(inst, 2)));
        return;
    case Op::FPNeg32:
    case Op::FPNeg64:
        set(fmt::format("(-{})", A(inst, 0)));
        return;
    case Op::FPRecip32:
    case Op::FPRecip64:
        set(fmt::format("(1.0f / {})", A(inst, 0)));
        return;
    case Op::FPRecipSqrt32:
    case Op::FPRecipSqrt64:
        un("inverseSqrt");
        return;
    case Op::FPSqrt:
        un("sqrt");
        return;
    case Op::FPSin:
        un("sin");
        return;
    case Op::FPCos:
        un("cos");
        return;
    case Op::FPExp2:
        un("exp2");
        return;
    case Op::FPLog2:
        un("log2");
        return;
    case Op::FPPow:
        call2("pow");
        return;
    case Op::FPLdexp:
        set(fmt::format("ldexp({}, {})", A(inst, 0), S(inst, 1)));
        return;
    case Op::FPSaturate32:
    case Op::FPSaturate64:
        set(fmt::format("clamp({}, 0.0f, 1.0f)", A(inst, 0)));
        return;
    case Op::FPClamp32:
    case Op::FPClamp64:
        set(fmt::format("clamp({}, {}, {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::FPRoundEven32:
    case Op::FPRoundEven64:
        un("round");
        return;
    case Op::FPFloor32:
    case Op::FPFloor64:
        un("floor");
        return;
    case Op::FPCeil32:
    case Op::FPCeil64:
        un("ceil");
        return;
    case Op::FPTrunc32:
    case Op::FPTrunc64:
        un("trunc");
        return;
    case Op::FPFract32:
    case Op::FPFract64:
        un("fract");
        return;
    case Op::FPFrexpSig32:
    case Op::FPFrexpSig64:
        set(fmt::format("frexp({}).fract", A(inst, 0)));
        return;
    case Op::FPFrexpExp32:
    case Op::FPFrexpExp64:
        set(fmt::format("bitcast<u32>(frexp({}).exp)", A(inst, 0)));
        return;
    case Op::FPOrdEqual32:
    case Op::FPOrdEqual64:
        fcmp_ord("==");
        return;
    case Op::FPUnordEqual32:
    case Op::FPUnordEqual64:
        fcmp_unord("==");
        return;
    case Op::FPOrdNotEqual32:
    case Op::FPOrdNotEqual64:
        Need("isnan");
        set(fmt::format("(({0} != {1}) && !is_nan_f({0}) && !is_nan_f({1}))", A(inst, 0),
                        A(inst, 1)));
        return;
    case Op::FPUnordNotEqual32:
    case Op::FPUnordNotEqual64:
        fcmp_ord("!=");
        return;
    case Op::FPOrdLessThan32:
    case Op::FPOrdLessThan64:
        fcmp_ord("<");
        return;
    case Op::FPUnordLessThan32:
    case Op::FPUnordLessThan64:
        fcmp_unord("<");
        return;
    case Op::FPOrdGreaterThan32:
    case Op::FPOrdGreaterThan64:
        fcmp_ord(">");
        return;
    case Op::FPUnordGreaterThan32:
    case Op::FPUnordGreaterThan64:
        fcmp_unord(">");
        return;
    case Op::FPOrdLessThanEqual32:
    case Op::FPOrdLessThanEqual64:
        fcmp_ord("<=");
        return;
    case Op::FPUnordLessThanEqual32:
    case Op::FPUnordLessThanEqual64:
        fcmp_unord("<=");
        return;
    case Op::FPOrdGreaterThanEqual32:
    case Op::FPOrdGreaterThanEqual64:
        fcmp_ord(">=");
        return;
    case Op::FPUnordGreaterThanEqual32:
    case Op::FPUnordGreaterThanEqual64:
        fcmp_unord(">=");
        return;
    case Op::FPIsNan32:
    case Op::FPIsNan64:
        Need("isnan");
        set(fmt::format("is_nan_f({})", A(inst, 0)));
        return;
    case Op::FPIsInf32:
    case Op::FPIsInf64:
        set(fmt::format("((bitcast<u32>({}) & 0x7fffffffu) == 0x7f800000u)", A(inst, 0)));
        return;

    // ---- integer ----
    case Op::IAdd32:
        bin("+");
        return;
    case Op::ISub32:
        bin("-");
        return;
    case Op::IMul32:
        bin("*");
        return;
    case Op::UMulHi:
        Need("mulhi");
        call2("umulhi");
        return;
    case Op::SMulHi:
        Need("mulhi");
        call2("smulhi");
        return;
    case Op::UDiv32:
        set(fmt::format("select({0} / {1}, 0xffffffffu, {1} == 0u)", A(inst, 0), A(inst, 1)));
        return;
    case Op::SDiv32:
        set(fmt::format("bitcast<u32>({} / {})", S(inst, 0), S(inst, 1)));
        return;
    case Op::UMod32:
        bin("%");
        return;
    case Op::SMod32:
        set(fmt::format("bitcast<u32>({} % {})", S(inst, 0), S(inst, 1)));
        return;
    case Op::INeg32:
        set(fmt::format("(0u - {})", A(inst, 0)));
        return;
    case Op::IAbs32:
        set(fmt::format("bitcast<u32>(abs({}))", S(inst, 0)));
        return;
    case Op::ShiftLeftLogical32:
        set(fmt::format("({} << ({} & 31u))", A(inst, 0), A(inst, 1)));
        return;
    case Op::ShiftRightLogical32:
        set(fmt::format("({} >> ({} & 31u))", A(inst, 0), A(inst, 1)));
        return;
    case Op::ShiftRightArithmetic32:
        set(fmt::format("bitcast<u32>({} >> ({} & 31u))", S(inst, 0), A(inst, 1)));
        return;
    case Op::BitwiseAnd32:
    case Op::BitwiseAnd64:
        bin("&");
        return;
    case Op::BitwiseOr32:
    case Op::BitwiseOr64:
        bin("|");
        return;
    case Op::BitwiseXor32:
    case Op::BitwiseXor64:
        bin("^");
        return;
    case Op::BitwiseNot32:
    case Op::BitwiseNot64:
        set(fmt::format("(~{})", A(inst, 0)));
        return;
    case Op::BitFieldInsert32:
        set(fmt::format("insertBits({}, {}, {}, {})", A(inst, 0), A(inst, 1), A(inst, 2),
                        A(inst, 3)));
        return;
    case Op::BitFieldSExtract:
        set(fmt::format("bitcast<u32>(extractBits({}, {}, {}))", S(inst, 0), A(inst, 1),
                        A(inst, 2)));
        return;
    case Op::BitFieldUExtract:
        set(fmt::format("extractBits({}, {}, {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::BitReverse32:
        un("reverseBits");
        return;
    case Op::BitCount32:
        un("countOneBits");
        return;
    case Op::BitCount64:
        set(fmt::format("(countOneBits(({0}).x) + countOneBits(({0}).y))", A(inst, 0)));
        return;
    case Op::FindSMsb32:
        set(fmt::format("bitcast<u32>(firstLeadingBit({}))", S(inst, 0)));
        return;
    case Op::FindUMsb32:
        un("firstLeadingBit");
        return;
    case Op::FindILsb32:
        un("firstTrailingBit");
        return;
    case Op::FindUMsb64:
        set(fmt::format("select(firstLeadingBit(({0}).x), firstLeadingBit(({0}).y) + 32u, "
                        "({0}).y != 0u)",
                        A(inst, 0)));
        return;
    case Op::FindILsb64:
        set(fmt::format("select(firstTrailingBit(({0}).y) + 32u, firstTrailingBit(({0}).x), "
                        "({0}).x != 0u)",
                        A(inst, 0)));
        return;
    case Op::SMin32:
        set(fmt::format("bitcast<u32>(min({}, {}))", S(inst, 0), S(inst, 1)));
        return;
    case Op::SMax32:
        set(fmt::format("bitcast<u32>(max({}, {}))", S(inst, 0), S(inst, 1)));
        return;
    case Op::UMin32:
        call2("min");
        return;
    case Op::UMax32:
        call2("max");
        return;
    case Op::SMinTri32:
        set(fmt::format("bitcast<u32>(min(min({}, {}), {}))", S(inst, 0), S(inst, 1),
                        S(inst, 2)));
        return;
    case Op::SMaxTri32:
        set(fmt::format("bitcast<u32>(max(max({}, {}), {}))", S(inst, 0), S(inst, 1),
                        S(inst, 2)));
        return;
    case Op::UMinTri32:
        set(fmt::format("min(min({}, {}), {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::UMaxTri32:
        set(fmt::format("max(max({}, {}), {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::SMedTri32:
        set(fmt::format("bitcast<u32>(max(min({0}, {1}), min(max({0}, {1}), {2})))", S(inst, 0),
                        S(inst, 1), S(inst, 2)));
        return;
    case Op::UMedTri32:
        set(fmt::format("max(min({0}, {1}), min(max({0}, {1}), {2}))", A(inst, 0), A(inst, 1),
                        A(inst, 2)));
        return;
    case Op::SClamp32:
        set(fmt::format("bitcast<u32>(clamp({}, {}, {}))", S(inst, 0), S(inst, 1), S(inst, 2)));
        return;
    case Op::UClamp32:
        set(fmt::format("clamp({}, {}, {})", A(inst, 0), A(inst, 1), A(inst, 2)));
        return;
    case Op::SLessThan32:
        scmp("<");
        return;
    case Op::SLessThanEqual32:
        scmp("<=");
        return;
    case Op::SGreaterThan32:
        scmp(">");
        return;
    case Op::SGreaterThanEqual32:
        scmp(">=");
        return;
    case Op::ULessThan32:
        ucmp("<");
        return;
    case Op::ULessThanEqual32:
        ucmp("<=");
        return;
    case Op::UGreaterThan32:
        ucmp(">");
        return;
    case Op::UGreaterThanEqual32:
        ucmp(">=");
        return;
    case Op::IEqual32:
        ucmp("==");
        return;
    case Op::INotEqual32:
        ucmp("!=");
        return;
    case Op::MaskedBitCount32:
        Error("MaskedBitCount32");
        set("0u");
        return;

    // ---- 64-bit integers as vec2<u32> (lo, hi) ----
    case Op::IAdd64:
        Need("i64");
        call2("add64");
        return;
    case Op::ISub64:
        Need("i64");
        call2("sub64");
        return;
    case Op::IMul64:
        Need("i64");
        Need("mulhi");
        call2("mul64");
        return;
    case Op::INeg64:
        Need("i64");
        set(fmt::format("sub64(vec2<u32>(0u), {})", A(inst, 0)));
        return;
    case Op::ShiftLeftLogical64:
        Need("i64");
        set(fmt::format("shl64({}, {})", A(inst, 0), ShiftAmount(inst)));
        return;
    case Op::ShiftRightLogical64:
        Need("i64");
        set(fmt::format("shr64({}, {})", A(inst, 0), ShiftAmount(inst)));
        return;
    case Op::ShiftRightArithmetic64:
        Need("i64");
        set(fmt::format("sar64({}, {})", A(inst, 0), ShiftAmount(inst)));
        return;
    case Op::IEqual64:
        set(fmt::format("all({} == {})", A(inst, 0), A(inst, 1)));
        return;
    case Op::INotEqual64:
        set(fmt::format("any({} != {})", A(inst, 0), A(inst, 1)));
        return;
    case Op::ULessThan64:
        Need("i64");
        call2("ult64");
        return;
    case Op::UGreaterThan64:
        Need("i64");
        set(fmt::format("ult64({}, {})", A(inst, 1), A(inst, 0)));
        return;
    case Op::ULessThanEqual64:
        Need("i64");
        set(fmt::format("!ult64({}, {})", A(inst, 1), A(inst, 0)));
        return;
    case Op::UGreaterThanEqual64:
        Need("i64");
        set(fmt::format("!ult64({}, {})", A(inst, 0), A(inst, 1)));
        return;
    case Op::SLessThan64:
        Need("i64");
        call2("slt64");
        return;
    case Op::SGreaterThan64:
        Need("i64");
        set(fmt::format("slt64({}, {})", A(inst, 1), A(inst, 0)));
        return;
    case Op::SLessThanEqual64:
        Need("i64");
        set(fmt::format("!slt64({}, {})", A(inst, 1), A(inst, 0)));
        return;
    case Op::SGreaterThanEqual64:
        Need("i64");
        set(fmt::format("!slt64({}, {})", A(inst, 0), A(inst, 1)));
        return;
    case Op::ConvertU64U32:
        set(fmt::format("vec2<u32>({}, 0u)", A(inst, 0)));
        return;

    // ---- logical ----
    case Op::LogicalOr:
        bin("||");
        return;
    case Op::LogicalAnd:
        bin("&&");
        return;
    case Op::LogicalXor:
        bin("!=");
        return;
    case Op::LogicalNot:
        set(fmt::format("(!{})", A(inst, 0)));
        return;

    // ---- conversions ----
    case Op::ConvertS32F32:
    case Op::ConvertS32F64:
        set(fmt::format("bitcast<u32>(i32({}))", A(inst, 0)));
        return;
    case Op::ConvertU32F32:
        set(fmt::format("u32({})", A(inst, 0)));
        return;
    case Op::ConvertF16F32:
        set(fmt::format("quantizeToF16({})", A(inst, 0)));
        return;
    case Op::ConvertF32F16:
    case Op::ConvertF32F64:
    case Op::ConvertF64F32:
        set(A(inst, 0));
        return;
    case Op::ConvertF32S32:
    case Op::ConvertF64S32:
        set(fmt::format("f32({})", S(inst, 0)));
        return;
    case Op::ConvertF32U32:
    case Op::ConvertF64U32:
    case Op::ConvertF32U16:
        set(fmt::format("f32({})", A(inst, 0)));
        return;
    case Op::ConvertU16U32:
        set(fmt::format("({} & 0xffffu)", A(inst, 0)));
        return;
    case Op::ConvertU8U32:
        set(fmt::format("({} & 0xffu)", A(inst, 0)));
        return;
    case Op::ConvertU32U16:
    case Op::ConvertU32U8:
        set(A(inst, 0));
        return;
    case Op::ConvertS32S8:
        set(fmt::format("bitcast<u32>(extractBits({}, 0u, 8u))", S(inst, 0)));
        return;
    case Op::ConvertS32S16:
        set(fmt::format("bitcast<u32>(extractBits({}, 0u, 16u))", S(inst, 0)));
        return;

    // ---- images ----
    case Op::ImageSampleImplicitLod:
    case Op::ImageSampleExplicitLod:
    case Op::ImageSampleDrefImplicitLod:
    case Op::ImageSampleDrefExplicitLod:
    case Op::ImageGather:
    case Op::ImageGatherDref:
    case Op::ImageQueryDimensions:
    case Op::ImageQueryLod:
    case Op::ImageGradient:
    case Op::ImageRead:
    case Op::ImageWrite:
        EmitImageOp(inst);
        return;

    // ---- wave operations (WGSL subgroups) ----
    case Op::LaneId:
        uses_subgroups = true;
        set("in_subgroup_invocation_id");
        return;
    case Op::ReadFirstLane:
        uses_subgroups = true;
        un("subgroupBroadcastFirst");
        return;
    case Op::ReadLane:
        uses_subgroups = true;
        call2("subgroupShuffle");
        return;
    case Op::ShuffleXor:
        uses_subgroups = true;
        call2("subgroupShuffleXor");
        return;
    case Op::Shuffle:
        uses_subgroups = true;
        call2("subgroupShuffle");
        return;
    case Op::QuadBroadcast:
        uses_subgroups = true;
        call2("quadBroadcast");
        return;
    case Op::Ballot:
        uses_subgroups = true;
        set(fmt::format("subgroupBallot({}).xy", A(inst, 0)));
        return;
    case Op::GroupAny:
        uses_subgroups = true;
        un("subgroupAny");
        return;
    case Op::BallotFindLsb:
        set(fmt::format("select(firstTrailingBit(({0}).y) + 32u, firstTrailingBit(({0}).x), "
                        "({0}).x != 0u)",
                        A(inst, 0)));
        return;
    case Op::Memtime:
        set("vec2<u32>(0u)");
        return;

    default:
        Error(fmt::format("opcode {}", op));
        if (inst.Type() != IR::Type::Void && TypeName(inst.Type())) {
            set(fmt::format("{}()", TypeName(inst.Type())));
        }
        return;
    }
}

// ---- helpers ---------------------------------------------------------------------------------

std::string Emitter::HelperCode() {
    std::string h;
    if (helpers_used.contains("f32bits")) {
        h += "fn f32_bits(b: u32) -> f32 { return bitcast<f32>(b); }\n";
    }
    if (helpers_used.contains("isnan")) {
        // Bit test: WGSL implementations may assume NaN never occurs in float comparisons.
        h += "fn is_nan_f(x: f32) -> bool { return (bitcast<u32>(x) & 0x7fffffffu) > "
             "0x7f800000u; }\n";
    }
    if (helpers_used.contains("mulhi") || helpers_used.contains("i64")) {
        h += R"(fn umulhi(a: u32, b: u32) -> u32 {
    let a0 = a & 0xffffu; let a1 = a >> 16u; let b0 = b & 0xffffu; let b1 = b >> 16u;
    let mid = ((a0 * b0) >> 16u) + ((a1 * b0) & 0xffffu) + ((a0 * b1) & 0xffffu);
    return (a1 * b1) + ((a1 * b0) >> 16u) + ((a0 * b1) >> 16u) + (mid >> 16u);
}
fn smulhi(a: u32, b: u32) -> u32 {
    var hi = umulhi(a, b);
    if (bitcast<i32>(a) < 0) { hi = hi - b; }
    if (bitcast<i32>(b) < 0) { hi = hi - a; }
    return hi;
}
)";
    }
    if (helpers_used.contains("i64")) {
        h += R"(fn add64(a: vec2<u32>, b: vec2<u32>) -> vec2<u32> {
    let lo = a.x + b.x; return vec2<u32>(lo, a.y + b.y + select(0u, 1u, lo < a.x));
}
fn sub64(a: vec2<u32>, b: vec2<u32>) -> vec2<u32> {
    return vec2<u32>(a.x - b.x, a.y - b.y - select(0u, 1u, a.x < b.x));
}
fn shl64(a: vec2<u32>, s: u32) -> vec2<u32> {
    let n = s & 63u;
    if (n == 0u) { return a; }
    if (n >= 32u) { return vec2<u32>(0u, a.x << (n - 32u)); }
    return vec2<u32>(a.x << n, (a.y << n) | (a.x >> (32u - n)));
}
fn shr64(a: vec2<u32>, s: u32) -> vec2<u32> {
    let n = s & 63u;
    if (n == 0u) { return a; }
    if (n >= 32u) { return vec2<u32>(a.y >> (n - 32u), 0u); }
    return vec2<u32>((a.x >> n) | (a.y << (32u - n)), a.y >> n);
}
fn sar64(a: vec2<u32>, s: u32) -> vec2<u32> {
    let n = s & 63u;
    let hi = bitcast<i32>(a.y);
    if (n == 0u) { return a; }
    if (n >= 32u) { return vec2<u32>(bitcast<u32>(hi >> (n - 32u)), bitcast<u32>(hi >> 31u)); }
    return vec2<u32>((a.x >> n) | (a.y << (32u - n)), bitcast<u32>(hi >> n));
}
fn ult64(a: vec2<u32>, b: vec2<u32>) -> bool { return a.y < b.y || (a.y == b.y && a.x < b.x); }
fn slt64(a: vec2<u32>, b: vec2<u32>) -> bool {
    return bitcast<i32>(a.y) < bitcast<i32>(b.y) || (a.y == b.y && a.x < b.x);
}
fn mul64(a: vec2<u32>, b: vec2<u32>) -> vec2<u32> {
    return vec2<u32>(a.x * b.x, umulhi(a.x, b.x) + a.x * b.y + a.y * b.x);
}
)";
    }
    if (helpers_used.contains("f64")) {
        // Doubles are carried as f32; these convert at the bit level where the IR reinterprets.
        h += R"(fn f64_bits_to_f32(v: vec2<u32>) -> f32 {
    let sign = v.y & 0x80000000u;
    let e = i32((v.y >> 20u) & 0x7ffu);
    if (e == 0) { return bitcast<f32>(sign); }
    if (e == 0x7ff) { return bitcast<f32>(sign | 0x7f800000u | select(0u, 1u, (v.y & 0xfffffu) != 0u || v.x != 0u)); }
    let e32 = e - 1023 + 127;
    if (e32 >= 255) { return bitcast<f32>(sign | 0x7f800000u); }
    if (e32 <= 0) { return bitcast<f32>(sign); }
    let mant = ((v.y & 0xfffffu) << 3u) | (v.x >> 29u);
    return bitcast<f32>(sign | (u32(e32) << 23u) | mant);
}
fn f32_to_f64_bits(f: f32) -> vec2<u32> {
    let b = bitcast<u32>(f);
    let sign = b & 0x80000000u;
    let e = (b >> 23u) & 0xffu;
    let m = b & 0x7fffffu;
    if (e == 0u) { return vec2<u32>(0u, sign); }
    if (e == 255u) { return vec2<u32>(m << 29u, sign | 0x7ff00000u | (m >> 3u)); }
    return vec2<u32>(m << 29u, sign | ((e - 127u + 1023u) << 20u) | (m >> 3u));
}
)";
    }
    if (helpers_used.contains("ufloat")) {
        h += R"(fn f32_to_ufloat(f: f32, mbits: u32) -> u32 {
    let b = bitcast<u32>(f);
    if ((b & 0x80000000u) != 0u || b == 0u) { return 0u; }
    let e = i32((b >> 23u) & 0xffu) - 127 + 15;
    if (((b >> 23u) & 0xffu) == 255u) { return (31u << mbits) | select(0u, 1u, (b & 0x7fffffu) != 0u); }
    if (e >= 31) { return 31u << mbits; }
    if (e <= 0) { return u32(round(f * f32(1u << (mbits + 14u)))) & ((1u << mbits) - 1u); }
    return (u32(e) << mbits) | ((b & 0x7fffffu) >> (23u - mbits));
}
fn ufloat_to_f32(v: u32, mbits: u32) -> f32 {
    let e = v >> mbits;
    let m = v & ((1u << mbits) - 1u);
    if (e == 0u) { return f32(m) / f32(1u << (14u + mbits)); }
    if (e == 31u) { return bitcast<f32>(0x7f800000u | m); }
    return bitcast<f32>(((e - 15u + 127u) << 23u) | (m << (23u - mbits)));
}
)";
    }
    if (helpers_used.contains("atomic_signed")) {
        for (const char* space : {"storage, read_write", "workgroup"}) {
            const char* suffix = std::string_view(space) == "workgroup" ? "_w" : "";
            h += fmt::format(
                "fn atomicMin_signed{0}(p: ptr<{1}, atomic<u32>>, v: u32) -> u32 {{\n"
                "    var old = atomicLoad(p);\n"
                "    loop {{ let n = bitcast<u32>(min(bitcast<i32>(old), bitcast<i32>(v)));\n"
                "        let r = atomicCompareExchangeWeak(p, old, n); if (r.exchanged) {{ return "
                "old; }} old = r.old_value; }}\n}}\n"
                "fn atomicMax_signed{0}(p: ptr<{1}, atomic<u32>>, v: u32) -> u32 {{\n"
                "    var old = atomicLoad(p);\n"
                "    loop {{ let n = bitcast<u32>(max(bitcast<i32>(old), bitcast<i32>(v)));\n"
                "        let r = atomicCompareExchangeWeak(p, old, n); if (r.exchanged) {{ return "
                "old; }} old = r.old_value; }}\n}}\n",
                suffix, space);
        }
    }
    if (helpers_used.contains("atomic_incdec")) {
        h += R"(fn atomic_inc_wrap(p: ptr<storage, atomic<u32>, read_write>) -> u32 { return atomicAdd(p, 1u); }
fn atomic_dec_wrap(p: ptr<storage, atomic<u32>, read_write>) -> u32 { return atomicSub(p, 1u); }
)";
    }
    if (helpers_used.contains("atomic_incdec_shared")) {
        h += R"(fn atomic_inc_wrap_shared(p: ptr<workgroup, atomic<u32>>) -> u32 { return atomicAdd(p, 1u); }
fn atomic_dec_wrap_shared(p: ptr<workgroup, atomic<u32>>) -> u32 { return atomicSub(p, 1u); }
)";
    }
    return h;
}

// ---- entry points ----------------------------------------------------------------------------

void Emitter::EmitEntryPoint() {
    std::string& e = entry;
    std::string& d = decls;
    switch (info.sw_stage) {
    case SwStage::Vertex: {
        // Attributes come from the fetch shader, translated inline as buffer loads, so the only
        // inputs are the vertex and instance index. This entry point is for checking the
        // program on its own; draws use LinkVertexShader's.
        d += "var<private> in_vertex_index: u32;\nvar<private> in_instance_index: u32;\n";
        d += "var<private> out_position: vec4<f32>;\n";
        std::string fields = "    @builtin(position) position: vec4<f32>,\n";
        std::string copy_out = "    o.position = out_position;\n";
        for (u32 i = 0; i < IR::NumParams; i++) {
            if (!info.stores.GetAny(IR::Attribute::Param0 + i)) {
                continue;
            }
            out.param_mask |= 1u << i;
            d += fmt::format("var<private> out_param{}: vec4<f32>;\n", i);
            fields += fmt::format("    @location({}) param{}: vec4<f32>,\n", i, i);
            copy_out += fmt::format("    o.param{} = out_param{};\n", i, i);
        }
        e += fmt::format("struct VsOut {{\n{}}}\n", fields);
        e += fmt::format("@vertex\nfn main(@builtin(vertex_index) vi: u32, @builtin(instance_index) "
                         "ii: u32) -> VsOut {{\n    in_vertex_index = vi;\n    in_instance_index = "
                         "ii;\n    shader_main();\n    var o: VsOut;\n{}    return o;\n}}\n",
                         copy_out);
        break;
    }
    case SwStage::Fragment: {
        std::string params = "@builtin(position) frag_coord: vec4<f32>";
        std::string copy_in = "    in_frag_coord = frag_coord;\n";
        d += "var<private> in_frag_coord: vec4<f32>;\n";
        if (info.loads.Get(IR::Attribute::IsFrontFace)) {
            d += "var<private> in_front_facing: bool;\n";
            params += ", @builtin(front_facing) front_facing: bool";
            copy_in += "    in_front_facing = front_facing;\n";
        }
        if (info.loads.Get(IR::Attribute::SampleIndex)) {
            d += "var<private> in_sample_index: u32;\n";
            params += ", @builtin(sample_index) sample_index: u32";
            copy_in += "    in_sample_index = sample_index;\n";
        }
        for (u32 i = 0; i < IR::NumParams; i++) {
            if (!info.loads.GetAny(IR::Attribute::Param0 + i)) {
                continue;
            }
            u32 param = i;
            bool flat = false;
            if (i < rt.hw.fs.num_inputs) {
                const auto& input = rt.hw.fs.inputs[i];
                param = input.param_index;
                flat = input.is_flat;
            }
            const u32 location = static_cast<u32>(out.inputs.size());
            // The vertex stage's output has to be declared the same way (LinkVertexShader).
            const auto& interp = info.fs_interpolation[i];
            std::string mode;
            if (flat || interp.primary == Qualifier::Flat) {
                mode = "flat";
            } else {
                if (interp.primary == Qualifier::PerVertex) {
                    Error("per-vertex fragment input (needs barycentrics)");
                }
                const char* type =
                    interp.primary == Qualifier::NoPerspective ? "linear" : "perspective";
                const char* sampling = interp.auxiliary == Qualifier::Centroid ? "centroid"
                                       : interp.auxiliary == Qualifier::Sample ? "sample"
                                                                               : "";
                if (*sampling) {
                    mode = fmt::format("{}, {}", type, sampling);
                } else if (interp.primary == Qualifier::NoPerspective) {
                    mode = type;
                }
            }
            out.inputs.push_back({location, param, mode});
            d += fmt::format("var<private> in_param{}: vec4<f32>;\n", i);
            params += fmt::format(", @location({}){} p{}: vec4<f32>", location,
                                  mode.empty() ? "" : fmt::format(" @interpolate({})", mode), i);
            copy_in += fmt::format("    in_param{} = p{};\n", i, i);
        }
        std::string fields;
        std::string copy_out;
        for (u32 i = 0; i < IR::NumRenderTargets; i++) {
            if (!info.stores.GetAny(IR::Attribute::RenderTarget0 + i)) {
                continue;
            }
            const auto nfmt = rt.hw.fs.color_buffers[i].num_format;
            const std::string t = nfmt == AmdGpu::NumberFormat::Uint   ? "vec4<u32>"
                                  : nfmt == AmdGpu::NumberFormat::Sint ? "vec4<i32>"
                                                                       : "vec4<f32>";
            d += fmt::format("var<private> out_color{}: {};\n", i, t);
            fields += fmt::format("    @location({}) color{}: {},\n", i, i, t);
            copy_out += fmt::format("    o.color{} = out_color{};\n", i, i);
            out.color_output_mask |= 1u << i;
        }
        if (info.stores.Get(IR::Attribute::Depth)) {
            d += "var<private> out_depth: f32;\n";
            fields += "    @builtin(frag_depth) depth: f32,\n";
            copy_out += "    o.depth = out_depth;\n";
            out.writes_depth = true;
        }
        if (fields.empty()) {
            e += fmt::format("@fragment\nfn main({}) {{\n{}    shader_main();\n}}\n", params,
                             copy_in);
        } else {
            e += fmt::format("struct FsOut {{\n{}}}\n", fields);
            e += fmt::format("@fragment\nfn main({}) -> FsOut {{\n{}    shader_main();\n    var o: "
                             "FsOut;\n{}    return o;\n}}\n",
                             params, copy_in, copy_out);
        }
        break;
    }
    case SwStage::Compute: {
        d += "var<private> in_workgroup_id: vec3<u32>;\nvar<private> in_local_invocation_id: "
             "vec3<u32>;\nvar<private> in_local_invocation_index: u32;\nvar<private> "
             "in_num_workgroups: vec3<u32>;\n";
        const auto& wg = rt.hw.cs.workgroup_size;
        e += fmt::format(
            "@compute @workgroup_size({}, {}, {})\nfn main(@builtin(workgroup_id) wid: vec3<u32>, "
            "@builtin(local_invocation_id) lid: vec3<u32>, @builtin(local_invocation_index) lidx: "
            "u32, @builtin(num_workgroups) nwg: vec3<u32>{}) {{\n    in_workgroup_id = wid;\n    "
            "in_local_invocation_id = lid;\n    in_local_invocation_index = lidx;\n    "
            "in_num_workgroups = nwg;\n{}    shader_main();\n}}\n",
            std::max(1u, wg[0]), std::max(1u, wg[1]), std::max(1u, wg[2]),
            uses_subgroups ? ", @builtin(subgroup_invocation_id) sid: u32" : "",
            uses_subgroups ? "    in_subgroup_invocation_id = sid;\n" : "");
        break;
    }
    default:
        Error(fmt::format("shader stage {} (WebGPU has vertex, fragment and compute only)",
                          magic_enum::enum_name(info.sw_stage)));
        break;
    }
    if (uses_subgroups) {
        d += "var<private> in_subgroup_invocation_id: u32;\n";
        if (info.sw_stage != SwStage::Compute) {
            Error("wave operations outside compute shaders");
        }
    }
}

Output Emitter::Run() {
    out.group = group;
    Prepass();
    EmitDeclarations();
    Traverse();
    EmitEntryPoint();

    std::string code;
    code += fmt::format("// shadPS4 WGSL backend: {} shader {:#018x}\n",
                        magic_enum::enum_name(info.sw_stage), info.pgm_hash);
    if (uses_subgroups) {
        code += "enable subgroups;\ndiagnostic(off, subgroup_uniformity);\n";
    }
    // GCN control flow is per-lane (exec masks), so texture sampling routinely happens in
    // control flow WGSL can't prove uniform; the derivatives are still well defined per quad.
    code += "diagnostic(off, derivative_uniformity);\n\n";
    code += decls;
    code += HelperCode();
    code += "\nfn shader_main() {\n";
    for (const auto& [id, type] : var_decls) {
        const char* name = TypeName(type);
        if (!name) {
            Error(fmt::format("value of type {}", type));
            name = "u32";
        }
        code += fmt::format("    var v{}: {};\n", id, name);
    }
    code += body;
    code += "}\n\n";
    out.code_without_entry = code;
    code += entry;
    if (uses_f64) {
        code += "// note: 64-bit floats are evaluated in 32-bit precision\n";
    }
    out.code = std::move(code);
    return std::move(out);
}

} // namespace

std::string StorageTexelFormat(AmdGpu::DataFormat dfmt, AmdGpu::NumberFormat nfmt) {
    using DF = AmdGpu::DataFormat;
    using NF = AmdGpu::NumberFormat;
    const bool u = nfmt == NF::Uint;
    const bool s = nfmt == NF::Sint;
    const bool sn = nfmt == NF::Snorm;
    const bool f = nfmt == NF::Float;
    // Unorm and sRGB images store the same bits: GCN image stores don't encode sRGB.
    const bool un = nfmt == NF::Unorm || nfmt == NF::Srgb;
    const auto pick = [&](const char* unorm, const char* snorm, const char* uint, const char* sint,
                          const char* flt) -> std::string {
        const char* r = un ? unorm : sn ? snorm : u ? uint : s ? sint : f ? flt : nullptr;
        return r ? r : "";
    };
    switch (dfmt) {
    case DF::Format8:
        return pick("r8unorm", "r8snorm", "r8uint", "r8sint", nullptr);
    case DF::Format8_8:
        return pick("rg8unorm", "rg8snorm", "rg8uint", "rg8sint", nullptr);
    case DF::Format8_8_8_8:
        return pick("rgba8unorm", "rgba8snorm", "rgba8uint", "rgba8sint", nullptr);
    case DF::Format16:
        return pick("r16unorm", "r16snorm", "r16uint", "r16sint", "r16float");
    case DF::Format16_16:
        return pick("rg16unorm", "rg16snorm", "rg16uint", "rg16sint", "rg16float");
    case DF::Format16_16_16_16:
        return pick("rgba16unorm", "rgba16snorm", "rgba16uint", "rgba16sint", "rgba16float");
    case DF::Format32:
        return pick(nullptr, nullptr, "r32uint", "r32sint", "r32float");
    case DF::Format32_32:
        return pick(nullptr, nullptr, "rg32uint", "rg32sint", "rg32float");
    case DF::Format32_32_32_32:
        return pick(nullptr, nullptr, "rgba32uint", "rgba32sint", "rgba32float");
    case DF::Format2_10_10_10:
        return pick("rgb10a2unorm", nullptr, "rgb10a2uint", nullptr, nullptr);
    case DF::Format10_11_11:
        return f ? "rg11b10ufloat" : "";
    default:
        return "";
    }
}

std::string LinkVertexShader(const Output& vs, const VertexLink& link) {
    std::string c = vs.code_without_entry;
    c += "struct DrawInfo {\n    vx: vec4<f32>,\n    vy: vec4<f32>,\n    vz: vec4<f32>,\n    pad: "
         "vec4<u32>,\n}\n@group(2) @binding(0) var<uniform> draw_info: DrawInfo;\n";
    if (link.rect_list) {
        c += "@group(2) @binding(1) var<storage, read> rect_indices: array<u32>;\n";
    }
    std::string fields = "    @builtin(position) position: vec4<f32>,\n";
    std::string copy;
    std::string reset;
    std::string fourth = "    o.position = a.position + b.position - k.position;\n";
    for (const auto& in : link.inputs) {
        fields += fmt::format("    @location({}){} p{}: vec4<f32>,\n", in.location,
                              in.interpolation.empty()
                                  ? ""
                                  : fmt::format(" @interpolate({})", in.interpolation),
                              in.location);
        const bool written = in.param < 32 && (vs.param_mask >> in.param) & 1;
        copy += written ? fmt::format("    o.p{} = out_param{};\n", in.location, in.param)
                        : fmt::format("    o.p{} = vec4<f32>(0.0f);\n", in.location);
        fourth += fmt::format("    o.p{0} = a.p{0} + b.p{0} - k.p{0};\n", in.location);
    }
    for (u32 i = 0; i < 32; i++) {
        if ((vs.param_mask >> i) & 1) {
            reset += fmt::format("    out_param{} = vec4<f32>(0.0f);\n", i);
        }
    }
    c += fmt::format("struct VsOut {{\n{}}}\n", fields);
    // The guest's position goes through its viewport transform into clip space for a viewport
    // that covers the render target (DrawInfo). Outputs are reset first: a rectangle list runs
    // the program more than once per invocation.
    c += fmt::format(
        "fn vs_run(vi: u32, ii: u32) -> VsOut {{\n    in_vertex_index = vi;\n    in_instance_index "
        "= ii;\n    out_position = vec4<f32>(0.0f, 0.0f, 0.0f, 1.0f);\n{}    shader_main();\n    "
        "var o: VsOut;\n    let p = out_position;\n    o.position = vec4<f32>(p.x * draw_info.vx.x "
        "+ p.w * draw_info.vx.y + draw_info.vx.z, p.y * draw_info.vy.x + p.w * draw_info.vy.y + "
        "draw_info.vy.z, p.z * draw_info.vz.x + p.w * draw_info.vz.y + draw_info.vz.z, p.w);\n{}"
        "    return o;\n}}\n",
        reset, copy);
    if (!link.rect_list) {
        c += "@vertex\nfn main(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) "
             "-> VsOut {\n    return vs_run(vi, ii);\n}\n";
        return c;
    }
    // Two triangles per rectangle: the guest's three vertices, then the two that aren't its
    // corner (the one sharing x with one neighbour and y with the other) and the fourth vertex,
    // opposite that corner - as upstream's rect-list tessellation shader computes it.
    c += fmt::format(
        "@vertex\nfn main(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> "
        "VsOut {{\n    let base = (vi / 6u) * 3u;\n    let corner = vi % 6u;\n    if (corner < 3u) "
        "{{\n        return vs_run(rect_indices[base + corner], ii);\n    }}\n    let o0 = "
        "vs_run(rect_indices[base], ii);\n    let o1 = vs_run(rect_indices[base + 1u], ii);\n    "
        "let o2 = vs_run(rect_indices[base + 2u], ii);\n    let e01 = o0.position.xy == "
        "o1.position.xy;\n    let e12 = o1.position.xy == o2.position.xy;\n    let e20 = "
        "o2.position.xy == o0.position.xy;\n    let c1 = (e12.x && e01.y) || (e12.y && e01.x);\n "
        "   let c2 = (e20.x && e12.y) || (e20.y && e12.x);\n    var k = o0;\n    var a = o1;\n    "
        "var b = o2;\n    if (c1) {{\n        k = o1;\n        a = o2;\n        b = o0;\n    }} "
        "else if (c2) {{\n        k = o2;\n        a = o0;\n        b = o1;\n    }}\n    if "
        "(corner == 3u) {{\n        return a;\n    }}\n    if (corner == 4u) {{\n        return "
        "b;\n    }}\n    var o: VsOut;\n{}    return o;\n}}\n",
        fourth);
    return c;
}

Profile MakeProfile() {
    Profile p{};
    // GCN waves are 64 lanes; the frontend uses this when lowering lane arithmetic.
    p.subgroup_size = 64;
    // WebGPU's guaranteed maxComputeWorkgroupStorageSize is 16KB; desktop adapters give 32KB.
    p.max_shared_memory_size = 32768;
    p.max_viewport_width = 8192;
    p.max_viewport_height = 8192;
    // No f16/f64/barycentric/explicit-layout/BDA in WGSL; LDS accesses need explicit barriers.
    p.needs_lds_barriers = true;
    return p;
}

Output EmitWGSL(const Profile& profile, const RuntimeInfo& runtime_info, const IR::Program& program) {
    Emitter emitter{profile, runtime_info, program};
    return emitter.Run();
}

} // namespace Shader::Backend::WGSL

/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include <sstream>
#include <limits>
#include <cassert>

#include <miopen/gcn_asm_utils.hpp>
#include <miopen/env.hpp>
#include <miopen/logger.hpp>
#include <miopen/handle.hpp>
#include <miopen/solver.hpp>
#include <miopen/generic_search.hpp>

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_GCN_ASM_DIRECT_1X1WRW_PERF_VALS)

namespace miopen {
namespace solver {

inline static bool Inc_1_2_4_8_16(int& v)
{
    assert(v == 1 || v == 2 || v == 4 || v == 8 || v == 16);
    if(v == 16)
    {
        v = 1;
        return true;
    }
    v = v * 2;
    return false;
}

inline static bool Is_1_2_4_8_16(const int& v)
{
    return v == 1 || v == 2 || v == 4 || v == 8 || v == 16;
}

inline static bool Inc_1_2_4(int& v)
{
    assert(v == 1 || v == 2 || v == 4);
    if(v == 4)
    {
        v = 1;
        return true;
    }
    v = v * 2;
    return false;
}

inline static bool Is_1_2_4(const int& v) { return v == 1 || v == 2 || v == 4; }

bool PerformanceConfigConvAsmBwdWrW1x1::SetNextValue()
{
    // Increment with wrap-around:
    do
    {
        if(!Inc_1_2_4_8_16(c_per_gpr))
            break;
        if(!Inc_1_2_4_8_16(c_mult))
            break;
        if(!Inc_1_2_4_8_16(k_per_gpr))
            break;
        if(!Inc_1_2_4_8_16(k_mult))
            break;
        if(++read_size <= 4)
            break;
        read_size = 1;
        if(!Inc_1_2_4(n_per_gpr))
            break;
        // All the fields (components) of performance confic have wrapped around.
        return false;
    } while(false);
    return true;
}

PerformanceConfigConvAsmBwdWrW1x1::PerformanceConfigConvAsmBwdWrW1x1(
    int c_per_gpr_, int c_mult_, int k_per_gpr_, int k_mult_, int read_size_, int n_per_gpr_)
    : c_per_gpr(c_per_gpr_),
      c_mult(c_mult_),
      k_per_gpr(k_per_gpr_),
      k_mult(k_mult_),
      read_size(read_size_),
      n_per_gpr(n_per_gpr_)
{
}

inline bool PerformanceConfigConvAsmBwdWrW1x1::
operator==(const PerformanceConfigConvAsmBwdWrW1x1& other) const
{
    // clang-format off
    return c_per_gpr == other.c_per_gpr
        && c_mult == other.c_mult
        && k_per_gpr == other.k_per_gpr
        && k_mult == other.k_mult
        && read_size == other.read_size
        && n_per_gpr == other.n_per_gpr; // clang-format on
}

bool PerformanceConfigConvAsmBwdWrW1x1::IsValidValue() const
{
    // clang-format off
    return Is_1_2_4_8_16(c_per_gpr)
        && Is_1_2_4_8_16(c_mult)
        && Is_1_2_4_8_16(k_per_gpr)
        && Is_1_2_4_8_16(k_mult)
        && (1 <= read_size && read_size <= 4)
        && Is_1_2_4(n_per_gpr); // clang-format on
}

bool PerformanceConfigConvAsmBwdWrW1x1::IsValid(const ConvolutionContext& config) const
{
    if(!IsValidValue())
        return false;
    assert((GetChunkSize() * c_per_gpr) == 16);
    if(!(k_per_gpr <= c_per_gpr))
        return false;
    if(c_mult > 1 || k_mult > 1)
    {
        assert(c_per_gpr * c_mult != 0);
        if(!(config.n_outputs % (c_per_gpr * c_mult) == 0))
            return false;
        assert(k_per_gpr * k_mult != 0);
        if(!(config.n_inputs % (k_per_gpr * k_mult) == 0))
            return false;
    }
    if(!(c_mult * k_mult * k_per_gpr + 9 + (c_mult + k_mult) * read_size * GetPipeDepth() <= 256))
    {
        return false;
    }
    return true;
}

void PerformanceConfigConvAsmBwdWrW1x1::EuristicInit(const ConvolutionContext& config)
{
    read_size = 4;
    n_per_gpr = (config.batch_sz >= 4 && (config.out_height * config.out_width) <= 128) ? 4 : 1;

    const auto c_k_256 = config.n_outputs * config.n_inputs / 256; // C*K/256
    if(c_k_256 < 2)
    {
        c_per_gpr = 1;
        c_mult    = 1;
        k_per_gpr = 1;
        k_mult    = 1;
    }
    else if(c_k_256 < (2 * 4))
    {
        c_per_gpr = 1;
        c_mult    = 2;
        k_per_gpr = 1;
        k_mult    = 2;
    }
    else if(c_k_256 < (2 * 4 * 4))
    {
        c_per_gpr = 2;
        c_mult    = 2;
        k_per_gpr = 2;
        k_mult    = 2;
    }
    else if(c_k_256 < (2 * 4 * 4 * 4))
    {
        c_per_gpr = 2;
        c_mult    = 4;
        k_per_gpr = 2;
        k_mult    = 4;
    }
    else
    {
        c_per_gpr = 4;
        c_mult    = 4;
        k_per_gpr = 4;
        k_mult    = 4;
    }

    if(!IsValid(config))
    {
        MIOPEN_LOG_I("!IsValid(): " << ToString() << ". Conservative re-init...");
        c_per_gpr = 4;
        c_mult    = 1;
        k_per_gpr = 4;
        k_mult    = 1;
        assert(IsValid(config));
    }
    MIOPEN_LOG_I(ToString());
}

std::string PerformanceConfigConvAsmBwdWrW1x1::ToString() const
{
    std::ostringstream ss;
    Serialize(ss);
    return ss.str();
}

PerformanceConfigConvAsmBwdWrW1x1
ConvAsmBwdWrW1x1::GetPerformanceConfig(const ConvolutionContext& params) const
{
    PerformanceConfigConvAsmBwdWrW1x1 pp;
    pp.EuristicInit(params);
    MIOPEN_LOG_I(pp.ToString());
    return pp;
}

bool ConvAsmBwdWrW1x1::IsValidPerformanceConfig(const ConvolutionContext& problem,
                                                const PerformanceConfigConvAsmBwdWrW1x1& c) const
{
    return c.IsValidValue() && c.IsValid(problem);
}

bool ConvAsmBwdWrW1x1::IsApplicable(const ConvolutionContext& params) const
{
    if(!params.use_asm_kernels)
    {
        return false;
    }

    if(params.n_passes)
    {
        return false;
    }

    if(!(params.rmv == rocm_meta_version::V3 || params.rmv == rocm_meta_version::AMDHSA_1_0))
    {
        return false;
    }
    const std::string name = params.GetStream().GetDeviceName();
    if(name.find("gfx8") == std::string::npos && name.find("gfx9") == std::string::npos)
    {
        return false;
    }
    assert(params.weights_layout.length() == 0); // _weights_layout is not supported yet
    // clang-format off
    bool ok = (params.pad0 == 0          // -q  pad_w
        && params.pad1 == 0             // -p  pad_h
        && params.kernel_stride0 <= 2   // -u  stride_w
        && params.kernel_stride1 <= 2   // -v  stride_h
        && params.kernel_stride0 == params.kernel_stride1
        && params.kernel_size0 == 1     // -x  S wei_w
        && params.kernel_size1 == 1     // -y  R wei_h
        && params.kernel_dilation0 == 1
        && params.kernel_dilation1 == 1
        && params.bias == 0
        && params.float_size == 32
        && params.in_layout == "NCHW");
    if(!ok)
    {
        return false; // Early exit to speed up the check.
    }
    // Check limits:
    auto h_w = static_cast<long>(params.out_height) * params.out_width;
    if(params.kernel_stride0 > 1 || params.kernel_stride1 > 1)
    {
        h_w = static_cast<long>(params.in_height) * params.in_width;
    }
    const auto r_s     = static_cast<long>(params.kernel_size1) * params.kernel_size0;
    const auto c_h_w   = static_cast<long>(params.n_outputs) * h_w;   // C*H*W
    const auto k_h_w   = static_cast<long>(params.n_inputs) * h_w;    // K*H*W
    const auto n_c_h_w = static_cast<long>(params.batch_sz) * c_h_w;  // N*C*H*W
    const auto n_k_h_w = static_cast<long>(params.batch_sz) * k_h_w;  // N*K*H*W
    const auto c_k_r_s = static_cast<long>(params.n_outputs) * params.n_inputs * r_s; // C*K*R*S
    ok = params.batch_sz < std::pow(2, 16)      // -n   N batch_size
         && params.n_outputs < std::pow(2, 16)  // -c   C input_channels
         && params.n_inputs < std::pow(2, 16)   // -k   K output_channels
         && c_h_w < std::pow(2, 24)
         && k_h_w < std::pow(2, 24)
         && n_c_h_w < std::pow(2, 29)
         && n_k_h_w < std::pow(2, 29)
         && c_k_r_s < std::pow(2, 29); // clang-format on
    return ok;
}

bool ConvAsmBwdWrW1x1::IsFast(const ConvolutionContext&) const { return true; }

static int divide_round_plus_inf(const int x, const int y)
{
    assert(x >= 0 && y > 0);
    if(x % y != 0)
        return x / y + 1;
    return x / y;
}

ConvSolution ConvAsmBwdWrW1x1::GetSolution(const ConvolutionContext& params,
                                           const PerformanceConfigConvAsmBwdWrW1x1& config,
                                           const bool disableConfigOverrideFromEnv) const
{
    ConvSolution result;
    std::ostringstream options;

    assert(params.pad1 == 0 && params.pad0 == 0);
    if(params.kernel_stride0 > 1 || params.kernel_stride1 > 1)
    {

        result.passes = 2;
    }
    else
    {
        result.passes = 1;
    }

    result.workspce_sz = 0;

    if(result.passes > 1 && (params.kernel_stride0 > 1 || params.kernel_stride1 > 1))
    {

        // subsampled input, in_height equals to image size after downsampling
        int in_batch_stride = params.in_stride * params.in_height * params.n_outputs;
        int write_unit      = (params.in_width % 4 == 0) ? 4 : (params.in_width % 3 == 0)
                                                              ? 3
                                                              : (params.in_width % 2 == 0) ? 2 : 1;
        int n_grp0_size0 = 256;

        const auto subsample_kernel_compilation_options =
            std::string(" -DMLO_GRP0_SZ0=") + std::to_string(n_grp0_size0) +
            std::string(" -DMLO_GRP0_SZ1=1 ") + std::string(" -DMLO_GRP0_SZ2=1 ") +
            std::string(" -DMLO_FILTER0_STRIDE0=") + std::to_string(params.kernel_stride0) +
            std::string(" -DMLO_FILTER0_STRIDE1=") + std::to_string(params.kernel_stride1) +
            std::string(" -DMLO_WRITE_UNIT=") + std::to_string(write_unit) +
            std::string(" -DMLO_OUT_CHANNEL_STRIDE=") + std::to_string(params.in_channel_stride) +
            std::string(" -DMLO_OUT_STRIDE=") + std::to_string(params.in_stride) +
            std::string(" -DMLO_IN_BATCH_STRIDE=") + std::to_string(in_batch_stride) +
            std::string(" -DMLO_IN0_BATCH_STRIDE=") + std::to_string(params.out_batch_stride) +
            std::string(" -DMLO_IN0_CHANNEL_STRIDE=") + std::to_string(params.out_channel_stride) +
            std::string(" -DMLO_IN0_STRIDE=") + std::to_string(params.out_stride) +
            params.general_compile_options;

        KernelInfo kernel;

        kernel.l_wk.push_back(n_grp0_size0);
        kernel.l_wk.push_back(1);
        kernel.l_wk.push_back(1);
        // output is number of subsampled input maps
        size_t gbl_wk0 = (in_batch_stride / write_unit);
        size_t gbl_wk1 = params.batch_sz;
        size_t gbl_wk2 = 1;

        kernel.g_wk.push_back(gbl_wk0);
        kernel.g_wk.push_back(gbl_wk1);
        kernel.g_wk.push_back(gbl_wk2);

        kernel.kernel_file = "MIOpenUtilKernels3.cl";

        kernel.kernel_name = "SubSample";

        kernel.comp_options = subsample_kernel_compilation_options;

        result.construction_params.push_back(kernel);

        assert(params.out_data_type == "FP16" || params.out_data_type == "FP32" ||
               params.out_data_type == "FP64");
        int data_len =
            (params.out_data_type == "FP16" ? 2 : (params.out_data_type == "FP32" ? 4 : 8));
        result.workspce_sz = in_batch_stride * params.batch_sz * data_len;

        // Note that params.in_height/params.in_width are swapped for output size when initialized
        // in mlo_internal.hpp for backward convolutions
        GenerateClangDefsym(options, "img_h", params.in_height); // H
        GenerateClangDefsym(options, "img_w", params.in_width);  // W
        GenerateClangDefsym(options, "stride_h", 1);
        GenerateClangDefsym(options, "stride_w", 1);
    }
    else
    {
        // Note that params.out_height/params.out_width are swapped for input size when initialized
        // in mlo_internal.hpp for backward convolutions
        GenerateClangDefsym(options, "img_h", params.out_height); // H
        GenerateClangDefsym(options, "img_w", params.out_width);  // W
        GenerateClangDefsym(options, "stride_h", params.kernel_stride1);
        GenerateClangDefsym(options, "stride_w", params.kernel_stride0);
    }

    GenerateClangDefsym(options, "batch_size", params.batch_sz); // N
    // Note that params.n_outputs and params.n_inputs are swapped for backward convolutions.
    GenerateClangDefsym(options, "input_channels", params.n_outputs); // C
    GenerateClangDefsym(options, "output_channels", params.n_inputs); // K
    GenerateClangDefsym(options, "wei_h", params.kernel_size1);       // R
    GenerateClangDefsym(options, "wei_w", params.kernel_size0);       // S
    GenerateClangDefsym(options, "pad_h", params.pad1);
    GenerateClangDefsym(options, "pad_w", params.pad0);
    GenerateClangDefsym(options, "weights_layout", 0);
    GenerateClangDefsym(options, "reverse_weights", 0);
    GenerateClangDefsym(
        options, "ROCM_METADATA_VERSION", (params.rmv == rocm_meta_version::V3) ? 3 : 4);
    // Perf tune:
    GenerateClangDefsym(options, "do_not_use_default_perf_params", 1);

    const PerformanceConfigConvAsmBwdWrW1x1* pcfg = &config;
    PerformanceConfigConvAsmBwdWrW1x1 fromEnv;
    if(!disableConfigOverrideFromEnv)
    {
        std::string s;
        const auto p_asciz = miopen::GetStringEnv(MIOPEN_DEBUG_GCN_ASM_DIRECT_1X1WRW_PERF_VALS{});
        if(p_asciz)
        {
            s = std::string(p_asciz);
            if(!s.empty()) // else nothing to parse.
            {
                if(!fromEnv.Deserialize(s) || !fromEnv.IsValid(params))
                {
                    MIOPEN_LOG_E("MIOPEN_DEBUG_GCN_ASM_DIRECT_1X1WRW_PERF_VALS: "
                                 "Bad format or invalid for the problem config: "
                                 << s);
                }
                else
                {
                    MIOPEN_LOG_I("Overridden from env: " << fromEnv.ToString());
                    pcfg = &fromEnv;
                }
            }
        }
    }
    GenerateClangDefsym(options, "n_per_gpr", pcfg->GetNPerGpr());
    GenerateClangDefsym(options, "pipe_depth", pcfg->GetPipeDepth());
    GenerateClangDefsym(options, "c_per_gpr", pcfg->GetCPerGpr());
    GenerateClangDefsym(options, "c_mult", pcfg->GetCMult());
    GenerateClangDefsym(options, "k_per_gpr", pcfg->GetKPerGpr());
    GenerateClangDefsym(options, "k_mult", pcfg->GetKMult());
    GenerateClangDefsym(options, "read_size", pcfg->GetReadSize());
    GenerateClangDefsym(options, "chunk_size", pcfg->GetChunkSize());
    GenerateClangDefsym(options, "hw_per_gpr", pcfg->GetHwPerGpr());

    KernelInfo kernel;

    kernel.comp_options = options.str();

    kernel.l_wk.clear(); // workgroupsize
    kernel.l_wk.push_back(64);
    kernel.l_wk.push_back(1);
    kernel.l_wk.push_back(1);

    kernel.g_wk.clear(); // gridsize
    kernel.g_wk.push_back(64);
    kernel.g_wk.push_back(
        divide_round_plus_inf(params.n_outputs, pcfg->GetCPerGpr() * pcfg->GetCMult()));
    kernel.g_wk.push_back(
        divide_round_plus_inf(params.n_inputs, pcfg->GetKPerGpr() * pcfg->GetKMult()));

    kernel.kernel_file = "conv1x1wrw.s";
    kernel.kernel_name = "gcnAsmConv1x1WrW";

    result.construction_params.push_back(kernel);

    return result;
}

int ConvAsmBwdWrW1x1::RunAndMeasureSolution(miopen::Handle& profile_h,
                                            Data_t bot_ocl_buf,
                                            Data_t top_ocl_buf,
                                            Data_t wei_ocl_buf,
                                            Data_t bias_ocl_buf,
                                            const ConvolutionContext& params,
                                            const ConvSolution& solution,
                                            float& elapsed_time) const
{
    assert(bias_ocl_buf == nullptr);
    (void)bias_ocl_buf;
    const KernelInfo k_info = solution.construction_params.back();
#ifdef NDEBUG
    try
#endif
    {
        elapsed_time = std::numeric_limits<float>::max();
        // ConvolutionContext::general_compile_options is for OpenCL kernels
        // and thus not applicable for assembly.
        auto kernel = profile_h.AddKernel("",
                                          "",
                                          k_info.kernel_file,
                                          k_info.kernel_name,
                                          k_info.l_wk,
                                          k_info.g_wk,
                                          k_info.comp_options);
        int unused       = 0;
        int* return_addr = nullptr;
        auto n_groups =
            static_cast<int>(params.GetStream().GetMaxComputeUnits()); // kernel needs int32

        kernel(params.batch_sz,   // N
               params.n_outputs,  // C
               params.out_height, // H
               params.out_width,  // W
               params.n_inputs,   // K
               n_groups,          // n_groups
               unused,
               unused,
               top_ocl_buf,
               wei_ocl_buf,
               bot_ocl_buf,
               return_addr);
        elapsed_time = profile_h.GetKernelTime();
    }
#ifdef NDEBUG
    catch(miopen::Exception&)
    {
        return -1;
    }
#endif
    return 0;
}

PerformanceConfigConvAsmBwdWrW1x1 ConvAsmBwdWrW1x1::Search(const ConvolutionContext& context) const
{
    return GenericSearch(*this, context);
}

} // namespace solver
} // namespace miopen

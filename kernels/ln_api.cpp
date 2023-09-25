#include <torch/extension.h>
#include "ATen/cuda/CUDAContext.h"
#include <c10/cuda/CUDAGuard.h>

#include "ln.h"

/*

Supported Type combinations:

input  residual   compute   weights   output
============================================
fp32     fp32      fp32      fp32      fp32
fp16     fp32      fp32      fp32      fp16
fp16     fp16      fp32      fp32      fp16
bf16     fp32      fp32      fp32      bf16
bf16     bf16      fp32      fp32      bf16
fp16     fp16      fp32      fp16      fp16
bf16     bf16      fp32      bf16      bf16

Remarks:
Output type = Input type
Compute always in FP32

*/

namespace layer_norm {

// Create registries and provide runtime versions of config hash functions.

FwdRegistry FWD_FUNCS;

////////////////////////////////////////////////////////////////////////////////////////////////////

uint32_t get_type_id(torch::Dtype dtype){
    if( dtype == torch::kFloat16 ) {
        return TypeId<fp16>::Value;
    } else if( dtype == torch::kBFloat16 ) {
        return TypeId<bf16>::Value;
    } else if( dtype == torch::kFloat32 ) {
        return TypeId<fp32>::Value;
    } else {
        TORCH_CHECK(false, "Type not supported: ", dtype);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

uint64_t get_key(torch::Dtype wtype, torch::Dtype itype, torch::Dtype rtype, torch::Dtype otype, torch::Dtype ctype, uint64_t hidden_size) {
    using namespace layer_norm;
    uint64_t type_key = get_type_id(wtype) | (get_type_id(itype) << 2) | (get_type_id(rtype) << 4) | (get_type_id(otype) << 6) | (get_type_id(ctype) << 8);
    uint64_t launcher_key = (type_key << 32) | hidden_size;
    return launcher_key;
}

}  // namespace layer_norm

////////////////////////////////////////////////////////////////////////////////////////////////////

layer_norm::FwdFunction & get_fwd_launcher(torch::Dtype wtype, torch::Dtype itype, torch::Dtype rtype, torch::Dtype otype, torch::Dtype ctype, uint32_t hidden_size) {
    auto iter = layer_norm::FWD_FUNCS.find(layer_norm::get_key(wtype, itype, rtype, otype, ctype, hidden_size));
    if( iter != layer_norm::FWD_FUNCS.end() ) {
        return iter->second;
    } else {
        TORCH_CHECK(false, "FWD: Unsupported hidden_size or types: ", hidden_size, wtype, itype, rtype, otype, ctype);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::vector<at::Tensor> dropout_add_ln_fwd(const at::Tensor &x0,      // Input: BxSxhidden_size
                                           c10::optional<const at::Tensor> &residual_,  // Residual: BxSxhidden_size
                                           const at::Tensor &gamma,   // hidden_size
                                           c10::optional<const at::Tensor> &beta_,   // hidden_size
                                           c10::optional<const at::Tensor> &rowscale_,      // BxS
                                           c10::optional<const at::Tensor> &colscale_,      // hidden_size
                                           c10::optional<const at::Tensor> &x0_subset_,      // BxS
                                           c10::optional<const at::Tensor> &z_subset_,      // BxS
                                           const float dropout_p,
                                           const float epsilon,
                                           const float rowscale_const,
                                           const int64_t z_numrows,
                                           c10::optional<at::Generator> gen_,
                                           bool residual_in_fp32=false,
                                           bool is_rms_norm=false
) {
    auto itype = x0.scalar_type();
    auto rtype = residual_.has_value()
        ? residual_.value().scalar_type()
        : (residual_in_fp32 ? torch::kFloat32 : x0.scalar_type());
    auto wtype = gamma.scalar_type();
    auto otype = itype;
    auto ctype = torch::kFloat32;
    auto mtype = torch::kUInt8;

    TORCH_CHECK(x0.is_cuda());
    TORCH_CHECK(gamma.is_cuda());

    TORCH_CHECK(x0.is_contiguous());
    // c10::IntArrayRef does not own the storage, so we need to construct a vector.
    // Otherwise just constructing IntArrayRef({blah}) will cause uninitialized memory because
    // blah is then deallocated.
    std::vector<int64_t> sizes_vec {!x0_subset_.has_value() ? x0.size(0) : x0_subset_.value().size(0), x0.size(1)};
    auto sizes = c10::IntArrayRef(sizes_vec);
    TORCH_CHECK(x0.dim() == 2);
    TORCH_CHECK(sizes.size() == 2);

    const int rows = sizes[0];
    const int cols = sizes[1];
    auto hidden_size = gamma.numel();
    TORCH_CHECK(hidden_size == cols);

    if (beta_.has_value()) {
        auto beta = beta_.value();
        TORCH_CHECK(beta.dtype() == wtype);
        TORCH_CHECK(beta.is_cuda());
        TORCH_CHECK(beta.is_contiguous());
        TORCH_CHECK(beta.sizes() == gamma.sizes());
    }

    if (residual_.has_value()) {
        auto residual = residual_.value();
        TORCH_CHECK(residual.is_cuda());
        TORCH_CHECK(residual.is_contiguous());
        TORCH_CHECK(residual.sizes() == sizes);
    }

    if (rowscale_.has_value()) {
        auto rowscale = rowscale_.value();
        TORCH_CHECK(rowscale.is_cuda());
        TORCH_CHECK(rowscale.is_contiguous());
        TORCH_CHECK(rowscale.sizes() == c10::IntArrayRef{rows});
        TORCH_CHECK(rowscale.dtype() == itype);
    }

    if (colscale_.has_value()) {
        auto colscale = colscale_.value();
        TORCH_CHECK(colscale.is_cuda());
        TORCH_CHECK(colscale.is_contiguous());
        TORCH_CHECK(colscale.sizes() == c10::IntArrayRef{cols});
        TORCH_CHECK(colscale.dtype() == wtype);
    }

    if (x0_subset_.has_value()) {
        auto x0_subset = x0_subset_.value();
        TORCH_CHECK(x0_subset.is_cuda());
        TORCH_CHECK(x0_subset.is_contiguous());
        TORCH_CHECK(x0_subset.sizes() == c10::IntArrayRef{rows});
        TORCH_CHECK(x0_subset.dtype() == torch::kInt32);

        TORCH_CHECK(z_subset_.has_value());
        auto z_subset = z_subset_.value();
        TORCH_CHECK(z_subset.is_cuda());
        TORCH_CHECK(z_subset.is_contiguous());
        TORCH_CHECK(z_subset.sizes() == c10::IntArrayRef{rows});
        TORCH_CHECK(z_subset.dtype() == torch::kInt32);
    }

    TORCH_CHECK((hidden_size % 8 == 0) && (hidden_size <= 8192));
    TORCH_CHECK(epsilon >= 0.f);

    // Otherwise the kernel will be launched from cuda:0 device
    // Cast to char to avoid compiler warning about narrowing
    at::cuda::CUDAGuard device_guard{(char)x0.get_device()};

    auto opts = x0.options();

    bool save_x = residual_.has_value() || (dropout_p > 0.f) || rowscale_.has_value() || colscale_.has_value() || x0_subset_.has_value() || (itype != rtype);
    at::Tensor x;
    if (save_x) { x = torch::empty(sizes, opts.dtype(rtype)); }
    at::Tensor dmask;
    if (dropout_p > 0.f) { dmask = torch::empty(x0.sizes(), opts.dtype(mtype)); };
    auto z = torch::empty(z_subset_.has_value() ? c10::IntArrayRef{z_numrows, cols} : sizes, opts.dtype(otype));

    auto mu = torch::empty({ rows }, opts.dtype(ctype));
    auto rsigma = torch::empty({ rows }, opts.dtype(ctype));

    layer_norm::LaunchParams<layer_norm::FwdParams> launch_params;

    launch_params.props = at::cuda::getCurrentDeviceProperties();
    launch_params.stream = at::cuda::getCurrentCUDAStream().stream();
    TORCH_CHECK(dropout_p < 1.f);
    launch_params.params.dropout_keep_p = 1.f - dropout_p;
    launch_params.params.residual = residual_.has_value() ? residual_.value().data_ptr() : nullptr;
    launch_params.params.rowscale = rowscale_.has_value() ? rowscale_.value().data_ptr() : nullptr;
    launch_params.params.colscale = colscale_.has_value() ? colscale_.value().data_ptr() : nullptr;
    launch_params.params.x0_subset = x0_subset_.has_value() ? x0_subset_.value().data_ptr() : nullptr;
    launch_params.params.z_subset = z_subset_.has_value() ? z_subset_.value().data_ptr() : nullptr;

    auto gen = at::get_generator_or_default<at::CUDAGeneratorImpl>(
        gen_, at::cuda::detail::getDefaultCUDAGenerator());

    auto round_multiple = [](int x, int m) { return (x + m - 1) / m * m; };
    const int multiple = hidden_size <= 1536 ? 256 : (hidden_size <= 3072 ? 512 : 1024);
    // Request the kernel launcher.
    auto launcher = get_fwd_launcher(wtype, itype, rtype, otype, ctype, round_multiple(hidden_size, multiple));

    // Set the kernel runtime parameters.
    layer_norm::FwdParams &params = launch_params.params;
    params.rows = rows;
    params.cols = cols;
    params.x0 = x0.data_ptr();
    params.x = save_x ? x.data_ptr() : nullptr;
    params.dmask = dropout_p > 0.f ? dmask.data_ptr() : nullptr;
    params.mu = mu.data_ptr();
    params.rs = rsigma.data_ptr();
    params.gamma = gamma.data_ptr();
    params.beta = beta_.has_value() ? beta_.value().data_ptr() : nullptr;
    params.z = z.data_ptr();
    params.epsilon = epsilon;
    params.dropout_scale = 1.f / (1.f - dropout_p);
    params.inverse_cols = 1.f / float(params.cols);
    params.rowscale_const = rowscale_const;
    params.is_rms_norm = is_rms_norm;

    // Query the kernel-specific launch parameters.
    launcher(launch_params, true);

    at::Tensor workspace, barrier;

    if (dropout_p > 0.f) {
        // number of times random will be generated per thread, to offset philox counter in thc random
        // state
        int64_t counter_offset = launch_params.elts_per_thread;

        // See Note [Acquire lock when using random generators]
        {
            std::lock_guard<std::mutex> lock(gen->mutex_);
            params.philox_args = gen->philox_cuda_state(counter_offset);
        }
    }

    if( launch_params.barrier_size > 0 ) {
        auto options = x0.options();
        barrier = torch::zeros(launch_params.barrier_size, options.dtype(torch::kInt32));
        workspace = torch::empty(launch_params.workspace_bytes, options.dtype(torch::kChar));
        params.workspace = workspace.data_ptr();
        params.barrier = barrier.data_ptr<int>();
    }

    // Launch the kernel.
    launcher(launch_params, false);

    return { z, x, dmask, mu, rsigma };
}
/*******************************************************************************
* Copyright 2016-2020 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"

#include "cpu/x64/lrn/jit_uni_lrn.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

using namespace dnnl::impl::format_tag;
using namespace dnnl::impl::status;
using namespace dnnl::impl::utils;

static constexpr int MAX_LOCAL_SIZE = 32u;

template <cpu_isa_t isa, data_type_t d_type>
jit_uni_lrn_fwd_t<isa, d_type>::jit_uni_lrn_fwd_t(const pd_t *apd)
    : primitive_t(apd), ker_(nullptr), ker_first_(nullptr), ker_last_(nullptr) {
    using namespace alg_kind;

    const int C = pd()->C();
    const int H = pd()->H();
    const int W = pd()->W();
    const int ls = pd()->desc()->local_size;
    float A = pd()->desc()->lrn_alpha / ls;
    const float K = pd()->desc()->lrn_k;
    const auto pk = pd()->desc()->prop_kind;
    const auto ak = pd()->desc()->alg_kind;
    const auto dat_tag = pd()->dat_tag_;

    if (dat_tag == nChw8c && ls == 5 && ak == lrn_across_channels) {
        ker_ = utils::make_unique<jit_uni_lrn_fwd_kernel<isa, d_type>>(
                nchw8c_across(H, W, 0), A, K, pk);
        ker_first_ = utils::make_unique<jit_uni_lrn_fwd_kernel<isa, d_type>>(
                nchw8c_across(H, W, -1), A, K, pk);
        ker_last_ = utils::make_unique<jit_uni_lrn_fwd_kernel<isa, d_type>>(
                nchw8c_across(H, W, +1), A, K, pk);
    } else if (one_of(dat_tag, nhwc, nChw8c, nChw16c)
            && ak == lrn_within_channel) {
        /* within channel, local_size (x) local_size */
        A /= ls; /* XXX: why? */
        ker_ = utils::make_unique<jit_uni_lrn_fwd_kernel<isa, d_type>>(
                within_config(H, W, C, ls, dat_tag), A, K, pk);
    } else if (dat_tag == nchw && ls == 5 && ak == lrn_across_channels) {
        ker_ = utils::make_unique<jit_uni_lrn_fwd_kernel<isa, d_type>>(
                nchw_across(C, H * W, 0), A, K, pk);
        const int remind = (H * W) % VECTOR_LENGTH;
        if (remind != 0) {
            ker_last_ = utils::make_unique<jit_uni_lrn_fwd_kernel<isa, d_type>>(
                    nchw_across(C, H * W, remind), A, K, pk);
        }
    } else {
        ker_ = utils::make_unique<jit_uni_lrn_fwd_kernel<isa, d_type>>(
                nhwc_across(C), A, K, pk);
    }
}

template <cpu_isa_t isa, data_type_t d_type>
jit_uni_lrn_fwd_t<isa, d_type>::~jit_uni_lrn_fwd_t() = default;

template <cpu_isa_t isa, data_type_t d_type>
void jit_uni_lrn_fwd_t<isa, d_type>::execute_forward(
        const exec_ctx_t &ctx) const {
    using namespace alg_kind;

    auto src = CTX_IN_MEM(const data_t *, DNNL_ARG_SRC);
    auto dst = CTX_OUT_MEM(data_t *, DNNL_ARG_DST);
    auto ws = CTX_OUT_MEM(data_t *, DNNL_ARG_WORKSPACE);

    const int N = pd()->MB();
    const int C = pd()->C();
    const int HW = pd()->H() * pd()->W();
    const int ls = pd()->desc()->local_size;

    const auto ak = pd()->desc()->alg_kind;
    const auto dat_tag = pd()->dat_tag_;
    const auto ker_first = ker_first_.get();
    const auto ker = ker_.get();
    const auto ker_last = ker_last_.get();

    if (dat_tag == nChw8c && ls == 5 && ak == lrn_across_channels) {
        parallel_nd(N, C / VECTOR_LENGTH, [&](int n, int c8) {
            jit_args_fwd_t args;
            args.src = &src[n * HW * C + c8 * HW * VECTOR_LENGTH];
            args.dst = &dst[n * HW * C + c8 * HW * VECTOR_LENGTH];
            args.scratch = &ws[n * HW * C + c8 * HW * VECTOR_LENGTH];
            if (c8 == 0)
                (*ker_first)(&args);
            else if (c8 == C / VECTOR_LENGTH - 1)
                (*ker_last)(&args);
            else
                (*ker)(&args);
        });
    } else if (one_of(dat_tag, nhwc, nChw8c, nChw16c)
            && ak == lrn_within_channel) {
        parallel_nd(N, C / VECTOR_LENGTH, [&](int n, int c) {
            const std::size_t offset = dat_tag == nhwc
                    ? n * HW * C + c * VECTOR_LENGTH
                    : n * HW * C + c * HW * VECTOR_LENGTH;
            jit_args_fwd_t args;
            args.src = &src[offset];
            args.dst = &dst[offset];
            args.scratch = &ws[offset];
            (*ker)(&args);
        });
    } else if (dat_tag == nchw && ls == 5 && ak == lrn_across_channels) {
        parallel_nd(N, (HW + VECTOR_LENGTH - 1) / VECTOR_LENGTH,
                [&](int n, int hw8) {
                    jit_args_fwd_t args;
                    args.src = &src[n * HW * C + hw8 * VECTOR_LENGTH];
                    args.dst = &dst[n * HW * C + hw8 * VECTOR_LENGTH];
                    args.scratch = &ws[n * HW * C + hw8 * VECTOR_LENGTH];
                    if ((hw8 + 1) * VECTOR_LENGTH > HW)
                        (*ker_last)(&args);
                    else
                        (*ker)(&args);
                });
    } else { // nhwc
        parallel_nd(N, HW, [&](int n, int hw) {
            jit_args_fwd_t args;
            args.src = &src[n * HW * C + hw * C];
            args.dst = &dst[n * HW * C + hw * C];
            args.scratch = &ws[n * HW * C + hw * C];
            (*ker)(&args);
        });
    }
}

template <cpu_isa_t isa, data_type_t d_type>
status_t jit_uni_lrn_fwd_t<isa, d_type>::pd_t::init(engine_t *engine) {
    using namespace prop_kind;
    using namespace alg_kind;

    const memory_desc_wrapper data_d(src_md());
    const bool ok = true && mayiuse(isa) && is_fwd()
            && everyone_is(d_type, data_d.data_type()) && !has_zero_dim_memory()
            && data_d.ndims() == 4 && data_d.dims()[1] % VECTOR_LENGTH == 0
            && data_d.dims()[1] >= 2 * VECTOR_LENGTH && desc()->lrn_beta == 0.75
            && attr()->has_default_values();
    if (!ok) return unimplemented;

    if (desc_.prop_kind == forward_training) ws_md_ = *src_md();

    dat_tag_ = memory_desc_matches_one_of_tag(
            *src_md(), nChw16c, nChw8c, nchw, nhwc);

    const int HW = data_d.dims()[2] * data_d.dims()[3];

    const bool args_ok_across = true && desc()->alg_kind == lrn_across_channels
            && desc()->local_size == 5 && one_of(dat_tag_, nChw8c, nchw, nhwc)
            && everyone_is(data_type::f32, data_d.data_type())
            /* SSE41: prevent loads smaller than the size of xmm registers,
         * otherwise it will result in an illegal memory read (seg-fault)
         * due to protected memory. */
            && IMPLICATION(isa == sse41 && dat_tag_ == nchw, HW >= 4)
            && isa != avx512_common;

    const int jit_max_local_size = 5; // bigger size triggers too big code size
    const bool args_ok_within = true && desc()->alg_kind == lrn_within_channel
            && desc()->local_size <= (jit_max_local_size <= MAX_LOCAL_SIZE
                               ? jit_max_local_size
                               : MAX_LOCAL_SIZE)
            && data_d.dims()[2] >= desc()->local_size
            && data_d.dims()[3] >= desc()->local_size
            && IMPLICATION(d_type == data_type::bf16, mayiuse(avx512_core))
            && (isa == avx512_common ? one_of(dat_tag_, nhwc, nChw16c)
                                     : one_of(dat_tag_, nhwc, nChw8c));

    return args_ok_across || args_ok_within ? success : unimplemented;
}

template <cpu_isa_t isa>
jit_uni_lrn_bwd_t<isa>::jit_uni_lrn_bwd_t(const pd_t *apd)
    : primitive_t(apd), ker_(nullptr), ker_first_(nullptr), ker_last_(nullptr) {
    using namespace alg_kind;
    const int C = pd()->C();
    const int H = pd()->H();
    const int W = pd()->W();
    const int ls = pd()->desc()->local_size;
    float A = pd()->desc()->lrn_alpha / ls;
    float B = pd()->desc()->lrn_beta;

    int use_h_parallelizm = 0; // XXX
    if (C / VECTOR_LENGTH == 1) {
        ker_ = new jit_uni_lrn_bwd_kernel_f32<isa>(
                nchw8c_across(H, W, 3), A, B, use_h_parallelizm);
    } else {
        ker_ = new jit_uni_lrn_bwd_kernel_f32<isa>(
                nchw8c_across(H, W, 0), A, B, use_h_parallelizm);
        ker_first_ = new jit_uni_lrn_bwd_kernel_f32<isa>(
                nchw8c_across(H, W, -1), A, B, use_h_parallelizm);
        ker_last_ = new jit_uni_lrn_bwd_kernel_f32<isa>(
                nchw8c_across(H, W, +1), A, B, use_h_parallelizm);
    }
}

template <cpu_isa_t isa>
jit_uni_lrn_bwd_t<isa>::~jit_uni_lrn_bwd_t() {
    delete ker_;
    delete ker_first_;
    delete ker_last_;
}

template <cpu_isa_t isa>
void jit_uni_lrn_bwd_t<isa>::execute_backward(const exec_ctx_t &ctx) const {
    auto src = CTX_IN_MEM(const data_t *, DNNL_ARG_SRC);
    auto diff_dst = CTX_IN_MEM(const data_t *, DNNL_ARG_DIFF_DST);
    auto ws = CTX_IN_MEM(const data_t *, DNNL_ARG_WORKSPACE);
    auto diff_src = CTX_OUT_MEM(data_t *, DNNL_ARG_DIFF_SRC);

    const int N = pd()->MB();
    const int C = pd()->C();
    const int H = pd()->H();
    const int W = pd()->W();

    int use_h_parallelizm = 0; // XXX
    if (use_h_parallelizm) {
        parallel_nd(N, C / VECTOR_LENGTH, H, [&](int n, int c8, int h) {
            auto offset = n * C * H * W + c8 * H * W * VECTOR_LENGTH
                    + h * W * VECTOR_LENGTH;
            jit_args_bwd_t args;
            args.src = &src[offset];
            args.diff_dst = &diff_dst[offset];
            args.scratch = &ws[offset];
            args.diff_src = &diff_src[offset];
            if (C / VECTOR_LENGTH == 1)
                (*ker_)(&args);
            else if (c8 == 0)
                (*ker_first_)(&args);
            else if (c8 == C / VECTOR_LENGTH - 1)
                (*ker_last_)(&args);
            else
                (*ker_)(&args);
        });
    } else {
        parallel_nd(N, C / VECTOR_LENGTH, [&](int n, int c8) {
            auto offset = n * C * H * W + c8 * H * W * VECTOR_LENGTH;
            jit_args_bwd_t args;
            args.src = &src[offset];
            args.diff_dst = &diff_dst[offset];
            args.scratch = &ws[offset];
            args.diff_src = &diff_src[offset];
            if (C / VECTOR_LENGTH == 1)
                (*ker_)(&args);
            else if (c8 == 0)
                (*ker_first_)(&args);
            else if (c8 == C / VECTOR_LENGTH - 1)
                (*ker_last_)(&args);
            else
                (*ker_)(&args);
        });
    }
}

template <cpu_isa_t isa>
status_t jit_uni_lrn_bwd_t<isa>::pd_t::init(engine_t *engine) {
    using namespace prop_kind;
    using namespace alg_kind;

    const memory_desc_wrapper data_d(src_md());
    bool ok = true && mayiuse(isa) && !is_fwd()
            && utils::everyone_is(data_type::f32, data_d.data_type())
            && set_default_formats_common() && !has_zero_dim_memory()
            && data_d.ndims() == 4 && data_d.dims()[1] % VECTOR_LENGTH == 0
            && desc()->lrn_beta == 0.75 && attr()->has_default_values();
    if (!ok) return unimplemented;

    ws_md_ = *src_md();
    if (!compare_ws(hint_fwd_pd_)) return unimplemented;

    dat_tag_ = memory_desc_matches_one_of_tag(*src_md(), nChw8c);

    bool args_ok_across = true && desc()->alg_kind == lrn_across_channels
            && desc()->local_size == 5 && utils::one_of(dat_tag_, nChw8c);

    return args_ok_across ? success : unimplemented;
}

template struct jit_uni_lrn_fwd_t<avx512_common, dnnl::impl::data_type::f32>;
template struct jit_uni_lrn_fwd_t<avx512_common, dnnl::impl::data_type::bf16>;
template struct jit_uni_lrn_fwd_t<avx2, dnnl::impl::data_type::f32>;
template struct jit_uni_lrn_fwd_t<sse41, dnnl::impl::data_type::f32>;
template struct jit_uni_lrn_bwd_t<avx2>;

} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s

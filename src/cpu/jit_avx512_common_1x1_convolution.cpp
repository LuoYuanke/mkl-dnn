/*******************************************************************************
* Copyright 2017 Intel Corporation
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

#include "mkldnn_types.h"

#include "c_types_map.hpp"
#include "jit_avx512_common_1x1_convolution.hpp"
#include "utils.hpp"
#include "mkldnn_thread.hpp"
#include "type_helpers.hpp"

#include "jit_generator.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::status;
using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;

namespace {
template <typename T, typename U>
void balance2D(U nthr, U ithr, T ny, T &ny_start, T &ny_end,
    T nx, T &nx_start, T &nx_end, T nx_divider)
{
    const T grp_size = utils::div_up(nthr, nx_divider);
    const T grp_count = utils::div_up(nthr, grp_size);

    T grp = ithr / grp_size;
    T grp_ithr = ithr % grp_size;
    T grp_nthr = grp_size;
    T first_grps = nthr % grp_count;
    if (first_grps > 0 && grp >= first_grps) {
        ithr -= first_grps * grp_size;
        grp_nthr--;
        grp = ithr / grp_nthr + first_grps;
        grp_ithr = ithr % grp_nthr;
    }
    balance211(nx, grp_count, grp, nx_start, nx_end);
    balance211(ny, grp_nthr, grp_ithr, ny_start, ny_end);
}
}
/* convolution forward */

template <bool with_relu, data_type_t src_type, data_type_t wei_type,
        data_type_t dst_type>
void _jit_avx512_common_1x1_convolution_fwd_t
    <with_relu, src_type, wei_type, dst_type>::execute_forward()
{
    auto src = reinterpret_cast<const src_data_t *>(this->input_memory(0));
    auto weights =
        reinterpret_cast<const wei_data_t *>(this->input_memory(1));
    auto bias = reinterpret_cast<const dst_data_t *>(this->input_memory(2));
    auto dst = reinterpret_cast<dst_data_t *>(this->memory());

    const memory_desc_wrapper src_d(conf_.src_pd());
    const memory_desc_wrapper dst_d(conf_.dst_pd());
    const memory_desc_wrapper weights_d(conf_.weights_pd(0));

    const auto &jcp = kernel_->jcp;

    const int work_amount = jcp.mb * jcp.ngroups * jcp.nb_bcast;

    const int stride_h = conf_.cdesc()->strides[0];
    const int stride_w = conf_.cdesc()->strides[1];
    const int pad_t = conf_.cdesc()->padding[0][0];
    const int pad_l = conf_.cdesc()->padding[0][1];

    auto step = [](int default_step, int remaining, int tail_step) {
        assert(default_step <= tail_step);
        return remaining < tail_step ? remaining : default_step;
    };

#   pragma omp parallel
    {
        int ithr = omp_get_thread_num(), nthr = omp_get_num_threads();

        jit_1x1_conv_call_s p = {};

        rtus_driver_t<avx512_common>::call_params_t rp = {};

        const int nb_oc = jcp.nb_load;
        const int nb_ic = jcp.nb_reduce;
        const int nb_ic_blocking = jcp.nb_reduce_blocking;
        const int os_block = jcp.bcast_block;

        int bcast_start{0}, bcast_end{0}, ocb_start{0}, ocb_end{0};
        balance2D(nthr, ithr, work_amount, bcast_start, bcast_end,
            jcp.nb_load, ocb_start, ocb_end, jcp.load_grp_count);

        auto init_bcast = [&](int iwork, int &n, int &g, int &bcast_step,
            int &oh, int &ow, int &ih, int &iw)
        {
            int osb{0};
            nd_iterator_init(iwork, n, jcp.mb, g, jcp.ngroups, osb,
                jcp.nb_bcast);
            bcast_step = step(jcp.nb_bcast_blocking, jcp.nb_bcast - osb,
                    jcp.nb_bcast_blocking_max);
            bcast_step = nstl::min(bcast_step, bcast_end - iwork);

            const int os = osb * os_block;
            oh = os / jcp.ow;
            ow = os % jcp.ow;

            ih = nstl::max(oh * stride_h - pad_t, 0);
            iw = nstl::max(ow * stride_w - pad_l, 0);
            rp.iw_start = iw;

            p.bcast_dim = this_block_size(os, jcp.os,
                bcast_step * os_block);
            rp.os = p.bcast_dim;
        };

        auto init_load = [&](int ocb, int &load_step)
        {
            load_step = step(jcp.nb_load_blocking, ocb_end - ocb,
                jcp.nb_load_blocking_max);
            p.load_dim = this_block_size(ocb * jcp.oc_block,
                ocb_end * jcp.oc_block, load_step * jcp.oc_block);
        };

        auto init_reduce = [&](int icb)
        {
            const int nb_ic_blocking_step =
                nstl::min(icb + nb_ic_blocking, nb_ic) - icb;
            p.reduce_pos_flag = 0
                | (icb == 0 ? FLAG_REDUCE_FIRST : 0)
                | (icb + nb_ic_blocking_step >= nb_ic
                        ? FLAG_REDUCE_LAST : 0);

            p.reduce_dim = this_block_size(icb * jcp.ic_block,
                jcp.ic, nb_ic_blocking_step * jcp.ic_block);
            rp.icb = p.reduce_dim / jcp.reduce_block;
        };

        auto inner_ker = [&](int ocb, int icb, int n, int g, int oh, int ow,
            int ih, int iw)
        {

            const int _ocb = g * nb_oc + ocb;
            const size_t dst_off = dst_d.blk_off(n, _ocb, oh, ow);

            p.output_data = &dst[dst_off];
            p.bias_data = &bias[_ocb * jcp.oc_block];
            p.load_data = &weights[conf_.with_groups()
                ? weights_d.blk_off(g, ocb, icb)
                : weights_d.blk_off(ocb, icb)];

            const int _icb = g * nb_ic + icb;
            if (conf_.rtus_.reduce_src_) {
                rp.ws = scratch_ + ithr * ws_per_thread_
                    + _icb * jcp.is * jcp.ic_block;
                if (ocb == ocb_start) {
                    rp.src = src + src_d.blk_off(n, _icb, ih, iw);
                    rtus_driver_->ker_(&rp);
                }
                p.bcast_data = rp.ws;
            } else
                p.bcast_data = src + src_d.blk_off(n, _icb, ih, iw);

            kernel_->jit_ker(&p);
        };

        if (jcp.loop_order == loop_rlb) {
            for (int icb = 0; icb < nb_ic; icb += nb_ic_blocking) {
                init_reduce(icb);
                int ocb = ocb_start;
                while (ocb < ocb_end) {
                    int load_step;
                    init_load(ocb, load_step);
                    int iwork = bcast_start;
                    while (iwork < bcast_end) {
                        int n, g, bcast_step, oh, ow, ih, iw;
                        init_bcast(iwork, n, g, bcast_step, oh, ow, ih, iw);
                        inner_ker(ocb, icb, n, g, oh, ow, ih, iw);
                        iwork += bcast_step;
                    }
                    ocb += load_step;
                }
            }
        } else if (jcp.loop_order == loop_lbr) {
            int ocb = ocb_start;
            while (ocb < ocb_end) {
                int load_step;
                init_load(ocb, load_step);
                int iwork = bcast_start;
                while (iwork < bcast_end) {
                    int n, g, bcast_step, oh, ow, ih, iw;
                    init_bcast(iwork, n, g, bcast_step, oh, ow, ih, iw);
                    for (int icb = 0; icb < nb_ic; icb += nb_ic_blocking) {
                        init_reduce(icb);
                        inner_ker(ocb, icb, n, g, oh, ow, ih, iw);
                    }
                    iwork += bcast_step;
                }
                ocb += load_step;
            }
        } else if (jcp.loop_order == loop_rbl) {
            for (int icb = 0; icb < nb_ic; icb += nb_ic_blocking) {
                init_reduce(icb);
                int iwork = bcast_start;
                while (iwork < bcast_end) {
                    int n, g, bcast_step, oh, ow, ih, iw;
                    init_bcast(iwork, n, g, bcast_step, oh, ow, ih, iw);
                    int ocb = ocb_start;
                    while (ocb < ocb_end) {
                        int load_step;
                        init_load(ocb, load_step);
                        inner_ker(ocb, icb, n, g, oh, ow, ih, iw);
                        ocb += load_step;
                    }
                    iwork += bcast_step;
                }
            }
        } else if (jcp.loop_order == loop_blr) {
            int iwork = bcast_start;
            while (iwork < bcast_end) {
                int n, g, bcast_step, oh, ow, ih, iw;
                init_bcast(iwork, n, g, bcast_step, oh, ow, ih, iw);
                int ocb = ocb_start;
                while (ocb < ocb_end) {
                    int load_step;
                    init_load(ocb, load_step);
                    for (int icb = 0; icb < nb_ic; icb += nb_ic_blocking) {
                        init_reduce(icb);
                        inner_ker(ocb, icb, n, g, oh, ow, ih, iw);
                    }
                    ocb += load_step;
                }
                iwork += bcast_step;
            }
        } else {
            assert(!"unsupported loop order");
        }
    }
}

template struct _jit_avx512_common_1x1_convolution_fwd_t<true, data_type::f32>;
template struct _jit_avx512_common_1x1_convolution_fwd_t<false, data_type::f32>;
template struct _jit_avx512_common_1x1_convolution_fwd_t<false, data_type::s16,
    data_type::s16, data_type::s32>;
template struct _jit_avx512_common_1x1_convolution_fwd_t<true, data_type::s16,
    data_type::s16, data_type::s32>;
/* convolution backward wtr data */

template <data_type_t diff_dst_type, data_type_t wei_type,
    data_type_t diff_src_type>
void _jit_avx512_common_1x1_convolution_bwd_data_t
    <diff_dst_type, wei_type, diff_src_type>::execute_backward_data()
{
    auto diff_dst = reinterpret_cast<const diff_dst_data_t *>
        (this->input_memory(0));
    auto weights = reinterpret_cast<const wei_data_t *>
        (this->input_memory(1));
    auto diff_src = reinterpret_cast<diff_src_data_t *>(this->memory());

    const memory_desc_wrapper diff_dst_d(conf_.diff_dst_pd());
    const memory_desc_wrapper weights_d(conf_.weights_pd(0));
    const memory_desc_wrapper diff_src_d(conf_.diff_src_pd());

    const auto &jcp = kernel_->jcp;

    // TODO (Roma): remove this restriction
    assert(jcp.stride_w == 1 && jcp.stride_h == 1);

    const int stride_h = conf_.desc()->strides[0];
    const int stride_w = conf_.desc()->strides[1];
    const int pad_t = conf_.desc()->padding[0][0];
    const int pad_l = conf_.desc()->padding[0][1];

    const int nb_ic = jcp.nb_load;
    const int nb_oc = jcp.nb_reduce;
    const int os_block = jcp.bcast_block;
    const int nb_oc_blocking = jcp.nb_reduce_blocking;

    const int work_amount = jcp.mb * jcp.ngroups * jcp.nb_bcast;

    auto step = [](int default_step, int remaining, int tail_step) {
        assert(default_step <= tail_step);
        return remaining < tail_step ? remaining : default_step;
    };

#   pragma omp parallel
    {
        int ithr = omp_get_thread_num(), nthr = omp_get_num_threads();

        jit_1x1_conv_call_s p = {};
        rtus_driver_t<avx512_common>::call_params_t rp = {};

        int bcast_start{0}, bcast_end{0}, icb_start{0}, icb_end{0};
        balance2D(nthr, ithr, work_amount, bcast_start, bcast_end,
            jcp.nb_load, icb_start, icb_end, jcp.load_grp_count);

        bool reduce_outer = (jcp.loop_order == loop_rbl
            || jcp.loop_order == loop_rlb);
        int nboc_outer = reduce_outer ? nb_oc : 1;
        int ocb_outer_step = reduce_outer ? nb_oc_blocking : 1;

        int nboc_inner = reduce_outer ? 1 : nb_oc;
        int ocb_inner_step = reduce_outer ? 1 : nb_oc_blocking;

        for (int ocb_outer = 0; ocb_outer < nboc_outer;
            ocb_outer += ocb_outer_step) {
            size_t cur_ocb_outer =
                nstl::min(ocb_outer + ocb_outer_step, nboc_outer) - ocb_outer;

            int load_step = 0;
            for (int icb = icb_start; icb < icb_end; icb += load_step) {
                load_step = step(jcp.nb_load_blocking, jcp.nb_load - icb,
                        jcp.nb_load_blocking_max);

                p.load_dim = this_block_size(icb * jcp.ic_block,
                    icb_end * jcp.ic_block, load_step * jcp.ic_block);
                rp.icb = p.load_dim / jcp.ic_block;

                int bcast_step;
                for (int iwork = bcast_start; iwork < bcast_end;
                    iwork += bcast_step)
                {
                    int n{0}, g{0}, osb{0};
                    nd_iterator_init(iwork, n, jcp.mb, g, jcp.ngroups, osb,
                            jcp.nb_bcast);

                    bcast_step = step(jcp.nb_bcast_blocking, jcp.nb_bcast - osb,
                            jcp.nb_bcast_blocking_max);
                    bcast_step = nstl::min(bcast_step, bcast_end - iwork);

                    const int os = osb * os_block;
                    p.bcast_dim = this_block_size(os, jcp.os,
                            bcast_step * os_block);
                    rp.os = p.bcast_dim;

                    const int oh = os / jcp.ow;
                    const int ow = os % jcp.ow;
                    const int ih = nstl::max(oh * stride_h - pad_t, 0);
                    const int iw = nstl::max(ow * stride_w - pad_l, 0);
                    rp.iw_start = iw;

                    const int _icb = g * nb_ic + icb;
                    rp.src = diff_src + diff_src_d.blk_off(n, _icb, ih, iw);

                    if (conf_.rtus_.reduce_src_) {
                        rp.ws = scratch_ + ithr * ws_per_thread_;
                        p.output_data = rp.ws;
                    } else
                        p.output_data = rp.src;

                    for (int ocb_inner = 0; ocb_inner < nboc_inner;
                        ocb_inner += ocb_inner_step) {
                        int cur_ocb_inner =
                            nstl::min(ocb_inner + ocb_inner_step, nboc_inner) -
                            ocb_inner;

                        int ocb = reduce_outer ? ocb_outer : ocb_inner;
                        int nb_oc_blocking_step = reduce_outer
                            ? cur_ocb_outer : cur_ocb_inner;
                        const int _ocb = g * nb_oc + ocb;
                        size_t diff_dst_off =
                            diff_dst_d.blk_off(n, _ocb, oh, ow);
                        p.bcast_data = &diff_dst[diff_dst_off];

                        p.load_data = &weights[conf_.with_groups()
                            ? weights_d.blk_off(g, ocb, icb)
                            : weights_d.blk_off(ocb, icb)];

                        p.reduce_pos_flag = ocb == 0 ? FLAG_REDUCE_FIRST : 0;

                        p.reduce_dim = this_block_size(ocb * jcp.oc_block,
                            jcp.oc, nb_oc_blocking_step * jcp.oc_block);

                        kernel_->jit_ker(&p);
                    }
                    if (conf_.rtus_.reduce_src_)
                        rtus_driver_->ker_(&rp);
                }
            }
        }
    }
}

template struct _jit_avx512_common_1x1_convolution_bwd_data_t<data_type::f32>;
template struct _jit_avx512_common_1x1_convolution_bwd_data_t<data_type::s16,
    data_type::s16, data_type::s32>;

/* convolution backward wtr weights */

jit_avx512_common_1x1_convolution_bwd_weights_t
::jit_avx512_common_1x1_convolution_bwd_weights_t(
        const pd_t *pd, const input_vector &inputs,
        const output_vector &outputs)
    : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd), kernel_(nullptr)
    , rtus_driver_(nullptr), ws_per_thread_(0), scratch_(nullptr)
{
    kernel_ = new jit_avx512_common_1x1_conv_kernel(conf_.jcp_);

    const auto &jcp = kernel_->jcp;

    const int ic_block = jcp.bcast_block;
    const int nb_ic = jcp.nb_bcast;
    const int nb_ic_blocking = jcp.nb_bcast_blocking;
    const int bcast_work = utils::div_up(nb_ic, nb_ic_blocking);

    const int oc_block = jcp.load_block;
    const int nb_oc = jcp.nb_load;
    const int nb_oc_blocking = jcp.nb_load_blocking;
    const int load_work = utils::div_up(nb_oc, nb_oc_blocking);

    const int job_size
        = nb_oc_blocking * nb_ic_blocking * ic_block * oc_block;
    const int njobs_x = bcast_work;
    const int njobs_y = jcp.ngroups * load_work;

    const int simd_w = 16;
    const int max_threads = omp_get_max_threads();
    const size_t max_buffer_size = max_threads * job_size * simd_w;

    reducer_weights_ = new cpu_reducer_2d_t<data_type::f32>(
            reduce_balancer_t(max_threads, job_size, njobs_y * njobs_x,
                jcp.mb * jcp.nb_reduce, max_buffer_size),
            job_size / nb_oc_blocking, nb_oc_blocking, ic_block,
            nb_ic * ic_block * oc_block, nb_oc, false);

    const int nworkers = reducer_weights_->balancer_.ngroups_
        * reducer_weights_->balancer_.nthr_per_group_;
    const int bias_len = jcp.oc * jcp.ngroups;
    reducer_bias_ = !conf_.with_bias() ? nullptr
        : new cpu_reducer_t<data_type::f32>(reduce_balancer_t(nworkers,
                    bias_len, 1, nworkers, 0));

    init_rtus_driver<avx512_common>(this);
}

void jit_avx512_common_1x1_convolution_bwd_weights_t::execute_backward_weights()
{
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto diff_weights = reinterpret_cast<data_t *>(this->memory(0));
    auto diff_bias = reinterpret_cast<data_t *>(this->memory(1));

    const memory_desc_wrapper diff_dst_d(conf_.diff_dst_pd());
    const memory_desc_wrapper src_d(conf_.src_pd());
    const memory_desc_wrapper diff_weights_d(conf_.diff_weights_pd(0));
    const memory_desc_wrapper diff_bias_d(conf_.diff_weights_pd(1));

    const auto &jcp = kernel_->jcp;

    // TODO (Roma): remove this restriction
    assert(jcp.stride_w == 1 && jcp.stride_h == 1);

    const int simd_w = 16;

    const int nb_ic = jcp.nb_bcast;
    const int nb_ic_blocking = jcp.nb_bcast_blocking;
    const int bcast_work = div_up(nb_ic, nb_ic_blocking);

    const int nb_oc = jcp.nb_load;
    const int nb_oc_blocking = jcp.nb_load_blocking;
    const int load_work = div_up(nb_oc, nb_oc_blocking);

    const int sp_nb = jcp.nb_reduce;
    const int mb_sp_work = jcp.mb * sp_nb;

    const int stride_h = conf_.desc()->strides[0];
    const int stride_w = conf_.desc()->strides[1];
    const int pad_t = conf_.desc()->padding[0][0];
    const int pad_l = conf_.desc()->padding[0][1];

    auto step = [](int default_step, int remaining, int tail_step) {
        assert(default_step <= tail_step);
        return remaining < tail_step ? remaining : default_step;
    };

    auto oc_ic_sp_loop = [=](int sp_b_start, int sp_b_end, bool first_image,
            data_t *store_to, size_t store_to_ld, data_t *d_bias,
            const data_t *diff_dst, const data_t *src, int ithr,
            bool ic_is_zero) {
        jit_1x1_conv_call_s p = {};
        rtus_driver_t<avx512_common>::call_params_t rp = {};

        p.output_stride = store_to_ld * jcp.typesize_out;

        int oc_b_step = 0;
        for (int oc_b = 0; oc_b < nb_oc_blocking; oc_b += oc_b_step) {
            oc_b_step = step(jcp.nb_load_blocking,
                            nb_oc_blocking - oc_b, jcp.nb_load_blocking_max);
            p.load_dim = oc_b_step * jcp.oc_block;
            p.bias_data = d_bias + oc_b * jcp.oc_block;

            int ic_b_step = 0;
            for (int ic_b = 0; ic_b < nb_ic_blocking; ic_b += ic_b_step) {
                ic_b_step = step(jcp.nb_bcast_blocking,
                            nb_ic_blocking - ic_b, jcp.nb_bcast_blocking_max);
                p.bcast_dim = ic_b_step * jcp.ic_block;
                rp.icb = p.bcast_dim / jcp.ic_block;

                p.output_data = store_to + oc_b * store_to_ld
                    + ic_b * jcp.ic_block * jcp.oc_block;

                p.reduce_pos_flag = ic_is_zero && ic_b == 0
                    ? FLAG_IC_FIRST : 0;

                /* spatial reduction */
                int sp_b_step = 0;
                for (int sp_b = sp_b_start; sp_b < sp_b_end; sp_b += sp_b_step)
                {
                    sp_b_step = step(jcp.nb_reduce_blocking, sp_b_end - sp_b,
                            jcp.nb_reduce_blocking_max);
                    p.reduce_dim = sp_b_step * jcp.reduce_block;
                    rp.os = p.reduce_dim;

                    if (sp_b == sp_b_start && first_image)
                        p.reduce_pos_flag |= FLAG_REDUCE_FIRST;
                    else
                        p.reduce_pos_flag &= ~FLAG_REDUCE_FIRST;

                    int sp = sp_b * jcp.reduce_block;
                    p.load_data = diff_dst
                            + (oc_b * jcp.reduce_dim + sp) * jcp.oc_block;

                    if (conf_.rtus_.reduce_src_) {
                        const int oh = sp / jcp.ow;
                        const int ow = sp % jcp.ow;

                        const int ih = nstl::max(oh * stride_h - pad_t, 0);
                        const int iw = nstl::max(ow * stride_w - pad_l, 0);
                        rp.iw_start = iw;

                        rp.ws = scratch_ + ithr * ws_per_thread_
                            + (ic_b * jcp.is + sp) * jcp.ic_block;
                        rp.src = src
                            + ih * src_d.blocking_desc().strides[0][2]
                            + iw * src_d.blocking_desc().strides[0][3];

                        if (oc_b == 0)
                            rtus_driver_->ker_(&rp);

                        p.bcast_data = rp.ws;
                    } else
                        p.bcast_data = src
                            + (ic_b * jcp.reduce_dim + sp) * jcp.ic_block;

                    kernel_->jit_ker(&p);
                }
            }
        }
    };

    auto ker = [&](const int ithr, const int nthr) {
        auto rw = this->reducer_weights_;
        auto rb = this->reducer_bias_;
        assert(nthr == rw->balancer_.nthr_);

        const int w_njobs = rw->balancer_.ithr_njobs(ithr);
        if (w_njobs == 0) return;

        data_t *loc_diff_bias = nullptr;
        if (diff_bias) {
            assert(ithr <
                    rw->balancer_.ngroups_ * rw->balancer_.nthr_per_group_);
            loc_diff_bias = rb->get_local_ptr(ithr, diff_bias);
            for (int i = 0; i < rb->balancer_.job_size_; ++i)
                loc_diff_bias[i] = 0;
        }

        /* setup: independent work (oc, ic) */
        const int w_job_start = rw->balancer_.ithr_job_off(ithr);
        int g{0}, load_i{0}, bcast_i{0};
        nd_iterator_init(w_job_start, g, jcp.ngroups, load_i, load_work,
                bcast_i, bcast_work);

        /* setup: reduction work (mb, sp) */
        int mb_sp_b_start{0}, mb_sp_b_end{0};
        balance211(mb_sp_work, rw->balancer_.nthr_per_group_,
                rw->balancer_.id_in_group(ithr), mb_sp_b_start, mb_sp_b_end);
        int img_start{0}, sp_b_start{0};
        nd_iterator_init(mb_sp_b_start, img_start, jcp.mb, sp_b_start, sp_nb);

        /* independent work */
        for (int iwork = 0; iwork < w_njobs; ++iwork) {
            const int oc_b = nb_oc_blocking * load_i;
            const int ic_b = nb_ic_blocking * bcast_i;

            const int _ic_b = g * nb_ic + ic_b;
            const int _oc_b = g * nb_oc + oc_b;

            data_t *store_to;
            size_t store_to_ld;

            if (rw->balancer_.nthr_per_group_ == 1 ||
                    (rw->balancer_.master(ithr) && rw->master_uses_dst_)) {
                const size_t off = conf_.with_groups()
                    ? diff_weights_d.blk_off(g, oc_b, ic_b)
                    : diff_weights_d.blk_off(oc_b, ic_b);
                store_to = &diff_weights[off];
                store_to_ld = jcp.ic * jcp.oc_block;
            } else {
                const size_t off = iwork * rw->balancer_.job_size_;
                store_to = &rw->get_local_ptr(ithr, nullptr)[off];
                store_to_ld = nb_ic_blocking * jcp.ic_block * jcp.oc_block;
            }

            /* reduction work */
            int img = img_start;
            int sp_b = sp_b_start;
            int sp_b_step = 0;
            for (int mb_sp_b = mb_sp_b_start; mb_sp_b < mb_sp_b_end;
                    mb_sp_b += sp_b_step) {
                sp_b_step = nstl::min(sp_nb - sp_b, mb_sp_b_end - mb_sp_b);

                const bool first_image = img == img_start;
                oc_ic_sp_loop(sp_b, sp_b + sp_b_step, first_image, store_to,
                        store_to_ld, &loc_diff_bias[_oc_b * jcp.oc_block],
                        &diff_dst[diff_dst_d.blk_off(img, _oc_b)],
                        &src[src_d.blk_off(img, _ic_b)], ithr, ic_b == 0);

                sp_b = 0;
                img += 1;
            }
            nd_iterator_step(g, jcp.ngroups, load_i, load_work, bcast_i,
                             bcast_work);
        }

        rw->reduce(ithr, diff_weights);
        if (diff_bias)
            rb->reduce(ithr, diff_bias);
    };

#   pragma omp parallel
    {
        int ithr = omp_get_thread_num();
        int nthr = omp_get_num_threads();
        ker(ithr, nthr);
    }
}

}
}
}

/*******************************************************
 * Copyright (c) 2022, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#include <Param.hpp>
#include <backend.hpp>
#include <common/Binary.hpp>
#include <common/Transform.hpp>
#include <common/dispatch.hpp>
#include <common/half.hpp>
//#include <copy.hpp>?
#include <debug_oneapi.hpp>
#include <err_oneapi.hpp>
#include <kernel/reduce_config.hpp>
#include <math.hpp>
#include <memory.hpp>

#include <iostream>
#include <memory>
#include <vector>

namespace oneapi {

/*
TODO: port half
__device__ auto operator*(float lhs, __half rhs) -> __half {
    return __float2half(lhs * __half2float(rhs));
}

__device__ auto operator/(__half lhs, float rhs) -> __half {
    return __float2half(__half2float(lhs) / rhs);
}
*/

namespace kernel {

template<typename T, int dimensions>
using local_accessor =
    sycl::accessor<T, dimensions, sycl::access::mode::read_write,
                   sycl::access::target::local>;

template<typename To, typename Tw>
void stable_mean(To *lhs, Tw *l_wt, To rhs, Tw r_wt) {
    if (((*l_wt) != (Tw)0) || (r_wt != (Tw)0)) {
        Tw l_scale = (*l_wt);
        (*l_wt) += r_wt;
        l_scale = l_scale / (*l_wt);

        Tw r_scale = r_wt / (*l_wt);
        (*lhs)     = (l_scale * *lhs) + (r_scale * rhs);
    }
}

template<typename Ti, typename Tw, typename To, uint dim, uint DIMY>
class meanDimKernelSMEM {
   public:
    meanDimKernelSMEM(sycl::accessor<To> out, KParam oInfo,
                      sycl::accessor<Tw> owt, KParam owInfo,
                      sycl::accessor<Ti> in, KParam iInfo,
                      sycl::accessor<Tw> iwt, KParam iwInfo, uint groups_x,
                      uint groups_y, uint offset_dim,
                      local_accessor<compute_t<To>, 1> s_val,
                      local_accessor<compute_t<Tw>, 1> s_idx,
                      sycl::stream debug, bool input_weight, bool output_weight)
        : out_(out)
        , oInfo_(oInfo)
        , owt_(owt)
        , owInfo_(owInfo)
        , in_(in)
        , iInfo_(iInfo)
        , iwt_(iwt)
        , iwInfo_(iwInfo)
        , groups_x_(groups_x)
        , groups_y_(groups_y)
        , offset_dim_(offset_dim)
        , s_val_(s_val)
        , s_idx_(s_idx)
        , debug_(debug)
        , input_weight_(input_weight)
        , output_weight_(output_weight) {}

    void operator()(sycl::nd_item<2> it) const {
        sycl::group g   = it.get_group();
        const uint lidx = it.get_local_id(0);
        const uint lidy = it.get_local_id(1);
        const uint lid  = lidy * g.get_local_range(0) + lidx;

        const uint zid        = g.get_group_id(0) / groups_x_;
        const uint wid        = g.get_group_id(1) / groups_y_;
        const uint groupIdx_x = g.get_group_id(0) - (groups_x_)*zid;
        const uint groupIdx_y = g.get_group_id(1) - (groups_y_)*wid;
        const uint xid        = groupIdx_x * g.get_local_range(0) + lidx;
        const uint yid =
            groupIdx_y;  // yid  of output. updated for input later.

        uint ids[4] = {xid, yid, zid, wid};

        const Ti *iptr = in_.get_pointer();
        To *optr       = out_.get_pointer();

        uint ooffset = ids[3] * oInfo_.strides[3] + ids[2] * oInfo_.strides[2] +
                       ids[1] * oInfo_.strides[1] + ids[0];
        // There is only one element per block for out
        // There are blockDim.y elements per block for in
        // Hence increment ids[dim] just after offseting out and before
        // offsetting in
        optr += ooffset;

        const uint blockIdx_dim = ids[dim];
        ids[dim]                = ids[dim] * g.get_local_range(1) + lidy;

        uint ioffset = ids[3] * iInfo_.strides[3] + ids[2] * iInfo_.strides[2] +
                       ids[1] * iInfo_.strides[1] + ids[0];
        iptr += ioffset;

        const Tw *iwptr;
        Tw *owptr;

        if (output_weight_) owptr = owt_.get_pointer() + ooffset;
        if (input_weight_) iwptr = iwt_.get_pointer() + ioffset;

        const uint id_dim_in   = ids[dim];
        const uint istride_dim = iInfo_.strides[dim];

        bool is_valid = (ids[0] < iInfo_.dims[0]) &&
                        (ids[1] < iInfo_.dims[1]) &&
                        (ids[2] < iInfo_.dims[2]) && (ids[3] < iInfo_.dims[3]);

        common::Transform<Ti, compute_t<To>, af_add_t> transform;

        compute_t<To> val    = common::Binary<compute_t<To>, af_add_t>::init();
        compute_t<Tw> weight = common::Binary<compute_t<Tw>, af_add_t>::init();

        if (is_valid && id_dim_in < iInfo_.dims[dim]) {
            val = transform(*iptr);
            if (iwptr != NULL) {
                weight = *iwptr;
            } else {
                weight = (Tw)1;
            }
        }

        const uint id_dim_in_start =
            id_dim_in + offset_dim_ * g.get_local_range(0);

        for (int id = id_dim_in_start; is_valid && (id < iInfo_.dims[dim]);
             id += offset_dim_ * g.get_local_range(0)) {
            iptr = iptr + offset_dim_ * g.get_local_range(0) * istride_dim;
            if (input_weight_) {
                iwptr =
                    iwptr + offset_dim_ * g.get_local_range(0) * istride_dim;
                stable_mean(&val, &weight, transform(*iptr),
                            compute_t<Tw>(*iwptr));
            } else {
                // Faster version of stable_mean when iwptr is NULL
                val    = val + (transform(*iptr) - val) / (weight + (Tw)1);
                weight = weight + (Tw)1;
            }
        }

        s_val_[lid] = val;
        s_idx_[lid] = weight;

        compute_t<To> *s_vptr = s_val_.get_pointer() + lid;
        compute_t<Tw> *s_iptr = s_idx_.get_pointer() + lid;
        group_barrier(g);

        if (DIMY == 8) {
            if (lidy < 4) {
                stable_mean(s_vptr, s_iptr, s_vptr[THREADS_X * 4],
                            s_iptr[THREADS_X * 4]);
            }
            group_barrier(g);
        }

        if (DIMY >= 4) {
            if (lidy < 2) {
                stable_mean(s_vptr, s_iptr, s_vptr[THREADS_X * 2],
                            s_iptr[THREADS_X * 2]);
            }
            group_barrier(g);
        }

        if (DIMY >= 2) {
            if (lidy < 1) {
                stable_mean(s_vptr, s_iptr, s_vptr[THREADS_X * 1],
                            s_iptr[THREADS_X * 1]);
            }
            group_barrier(g);
        }

        if (lidy == 0 && is_valid && (blockIdx_dim < oInfo_.dims[dim])) {
            *optr = *s_vptr;
            if (output_weight_) *owptr = *s_iptr;
        }
    }

   protected:
    sycl::accessor<To> out_;
    sycl::accessor<Tw> owt_;
    sycl::accessor<Ti> in_;
    sycl::accessor<Tw> iwt_;
    KParam oInfo_, owInfo_, iInfo_, iwInfo_;
    const uint groups_x_, groups_y_, offset_dim_;
    local_accessor<compute_t<To>, 1> s_val_;
    local_accessor<compute_t<Tw>, 1> s_idx_;
    bool input_weight_, output_weight_;
    sycl::stream debug_;
};

template<typename Ti, typename Tw, typename To, int dim>
void mean_dim_launcher(Param<To> out, Param<Tw> owt, Param<Ti> in,
                       Param<Tw> iwt, const uint threads_y,
                       const dim_t blocks_dim[4]) {
    sycl::range<2> local(THREADS_X, threads_y);
    sycl::range<2> global(blocks_dim[0] * blocks_dim[2] * local[0],
                          blocks_dim[1] * blocks_dim[3] * local[1]);

    sycl::buffer<Tw, 1> empty(sycl::range<1>{1});
    getQueue().submit([&](sycl::handler &h) {
        auto out_acc = out.data->get_access(h);
        auto in_acc  = in.data->get_access(h);

        sycl::stream debug_stream(2048 * 2048, 2048, h);

        auto s_val = local_accessor<compute_t<To>, 1>(THREADS_PER_BLOCK, h);
        auto s_idx = local_accessor<compute_t<Tw>, 1>(THREADS_PER_BLOCK, h);

        bool input_weight = ((iwt.info.dims[0] * iwt.info.dims[1] *
                              iwt.info.dims[2] * iwt.info.dims[3]) != 0);

        bool output_weight = ((owt.info.dims[0] * owt.info.dims[1] *
                               owt.info.dims[2] * owt.info.dims[3]) != 0);

        auto owt_acc =
            (output_weight) ? owt.data->get_access(h) : empty.get_access(h);
        auto iwt_acc =
            (input_weight) ? iwt.data->get_access(h) : empty.get_access(h);

        switch (threads_y) {
            case 8:
                h.parallel_for(sycl::nd_range<2>(global, local),
                               meanDimKernelSMEM<Ti, Tw, To, dim, 8>(
                                   out_acc, out.info, owt_acc, owt.info, in_acc,
                                   in.info, iwt_acc, iwt.info, blocks_dim[0],
                                   blocks_dim[1], blocks_dim[dim], s_val, s_idx,
                                   debug_stream, input_weight, output_weight));
                break;
            case 4:
                h.parallel_for(sycl::nd_range<2>(global, local),
                               meanDimKernelSMEM<Ti, Tw, To, dim, 8>(
                                   out_acc, out.info, owt_acc, owt.info, in_acc,
                                   in.info, iwt_acc, iwt.info, blocks_dim[0],
                                   blocks_dim[1], blocks_dim[dim], s_val, s_idx,
                                   debug_stream, input_weight, output_weight));
                break;
            case 2:
                h.parallel_for(sycl::nd_range<2>(global, local),
                               meanDimKernelSMEM<Ti, Tw, To, dim, 8>(
                                   out_acc, out.info, owt_acc, owt.info, in_acc,
                                   in.info, iwt_acc, iwt.info, blocks_dim[0],
                                   blocks_dim[1], blocks_dim[dim], s_val, s_idx,
                                   debug_stream, input_weight, output_weight));
                break;
            case 1:
                h.parallel_for(sycl::nd_range<2>(global, local),
                               meanDimKernelSMEM<Ti, Tw, To, dim, 8>(
                                   out_acc, out.info, owt_acc, owt.info, in_acc,
                                   in.info, iwt_acc, iwt.info, blocks_dim[0],
                                   blocks_dim[1], blocks_dim[dim], s_val, s_idx,
                                   debug_stream, input_weight, output_weight));
                break;
        }
    });
    ONEAPI_DEBUG_FINISH(getQueue());
}

template<typename Ti, typename Tw, typename To, int dim>
void mean_dim(Param<To> out, Param<Ti> in, Param<Tw> iwt) {
    uint threads_y = std::min(THREADS_Y, nextpow2(in.info.dims[dim]));
    uint threads_x = THREADS_X;

    dim_t blocks_dim[] = {divup(in.info.dims[0], threads_x), in.info.dims[1],
                          in.info.dims[2], in.info.dims[3]};

    blocks_dim[dim] = divup(in.info.dims[dim], threads_y * REPEAT);

    Array<To> tmpOut = createEmptyArray<To>(dim4());
    Array<Tw> tmpWt  = createEmptyArray<Tw>(dim4());

    if (blocks_dim[dim] > 1) {
        dim4 dims(4, out.info.dims);
        dims[dim] = blocks_dim[dim];
        tmpOut    = createEmptyArray<To>(dims);
        tmpWt     = createEmptyArray<Tw>(dims);
    } else {
        tmpOut = createParamArray(out, false);
    }

    mean_dim_launcher<Ti, Tw, To, dim>(tmpOut, tmpWt, in, iwt, threads_y,
                                       blocks_dim);

    if (blocks_dim[dim] > 1) {
        blocks_dim[dim] = 1;

        Array<Tw> owt = createEmptyArray<Tw>(dim4());
        mean_dim_launcher<To, Tw, To, dim>(out, owt, tmpOut, tmpWt, threads_y,
                                           blocks_dim);
    }
}

// Calculate mean along the first dimension. If wt is an empty Param, use
// weight as 1 and treat it as count. If owt is empty Param, do not write
// temporary reduced counts/weights to it.
template<typename Ti, typename Tw, typename To>
class meanFirstKernelSMEM {
   public:
    meanFirstKernelSMEM(sycl::accessor<To> out, KParam oInfo,
                        sycl::accessor<Tw> owt, KParam owInfo,
                        sycl::accessor<Ti> in, KParam iInfo,
                        sycl::accessor<Tw> iwt, KParam iwInfo, const uint DIMX,
                        const uint groups_x, const uint groups_y,
                        const uint repeat,
                        local_accessor<compute_t<To>, 1> s_val,
                        local_accessor<compute_t<Tw>, 1> s_idx,
                        sycl::stream debug, bool input_weight,
                        bool output_weight)
        : out_(out)
        , oInfo_(oInfo)
        , owt_(owt)
        , owInfo_(owInfo)
        , in_(in)
        , iInfo_(iInfo)
        , iwt_(iwt)
        , iwInfo_(iwInfo)
        , DIMX_(DIMX)
        , groups_x_(groups_x)
        , groups_y_(groups_y)
        , repeat_(repeat)
        , s_val_(s_val)
        , s_idx_(s_idx)
        , debug_(debug)
        , input_weight_(input_weight)
        , output_weight_(output_weight) {}

    void operator()(sycl::nd_item<2> it) const {
        sycl::group g   = it.get_group();
        const uint lidx = it.get_local_id(0);
        const uint lidy = it.get_local_id(1);
        const uint lid  = lidy * DIMX_ + lidx;

        const uint zid        = g.get_group_id(0) / groups_x_;
        const uint wid        = g.get_group_id(1) / groups_y_;
        const uint groupIdx_x = g.get_group_id(0) - (groups_x_)*zid;
        const uint groupIdx_y = g.get_group_id(1) - (groups_y_)*wid;
        const uint xid = groupIdx_x * g.get_local_range(0) * repeat_ + lidx;
        const uint yid = groupIdx_y * g.get_local_range(1) + lidy;

        const Ti *iptr = in_.get_pointer();
        To *optr       = out_.get_pointer();

        iptr += wid * iInfo_.strides[3] + zid * iInfo_.strides[2] +
                yid * iInfo_.strides[1];
        optr += wid * oInfo_.strides[3] + zid * oInfo_.strides[2] +
                yid * oInfo_.strides[1];

        const Tw *iwptr;
        Tw *owptr;
        if (input_weight_)
            iwptr = iwt_.get_pointer() + wid * iwInfo_.strides[3] +
                    zid * iwInfo_.strides[2] + yid * iwInfo_.strides[1];

        if (output_weight_)
            owptr = owt_.get_pointer() + wid * oInfo_.strides[3] +
                    zid * oInfo_.strides[2] + yid * oInfo_.strides[1];

        bool cond = (yid < iInfo_.dims[1] && zid < iInfo_.dims[2] &&
                     wid < iInfo_.dims[3]);

        int lim = sycl::min((dim_t)(xid + repeat_ * DIMX_), iInfo_.dims[0]);

        common::Transform<Ti, compute_t<To>, af_add_t> transform;

        compute_t<To> val    = common::Binary<compute_t<To>, af_add_t>::init();
        compute_t<Tw> weight = common::Binary<compute_t<Tw>, af_add_t>::init();

        if (cond && xid < lim) {
            val = transform(iptr[xid]);
            if (input_weight_) {
                weight = iwptr[xid];
            } else {
                weight = (Tw)1;
            }
        }

        if (input_weight_) {
            for (int id = xid + DIMX_; cond && id < lim; id += DIMX_) {
                stable_mean(&val, &weight, transform(iptr[id]),
                            compute_t<Tw>(iwptr[id]));
            }
        } else {
            for (int id = xid + DIMX_; cond && id < lim; id += DIMX_) {
                // Faster version of stable_mean when iwptr is NULL
                val    = val + (transform(iptr[id]) - val) / (weight + (Tw)1);
                weight = weight + (Tw)1;
            }
        }

        s_val_[lid] = val;
        s_idx_[lid] = weight;
        group_barrier(g);

        compute_t<To> *s_vptr = s_val_.get_pointer() + lidy * DIMX_;
        compute_t<Tw> *s_iptr = s_idx_.get_pointer() + lidy * DIMX_;

        if (DIMX_ == 256) {
            if (lidx < 128) {
                stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 128],
                            s_iptr[lidx + 128]);
            }
            group_barrier(g);
        }

        if (DIMX_ >= 128) {
            if (lidx < 64) {
                stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 64],
                            s_iptr[lidx + 64]);
            }
            group_barrier(g);
        }

        if (DIMX_ >= 64) {
            if (lidx < 32) {
                stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 32],
                            s_iptr[lidx + 32]);
            }
            group_barrier(g);
        }

        if (lidx < 16) {
            stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 16],
                        s_iptr[lidx + 16]);
        }
        group_barrier(g);

        if (lidx < 8) {
            stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 8],
                        s_iptr[lidx + 8]);
        }
        group_barrier(g);

        if (lidx < 4) {
            stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 4],
                        s_iptr[lidx + 4]);
        }
        group_barrier(g);

        if (lidx < 2) {
            stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 2],
                        s_iptr[lidx + 2]);
        }
        group_barrier(g);

        if (lidx < 1) {
            stable_mean(s_vptr + lidx, s_iptr + lidx, s_vptr[lidx + 1],
                        s_iptr[lidx + 1]);
        }
        group_barrier(g);

        if (cond && lidx == 0) {
            optr[groupIdx_x] = s_vptr[0];
            if (output_weight_) owptr[groupIdx_x] = s_iptr[0];
        }
    }

   protected:
    sycl::accessor<To> out_;
    sycl::accessor<Tw> owt_;
    sycl::accessor<Ti> in_;
    sycl::accessor<Tw> iwt_;
    KParam oInfo_, owInfo_, iInfo_, iwInfo_;
    const uint DIMX_, groups_x_, groups_y_, repeat_;
    local_accessor<compute_t<To>, 1> s_val_;
    local_accessor<compute_t<Tw>, 1> s_idx_;
    bool input_weight_, output_weight_;
    sycl::stream debug_;
};

template<typename Ti, typename Tw, typename To>
void mean_first_launcher(Param<To> out, Param<Tw> owt, Param<Ti> in,
                         Param<Tw> iwt, const uint groups_x,
                         const uint groups_y, const uint threads_x) {
    sycl::range<2> local(threads_x, THREADS_PER_BLOCK / threads_x);
    sycl::range<2> global(groups_x * in.info.dims[2] * local[0],
                          groups_y * in.info.dims[3] * local[1]);

    uint repeat = divup(in.info.dims[0], (groups_x * threads_x));

    sycl::buffer<Tw, 1> empty(sycl::range<1>{1});
    getQueue().submit([&](sycl::handler &h) {
        auto out_acc = out.data->get_access(h);
        auto in_acc  = in.data->get_access(h);

        sycl::stream debug_stream(2048 * 2048, 2048, h);

        auto s_val = local_accessor<compute_t<To>, 1>(THREADS_PER_BLOCK, h);
        auto s_idx = local_accessor<compute_t<Tw>, 1>(THREADS_PER_BLOCK, h);

        bool input_weight = ((iwt.info.dims[0] * iwt.info.dims[1] *
                              iwt.info.dims[2] * iwt.info.dims[3]) != 0);

        bool output_weight = ((owt.info.dims[0] * owt.info.dims[1] *
                               owt.info.dims[2] * owt.info.dims[3]) != 0);

        auto owt_acc =
            (output_weight) ? owt.data->get_access(h) : empty.get_access(h);
        auto iwt_acc =
            (input_weight) ? iwt.data->get_access(h) : empty.get_access(h);

        h.parallel_for(
            sycl::nd_range<2>(global, local),
            meanFirstKernelSMEM<Ti, Tw, To>(
                out_acc, out.info, owt_acc, owt.info, in_acc, in.info, iwt_acc,
                iwt.info, threads_x, groups_x, groups_y, repeat, s_val, s_idx,
                debug_stream, input_weight, output_weight));
    });
    ONEAPI_DEBUG_FINISH(getQueue());
}

template<typename Ti, typename Tw, typename To>
void mean_first(Param<To> out, Param<Ti> in, Param<Tw> iwt) {
    uint threads_x = nextpow2(std::max(32u, (uint)in.info.dims[0]));
    threads_x      = std::min(threads_x, THREADS_PER_BLOCK);
    uint threads_y = THREADS_PER_BLOCK / threads_x;

    uint blocks_x = divup(in.info.dims[0], threads_x * REPEAT);
    uint blocks_y = divup(in.info.dims[1], threads_y);

    Array<To> tmpOut = createEmptyArray<To>(dim4());
    Array<Tw> tmpWt  = createEmptyArray<Tw>(dim4());
    if (blocks_x > 1) {
        tmpOut = createEmptyArray<To>(
            {blocks_x, in.info.dims[1], in.info.dims[2], in.info.dims[3]});
        tmpWt = createEmptyArray<Tw>(
            {blocks_x, in.info.dims[1], in.info.dims[2], in.info.dims[3]});
    } else {
        tmpOut = createParamArray(out, false);
    }

    mean_first_launcher<Ti, Tw, To>(tmpOut, tmpWt, in, iwt, blocks_x, blocks_y,
                                    threads_x);

    if (blocks_x > 1) {
        Param<Tw> owt;
        owt.data = nullptr;
        mean_first_launcher<To, Tw, To>(out, owt, tmpOut, tmpWt, 1, blocks_y,
                                        threads_x);
    }
}

template<typename Ti, typename Tw, typename To>
void mean_weighted(Param<To> out, Param<Ti> in, Param<Tw> iwt, int dim) {
    switch (dim) {
        case 0: return mean_first<Ti, Tw, To>(out, in, iwt);
        case 1: return mean_dim<Ti, Tw, To, 1>(out, in, iwt);
        case 2: return mean_dim<Ti, Tw, To, 2>(out, in, iwt);
        case 3: return mean_dim<Ti, Tw, To, 3>(out, in, iwt);
    }
}

template<typename Ti, typename Tw, typename To>
void mean(Param<To> out, Param<Ti> in, int dim) {
    Param<Tw> dummy_weight;
    mean_weighted<Ti, Tw, To>(out, in, dummy_weight, dim);
}

template<typename T, typename Tw>
T mean_all_weighted(Param<T> in, Param<Tw> iwt) {
    int in_elements =
        in.info.dims[0] * in.info.dims[1] * in.info.dims[2] * in.info.dims[3];
    // FIXME: Use better heuristics to get to the optimum number
    if (in_elements > 4096) {
        bool in_is_linear = (in.info.strides[0] == 1);
        bool wt_is_linear = (iwt.info.strides[0] == 1);
        for (int k = 1; k < 4; k++) {
            in_is_linear &= (in.info.strides[k] ==
                             (in.info.strides[k - 1] * in.info.dims[k - 1]));
            wt_is_linear &= (iwt.info.strides[k] ==
                             (iwt.info.strides[k - 1] * iwt.info.dims[k - 1]));
        }

        if (in_is_linear && wt_is_linear) {
            in.info.dims[0] = in_elements;
            for (int k = 1; k < 4; k++) {
                in.info.dims[k]    = 1;
                in.info.strides[k] = in_elements;
            }

            for (int k = 0; k < 4; k++) {
                iwt.info.dims[k]    = in.info.dims[k];
                iwt.info.strides[k] = in.info.strides[k];
            }
        }

        uint threads_x = nextpow2(std::max(32u, (uint)in.info.dims[0]));
        threads_x      = std::min(threads_x, THREADS_PER_BLOCK);
        uint threads_y = THREADS_PER_BLOCK / threads_x;

        uint blocks_x = divup(in.info.dims[0], threads_x * REPEAT);
        uint blocks_y = divup(in.info.dims[1], threads_y);

        Array<T> tmpOut = createEmptyArray<T>(
            {blocks_x, in.info.dims[1], in.info.dims[2], in.info.dims[3]});
        Array<Tw> tmpWt = createEmptyArray<Tw>(
            {blocks_x, in.info.dims[1], in.info.dims[2], in.info.dims[3]});

        int tmp_elements = tmpOut.elements();

        mean_first_launcher<T, Tw, T>(tmpOut, tmpWt, in, iwt, blocks_x,
                                      blocks_y, threads_x);

        std::vector<T> h_ptr(tmp_elements);
        std::vector<Tw> h_wptr(tmp_elements);
        sycl::buffer hBuffer(h_ptr.data(), {tmp_elements},
                             {sycl::property::buffer::use_host_ptr()});
        sycl::buffer hwBuffer(h_wptr.data(), {tmp_elements},
                              {sycl::property::buffer::use_host_ptr()});

        auto e1 = getQueue().submit([&](sycl::handler &h) {
            auto acc_in =
                tmpOut.getData()->get_access(h, sycl::range{tmp_elements});
            auto acc_out = hBuffer.get_access();
            h.copy(acc_in, acc_out);
        });
        auto e2 = getQueue().submit([&](sycl::handler &h) {
            auto acc_in =
                tmpWt.getData()->get_access(h, sycl::range{tmp_elements});
            auto acc_out = hwBuffer.get_access();
            h.copy(acc_in, acc_out);
        });
        e1.wait();
        e2.wait();

        compute_t<T> val     = static_cast<compute_t<T>>(h_ptr[0]);
        compute_t<Tw> weight = static_cast<compute_t<Tw>>(h_wptr[0]);

        for (int i = 1; i < tmp_elements; i++) {
            stable_mean(&val, &weight, compute_t<T>(h_ptr[i]),
                        compute_t<Tw>(h_wptr[i]));
        }

        return static_cast<T>(val);
    } else {
        std::vector<T> h_ptr(in_elements);
        std::vector<Tw> h_wptr(in_elements);

        sycl::buffer hBuffer(h_ptr.data(), {in_elements},
                             {sycl::property::buffer::use_host_ptr()});
        sycl::buffer hwBuffer(h_wptr.data(), {in_elements},
                              {sycl::property::buffer::use_host_ptr()});

        auto e1 = getQueue().submit([&](sycl::handler &h) {
            auto acc_in  = in.data->get_access(h, sycl::range{in_elements});
            auto acc_out = hBuffer.get_access();
            h.copy(acc_in, acc_out);
        });
        auto e2 = getQueue().submit([&](sycl::handler &h) {
            auto acc_in  = iwt.data->get_access(h, sycl::range{in_elements});
            auto acc_out = hwBuffer.get_access();
            h.copy(acc_in, acc_out);
        });
        e1.wait();
        e2.wait();

        compute_t<T> val     = static_cast<compute_t<T>>(h_ptr[0]);
        compute_t<Tw> weight = static_cast<compute_t<Tw>>(h_wptr[0]);
        for (int i = 1; i < in_elements; i++) {
            stable_mean(&val, &weight, compute_t<T>(h_ptr[i]),
                        compute_t<Tw>(h_wptr[i]));
        }

        return static_cast<T>(val);
    }
}

template<typename Ti, typename Tw, typename To>
To mean_all(Param<Ti> in) {
    using std::unique_ptr;
    int in_elements =
        in.info.dims[0] * in.info.dims[1] * in.info.dims[2] * in.info.dims[3];
    bool is_linear = (in.info.strides[0] == 1);
    for (int k = 1; k < 4; k++) {
        is_linear &= (in.info.strides[k] ==
                      (in.info.strides[k - 1] * in.info.dims[k - 1]));
    }

    // FIXME: Use better heuristics to get to the optimum number
    if (in_elements > 4096 || !is_linear) {
        if (is_linear) {
            in.info.dims[0] = in_elements;
            for (int k = 1; k < 4; k++) {
                in.info.dims[k]    = 1;
                in.info.strides[k] = in_elements;
            }
        }

        uint threads_x = nextpow2(std::max(32u, (uint)in.info.dims[0]));
        threads_x      = std::min(threads_x, THREADS_PER_BLOCK);
        uint threads_y = THREADS_PER_BLOCK / threads_x;

        uint blocks_x = divup(in.info.dims[0], threads_x * REPEAT);
        uint blocks_y = divup(in.info.dims[1], threads_y);

        dim4 outDims(blocks_x, in.info.dims[1], in.info.dims[2],
                     in.info.dims[3]);

        Array<To> tmpOut = createEmptyArray<To>(outDims);
        Array<Tw> tmpCt  = createEmptyArray<Tw>(outDims);

        Param<Tw> iwt;
        mean_first_launcher<Ti, Tw, To>(tmpOut, tmpCt, in, iwt, blocks_x,
                                        blocks_y, threads_x);

        int tmp_elements = tmpOut.elements();
        std::vector<To> h_ptr(tmp_elements);
        std::vector<Tw> h_cptr(tmp_elements);

        sycl::buffer hBuffer(h_ptr.data(), {tmp_elements},
                             {sycl::property::buffer::use_host_ptr()});
        sycl::buffer hcBuffer(h_cptr.data(), {tmp_elements},
                              {sycl::property::buffer::use_host_ptr()});

        auto e1 = getQueue().submit([&](sycl::handler &h) {
            auto acc_in =
                tmpOut.getData()->get_access(h, sycl::range{tmp_elements});
            auto acc_out = hBuffer.get_access();
            h.copy(acc_in, acc_out);
        });
        auto e2 = getQueue().submit([&](sycl::handler &h) {
            auto acc_in =
                tmpCt.getData()->get_access(h, sycl::range{tmp_elements});
            auto acc_out = hcBuffer.get_access();
            h.copy(acc_in, acc_out);
        });
        e1.wait();
        e2.wait();

        compute_t<To> val    = static_cast<compute_t<To>>(h_ptr[0]);
        compute_t<Tw> weight = static_cast<compute_t<Tw>>(h_cptr[0]);

        for (int i = 1; i < tmp_elements; i++) {
            stable_mean(&val, &weight, compute_t<To>(h_ptr[i]),
                        compute_t<Tw>(h_cptr[i]));
        }

        return static_cast<To>(val);
    } else {
        std::vector<Ti> h_ptr(in_elements);
        sycl::buffer outBuffer(h_ptr.data(), {in_elements},
                               {sycl::property::buffer::use_host_ptr()});

        getQueue()
            .submit([&](sycl::handler &h) {
                auto acc_in  = in.data->get_access(h);
                auto acc_out = outBuffer.get_access();
                h.copy(acc_in, acc_out);
            })
            .wait();

        common::Transform<Ti, compute_t<To>, af_add_t> transform;
        compute_t<Tw> count = static_cast<compute_t<Tw>>(1);

        compute_t<To> val    = transform(h_ptr[0]);
        compute_t<Tw> weight = count;
        for (int i = 1; i < in_elements; i++) {
            stable_mean(&val, &weight, transform(h_ptr[i]), count);
        }

        return static_cast<To>(val);
    }
}

}  // namespace kernel
}  // namespace oneapi
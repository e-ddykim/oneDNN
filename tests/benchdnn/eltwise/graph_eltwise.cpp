/*******************************************************************************
* Copyright 2021 Intel Corporation
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

#include "eltwise/graph_eltwise.hpp"
#include "compare.hpp"

namespace benchdnnext {
namespace eltwise {

eltwise_graph_prb_t::spec_t::spec_t(const ::eltwise::prb_t *prb) {
    dims = prb->dims;
    eltwise_dt = convert_dt(prb->dt);

    op_kind = convert_alg_kind(attr_t::post_ops_t::kind2dnnl_kind(prb->alg));

    alpha = prb->alpha;
    beta = prb->beta;
}

fill_status_t eltwise_graph_prb_t::handle_main_op_() {
    using op = dnnl::graph::op;

    const std::string SRC {"eltwise_src"};
    const std::string DST {"eltwise_dst"};

    tensor_descs_.emplace(SRC, spec_.eltwise_dt, spec_.dims, lt::strided);
    tensor_descs_.emplace(DST, spec_.eltwise_dt, spec_.dims, lt::strided);

    std::vector<dnnl::graph::logical_tensor> ltensors_in;
    std::vector<dnnl::graph::logical_tensor> ltensors_out;

    ltensors_in.push_back({tensor_descs_[SRC]});
    ltensors_out.push_back({tensor_descs_[DST]});

    op eltwise_op(1, spec_.op_kind, ltensors_in, ltensors_out, "eltwise");

    //Set alpha, beta, min and max for relevant ops
    switch (spec_.op_kind) {
        case dnnl::graph::op::kind::Elu:
            eltwise_op.set_attr("alpha", spec_.alpha);
            break;
        case dnnl::graph::op::kind::HardTanh:
            eltwise_op.set_attr("min", spec_.alpha);
            eltwise_op.set_attr("max", spec_.beta);
            break;
        default: break;
    }

    ops_.emplace_back(eltwise_op);
    curr_out_map_ids_.assign({"eltwise_dst"});

    return fill_status::DONE;
}

int doit(const ::eltwise::prb_t *prb, res_t *res) {
    using dt = dnnl::graph::logical_tensor::data_type;
    res->impl_name = "graph";

    if (bench_mode == LIST) return res->state = LISTED, OK;
    ::eltwise::check_known_skipped_case(prb, res);
    if (res->state == SKIPPED) return OK;

    eltwise_graph_prb_t graph_prb(prb);
    if (graph_prb.ctor_status != fill_status::DONE
            && graph_prb.ctor_status != fill_status::UNHANDLED_CONFIG_OPTIONS) {
        return res->state = UNIMPLEMENTED, FAIL;
    }

    auto graph_h = graph_prb.to_graph();

    const auto partitions = graph_h.get_partitions();
    if (partitions.empty() || partitions.size() > 1)
        return res->state = FAILED, FAIL;

    const auto par = partitions[0];
    if (!par.is_supported()) return res->state = UNIMPLEMENTED, FAIL;

    const auto ins = par.get_in_ports();
    const auto outs = par.get_out_ports();

    const auto &e = benchdnnext::get_test_engine();
    auto cp = par.compile(ins, outs, e);

    static const engine_t cpu_engine(dnnl_cpu);

    dnn_mem_t src_fp = make_dnn_mem(ins[0], dt::f32, tag::abx);
    // we need src_fp for proper comparison, => no in-place reference
    dnn_mem_t dst_fp = make_dnn_mem(outs[0], dt::f32, tag::abx);

    dnn_mem_t placeholder_dst_dt;
    if (!prb->inplace) {
        placeholder_dst_dt = make_dnn_mem(outs[0], (prb->tag).c_str());
    }

    dnn_mem_t src_dt = make_dnn_mem(ins[0], (prb->tag).c_str());
    dnn_mem_t &dst_dt = prb->inplace ? src_dt : placeholder_dst_dt;
    //TODO: need to extend for post ops
    std::vector<dnn_mem_t> binary_po_fp, binary_po_dt;

    SAFE(::eltwise::fill_data(prb, SRC, src_dt, src_fp), WARN);

    const bool is_fwd = prb->dir & FLAG_FWD;
    dnn_mem_t &arg_fp = !is_fwd && prb->use_dst() ? dst_fp : src_fp;

    // Shouldn't be defined inside since not available when `eltwise_add_check`
    // is invoked due to removed from stack.
    const float trh
            = ::eltwise::get_eltwise_threshold(prb->dt, prb->alg, is_fwd);
    compare::compare_t cmp;
    if (is_bench_mode(CORR)) {
        cmp.set_threshold(trh);
        cmp.set_zero_trust_percent(
                ::eltwise::get_eltwise_zero_trust_percent(prb));

        const auto eltwise_add_check =
                [&](const compare::compare_t::driver_check_func_args_t &args) {
                    // Some algorithms require absolute value comparison for inputs
                    // where catastrophic cancellation may happen.
                    const float src = arg_fp.get_elem(args.idx);
                    if (::eltwise::check_abs_err(prb, src, trh))
                        return args.diff <= trh;
                    if (prb->attr.post_ops.binary_index() != -1)
                        return args.diff <= trh;
                    return false;
                };
        cmp.set_driver_check_function(eltwise_add_check);
    }

    std::vector<dnnl::graph::tensor> tensors_in;
    std::vector<dnnl::graph::tensor> tensors_out;
    tensors_in.push_back(
            dnnl::graph::tensor(ins[0], static_cast<void *>(src_dt)));
    tensors_out.push_back(
            dnnl::graph::tensor(outs[0], static_cast<void *>(dst_dt)));

    if (prb->dir & FLAG_FWD) {
        SAFE(execute_and_wait(cp, tensors_in, tensors_out), WARN);

        if (is_bench_mode(CORR)) {
            ::eltwise::compute_ref_fwd(prb, src_fp, binary_po_fp, dst_fp);
            SAFE(cmp.compare(dst_fp, dst_dt, prb->attr, res), WARN);
        }
    }

    measure_perf(res->timer, cp, tensors_in, tensors_out);

    return OK;
}
} // namespace eltwise
} // namespace benchdnnext
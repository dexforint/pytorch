#include <torch/csrc/jit/runtime/static/passes.h>

#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/ir/subgraph_matcher.h>
#include <torch/csrc/jit/passes/constant_pooling.h>
#include <torch/csrc/jit/passes/constant_propagation.h>
#include <torch/csrc/jit/passes/subgraph_rewrite.h>
#include <torch/csrc/jit/passes/variadic_ops.h>
#include <torch/csrc/jit/runtime/graph_iterator.h>
#include <torch/csrc/jit/runtime/static/ops.h>

C10_DEFINE_bool(
    enable_clip_ranges_gather_fusions,
    true,
    "If on, static runtime or optimize_sparse_nn_model will fuse clip ranges gather ops.");

namespace torch {
namespace jit {

bool graphHasOp(std::shared_ptr<Graph>& graph, const char* op_name) {
  DepthFirstGraphNodeIterator graph_it(graph);
  for (auto node = graph_it.next(); node != nullptr; node = graph_it.next()) {
    const char* node_qual_string = node->kind().toQualString();
    if (strcmp(node_qual_string, op_name) == 0) {
      return true;
    }
  }
  return false;
}

bool forwardHasOp(
    const torch::jit::script::Module& module,
    const char* op_name) {
  using Method = ::torch::jit::Method;
  Method method = module.get_method("forward");
  auto graph = method.graph();
  return graphHasOp(graph, op_name);
}

namespace {
C10_UNUSED
void ConcatAddMulReplaceNaNClip(std::shared_ptr<torch::jit::Graph>& graph) {
  // TODO:: check restrictions for inputs; outputs not used elsewhere
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g, %h, %i, %j):
        %y0 = aten::cat(%a, %b)
        %y1 = aten::add(%y0, %c, %d)
        %y2 = aten::mul(%y1, %e)
        %y3 = aten::nan_to_num(%y2, %f, %g, %h)
        %res = aten::clamp(%y3, %i, %j)
        return (%res))IR";
  std::string pattern2 = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g, %h, %i, %j):
        %y0 = aten::cat(%a, %b)
        %y1 = aten::add(%y0, %c, %d)
        %y2 = aten::mul(%y1, %e)
        %y3 = aten::nan_to_num_(%y2, %f, %g, %h)
        %res = aten::clamp(%y3, %i, %j)
        return (%res))IR";
  std::string pattern3 = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g, %h, %i, %j):
        %y0 = aten::cat(%a, %b)
        %y1 = aten::add(%y0, %c, %d)
        %y2 = aten::mul(%y1, %e)
        %y3 = aten::nan_to_num_(%y2, %f, %g, %h)
        %res = aten::clamp_(%y3, %i, %j)
        return (%res))IR";
  std::string pattern4 = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g, %h, %i, %j):
        %y0 = aten::cat(%a, %b)
        %y1 = aten::add(%y0, %c, %d)
        %y2 = aten::mul(%y1, %e)
        %y3 = aten::nan_to_num(%y2, %f, %g, %h)
        %res = aten::clamp_(%y3, %i, %j)
        return (%res))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g, %h, %i, %j):
        %res = fb::concat_add_mul_replacenan_clip(%c, %e, %a, %i, %j, %b)
        return (%res))IR";

  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);

  fuse.RegisterRewritePattern(pattern2, fused_pattern);
  fuse.runOnGraph(graph);

  fuse.RegisterRewritePattern(pattern3, fused_pattern);
  fuse.runOnGraph(graph);

  fuse.RegisterRewritePattern(pattern4, fused_pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED
void CastedBatchOneHotLengths(std::shared_ptr<torch::jit::Graph>& graph) {
  // TODO:: check restrictions for inputs; outputs not used elsewhere
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g):
        %y0 : Tensor = aten::to(%a, %b, %c, %c, %d)
        %y1 : Tensor = fb::batch_one_hot_lengths(%y0, %e, %f)
        %res : Tensor = aten::to(%y1, %g, %c, %c, %d)
        return (%res))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g):
        %res : Tensor = fb::casted_batch_one_hot_lengths(%a, %e, %f)
        return (%res))IR";
  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);

  std::string pattern2 = R"IR(
    graph(%a, %b, %c, %d, %e, %f):
        %y0 : Tensor = aten::to(%a, %b, %c, %c)
        %y1 : Tensor = fb::batch_one_hot_lengths(%y0, %d, %e)
        %res : Tensor = aten::to(%y1, %f, %c, %c)
        return (%res))IR";
  std::string fused_pattern2 = R"IR(
    graph(%a, %b, %c, %d, %e, %f):
        %res : Tensor = fb::casted_batch_one_hot_lengths(%a, %d, %e)
        return (%res))IR";
  fuse.RegisterRewritePattern(pattern2, fused_pattern2);
  fuse.runOnGraph(graph);
}

C10_UNUSED
void ConcatBatchMatMulBatchGather(std::shared_ptr<torch::jit::Graph>& graph) {
  // TODO:: check restrictions for inputs; outputs not used elsewhere
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f):
        %y0 : Tensor = aten::stack(%a, %b)
        %y1 : Tensor = aten::transpose(%y0, %b, %c)
        %y2 : Tensor = aten::bmm(%y0, %y1)
        %y3 : Tensor = aten::flatten(%y2, %d, %e)
        %res : Tensor = aten::index_select(%y3, %b, %f)
        return (%res))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f):
        %res : Tensor = fb::concat_batch_matmul_batch_gather(%f, %a)
        return (%res))IR";
  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED void ClipRangesGatherRangesLengthsToOffsets(
    std::shared_ptr<torch::jit::Graph>& graph) {
  // TODO:: check restrictions for inputs; outputs not used elsewhere
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d):
        %y0 : Tensor = fb::clip_ranges(%b, %c)
        %y1 : Tensor, %y2 : Tensor = fb::gather_ranges(%a, %y0)
        %y3 : Tensor = fb::lengths_to_offsets(%y2, %d)
        return (%y3, %y1))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c, %d):
        %y0 : Tensor, %y1 : Tensor = fb::clip_ranges_gather_lengths_to_offsets(%a, %b, %c, %d)
        return (%y1, %y0))IR";
  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED void ClipRangesGather(std::shared_ptr<torch::jit::Graph>& graph) {
  // TODO:: check restrictions for inputs; outputs not used elsewhere
  // fuse without lengths-to-offsets
  std::string pattern = R"IR(
    graph(%a, %b, %c):
        %y0 : Tensor = fb::clip_ranges(%b, %c)
        %y1 : Tensor, %y2 : Tensor = fb::gather_ranges(%a, %y0)
        return (%y2, %y1))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c):
        %y0 : Tensor, %y1 : Tensor = fb::clip_ranges_gather(%a, %b, %c)
        return (%y1, %y0))IR";
  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED void PrecomputeMultiplierShiftForSigridHash(
    std::shared_ptr<torch::jit::Graph>& graph) {
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d):
        %y0 : Tensor = fb::sigrid_hash(%a, %b, %c, %d)
        return (%y0)
  )IR";
  std::string split_pattern = R"IR(
    graph(%a, %b, %c, %d):
        %y0 : Tensor = fb::sigrid_hash_compute_multipler_shift(%c)
        %y2 : Tensor = fb::sigrid_hash_precompute(%a, %b, %c, %y0, %d)
        return (%y2)
  )IR";
  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, split_pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED void ClipRangesToGatherToOffsets(
    std::shared_ptr<torch::jit::Graph>& graph) {
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d, %to0_in0, %to0_in1, %to0_in2):
        %y0 : Tensor, %y1 : Tensor = fb::clip_ranges_gather(%a, %b, %c)
        %y2 : Tensor = aten::to(%y1, %to0_in0, %to0_in1, %to0_in1, %to0_in2)
        %y3 : Tensor = fb::lengths_to_offsets(%y2, %d)
        return (%y3, %y0))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c, %d, %to0_in0, %to0_in1, %to0_in2):
        %y0 : Tensor, %y1 : Tensor = fb::clip_ranges_gather_to_offsets(%a, %b, %c, %d, %to0_in0)
        return (%y1, %y0))IR";
  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);

  std::string pattern2 = R"IR(
    graph(%a, %b, %c, %d, %to0_in0, %to0_in1):
        %y0 : Tensor, %y1 : Tensor = fb::clip_ranges_gather(%a, %b, %c)
        %y2 : Tensor = aten::to(%y1, %to0_in0, %to0_in1, %to0_in1)
        %y3 : Tensor = fb::lengths_to_offsets(%y2, %d)
        return (%y3, %y0))IR";
  std::string fused_pattern2 = R"IR(
    graph(%a, %b, %c, %d, %to0_in0, %to0_in1):
        %y0 : Tensor, %y1 : Tensor = fb::clip_ranges_gather_to_offsets(%a, %b, %c, %d, %to0_in0)
        return (%y1, %y0))IR";
  fuse.RegisterRewritePattern(pattern2, fused_pattern2);
  fuse.runOnGraph(graph);
}

C10_UNUSED
void ClipRangesGatherSigridHash(std::shared_ptr<torch::jit::Graph>& graph) {
  // TODO:: check restrictions for inputs; outputs not used elsewhere
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g, %h):
        %y0 : Tensor, %y1 : Tensor = fb::clip_ranges_gather_lengths_to_offsets(%a, %b, %c, %d)
        %y2 : Tensor = fb::sigrid_hash_precompute(%y0, %e, %f, %g, %h)
        return (%y2, %y1))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g, %h):
        %off : Tensor, %out : Tensor = fb::clip_ranges_gather_sigrid_hash_precompute_offsets(%b, %a, %c, %e, %f, %g, %h, %d)
        return (%out, %off))IR";
  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED void ClipRangesGatherRangesSigridHash(
    std::shared_ptr<torch::jit::Graph>& graph) {
  std::string pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g):
        %y0 : Tensor = fb::clip_ranges(%b, %c)
        %y1 : Tensor, %y2 : Tensor = fb::gather_ranges(%a, %y0)
        %y3 : Tensor = fb::sigrid_hash_precompute(%y1, %d, %e, %f, %g)
        return (%y3, %y2))IR";
  std::string fused_pattern = R"IR(
    graph(%a, %b, %c, %d, %e, %f, %g):
        %off : Tensor, %out : Tensor = fb::clip_ranges_gather_sigrid_hash_precompute_v3(%b, %a, %c, %d, %e, %f, %g)
        return (%out, %off))IR";

  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED void ClipRangesGatherRangesX2SigridHashPrecompute(
    std::shared_ptr<torch::jit::Graph>& graph) {
  // Placeholder is a dummy op used to capture the first subgraph
  std::string pattern = R"IR(
    graph(%ranges, %values, %max_length, %salt, %max_value, %mul_shift, %hash_into_int32):
        %clipped : Tensor = fb::clip_ranges(%ranges, %max_length)
        %output : Tensor, %unused : Tensor = fb::gather_ranges(%values, %clipped)
        %sigrid_hash_out : Tensor = fb::sigrid_hash_precompute(%output, %salt, %max_value, %mul_shift, %hash_into_int32)
        return (%sigrid_hash_out, %clipped))IR";
  std::string fused_pattern = R"IR(
    graph(%ranges, %values, %max_length, %salt, %max_value, %mul_shift, %hash_into_int32):
        %sigrid_hash_out : Tensor, %clipped : Tensor = fb::placeholder(%ranges, %values, %max_length, %salt, %max_value, %mul_shift, %hash_into_int32)
        return (%sigrid_hash_out, %clipped))IR";

  // the second gather_ranges can be eliminated because the `lengths` is
  // produces is identical to the lengths produced by
  // clip_ranges_gather_sigrid_hash_v3 (caveat, the fused ops makes some
  // simplifying assumptions about the ranges input)
  std::string pattern2 = R"IR(
    graph(%gather2_values, %ranges, %values, %max_length, %salt, %max_value, %mul_shift, %hash_into_int32):
        %sigrid_hash_out : Tensor, %clipped : Tensor = fb::placeholder(%ranges, %values, %max_length, %salt, %max_value, %mul_shift, %hash_into_int32)
        %unused : Tensor, %lengths : Tensor = fb::gather_ranges(%gather2_values, %clipped)
        return (%lengths, %sigrid_hash_out))IR";

  std::string fused_pattern2 = R"IR(
    graph(%gather2_values, %ranges, %values, %max_length, %salt, %max_value, %mul_shift, %hash_into_int32):
        %lengths : Tensor, %sigrid_hash_out : Tensor = fb::clip_ranges_gather_sigrid_hash_precompute_v3(%ranges, %values, %max_length, %salt, %max_value, %mul_shift, %hash_into_int32)
        return (%lengths, %sigrid_hash_out))IR";

  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);

  fuse.RegisterRewritePattern(pattern2, fused_pattern2);
  fuse.runOnGraph(graph);

  // reverse the ops that got fused in step 1 but not in step2
  fuse.RegisterRewritePattern(fused_pattern, pattern);
  fuse.runOnGraph(graph);
}

C10_UNUSED void SplitOutPrecomputeOpsForSparseNN(
    std::shared_ptr<torch::jit::Graph>& graph) {
#ifdef FBCODE_CAFFE2
  PrecomputeMultiplierShiftForSigridHash(graph);
  ConstantPropagation(graph);
  ConstantPooling(graph);
#endif
}
} // namespace

void FuseInferenceOpsForSparseNN(std::shared_ptr<torch::jit::Graph>& graph) {
#ifdef FBCODE_CAFFE2
  SplitOutPrecomputeOpsForSparseNN(graph);

  ConcatAddMulReplaceNaNClip(graph);
  CastedBatchOneHotLengths(graph);
  ConcatBatchMatMulBatchGather(graph);

  if (FLAGS_enable_clip_ranges_gather_fusions) {
    ClipRangesGatherRangesLengthsToOffsets(graph);
  }
  ClipRangesGatherSigridHash(graph);
  ClipRangesGatherRangesSigridHash(graph);

  ClipRangesGatherRangesX2SigridHashPrecompute(graph);

  if (FLAGS_enable_clip_ranges_gather_fusions) {
    // prioritize clip_ranges+gather_ranges+sigrid_hash fusion over
    // clip_ranges+gather_ranges
    ClipRangesGather(graph);

    ClipRangesToGatherToOffsets(graph);
  }
#endif
}

TORCH_LIBRARY_FRAGMENT(static_runtime, m) {
  m.def(torch::schema(
      "static_runtime::permute_copy(Tensor self, int[] dims) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::reshape_copy(Tensor self, int[] shape) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::flatten_copy.using_ints(Tensor self, int start_dim=0, int end_dim=-1) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::expand_dims_copy(Tensor input, int[] dims) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::to_maybe_copy_out.prim_dtype(Tensor self, int? dtype=None, bool non_blocking=False, bool copy=False) -> (Tensor, bool)",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::to_maybe_copy_out.dtype(Tensor self, ScalarType dtype, bool non_blocking=False, bool copy=False, MemoryFormat? memory_format=None) -> (Tensor, bool)",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::to_maybe_copy_out.other(Tensor self, Tensor other, bool non_blocking=False, bool copy=False, MemoryFormat? memory_format=None) -> (Tensor, bool)",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::to_copy.prim_dtype(Tensor self, int? dtype=None, bool non_blocking=False, bool copy=False) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::to_copy.dtype(Tensor self, ScalarType dtype, bool non_blocking=False, bool copy=False, MemoryFormat? memory_format=None) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::to_copy.other(Tensor self, Tensor other, bool non_blocking=False, bool copy=False, MemoryFormat? memory_format=None) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::layer_norm(Tensor input, int[] normalized_shape, Tensor? weight=None, Tensor? bias=None, float eps=1e-05, bool cudnn_enable=True) -> (Tensor, Tensor, Tensor)",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def("static_runtime::signed_log1p(Tensor input) -> Tensor");
  m.def(torch::schema(
      "static_runtime::dict_unpack(...) -> ...",
      c10::AliasAnalysisKind::CONSERVATIVE));
  m.def(torch::schema(
      "static_runtime::VarTupleUnpack(...) -> ...",
      c10::AliasAnalysisKind::CONSERVATIVE));
  m.def(torch::schema(
      "static_runtime::fused_equally_split(Tensor input, int num_split, int dim) -> ...",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::dequantize_copy.self(Tensor self) -> Tensor",
      c10::AliasAnalysisKind::PURE_FUNCTION));
  m.def(torch::schema(
      "static_runtime::select_tensor(Tensor(a) a, Tensor(b) b, bool use_b) -> Tensor(a|b)",
      c10::AliasAnalysisKind::FROM_SCHEMA));
  m.def(torch::schema("static_runtime::create_owned_ref(...) -> ..."));
}

void FuseSignLog1P(std::shared_ptr<torch::jit::Graph>& graph) {
  std::string pattern = R"IR(
    graph(%input):
        %0 : Tensor = aten::sign(%input)
        %1 : Tensor = aten::abs(%input)
        %2 : Tensor = aten::log1p(%1)
        %res : Tensor = aten::mul(%0, %2)
        return (%res)
  )IR";

  std::string fused_pattern = R"IR(
    graph(%input):
        %res : Tensor = static_runtime::signed_log1p(%input)
        return (%res)
    )IR";

  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph);
}

namespace {

using TupleUnpackBlock = std::vector<Node*>;

std::vector<TupleUnpackBlock> CollectVariadicTupleUnpackFusionCandidates(
    const std::shared_ptr<Graph>& graph) {
  std::vector<TupleUnpackBlock> candidates;
  auto nodes = graph->nodes();
  std::vector<Node*> block;
  for (Node* cur_node : nodes) {
    if (cur_node->kind() == prim::TupleUnpack) {
      block.push_back(cur_node);
      continue;
    }
    if (block.size() > 1) {
      candidates.emplace_back(std::move(block));
    }
    block.clear();
  }
  TORCH_CHECK(block.empty());
  return candidates;
}

void FuseTupleUnpackBlock(const TupleUnpackBlock& nodes) {
  TORCH_CHECK(nodes.size() > 0);
  auto graph = nodes[0]->owningGraph();
  auto var_unpack = graph->create(
      fromQualString("static_runtime::VarTupleUnpack"),
      /* num_outputs */ 0);
  var_unpack->insertAfter(nodes[nodes.size() - 1]);
  for (Node* node : nodes) {
    TORCH_CHECK(
        node->kind() == prim::TupleUnpack && node->inputs().size() == 1);
    var_unpack->addInput(node->input());

    for (Value* output : node->outputs()) {
      auto new_output = var_unpack->addOutput();
      new_output->copyMetadata(output);
      output->replaceAllUsesWith(new_output);
    }
    node->destroy();
  }
}

} // namespace

void UseVariadicTupleUnpack(const std::shared_ptr<Graph>& graph) {
  for (auto& c : CollectVariadicTupleUnpackFusionCandidates(graph)) {
    FuseTupleUnpackBlock(c);
  }
}

// This macro makes maps from c10::Symbol -> c10::Symbol a lot easier to read.
#define OP_PAIR(first, second) \
  { fromQualString(first), fromQualString(second) }

// Out variants of ops cannot participate in memory planning if they
// have outputs that alias inputs. For ops that either return their
// input directly or copy it (most notably aten::to), we adopt the
// following strategy instead of directly making them out variants so
// that they can participate in memory planning anyway. Let `a` denote
// the input Tensor to the op.
//
// 1) Pass `a` (and the other operator inputs) to a special
// `static_runtime::$OP_maybe_copy_out` variant of the op. This op
// returns a normal output Tensor (call it `b_out` as well as a
// `did_copy` flag indicating whether the output should be used. If
// `did_copy` is false, the value of `b_out` is unspecified. Note that
// this operator is an ordinary out variant that is perfectly amenable
// to memory planning.
//
// 2) Pass `a`, `b_out`, and `did_copy` to a special
// `static_runtime::select_tensor` op, which returns `b_out` if
// `did_copy` is true and `a` otherwise. Note that this operator does
// not need to participate in memory planning because its output
// always aliases one of its inputs.
//
// Here is an illustration:
//
//                        |
// |----------------------+ a
// |                      v
// |    +------------------------------------+
// |    |                                    |
// |    | static_runtime::$OP_maybe_copy_out |
// |    |                                    |
// |    +------------------+--------+--------+
// |                       |        |
// +--------------+        | b_out  | did_copy
//                | a      |        |
//                v        v        v
//      +------------------------------------+
//      |                                    |
//      |    static_runtime::select_tensor   |
//      |                                    |
//      +------------------+-----------------+
//                         |
//                         |
//                         | either a or b_out
//                         |
//                         v

void ReplaceWithMaybeCopy(
    std::shared_ptr<Graph>& graph,
    bool outputs_are_immutable) {
  AliasDb db(graph);
  // for ops that have overloads, match the schema
  static const std::array<std::pair<c10::FunctionSchema, c10::Symbol>, 3> supported_schema =
      {{{torch::schema(
             "aten::to.prim_dtype(Tensor(a) self, int? dtype=None, bool non_blocking=False, bool copy=False) -> Tensor(a|b)"),
         fromQualString("static_runtime::to_maybe_copy_out")},
        {torch::schema(
             "aten::to.dtype(Tensor(a) self, ScalarType dtype, bool non_blocking=False, bool copy=False, MemoryFormat? memory_format=None) -> Tensor(a)"),
         fromQualString("static_runtime::to_maybe_copy_out")},
        {torch::schema(
             "aten::to.other(Tensor(a) self, Tensor other, bool non_blocking=False, bool copy=False, MemoryFormat? memory_format=None) -> Tensor(a)"),
         fromQualString("static_runtime::to_maybe_copy_out")}}};

  auto match_schema = [](const Node* node, c10::Symbol& out_matched_symbol) {
    for (auto& schema : supported_schema) {
      if (node->matches(schema.first)) {
        out_matched_symbol = schema.second;
        return true;
      }
    }
    return false;
  };

  // old node, new node, select_tensor node
  std::vector<std::tuple<Node*, Node*, Node*>> replacement;
  DepthFirstGraphNodeIterator graph_it(graph);
  for (auto n = graph_it.next(); n != nullptr; n = graph_it.next()) {
    c10::Symbol new_symbol;
    if (!match_schema(n, new_symbol)) {
      continue;
    }
    TORCH_CHECK(n->outputs().size() == 1);

    // Duplicate input writers guard from ReplaceWithCopy below.
    if (db.hasInputWriters(n)) {
      continue;
    }

    auto* out = n->output();
    if (!outputs_are_immutable && db.mayContainAlias(out, graph->outputs())) {
      continue;
    }

    // Add the did_copy flag to outputs.
    auto* new_node = graph->create(new_symbol, n->outputs().size() + 1);
    for (auto* input : n->inputs()) {
      new_node->addInput(input);
    }
    new_node->outputs().at(1)->setType(c10::BoolType::get());

    static const auto select_tensor_symbol =
        fromQualString("static_runtime::select_tensor");
    auto* select_tensor_node = graph->create(select_tensor_symbol, 1);
    DCHECK_EQ(new_node->outputs().size(), 2);
    select_tensor_node->addInput(n->input(0));
    for (auto* output : new_node->outputs()) {
      select_tensor_node->addInput(output);
    }
    replacement.emplace_back(n, new_node, select_tensor_node);
  }

  for (const auto& tup : replacement) {
    auto* const old_node = std::get<0>(tup);
    auto* const new_node = std::get<1>(tup);
    auto* const select_tensor_node = std::get<2>(tup);

    new_node->insertBefore(old_node);
    select_tensor_node->insertBefore(old_node);
    new_node->outputs()[0]->copyMetadata(old_node->output());
    select_tensor_node->output()->copyMetadata(old_node->output());
    old_node->replaceAllUsesWith(select_tensor_node);
    old_node->destroy();
  }
#ifndef NDEBUG
  graph->lint();
  AliasDb db2(graph);
  torch::jit::Lint(&db2);
#endif
}

void ReplaceWithCopy(
    std::shared_ptr<Graph>& graph,
    bool outputs_are_immutable) {
  AliasDb db(graph);
  const FastMap<c10::Symbol, c10::Symbol> supported = {
#ifdef FBCODE_CAFFE2
      OP_PAIR("aten::permute", "static_runtime::permute_copy"),
      OP_PAIR("fb::expand_dims", "static_runtime::expand_dims_copy"),
#endif
      OP_PAIR("aten::narrow", "aten::narrow_copy"),
      OP_PAIR("aten::reshape", "static_runtime::reshape_copy"),
      OP_PAIR("aten::flatten", "static_runtime::flatten_copy")};

  static const std::array<std::pair<c10::FunctionSchema, c10::Symbol>, 1>
      supported_schema = {
          {{torch::schema("aten::dequantize.self(Tensor self) -> Tensor"),
            fromQualString("static_runtime::dequantize_copy")}}};

  auto match_schema = [](const Node* node, c10::Symbol& out_matched_symbol) {
    for (auto& schema : supported_schema) {
      if (node->matches(schema.first)) {
        out_matched_symbol = schema.second;
        return true;
      }
    }
    return false;
  };

  std::vector<std::pair<Node*, Node*>> replacement;
  DepthFirstGraphNodeIterator graph_it(graph);
  for (auto n = graph_it.next(); n != nullptr; n = graph_it.next()) {
    c10::Symbol new_symbol;
    if (supported.count(n->kind()) && opIsRegistered(supported.at(n->kind()))) {
      new_symbol = supported.at(n->kind());
    } else if (!match_schema(n, new_symbol)) {
      continue;
    }
    TORCH_CHECK(n->outputs().size() == 1);

    // We do not want to replace operators with their copy variant when the
    // inputs to the operators have writers (can be updated). With an output
    // that aliases to the input, updates to the input will be visible to the
    // operator's output as well. For example:
    //
    // def forward(self, inp: Tensor, shape: List[int]):
    //   a = inp + inp
    //   b = a.reshape(shape)
    //   c = b.sigmoid_()
    //   d = c + c
    //   e = a + a
    //   f = b + b
    //   return (d, e, f)
    //
    // b and c are aliases of a, sigmoid_ changes b, c, as well as a. e should
    // equal to d in this case. If we replace reshape with the copy version, b
    // and c are no longer aliases of a, the value of e would change as a
    // result. To keep static runtime consistent with the jit interpreter, here
    // we choose not to replace reshape with the copy version
    if (db.hasInputWriters(n)) {
      continue;
    }

    auto* out = n->output();
    if (!outputs_are_immutable && db.mayContainAlias(out, graph->outputs())) {
      continue;
    }
    auto* new_node = graph->create(new_symbol, n->outputs().size());
    for (auto* input : n->inputs()) {
      new_node->addInput(input);
    }
    replacement.emplace_back(std::make_pair(n, new_node));
  }

  for (const auto& p : replacement) {
    auto* old_node = p.first;
    auto* new_node = p.second;
    new_node->insertBefore(old_node);
    new_node->output()->copyMetadata(old_node->output());
    old_node->replaceAllUsesWith(new_node);
    old_node->destroy();
  }
#ifndef NDEBUG
  graph->lint();
  AliasDb db2(graph);
  torch::jit::Lint(&db2);
#endif
}

void EliminateTrivialEquallySplit(std::shared_ptr<torch::jit::Graph>& graph) {
  const auto equally_split = fromQualString("fb::equally_split");
  std::vector<Node*> to_remove;
  DepthFirstGraphNodeIterator graph_it(graph);
  for (auto node = graph_it.next(); node != nullptr; node = graph_it.next()) {
    if (node->kind() != equally_split) {
      continue;
    }

    const Value* value_out = node->outputs()[0];
    if (value_out->uses().size() != 1) {
      continue;
    }

    Node* list_unpack_node = value_out->uses()[0].user;
    if (list_unpack_node->kind() != prim::ListUnpack) {
      continue;
    }

    auto list_unpack_outputs = list_unpack_node->outputs();
    if (list_unpack_outputs.size() != 1) {
      continue;
    }

    list_unpack_node->output()->replaceAllUsesWith(node->input(0));
    to_remove.push_back(list_unpack_node);
    to_remove.push_back(node);
  }

  for (Node* node : to_remove) {
    node->destroy();
  }
}

namespace {

bool shouldNotFuseListUnpackSpecialCase(const Node* node) {
  const static std::array<c10::Symbol, 3> sigrid_transforms_symbols{
      c10::Symbol::fromQualString("fb::variadic_sigrid_transforms_torch_bind"),
      c10::Symbol::fromQualString("fb::sigrid_transforms_torch_bind"),
      c10::Symbol::fromQualString("fb::sigrid_transforms")};

  if (std::find(
          sigrid_transforms_symbols.begin(),
          sigrid_transforms_symbols.end(),
          node->kind()) == sigrid_transforms_symbols.end()) {
    return false;
  }

  // To fuse with sigrid transforms, we must be able to statically determine
  // `instance` and `use_offsets` - these two together let us statically
  // determine the types of the outputs. Rationale: it is a huge pain to write
  // fused sigrid transforms without static type information, and these two
  // arguments are indeed statically known in every model we've seen.
  // The reason why trying to fuse the outputs is annoying without static type
  // information is that, if one of the outputs is not managed, you need to
  // reset to an empty tensor of the correct type each iteration. So, if we
  // can't collect types ahead of time, we would have to do it lazily on the
  // first iteration, which would could be wasteful in terms of time/memory
  // - either each thread would have its own set of output types, or we would
  // need a lock to prevent data races.
  const auto num_inputs = node->inputs().size();
  return !toIValue(node->input(0)).has_value() ||
      !toIValue(node->input(num_inputs - 1)).has_value();
}

} // namespace

void FuseListUnpack(std::shared_ptr<torch::jit::Graph>& graph) {
  const FastMap<c10::Symbol, c10::Symbol> unfused_to_fused = {
      OP_PAIR("fb::equally_split", "static_runtime::fused_equally_split"),
      OP_PAIR(
          "fb::sigrid_transforms", "static_runtime::fused_sigrid_transforms"),
      OP_PAIR(
          "static_runtime::variadic_grouped_accessor_op",
          "static_runtime::fused_variadic_grouped_accessor_op"),
      OP_PAIR(
          "static_runtime::variadic_grouped_accessor_op_v2",
          "static_runtime::fused_variadic_grouped_accessor_op_v2"),
      OP_PAIR(
          "fb::sigrid_transforms_torch_bind",
          "static_runtime::fused_sigrid_transforms_torch_bind"),
      OP_PAIR(
          "fb::variadic_sigrid_transforms_torch_bind",
          "static_runtime::fused_variadic_sigrid_transforms_torch_bind"),
      OP_PAIR(
          "fb::gather_ranges_to_dense",
          "static_runtime::fused_gather_ranges_to_dense"),
      OP_PAIR(
          "fb::gather_ranges_to_dense_v2",
          "static_runtime::fused_gather_ranges_to_dense_v2"),
      OP_PAIR(
          "fb::split_and_squeeze", "static_runtime::fused_split_and_squeeze")};

  // replacement contains (old_node, new_node, list_unpack_node)
  std::vector<std::tuple<Node*, Node*, Node*>> replacement;
  DepthFirstGraphNodeIterator graph_it(graph);
  for (auto node = graph_it.next(); node != nullptr; node = graph_it.next()) {
    auto unfused_to_fused_it = unfused_to_fused.find(node->kind());
    if (unfused_to_fused_it == unfused_to_fused.end()) {
      continue;
    }

    const Value* value_out = node->outputs()[0];
    if (value_out->uses().size() != 1) {
      continue;
    }

    Node* list_unpack_node = value_out->uses()[0].user;
    if (list_unpack_node->kind() != prim::ListUnpack) {
      continue;
    }

    auto list_unpack_outputs = list_unpack_node->outputs();
    if (list_unpack_outputs.empty()) {
      continue;
    }

    if (shouldNotFuseListUnpackSpecialCase(node)) {
      continue;
    }

    const auto& new_sym = unfused_to_fused_it->second;
    auto* new_node = graph->create(new_sym, 0);

    for (Value* in : node->inputs()) {
      new_node->addInput(in);
    }

    for (Value* out : list_unpack_outputs) {
      Value* new_out = new_node->addOutput();
      new_out->copyMetadata(out);
      out->replaceAllUsesWith(new_out);
    }
    replacement.emplace_back(node, new_node, list_unpack_node);
  }

  for (const auto& nodes : replacement) {
    auto* old_node = std::get<0>(nodes);
    auto* new_node = std::get<1>(nodes);
    auto* list_unpack_node = std::get<2>(nodes);

    new_node->insertAfter(old_node);
    list_unpack_node->destroy();
    old_node->destroy();
  }
} // namespace jit

void RemoveImmutableInputDictLookups(
    std::shared_ptr<torch::jit::Graph>& graph) {
  auto nodes = graph->nodes();
  AliasDb db(graph);
  // Gather all dict -> getitems where dict is immutable and getitems use
  // constant keys.
  std::unordered_map<Value*, std::vector<Node*>> dict_to_getitems;
  std::unordered_set<Node*> keys;
  for (Node* node : nodes) {
    // Find aten::__getitem__(%dict, %constant_key).
    if (node->kind() != aten::__getitem__) {
      continue;
    }
    Node* getitem_node = node;
    Value* dict = getitem_node->input(0);
    if (db.hasWriters(dict)) {
      // Mutable dict. Skip this optimization.
      continue;
    }
    if (dict->type()->kind() != TypeKind::DictType ||
        dict->node() != graph->param_node()) {
      continue;
    }
    DCHECK(getitem_node->inputs().size() == 2);
    Node* key = getitem_node->input(1)->node();
    if (key->kind() != prim::Constant) {
      continue;
    }
    keys.insert(key);
    auto iter = dict_to_getitems.find(dict);
    if (iter == dict_to_getitems.end()) {
      dict_to_getitems.emplace(dict, std::vector<Node*>{getitem_node});
      continue;
    }
    iter->second.push_back(getitem_node);
  }
  if (keys.size() == 0) {
    return;
  }
  // Move all keys to the beginning of the graph and insert new dict_unpack
  // nodes after that.
  auto* marker = graph->create(prim::Constant);
  graph->prependNode(marker);
  graph->setInsertPoint(marker);
  for (Node* key : keys) {
    DCHECK(key->inputs().size() == 0);
    key->moveBefore(marker);
  }
  const c10::Symbol static_runtime_dict_unpack_symbol =
      fromQualString("static_runtime::dict_unpack");
  for (auto& it : dict_to_getitems) {
    Value* dict = it.first;
    std::vector<Node*>& getitems = it.second;
    DCHECK(getitems.size() > 0);
    auto* dict_unpack =
        graph->create(static_runtime_dict_unpack_symbol, getitems.size());
    graph->insertNode(dict_unpack);
    dict_unpack->addInput(getitems[0]->input(0));
    for (size_t i = 0; i < getitems.size(); ++i) {
      Node* getitem_node = getitems[i];
      DCHECK(getitem_node->input(0) == dict);
      dict_unpack->addInput(getitem_node->input(1));
      dict_unpack->output(i)->copyMetadata(getitem_node->output());
      getitem_node->output(0)->replaceAllUsesWith(dict_unpack->output(i));
      getitem_node->destroy();
    }
  }
  graph->setInsertPoint(graph->block());
  marker->destroy();
}

void UseVariadicGroupedAccessor(const std::shared_ptr<Graph>& graph) {
  // Migration to v2 is still in progress. For now, SR will support
  // both versions of this op.
  UseVariadicOp(
      graph,
      fromQualString("grouped_accessor::grouped_accessor_op"),
      fromQualString("static_runtime::variadic_grouped_accessor_op"));
  UseVariadicOp(
      graph,
      fromQualString("grouped_accessor::grouped_accessor_op_v2"),
      fromQualString("static_runtime::variadic_grouped_accessor_op_v2"));
}

namespace {

void CreateOwnedRefsForSpecialValuesHelper(Graph& graph, Block* block) {
  for (auto* node : block->nodes()) {
    for (auto* sub_block : node->blocks()) {
      CreateOwnedRefsForSpecialValuesHelper(graph, sub_block);
    }
  }

  auto outputs = block->outputs();
  for (const auto i : c10::irange(outputs.size())) {
    auto* output = outputs[i];

    if (output->type()->kind() == c10::TypeKind::NoneType) {
      // No need to create owned refs of NoneType since moving
      // from None will have no effect
      continue;
    }

    if (toIValue(output).has_value() ||
        // If the output's owning block is not this one, it's from an outer
        // scope
        output->node()->owningBlock() != block) {
      auto* create_owned_ref_node =
          graph.create(fromQualString("static_runtime::create_owned_ref"));
      create_owned_ref_node->addInput(output);
      create_owned_ref_node->output()->copyMetadata(output);

      block->appendNode(create_owned_ref_node);
      block->replaceOutput(i, create_owned_ref_node->output());
    }
  }
}

void ForceNonEmptyOutputsHelper(Value* none_value, Block* block) {
  for (auto* node : block->nodes()) {
    bool needs_output = false;
    for (auto* sub_block : node->blocks()) {
      if (sub_block->outputs().empty()) {
        sub_block->registerOutput(none_value);
        needs_output = true;
      }

      ForceNonEmptyOutputsHelper(none_value, sub_block);
    }

    if (needs_output) {
      // Loop sub-blocks should always return at least one output (the new loop
      // condition)
      DCHECK(node->kind() == prim::If);
      auto* output = node->addOutput();
      output->setType(c10::NoneType::get());
    }
  }
}

Node* findOrCreateNoneConstant(Graph& graph) {
  // Only search the top-level block
  for (auto* node : graph.nodes()) {
    if (node->kind() != prim::Constant) {
      continue;
    }
    const auto ival_opt = toIValue(node->output());
    DCHECK(ival_opt.has_value());
    if (ival_opt->isNone()) {
      return node;
    }
  }

  auto* none_node = graph.create(prim::Constant);
  none_node->output()->setType(c10::NoneType::get());
  graph.prependNode(none_node);
  return none_node;
}

} // namespace

void CreateOwnedRefsForSpecialValues(Graph& graph) {
  CreateOwnedRefsForSpecialValuesHelper(graph, graph.block());
}

void ForceNonEmptyOutputs(Graph& graph) {
  auto* none_node = findOrCreateNoneConstant(graph);
  ForceNonEmptyOutputsHelper(none_node->output(), graph.block());
  if (!none_node->hasUses()) {
    none_node->destroy();
  }
}

void EliminateExtraPermuteOps(std::shared_ptr<Graph>& graph) {
  auto input_is_constant_list =
      [](Node* node, size_t input_idx, const c10::List<int64_t>& expected) {
        auto input_opt = toIValue(node->input(input_idx));
        if (!input_opt.has_value() || !input_opt->isIntList()) {
          return false;
        }
        return input_opt->toIntList() == expected;
      };

  // SubgraphRewriter can't pattern-match on constants, so we use this
  // extra filter to make sure the values of the `dim` arguments are
  // correct.
  auto dims_are_valid_constants =
      [&input_is_constant_list](
          const Match& match,
          const std::unordered_map<std::string, Value*>& vmap) {
        // Get the nodes in the real graph from the nodes in the template
        // pattern graph
        const auto& node_map = match.nodes_map;
        auto* sum_node = node_map.at(vmap.at("c")->node());
        auto* permute_node = node_map.at(vmap.at("b")->node());
        return input_is_constant_list(sum_node, 1, c10::List<int64_t>{-1}) &&
            input_is_constant_list(
                   permute_node, 1, c10::List<int64_t>{0, 2, 1});
      };

  const auto pattern = R"IR(
    graph(%a, %sum_dim, %permute_dim, %keepdim, %dtype):
        %b = aten::permute(%a, %permute_dim)
        %c = aten::sum(%b, %sum_dim, %keepdim, %dtype)
        return (%c))IR";

  const auto fused_pattern = R"IR(
    graph(%a, %sum_dim, %permute_dim, %keepdim, %dtype):
        %new_sum_dim: int[] = prim::Constant[value=[1]]()
        %d = aten::sum(%a, %new_sum_dim, %keepdim, %dtype)
        return (%d))IR";

  SubgraphRewriter fuse;
  fuse.RegisterRewritePattern(pattern, fused_pattern);
  fuse.runOnGraph(graph, dims_are_valid_constants);
}

} // namespace jit
} // namespace torch

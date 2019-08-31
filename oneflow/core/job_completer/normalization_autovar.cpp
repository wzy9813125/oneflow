#include "oneflow/core/job_completer/autovar.h"

namespace oneflow {

namespace {

void GenerateInputVarOpConf(
    const Operator& op, std::vector<OperatorConf>* op_confs,
    const std::function<const BlobDesc&(const std::string&)>& BlobDesc4ModelBn) {
  CHECK(op.op_conf().has_normalization_conf());
  OperatorConf normalization_conf(op.op_conf());
  auto* mut_conf = normalization_conf.mutable_normalization_conf();
  const auto& conf = op.op_conf().normalization_conf();
  if (!conf.has_moving_mean()) {
    OperatorConf moving_mean_var_op = GenerateVariableOpConf(
        BlobDesc4ModelBn("moving_mean"), op.op_name() + "-moving_mean", "moving_mean");
    moving_mean_var_op.mutable_variable_conf()
        ->mutable_initializer()
        ->mutable_constant_conf()
        ->set_value(conf.mean_init());
    op_confs->push_back(moving_mean_var_op);
    mut_conf->set_moving_mean(moving_mean_var_op.name() + "/out");
  }
  if (!conf.has_moving_variance()) {
    OperatorConf moving_variance_var_op = GenerateVariableOpConf(
        BlobDesc4ModelBn("moving_variance"), op.op_name() + "-moving_variance", "moving_variance");
    moving_variance_var_op.mutable_variable_conf()
        ->mutable_initializer()
        ->mutable_constant_conf()
        ->set_value(conf.variance_init());
    op_confs->push_back(moving_variance_var_op);
    mut_conf->set_moving_variance(moving_variance_var_op.name() + "/out");
  }
  if (conf.center()) {
    if (!conf.has_beta()) {
      OperatorConf beta_var_op =
        GenerateVariableOpConf(BlobDesc4ModelBn("beta"), op.op_name() + "-beta", "beta");
      beta_var_op.mutable_variable_conf()
        ->mutable_initializer()
        ->mutable_constant_conf()
        ->set_value(conf.beta_init());
      op_confs->push_back(beta_var_op);
      mut_conf->set_beta(beta_var_op.name() + "/out");
    }
  }
  if (conf.scale()) {
    if (!conf.has_gamma()) {
      OperatorConf gamma_var_op =
        GenerateVariableOpConf(BlobDesc4ModelBn("gamma"), op.op_name() + "-gamma", "gamma");
      gamma_var_op.mutable_variable_conf()
        ->mutable_initializer()
        ->mutable_constant_conf()
        ->set_value(conf.gamma_init());
      op_confs->push_back(gamma_var_op);
      mut_conf->set_gamma(gamma_var_op.name() + "/out");
    }
  }
  op_confs->push_back(normalization_conf);
}

}  // namespace

REGISTER_OP_INPUT_VAR(OperatorConf::kNormalizationConf, &GenerateInputVarOpConf);

}  // namespace oneflow

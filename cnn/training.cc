#include "cnn/training.h"

#include "cnn/gpu-ops.h"
#include "cnn/param-nodes.h"
#include "cnn/weight-decay.h"

namespace cnn {

using namespace std;

template <class Derived>
bool is_valid(const Eigen::MatrixBase<Derived>& x) {
  return ((x - x).array() == (x - x).array()).all();
}

Trainer::~Trainer() {}

void Trainer::rescale_and_reset_weight_decay() {
  const float weight_decay = global_weight_decay.CurrentWeightDecay();
  for (auto p : model->parameters_list())
    p->scale_parameters(weight_decay);
  global_weight_decay.ResetWeightDecay();
}

float Trainer::clip_gradients() {
  float gscale = 1;
  if (clipping_enabled) {
    float gg = model->gradient_l2_norm();
    if (isnan(gg) || isinf(gg)) {
      cerr << "Magnitude of gradient is bad: " << gg << endl;
      abort();
    }
    if (gg > clip_threshold) {
      ++clips;
      gscale = clip_threshold / gg;
    }
  }
  return gscale;
}

// this calls the rule-specific
void Trainer::update(real scale) {
  update_impl(scale);
  global_weight_decay.UpdateWeightDecay(); // update global weight scale
  if (global_weight_decay.ParametersNeedRescaled())
    rescale_and_reset_weight_decay();  // if wdscale is getting to small multiply all weights by wdscale, and set wdscale to 1
}

void SimpleSGDTrainer::update_impl(real scale) {
  update_params(model->lookup_parameters_list(), model->parameters_list(), scale);
}

void SimpleSGDTrainer::update_params(const std::vector<LookupParameterStorage*> &lookup_params, const std::vector<ParameterStorage*> &params, real scale) {
  const float gscale = clip_gradients();
  for (auto p : params) {
#if HAVE_CUDA
    gpu::sgd_update(p->values.d.size(), p->g.v, p->values.v, eta * scale * gscale / global_weight_decay.CurrentWeightDecay(), 0);
#else
    p->values.vec() -= ((eta * scale * gscale) * p->g.vec() / global_weight_decay.CurrentWeightDecay());
#endif
    p->clear();
  }
  for (auto p : lookup_params) {
    for (auto i : p->non_zero_grads) {
#if HAVE_CUDA
      gpu::sgd_update(p->values[i].d.size(), p->grads[i].v, p->values[i].v, eta * scale * gscale / global_weight_decay.CurrentWeightDecay(), 0);
#else
      p->values[i].vec() -= (p->grads[i].vec() * (eta * scale * gscale) / global_weight_decay.CurrentWeightDecay());
#endif
    }
    p->clear();
  }
  ++updates;
}

void MomentumSGDTrainer::update_impl(real scale) {
  // executed on the first iteration to create vectors to
  // store the velocity
  if (!velocity_allocated) {
    vp = AllocateShadowParameters(*model);
    vlp = AllocateShadowLookupParameters(*model);
    velocity_allocated = true;
  }

  const float gscale = clip_gradients();
  unsigned pi = 0;
  for (auto p : model->parameters_list()) {
    Tensor& v = vp[pi++].h;
    v.vec() = momentum * v.vec() - (eta * scale * gscale) * p->g.vec();
    p->values.vec() += v.vec() / global_weight_decay.CurrentWeightDecay();
    p->clear();
  }
  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& vx = vlp[pi++].h;
    for (auto i : p->non_zero_grads) {
      Tensor& v = vx[i];
      v.vec() = momentum * v.vec() - (eta * scale * gscale) * (p->grads[i].vec());
      p->values[i].vec() += v.vec() / global_weight_decay.CurrentWeightDecay();
    }
    p->clear();
  }
  ++updates;
}

void AdagradTrainer::update_impl(real scale) {
  unsigned pi;
  if (!shadow_params_allocated) {
    vp = AllocateShadowParameters(*model);
    vlp = AllocateShadowLookupParameters(*model);
    shadow_params_allocated = true;
  }

  pi = 0;
  const float gscale = clip_gradients();
  for (auto p : model->parameters_list()) {
    Tensor& v = vp[pi++].h;
    auto g = scale * gscale * p->g.vec();
    auto g2 = g.cwiseProduct(g);
    v.vec() += g2;
    auto delta = -eta * g.cwiseQuotient((v.vec().array() + epsilon).matrix().cwiseSqrt());
    p->values.vec() += delta / global_weight_decay.CurrentWeightDecay();
    p->clear();
  }

  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& vx = vlp[pi++].h;
    for (auto i : p->non_zero_grads) {
      Tensor& v = vx[i];
      auto g = scale * gscale * p->grads[i].vec();
      auto g2 = g.cwiseProduct(g);
      v.vec() += g2;
      auto delta = -eta * g.cwiseQuotient((v.vec().array() + epsilon).matrix().cwiseSqrt());
      p->values[i].vec() += delta / global_weight_decay.CurrentWeightDecay();
    }
    p->clear();
  }

  ++updates;
}

void AdadeltaTrainer::update_impl(real scale) {
  unsigned pi;
  if (!shadow_params_allocated) {
    hg = AllocateShadowParameters(*model);
    hlg = AllocateShadowLookupParameters(*model);
    hd = AllocateShadowParameters(*model);
    hld = AllocateShadowLookupParameters(*model);

    /*pi = 0;
    for (auto p : model->parameters_list()) {
      TensorTools::Constant(hg[pi].h, epsilon);
      TensorTools::Constant(hd[pi].h, epsilon);
      ++pi;
    }

    pi = 0;
    for (auto p : model->lookup_parameters_list()) {
      vector<Tensor>& hgx = hlg[pi].h;
      vector<Tensor>& hdx = hld[pi].h;
      for (unsigned i = 0; i < hgx.size(); ++i) {
        TensorTools::Constant(hgx[i], epsilon);
        TensorTools::Constant(hdx[i], epsilon);
      }
      ++pi;
    }*/

    shadow_params_allocated = true;
  }

  const float gscale = clip_gradients();
  pi = 0;
  for (auto p : model->parameters_list()) {
    auto& g = (scale * gscale) * p->g.vec();
    Tensor& hgv = hg[pi].h;
    Tensor& hdv = hd[pi].h;
    auto g2 = g.cwiseProduct(g);
    hgv.vec() = rho * hgv.vec() + (1.0 - rho) * g2;
    auto num = -g.cwiseProduct((hdv.vec().array() + epsilon).matrix().cwiseSqrt());
    auto den = (hgv.vec().array() + epsilon).matrix().cwiseSqrt();
    auto delta = num.cwiseQuotient(den);
    auto d2 = delta.cwiseProduct(delta);
    hdv.vec() = rho * hdv.vec() + (1.0 - rho) * d2;
    p->values.vec() += delta / global_weight_decay.CurrentWeightDecay();
    p->clear();
    pi++;
  }

  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& hgvx = hlg[pi].h;
    vector<Tensor>& hdvx = hld[pi].h;
    for (auto i : p->non_zero_grads) {
      Tensor& hgv = hgvx[i];
      Tensor& hdv = hdvx[i];
      auto& g = scale * gscale * p->grads[i].vec();
      auto g2 = g.cwiseProduct(g);
      hgv.vec() = rho * hgv.vec() + (1.0 - rho) * g2;
      auto num = -g.cwiseProduct((hdv.vec().array() + epsilon).matrix().cwiseSqrt());
      auto den = (hgv.vec().array() + epsilon).matrix().cwiseSqrt();
      auto delta = num.cwiseQuotient(den);
      auto d2 = delta.cwiseProduct(delta);
      hdv.vec() = rho * hdv.vec() + (1.0 - rho) * d2;
      p->values[i].vec() += delta / global_weight_decay.CurrentWeightDecay();
    }
    p->clear();
    pi++;
  }
  ++updates;
}

void RmsPropTrainer::update_impl(real scale) {
  unsigned pi = 0;
  if (!shadow_params_allocated) {
    hg.resize(model->parameters_list().size());

    pi = 0;
    hlg.resize(model->lookup_parameters_list().size());
    for (auto p : model->lookup_parameters_list()) {
      hlg[pi++].resize(p->size());
    }

    shadow_params_allocated = true;
  }

  const float gscale = clip_gradients();
  pi = 0;
  for (auto p : model->parameters_list()) {
    real& d2 = hg[pi++];
    real g2 = p->g.vec().squaredNorm();
    d2 = rho * d2 + (1.0 - rho) * g2;
    p->values.vec() -= ((eta * scale * gscale / sqrt(d2 + epsilon)) * p->g.vec()) / global_weight_decay.CurrentWeightDecay();
    p->clear();
  }

  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<real>& hlgx = hlg[pi++];
    for (auto i : p->non_zero_grads) {
      real& d2 = hlgx[i];
      real g2 = p->grads[i].vec().squaredNorm();
      d2 = rho * d2 + (1.0 - rho) * g2;
      p->values[i].vec() -= ((eta * scale * gscale / sqrt(d2 + epsilon)) * p->grads[i].vec()) / global_weight_decay.CurrentWeightDecay();
    }
    p->clear();
  }
  ++updates;
}

void AdamTrainer::update_impl(real scale) {
  unsigned pi;
  if (!shadow_params_allocated) {
    m = AllocateShadowParameters(*model);
    lm = AllocateShadowLookupParameters(*model);
    v = AllocateShadowParameters(*model);
    lv = AllocateShadowLookupParameters(*model);
    shadow_params_allocated = true;
  }

  const float gscale = clip_gradients();
  pi = 0;
  static unsigned t = 0;
  for (auto p : model->parameters_list()) {
    ++t;
    auto g_t = (scale * gscale) * p->g.vec();
    auto m_t = m[pi].h.vec();
    auto v_t = v[pi].h.vec();
    m_t = beta_1 * m_t + (1 - beta_1) * g_t;
    auto g2 = g_t.cwiseProduct(g_t);
    v_t = beta_2 * v_t + (1 - beta_2) * g2;
    float s1 = 1 - pow(beta_1, t);
    float s2 = 1 - pow(beta_2, t);
    auto mhat = m_t / s1;
    auto vhat = v_t / s2;
    auto delta = (-eta * mhat).cwiseQuotient((vhat.array().sqrt() + eps).matrix());
    p->values.vec() += delta / global_weight_decay.CurrentWeightDecay();
    p->clear();
    pi++;
  }

  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& vm = lm[pi].h;
    vector<Tensor>& vv = lv[pi].h;
    for (auto i : p->non_zero_grads) {
      auto m_t = vm[i].vec();
      auto v_t = vv[i].vec();
      auto g_t = scale * gscale * p->grads[i].vec();
      auto g2 = g_t.cwiseProduct(g_t);
      m_t = beta_1 * m_t + (1 - beta_1) * g_t;
      v_t = beta_2 * v_t + (1 - beta_2) * g2;
      float s1 = 1 - pow(beta_1, t);
      float s2 = 1 - pow(beta_2, t);
      auto mhat = m_t / s1;
      auto vhat = v_t / s2;
      auto delta = (-eta * mhat).cwiseQuotient((vhat.array().sqrt() + eps).matrix());
      p->values[i].vec() += delta / global_weight_decay.CurrentWeightDecay();
    }
    p->clear();
    pi++;
  }
  ++updates;
}

} // namespace cnn

#ifndef CNN_LSTM2_H_
#define CNN_LSTM2_H_

#include "cnn/cnn.h"
#include "cnn/edges.h"

namespace cnn {

class Model;

struct LSTMBuilder2 {
  LSTMBuilder2() {}
  explicit LSTMBuilder2(unsigned layers,
                       unsigned input_dim,
                       unsigned hidden_dim,
                       Model* model);

  // call this to reset the builder when you are going to create
  // a new computation graph
  void new_graph();

  // call this before add_input
  void add_parameter_edges(Hypergraph* hg);

  // call this before input and
  // initialize c and h to given values at each layer
  void add_parameter_edges(Hypergraph* hg,
                           vector<VariableIndex> c_0,
                           vector<VariableIndex> h_0);

  // add another timestep by reading in the variable x
  // return the hidden representation of the deepest layer
  VariableIndex add_input(VariableIndex x, Hypergraph* hg);

  // rewind the last timestep - this DOES NOT remove the variables
  // from the computation graph, it just means the next time step will
  // see a different previous state. You can remind as many times as
  // you want.
  void rewind_one_step() {
    h.pop_back();
    c.pop_back();
  }

  // returns node index (variable) of most recent output
  VariableIndex back() const { return h.back().back(); }

  // check to make sure parameters have been added before adding input
  unsigned builder_state;

  // first index is layer, then ...
  std::vector<std::vector<Parameters*>> params;

  // first index is layer, then ...
  std::vector<std::vector<VariableIndex>> param_vars;

  // first index is time, second is layer 
  std::vector<std::vector<VariableIndex>> h, c;

  // initial values of h and c at each layer
  // - both default to zero input
  std::vector<VariableIndex> h0;
  std::vector<VariableIndex> c0;
  unsigned hidden_dim;
  unsigned layers;
};

} // namespace cnn

#endif

// Copyright 2021 Pierre Talbot, Frédéric Pinel

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MODEL_BUILDER_HPP
#define MODEL_BUILDER_HPP

#include <vector>
#include <map>
#include <tuple>
#include <exception>

#include "XCSP3Tree.h"
#include "XCSP3TreeNode.h"
#include "XCSP3Variable.h"
#include "XCSP3Constants.h"

#include "vstore.cuh"
#include "propagators.cuh"

using namespace XCSP3Core;

class ModelBuilder {
  private:
    std::map<std::string, std::tuple<Var, Interval>> var2idx;
    std::vector<std::string> idx2var;
    Constraints constraints;
    Var minimize_obj;

  private:
    Propagator* make_temporal_constraint(std::string x, int k, OrderType op, std::string y) {

      Var xi = std::get<0>(var2idx[x]);
      Var yi = std::get<0>(var2idx[y]);
      assert(xi != 0 && yi != 0);

      // Turn > and < into <= and >=
      if (op == LT) {
        op = LE;
        k = k - 1;
      }
      else if (op == GT) {
        op = GE;
        k = k + 1;
      }

      if (op == LE) {
        yi = -yi;
        k = -k;
      }
      else if (op == GE) {
        xi = -xi;
      }
      else if (op == EQ) {
        Propagator* p1 = make_temporal_constraint(x, k, LE, y);
        Propagator* p2 = make_temporal_constraint(x, k, GE, y);
        return new LogicalAnd(p1, p2);
      }
      else if (op == IN || op == NE) {
        throw std::runtime_error("Operators IN and NE are not supported in unary constraints.");
      }
      return new TemporalProp(xi, yi, k);
    }

  public:
    ModelBuilder() {
      // Negative variable's indices are used for negative view of variable, e.g., `-x`.
      // However the index `0` can't be negated, so we occupy this position with a dumb value.
      add_var("zero_var(fake)", 0, 0);
    }

    void add_var(std::string name, int min, int max) {
      Var idx = idx2var.size();
      idx2var.push_back(name);
      var2idx[name] = std::make_tuple(idx, Interval(min,max));
    }

    void add_constraint(Tree *tree) {
      if (tree->arity() == 1) {
        strengthen_domain_from_node(tree->root);
      }
      // b <=> (x < y /\ y - x >= 1)
      else if(tree->root->type == OIFF) {
        add_reified_constraint(tree->root);
      }
      else if(tree->root->type == OLE) {
        add_linear_constraint(tree->root);
      }
      else {
        throw std::runtime_error("Unsupported constraint.");
      }
    }

    VStore* build_store() {
      alignas(VStore) unsigned char *v;
      malloc2_managed(v, sizeof(VStore));
      VStore* vstore = new(v) VStore(idx2var.size());
      for (const auto& x : var2idx) {
        vstore->dom(std::get<0>(x.second), std::get<1>(x.second));
      }
      vstore->init_names(idx2var);
      return vstore;
    }

    Constraints build_constraints() {
      constraints.init_uids();
      return constraints;
    }

    Var build_minimize_obj() {
      return minimize_obj;
    }

    // x <op> k
    void strengthen_domain(XVariable *x, OrderType op, int k) {
      strengthen_domain2(x->id, op, k);
    }

    // x + k <op> y
    void add_temporal_constraint(XVariable *x, int k, OrderType op, XVariable *y) {
      constraints.propagators.push_back(make_temporal_constraint(x->id, k, op, y->id));
    }

    void add_objective_minimize(XVariable *x) {
      minimize_obj = std::get<0>(var2idx[x->id]);
    }

  private:
    void strengthen_domain2(std::string x, OrderType op, int k) {
      Interval& v = std::get<1>(var2idx[x]);
      if(op == LT) {
        v.ub = k - 1;
      }
      if (op == GT) {
        v.lb = k + 1;
      }
      if(op == LE || op == EQ) {
        v.ub = k;
      }
      if (op == GE || op == EQ) {
        v.lb = k;
      }
      if (op == IN || op == NE) {
        throw std::runtime_error("Operators IN and NE are not supported in unary constraints.");
      }
    }

    // Transform X * 1 into X.
    void evaluate_constant(Node** node_ptr) {
      Node* node = *node_ptr;
      if(node->type == OMUL) {
        if(node->parameters[0]->type == OVAR && node->parameters[1]->type == ODECIMAL) {
          if(((NodeConstant*)node->parameters[1])->val == 1) {
            *node_ptr = node->parameters[0];
          }
        }
      }
    }

    // Treat constraint of the form X * a <= b.
    bool le_mul_domain(Node* node) {
      if (node->type == OLE &&
          node->parameters[0]->type == OMUL &&
          node->parameters[1]->type == ODECIMAL &&
          node->parameters[0]->parameters[0]->type == OVAR &&
          node->parameters[0]->parameters[1]->type == ODECIMAL)
      {
        std::string x = node->parameters[0]->parameters[0]->toString();
        int a = ((NodeConstant*)node->parameters[0]->parameters[1])->val;
        int b = ((NodeConstant*)node->parameters[1])->val;
        if(a == 0) {
          if(b < 0) {
            add_var("fake var (contradiction detected at root node)", 1, 0);
          }
        }
        else if (b == 0) {
          if(a > 0) {
            strengthen_domain2(x, LE, 0);
          }
          else if (a < 0) {
            strengthen_domain2(x, GE, 0);
          }
        }
        else {
          // At this point, a and b are different from 0.
          int res = b / a;
          if(a > 0 && b > 0) {
            strengthen_domain2(x, LE, res);
          }
          else if(a > 0 && b < 0) {
            strengthen_domain2(x, LE, res - (-b % a));
          }
          else if(a < 0 && b > 0) {
            strengthen_domain2(x, GE, res);
          }
          else {
            strengthen_domain2(x, GE, res + (-b % -a));
          }
        }
        return true;
      }
      return false;
    }

    void strengthen_domain_from_node(Node* node) {
      if (node->parameters.size() != 2) {
        throw std::runtime_error("Expected binary constraints.");
      }
      bool treated = le_mul_domain(node);
      if(!treated) {
        if (node->parameters[0]->type != OVAR) {
          evaluate_constant(&node->parameters[0]);
          if (node->parameters[0]->type != OVAR) {
            std::cout << node->toString() << std::endl;
            throw std::runtime_error("Expected variable on the lhs (in domain constraint).");
          }
        }
        if (node->parameters[1]->type != ODECIMAL) {
          throw std::runtime_error("Expected value on the rhs.");
        }
        std::string x = node->parameters[0]->toString();
        int v = dynamic_cast<NodeConstant*>(node->parameters[1])->val;
        OrderType op;
        if (node->type == OLE) { op = LE; }
        else if (node->type == OLT) { op = LT; }
        else if (node->type == OGE) { op = GE; }
        else if (node->type == OGT) { op = GT; }
        else if (node->type == OEQ) { op = EQ; }
        else if (node->type == ONE) { op = NE; }
        else if (node->type == OIN) { op = IN; }
        else {
          throw std::runtime_error("Unsupported unary domain operator.");
        }
        strengthen_domain2(x, op, v);
      }
    }

    // The node must have a very precise shape, X <= Y or X <= Y + k, otherwise a runtime_error is thrown.
    Propagator* make_temporal_constraint_from_node(Node* node) {
      if (node->type == OLE) {
        if (node->parameters.size() != 2) {
          throw std::runtime_error("Expected binary constraints.");
        }
        if (node->parameters[0]->type != OVAR) {
          throw std::runtime_error("Expected variable on the lhs (in temporal constraint).");
        }
        std::string x = node->parameters[0]->toString();
        if (node->parameters[1]->type == OVAR) {
          std::string y = node->parameters[1]->toString();
          return make_temporal_constraint(x, 0, LE, y);
        }
        else if (node->parameters[1]->type == OADD) {
          Node* add = node->parameters[1];
          if (add->parameters[0]->type != OVAR || add->parameters[1]->type != ODECIMAL) {
            throw std::runtime_error("Expected <var> + <constant>.");
          }
          std::string y = add->parameters[0]->toString();
          int k = dynamic_cast<NodeConstant*>(add->parameters[1])->val;
          return make_temporal_constraint(x, -k, LE, y);
        }
        else {
          std::cout << node->toString() << std::endl;
          throw std::runtime_error("Expected rhs of type OADD or OVAR.");
        }
      }
      else {
        throw std::runtime_error("Expect node in canonized form. TemporalProp constraint of the form x <= y + k");
      }
    }

    bool tautology(Node* node) {
      if(node->parameters[0]->type == ODECIMAL &&
         node->parameters[1]->type == ODECIMAL) {
        if(((NodeConstant*)node->parameters[0])->val <= ((NodeConstant*)node->parameters[1])->val) {
          return true;
        }
        else {
          add_var("fake var (contradiction detected at root node)", 1, 0);
          return true;
        }
      }
      return false;
    }

    void add_linear_constraint(Node* node) {
      if (node->parameters.size() != 2) {
        throw std::runtime_error("Expected comparison operator with two arguments.");
      }
      if(!tautology(node)) {
        if (node->parameters[0]->type != OADD || node->parameters[1]->type != ODECIMAL) {
          std::cout << node->toString() << std::endl;
          throw std::runtime_error("Expected linear constraint of the form x1c1 + ... + xNcN <= c");
        }
        int c = dynamic_cast<NodeConstant*>(node->parameters[1])->val;
        std::vector<Var> vars;
        std::vector<int> constants;
        for (Node* n : node->parameters[0]->parameters) {
          if (n->type != OMUL || n->parameters[0]->type != OVAR || n->parameters[1]->type != ODECIMAL) {
            throw std::runtime_error("Expected sum of factors x1c1 + ... xNcN");
          }
          vars.push_back(std::get<0>(var2idx[n->parameters[0]->toString()]));
          constants.push_back(dynamic_cast<NodeConstant*>(n->parameters[1])->val);
        }
        constraints.propagators.push_back(new LinearIneq(vars, constants, c));
      }
    }

    void add_reified_constraint(Node* node) {
      if (node->parameters[0]->type == OVAR &&
          node->parameters[1]->type == OAND) {
        std::string b = node->parameters[0]->toString();
        NodeAnd* and_node = dynamic_cast<NodeAnd*>(node->parameters[1]);
        Propagator* p1 = make_temporal_constraint_from_node(and_node->parameters[0]);
        Propagator* p2 = make_temporal_constraint_from_node(and_node->parameters[1]);
        Propagator* rhs = new LogicalAnd(p1, p2);
        constraints.propagators.push_back(new ReifiedProp(std::get<0>(var2idx[b]), rhs));
      }
      else if (node->parameters[0]->type == OAND &&
        node->parameters[1]->type == OVAR) {
        std::swap(node->parameters[0], node->parameters[1]);
        add_reified_constraint(node);
      }
      else {
        throw std::runtime_error("Expected reified constraint of the form  b <=> (c1 /\\ c2)");
      }
    }
};

#endif

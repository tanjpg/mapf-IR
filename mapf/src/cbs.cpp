#include "../include/cbs.hpp"

const std::string CBS::SOLVER_NAME = "CBS";

CBS::CBS(Problem* _P) : Solver(_P)
{
  solver_name = CBS::SOLVER_NAME;
}

void CBS::run()
{
  // set objective function
  CompareHighLevelNodes compare = getObjective();

  // OPEN
  std::priority_queue<HighLevelNode_p,
                      std::vector<HighLevelNode_p>,
                      decltype(compare)> HighLevelTree(compare);

  HighLevelNode_p n(new HighLevelNode);
  setInitialHighLevelNode(n);
  if (!n->valid) return;  // failed to plan initial paths
  HighLevelTree.push(n);

  int h_node_num = 1;
  int iteration = 0;
  while (!HighLevelTree.empty()) {
    ++iteration;
    // check limitation
    if (overCompTime()) {
      info(" ", "timeout");
      break;
    }

    n = HighLevelTree.top();
    HighLevelTree.pop();

    info(" ",
         "elapsed:", getSolverElapsedTime(),
         ", explored_node_num:", iteration,
         ", nodes_num:", h_node_num,
         ", conflicts:", n->f,
         ", constraints:", n->constraints.size(),
         ", soc:", n->soc);

    // check conflict
    LibCBS::Constraints constraints =
      LibCBS::getFirstConstraints(n->paths);
    if (constraints.empty()) {
      solved = true;
      break;
    }

    // create new nodes
    for (auto c : constraints) {
      LibCBS::Constraints new_constraints = n->constraints;
      new_constraints.push_back(c);
      HighLevelNode_p m(new HighLevelNode{
          h_node_num,
          n->paths,
          new_constraints,
          n->makespan,
          n->soc,
          n->f,  // # conflicts
          true });
      invoke(m, c->id);
      if (!m->valid) continue;
      HighLevelTree.push(m);
      ++h_node_num;
    }
  }

  if (solved) solution = pathsToPlan(n->paths);
}

CBS::CompareHighLevelNodes CBS::getObjective()
{
  CompareHighLevelNodes compare =
    [] (HighLevelNode_p a, HighLevelNode_p b)
    {
     if (a->soc != b->soc) return a->soc > b->soc;
     if (a->f != b->f) return a->f > b->f;  // tie-breaker
     return false;
    };
  return compare;
}

void CBS::setInitialHighLevelNode(HighLevelNode_p n)
{
  if (n->paths.empty()) {
    Paths paths(P->getNum());
    for (int i = 0; i < P->getNum(); ++i) {
      Path path = getInitialPath(i);
      if (path.empty()) {
        n->valid = false;
        return;
      }
      paths.insert(i, path);
    }
    n->paths = paths;
  }

  n->id = 0;
  n->constraints = {};  // constraints
  n->makespan = n->paths.getMakespan();
  n->soc = n->paths.getSOC();
  n->f = n->paths.countConflict();
  n->valid = true;  // valid
}

Path CBS::getInitialPath(int id)
{
  Node* s = P->getStart(id);
  Node* g = P->getGoal(id);
  Nodes config_g = P->getConfigGoal();

  AstarHeuristics fValue =
    [&] (AstarNode* n) { return n->g + pathDist(n->v, g); };

  CompareAstarNode compare =
    [&] (AstarNode* a, AstarNode* b) {
      if (a->f != b->f) return a->f > b->f;
      // IMPORTANT! Avoid goal locations of others.
      if (a->v != g && inArray(a->v, config_g)) return true;
      if (b->v != g && inArray(b->v, config_g)) return false;
      if (a->g != b->g) return a->g < b->g;
      return false;
    };

  CheckAstarFin checkAstarFin = [&] (AstarNode* n) { return n->v == g; };

  CheckInvalidAstarNode checkInvalidAstarNode =
    [&] (AstarNode* m) {  return false; };

  return getTimedPath(s, g,
                      fValue,
                      compare,
                      checkAstarFin,
                      checkInvalidAstarNode);
}

void CBS::invoke(HighLevelNode_p h_node, int id)
{
  Path path = getConstrainedPath(h_node, id);
  if (path.empty()) {
    h_node->valid = false;
    return;
  }
  Paths paths = h_node->paths;
  paths.insert(id, path);
  h_node->f = h_node->f
    - h_node->paths.countConflict(id, h_node->paths.get(id))
    + h_node->paths.countConflict(id, paths.get(id));
  h_node->paths = paths;
  h_node->makespan = h_node->paths.getMakespan();
  h_node->soc = h_node->paths.getSOC();
}

Path CBS::getConstrainedPath(HighLevelNode_p h_node, int id)
{
  Node* s = P->getStart(id);
  Node* g = P->getGoal(id);

  LibCBS::Constraints constraints;
  int max_constraint_time = 0;
  for (auto c : h_node->constraints) {
    if (c->id == id) {
      constraints.push_back(c);
      if (c->v == g && c->u == nullptr) {
        max_constraint_time = std::max(max_constraint_time, c->t);
      }
    }
  }

  AstarHeuristics fValue;
  if (h_node->paths.costOfPath(id) > max_constraint_time) {
    fValue = [&] (AstarNode* n) { return n->g + pathDist(n->v, g); };
  } else {
    fValue =
      [&] (AstarNode* n) {
        return std::max(n->g, max_constraint_time) + pathDist(n->v, g);
      };
  }

  CompareAstarNode compare =
    [&] (AstarNode* a, AstarNode* b) {
      if (a->f != b->f) return a->f > b->f;
      if (a->g != b->g) return a->g < b->g;
      // avoid conflict with others
      for (int i = 0; i < P->getNum(); ++i) {
        if (i == id) continue;
        if (a->g <= h_node->makespan &&
            h_node->paths.get(i, a->g) == a->v) return true;
        if (b->g <= h_node->makespan &&
            h_node->paths.get(i, b->g) == b->v) return false;
      }
      return false;
    };

  CheckAstarFin checkAstarFin =
    [&] (AstarNode* n) {
      return n->v == g && n->g > max_constraint_time;
    };

  CheckInvalidAstarNode checkInvalidAstarNode =
    [&] (AstarNode* m) {
      for (auto c : constraints) {
        if (m->g == c->t && m->v == c->v) {
          // vertex or swap conflict
          if (c->u == nullptr || c->u == m->p->v) return true;
        }
      }
      return false;
    };

  return getTimedPath(s, g,
                      fValue,
                      compare,
                      checkAstarFin,
                      checkInvalidAstarNode);
}

void CBS::printHelp()
{
  std::cout << CBS::SOLVER_NAME << "\n"
            << "  (no option)"
            << std::endl;
}

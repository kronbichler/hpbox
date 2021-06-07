// ---------------------------------------------------------------------
//
// Copyright (C) 2021 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------

#ifndef operator_base_h
#define operator_base_h


#include <solver/mg_solver.h>


namespace Operator
{
  template <int dim,
            typename VectorType =
              dealii::LinearAlgebra::distributed::Vector<double>,
            int spacedim = dim>
  class Base : public dealii::MGSolverOperatorBase<dim, VectorType>
  {
  public:
    using value_type = typename VectorType::value_type;

    virtual void
    reinit(const dealii::DoFHandler<dim, spacedim> &    dof_handler,
           const dealii::AffineConstraints<value_type> &constraints,
           VectorType &                                 system_rhs) = 0;
  };
} // namespace Operator


#endif

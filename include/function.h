// ---------------------------------------------------------------------
//
// Copyright (C) 2020 - 2022 by the deal.II authors
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

#ifndef function_h
#define function_h


#include <deal.II/base/exceptions.h>
#include <deal.II/base/function.h>


namespace Function
{
  template <int dim>
  class ReentrantCorner : public dealii::Function<dim>
  {
  public:
    ReentrantCorner(const double alpha = 2. / 3.);

    virtual double
    value(const dealii::Point<dim> &p,
          const unsigned int        component = 0) const override;

    virtual dealii::Tensor<1, dim>
    gradient(const dealii::Point<dim> &p,
             const unsigned int        component = 0) const override;

  private:
    const double alpha;
  };

  /**
   * Function from step-55.
   */
  template <int dim>
  class KovasznayExact : public dealii::Function<dim>
  {
  public:
    KovasznayExact();

    virtual void
    vector_value(const dealii::Point<dim> &p,
                 dealii::Vector<double>   &values) const override;
  };

  template <int dim>
  class KovasznayRHS : public dealii::Function<dim>
  {
  public:
    KovasznayRHS();

    virtual void
    vector_value(const dealii::Point<dim> &p,
                 dealii::Vector<double>   &values) const override;
  };
} // namespace Function


#endif
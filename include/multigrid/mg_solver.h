// ---------------------------------------------------------------------
//
// Copyright (C) 2021 - 2024 by the deal.II authors
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

#ifndef multigrid_mg_solver_h
#define multigrid_mg_solver_h


#include <deal.II/base/config.h>

#include <deal.II/base/convergence_table.h>
#include <deal.II/base/mg_level_object.h>
#include <deal.II/base/signaling_nan.h>

#include <deal.II/lac/diagonal_matrix.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/solver_control.h>

#ifdef DEAL_II_WITH_TRILINOS
#  include <deal.II/lac/trilinos_precondition.h>
#endif

#include <deal.II/multigrid/mg_coarse.h>
#include <deal.II/multigrid/mg_matrix.h>
#include <deal.II/multigrid/mg_smoother.h>
#include <deal.II/multigrid/multigrid.h>

#include <global.h>
#include <multigrid/operator_base.h>
#include <multigrid/parameter.h>

#include <vector>


// NOTE:
// This section is modified from WIP PR #11699 which determines the interface
// of the MGSolverOperatorBase class.


DEAL_II_NAMESPACE_OPEN

template <typename VectorType,
          int dim,
          int spacedim,
          typename SystemMatrixType,
          typename LevelMatrixType,
          typename SmootherPreconditionerType,
          typename MGTransferType>
static void
mg_solve(
  SolverControl                                                    &solver_control,
  VectorType                                                       &dst,
  const VectorType                                                 &src,
  const MGSolverParameters                                         &mg_data,
  const DoFHandler<dim, spacedim>                                  &dof,
  const SystemMatrixType                                           &fine_matrix,
  const MGLevelObject<std::unique_ptr<LevelMatrixType>>            &mg_matrices,
  const MGLevelObject<std::shared_ptr<SmootherPreconditionerType>> &mg_smoother_preconditioners,
  const MGTransferType                                             &mg_transfer,
  const std::string                                                &filename_mg_level)
{
  AssertThrow(mg_data.smoother.type == "chebyshev", ExcNotImplemented());

  const unsigned int min_level = mg_matrices.min_level();
  const unsigned int max_level = mg_matrices.max_level();

  using SmootherType =
    PreconditionChebyshev<LevelMatrixType, VectorType, SmootherPreconditionerType>;
  using PreconditionerType = PreconditionMG<dim, VectorType, MGTransferType>;

  // Initialize level operators.
  mg::Matrix<VectorType> mg_matrix(mg_matrices);

  // Initialize smoothers.
  MGLevelObject<typename SmootherType::AdditionalData> smoother_data(min_level, max_level);

  for (unsigned int level = min_level; level <= max_level; level++)
    {
      smoother_data[level].preconditioner      = mg_smoother_preconditioners[level];
      smoother_data[level].smoothing_range     = mg_data.smoother.smoothing_range;
      smoother_data[level].degree              = mg_data.smoother.degree;
      smoother_data[level].eig_cg_n_iterations = mg_data.smoother.eig_cg_n_iterations;
    }

  // ----------
  // Estimate eigenvalues on all levels, i.e., all operators
  // TODO: based on peter's code
  // https://github.com/peterrum/dealii-asm/blob/d998b9b344a19c9d2890e087f953c2f93e6546ae/include/precondition.templates.h#L292-L316
  std::vector<double> min_eigenvalues(max_level + 1, numbers::signaling_nan<double>());
  std::vector<double> max_eigenvalues(max_level + 1, numbers::signaling_nan<double>());
  if (mg_data.estimate_eigenvalues == true)
    {
      for (unsigned int level = min_level + 1; level <= max_level; level++)
        {
          SmootherType chebyshev;
          chebyshev.initialize(*mg_matrices[level], smoother_data[level]);

          VectorType vec;
          mg_matrices[level]->initialize_dof_vector(vec);
          const auto evs = chebyshev.estimate_eigenvalues(vec);

          min_eigenvalues[level] = evs.min_eigenvalue_estimate;
          max_eigenvalues[level] = evs.max_eigenvalue_estimate;

          // We already computed eigenvalues, reset the one in the actual smoother
          smoother_data[level].eig_cg_n_iterations = 0;
          smoother_data[level].max_eigenvalue      = evs.max_eigenvalue_estimate * 1.1;
        }

      // log maximum over all levels
      const double max = *std::max_element(++(max_eigenvalues.begin()), max_eigenvalues.end());
      getPCOut() << "   Max EV on all MG levels:      " << max << std::endl;
      getTable().add_value("max_ev", max);
    }
  // ----------

  MGSmootherRelaxation<LevelMatrixType, SmootherType, VectorType> mg_smoother;
  mg_smoother.initialize(mg_matrices, smoother_data);

  // Initialize coarse-grid solver.
  ReductionControl     coarse_grid_solver_control(mg_data.coarse_solver.maxiter,
                                              mg_data.coarse_solver.abstol,
                                              mg_data.coarse_solver.reltol,
                                              /*log_history=*/true,
                                              /*log_result=*/true);
  SolverCG<VectorType> coarse_grid_solver(coarse_grid_solver_control);

  PreconditionIdentity precondition_identity;
  PreconditionChebyshev<LevelMatrixType, VectorType, DiagonalMatrix<VectorType>>
    precondition_chebyshev;

#ifdef DEAL_II_WITH_TRILINOS
  TrilinosWrappers::PreconditionAMG precondition_amg;
#endif

  std::unique_ptr<MGCoarseGridBase<VectorType>> mg_coarse;

  if (mg_data.coarse_solver.type == "cg")
    {
      // CG with identity matrix as preconditioner

      mg_coarse =
        std::make_unique<MGCoarseGridIterativeSolver<VectorType,
                                                     SolverCG<VectorType>,
                                                     LevelMatrixType,
                                                     PreconditionIdentity>>(coarse_grid_solver,
                                                                            *mg_matrices[min_level],
                                                                            precondition_identity);
    }
  else if (mg_data.coarse_solver.type == "cg_with_chebyshev")
    {
      // CG with Chebyshev as preconditioner

      typename decltype(precondition_chebyshev)::AdditionalData smoother_data;

      smoother_data.preconditioner = std::make_shared<DiagonalMatrix<VectorType>>();
      mg_matrices[min_level]->compute_inverse_diagonal(smoother_data.preconditioner->get_vector());
      smoother_data.smoothing_range     = mg_data.smoother.smoothing_range;
      smoother_data.degree              = mg_data.smoother.degree;
      smoother_data.eig_cg_n_iterations = mg_data.smoother.eig_cg_n_iterations;

      precondition_chebyshev.initialize(*mg_matrices[min_level], smoother_data);

      mg_coarse = std::make_unique<MGCoarseGridIterativeSolver<VectorType,
                                                               SolverCG<VectorType>,
                                                               LevelMatrixType,
                                                               decltype(precondition_chebyshev)>>(
        coarse_grid_solver, *mg_matrices[min_level], precondition_chebyshev);
    }
  else if (mg_data.coarse_solver.type == "cg_with_amg")
    {
      // CG with AMG as preconditioner

#ifdef DEAL_II_WITH_TRILINOS
      TrilinosWrappers::PreconditionAMG::AdditionalData amg_data;
      amg_data.smoother_sweeps = mg_data.coarse_solver.smoother_sweeps;
      amg_data.n_cycles        = mg_data.coarse_solver.n_cycles;
      amg_data.smoother_type   = mg_data.coarse_solver.smoother_type.c_str();

      // CG with AMG as preconditioner
      precondition_amg.initialize(mg_matrices[min_level]->get_system_matrix(), amg_data);

      mg_coarse = std::make_unique<MGCoarseGridIterativeSolver<VectorType,
                                                               SolverCG<VectorType>,
                                                               LevelMatrixType,
                                                               decltype(precondition_amg)>>(
        coarse_grid_solver, *mg_matrices[min_level], precondition_amg);
#else
      AssertThrow(false, ExcNotImplemented());
#endif
    }
  else
    {
      AssertThrow(false, ExcNotImplemented());
    }

  // Create multigrid object.
  Multigrid<VectorType> mg(mg_matrix, *mg_coarse, mg_transfer, mg_smoother, mg_smoother);

  // ----------
  // TODO: timing based on peters dealii-multigrid
  // https://github.com/peterrum/dealii-multigrid/blob/c50581883c0dbe35c83132699e6de40da9b1b255/multigrid_throughput.cc#L1183-L1192
  std::vector<std::vector<std::pair<double, std::chrono::time_point<std::chrono::system_clock>>>>
    all_mg_timers(max_level - min_level + 1);

  for (unsigned int i = 0; i < all_mg_timers.size(); ++i)
    all_mg_timers[i].resize(7);

  if (mg_data.log_levels == true)
    {
      const auto create_mg_timer_function = [&](const unsigned int i, const std::string &label) {
        return [i, label, &all_mg_timers](const bool flag, const unsigned int level) {
          // if (false && flag)
          //   std::cout << label << " " << level << std::endl;
          if (flag)
            all_mg_timers[level][i].second = std::chrono::system_clock::now();
          else
            all_mg_timers[level][i].first +=
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now() - all_mg_timers[level][i].second)
                .count() /
              1e9;
        };
      };

      mg.connect_pre_smoother_step(create_mg_timer_function(0, "pre_smoother_step"));
      mg.connect_residual_step(create_mg_timer_function(1, "residual_step"));
      mg.connect_restriction(create_mg_timer_function(2, "restriction"));
      mg.connect_coarse_solve(create_mg_timer_function(3, "coarse_solve"));
      mg.connect_prolongation(create_mg_timer_function(4, "prolongation"));
      mg.connect_edge_prolongation(create_mg_timer_function(5, "edge_prolongation"));
      mg.connect_post_smoother_step(create_mg_timer_function(6, "post_smoother_step"));
    }
  // ----------

  // Convert it to a preconditioner.
  PreconditionerType preconditioner(dof, mg, mg_transfer);

  // Finally, solve.
  SolverCG<VectorType>(solver_control).solve(fine_matrix, dst, src, preconditioner);

  // ----------
  // dump to Table and then file system
  if ((mg_data.log_levels == true) &&
      (Utilities::MPI::this_mpi_process(dof.get_communicator()) == 0))
    {
      dealii::ConvergenceTable table;
      for (unsigned int level = 0; level < all_mg_timers.size(); ++level)
        {
          table.add_value("level", level);
          table.add_value("pre_smoother_step", all_mg_timers[level][0].first);
          table.add_value("residual_step", all_mg_timers[level][1].first);
          table.add_value("restriction", all_mg_timers[level][2].first);
          table.add_value("coarse_solve", all_mg_timers[level][3].first);
          table.add_value("prolongation", all_mg_timers[level][4].first);
          table.add_value("edge_prolongation", all_mg_timers[level][5].first);
          table.add_value("post_smoother_step", all_mg_timers[level][6].first);
          if (mg_data.estimate_eigenvalues == true)
            {
              table.add_value("min_eigenvalue", min_eigenvalues[level]);
              table.add_value("max_eigenvalue", max_eigenvalues[level]);
            }
        }
      std::ofstream mg_level_stream(filename_mg_level);
      table.write_text(mg_level_stream);
    }
  // ----------
}

DEAL_II_NAMESPACE_CLOSE


#endif

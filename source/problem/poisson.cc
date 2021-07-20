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


#include <deal.II/fe/fe_q.h>

#include <deal.II/grid/grid_generator.h>

#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/vector_tools.h>

#include <adaptation/factory.h>
#include <function/factory.h>
#include <global.h>
#include <problem/poisson.h>
#include <solver/cg/amg.h>
#include <solver/cg/gmg.h>

#include <ctime>
#include <iomanip>
#include <sstream>

using namespace dealii;


namespace Problem
{
  template <int dim, int spacedim>
  Poisson<dim, spacedim>::Poisson(const Parameters &prm)
    : mpi_communicator(MPI_COMM_WORLD)
    , prm(prm)
    , triangulation(mpi_communicator)
    , dof_handler(triangulation)
  {
    TimerOutput::Scope t(getTimer(), "initialize_problem");

    // prepare name for logfile
    time_t             now = time(nullptr);
    tm *               ltm = localtime(&now);
    std::ostringstream oss;
    oss << prm.filestem << "-" << std::put_time(ltm, "%Y%m%d-%H%M%S") << ".log";
    filename_log = oss.str();

    // prepare collections
    mapping_collection.push_back(MappingQ1<dim>());

    for (unsigned int degree = 1; degree <= prm.prm_adaptation.max_p_degree;
         ++degree)
      {
        fe_collection.push_back(FE_Q<dim>(degree));
        quadrature_collection.push_back(QGauss<dim>(degree + 1));
      }

    const unsigned int min_fe_index = prm.prm_adaptation.min_p_degree - 1;
    fe_collection.set_hierarchy(
      /*next_index=*/
      [](const typename hp::FECollection<dim> &fe_collection,
         const unsigned int                    fe_index) -> unsigned int {
        return ((fe_index + 1) < fe_collection.size()) ? fe_index + 1 :
                                                         fe_index;
      },
      /*previous_index=*/
      [min_fe_index](const typename hp::FECollection<dim> &,
                     const unsigned int fe_index) -> unsigned int {
        Assert(fe_index >= min_fe_index,
               ExcMessage("Finite element is not part of hierarchy!"));
        return (fe_index > min_fe_index) ? fe_index - 1 : fe_index;
      });

    // prepare operator (and fe values)
    // TODO: Maybe move this to separate factory function?
    if (prm.operator_type == "MatrixBased")
      {
        {
          TimerOutput::Scope t(getTimer(), "calculate_fevalues");

          fe_values_collection =
            std::make_unique<hp::FEValues<dim>>(mapping_collection,
                                                fe_collection,
                                                quadrature_collection,
                                                update_values |
                                                  update_gradients |
                                                  update_quadrature_points |
                                                  update_JxW_values);
          fe_values_collection->precalculate_fe_values();
        }

        poisson_operator_matrixbased =
          std::make_unique<Operator::Poisson::MatrixBased<
            dim,
            LinearAlgebra::distributed::Vector<double>,
            spacedim>>(mapping_collection,
                       quadrature_collection,
                       *fe_values_collection);
      }
    else if (prm.operator_type == "MatrixFree")
      {
        poisson_operator_matrixfree =
          std::make_unique<Operator::Poisson::MatrixFree<
            dim,
            LinearAlgebra::distributed::Vector<double>,
            spacedim>>(mapping_collection, quadrature_collection);
      }
    else
      {
        Assert(false, ExcNotImplemented());
      }

    // choose functions
    boundary_function = Factory::create_function<dim>("reentrant corner");
    solution_function = Factory::create_function<dim>("reentrant corner");
    rhs_function      = Factory::create_function<dim>("zero");

    // choose adaptation strategy
    adaptation_strategy =
      Factory::create_adaptation<dim>(prm.adaptation_type,
                                      prm.prm_adaptation,
                                      locally_relevant_solution,
                                      fe_collection,
                                      dof_handler,
                                      triangulation);
  }



  template <int dim, int spacedim>
  void
  Poisson<dim, spacedim>::initialize_grid()
  {
    TimerOutput::Scope t(getTimer(), "initialize_grid");

    std::vector<unsigned int> repetitions(dim);
    Point<dim>                bottom_left, top_right;
    for (unsigned int d = 0; d < dim; ++d)
      if (d < 2)
        {
          repetitions[d] = 2;
          bottom_left[d] = -1.;
          top_right[d]   = 1.;
        }
      else
        {
          repetitions[d] = 1;
          bottom_left[d] = 0.;
          top_right[d]   = 1.;
        }

    std::vector<int> cells_to_remove(dim, 1);
    cells_to_remove[0] = -1;

    GridGenerator::subdivided_hyper_L(
      triangulation, repetitions, bottom_left, top_right, cells_to_remove);

    triangulation.refine_global(
      adaptation_strategy->get_n_initial_refinements());

    const unsigned int min_fe_index = prm.prm_adaptation.min_p_degree - 1;
    for (const auto &cell : dof_handler.active_cell_iterators())
      if (cell->is_locally_owned())
        cell->set_active_fe_index(min_fe_index);
  }



  template <int dim, int spacedim>
  void
  Poisson<dim, spacedim>::setup_system()
  {
    TimerOutput::Scope t(getTimer(), "setup");

    dof_handler.distribute_dofs(fe_collection);

    locally_owned_dofs = dof_handler.locally_owned_dofs();
    DoFTools::extract_locally_relevant_dofs(dof_handler, locally_relevant_dofs);

    locally_relevant_solution.reinit(locally_owned_dofs,
                                     locally_relevant_dofs,
                                     mpi_communicator);
    system_rhs.reinit(locally_owned_dofs, mpi_communicator);

    constraints.clear();
    constraints.reinit(locally_relevant_dofs);
    DoFTools::make_hanging_node_constraints(dof_handler, constraints);
    VectorTools::interpolate_boundary_values(
      mapping_collection, dof_handler, 0, *boundary_function, constraints);

#ifdef DEBUG
    // We have not dealt with chains of constraints on ghost cells yet.
    // Thus, we are content with verifying their consistency for now.
    std::vector<IndexSet> locally_owned_dofs_per_processor =
      Utilities::MPI::all_gather(mpi_communicator,
                                 dof_handler.locally_owned_dofs());

    IndexSet locally_active_dofs;
    DoFTools::extract_locally_active_dofs(dof_handler, locally_active_dofs);

    AssertThrow(
      constraints.is_consistent_in_parallel(locally_owned_dofs_per_processor,
                                            locally_active_dofs,
                                            mpi_communicator,
                                            /*verbose=*/true),
      ExcMessage("AffineConstraints object contains inconsistencies!"));
#endif
    constraints.close();
  }



  template <int dim, int spacedim>
  void
  Poisson<dim, spacedim>::log_diagnostics()
  {
    dealii::ConditionalOStream &pcout = getPCOut();
    dealii::TableHandler &      table = getTable();

    const unsigned int n_processes =
      Utilities::MPI::n_mpi_processes(mpi_communicator);
    table.add_value("n_procs", n_processes);

    const unsigned int first_n_processes =
      std::min<unsigned int>(8, n_processes);
    const bool output_cropped = first_n_processes < n_processes;

    {
      pcout << "   Number of active cells:       "
            << triangulation.n_global_active_cells() << std::endl;
      table.add_value("active_cells", triangulation.n_global_active_cells());

      pcout << "     by partition:              ";
      std::vector<unsigned int> n_active_cells_per_subdomain =
        Utilities::MPI::gather(mpi_communicator,
                               triangulation.n_locally_owned_active_cells());
      for (unsigned int i = 0; i < first_n_processes; ++i)
        pcout << ' ' << n_active_cells_per_subdomain[i];
      if (output_cropped)
        pcout << " ...";
      pcout << std::endl;
    }

    {
      pcout << "   Number of degrees of freedom: " << dof_handler.n_dofs()
            << std::endl;
      table.add_value("dofs", dof_handler.n_dofs());

      pcout << "     by partition:              ";
      std::vector<types::global_dof_index> n_dofs_per_subdomain =
        Utilities::MPI::gather(mpi_communicator,
                               dof_handler.n_locally_owned_dofs());
      for (unsigned int i = 0; i < first_n_processes; ++i)
        pcout << ' ' << n_dofs_per_subdomain[i];
      if (output_cropped)
        pcout << " ...";
      pcout << std::endl;
    }

    {
      std::vector<types::global_dof_index> n_constraints_per_subdomain =
        Utilities::MPI::gather(mpi_communicator, constraints.n_constraints());
      const unsigned int n_constraints =
        std::accumulate(n_constraints_per_subdomain.begin(),
                        n_constraints_per_subdomain.end(),
                        0);

      pcout << "   Number of constraints:        " << n_constraints
            << std::endl;
      table.add_value("constraints", n_constraints);

      pcout << "     by partition:              ";
      for (unsigned int i = 0; i < first_n_processes; ++i)
        pcout << ' ' << n_constraints_per_subdomain[i];
      if (output_cropped)
        pcout << " ...";
      pcout << std::endl;
    }

    {
      std::vector<unsigned int> n_fe_indices(fe_collection.size(), 0);
      for (const auto &cell : dof_handler.active_cell_iterators())
        if (cell->is_locally_owned())
          n_fe_indices[cell->active_fe_index()]++;

      Utilities::MPI::sum(n_fe_indices, mpi_communicator, n_fe_indices);

      pcout << "   Frequencies of poly. degrees:";
      for (unsigned int i = 0; i < fe_collection.size(); ++i)
        if (n_fe_indices[i] > 0)
          pcout << ' ' << fe_collection[i].degree << ":" << n_fe_indices[i];
      pcout << std::endl;
    }
  }



  template <int dim, int spacedim>
  template <typename OperatorType>
  void
  Poisson<dim, spacedim>::solve(
    const OperatorType &                              system_matrix,
    LinearAlgebra::distributed::Vector<double> &      locally_relevant_solution,
    const LinearAlgebra::distributed::Vector<double> &system_rhs)
  {
    TimerOutput::Scope t(getTimer(), "solve");

    LinearAlgebra::distributed::Vector<double> completely_distributed_solution;
    LinearAlgebra::distributed::Vector<double>
      completely_distributed_system_rhs;

    system_matrix.initialize_dof_vector(completely_distributed_solution);
    system_matrix.initialize_dof_vector(completely_distributed_system_rhs);

    completely_distributed_system_rhs.copy_locally_owned_data_from(system_rhs);

    SolverControl solver_control(completely_distributed_system_rhs.size(),
                                 1e-12 *
                                   completely_distributed_system_rhs.l2_norm());

    if (prm.solver_type == "AMG")
      {
        Solver::CG::AMG::solve(solver_control,
                               system_matrix,
                               completely_distributed_solution,
                               completely_distributed_system_rhs);
      }
    else if (prm.solver_type == "GMG")
      {
        if constexpr (std::is_same<OperatorType,
                                   Operator::Poisson::MatrixFree<
                                     dim,
                                     LinearAlgebra::distributed::Vector<double>,
                                     spacedim>>::value)
          Solver::CG::GMG::solve(solver_control,
                                 system_matrix,
                                 completely_distributed_solution,
                                 completely_distributed_system_rhs,
                                 /*boundary_values=*/mapping_collection,
                                 dof_handler,
                                 /*operator_constructor=*/mapping_collection,
                                 quadrature_collection);
        else if constexpr (std::is_same<
                             OperatorType,
                             Operator::Poisson::MatrixBased<
                               dim,
                               LinearAlgebra::distributed::Vector<double>,
                               spacedim>>::value)
          Solver::CG::GMG::solve(solver_control,
                                 system_matrix,
                                 completely_distributed_solution,
                                 completely_distributed_system_rhs,
                                 /*boundary_values=*/mapping_collection,
                                 dof_handler,
                                 /*operator_constructor=*/mapping_collection,
                                 quadrature_collection,
                                 *fe_values_collection);
        else
          Assert(false, ExcNotImplemented());
      }
    else
      {
        Assert(false, ExcNotImplemented());
      }

    getPCOut() << "   Number of iterations:         "
               << solver_control.last_step() << std::endl;
    getTable().add_value("iteratations", solver_control.last_step());

    constraints.distribute(completely_distributed_solution);

    locally_relevant_solution.copy_locally_owned_data_from(
      completely_distributed_solution);
    locally_relevant_solution.update_ghost_values();
  }



  template <int dim, int spacedim>
  void
  Poisson<dim, spacedim>::compute_errors()
  {
    TimerOutput::Scope t(getTimer(), "compute_errors");

    Vector<float> difference_per_cell(triangulation.n_active_cells());
    VectorTools::integrate_difference(dof_handler,
                                      locally_relevant_solution,
                                      *solution_function,
                                      difference_per_cell,
                                      quadrature_collection,
                                      VectorTools::L2_norm);
    const double L2_error =
      VectorTools::compute_global_error(triangulation,
                                        difference_per_cell,
                                        VectorTools::L2_norm);

    VectorTools::integrate_difference(dof_handler,
                                      locally_relevant_solution,
                                      *solution_function,
                                      difference_per_cell,
                                      quadrature_collection,
                                      VectorTools::H1_norm);
    const double H1_error =
      VectorTools::compute_global_error(triangulation,
                                        difference_per_cell,
                                        VectorTools::H1_norm);

    getPCOut() << "   L2 error:                     " << L2_error << std::endl
               << "   H1 error:                     " << H1_error << std::endl;

    TableHandler &table = getTable();
    table.add_value("L2", L2_error);
    table.add_value("H1", H1_error);
    table.set_scientific("L2", true);
    table.set_scientific("H1", true);
  }



  template <int dim, int spacedim>
  void
  Poisson<dim, spacedim>::output_results(const unsigned int cycle)
  {
    TimerOutput::Scope t(getTimer(), "output_results");

    Vector<float> fe_degrees(triangulation.n_active_cells());
    for (const auto &cell : dof_handler.active_cell_iterators())
      if (cell->is_locally_owned())
        fe_degrees(cell->active_cell_index()) = cell->get_fe().degree;

    Vector<float> subdomain(triangulation.n_active_cells());
    for (auto &subd : subdomain)
      subd = triangulation.locally_owned_subdomain();

    DataOut<dim> data_out;
    data_out.attach_dof_handler(dof_handler);

    data_out.add_data_vector(locally_relevant_solution, "solution");
    data_out.add_data_vector(fe_degrees, "fe_degree");
    data_out.add_data_vector(subdomain, "subdomain");

    if (adaptation_strategy->get_error_estimates().size() > 0)
      data_out.add_data_vector(adaptation_strategy->get_error_estimates(),
                               "error");
    if (adaptation_strategy->get_hp_indicators().size() > 0)
      data_out.add_data_vector(adaptation_strategy->get_hp_indicators(),
                               "hp_indicator");

    data_out.build_patches(mapping_collection);

    data_out.write_vtu_with_pvtu_record(
      "./", prm.filestem, cycle, mpi_communicator, 2, 1);
  }



  template <int dim, int spacedim>
  void
  Poisson<dim, spacedim>::log_timings()
  {
    getTimer().print_summary();
    getPCOut() << std::endl;

    for (const auto &summary :
         getTimer().get_summary_data(TimerOutput::total_wall_time))
      {
        getTable().add_value(summary.first, summary.second);
        getTable().set_scientific(summary.first, true);
      }
  }



  template <int dim, int spacedim>
  void
  Poisson<dim, spacedim>::run()
  {
    getTable().set_auto_fill_mode(true);

    for (unsigned int cycle = 0; cycle < adaptation_strategy->get_n_cycles();
         ++cycle)
      {
        getPCOut() << "Cycle " << cycle << ':' << std::endl;
        getTable().add_value("cycle", cycle);

        {
          TimerOutput::Scope t(getTimer(), "full_cycle");

          if (cycle == 0)
            initialize_grid();
          else
            adaptation_strategy->refine();

          setup_system();

          log_diagnostics();

          // TODO: I am not happy with this
          if (prm.operator_type == "MatrixBased")
            {
              poisson_operator_matrixbased->reinit(dof_handler,
                                                   constraints,
                                                   system_rhs);
              solve(*poisson_operator_matrixbased,
                    locally_relevant_solution,
                    system_rhs);
            }
          else if (prm.operator_type == "MatrixFree")
            {
              poisson_operator_matrixfree->reinit(dof_handler,
                                                  constraints,
                                                  system_rhs);
              solve(*poisson_operator_matrixfree,
                    locally_relevant_solution,
                    system_rhs);
            }
          else
            Assert(false, ExcInternalError());

          compute_errors();
          adaptation_strategy->estimate_mark();

          output_results(cycle);
        }

        log_timings();

        if (Utilities::MPI::this_mpi_process(mpi_communicator) == 0)
          {
            std::ofstream logstream(filename_log);
            getTable().write_text(logstream);
          }

        getTimer().reset();
        getTable().start_new_row();
      }
  }



  // explicit instantiations
  template class Poisson<2>;
  template class Poisson<3>;
} // namespace Problem

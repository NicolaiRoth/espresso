/*
  Copyright (C) 2014 The ESPResSo project
  
  This file is part of ESPResSo.
  
  ESPResSo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  ESPResSo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>. 
*/
#include "cells.hpp"
#include "communication.hpp"
#include "cuda_interface.hpp"

#include "grid.hpp"
#include "config.hpp"
#include "random.hpp"
#include "particle_data.hpp"
#include "interaction_data.hpp"
#include "energy.hpp"

#ifdef CUDA

/// MPI tag for cuda particle gathering
#define REQ_CUDAGETPARTS  0xcc01
/// MPI tag for cuda force gathering
#define REQ_CUDAGETFORCES 0xcc02

static void cuda_mpi_get_particles_slave();
static void cuda_mpi_send_forces_slave();
#ifdef ENGINE
static void cuda_mpi_send_v_cs_slave();
#endif

void cuda_bcast_global_part_params() {
  COMM_TRACE(fprintf(stderr, "%d: cuda_bcast_global_part_params\n", this_node));
  mpi_bcast_cuda_global_part_vars();
  COMM_TRACE(fprintf(stderr, "%d: cuda_bcast_global_part_params finished\n", this_node));
}

/*************** REQ_GETPARTS ************/
void cuda_mpi_get_particles(CUDA_particle_data *particle_data_host)
{
    int n_part;
    int g, pnode;
    Cell *cell;
    int c;
    MPI_Status status;

    int i;  
    int *sizes;
    sizes = (int*) malloc(sizeof(int)*n_nodes);

    n_part = cells_get_n_particles();
    
    /* first collect number of particles on each node */
    MPI_Gather(&n_part, 1, MPI_INT, sizes, 1, MPI_INT, 0, comm_cart);

    /* just check if the number of particles is correct */
    if(this_node > 0){
      /* call slave functions to provide the slave datas */
      cuda_mpi_get_particles_slave();
    }
    else {
      /* master: fetch particle informations into 'result' */
      g = 0;
      for (pnode = 0; pnode < n_nodes; pnode++) {
        if (sizes[pnode] > 0) {
          if (pnode == 0) {
            for (c = 0; c < local_cells.n; c++) {
              Particle *part;
              int npart;  
              int dummy[3] = {0,0,0};
              double pos[3];
              cell = local_cells.cell[c];
              part = cell->part;
              npart = cell->n;
              for (i=0;i<npart;i++) {
                memcpy(pos, part[i].r.p, 3*sizeof(double));
                fold_position(pos, dummy);
                particle_data_host[i+g].p[0] = (float)pos[0];
                particle_data_host[i+g].p[1] = (float)pos[1];
                particle_data_host[i+g].p[2] = (float)pos[2];

                particle_data_host[i+g].v[0] = (float)part[i].m.v[0];
                particle_data_host[i+g].v[1] = (float)part[i].m.v[1];
                particle_data_host[i+g].v[2] = (float)part[i].m.v[2];
#ifdef SHANCHEN
                // SAW TODO: does this really need to be copied every time?
                int ii;
                for(ii=0;ii<2*LB_COMPONENTS;ii++){
                  particle_data_host[i+g].solvation[ii] = (float)part[i].p.solvation[ii];
                }
#endif

#ifdef LB_ELECTROHYDRODYNAMICS
                particle_data_host[i+g].mu_E[0] = (float)part[i].p.mu_E[0];
                particle_data_host[i+g].mu_E[1] = (float)part[i].p.mu_E[1];
                particle_data_host[i+g].mu_E[2] = (float)part[i].p.mu_E[2];
#endif

#ifdef ELECTROSTATICS
                if (coulomb.method == COULOMB_P3M_GPU || coulomb.method == COULOMB_MMM1D_GPU) { // TODO: this defeats the purpose of needsQ in the interface...
                  particle_data_host[i+g].q = (float)part[i].p.q;
                }
#endif

#ifdef ENGINE
                particle_data_host[i+g].swim.v_swim        = (float)part[i].swim.v_swim;
                particle_data_host[i+g].swim.f_swim        = (float)part[i].swim.f_swim;
                particle_data_host[i+g].swim.quatu[0]      = (float)part[i].r.quatu[0];
                particle_data_host[i+g].swim.quatu[1]      = (float)part[i].r.quatu[1];
                particle_data_host[i+g].swim.quatu[2]      = (float)part[i].r.quatu[2];
                particle_data_host[i+g].swim.push_pull     =        part[i].swim.push_pull;
                particle_data_host[i+g].swim.dipole_length = (float)part[i].swim.dipole_length;
#endif
              }  
              g += npart;
            }
          }
          else {
            MPI_Recv(&particle_data_host[g], sizes[pnode]*sizeof(CUDA_particle_data), MPI_BYTE, pnode, REQ_CUDAGETPARTS,
            comm_cart, &status);
            g += sizes[pnode];
          }
        }
      }
    }
    COMM_TRACE(fprintf(stderr, "%d: finished get\n", this_node));
    free(sizes);
}

static void cuda_mpi_get_particles_slave(){
   
    int n_part;
    int g;
    CUDA_particle_data *particle_data_host_sl;
    Cell *cell;
    int c, i;

    n_part = cells_get_n_particles();

    COMM_TRACE(fprintf(stderr, "%d: get_particles_slave, %d particles\n", this_node, n_part));

    if (n_part > 0) {
      /* get (unsorted) particle informations as an array of type 'particle' */
      /* then get the particle information */
      particle_data_host_sl = (CUDA_particle_data*) malloc(n_part*sizeof(CUDA_particle_data));
      
      g = 0;
      for (c = 0; c < local_cells.n; c++) {
        Particle *part;
        int npart;
        int dummy[3] = {0,0,0};
        double pos[3];
        cell = local_cells.cell[c];
        part = cell->part;
        npart = cell->n;

        for (i=0;i<npart;i++) {
          memcpy(pos, part[i].r.p, 3*sizeof(double));
          fold_position(pos, dummy);  
      
          particle_data_host_sl[i+g].p[0] = (float)pos[0];
          particle_data_host_sl[i+g].p[1] = (float)pos[1];
          particle_data_host_sl[i+g].p[2] = (float)pos[2];

          particle_data_host_sl[i+g].v[0] = (float)part[i].m.v[0];
          particle_data_host_sl[i+g].v[1] = (float)part[i].m.v[1];
          particle_data_host_sl[i+g].v[2] = (float)part[i].m.v[2];
          
#ifdef SHANCHEN
        // SAW TODO: does this really need to be copied every time?
        int ii;
        for(ii=0;ii<2*LB_COMPONENTS;ii++){
           particle_data_host_sl[i+g].solvation[ii] = (float)part[i].p.solvation[ii];
        }
#endif



  #ifdef LB_ELECTROHYDRODYNAMICS
          particle_data_host_sl[i+g].mu_E[0] = (float)part[i].p.mu_E[0];
          particle_data_host_sl[i+g].mu_E[1] = (float)part[i].p.mu_E[1];
          particle_data_host_sl[i+g].mu_E[2] = (float)part[i].p.mu_E[2];
  #endif

  #ifdef ELECTROSTATICS
          if (coulomb.method == COULOMB_P3M_GPU || coulomb.method == COULOMB_MMM1D_GPU) {
            particle_data_host_sl[i+g].q = (float)part[i].p.q;
          }
  #endif

#ifdef ENGINE
          particle_data_host_sl[i+g].swim.v_swim        = (float)part[i].swim.v_swim;
          particle_data_host_sl[i+g].swim.f_swim        = (float)part[i].swim.f_swim;
          particle_data_host_sl[i+g].swim.quatu[0]      = (float)part[i].r.quatu[0];
          particle_data_host_sl[i+g].swim.quatu[1]      = (float)part[i].r.quatu[1];
          particle_data_host_sl[i+g].swim.quatu[2]      = (float)part[i].r.quatu[2];
          particle_data_host_sl[i+g].swim.push_pull     =        part[i].swim.push_pull;
          particle_data_host_sl[i+g].swim.dipole_length = (float)part[i].swim.dipole_length;
#endif
        }
        g+=npart;
      }
      /* and send it back to the master node */
      MPI_Send(particle_data_host_sl, n_part*sizeof(CUDA_particle_data), MPI_BYTE, 0, REQ_CUDAGETPARTS, comm_cart);
      free(particle_data_host_sl);
    }
}

void cuda_mpi_send_forces(CUDA_particle_force *host_forces,CUDA_fluid_composition * host_composition){
  int n_part;
  int g, pnode;
  Cell *cell;
  int c;
  int i;  
  int *sizes;
  sizes = (int *) malloc(sizeof(int)*n_nodes);
  n_part = cells_get_n_particles();
  /* first collect number of particles on each node */
  MPI_Gather(&n_part, 1, MPI_INT, sizes, 1, MPI_INT, 0, comm_cart);

  /* call slave functions to provide the slave data */
  if(this_node > 0) {
    cuda_mpi_send_forces_slave();
  }
  else{
    /* fetch particle informations into 'result' */
    g = 0;
    for (pnode = 0; pnode < n_nodes; pnode++) {
      if (sizes[pnode] > 0) {
        if (pnode == 0) {
          for (c = 0; c < local_cells.n; c++) {
            int npart;  
            cell = local_cells.cell[c];
            npart = cell->n;
            for (i=0;i<npart;i++) {
              cell->part[i].f.f[0] += (double)host_forces[i+g].f[0];
              cell->part[i].f.f[1] += (double)host_forces[i+g].f[1];
              cell->part[i].f.f[2] += (double)host_forces[i+g].f[2];
#ifdef SHANCHEN
              for (int ii=0;ii<LB_COMPONENTS;ii++) {
                cell->part[i].r.composition[ii] = (double)host_composition[i+g].weight[ii];
              }
#endif
            }
            g += npart;
          }
        }
        else {
          /* and send it back to the slave node */
          MPI_Send(&host_forces[g], sizes[pnode]*sizeof(CUDA_particle_force), MPI_BYTE, pnode, REQ_CUDAGETFORCES, comm_cart);      
#ifdef SHANCHEN
          MPI_Send(&host_composition[g], sizes[pnode]*sizeof(CUDA_fluid_composition), MPI_BYTE, pnode, REQ_CUDAGETPARTS, comm_cart);      
#endif
          g += sizes[pnode];
        }
      }
    }
  }
  COMM_TRACE(fprintf(stderr, "%d: finished send\n", this_node));

  free(sizes);
}

static void cuda_mpi_send_forces_slave(){

    int n_part;
    CUDA_particle_force *host_forces_sl=NULL;
#ifdef SHANCHEN
    CUDA_fluid_composition *host_composition_sl=NULL;
#endif
    Cell *cell;
    int c, i;
    MPI_Status status;

    n_part = cells_get_n_particles();

    COMM_TRACE(fprintf(stderr, "%d: send_particles_slave, %d particles\n", this_node, n_part));


    if (n_part > 0) {
      int g = 0;
      /* get (unsorted) particle informations as an array of type 'particle' */
      /* then get the particle information */
      host_forces_sl = (CUDA_particle_force *) malloc(n_part*sizeof(CUDA_particle_force));
      MPI_Recv(host_forces_sl, n_part*sizeof(CUDA_particle_force), MPI_BYTE, 0, REQ_CUDAGETFORCES,
        comm_cart, &status);
#ifdef SHANCHEN
      host_composition_sl = (CUDA_fluid_composition *) malloc(n_part*sizeof(CUDA_fluid_composition));
      MPI_Recv(host_composition_sl, n_part*sizeof(CUDA_particle_force), MPI_BYTE, 0, REQ_CUDAGETPARTS,
        comm_cart, &status);
#endif
      for (c = 0; c < local_cells.n; c++) {
        int npart;  
        cell = local_cells.cell[c];
        npart = cell->n;
        for (i=0;i<npart;i++) {
          cell->part[i].f.f[0] += (double)host_forces_sl[i+g].f[0];
          cell->part[i].f.f[1] += (double)host_forces_sl[i+g].f[1];
          cell->part[i].f.f[2] += (double)host_forces_sl[i+g].f[2];
#ifdef SHANCHEN
          for (int ii=0;ii<LB_COMPONENTS;ii++) {
             cell->part[i].r.composition[ii] = (double)host_composition_sl[i+g].weight[ii];
          }
#endif
        }
        g += npart;
      }
      free(host_forces_sl);
#ifdef SHANCHEN
      free(host_composition_sl);
#endif 
    } 
}

#ifdef ENGINE
void cuda_mpi_send_v_cs(CUDA_v_cs *host_v_cs){
  int n_part;
  int g;
  Cell *cell;
  int *sizes;

  // first collect number of particles on each node
  sizes = (int *) malloc(sizeof(int)*n_nodes);
  n_part = cells_get_n_particles();
  MPI_Gather(&n_part, 1, MPI_INT, sizes, 1, MPI_INT, 0, comm_cart);

  // call slave functions to provide the slave data
  if(this_node > 0)
  {
    cuda_mpi_send_v_cs_slave();
  }
  else
  {
    // fetch particle informations into 'result'
    g = 0;
    for (int pnode = 0; pnode < n_nodes; pnode++)
    {
      if (sizes[pnode] > 0)
      {
        if (pnode == 0)
        {
          for (int c = 0; c < local_cells.n; c++)
          {
            int npart;  
            cell = local_cells.cell[c];
            npart = cell->n;
            for (int i = 0; i < npart; i++)
            {
              cell->part[i].swim.v_center[0] = (double)host_v_cs[i+g].v_cs[0];
              cell->part[i].swim.v_center[1] = (double)host_v_cs[i+g].v_cs[1];
              cell->part[i].swim.v_center[2] = (double)host_v_cs[i+g].v_cs[2];
              cell->part[i].swim.v_source[0] = (double)host_v_cs[i+g].v_cs[3];
              cell->part[i].swim.v_source[1] = (double)host_v_cs[i+g].v_cs[4];
              cell->part[i].swim.v_source[2] = (double)host_v_cs[i+g].v_cs[5];
            }
            g += npart;
          }
        }
        else
        {
          // and send it back to the slave node
          MPI_Send(&host_v_cs[g], sizes[pnode]*sizeof(CUDA_v_cs), MPI_BYTE, pnode, REQ_CUDAGETFORCES, comm_cart);      
          g += sizes[pnode];
        }
      }
    }
  }
  COMM_TRACE(fprintf(stderr, "%d: finished send\n", this_node));

  free(sizes);
}

static void cuda_mpi_send_v_cs_slave(){
  int n_part;
  CUDA_v_cs *host_v_cs_slave = NULL;
  Cell *cell;
  MPI_Status status;

  n_part = cells_get_n_particles();

  COMM_TRACE(fprintf(stderr, "%d: send_particles_slave, %d particles\n", this_node, n_part));


  if (n_part > 0)
  {
    int g = 0;
    // get (unsorted) particle informations as an array of type 'particle'
    // then get the particle information
    host_v_cs_slave = (CUDA_v_cs *) malloc(n_part*sizeof(CUDA_v_cs));
    MPI_Recv(host_v_cs_slave, n_part*sizeof(CUDA_v_cs), MPI_BYTE, 0, REQ_CUDAGETFORCES,
        comm_cart, &status);

    for (int c = 0; c < local_cells.n; c++)
    {
      int npart;  
      cell = local_cells.cell[c];
      npart = cell->n;
      for (int i = 0; i < npart; i++)
      {
        cell->part[i].swim.v_center[0] = (double)host_v_cs_slave[i+g].v_cs[0];
        cell->part[i].swim.v_center[1] = (double)host_v_cs_slave[i+g].v_cs[1];
        cell->part[i].swim.v_center[2] = (double)host_v_cs_slave[i+g].v_cs[2];
        cell->part[i].swim.v_source[0] = (double)host_v_cs_slave[i+g].v_cs[3];
        cell->part[i].swim.v_source[1] = (double)host_v_cs_slave[i+g].v_cs[4];
        cell->part[i].swim.v_source[2] = (double)host_v_cs_slave[i+g].v_cs[5];
      }
      g += npart;
    }
    free(host_v_cs_slave);
  } 
}
#endif // ifdef ENGINE

/** Takes a CUDA_energy struct and adds it to the core energy struct.
This cannot be done from inside cuda_common_cuda.cu:copy_energy_from_GPU() because energy.hpp indirectly includes on mpi.h while .cu files may not depend on mpi.h.*/
void copy_CUDA_energy_to_energy(CUDA_energy energy_host)
{
  energy.bonded[0] += energy_host.bonded;
  energy.non_bonded[0] += energy_host.non_bonded;
  energy.coulomb[0] += energy_host.coulomb;
  energy.dipolar[0] += energy_host.dipolar;
}

#endif /* ifdef CUDA */

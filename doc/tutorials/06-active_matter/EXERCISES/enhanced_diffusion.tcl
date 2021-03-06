################################################################################
#                                                                              #
# Copyright (C) 2010,2011,2012,2013,2014, 2015 The ESPResSo project            #
#                                                                              #
# This file is part of ESPResSo.                                               #
#                                                                              #
# ESPResSo is free software: you can redistribute it and/or modify             #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation, either version 3 of the License, or            #
# (at your option) any later version.                                          #
#                                                                              #
# ESPResSo is distributed in the hope that it will be useful,                  #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program.  If not, see <http://www.gnu.org/licenses/>.        #
#                                                                              #
################################################################################
#                                                                              #
#                  Active Matter: Enhanced Diffusion Tutorial                  #
#                                                                              #
################################################################################

# create an output folder

set dir "./RESULTS_ENHANCED_DIFFUSION/"
file mkdir $dir
after 250

################################################################################

# Read in the active velocity from the command prompt

if { [llength $argv] != 1 } {
  puts "Usage: Espresso $argv0 <vel> (0 <= vel < 10.0)"
  exit 1
}

set vel [lindex $argv 0]

################################################################################
#
# To obtain accurate statistics, you will need to run the simulation
# several times, which is accomplished by this loop. Do not increase
# this number too much, as it will slow down the simulation.
#
################################################################################

## Exercise 4 ##
# Once you have tested the routine for a single , then
# make it such that you can loop over the run parameter
# and repeat the simulation 5 times.

set run 0
...
{

  # Set up a random seed (a new one for each run)

## Exercise 1 ##
# Explain the choice of the random seed

  t_random seed [expr ([clock clicks]*[pid])%2147483647]

  # Set the basic simulation parameters

  set tcl_precision 10
  setmd box_l 10.0 10.0 10.0
  setmd periodic 1 1 1
  setmd skin 0.3
  set sampsteps 5000
  set samplength 1000
  set tstep 0.01  
  setmd time_step $tstep

## Exercise 2 ##
# Why can we get away with such a small box?
# Could it be even smaller?

  # Use the Langevin thermostat (no hydrodynamics)

  thermostat langevin 1.0 1.0

  # Place a single active particle

  part 0 pos 5.0 5.0 5.0 swimming v_swim $vel

  # Initialize the mean squared displacement (MSD) correlator

  set tmax [expr $tstep*$sampsteps]

  set pos_id [observable new particle_positions id 0]
  set msd [correlation new obs1 $pos_id \
           corr_operation square_distance_componentwise \
           dt [expr 1.000*$tstep] tau_max $tmax tau_lin 16]
  correlation $msd autoupdate start

## Exercise 3 ##
# Construct the auto-correlators for the VACF and AVACF,
# using the example of the MSD

  # Initialize the velocity auto-correlation function (VACF) correlator

  ...

  # Initialize the angular velocity auto-correlation function (AVACF) correlator

  ...

  # Integrate 5,000,000 steps. This can be done in one go as well.

  for { set i 0 } { $i < $sampsteps } { incr i } {

    integrate $samplength

  }

  # Finalize the correlators and write to disk

  correlation $msd autoupdate stop
  correlation $msd finalize
  set msdstr "msd\_$vel\_$run"
  set msdexi ".dat"
  correlation $msd write_to_file "$dir$msdstr$msdexi"

  ...

  ...

}

################################################################################

exit

################################################################################

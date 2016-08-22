#include <iostream>
#include <numeric>
#include "fix_ssages.h"
#include "atom.h"
#include "compute.h"
#include "modify.h"
#include "force.h"
#include "pair.h"
#include "update.h"
#include "domain.h"
#include <boost/mpi.hpp>
#include <boost/version.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>

using namespace SSAGES;
using namespace LAMMPS_NS::FixConst;
using namespace boost;

namespace LAMMPS_NS
{
	// Copyright (C) 2015 Lorenz Hübschle-Schneider <lorenz@4z2.de>
	// Helper function for variable MPI_allgather.
	template <typename T, typename transmit_type = uint64_t>
	void allgatherv_serialize(const mpi::communicator &comm, std::vector<T> &in, std::vector<T> &out)
	{
		// Step 1: exchange sizes
		// We need to compute the displacement array, specifying for each PE
		// at which position in out to place the data received from it
		// Need to cast to int because this is what MPI uses as size_t...
		const int factor = sizeof(T) / sizeof(transmit_type);
		const int in_size = static_cast<int>(in.size()) * factor;
		std::vector<int> sizes(comm.size());
		mpi::all_gather(comm, in_size, sizes.data());

		// Step 2: calculate displacements from sizes
		// Compute prefix sum to compute displacements from sizes
		std::vector<int> displacements(comm.size() + 1);
		displacements[0] = 0;
		std::partial_sum(sizes.begin(), sizes.end(), displacements.begin() + 1);

		// divide by factor by which T is larger than transmit_type
		out.resize(displacements.back() / factor);

		// Step 3: MPI_Allgatherv
		transmit_type *sendptr = reinterpret_cast<transmit_type*>(in.data());
		transmit_type *recvptr = reinterpret_cast<transmit_type*>(out.data());
		const MPI_Datatype datatype = mpi::get_mpi_datatype<transmit_type>();
		int status = MPI_Allgatherv(sendptr, in_size, datatype, recvptr,
									sizes.data(), displacements.data(),
									datatype, comm);
		if (status != 0) 
		{
			std::cerr << "PE " << comm.rank() << ": MPI_Allgatherv returned "
			<< status << ", errno " << errno << std::endl;
			exit(-1);
		}
	}

	FixSSAGES::FixSSAGES(LAMMPS *lmp, int narg, char **arg) : 
	Fix(lmp, narg, arg), Hook()
	{
	}

	void FixSSAGES::setup(int)
	{
		// Allocate vectors for snapshot.
		// Here we size to local number of atoms. We will 
		// resize later.

		auto n = atom->nlocal;
		auto ntype = atom->ntypes;

		int dim;
		const auto* _sigma = (force->pair->extract("sigma", dim));

		if (_sigma==NULL)
		{
			std::cout<<"Could not locate sigma from force->pair->extract"<<std::endl;
			exit(-1);
		}

		auto& pos = _snapshot->GetPositions();
		pos.resize(n);
		auto& vel = _snapshot->GetVelocities();
		vel.resize(n);
		auto& frc = _snapshot->GetForces();
		frc.resize(n);
		auto& masses = _snapshot->GetMasses();
		masses.resize(n);
		auto& ids = _snapshot->GetAtomIDs();
		ids.resize(n);
		auto& types = _snapshot->GetAtomTypes();
		types.resize(n);
		auto& charges = _snapshot->GetCharges();
		charges.resize(n);

		auto& sigmas = _snapshot->GetSigmas();
		sigmas.resize(ntype);
		for (auto& sig : sigmas)
			sig.resize(ntype);

		auto& flags = _snapshot->GetImageFlags();
		flags.resize(n);

		SyncToSnapshot();
		Hook::PreSimulationHook();
	}

	void FixSSAGES::post_force(int)
	{
		SyncToSnapshot();
		Hook::PostIntegrationHook();
	}

	void FixSSAGES::post_run()
	{
		SyncToSnapshot();
		Hook::PostSimulationHook();
	}

	void FixSSAGES::end_of_step()
	{
		Hook::PostStepHook();
	}

	int FixSSAGES::setmask()
	{
		// We are interested in post-force and post run hooks.
		int mask = 0;
		mask |= POST_FORCE;
		mask |= POST_RUN;
		mask |= END_OF_STEP;
		return mask;
	}

	void FixSSAGES::SyncToSnapshot() //put LAMMPS values -> Snapshot
	{
		using namespace boost::mpi;

		// Obtain local reference to snapshot variables.
		// Const Atom will ensure that atom variables are
		// not being changed. Only snapshot side variables should
		// change.
		const auto* _atom = atom;

		int dim;
		const auto* _sigma = (double**)(force->pair->extract("sigma", dim));

		if (_sigma==NULL)
		{
			std::cout<<"Could not locate sigma from force->pair->extract"<<std::endl;
			exit(-1);
		}

		auto n = atom->nlocal;

		auto& pos = _snapshot->GetPositions();
		pos.resize(n);
		auto& vel = _snapshot->GetVelocities();
		vel.resize(n);
		auto& frc = _snapshot->GetForces();
		frc.resize(n);
		auto& masses = _snapshot->GetMasses();
		masses.resize(n);
		auto& flags = _snapshot->GetImageFlags();
		flags.resize(n);

		auto& sigmas = _snapshot->GetSigmas();
		sigmas.resize(n);

		// Labels and ids for future work on only updating
		// atoms that have changed.
		auto& ids = _snapshot->GetAtomIDs();
		ids.resize(n);
		auto& types = _snapshot->GetAtomTypes();
		types.resize(n);

		auto& charges = _snapshot->GetCharges();
		charges.resize(n);

		// Thermo properties:
		int icompute; 

		//Temperature
		const char* id_temp = "thermo_temp";
		icompute = modify->find_compute(id_temp);
		auto* temperature = modify->compute[icompute];
		_snapshot->SetTemperature(temperature->compute_scalar());
		
		//Energy
		double etot = 0;

		// Get potential energy.
		const char* id_pe = "thermo_pe";
		icompute = modify->find_compute(id_pe);
		auto* pe = modify->compute[icompute];
		etot += pe->scalar;

		// Compute kinetic energy.
		double ekin = 0.5 * temperature->scalar * temperature->dof  * force->boltz;
		etot += ekin;

		// Store in snapshot.
		_snapshot->SetEnergy(etot/atom->natoms);
		
		// Get iteration.
		_snapshot->SetIteration(update->ntimestep);

		_snapshot->SetDielectric(force->dielectric);

		_snapshot->Setqqrd2e(force->qqrd2e);

		_snapshot->SetNumAtoms(n);

		
		// Get H-matrix.
		Matrix3 H;
		H << domain->h[0], domain->h[5], domain->h[4],
		                0, domain->h[1], domain->h[3],
		                0,            0, domain->h[2];

		_snapshot->SetHMatrix(H);
		_snapshot->SetKb(force->boltz);

		// Get box origin. 
		Vector3 origin;
		if(domain->triclinic == 0)
		{
			origin = {
				domain->boxlo[0], 
				domain->boxlo[1], 
				domain->boxlo[2]
			};
		}
		else
		{
			origin = {
				domain->boxlo_bound[0], 
				domain->boxlo_bound[1], 
				domain->boxlo_bound[2]
			};
		}
		_snapshot->SetOrigin(origin);

		// Set periodicity. 
		_snapshot->SetPeriodicity({
			domain->xperiodic, 
			domain->yperiodic, 
			domain->zperiodic
		});

		// First we sync local data, then gather.
		// we gather data across all processors.
		for (int i = 0; i < n; ++i)
		{
			pos[i][0] = _atom->x[i][0]; //x
			pos[i][1] = _atom->x[i][1]; //y
			pos[i][2] = _atom->x[i][2]; //z
			
			frc[i][0] = _atom->f[i][0]; //force->x
			frc[i][1] = _atom->f[i][1]; //force->y
			frc[i][2] = _atom->f[i][2]; //force->z

			if(_atom->rmass_flag)
				masses[i] = _atom->rmass[i];
			else
				masses[i] = _atom->mass[_atom->type[i]];
			
			vel[i][0] = _atom->v[i][0];
			vel[i][1] = _atom->v[i][1];
			vel[i][2] = _atom->v[i][2];

			// Image flags. 
			flags[i][0] = (_atom->image[i] & IMGMASK) - IMGMAX;;
			flags[i][1] = (_atom->image[i] >> IMGBITS & IMGMASK) - IMGMAX;
			flags[i][2] = (_atom->image[i] >> IMG2BITS) - IMGMAX;

			ids[i] = _atom->tag[i];
			types[i] = _atom->type[i];
			charges[i] = _atom->q[i];
		}

		for (int i = 0; i < sigmas.size(); ++i)
			for (int j = 0; j <sigmas[i].size(); ++j)
				sigmas[i][j] = _sigma[i][j];

		auto& comm = _snapshot->GetCommunicator();

		std::vector<Vector3> gpos, gvel, gfrc;
		std::vector<double> gmasses;
        std::vector<double> gcharges;
		std::vector<int> gids, gtypes; 
		
		allgatherv_serialize(comm, pos, gpos);
		allgatherv_serialize(comm, vel, gvel);
		allgatherv_serialize(comm, frc, gfrc);
		allgatherv_serialize(comm, masses, gmasses);
		allgatherv_serialize(comm, charges, gcharges);
		allgatherv_serialize<int,int>(comm, ids, gids);
		allgatherv_serialize<int,int>(comm, types, gtypes);

		pos = gpos; 
		vel = gvel;
		frc = gfrc; 
		masses = gmasses;
        charges = gcharges;
		ids = gids; 
		types = gtypes;
	}

	void FixSSAGES::SyncToEngine() //put Snapshot values -> LAMMPS
	{
		// Obtain local const reference to snapshot variables.
		// Const will ensure that _snapshot variables are
		// not being changed. Only engine side variables should
		// change. 
		const auto& pos = _snapshot->GetPositions();
		const auto& vel = _snapshot->GetVelocities();
		const auto& frc = _snapshot->GetForces();
		const auto& masses = _snapshot->GetMasses();
		const auto& charges = _snapshot->GetCharges();
		const auto& sigmas = _snapshot->GetSigmas();

		// Labels and ids for future work on only updating
		// atoms that have changed.
		const auto& ids = _snapshot->GetAtomIDs();
		const auto& types = _snapshot->GetAtomTypes();

		// Sync back only local atom data. all_gather guarantees 
		// sorting of data in vector based on rank. Compute offset
		// based on rank and sync back only the appropriate data.
		std::vector<int> natoms;
		auto& comm = _snapshot->GetCommunicator();
		natoms.reserve(comm.size());
		boost::mpi::all_gather(comm, atom->nlocal, natoms);
		auto j = std::accumulate(natoms.begin(), natoms.begin() + comm.rank(), 0);

		for (int i = 0; i < atom->nlocal; ++i, ++j)
		{
			atom->x[i][0] = pos[j][0]; //x 
			atom->x[i][1] = pos[j][1]; //y
			atom->x[i][2] = pos[j][2]; //z
			atom->f[i][0] = frc[j][0]; //force->x
			atom->f[i][1] = frc[j][1]; //force->y
			atom->f[i][2] = frc[j][2]; //force->z
			atom->v[i][0] = vel[j][0]; //velocity->x
			atom->v[i][1] = vel[j][1]; //velocity->y
			atom->v[i][2] = vel[j][2]; //velocity->z
			atom->tag[i] = ids[j];
			atom->type[i] = types[j];
			atom->q[i] = charges[j];
			
			// Current masses can only be changed if using
			// per atom masses in lammps
			if(atom->rmass_flag)
				atom->rmass[i] = masses[j];
			
		}

		// LAMMPS computes will reset thermo data based on
		// updated information. No need to sync thermo data
		// from snapshot to engine.
		// However, this will change in the future.
	}
}


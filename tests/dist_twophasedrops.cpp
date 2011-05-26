/// \file dist_twophasedrops.cpp
/// \brief flow in measurement cell or brick
/// \author LNM RWTH Aachen: Patrick Esser, Joerg Grande, Sven Gross; SC RWTH Aachen: Oliver Fortmeier

/*
 * This file is part of DROPS.
 *
 * DROPS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DROPS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with DROPS. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Copyright 2009 LNM/SC RWTH Aachen, Germany
*/

//multigrid
#include "geom/multigrid.h"
#include "geom/builder.h"
//time integration
#include "navstokes/instatnavstokes2phase.h"
#include "stokes/integrTime.h"
//output
#include "out/output.h"
#include "out/ensightOut.h"
#include "out/vtkOut.h"
//levelset
#include "levelset/coupling.h"
#include "levelset/adaptriang.h"
#include "levelset/mzelle_hdr.h"
#include "levelset/twophaseutils.h"
//surfactants
#include "surfactant/ifacetransp.h"
//function map
#include "misc/bndmap.h"
//solver factory for stokes
#include "num/stokessolverfactory.h"
#ifndef _PAR
#include "num/stokessolver.h"
#else
#include "num/parstokessolver.h"
#include "parallel/loadbal.h"
#include "parallel/parmultigrid.h"
#endif
//general: streams
#include <fstream>
#include <sstream>

DROPS::ParamMesszelleNsCL C;

// rho*du/dt - mu*laplace u + Dp = f + rho*g - okn
//                        -div u = 0
//                             u = u0, t=t0

namespace DROPS // for Strategy
{

void Strategy( InstatNavierStokes2PhaseP2P1CL& Stokes, LsetBndDataCL& lsetbnddata, AdapTriangCL& adap)
// flow control
{
    //DROPS::InScaMap & inscamap = DROPS::InScaMap::getInstance();
    //DROPS::ScaMap & scamap = DROPS::ScaMap::getInstance();
    //DROPS::InVecMap & vecmap = DROPS::InVecMap::getInstance();
    DROPS::MatchMap & matchmap = DROPS::MatchMap::getInstance();

  
    bool is_periodic = C.exp_UsePerMatching > 0;
    match_fun periodic_match = is_periodic ? matchmap[C.exp_PerMatching] : 0;
    
    MultiGridCL& MG= Stokes.GetMG();

    // initialization of surface tension
    sigma= Stokes.GetCoeff().SurfTens;
    eps= C.sft_JumpWidth;    lambda= C.sft_RelPos;    sigma_dirt_fac= C.sft_DirtFactor;
    instat_scalar_fun_ptr sigmap  = 0;
    if (C.sft_VarTension)
    {
        sigmap  = &sigma_step;
    }
    else
    {
        sigmap  = &sigmaf;
    }
    SurfaceTensionCL sf( sigmap);


    LevelsetP2CL lset( MG, lsetbnddata, sf, C.lvs_SD, C.lvs_CurvDiff);
    
    if (is_periodic) //CL: Anyone a better idea? perDirection from ParameterFile?
    {
        DROPS::Point3DCL dx;
        //hack:
        std::string mesh( C.dmc_MeshFile), delim("x@");
        size_t idx_;
        while ((idx_= mesh.find_first_of( delim)) != std::string::npos )
            mesh[idx_]= ' ';
        std::istringstream brick_info( mesh);
        brick_info >> dx[0] >> dx[1] >> dx[2] ;
        int n = 0;
        if (C.exp_PerMatching == "periodicx" || C.exp_PerMatching == "periodicy" || C.exp_PerMatching == "periodicz")
            n = 1;
        if (C.exp_PerMatching == "periodicxy" || C.exp_PerMatching == "periodicxz" || C.exp_PerMatching == "periodicyz")
            n = 2;
        LevelsetP2CL::perDirSetT pdir(n);
        if (C.exp_PerMatching == "periodicx") pdir[0][0] = dx[0];
        if (C.exp_PerMatching == "periodicy") pdir[0][1] = dx[1];
        if (C.exp_PerMatching == "periodicz") pdir[0][2] = dx[2];
        if (C.exp_PerMatching == "periodicxy") {pdir[0][0] = dx[0]; pdir[1][1] = dx[1];}
        if (C.exp_PerMatching == "periodicxz") {pdir[0][0] = dx[0]; pdir[1][2] = dx[2];}
        if (C.exp_PerMatching == "periodicyz") {pdir[0][1] = dx[1]; pdir[1][2] = dx[2];}
        if (C.exp_PerMatching != "periodicx" && C.exp_PerMatching != "periodicy" && C.exp_PerMatching != "periodicz" && 
          C.exp_PerMatching != "periodicxy" && C.exp_PerMatching != "periodicxz" && C.exp_PerMatching != "periodicyz"){
            std::cout << "WARNING: could not set periodic directions! Reparametrization can not work correctly now!" << std::endl;
            std::cout << "Press any key to continue" << std::endl; getchar();
        }
        lset.SetPeriodicDirections(&pdir);
    }

    LevelsetRepairCL lsetrepair( lset);
    adap.push_back( &lsetrepair);
    VelocityRepairCL velrepair( Stokes);
    adap.push_back( &velrepair);
    PressureRepairCL prrepair( Stokes, lset);
    adap.push_back( &prrepair);

    IdxDescCL* lidx= &lset.idx;
    MLIdxDescCL* vidx= &Stokes.vel_idx;
    MLIdxDescCL* pidx= &Stokes.pr_idx;

    lset.CreateNumbering( MG.GetLastLevel(), lidx, periodic_match);
    lset.Phi.SetIdx( lidx);
    if (C.sft_VarTension)
        lset.SetSurfaceForce( SF_ImprovedLBVar);
    else
        lset.SetSurfaceForce( SF_ImprovedLB);

    if ( StokesSolverFactoryHelperCL<ParamMesszelleNsCL>().VelMGUsed(C))
        Stokes.SetNumVelLvl ( Stokes.GetMG().GetNumLevel());
    if ( StokesSolverFactoryHelperCL<ParamMesszelleNsCL>().PrMGUsed(C))
        Stokes.SetNumPrLvl  ( Stokes.GetMG().GetNumLevel());

    SetInitialLevelsetConditions( lset, MG, C);
    Stokes.CreateNumberingVel( MG.GetLastLevel(), vidx, periodic_match);
    Stokes.CreateNumberingPr(  MG.GetLastLevel(), pidx, periodic_match, &lset);
    // For a two-level MG-solver: P2P1 -- P2P1X; comment out the preceding CreateNumberings
//     Stokes.SetNumVelLvl ( 2);
//     Stokes.SetNumPrLvl  ( 2);
//     Stokes.vel_idx.GetCoarsest().CreateNumbering( MG.GetLastLevel(), MG, Stokes.GetBndData().Vel);
//     Stokes.vel_idx.GetFinest().  CreateNumbering( MG.GetLastLevel(), MG, Stokes.GetBndData().Vel);
//     Stokes.pr_idx.GetCoarsest(). GetXidx().SetBound( 1e99);
//     Stokes.pr_idx.GetCoarsest(). CreateNumbering( MG.GetLastLevel(), MG, Stokes.GetBndData().Pr, 0, &lset.Phi);
//     Stokes.pr_idx.GetFinest().   CreateNumbering( MG.GetLastLevel(), MG, Stokes.GetBndData().Pr, 0, &lset.Phi);

    StokesVelBndDataCL::bnd_val_fun ZeroVel = InVecMap::getInstance().find("ZeroVel")->second;

    Stokes.SetIdx();
    Stokes.v.SetIdx  ( vidx);
    Stokes.p.SetIdx  ( pidx);
    Stokes.InitVel( &Stokes.v, ZeroVel);
    SetInitialConditions( Stokes, lset, MG, C);
    DisplayDetailedGeom( MG);
    DisplayUnks(Stokes, lset, MG);

    double Vol = 0;

    if (C.exp_InitialLSet == "Ellipsoid"){
        Vol = EllipsoidCL::GetVolume();
        std::cout << "initial volume: " << lset.GetVolume()/Vol << std::endl;
        double dphi= lset.AdjustVolume( Vol, 1e-9);
        std::cout << "initial volume correction is " << dphi << std::endl;
        lset.Phi.Data+= dphi;
        std::cout << "new initial volume: " << lset.GetVolume()/Vol << std::endl;
    }else{
        Vol = lset.GetVolume();
    }
/*    
    const DROPS::BndCondT c_bc[6]= {
        DROPS::OutflowBC, DROPS::OutflowBC, DROPS::OutflowBC,
        DROPS::OutflowBC, DROPS::OutflowBC, DROPS::OutflowBC
    };
    const DROPS::BndDataCL<>::bnd_val_fun c_bfun[6]= {0, 0, 0, 0, 0, 0};
    DROPS::BndDataCL<> Bnd_c( 6, c_bc, c_bfun);
    double D[2] = {C.trp_DiffPos, C.trp_DiffNeg};
    TransportP1CL massTransp( MG, Bnd_c, Stokes.GetBndData().Vel, C.trp_Theta, D, C.trp_HNeg/C.trp_HPos, &Stokes.v, lset, C.tm_StepSize, C.trp_Iter, C.trp_Tol);
    TransportRepairCL transprepair(massTransp, MG);
    if (C.trp_DoTransp)
    {
        adap.push_back(&transprepair);
        MLIdxDescCL* cidx= &massTransp.idx;
        massTransp.CreateNumbering( MG.GetLastLevel(), cidx);
        massTransp.ct.SetIdx( cidx);
        if (C.dmc_InitialCond != -1){
            massTransp.Init( inscamap["Initialcneg"], inscamap["Initialcpos"]);
        }
        else
        {
            ReadFEFromFile( massTransp.ct, MG, C.dmc_InitialFile+"concentrationTransf");
        }
        massTransp.Update();
        std::cout << massTransp.c.Data.size() << " concentration unknowns,\n";
    }

    /// \todo rhs beruecksichtigen
    SurfactantcGP1CL surfTransp( MG, Stokes.GetBndData().Vel, C.surf_Theta, C.surf_Visc, &Stokes.v, lset.Phi, lset.GetBndData(), C.tm_StepSize, C.surf_Iter, C.surf_Tol, C.surf_OmitBound);
    InterfaceP1RepairCL surf_repair( MG, lset.Phi, lset.GetBndData(), surfTransp.ic);
    if (C.surf_DoTransp)
    {
        adap.push_back( &surf_repair);
        surfTransp.idx.CreateNumbering( MG.GetLastLevel(), MG, &lset.Phi, &lset.GetBndData());
        std::cout << "Surfactant transport: NumUnknowns: " << surfTransp.idx.NumUnknowns() << std::endl;
        surfTransp.ic.SetIdx( &surfTransp.idx);
        surfTransp.Init( inscamap["surf_sol"]);
    }
*/
    // Stokes-Solver
    StokesSolverFactoryCL<InstatNavierStokes2PhaseP2P1CL, ParamMesszelleNsCL> stokessolverfactory(Stokes, C);
    StokesSolverBaseCL* stokessolver = stokessolverfactory.CreateStokesSolver();
//     StokesSolverAsPreCL pc (*stokessolver1, 1);
//     GCRSolverCL<StokesSolverAsPreCL> gcr(pc, C.stk_OuterIter, C.stk_OuterIter, C.stk_OuterTol, /*rel*/ false);
//     BlockMatrixSolverCL<GCRSolverCL<StokesSolverAsPreCL> >* stokessolver =
//             new BlockMatrixSolverCL<GCRSolverCL<StokesSolverAsPreCL> > (gcr);

    // Navier-Stokes-Solver
    NSSolverBaseCL<InstatNavierStokes2PhaseP2P1CL>* navstokessolver = 0;
    if (C.ns_Nonlinear==0.0)
        navstokessolver = new NSSolverBaseCL<InstatNavierStokes2PhaseP2P1CL>(Stokes, *stokessolver);
    else
        navstokessolver = new AdaptFixedPtDefectCorrCL<InstatNavierStokes2PhaseP2P1CL>(Stokes, *stokessolver, C.ns_Iter, C.ns_Tol, C.ns_Reduction);

    // Level-Set-Solver
#ifndef _PAR
    SSORPcCL ssorpc;
    GMResSolverCL<SSORPcCL>* gm = new GMResSolverCL<SSORPcCL>( ssorpc, 100, C.lvs_Iter, C.lvs_Tol);
#else
    ParJac0CL jacparpc( *lidx);
    ParPreGMResSolverCL<ParJac0CL>* gm = new ParPreGMResSolverCL<ParJac0CL>
           (/*restart*/100, C.lvs_Iter, C.lvs_Tol, *lidx, jacparpc,/*rel*/true, /*modGS*/false, LeftPreconditioning, /*parmod*/true);
#endif

    LevelsetModifyCL lsetmod( C.rpm_Freq, C.rpm_Method, C.rpm_MaxGrad, C.rpm_MinGrad, C.lvs_VolCorrection, Vol, is_periodic);

    // Time discretisation + coupling
    TimeDisc2PhaseCL* timedisc= CreateTimeDisc(Stokes, lset, navstokessolver, gm, C, lsetmod);

    if (C.tm_NumSteps != 0){
        timedisc->SetTimeStep( C.tm_StepSize);
        timedisc->SetSchurPrePtr( stokessolverfactory.GetSchurPrePtr() );
    }

    if (C.ns_Nonlinear!=0.0 || C.tm_NumSteps == 0) {
        stokessolverfactory.SetMatrixA( &navstokessolver->GetAN()->GetFinest());
            //for Stokes-MGM
        stokessolverfactory.SetMatrices( &navstokessolver->GetAN()->GetCoarsest(), &Stokes.B.Data.GetCoarsest(),
                                         &Stokes.M.Data.GetCoarsest(), &Stokes.prM.Data.GetCoarsest(), &Stokes.pr_idx.GetCoarsest());
    }
    else {
        stokessolverfactory.SetMatrixA( &timedisc->GetUpperLeftBlock()->GetFinest());
            //for Stokes-MGM
        stokessolverfactory.SetMatrices( &timedisc->GetUpperLeftBlock()->GetCoarsest(), &Stokes.B.Data.GetCoarsest(),
                                         &Stokes.M.Data.GetCoarsest(), &Stokes.prM.Data.GetCoarsest(), &Stokes.pr_idx.GetCoarsest());
    }

    UpdateProlongationCL PVel( Stokes.GetMG(), stokessolverfactory.GetPVel(), &Stokes.vel_idx, &Stokes.vel_idx);
    adap.push_back( &PVel);
    UpdateProlongationCL PPr ( Stokes.GetMG(), stokessolverfactory.GetPPr(), &Stokes.pr_idx, &Stokes.pr_idx);
    adap.push_back( &PPr);
    // For a two-level MG-solver: P2P1 -- P2P1X;
//     MakeP1P1XProlongation ( Stokes.vel_idx.NumUnknowns(), Stokes.pr_idx.NumUnknowns(),
//         Stokes.pr_idx.GetFinest().GetXidx().GetNumUnknownsStdFE(),
//         stokessolverfactory.GetPVel()->GetFinest(), stokessolverfactory.GetPPr()->GetFinest());

    std::ofstream* infofile = 0;
    IF_MASTER {
        infofile = new std::ofstream ((C.ens_EnsCase+".info").c_str());
    }
    IFInfo.Init(infofile);
    IFInfo.WriteHeader();

    if (C.tm_NumSteps == 0)
        SolveStatProblem( Stokes, lset, *navstokessolver);

    // for serialization of geometry and numerical data
    TwoPhaseStoreCL<InstatNavierStokes2PhaseP2P1CL> ser(MG, Stokes, lset, /*C.trp_DoTransp ? &massTransp : */0, C.rst_Outputfile, C.rst_Overwrite, C.rst_Binary);

    // writer for vtk-format
    VTKOutCL vtkwriter(adap.GetMG(), "DROPS data", (C.vtk_VTKOut ? C.tm_NumSteps/C.vtk_VTKOut+1 : 0),
                std::string(C.vtk_VTKDir + "/" + C.vtk_VTKName), C.vtk_Binary);

    vtkwriter.Register( make_VTKVector( Stokes.GetVelSolution(), "velocity") );
    vtkwriter.Register( make_VTKScalar( Stokes.GetPrSolution(), "pressure") );
    vtkwriter.Register( make_VTKScalar( lset.GetSolution(), "level-set") );
/*
    if (C.trp_DoTransp) {
        vtkwriter.Register( make_VTKScalar( massTransp.GetSolution(), "massTransport") );
    }

    if (C.surf_DoTransp) {
        vtkwriter.Register( make_VTKIfaceScalar( MG, surfTransp.ic,  "InterfaceSol"));
    }
*/
    if (C.vtk_VTKOut)
        vtkwriter.Write(Stokes.v.t, true);

    for (int step= 1; step<=C.tm_NumSteps; ++step)
    {
        std::cout << "============================================================ step " << step << std::endl;

        IFInfo.Update( lset, Stokes.GetVelSolution());
        IFInfo.Write(Stokes.v.t);

//        if (C.surf_DoTransp) surfTransp.InitOld();
        timedisc->DoStep( C.cpl_Iter);
//        if (C.trp_DoTransp) massTransp.DoStep( step*C.tm_StepSize);
//        if (C.surf_DoTransp) {
//            surfTransp.DoStep( step*C.tm_StepSize);
//            BndDataCL<> ifbnd( 0);
//            std::cout << "surfactant on \\Gamma: " << Integral_Gamma( MG, lset.Phi, lset.GetBndData(), make_P1Eval(  MG, ifbnd, surfTransp.ic)) << '\n';
//        }

        // WriteMatrices( Stokes, step);

        // grid modification
        bool doGridMod= C.ref_Freq && step%C.ref_Freq == 0;
        if (doGridMod) {
            adap.UpdateTriang( lset);
            if (adap.WasModified()) {
                timedisc->Update();
//                if (C.trp_DoTransp) massTransp.Update();
            }
        }

        if (C.vtk_VTKOut && step%C.vtk_VTKOut==0)
            vtkwriter.Write(Stokes.v.t);
        if (C.rst_Serialization && step%C.rst_Serialization==0)
            ser.Write();
    }
    IFInfo.Update( lset, Stokes.GetVelSolution());
    IFInfo.Write(Stokes.v.t);
    std::cout << std::endl;
    delete timedisc;
    delete navstokessolver;
    delete stokessolver;
    delete gm;
    if (infofile) delete infofile;
//     delete stokessolver1;
}

} // end of namespace DROPS

int main (int argc, char** argv)
{
#ifdef _PAR
    DROPS::ProcInitCL procinit(&argc, &argv);
#endif
  try
  {
#ifdef _PAR
    DROPS::ParMultiGridInitCL pmginit;
#endif
    std::ifstream param;
    if (argc!=2)
    {
        std::cout << "Using default parameter file: risingdroplet.param\n";
        param.open( "risingdroplet.param");
    }
    else
        param.open( argv[1]);
    if (!param)
    {
        std::cerr << "error while opening parameter file\n";
        return 1;
    }
    param >> C;
    param.close();
    std::cout << C << std::endl;

    DROPS::MatchMap & matchmap = DROPS::MatchMap::getInstance();
    bool is_periodic = C.exp_UsePerMatching > 0;
    DROPS::match_fun periodic_match = is_periodic ? matchmap[C.exp_PerMatching] : 0;

    DROPS::MultiGridCL* mg= 0;
    typedef DROPS::BndDataCL<DROPS::Point3DCL> VelBndDataCL;
    typedef DROPS::BndDataCL<double>    PrBndDataCL; 
    VelBndDataCL *velbnddata = 0;
    PrBndDataCL *prbnddata = 0;
    DROPS::LsetBndDataCL* lsetbnddata= 0;

    DROPS::BuildDomain( mg, C.dmc_MeshFile, C.dmc_GeomType, C.rst_Inputfile, C.exp_RadInlet);
    std::cout << "Generated MG of " << mg->GetLastLevel() << " levels." << std::endl;
    
    std::string perbndtypestr;
    std::string zerobndfun;
    for( size_t i= 1; i<=mg->GetBnd().GetNumBndSeg(); ++i) {
        zerobndfun += "Zero";
        if (i!=mg->GetBnd().GetNumBndSeg())
          zerobndfun += "!";
    }
    DROPS::BuildBoundaryData( mg, velbnddata, C.dmc_BoundaryType, C.dmc_BoundaryFncs, periodic_match, &perbndtypestr);
    std::cout << "Generated boundary conditions for velocity, ";
    DROPS::BuildBoundaryData( mg, prbnddata, perbndtypestr, zerobndfun, periodic_match);
    std::cout << "pressure, ";
    DROPS::BuildBoundaryData( mg, lsetbnddata, perbndtypestr, zerobndfun, periodic_match);
    std::cout << "and levelset." << std::endl;
    DROPS::StokesBndDataCL bnddata(*velbnddata,*prbnddata);

    if (C.exp_InitialLSet == "Ellipsoid")
      DROPS::EllipsoidCL::Init( C.exp_PosDrop, C.exp_RadDrop);

    DROPS::AdapTriangCL adap( *mg, C.ref_Width, C.ref_CoarsestLevel, C.ref_FinestLevel, ((C.rst_Inputfile == "none") ? C.ref_LoadBalStrategy : -C.ref_LoadBalStrategy), C.ref_Partitioner);
    // If we read the Multigrid, it shouldn't be modified;
    // otherwise the pde-solutions from the ensight files might not fit.
//    if (C.rst_Inputfile == "none")
//        adap.MakeInitialTriang( * DROPS::ScaMap::getInstance()[C.exp_InitialLSet]);
    mg->SizeInfo(std::cout);
    std::cout << DROPS::SanityMGOutCL(*mg) << std::endl;
    for (int i = 0; i<C.ref_FinestLevel; ++i) {
        MarkAll( *mg);
        mg->Refine();
    }
    mg->SizeInfo(std::cout);
    std::cout << DROPS::SanityMGOutCL(*mg) << std::endl;
#ifdef _PAR
    adap.GetLb().GetLB().SetWeightFnct(3);
 //   if (DROPS::ProcCL::Check( CheckParMultiGrid( adap.GetPMG())))
 //       std::cout << "As far as I can tell the ParMultigridCl is sane\n";
#endif
    DROPS::InstatNavierStokes2PhaseP2P1CL prob( *mg, DROPS::TwoPhaseFlowCoeffCL(C), bnddata, C.stk_XFEMStab<0 ? DROPS::P1_FE : DROPS::P1X_FE, C.stk_XFEMStab);

    Strategy( prob, *lsetbnddata, adap);    // do all the stuff

    delete mg;
    delete velbnddata;
    delete prbnddata;
    delete lsetbnddata;
  }
  catch (DROPS::DROPSErrCL err) { err.handle(); }
  return 0;
}

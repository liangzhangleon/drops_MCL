#include "geom/multigrid.h"
#include "out/output.h"
#include "geom/builder.h"
#include "stokes/stokes.h"
#include "num/fe.h"
#include "num/stokessolver.h"
#include "num/MGsolver.h"
#include <fstream>


inline DROPS::SVectorCL<3> LsgVel(const DROPS::Point3DCL& p, double)
{
    DROPS::SVectorCL<3> ret;
    ret[0]=    sin(p[0])*sin(p[1])*sin(p[2])/3.;
    ret[1]=  - cos(p[0])*cos(p[1])*sin(p[2])/3.;
    ret[2]= 2.*cos(p[0])*sin(p[1])*cos(p[2])/3.;
    return ret;
}

// Jacobi-matrix of the exact solution
inline DROPS::SMatrixCL<3, 3> DLsgVel(const DROPS::Point3DCL& p)
{
    DROPS::SMatrixCL<3, 3> ret;
        ret(0,0)= cos(p[0])*sin(p[1])*sin(p[2])/3.;
        ret(0,1)= sin(p[0])*cos(p[1])*sin(p[2])/3.;
        ret(0,2)= sin(p[0])*sin(p[1])*cos(p[2])/3.;

        ret(1,0)=   sin(p[0])*cos(p[1])*sin(p[2])/3.;
        ret(1,1)=   cos(p[0])*sin(p[1])*sin(p[2])/3.;
        ret(1,2)= - cos(p[0])*cos(p[1])*cos(p[2])/3.;

        ret(2,0)= -2.*sin(p[0])*sin(p[1])*cos(p[2])/3.;
        ret(2,1)=  2.*cos(p[0])*cos(p[1])*cos(p[2])/3.;
        ret(2,2)= -2.*cos(p[0])*sin(p[1])*sin(p[2])/3.;
    return ret;
}


// Volume of the box: 0.484473073129685
// int(p)/vol = -0.125208551608365
inline double LsgPr(const DROPS::Point3DCL& p)
{
    return cos(p[0])*sin(p[1])*sin(p[2]) - 0.125208551608365;
}
inline double LsgPr(const DROPS::Point3DCL& p, double)
{
    return cos(p[0])*sin(p[1])*sin(p[2]) - 0.125208551608365;
}


// q*u - nu*laplace u + Dp = f
//                  -div u = 0
class StokesCoeffCL
{
  public:
    static double q(const DROPS::Point3DCL&) { return 0.0; }
    static DROPS::SVectorCL<3> f(const DROPS::Point3DCL& p, double)
        { DROPS::SVectorCL<3> ret(0.0); ret[2]= 3.*cos(p[0])*sin(p[1])*cos(p[2]); return ret; }
    const double nu;
    
    StokesCoeffCL() : nu(1.0) {}
};

typedef DROPS::StokesP2P1CL<StokesCoeffCL> 
        StokesOnBrickCL;
typedef StokesOnBrickCL MyStokesCL;

namespace DROPS // for Strategy
{
using ::MyStokesCL;

template<class Coeff>
void Strategy(StokesP2P1CL<Coeff>& Stokes, double inner_iter_tol, double tol, int meth,
                                            Uint maxStep, double rel_red, double markratio,
                                            double tau, Uint uzawa_inner_iter)
// flow control
{
    MultiGridCL& MG= Stokes.GetMG();
    const typename MyStokesCL::BndDataCL::PrBndDataCL& PrBndData= Stokes.GetBndData().Pr;
    const typename MyStokesCL::BndDataCL::VelBndDataCL& VelBndData= Stokes.GetBndData().Vel;

    IdxDescCL  loc_vidx, loc_pidx;
    IdxDescCL* vidx1= &Stokes.vel_idx;
    IdxDescCL* pidx1= &Stokes.pr_idx;
    IdxDescCL* vidx2= &loc_vidx;
    IdxDescCL* pidx2= &loc_pidx;
    
    VecDescCL     loc_p;
    VelVecDescCL  loc_v;
    VelVecDescCL* v1= &Stokes.v;
    VelVecDescCL* v2= &loc_v;
    VecDescCL*    p1= &Stokes.p;
    VecDescCL*    p2= &loc_p;
    VelVecDescCL* b= &Stokes.b;
    VelVecDescCL* c= &Stokes.c;

    MatDescCL* A= &Stokes.A;
    MatDescCL* B= &Stokes.B;

    Uint step= 0;
    StokesDoerflerMarkCL<typename MyStokesCL::est_fun, MyStokesCL>
        Estimator(rel_red, markratio, .484473073129685, true, &MyStokesCL::ResidualErrEstimator, Stokes);
    bool new_marks= false;

    vidx1->Set(3, 3, 0, 0);
    vidx2->Set(3, 3, 0, 0);
    pidx1->Set(1, 0, 0, 0);
    pidx2->Set(1, 0, 0, 0);
    TimerCL time;

    do
    {
        MG.Refine();
        Stokes.CreateNumberingVel(MG.GetLastLevel(), vidx1);    
        Stokes.CreateNumberingPr(MG.GetLastLevel(), pidx1);    
        std::cerr << "altes und neues TriangLevel: " << vidx2->TriangLevel << ", "
                  << vidx1->TriangLevel << std::endl;
		  
/*     std::cout << "    altes                    vidx2 " 
               << vidx2->TriangLevel       << ", "
               << vidx2->NumUnknownsVertex << ", "
               << vidx2->NumUnknownsEdge   << ", "
               << vidx2->NumUnknownsFace   << ", "
               << vidx2->NumUnknownsTetra  << ", "
               << vidx2->NumUnknowns
               << std::endl;
     std::cout << "    neues                    vidx1 " 
               << vidx1->TriangLevel       << ", "
               << vidx1->NumUnknownsVertex << ", "
               << vidx1->NumUnknownsEdge   << ", "
               << vidx1->NumUnknownsFace   << ", "
               << vidx1->NumUnknownsTetra  << ", "
               << vidx1->NumUnknowns
               << std::endl;
*/
        MG.SizeInfo(std::cerr);
        b->SetIdx(vidx1);
        c->SetIdx(pidx1);
        p1->SetIdx(pidx1);
        v1->SetIdx(vidx1);
        std::cerr << "Anzahl der Druck-Unbekannten: " << p2->Data.size() << ", "
                  << p1->Data.size() << std::endl;
        std::cerr << "Anzahl der Geschwindigkeitsunbekannten: " << v2->Data.size() << ", "
                  << v1->Data.size() << std::endl;
    
        if (p2->RowIdx)
        {
            P1EvalCL<double, const StokesBndDataCL::PrBndDataCL, const VecDescCL>  pr2(p2, &PrBndData, &MG);
            P1EvalCL<double, const StokesBndDataCL::PrBndDataCL, VecDescCL>        pr1(p1, &PrBndData, &MG);
//            P2EvalCL<SVectorCL<3>, const StokesVelBndDataCL, const VelVecDescCL> vel2(v2, &VelBndData, &MG);
//            P2EvalCL<SVectorCL<3>, const StokesVelBndDataCL, VelVecDescCL>       vel1(v1, &VelBndData, &MG);
            Interpolate(pr1, pr2);
//            Interpolate(vel1, vel2);
//            Stokes.CheckSolution(v1,p1,&LsgVel, &DLsgVel, &LsgPr);
            v2->Reset();
            p2->Reset();
        }
        A->SetIdx(vidx1, vidx1);
        B->SetIdx(pidx1, vidx1);
        time.Reset();
        time.Start();
        Stokes.SetupSystem(A, b, B, c);
        time.Stop();
        std::cerr << time.GetTime() << " seconds for setting up all systems!" << std::endl;
        time.Reset();
        time.Start();
        A->Data * v1->Data;
        time.Stop();
        std::cerr << " A*x took " << time.GetTime() << " seconds!" << std::endl;
        time.Reset();
        time.Start();
        transp_mul( A->Data, v1->Data);
        time.Stop();
        std::cerr << "AT*x took " << time.GetTime() << " seconds!" << std::endl;
        
//        { // write system in files for MatLab
//            std::ofstream Adat("Amat.dat"), Bdat("Bmat.dat"), bdat("fvec.dat"), cdat("gvec.dat");
//            Adat << A->Data;   Bdat << B->Data;    bdat << b->Data;    cdat << c->Data;
//        }
        Stokes.GetDiscError(&LsgVel, &LsgPr);
//std::cout << A->Data << std::endl << b->Data << std::endl
//          << B->Data << std::endl << c->Data << std::endl
//          << v1->Data << std::endl << p1->Data << std::endl;

// Initialize the preconditioners 
//            for Laplace operator: MGDataMatrix (Idx,A,P)
// and        for Schur complement: MassMatrix

        time.Reset();
	
	MGDataCL MGData;
	IdxDescCL* c_idx=0;
        for(Uint lvl= 0; lvl<=MG.GetLastLevel(); ++lvl)
        {
           MGData.push_back(MGLevelDataCL());
           MGLevelDataCL& tmp= MGData.back();
     
           std::cerr << "                        Create MGData on Level " << lvl << std::endl;
           tmp.Idx.Set( 3, 3);
           Stokes.CreateNumberingVel(lvl, &tmp.Idx);
           tmp.A.SetIdx( &tmp.Idx, &tmp.Idx);
           std::cerr << "                        Create StiffMatrix     " << (&tmp.Idx)->NumUnknowns <<std::endl;
           Stokes.SetupStiffnessMatrix( &tmp.A );
            
           if(lvl!=0)
           {
               std::cerr << "                       Create Prolongation on Level " << lvl << std::endl;
               SetupP2ProlongationMatrix( MG, tmp.P, c_idx, &tmp.Idx);
//               std::cout << "    Matrix P " << tmp.P.Data << std::endl;
	       
           }
           c_idx= &tmp.Idx;
        }
	MGDataCL& MGD = MGData;
	
        MatDescCL M;
        M.SetIdx( pidx1, pidx1);
        Stokes.SetupPrMass( &M);

        time.Stop();
        std::cerr << "Setting up all preconditioners took " << time.GetTime() 
	          << " seconds. " << std::endl;
		  
        std::cerr << "Check MG-Data..." << std::endl;
        std::cerr << "                begin     " << MGData.begin()->Idx.NumUnknowns << std::endl;
        std::cerr << "                end       " << MGData.end()->Idx.NumUnknowns << std::endl;
        CheckMGData( MGData.begin(), MGData.end() );

// Solve
        double outer_tol= tol;
        if (meth)
        {
            std::cerr << "Schur complement method..." << std::endl;
            PSchur_MG_CL MGschurSolver( M.Data, 200, outer_tol, MGD, 200, inner_iter_tol);
//            PSchur_PCG_CL schurSolver( M.Data, 200, outer_tol, 200, inner_iter_tol);
            time.Start();
//std::cout << M.Data << std::endl;
//std::cout << A->Data << std::endl << b->Data << std::endl
//          << B->Data << std::endl << c->Data << std::endl
//          << v1->Data << std::endl << p1->Data << std::endl;
//            schurSolver.Solve( A->Data, B->Data, v1->Data, p1->Data, b->Data, c->Data);
            MGschurSolver.Solve( A->Data, B->Data, v1->Data, p1->Data, b->Data, c->Data);
            time.Stop();
        }
        else // Uzawa
        {
            std::cerr << "Uzawa method..." << std::endl;
//            double tau;
//            Uint inner_iter;
//            std::cerr << "tau = "; std::cin >> tau;
//            std::cerr << "#PCG steps = "; std::cin >> inner_iter;
            Uzawa_PCG_CL uzawaSolver( M.Data, 5000, outer_tol, uzawa_inner_iter, inner_iter_tol, tau);
            time.Start();
            uzawaSolver.Solve( A->Data, B->Data, v1->Data, p1->Data, b->Data, c->Data);
            time.Stop();
            std::cerr << "Iterationen: " << uzawaSolver.GetIter()
                      << "Norm des Res.: " << uzawaSolver.GetResid() << std::endl;
        }
        std::cerr << "Das Verfahren brauchte "<<time.GetTime()<<" Sekunden.\n";
        Stokes.CheckSolution(v1, p1, &LsgVel, &DLsgVel, &LsgPr);
        if (step==0)
        {
            Estimator.Init(typename MyStokesCL::DiscPrSolCL(p1, &PrBndData, &MG), typename MyStokesCL::DiscVelSolCL(v1, &VelBndData, &MG));
        }
        time.Reset();
        time.Start();
    char dummy;
    std::cin >> dummy;
        new_marks= Estimator.Estimate(typename MyStokesCL::DiscPrSolCL(p1, &PrBndData, &MG), typename MyStokesCL::DiscVelSolCL(v1, &VelBndData, &MG) );
        time.Stop();
        std::cerr << "Estimation took " << time.GetTime() << " seconds\n";
        A->Reset();
        B->Reset();
        b->Reset();
        c->Reset();
//        std::cerr << "Loesung Druck: " << p1->Data << std::endl;
        std::swap(v2, v1);
        std::swap(p2, p1);
        std::swap(vidx2, vidx1);
        std::swap(pidx2, pidx1);
        std::cerr << std::endl;
    }
    while (++step<maxStep);
    // we want the solution to be in Stokes.v, Stokes.pr
    if (v2 == &loc_v)
    {
        Stokes.vel_idx.swap( loc_vidx);
        Stokes.pr_idx.swap( loc_pidx);
        Stokes.v.SetIdx(&Stokes.vel_idx);
        Stokes.p.SetIdx(&Stokes.pr_idx);
        
        Stokes.v.Data= loc_v.Data;
        Stokes.p.Data= loc_p.Data;
    }
}

} // end of namespace DROPS


int main (int argc, char** argv)
{
  try
  {
    if (argc!=9)
    {
        std::cerr << "Usage: sdropsP2 <inner_iter_tol> <tol> <meth> <num_refinement> <rel_red> <markratio> <tau> <uz_inner_iter>"
                  << std::endl;
        return 1;
    }
    double inner_iter_tol= atof(argv[1]);
    double tol= atof(argv[2]);
    int meth= atoi(argv[3]);
    int num_ref= atoi(argv[4]);
    double rel_red= atof(argv[5]);
    double markratio= atof(argv[6]);
    double tau= atof(argv[7]);
    unsigned int uz_inner_iter= atoi(argv[8]);
    std::cerr << "inner iter tol: " << inner_iter_tol << ", "
              << "tol: " << tol << ", "
              << "meth: " << meth << ", "
              << "refinements: " << num_ref << ", "
              << "relative error reduction: " << rel_red << ", "
              << "markratio: " << markratio << ", "
              << "tau: " << tau << ", "
              << "uzawa inner iter: " << uz_inner_iter
              << std::endl;

    DROPS::Point3DCL null(0.0);
    DROPS::Point3DCL e1(0.0), e2(0.0), e3(0.0);
    e1[0]= e2[1]= e3[2]= M_PI/4.;

    DROPS::BrickBuilderCL brick(null, e1, e2, e3, 3, 3, 3);
    const bool IsNeumann[6]= 
        {false, false, false, false, false, false};
    const DROPS::StokesBndDataCL::VelBndDataCL::bnd_val_fun bnd_fun[6]= 
        { &LsgVel, &LsgVel, &LsgVel, &LsgVel, &LsgVel, &LsgVel};
        
    StokesOnBrickCL prob(brick, StokesCoeffCL(), DROPS::StokesBndDataCL(6, IsNeumann, bnd_fun));
    DROPS::MultiGridCL& mg = prob.GetMG();
    DROPS::RBColorMapperCL colormap;

    Strategy(prob, inner_iter_tol, tol, meth, num_ref, rel_red, markratio, tau, uz_inner_iter);
    std::cerr << "hallo" << std::endl;
    std::cerr << DROPS::SanityMGOutCL(mg) << std::endl;
    std::ofstream fil("ttt.off");
    double min= prob.p.Data.min(),
           max= prob.p.Data.max();
    fil << DROPS::GeomSolOutCL<MyStokesCL::DiscPrSolCL>(mg, prob.GetPrSolution(), &colormap, -1, false, 0.0, min, max) << std::endl;

//    std::cout << DROPS::GeomMGOutCL(mg, -1, true) << std::endl;
    return 0;
  }
  catch (DROPS::DROPSErrCL err) { err.handle(); }
}

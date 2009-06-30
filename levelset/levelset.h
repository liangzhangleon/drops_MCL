/// \file
/// \brief levelset equation for two phase flow problems
/// \author Sven Gross, Joerg Peters, Volker Reichelt, IGPM RWTH Aachen
///         Oliver Fortmeier, SC RWTH Aachen

#ifndef DROPS_LEVELSET_H
#define DROPS_LEVELSET_H

#include "num/spmat.h"
#include "num/discretize.h"
#include "num/solver.h"
#include "num/bndData.h"
#include "num/fe.h"
#include "levelset/mgobserve.h"
#include <vector>

#ifdef _PAR
# include "parallel/exchange.h"
# include "num/parsolver.h"
# include "num/parprecond.h"
# include "misc/container.h"
#endif

namespace DROPS
{

enum SurfaceForceT
/// different types of surface forces
{
    SF_LB=0,             ///< Laplace-Beltrami discretization: \f$\mathcal O(h^{1/2})\f$
    SF_ImprovedLB=1,     ///< improved Laplace-Beltrami discretization: \f$\mathcal O(h)\f$
    SF_Const=2,          ///< surface force with constant curvature
    SF_ImprovedLBVar=3   ///< improved Laplace-Beltrami discretization with variable surface tension
};

class LevelsetP2CL
/// P2-discretization and solution of the level set equation for two phase flow problems.
{
  public:
    typedef BndDataCL<>    BndDataT;
    typedef P2EvalCL<double, const BndDataT, VecDescCL>       DiscSolCL;
    typedef P2EvalCL<double, const BndDataT, const VecDescCL> const_DiscSolCL;

    IdxDescCL             idx;
    VecDescCL             Phi;        ///< level set function
    instat_scalar_fun_ptr sigma;      ///< variable surface tension
    instat_vector_fun_ptr grad_sigma; ///< gradient of sigma

  private:
    MultiGridCL&        MG_;
    double              curvDiff_, ///< amount of diffusion in curvature calculation
                        SD_;       ///< streamline diffusion
    BndDataT            Bnd_;
    SurfaceForceT       SF_;

    void SetupReparamSystem( MatrixCL&, MatrixCL&, const VectorCL&, VectorCL&) const;
    void SetupSmoothSystem ( MatrixCL&, MatrixCL&)                             const;
    void SmoothPhi( VectorCL& SmPhi, double diff)                              const;

  public:
    MatrixCL            E, H;

    LevelsetP2CL( MultiGridCL& mg, instat_scalar_fun_ptr sig= 0,instat_vector_fun_ptr gsig= 0,
        double SD= 0., double curvDiff= -1., double __UNUSED__ narrowBand=-1.)
    : idx( P2_FE), sigma( sig), grad_sigma( gsig), MG_( mg), curvDiff_( curvDiff), SD_( SD),
        Bnd_( BndDataT(mg.GetBnd().GetNumBndSeg())), SF_( SF_ImprovedLB)
    {}

    LevelsetP2CL( MultiGridCL& mg, const BndDataT& bnd, instat_scalar_fun_ptr sig= 0,
        instat_vector_fun_ptr gsig= 0, double SD= 0, double curvDiff= -1)
    : idx( P2_FE), sigma( sig), grad_sigma( gsig), MG_( mg), curvDiff_( curvDiff), SD_( SD),
        Bnd_( bnd), SF_(SF_ImprovedLB)
    {}

    const MultiGridCL& GetMG() const { return MG_; }    ///< Get reference on the multigrid
    MultiGridCL& GetMG() { return MG_; }                ///< Get reference on the multigrid

    const BndDataT& GetBndData() const { return Bnd_; }

    /// \name Numbering
    ///@{
    void CreateNumbering( Uint level, IdxDescCL* idx, match_fun match= 0);
    void DeleteNumbering( IdxDescCL* idx)
        { idx->DeleteNumbering( MG_); }
    ///@}

    /// initialize level set function
    void Init( scalar_fun_ptr);

    /// \remarks call SetupSystem \em before calling SetTimeStep!
    template<class DiscVelSolT>
    void SetupSystem( const DiscVelSolT&);
    /// Reparametrization by Fast Marching method (recommended).
    void ReparamFastMarching( bool ModifyZero= true, bool Periodic= false, bool OnlyZeroLvl= false, bool euklid= false);

    /// tests whether level set function changes its sign on tetra \p t.
    bool   Intersects( const TetraCL&) const;
    /// returns information about level set function and interface.
    template<class DiscVelSolT>
    void   GetInfo( double& maxGradPhi, double& Volume, Point3DCL& bary, Point3DCL& vel, const DiscVelSolT& vel_sol, Point3DCL& minCoord, Point3DCL& maxCoord) const;
    /// returns the maximum and minimum of the gradient of phi
    void   GetMaxMinGradPhi(double& maxGradPhi, double& minGradPhi) const;
    /// returns approximate volume of domain where level set function is negative.
    double GetVolume( double translation= 0) const;
    /// volume correction to ensure no loss or gain of mass.
    double AdjustVolume( double vol, double tol, double surf= 0) const;
    /// Set type of surface force.
    void   SetSurfaceForce( SurfaceForceT SF) { SF_= SF; }
    /// Get type of surface force.
    SurfaceForceT GetSurfaceForce() const { return SF_; }
    /// Discretize surface force
    void   AccumulateBndIntegral( VecDescCL& f) const;
    /// Set surface tension and its gradient.
    void   SetSigma( instat_scalar_fun_ptr sig, instat_vector_fun_ptr gsig= 0) { sigma= sig; grad_sigma= gsig; }
    /// Clear all matrices, should be called after grid change to avoid reuse of matrix pattern
    void   ClearMat() { E.clear(); H.clear(); }
    /// \name Evaluate Solution
    ///@{
    const_DiscSolCL GetSolution() const
        { return const_DiscSolCL( &Phi, &Bnd_, &MG_); }
    const_DiscSolCL GetSolution( const VecDescCL& MyPhi) const
        { return const_DiscSolCL( &MyPhi, &Bnd_, &MG_); }
    ///@}
};


/// \brief Observes the MultiGridCL-changes by AdapTriangCL to repair the Function ls.Phi.
///
/// Sequential: The actual work is done in post_refine().<br>
/// Parallel:
/// - In pre_refine_sequence the parallel multigrid is informed about
///  the DOF of the level-set function in order to handle them during the
///  refinement and the load-migration.
/// - In post_refine_sequence the actual work is done.
class LevelsetRepairCL : public MGObserverCL
{
  private:
    LevelsetP2CL& ls_;

  public:
    /// \brief Construct a levelset repair class
    LevelsetRepairCL (LevelsetP2CL& ls)
        : ls_( ls) {}

    void pre_refine  ();
    void post_refine ();

    void pre_refine_sequence  () {}
    void post_refine_sequence () {}
};

/// \brief volume correction and reparametrisation of level set function phi
class LevelsetModifyCL
{
private:
	int    rpm_Freq_,
	       rpm_Method_;
	double rpm_MaxGrad_,
	       rpm_MinGrad_;
    int    lvs_VolCorrection_;

    const double Vol_;

    int    step_;
    bool   reparam_;

public:
    LevelsetModifyCL( int rpm_Freq, int rpm_Method, double rpm_MaxGrad, double rpm_MinGrad, int lvs_VolCorrection, double Vol) :
        rpm_Freq_( rpm_Freq), rpm_Method_( rpm_Method), rpm_MaxGrad_( rpm_MaxGrad),
        rpm_MinGrad_( rpm_MinGrad), lvs_VolCorrection_( lvs_VolCorrection), Vol_( Vol), step_( 0), reparam_(true) {}


    double maybeModify( LevelsetP2CL& lset) {
        bool doReparam= reparam_ && rpm_Freq_ && step_%rpm_Freq_ == 0;
        bool doVolCorr= lvs_VolCorrection_ && step_%lvs_VolCorrection_ == 0;
        double dphi = 0.0;

        if (! (doReparam || doVolCorr)) return dphi;

        double lsetmaxGradPhi, lsetminGradPhi;

        if (doReparam) {
            lset.GetMaxMinGradPhi( lsetmaxGradPhi, lsetminGradPhi);
            doReparam = (lsetmaxGradPhi > rpm_MaxGrad_ || lsetminGradPhi < rpm_MinGrad_);
        }

        // volume correction before reparam
        if (lvs_VolCorrection_ && doReparam) {
            dphi= lset.AdjustVolume( Vol_, 1e-9);
            std::cout << "volume correction is " << dphi << std::endl;
            lset.Phi.Data+= dphi;
            std::cout << "new rel. volume: " << lset.GetVolume()/Vol_ << std::endl;
        }

        // reparam levelset function
        if (doReparam) {
            std::cout << "before reparametrization: minGradPhi " << lsetminGradPhi << "\tmaxGradPhi " << lsetmaxGradPhi << '\n';
            lset.ReparamFastMarching( rpm_Method_, false, false, rpm_Method_==3);
            lset.GetMaxMinGradPhi( lsetmaxGradPhi, lsetminGradPhi);
            std::cout << "after  reparametrization: minGradPhi " << lsetminGradPhi << "\tmaxGradPhi " << lsetmaxGradPhi << '\n';
            reparam_ = false;
        }

        if (doVolCorr) {
            dphi= lset.AdjustVolume( Vol_, 1e-9);
            std::cout << "volume correction is " << dphi << std::endl;
            lset.Phi.Data+= dphi;
            std::cout << "new rel. volume: " << lset.GetVolume()/Vol_ << std::endl;
        }
        return dphi;
    }
    void init() {
        step_++;
        reparam_ = true;
    }
};



/// marks all tetrahedra in the band |\p DistFct(x)| < \p width for refinement
void MarkInterface (scalar_fun_ptr DistFct, double width, MultiGridCL&);
/// marks all tetrahedra in the band |\p lset(x)| < \p width for refinement
void MarkInterface ( const LevelsetP2CL::const_DiscSolCL& lset, double width, MultiGridCL& mg);

} // end of namespace DROPS

#include "levelset/levelset.tpp"

#endif


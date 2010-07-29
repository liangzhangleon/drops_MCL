/// \file levelset.tpp
/// \brief levelset equation for two phase flow problems
/// \author LNM RWTH Aachen: Patrick Esser, Joerg Grande, Sven Gross, Volker Reichelt; SC RWTH Aachen: Oliver Fortmeier

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

#include "num/discretize.h"
#include "levelset/fastmarch.h"
#include <fstream>

namespace DROPS
{

template<class DiscVelSolT>
void LevelsetP2CL::GetInfo( double& maxGradPhi, double& Volume, Point3DCL& bary, Point3DCL& vel, const DiscVelSolT& velsol, Point3DCL& minCoord, Point3DCL& maxCoord, double& surfArea) const
/**
 * - \p maxGradPhi is the maximal 2-norm of the gradient of the level set function. This can be used as an indicator to decide
 *   whether a reparametrization should be applied.
 * - \p Volume is the volume inside the approximate interface consisting of planar segments.
 * - \p bary is the barycenter of the droplet.
 * - \p vel is the velocity of the barycenter of the droplet.
 * - The entries of \p minCoord store the minimal x, y and z coordinates of the approximative interface, respectively.
 * - The entries of \p maxCoord store the maximal x, y and z coordinates of the approximative interface, respectively.
 * - \p surfArea is the surface area of the approximative interface
 */
{
    Quad2CL<Point3DCL> Grad[10], GradRef[10];
    SMatrixCL<3,3> T;
    double det, absdet;
    InterfaceTetraCL tetra;  
    InterfaceTriangleCL triangle;
    
    P2DiscCL::GetGradientsOnRef( GradRef);
    maxGradPhi= -1.;
    Volume= 0.;
    surfArea= 0.;
    bary[0]= bary[1]= bary[2]= 0;
    vel[0]= vel[1]= vel[2]= 0;
    minCoord[0]= minCoord[1]= minCoord[2]= 1e99;
    maxCoord[0]= maxCoord[1]= maxCoord[2]= -1e99;
    LocalP2CL<double> ones( 1.);
    LocalP2CL<Point3DCL> Coord, Vel;

    for (MultiGridCL::const_TriangTetraIteratorCL it=const_cast<const MultiGridCL&>(MG_).GetTriangTetraBegin(), end=const_cast<const MultiGridCL&>(MG_).GetTriangTetraEnd();
        it!=end; ++it)
    {
        GetTrafoTr( T, det, *it);
        absdet= std::abs( det);
        P2DiscCL::GetGradients( Grad, GradRef, T); // Gradienten auf aktuellem Tetraeder

        tetra.Init( *it, Phi, BndData_);
        triangle.Init( *it, Phi, BndData_);

        // compute maximal norm of grad Phi
        Quad2CL<Point3DCL> gradPhi;
        for (int v=0; v<10; ++v) // init gradPhi, Coord
        {
            gradPhi+= tetra.GetPhi(v)*Grad[v];
            Coord[v]= v<4 ? it->GetVertex(v)->GetCoord() : GetBaryCenter( *it->GetEdge(v-4));
        }
        Vel.assign( *it, velsol);
        VectorCL normGrad( 5);
        for (int v=0; v<5; ++v) // init normGrad
            normGrad[v]= norm( gradPhi[v]);
        const double maxNorm= normGrad.max();
        if (maxNorm > maxGradPhi) maxGradPhi= maxNorm;

        for (int ch=0; ch<8; ++ch)
        {
            // compute volume, barycenter and velocity
            tetra.ComputeCutForChild(ch);
            Volume+= tetra.quad( ones, absdet, false);
            bary+= tetra.quad( Coord, absdet, false);
            vel+= tetra.quad( Vel, absdet, false);

            // find minimal/maximal coordinates of interface
            if (!triangle.ComputeForChild(ch)) // no patch for this child
                continue;
            for (int tri=0; tri<triangle.GetNumTriangles(); ++tri)
                surfArea+= triangle.GetAbsDet(tri);
            for (Uint i=0; i<triangle.GetNumPoints(); ++i)
            {
                const Point3DCL p= triangle.GetPoint(i);
                for (int j=0; j<3; ++j)
                {
                    if (p[j] < minCoord[j]) minCoord[j]= p[j];
                    if (p[j] > maxCoord[j]) maxCoord[j]= p[j];
                }
            }
        }
    }
#ifdef _PAR
    // Globalization of  data
    // -----
    const Point3DCL local_bary(bary), local_vel(vel), local_minCoord(minCoord), local_maxCoord(maxCoord);
    maxGradPhi= ProcCL::GlobalMax(maxGradPhi);
    Volume    = ProcCL::GlobalSum(Volume);
    surfArea  = ProcCL::GlobalSum(surfArea);
    ProcCL::GlobalSum(Addr(local_bary), Addr(bary), 3);
    ProcCL::GlobalSum(Addr(local_vel), Addr(vel), 3);
    ProcCL::GlobalMin(Addr(local_minCoord), Addr(minCoord), 3);
    ProcCL::GlobalMax(Addr(local_maxCoord), Addr(maxCoord), 3);
#endif

    bary/= Volume;
    vel/= Volume;
    surfArea*= 0.5;
}

template<class DiscVelSolT>
void LevelsetP2CL::SetupSystem( const DiscVelSolT& vel, const double dt)
/**Sets up the stiffness matrices: <br>
   E is of mass matrix type:    E_ij = ( v_j       , v_i + SD * u grad v_i ) <br>
   H describes the convection:  H_ij = ( u grad v_j, v_i + SD * u grad v_i ) <br>
   where v_i, v_j denote the ansatz functions.
   \remarks call SetupSystem \em before calling SetTimeStep!
   \todo: implementation of other boundary conditions
*/
{
    const IdxT num_unks= Phi.RowIdx->NumUnknowns();
    const Uint lvl= Phi.GetLevel();

    SparseMatBuilderCL<double> bE(&E, num_unks, num_unks),
                               bH(&H, num_unks, num_unks);

#ifndef _PAR
    __UNUSED__ const IdxT allnum_unks= num_unks;
#else
    __UNUSED__ const IdxT allnum_unks= DROPS::ProcCL::GlobalSum(num_unks);
#endif
    Comment("entering Levelset::SetupSystem: " << allnum_unks << " levelset unknowns.\n", DebugDiscretizeC);

    // fill value part of matrices
    Quad5CL<Point3DCL> Grad[10], GradRef[10], u_loc;
    Quad5CL<double> u_Grad[10]; // fuer u grad v_i
    SMatrixCL<3,3> T;
    double det, absdet, h_T;
    LocalNumbP2CL n;
    
    P2DiscCL::GetGradientsOnRef( GradRef);

    for (MultiGridCL::const_TriangTetraIteratorCL sit=const_cast<const MultiGridCL&>(MG_).GetTriangTetraBegin(lvl), send=const_cast<const MultiGridCL&>(MG_).GetTriangTetraEnd(lvl);
         sit!=send; ++sit)
    {
        GetTrafoTr( T, det, *sit);
        P2DiscCL::GetGradients( Grad, GradRef, T);
        absdet= std::fabs( det);
        h_T= std::pow( absdet, 1./3.);

        
        // save information about the edges and verts of the tetra in Numb
        n.assign( *sit, *Phi.RowIdx, BndData_);
        
        // save velocities inside tetra for quadrature in u_loc
        u_loc.assign( *sit, vel);

        for(int i=0; i<10; ++i)
            u_Grad[i]= dot( u_loc, Grad[i]);

        double maxV = 0.; // scaling of SD parameter (cf. master thesis of Rodolphe Prignitz)
        const double limit= (h_T*h_T)/dt;
        //const double limit= 1e-3;
        for(int i=0; i<Quad5DataCL::NumNodesC; ++i)
            maxV = std::max( maxV, u_loc[i].norm());
        maxV= std::max( maxV, limit/h_T); // no scaling for extremely small velocities
        //double maxV= 1; // no scaling
        for(int i=0; i<10; ++i)    // assemble row Numb[i]
            for(int j=0; j<10; ++j)
            {
                // E is of mass matrix type:    E_ij = ( v_j       , v_i + SD * u grad v_i )
                bE( n.num[i], n.num[j])+= P2DiscCL::GetMass(i,j) * absdet
                                     + u_Grad[i].quadP2(j, absdet)*SD_/maxV*h_T;

                // H describes the convection:  H_ij = ( u grad v_j, v_i + SD * u grad v_i )
                bH( n.num[i], n.num[j])+= u_Grad[j].quadP2(i, absdet)
                                     + Quad5CL<>(u_Grad[i]*u_Grad[j]).quad( absdet) * SD_/maxV*h_T;
            }
    }

    bE.Build();
    bH.Build();
#ifndef _PAR
    Comment(E.num_nonzeros() << " nonzeros in E, "<< H.num_nonzeros() << " nonzeros in H! " << std::endl, DebugDiscretizeC);
#endif
}

} // end of namespace DROPS


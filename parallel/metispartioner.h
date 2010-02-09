/// \file metispartioner.h
/// \brief implements a class that is used to partition a graph with Metis/ParMetis.
/// \author LNM RWTH Aachen: ; SC RWTH Aachen: Oliver Fortmeier, Timo Henrich

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

/// \todo(of) This file must be filled with content

#ifndef DROPS_METISPARTIONER_H
#define DROPS_METISPARTIONER_H

#include "parallel/parallel.h"
#include "parallel/loadbal.h"
#include <parmetis.h>
#include <metis.h>

#include "parallel/partime.h"
#include "parallel/parmultigrid.h"
#include "geom/multigrid.h"
#include "misc/problem.h"
#include <map>
#include <set>
#include <iostream>
#include <fstream>

namespace DROPS{

enum PartMethod ;

class MetisPartionerCL
{

public:
   typedef idxtype* IndexArray;                            /// type of an array of indices

private:

    int    wgtflag_,              // Weights on vertices and adjacencies are given
           numflag_,              // numbering of verts starts by 0 (C-Style)
           ncon_,                 // one weight per vertex
           nparts_;   		  // number of subdomains (per proc one)

    int    edgecut_;		 // Taken from loadbal.h

    float *tpwgts_, 		 // weight of partion
           ubvec_;            	 // imbalace tolerance for eacht vertex weight
    int   *options_;       	 // default options

    IndexArray     xadj_,                                   // Startingindex in the array _adjncy, where node[i] writes its neighbors
                   adjncy_,                                 // Adjacencies of the nodes
                   vtxdist_,                                // numbers of nodes, that is stored by all procs
                   vwgt_,                                   // weight of the Nodes
                   adjwgt_;

public:

/**
* The class constructor
*/
MetisPartionerCL():
 wgtflag_(3),numflag_(0),ncon_(1),nparts_(DynamicDataInterfaceCL::InfoProcs()), ubvec_(1.05)
{
    tpwgts_ = new float[nparts_],
    options_ = new int[1];
}


/**
* Creates the internal graph used for the partioning algorithm. In the case of
* Metis this is excatly the graph generated by the load-balancer.
*
* \param vtxdist
* \param xadj
* \param adjncy
* \param vwgt
* \param adjwgt
*
* \todo (Oliver) Müssen hier auch der EdgeCount und Ubvec mit übergeben werden?
*/
void BuildGraph(IndexArray vtxdist,IndexArray xadj,IndexArray adjncy,IndexArray vwgt,IndexArray adjwgt)
{
	vtxdist_ = 	vtxdist;
	xadj_	 = 	xadj;
	adjncy_  = 	adjncy;
	vwgt_ 	 = 	vwgt ;
	adjwgt_  = 	adjwgt;
}

/**
* Performs a parallel partioning.
*
* \param Reference to an indexarray where the generated partion is written to.
* \param Method used for partioning.
*/
void ParPartition(IndexArray &part, PartMethod method)
{

	// \todo (Oliver) Ist das Okay so ?
	MPI_Comm comm = MPI_COMM_WORLD;

method = KWay;

	switch(method)
	{

		case KWay:

		    ParMETIS_V3_PartKway(
			    vtxdist_, xadj_, adjncy_, vwgt_, adjwgt_,
			    &wgtflag_, &numflag_, &ncon_, &nparts_,
			    tpwgts_, &ubvec_,0 ,
			    &edgecut_, part, &comm);

			break;

		case Adaptive:

			break;

		case NoMig:
			break;

		case Identity:
			break;

		default:
			std::cout<<"MetisPartioner: Method not implemented yet."<<std::endl;
	}

}

/**
* Performs a serial partioning.
*
* \param Reference to an indexarray where the generated partion is written to.
* \param Method used for partioning.
*/
void SerPartition(IndexArray &, PartMethod method )
{

	switch(method)
	{
		case KWay:

			break;

		case Recursive:

			break;

		case NoMig:
			break;

		case Identity:

			break;

		default:
			std::cout<<"MetisPartioner: Method not implemented yet."<<std::endl;
	}

}

/**
*	Getter & Setter Functions
*/
void setUbvec(float nVal)
{
	ubvec_ = nVal;
}

void setEdgecut(int nVal)
{
        edgecut_ = nVal;
}

}; // END OF CLASS

}  // END OF NAMESPACE

#endif
